#pragma once

// SEG-125 (ADR 0273): the optional build profiler of `segarecomp build --performance-report <file>`. One plain class, no registry, no event bus: the build
// pipeline brackets its existing stages and operations with scoped spans and hands the compiler-job results of each batch to record_batch(); at the end
// the class renders one versioned JSON document (kind = "build"). Without --performance-report no instance is enabled and every entry point is a single
// branch. All timing is HOST wall/CPU time of the build, never guest time. Spans are only opened/closed by the build's main thread (the compile worker
// pool only produces per-job durations that the main thread reads after joining), so no locking exists.
//
// Accounting: every span has an inclusive time and an exclusive time (inclusive minus its direct child spans). Exclusive times of all spans plus the
// unattributed remainder add up to the total wall time, so nested stages are never double counted. The compile workers run concurrently: the wall time of
// a batch (span "compile_batch") and the aggregate of its jobs' own wall times are reported separately, never added.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "process_run.hpp"

namespace segarecomp_cli {

class BuildProfile {
 public:
  using NowFn = std::function<std::uint64_t()>;   // monotonic nanoseconds

  struct Config {
    std::string platform;          // "genesis" | "master_system"
    std::string optimize, runtime_optimize, aot_policy, compiler_name, compiler_version;
    unsigned jobs = 0;
    bool viewer = false;
  };

  struct JobResult {                // one compiler job (or one reused unit)
    std::string object;             // object path: local identity used to match the final link set; never exported
    std::string label;              // source basename (private reports only)
  std::string category;           // optional; derived from the object name when empty
    bool compiled = true;           // false: reused from an earlier compile of the same content
    double wall_ms = 0;
    ProcessUsage usage;
    std::uint64_t source_bytes = 0, object_bytes = 0;
  };

  explicit BuildProfile(NowFn now = default_now) : now_(std::move(now)) {}
  void enable(const std::filesystem::path &report) { report_ = report; enabled_ = true; start_ = now_(); }
  bool enabled() const { return enabled_; }
  const std::filesystem::path &report_path() const { return report_; }
  void set_config(Config config) { config_ = std::move(config); }
  void set_platform(const std::string &platform) { config_.platform = platform; }

  // ---- spans -------------------------------------------------------------------------------------------------------------------------------
  void begin(const std::string &name, bool stage) {
    if (!enabled_) return;
    stack_.push_back({name, stage, now_(), 0});
  }
  // Closes the innermost span called `name` (and any unclosed children, which only happens on a failure path).
  void end(const std::string &name) {
    if (!enabled_) return;
    if (std::none_of(stack_.begin(), stack_.end(), [&](const Open &o) { return o.name == name; })) return;
    for (;;) {
      const bool match = stack_.back().name == name;
      close_top();
      if (match) return;
    }
  }
  std::string innermost_stage() const {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it)
      if (it->stage) return it->name;
    return {};
  }

  // ---- counters and compiler jobs ----------------------------------------------------------------------------------------------------------
  void count(const std::string &name, std::uint64_t n = 1) { if (enabled_) counts_[name] += n; }
  void set_size(const std::string &name, std::uint64_t bytes) { if (enabled_) sizes_[name] = bytes; }
  // A new emission round of the generated M68K units (0 = the generate stage's emission, then one per alias-preparation round).
  void next_round() { if (enabled_) ++round_; }
  int round() const { return round_; }
  void record_job(JobResult job) {
    if (!enabled_) return;
    Unit u;
    u.job = std::move(job);
    u.round = round_;
    u.category = u.job.category.empty() ? category_of(u.job.object) : u.job.category;
    if (u.job.compiled) compiled_wall_[u.job.object] = u.job.wall_ms;
    units_.push_back(std::move(u));
  }
  // One finished batch: its own wall time is the "compile_batch" span (opened by the caller); the batch contributes its job results here.
  void record_batch_wall(double wall_ms, unsigned workers, std::size_t jobs) {
    if (!enabled_) return;
    ++batches_;
    batch_wall_ms_ += wall_ms;
    if (jobs > 0) max_workers_ = std::max<unsigned>(max_workers_, std::min<unsigned>(workers, static_cast<unsigned>(jobs)));
  }
  void record_link(const ProcessUsage &usage, double wall_ms) {
    if (!enabled_) return;
    ++links_;
    link_cpu_available_ = link_cpu_available_ || usage.cpu_available;
    link_cpu_ms_ += static_cast<double>(usage.cpu_ms);
    link_wall_ms_ += wall_ms;
  }
  // After the final link: which objects the executable contains, and which objects only the build-time observation program used.
  void mark_final(const std::vector<std::string> &final_objects, const std::vector<std::string> &pass_only_objects) {
    if (!enabled_) return;
    final_.insert(final_objects.begin(), final_objects.end());
    pass_only_.insert(pass_only_objects.begin(), pass_only_objects.end());
    final_marked_ = true;
  }

