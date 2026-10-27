// SEG-078-T003 (ADR 0205): `segarecomp sega-cd build` - the supported, self-service Sega CD build. The multi-pass fixed point of
// tools/sega_cd_bios_build.py in the product CLI: emit the generated program from the BIOS, the disc and the admissions so far, compile it (parallel,
// content-addressed object cache), run it once from reset as a build pass, and read what stopped it:
//   - the sub's runnable-epoch image            -> admitted (ordered, with the identity of the pass that produced it);
//   - a RAM `JMP (xxx).W/.L` frontier           -> an operand template (or a run of contiguous slots);
//   - any other RAM frontier                    -> `sega-cd resolve-source`: the disc span the live window is an exact copy of among the blocks the
//                                                  machine itself read; admitted as a disc extent, or the typed refusal ends the build there;
//   - anything else (idle, budget, other stop)  -> the candidate: a confirming pass must reproduce it exactly.
// No reference emulator, trace, recipe or per-title table is consulted. Every bound is a constant; exhaustion is a typed failure. Progress goes to
// stdout as `@stage <name> <state>` / `@round <n> <what>` lines and the final `@result`; the outcome is also written to <output>/status.json.
#include "sega_cd_command.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "process_run.hpp"
#include "segarecomp/sha256.hpp"

namespace fs = std::filesystem;
using segarecomp_cli::kProcessTimedOut;
using segarecomp_cli::run_process;

namespace {

constexpr unsigned kMaxEpochs = 4, kMaxTemplates = 64, kMaxTemplateSlots = 256, kMaxRunSlots = 128, kMaxExtents = 16;
constexpr unsigned kMaxRounds = 1 + kMaxEpochs + kMaxTemplates + kMaxExtents;
constexpr const char *kCacheVersion = "segarecomp.sega_cd_build.objects.v1";
constexpr const char *kViewerFoundation[] = {"sv_settings.c", "sv_layout.c", "sv_menu.c", "sv_input_state.c", "sv_input_diag.c", "sv_font.c", "sv_font_data.c", "sv_ui.c", "sv_host.c", "sv_pages.c", "sv_draw.c"};

struct Options {
  fs::path cue, bios, output, runtime_dir, cache_dir, input_script, backup_ram;
  std::string cc;
  std::vector<std::string> cc_args, link_args;
  fs::path sdl3_include, sdl3_lib, viewer_host_source;   // a project-authored test host replaces the SDL3 viewer sources (tests only)
  std::vector<std::string> pass_sets;                    // register presets of the build pass (project-authored fixtures select a case this way)
  bool viewer = false;
  unsigned jobs = 0, pass_timeout = 900, compile_timeout = 1800;
  std::uint64_t budget = 300000000ULL, until_tick = 0;
  std::string optimize = "1";
};

struct Failure {
  std::string stage, message;
};

std::string json_escape(const std::string &text) {
  std::string out;
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(ch); }
    else if (c < 0x20U) out.push_back(' ');
    else out.push_back(ch);
  }
  return out;
}