  // ---- report ------------------------------------------------------------------------------------------------------------------------------
  // status: "complete" | "failed" | "incomplete". Always callable; closes spans still open (a failure), then renders and writes the document.
  bool finish(const std::string &status, const std::string &failure_stage, bool shareable = false) {
    if (!enabled_) return true;
    while (!stack_.empty()) close_top();
    end_ = now_();
    std::ofstream out(report_, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << render(status, failure_stage, shareable);
    out.flush();
    return static_cast<bool>(out);
  }

  std::string render(const std::string &status, const std::string &failure_stage, bool shareable) const;

  static std::uint64_t default_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }

 private:
  struct Open { std::string name; bool stage; std::uint64_t start; std::uint64_t child_ns; };
  struct Agg { bool stage = false; std::uint64_t count = 0, inclusive_ns = 0, exclusive_ns = 0; };
  struct Unit { JobResult job; int round = 0; std::string category; };

  void close_top() {
    const Open top = stack_.back();
    stack_.pop_back();
    const std::uint64_t t = now_();
    const std::uint64_t incl = t > top.start ? t - top.start : 0;
    const std::uint64_t excl = incl > top.child_ns ? incl - top.child_ns : 0;
    auto it = std::find_if(agg_.begin(), agg_.end(), [&](const auto &e) { return e.first == top.name && e.second.stage == top.stage; });
    if (it == agg_.end()) { agg_.emplace_back(top.name, Agg{top.stage, 0, 0, 0}); it = agg_.end() - 1; }
    it->second.count += 1;
    it->second.inclusive_ns += incl;
    it->second.exclusive_ns += excl;
    if (!stack_.empty()) stack_.back().child_ns += incl; else top_level_ns_ += incl;
  }

  // The category of a unit from its object file name (the build names every object after what it is).
  static std::string category_of(const std::string &object) {
    const std::string stem = std::filesystem::path(object).filename().string();
    const auto starts = [&](const char *p) { return stem.rfind(p, 0) == 0; };
    if (starts("m68k-")) return "m68k_generated";
    if (starts("z80-")) return "z80_generated";
    if (starts("psg") || starts("ym")) return "sound_device";
    if (starts("viewer") || starts("sv-") || starts("vdp-render") || starts("native")) return "viewer";
    if (starts("rt-") || starts("hook") || starts("frame-")) return "runtime";
    return "other";
  }
  static std::string esc(const std::string &text) {
    std::string out;
    for (const char ch : text) {
      const auto c = static_cast<unsigned char>(ch);
      if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(ch); }
      else if (c < 0x20U) out.push_back(' ');
      else out.push_back(ch);
    }
    return out;
  }
  static double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1.0e6; }

  NowFn now_;
  bool enabled_ = false, final_marked_ = false;
  std::filesystem::path report_;
  Config config_;
  std::uint64_t start_ = 0, end_ = 0, top_level_ns_ = 0;
  std::vector<Open> stack_;
  std::vector<std::pair<std::string, Agg>> agg_;
  std::map<std::string, std::uint64_t> counts_, sizes_;
  std::vector<Unit> units_;
  std::map<std::string, double> compiled_wall_;
  std::set<std::string> final_, pass_only_;
  int round_ = 0;
  unsigned batches_ = 0, max_workers_ = 0, links_ = 0;
  double batch_wall_ms_ = 0, link_cpu_ms_ = 0, link_wall_ms_ = 0;
  bool link_cpu_available_ = false;
};

inline std::string BuildProfile::render(const std::string &status, const std::string &failure_stage, const bool shareable) const {
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(3);
  const std::uint64_t end = end_ != 0 ? end_ : now_();   // finish() fixed it; a direct render (tests) measures up to now
  const double wall = ms(end > start_ ? end - start_ : 0);
  o << "{\"schema\":\"segarecomp.performance\",\"schema_version\":1,\"kind\":\"build\",\"status\":\"" << status << "\",\"privacy\":\""
    << (shareable ? "shareable" : "private") << "\",\"platform\":\"" << esc(config_.platform) << "\"";
  if (status != "complete") o << ",\"failure\":{\"stage\":\"" << esc(failure_stage) << "\"}";
  o << ",\"config\":{\"optimize\":\"" << esc(config_.optimize) << "\",\"runtime_optimize\":\"" << esc(config_.runtime_optimize) << "\",\"aot_policy\":\""
    << esc(config_.aot_policy) << "\",\"jobs\":" << config_.jobs << ",\"hardware_concurrency\":" << std::thread::hardware_concurrency()
    << ",\"viewer\":" << (config_.viewer ? "true" : "false") << ",\"compiler\":{\"name\":\"" << esc(config_.compiler_name) << "\",\"version\":\""
    << esc(config_.compiler_version) << "\"},\"host_os\":\""
#if defined(_WIN32)
    << "windows"
#elif defined(__APPLE__)
    << "macos"
#elif defined(__linux__)
    << "linux"
#else
    << "other"
#endif
    << "\"}";
  // ---- stages and operations
  const auto emit_aggs = [&](bool stage) {
    bool first = true;
    for (const auto &e : agg_) {
      if (e.second.stage != stage) continue;
      o << (first ? "" : ",") << "{\"name\":\"" << esc(e.first) << "\",\"count\":" << e.second.count << ",\"inclusive_ms\":" << ms(e.second.inclusive_ns)
        << ",\"exclusive_ms\":" << ms(e.second.exclusive_ns) << "}";
      first = false;
    }
  };
  std::uint64_t excl_sum = 0;
  for (const auto &e : agg_) excl_sum += e.second.exclusive_ns;
  const std::uint64_t total_ns = end > start_ ? end - start_ : 0;
  o << ",\"build\":{\"wall_ms\":" << wall << ",\"stages\":[";
  emit_aggs(true);
  o << "],\"operations\":[";
  emit_aggs(false);
  o << "],\"accounting\":{\"exclusive_sum_ms\":" << ms(excl_sum) << ",\"unattributed_ms\":" << ms(total_ns > excl_sum ? total_ns - excl_sum : 0) << "}";
  o << ",\"counts\":{";
  {
    bool first = true;
    for (const auto &c : counts_) { o << (first ? "" : ",") << "\"" << esc(c.first) << "\":" << c.second; first = false; }
  }
  o << "},\"sizes\":{";
  {
    bool first = true;
    for (const auto &c : sizes_) { o << (first ? "" : ",") << "\"" << esc(c.first) << "_bytes\":" << c.second; first = false; }
  }
  o << "}";
  // ---- compilation
  struct Cat { std::uint64_t units = 0, compiled = 0, reused = 0, src = 0, obj = 0; double job_ms = 0, wasted_ms = 0, saved_ms = 0; };
  std::map<std::string, Cat> cats;
  double aggregate_ms = 0, wasted_ms = 0, saved_ms = 0, preparation_ms = 0, initial_ms = 0;
  std::uint64_t compiled = 0, reused = 0, superseded = 0, pass_only = 0, finals = 0, prep_compiled = 0, init_compiled = 0;
  bool cpu_all = true, rss_all = true;
  double cpu_ms = 0;
  std::uint64_t peak_rss_kib = 0;
  std::vector<double> durations;
  struct Row { const Unit *unit; std::string disposition; };
  std::vector<Row> rows;
  for (const Unit &u : units_) {
    Cat &cat = cats[u.category];
    ++cat.units;
    if (!u.job.compiled) {
      ++reused;
      ++cat.reused;
      const auto it = compiled_wall_.find(u.job.object);
      if (it != compiled_wall_.end()) { saved_ms += it->second; cat.saved_ms += it->second; }
      continue;
    }
    ++compiled;
    ++cat.compiled;
    cat.src += u.job.source_bytes;
    cat.obj += u.job.object_bytes;
    cat.job_ms += u.job.wall_ms;
    aggregate_ms += u.job.wall_ms;
    durations.push_back(u.job.wall_ms);
    std::string disposition;
    if (!final_marked_) disposition = "unknown";
    else if (final_.count(u.job.object)) { disposition = "final"; ++finals; }
    else if (pass_only_.count(u.job.object)) { disposition = "pass_only"; ++pass_only; }
    else { disposition = "superseded"; ++superseded; wasted_ms += u.job.wall_ms; cat.wasted_ms += u.job.wall_ms; }
    if (u.round > 0) { preparation_ms += u.job.wall_ms; ++prep_compiled; } else { initial_ms += u.job.wall_ms; ++init_compiled; }
    if (u.job.usage.cpu_available) cpu_ms += static_cast<double>(u.job.usage.cpu_ms); else cpu_all = false;
    if (u.job.usage.rss_available) peak_rss_kib = std::max(peak_rss_kib, u.job.usage.peak_rss_kib); else rss_all = false;
    rows.push_back({&u, disposition});
  }
  std::sort(durations.begin(), durations.end());
  const auto pct = [&](double q) { return durations.empty() ? 0.0 : durations[std::min(durations.size() - 1, static_cast<std::size_t>(q * static_cast<double>(durations.size())))]; };
  o << ",\"compile\":{\"batches\":" << batches_ << ",\"max_workers\":" << max_workers_ << ",\"units_total\":" << units_.size() << ",\"compiled\":" << compiled << ",\"reused\":" << reused
    << ",\"final\":" << finals << ",\"superseded\":" << superseded << ",\"pass_only\":" << pass_only << ",\"final_marked\":" << (final_marked_ ? "true" : "false")
    << ",\"batch_wall_ms\":" << batch_wall_ms_ << ",\"aggregate_job_ms\":" << aggregate_ms << ",\"wasted_ms\":" << wasted_ms << ",\"reuse_saved_ms\":" << saved_ms
    << ",\"phases\":{\"initial\":{\"compiled\":" << init_compiled << ",\"job_ms\":" << initial_ms << "},\"preparation\":{\"compiled\":" << prep_compiled << ",\"job_ms\":" << preparation_ms << "}}"
    << ",\"duration_ms\":{\"count\":" << durations.size() << ",\"min\":" << (durations.empty() ? 0.0 : durations.front()) << ",\"p50\":" << pct(0.50) << ",\"p95\":" << pct(0.95)
    << ",\"max\":" << (durations.empty() ? 0.0 : durations.back()) << "}";
  o << ",\"categories\":[";
  {
    bool first = true;
    for (const auto &c : cats) {
      o << (first ? "" : ",") << "{\"category\":\"" << c.first << "\",\"units\":" << c.second.units << ",\"compiled\":" << c.second.compiled << ",\"reused\":" << c.second.reused
        << ",\"job_ms\":" << c.second.job_ms << ",\"wasted_ms\":" << c.second.wasted_ms << ",\"reuse_saved_ms\":" << c.second.saved_ms << ",\"source_bytes\":" << c.second.src
        << ",\"object_bytes\":" << c.second.obj << "}";
      first = false;
    }
  }
  o << "],\"resources\":{\"compiler_cpu_ms\":";
  if (cpu_all && compiled > 0) o << cpu_ms; else o << "null";
  o << ",\"compiler_peak_rss_kib\":";
  if (rss_all && compiled > 0) o << peak_rss_kib; else o << "null";
  o << ",\"link_cpu_ms\":";
  if (link_cpu_available_) o << link_cpu_ms_; else o << "null";
  o << ",\"link_wall_ms\":" << link_wall_ms_ << "}";
  if (!shareable) {   // unit labels are local identifiers: private reports only
    const auto emit_top = [&](const char *key, bool by_size) {
      std::vector<Row> sorted = rows;
      std::stable_sort(sorted.begin(), sorted.end(), [&](const Row &a, const Row &b) {
        return by_size ? a.unit->job.source_bytes > b.unit->job.source_bytes : a.unit->job.wall_ms > b.unit->job.wall_ms;
      });
      o << ",\"" << key << "\":[";
      for (std::size_t i = 0; i < sorted.size() && i < 10; ++i) {
        const Unit &u = *sorted[i].unit;
        o << (i ? "," : "") << "{\"label\":\"" << esc(u.job.label) << "\",\"category\":\"" << u.category << "\",\"wall_ms\":" << u.job.wall_ms << ",\"source_bytes\":" << u.job.source_bytes
          << ",\"object_bytes\":" << u.job.object_bytes << ",\"round\":" << u.round << ",\"disposition\":\"" << sorted[i].disposition << "\"}";
      }
      o << "]";
    };
    emit_top("slowest_units", false);
    emit_top("largest_units", true);
  }
  o << "}}";
  o << ",\"unavailable\":[";
  {
    bool first = true;
    const auto miss = [&](const char *metric, const char *reason) { o << (first ? "" : ",") << "{\"metric\":\"" << metric << "\",\"reason\":\"" << reason << "\"}"; first = false; };
    if (!cpu_all || compiled == 0) miss("compiler_cpu_ms", "the platform did not report child CPU time for every compiler job");
    if (!rss_all || compiled == 0) miss("compiler_peak_rss_kib", "the platform did not report the peak resident set of every compiler job");
    if (!link_cpu_available_) miss("link_cpu_ms", "the platform did not report child CPU time for the linker");
    if (!final_marked_) miss("unit_disposition", "the build ended before the final link set was known");
  }
  o << "]}\n";
  return o.str();
}

// RAII span: `BuildProfile::begin`/`end` with the usual scope rules. A null or disabled profile makes it a no-op.
class ProfileSpan {
 public:
  ProfileSpan(BuildProfile *profile, std::string name, bool stage = false) : profile_(profile != nullptr && profile->enabled() ? profile : nullptr), name_(std::move(name)) {
    if (profile_ != nullptr) profile_->begin(name_, stage);
  }
  ~ProfileSpan() { if (profile_ != nullptr) profile_->end(name_); }
  ProfileSpan(const ProfileSpan &) = delete;
  ProfileSpan &operator=(const ProfileSpan &) = delete;

 private:
  BuildProfile *profile_;
  std::string name_;
};

}  // namespace segarecomp_cli