std::string read_text(const fs::path &path, std::uintmax_t limit = 64U << 20U) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > limit) return {};
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string sha_of(const std::string &text) {
  return segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

// Scans for `"key":` in text produced by this runtime (a fixed, flat schema; never user data) and returns the raw value text.
std::optional<std::string> raw_value(const std::string &text, const std::string &key, std::size_t from = 0) {
  const auto at = text.find("\"" + key + "\":", from);
  if (at == std::string::npos) return std::nullopt;
  std::size_t p = at + key.size() + 3U;
  while (p < text.size() && text[p] == ' ') ++p;
  if (p >= text.size()) return std::nullopt;
  if (text[p] == '"') {
    const auto end = text.find('"', p + 1U);
    return end == std::string::npos ? std::nullopt : std::optional<std::string>(text.substr(p + 1U, end - p - 1U));
  }
  std::size_t e = p;
  while (e < text.size() && text[e] != ',' && text[e] != '}' && text[e] != ']') ++e;
  return text.substr(p, e - p);
}

std::optional<std::uint64_t> number_value(const std::string &text, const std::string &key, std::size_t from = 0) {
  const auto raw = raw_value(text, key, from);
  if (!raw || raw->empty() || raw->size() > 18U || raw->find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
  return std::stoull(*raw);
}

void stage(const std::string &name, const char *state) { std::cout << "@stage " << name << ' ' << state << std::endl; }

// A per-process token so two builds sharing one object cache never write the same partial file.
std::uint64_t process_token() {
  static const std::uint64_t token = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                                     (reinterpret_cast<std::uintptr_t>(&token) << 7U) ^ static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return token;
}

class Builder {
 public:
  Builder(Options options, std::string self) : o_(std::move(options)), self_(std::move(self)) {}

  int run() {
    std::error_code ec;
    fs::create_directories(o_.output, ec);
    if (!fs::is_directory(o_.output)) return fail({"setup", "the output folder cannot be created"});
    log_.open(o_.output / "build.log", std::ios::trunc);
    if (o_.cache_dir.empty()) o_.cache_dir = o_.output / "objects";
    fs::create_directories(o_.cache_dir, ec);
    if (o_.jobs == 0) o_.jobs = std::max(1U, std::thread::hardware_concurrency());
    const std::string bios_text = read_text(o_.bios, 0x20001U);
    const std::string cue_text = read_text(o_.cue, 1U << 20U);
    if (bios_text.empty() || cue_text.empty()) return fail({"setup", "the BIOS or the CUE file cannot be read"});
    bios_sha_ = sha_of(bios_text);
    if (const auto f = key_base()) return fail(*f);

    stage("analyze", "running");
    std::vector<std::string> admits;
    unsigned epochs = 0, templates = 0, slot_total = 0;
    for (unsigned round = 0; round < kMaxRounds; ++round) {
      std::cout << "@round " << round << " start" << std::endl;
      const fs::path src = o_.output / "src";
      std::string program_identity;
      if (const auto f = emit(admits, src, program_identity)) return fail(*f);
      if (round == 0) stage("analyze", "done");
      stage("generate", "done");
      const fs::path exe = o_.output / ("program" + exe_suffix());
      std::vector<fs::path> units;
      if (const auto f = compile_link(src, exe, units)) return fail(*f);
      stage("pass", "running");
      std::string report, capture_text;
      if (const auto f = pass_run(exe, o_.output / "pass", report, capture_text)) return fail(*f);
      stage("pass", "done");
      std::map<std::string, std::string> entry{{"round", std::to_string(round)}};
      history_ += (history_.empty() ? "" : ",") + std::string("{\"round\":") + std::to_string(round) + ",\"stop\":\"" + stop_class(report) + "\"";
      const bool have_capture = !capture_text.empty();
      const std::string kind = have_capture ? raw_value(capture_text, "kind").value_or("") : "";
      if (have_capture && raw_value(capture_text, "pass_identity").value_or("") != program_identity) return fail({"pass", "the build pass reports another program identity"});
      if (kind == "epoch") {
        if (++epochs > kMaxEpochs) return fail({"pass", "epoch_bound_exceeded"});
        const fs::path snapshot = o_.output / ("epoch" + std::to_string(epochs - 1U) + ".bin");
        fs::copy_file(o_.output / "pass" / "epoch.bin", snapshot, fs::copy_options::overwrite_existing, ec);
        admits.push_back("epoch:" + program_identity + ":" + snapshot.string());
        history_ += ",\"admitted\":\"epoch\"}";
        continue;
      }
      if (kind == "frontier") {
        const std::string cpu = raw_value(capture_text, "cpu").value_or("0") == "0" ? "main" : "sub";
        const std::uint64_t pc = number_value(capture_text, "pc").value_or(0);
        const std::string bytes = raw_value(capture_text, "bytes").value_or("");
        const bool template_shape = bytes.rfind("4ef8", 0) == 0 || bytes.rfind("4ef9", 0) == 0;
        if (template_shape && !proposed(admits, cpu, pc)) {
          if (++templates > kMaxTemplates) return fail({"pass", "template_bound_exceeded"});
          if (const auto f = admit_template(admits, capture_text, program_identity, cpu, pc, bytes, slot_total)) return fail(*f);
          history_ += ",\"admitted\":\"template\"}";
          continue;
        }
        const auto spec = resolve(capture_text, cpu, pc, extents_.size());
        if (spec.extent) {
          extents_.push_back(*spec.extent);
          admits.push_back("extent:" + program_identity + ":" + *spec.extent);
          history_ += ",\"admitted\":\"extent\"}";
          continue;
        }
        frontier_ = "{\"cpu\":\"" + cpu + "\",\"pc\":" + std::to_string(pc) + ",\"refusal\":\"" + spec.refusal + "\"}";
        history_ += ",\"frontier\":" + frontier_;
      }
      history_ += "}";
      // the candidate: a confirming pass must reproduce its outcome exactly
      stage("confirm", "running");
      std::string confirm_report, confirm_capture;
      if (const auto f = pass_run(exe, o_.output / "confirm", confirm_report, confirm_capture)) return fail(*f);
      if (confirm_report != report || confirm_capture.empty() != capture_text.empty()) return fail({"confirm", "materialization_nondeterministic"});
      stage("confirm", "done");
      return finish(exe, program_identity, report, round + 1U);
    }
    return fail({"build", "round_bound_exceeded"});
  }

 private:
  static std::string exe_suffix() {
#if defined(_WIN32)
    return ".exe";
#else
    return "";
#endif
  }

  int fail(const Failure &f) {
    log_ << "FAILED (" << f.stage << "): " << f.message << std::endl;
    std::ofstream(o_.output / "status.json", std::ios::binary | std::ios::trunc) << "{\"status\":\"failed\",\"stage\":\"" << f.stage << "\",\"message\":\"" << json_escape(f.message) << "\"}\n";
    std::cout << "@result failed stage=" << f.stage << " message=" << f.message << std::endl;
    return 3;
  }

  int finish(const fs::path &exe, const std::string &program_identity, const std::string &report, unsigned rounds) {
    std::string viewer_path;
    if (o_.viewer) {
      stage("viewer", "running");
      const fs::path viewer = o_.output / ("program-viewer" + exe_suffix());
      if (const auto f = link_viewer(viewer)) return fail(*f);
      viewer_path = viewer.filename().string();
      stage("viewer", "done");
    }
    std::ostringstream out;
    out << "{\"status\":\"ok\",\"program\":\"" << exe.filename().string() << "\",\"viewer_program\":" << (viewer_path.empty() ? "null" : "\"" + viewer_path + "\"")
        << ",\"program_identity\":\"" << program_identity << "\",\"bios_sha256\":\"" << bios_sha_ << "\",\"rounds\":" << rounds << ",\"extents\":[";
    for (std::size_t i = 0; i < extents_.size(); ++i) out << (i ? "," : "") << "\"" << extents_[i] << "\"";
    out << "],\"frontier\":" << (frontier_.empty() ? "null" : frontier_) << ",\"result\":\"" << raw_value(report, "result").value_or("") << "\",\"stop\":\"" << stop_class(report)
        << "\",\"viewer\":" << (o_.viewer ? "true" : "false") << ",\"history\":[" << history_ << "]}\n";
    std::ofstream(o_.output / "status.json", std::ios::binary | std::ios::trunc) << out.str();
    std::cout << "@result ok" << std::endl;
    return 0;
  }

  static std::string stop_class(const std::string &report) {
    const auto at = report.find("\"stop\":{");
    return at == std::string::npos ? std::string("none") : raw_value(report, "class", at).value_or("none");
  }

  static bool proposed(const std::vector<std::string> &admits, const std::string &cpu, std::uint64_t pc) {
    for (const auto &a : admits) {
      std::vector<std::string> f;
      std::stringstream s(a);
      for (std::string part; std::getline(s, part, ':');) f.push_back(part);
      if (f.size() >= 4U && f[0] == "template" && f[2] == cpu && std::stoull(f[3], nullptr, 0) == pc) return true;
      if (f.size() >= 5U && f[0] == "template-run" && f[2] == cpu) {
        const std::uint64_t base = std::stoull(f[3], nullptr, 0), slots = std::stoull(f[4]);
        if (pc >= base && pc < base + 6U * slots && (pc - base) % 6U == 0U) return true;
      }
    }
    return false;
  }

  // ---- the key of every cached object: compiler identity, flags, the bytes of every runtime source and header ----
  std::optional<Failure> key_base() {
    const fs::path version_log = o_.output / "cc-version.txt";
    if (run_process({o_.cc, "--version"}, version_log, {}, 60) != 0) {
      std::vector<std::string> v{o_.cc};
      for (const auto &a : o_.cc_args) v.push_back(a);
      v.push_back("--version");
      if (run_process(v, version_log, {}, 60) != 0) return Failure{"setup", "the C compiler cannot be run"};
    }
    std::string material = std::string(kCacheVersion) + "\n" + o_.cc + "\n" + read_text(version_log) + "\n";
    for (const auto &a : o_.cc_args) material += "arg " + a + "\n";
    material += "opt " + o_.optimize + "\n";
    std::vector<fs::path> files;
    for (const char *rel : {"platforms/sega-cd/runtime", "platforms/sega-cd/viewer", "platforms/genesis/runtime", "platforms/genesis/viewer", "platforms/genesis/machine/include", "libs/cpu/m68k/include",
                            "libs/device/sega/genesis/include", "libs/codegen/c11/include", "libs/viewer/include", "libs/viewer/src"}) {
      std::error_code ec;
      for (fs::recursive_directory_iterator it(o_.runtime_dir / rel, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && (it->path().extension() == ".c" || it->path().extension() == ".h")) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) return Failure{"setup", "the runtime sources were not found under the runtime folder"};
    for (const auto &f : files) material += fs::relative(f, o_.runtime_dir).generic_string() + " " + sha_of(read_text(f)) + "\n";
    key_base_ = sha_of(material);
    return std::nullopt;
  }

  std::vector<std::string> compile_command(const fs::path &source, const fs::path &object, const std::vector<std::string> &extra, bool viewer) const {
    std::vector<std::string> c{o_.cc};
    c.insert(c.end(), o_.cc_args.begin(), o_.cc_args.end());
    for (const char *flag : {"-std=c11", "-w"}) c.push_back(flag);
    c.push_back("-O" + o_.optimize);
    const fs::path rt = o_.runtime_dir;
    for (const fs::path &inc : {rt / "platforms/sega-cd/runtime", rt / "platforms/genesis/runtime"}) c.push_back("-I" + inc.string());
    if (viewer) {
      for (const fs::path &inc : {rt / "platforms/sega-cd/viewer", rt / "platforms/genesis/viewer", rt / "libs/viewer/include", rt / "libs/viewer/src"}) c.push_back("-I" + inc.string());
      if (!o_.sdl3_include.empty()) c.push_back("-I" + o_.sdl3_include.string());
    }
    c.insert(c.end(), extra.begin(), extra.end());
    c.insert(c.end(), {"-c", source.string(), "-o", object.string()});
    return c;
  }

  // A cached object: the key is the base, the unit name, its bytes and the extra flags. Returns the object path.
  std::optional<Failure> object_for(const fs::path &source, const std::vector<std::string> &extra, bool viewer, fs::path &object, bool &rebuilt) {
    std::string extra_text;
    for (const auto &e : extra) extra_text += e + "\n";
    const std::string key = sha_of(key_base_ + "\n" + (viewer ? "viewer\n" : "") + source.filename().string() + "\n" + extra_text + sha_of(read_text(source, 1ULL << 31U)));
    object = o_.cache_dir / (key + ".o");
    rebuilt = false;
    std::error_code ec;
    if (fs::exists(object, ec)) return std::nullopt;
    const fs::path partial = o_.cache_dir / (key + ".part" + std::to_string(process_token()) + "-" + std::to_string(counter_.fetch_add(1)));
        const int code = run_process(compile_command(source, partial, extra, viewer), partial.string() + ".log", {}, o_.compile_timeout);
    if (code != 0) {
      std::lock_guard<std::mutex> lock(log_mutex_);
      log_ << "compile failed: " << source.filename().string() << "\n" << read_text(partial.string() + ".log", 1U << 16U) << std::endl;
      fs::remove(partial.string() + ".log", ec);
      return Failure{"compile", "compiling " + source.filename().string() + (code == kProcessTimedOut ? " timed out" : " failed")};
    }
    fs::remove(partial.string() + ".log", ec);
    fs::rename(partial, object, ec);
    if (ec) return Failure{"compile", "cannot store an object"};
    rebuilt = true;
    return std::nullopt;
  }

  std::optional<Failure> objects_for(const std::vector<fs::path> &sources, const std::vector<std::string> &extra, bool viewer, std::vector<fs::path> &objects, unsigned &rebuilt) {
    objects.assign(sources.size(), fs::path{});
    std::atomic<std::size_t> next{0};
    std::atomic<unsigned> count{0};
    std::optional<Failure> failure;
    std::mutex failure_mutex;
    const auto worker = [&] {
      for (;;) {
        const std::size_t i = next.fetch_add(1);
        if (i >= sources.size()) return;
        bool built = false;
        if (const auto f = object_for(sources[i], extra, viewer, objects[i], built)) {
          std::lock_guard<std::mutex> lock(failure_mutex);
          if (!failure) failure = f;
          next.store(sources.size());
          return;
        }
        if (built) count.fetch_add(1);
      }
    };
    std::vector<std::thread> pool;
    for (unsigned n = 1; n < std::min<unsigned>(o_.jobs, static_cast<unsigned>(sources.size())); ++n) pool.emplace_back(worker);
    worker();
    for (auto &t : pool) t.join();
    rebuilt = count.load();
    return failure;
  }

  std::vector<fs::path> runtime_sources() const {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(o_.runtime_dir / "platforms/sega-cd/runtime", ec), end; !ec && it != end; it.increment(ec))
      if (it->path().extension() == ".c") out.push_back(it->path());
    std::sort(out.begin(), out.end());
    for (const char *n : {"runtime.c", "vdp_render.c", "frame_export.c"}) out.push_back(o_.runtime_dir / "platforms/genesis/runtime" / n);
    return out;
  }

  std::optional<Failure> runtime_objects(std::vector<fs::path> &objects) {
    if (!runtime_objects_.empty()) { objects = runtime_objects_; return std::nullopt; }
    unsigned rebuilt = 0;
    if (const auto f = objects_for(runtime_sources(), {}, false, objects, rebuilt)) return f;
    runtime_objects_ = objects;
    return std::nullopt;
  }

  std::vector<std::string> link_command(const std::vector<fs::path> &objects, const fs::path &exe, bool with_sdl) const {
    std::vector<std::string> c{o_.cc};
    c.insert(c.end(), o_.cc_args.begin(), o_.cc_args.end());
    for (const auto &obj : objects) c.push_back(obj.string());
    c.insert(c.end(), {"-o", exe.string()});
    if (with_sdl && !o_.sdl3_lib.empty()) c.push_back("-L" + o_.sdl3_lib.string());
    if (with_sdl) c.push_back("-lSDL3");
#if !defined(_WIN32)
    c.push_back("-lm");
#endif
    c.insert(c.end(), o_.link_args.begin(), o_.link_args.end());
    return c;
  }

  std::optional<Failure> emit(const std::vector<std::string> &admits, const fs::path &src, std::string &identity) {
    std::error_code ec;
    fs::remove_all(src, ec);
    fs::create_directories(src, ec);
    std::vector<std::string> cmd{self_, "sega-cd", "emit", "--cue", o_.cue.string(), "--bios", o_.bios.string(), "--out", (src / "program.c").string(), "--partition-window", "8192"};
    for (const auto &a : admits) { cmd.push_back("--admit"); cmd.push_back(a); }
    const fs::path out = o_.output / "emit.json";
    if (run_process(cmd, out, {}, o_.compile_timeout) != 0) return Failure{"generate", "the program could not be generated: " + read_text(out, 4096)};
    const std::string text = read_text(out);
    if (raw_value(text, "ok").value_or("") != "true") return Failure{"generate", "the program could not be generated: " + text.substr(0, 300)};
    identity = raw_value(text, "program_identity").value_or("");
    unit_names_.clear();   // the emitter's order is the link order
    const auto units = text.find("\"units\":[");
    const auto units_end = units == std::string::npos ? units : text.find("],\"processors\"", units);
    for (auto at = units; at != std::string::npos && at < units_end;) {
      const auto name = raw_value(text, "name", at);
      if (!name) break;
      const auto next = text.find("\"name\":", at);
      if (next >= units_end) break;
      unit_names_.push_back(*name);
      at = next + 8U;
    }
    return std::nullopt;
  }

  std::optional<Failure> compile_link(const fs::path &src, const fs::path &exe, std::vector<fs::path> &) {
    stage("compile", "running");
    std::vector<fs::path> sources;
    for (const auto &name : unit_names_) sources.push_back(src / name);
    sources.push_back(src / "program.c");   // the program unit (main) is last: the link order is deterministic
    std::vector<fs::path> objects, rt;
    unsigned rebuilt = 0;
    if (const auto f = objects_for(sources, {}, false, objects, rebuilt)) return f;
    unit_sources_ = sources;
    unit_objects_ = objects;
    if (const auto f = runtime_objects(rt)) return f;
    stage("compile", "done");
    stage("link", "running");
    std::vector<fs::path> all = objects;
    all.insert(all.end(), rt.begin(), rt.end());
    if (run_process(link_command(all, exe, false), o_.output / "link.log", {}, 600) != 0) return Failure{"link", "linking failed: " + read_text(o_.output / "link.log", 2048)};
    stage("link", "done");
    return std::nullopt;
  }

  std::optional<Failure> link_viewer(const fs::path &viewer) {
    const bool host = !o_.viewer_host_source.empty();
    if (!host && (o_.sdl3_include.empty() || o_.sdl3_lib.empty())) return Failure{"viewer", "sdl3_unavailable"};
    const fs::path rt = o_.runtime_dir;
    // the program unit again, with its entry renamed, and every unit object reused
    fs::path program_object;
    bool built = false;
    if (const auto f = object_for(unit_sources_.back(), {"-Dscd_program_main_bios=scd_viewer_main_bios"}, true, program_object, built)) return f;
    std::vector<fs::path> viewer_sources{rt / "platforms/sega-cd/viewer/scd_viewer.c", rt / "platforms/genesis/viewer/viewer.c"};
    if (host) {
      viewer_sources = {o_.viewer_host_source};
    } else {
      viewer_sources.push_back(rt / "platforms/genesis/viewer/viewer_sdl3.c");
      for (const char *n : kViewerFoundation) viewer_sources.push_back(rt / "libs/viewer/src" / n);
    }
    std::vector<fs::path> viewer_objects, rt_objects;
    unsigned rebuilt = 0;
    if (const auto f = objects_for(viewer_sources, {}, true, viewer_objects, rebuilt)) return f;
    if (const auto f = runtime_objects(rt_objects)) return f;
    std::vector<fs::path> all(unit_objects_.begin(), unit_objects_.end() - 1);
    all.push_back(program_object);
    all.insert(all.end(), rt_objects.begin(), rt_objects.end());
    all.insert(all.end(), viewer_objects.begin(), viewer_objects.end());
    if (run_process(link_command(all, viewer, !host), o_.output / "link-viewer.log", {}, 600) != 0) return Failure{"viewer", "linking the viewer failed: " + read_text(o_.output / "link-viewer.log", 2048)};
    return std::nullopt;
  }

  std::optional<Failure> pass_run(const fs::path &exe, const fs::path &dir, std::string &report, std::string &capture) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::vector<std::string> cmd{exe.string(), "--disc-dir", o_.cue.parent_path().string(), "--bios", o_.bios.string(), "--instruction-budget", std::to_string(o_.budget),
                                 "--materialize-dir", dir.string(), "--report", (dir / "report.json").string()};
    if (o_.until_tick != 0) { cmd.push_back("--until-tick"); cmd.push_back(std::to_string(o_.until_tick)); }
    if (!o_.backup_ram.empty()) { cmd.push_back("--backup-ram"); cmd.push_back(o_.backup_ram.string()); }
    if (!o_.input_script.empty()) { cmd.push_back("--input-script"); cmd.push_back(o_.input_script.string()); }
    for (const auto &set : o_.pass_sets) { cmd.push_back("--set"); cmd.push_back(set); }
    const int code = run_process(cmd, dir / "stdout.log", {}, o_.pass_timeout);
    if (code == kProcessTimedOut) return Failure{"pass", "the build pass exceeded its wall-clock bound"};
    report = read_text(dir / "report.json");
    if (report.empty()) return Failure{"pass", "the build pass produced no report (exit " + std::to_string(code) + "): " + read_text(dir / "stdout.log", 400)};
    capture = read_text(dir / "capture.json");
    return std::nullopt;
  }

  // ---- operand templates (ADR 0142 decision 7, ADR 0180) ----
  static std::vector<std::pair<std::string, std::uint64_t>> proposed_slots(const std::vector<std::string> &admits) {
    std::vector<std::pair<std::string, std::uint64_t>> out;
    for (const auto &a : admits) {
      std::vector<std::string> f;
      std::stringstream s(a);
      for (std::string part; std::getline(s, part, ':');) f.push_back(part);
      if (f.size() >= 4U && f[0] == "template") out.emplace_back(f[2], std::stoull(f[3], nullptr, 0));
      if (f.size() >= 5U && f[0] == "template-run")
        for (std::uint64_t k = 0; k < std::stoull(f[4]); ++k) out.emplace_back(f[2], std::stoull(f[3], nullptr, 0) + 6U * k);
    }
    return out;
  }

  std::optional<Failure> admit_template(std::vector<std::string> &admits, const std::string &capture, const std::string &pass, const std::string &cpu, std::uint64_t pc,
                                        const std::string &bytes, unsigned &slot_total) {
    const auto taken = proposed_slots(admits);
    const auto is_taken = [&](std::uint64_t address) { return std::find(taken.begin(), taken.end(), std::make_pair(cpu, address)) != taken.end(); };
    const auto run_at = capture.find("\"run\":{");
    if (run_at != std::string::npos) {
      const std::uint64_t base = number_value(capture, "base", run_at).value_or(0), slots = number_value(capture, "slots", run_at).value_or(0);
      const std::string hex = raw_value(capture, "bytes", run_at).value_or("");
      if (slots >= 1 && hex.size() == 12U * slots && base <= pc && pc < base + 6U * slots && (pc - base) % 6U == 0U) {
        std::uint64_t lo = (pc - base) / 6U, hi = lo;
        while (lo > 0 && !is_taken(base + 6U * (lo - 1U))) --lo;
        while (hi + 1U < slots && !is_taken(base + 6U * (hi + 1U))) ++hi;
        const std::uint64_t count = hi - lo + 1U;
        if (count > 1U) {
          if (count > kMaxRunSlots) return Failure{"pass", "template_run_invalid"};
          slot_total += static_cast<unsigned>(count);
          if (slot_total > kMaxTemplateSlots) return Failure{"pass", "template_slot_bound_exceeded"};
          char first[32];
          std::snprintf(first, sizeof first, "0x%llx", static_cast<unsigned long long>(base + 6U * lo));
          admits.push_back("template-run:" + pass + ":" + cpu + ":" + first + ":" + std::to_string(count) + ":" + hex.substr(static_cast<std::size_t>(12U * lo), static_cast<std::size_t>(12U * count)));
          return std::nullopt;
        }
      }
    }
    slot_total += 1U;
    if (slot_total > kMaxTemplateSlots) return Failure{"pass", "template_slot_bound_exceeded"};
    char at[32];
    std::snprintf(at, sizeof at, "0x%llx", static_cast<unsigned long long>(pc));
    admits.push_back("template:" + pass + ":" + cpu + ":" + at + ":" + bytes);
    return std::nullopt;
  }

  // ---- disc extents (ADR 0205) ----
  struct Resolution {
    std::optional<std::string> extent;   // "<cpu>:<offset>:<base>:<length>"
    std::string refusal;
  };

  Resolution resolve(const std::string &capture, const std::string &cpu, std::uint64_t pc, std::size_t have) {
    const auto at = capture.find("\"region\":{");
    if (at == std::string::npos) return {std::nullopt, "no_evidence"};
    if (have >= kMaxExtents) return {std::nullopt, "extent_bound_exceeded"};
    const std::uint64_t base = number_value(capture, "base", at).value_or(0);
    const std::string overflow = raw_value(capture, "delivered_overflow", at).value_or("0");
    std::string delivered;
    const auto list = capture.find("\"delivered\":[", at);
    if (list != std::string::npos) {
      std::size_t p = list + 13U;
      while (p < capture.size() && capture[p] == '[') {
        const auto close = capture.find(']', p);
        if (close == std::string::npos) break;
        std::string pair = capture.substr(p + 1U, close - p - 1U);
        std::replace(pair.begin(), pair.end(), ',', ':');
        delivered += (delivered.empty() ? "" : ",") + pair;
        p = close + 1U;
        if (p < capture.size() && capture[p] == ',') ++p;
      }
    }
    std::vector<std::string> cmd{self_, "sega-cd", "resolve-source", "--cue", o_.cue.string(), "--cpu", cpu, "--pc", std::to_string(pc), "--region", (o_.output / "pass" / "region.bin").string(),
                                 "--region-base", std::to_string(base), "--delivered-overflow", overflow};
    if (!delivered.empty()) { cmd.push_back("--delivered"); cmd.push_back(delivered); }
    const fs::path out = o_.output / "resolve.json";
    std::error_code rm_ec;
    fs::remove(out, rm_ec);   // never read an earlier round's answer
    if (run_process(cmd, out, {}, 600) < 0) return {std::nullopt, "resolver_failed"};   // exit 8 is a normal typed refusal; < 0 is a start failure or a timeout
    const std::string text = read_text(out);
    if (raw_value(text, "ok").value_or("") == "true") {
      const std::string spec = cpu + ":" + raw_value(text, "offset").value_or("0") + ":" + raw_value(text, "base").value_or("0") + ":" + raw_value(text, "length").value_or("0");
      if (std::find(extents_.begin(), extents_.end(), spec) != extents_.end()) return {std::nullopt, "no_progress"};
      return {spec, ""};
    }
    return {std::nullopt, raw_value(text, "error").value_or("resolver_failed")};
  }

  Options o_;
  std::string self_, bios_sha_, key_base_, history_, frontier_;
  std::ofstream log_;
  std::mutex log_mutex_;
  std::atomic<unsigned> counter_{0};
  std::vector<std::string> extents_;
  std::vector<std::string> unit_names_;
  std::vector<fs::path> unit_sources_, unit_objects_, runtime_objects_;
};

}  // namespace

int segarecomp_sega_cd_build(int argc, char **argv) {
  Options o;
  bool have_cue = false, have_bios = false, have_output = false, have_runtime = false;
  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string_view key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--cue") { o.cue = value; have_cue = true; }
    else if (key == "--bios") { o.bios = value; have_bios = true; }
    else if (key == "--output") { o.output = value; have_output = true; }
    else if (key == "--runtime-dir") { o.runtime_dir = value; have_runtime = true; }
    else if (key == "--cache-dir") o.cache_dir = value;
    else if (key == "--cc") o.cc = value;
    else if (key == "--cc-arg") o.cc_args.push_back(value);
    else if (key == "--link-arg") o.link_args.push_back(value);
    else if (key == "--sdl3-include") o.sdl3_include = value;
    else if (key == "--sdl3-lib") o.sdl3_lib = value;
    else if (key == "--viewer-host-source") o.viewer_host_source = value;
    else if (key == "--pass-set") o.pass_sets.push_back(value);
    else if (key == "--viewer") o.viewer = value == "1";
    else if (key == "--jobs") o.jobs = static_cast<unsigned>(std::max(0, std::atoi(value.c_str())));
    else if (key == "--budget") o.budget = std::strtoull(value.c_str(), nullptr, 10);
    else if (key == "--until-tick") o.until_tick = std::strtoull(value.c_str(), nullptr, 10);
    else if (key == "--pass-timeout") o.pass_timeout = static_cast<unsigned>(std::max(1, std::atoi(value.c_str())));
    else if (key == "--input-script") o.input_script = value;
    else if (key == "--backup-ram") o.backup_ram = value;
    else if (key == "--optimize" && (value == "0" || value == "1" || value == "2")) o.optimize = value;
    else {
      std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
      return 2;
    }
  }
  if (!have_cue || !have_bios || !have_output || !have_runtime || o.cc.empty() || o.budget == 0 || (argc - 3) % 2 != 0) {
    std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
    return 2;
  }
  std::string self = fs::absolute(argv[0]).string();
#if defined(__linux__)
  { std::error_code exe_ec; const fs::path exe = fs::read_symlink("/proc/self/exe", exe_ec); if (!exe_ec && !exe.empty()) self = exe.string(); }
#endif
  Builder builder(std::move(o), self);
  try {
    return builder.run();
  } catch (const std::exception &e) {   // malformed tool output must be a typed failure, never an abort
    std::cout << "@result failed stage=build message=internal_error" << std::endl;
    std::cerr << "internal error: " << e.what() << '\n';
    return 3;
  }
}
