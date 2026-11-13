// `segarecomp mod diff-run` and `segarecomp mod test` (SEG-136-T007, ADR 0300): the deterministic Vanilla-versus-Modded workflow.
//
// Both commands build ordinary generated-native programs with the UNMODIFIED product (`segarecomp build`: the Vanilla program from the original image, the Modded one with `--luau-package`, or from
// the effective image of a data-only package), run them headless with the SAME scripted pad input under the existing frame capture (frame digests, work-RAM region samples, the optional PCM
// digest), and compare. Everything is bounded and explicit: the caller names an instruction budget, a wall-clock timeout per run and a frame count; the build has a fixed timeout. Nothing is
// executed in this process: the guest programs are children that are killed on timeout. The report is one JSON object; the class of a refusal is stable, the text is not. Real-game use stays
// local: `--aggregate` reports counts and digests only (no byte values) and nothing is written outside the work directory.
#include "mod_run.hpp"

#include "luau_build.hpp"
#include "process_run.hpp"
#include "segarecomp/mod.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace segarecomp_cli {

namespace fs = std::filesystem;
using namespace segarecomp;

namespace {

constexpr unsigned kBuildTimeoutSeconds = 900;
constexpr unsigned kSliceDispatches = 16;   // the capture applies a keyed pad entry at slice resolution: 16 dispatches keeps it within a fraction of a frame
constexpr std::uint64_t kBudgetMax = 2000000000ULL;
constexpr unsigned kTimeoutMax = 3600;
constexpr unsigned kFramesMax = 64;
constexpr std::size_t kChangedBytesMax = 32;

std::string esc(const std::string &s) {
  std::string o;
  for (const char c : s) {
    if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
    else if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) o.push_back(c);
    else o.push_back('?');
  }
  return o;
}

struct Refusal {
  std::string code, message;
};
int refuse(const Refusal &r) {
  std::cout << "{\"ok\":false,\"class\":\"" << esc(r.code) << "\",\"error\":\"" << esc(r.message) << "\"}\n";
  return 1;
}

bool is_hex(const std::string &s, std::size_t n) { return s.size() == n && s.find_first_not_of("0123456789abcdef") == std::string::npos; }

bool read_file(const fs::path &p, std::vector<std::uint8_t> &out, std::uintmax_t limit) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec || size > limit) return false;
  std::ifstream in(p, std::ios::binary);
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return static_cast<bool>(in) || in.eof();
}

// ---- argument grammar (shared by both commands and by the scenarios) ----
// pad script "<frame>:<player 1|2>:<mask 0..255, decimal or 0x hex>[,...]" with non-decreasing frames: the same grammar the capture parses (frame_capture.c), validated here first.
bool pad_script_ok(const std::string &text) {
  if (text.empty()) return true;
  std::size_t count = 0;
  std::uint64_t previous = 0;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t end = text.find(',', at);
    const std::string entry = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    std::vector<std::string> parts;
    std::size_t from = 0;
    for (std::size_t i = 0; i <= entry.size(); ++i)
      if (i == entry.size() || entry[i] == ':') { parts.push_back(entry.substr(from, i - from)); from = i + 1; }
    if (parts.size() != 3 || parts[0].empty() || parts[0].size() > 9 || parts[0].find_first_not_of("0123456789") != std::string::npos) return false;
    if (parts[1] != "1" && parts[1] != "2") return false;
    const std::string &m = parts[2];
    const bool hex = m.size() > 2 && (m.compare(0, 2, "0x") == 0 || m.compare(0, 2, "0X") == 0);
    if (m.empty() || (hex ? m.find_first_not_of("0123456789abcdefABCDEF", 2) != std::string::npos || m.size() > 4 : m.size() > 3 || m.find_first_not_of("0123456789") != std::string::npos)) return false;
    if ((hex ? std::stoul(m, nullptr, 16) : std::stoul(m)) > 255) return false;
    const std::uint64_t frame = std::stoull(parts[0]);
    if (frame < previous) return false;
    previous = frame;
    if (++count > 4096) return false;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return true;
}

struct Region {
  std::uint32_t offset = 0, length = 0;
};
// "<offset>:<length>" decimal or 0x hex offsets; at most 8 regions and 1024 bytes in total; inside `limit`.
std::optional<std::vector<Region>> parse_regions(const std::vector<std::string> &texts, std::uint64_t limit) {
  std::vector<Region> out;
  std::uint64_t total = 0;
  for (const std::string &t : texts) {
    const std::size_t colon = t.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= t.size()) return std::nullopt;
    const auto number = [](const std::string &s) -> std::optional<std::uint64_t> {
      if (s.empty() || s.size() > 10) return std::nullopt;
      const bool hex = s.size() > 2 && s.compare(0, 2, "0x") == 0;
      if (hex ? s.find_first_not_of("0123456789abcdef", 2) != std::string::npos : s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
      return hex ? std::stoull(s.substr(2), nullptr, 16) : std::stoull(s);
    };
    const auto offset = number(t.substr(0, colon)), length = number(t.substr(colon + 1));
    if (!offset || !length || *length == 0 || *offset > limit || *length > limit - *offset) return std::nullopt;
    total += *length;
    if (total > 1024 || out.size() >= 8) return std::nullopt;
    out.push_back({static_cast<std::uint32_t>(*offset), static_cast<std::uint32_t>(*length)});
  }
  return out;
}

// ---- reading the child's report lines (the lines are produced by this repository's own capture seam) ----
std::optional<std::string> line_with(const std::string &log, const std::string &tag) {
  std::size_t at = 0;
  while (at < log.size()) {
    std::size_t end = log.find('\n', at);
    if (end == std::string::npos) end = log.size();
    if (log.compare(at, tag.size() + 1, tag + " ") == 0) return log.substr(at + tag.size() + 1, end - at - tag.size() - 1);
    at = end + 1;
  }
  return std::nullopt;
}
std::string str_field(const std::string &j, const std::string &key, std::size_t from = 0) {
  const std::string k = "\"" + key + "\":\"";
  const std::size_t at = j.find(k, from);
  if (at == std::string::npos) return {};
  const std::size_t end = j.find('"', at + k.size());
  return end == std::string::npos ? std::string() : j.substr(at + k.size(), end - at - k.size());
}
std::uint64_t num_field(const std::string &j, const std::string &key) {
  const std::string k = "\"" + key + "\":";
  const std::size_t at = j.find(k);
  if (at == std::string::npos) return 0;
  return std::strtoull(j.c_str() + at + k.size(), nullptr, 10);
}
std::vector<std::string> list_field(const std::string &j, const std::string &key) {
  std::vector<std::string> out;
  const std::string k = "\"" + key + "\":[";
  std::size_t at = j.find(k);
  if (at == std::string::npos) return out;
  at += k.size();
  while (at < j.size() && j[at] != ']') {
    if (j[at] == '"') {
      const std::size_t end = j.find('"', at + 1);
      if (end == std::string::npos) break;
      out.push_back(j.substr(at + 1, end - at - 1));
      at = end + 1;
    } else {
      ++at;
    }
  }
  return out;
}

struct ModResult {
  bool present = false, faulted = false, nondeterministic = false;
  std::string id, fault, write_digest, overlay_sha256, roundtrip_status, state_sha256;
  std::uint64_t callbacks = 0, committed_writes = 0, overlay_ops = 0;
};
struct Observed {
  std::string outcome, final_state, pcm, frame_log_error;
  std::uint64_t frames_captured = 0, frames_published = 0;
  std::vector<std::string> digests, ram_frames;
  std::vector<Region> regions;
  ModResult mod;
};

struct Workspace {
  fs::path dir;
  bool keep = false;
  ~Workspace() {
    if (keep || dir.empty()) return;
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

fs::path self_exe() { return segarecomp_cli::self_executable(); }

// Builds one program. `package` empty = Vanilla (or the effective image of a data-only package, written as `image`).
std::optional<Refusal> build_program(const RunOptions &o, const fs::path &image, const fs::path &out, const fs::path &package, const std::string &identity) {
  std::vector<std::string> argv{self_exe().string(), "build", "--rom", image.string(), "--output", out.string(), "--cc", o.cc, "--runtime-dir", (o.sdk / "genesis").string(), "--optimize", "0", "--runtime-optimize", "1"};
  if (!package.empty()) { argv.push_back("--luau-package"); argv.push_back(package.string()); argv.push_back("--luau-identity"); argv.push_back(identity); }
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  const fs::path log = out.parent_path() / (out.filename().string() + "-build.log");
  const int rc = run_process(argv, log, {}, kBuildTimeoutSeconds);
  if (rc == kProcessTimedOut) return Refusal{"E_BUILD_TIMEOUT", "the program build exceeded " + std::to_string(kBuildTimeoutSeconds) + " seconds"};
  if (rc != 0) {
    std::ifstream in(log, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string failed = [&] { const std::size_t at = text.rfind("@result failed"); return at == std::string::npos ? std::string() : text.substr(at, text.find('\n', at) - at); }();
    return Refusal{"E_BUILD_FAILED", "the program did not build" + (failed.empty() ? std::string() : ": " + failed)};
  }
  return std::nullopt;
}

struct RunSpec {
  std::uint64_t frames, budget;
  unsigned timeout;
  std::string input;
  std::vector<Region> regions;
  std::uint64_t checkpoint = 0;
  bool want_pcm = false;
  std::uint64_t first = 1;
  bool expect_mod = false;   // the program was built with a scheduled package: its MOD_LOADED and MOD_SUMMARY lines are required
};

// Runs a built program under the capture and parses what it reported.
std::optional<Refusal> run_program(const fs::path &out, const RunSpec &spec, const fs::path &frames_dir, Observed &obs) {
  std::error_code ec;
  fs::create_directories(frames_dir, ec);
#if defined(_WIN32)
  const fs::path exe = out / "game.exe";
#else
  const fs::path exe = out / "game";
#endif
  Environment env{{"SEGARECOMP_CAPTURE_DIR", frames_dir.string()}, {"SEGARECOMP_CAPTURE_FIRST", std::to_string(spec.first)}, {"SEGARECOMP_CAPTURE_COUNT", std::to_string(spec.frames)},
                  {"SEGARECOMP_CAPTURE_SLICE", std::to_string(kSliceDispatches)}, {"SEGARECOMP_CAPTURE_STRIDE", "1"}, {"SEGARECOMP_PAD_SCRIPT", spec.input},
                  {"SEGARECOMP_MOD_LOAD", ""}, {"SEGARECOMP_MOD_SAVE", ""}, {"SEGARECOMP_MOD_CHECKPOINT", spec.checkpoint != 0 ? std::to_string(spec.checkpoint) : std::string()}, {"SEGARECOMP_SOUND_SUMMARY", "1"}};   // every observation variable is set: the caller's environment never reaches the guest
  if (!spec.regions.empty()) {
    std::string ram;
    for (const Region &r : spec.regions) ram += (ram.empty() ? "" : ",") + std::to_string(r.offset) + ":" + std::to_string(r.length);
    env.emplace_back("SEGARECOMP_CAPTURE_RAM", ram);
  }
  const fs::path log = out / "run.log";
  // Every inherited SEGARECOMP_* variable that is not set above is withheld from the guest: the observation depends on the flags alone.
  const int rc = run_process({exe.string(), "--instruction-budget", std::to_string(spec.budget)}, log, env, spec.timeout, nullptr, "SEGARECOMP_");
  if (rc == kProcessTimedOut) return Refusal{"E_RUN_TIMEOUT", "the program run exceeded its " + std::to_string(spec.timeout) + " second timeout"};
  std::ifstream in(log, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (const auto refused = line_with(text, "MOD_REFUSED")) return Refusal{"E_MOD_REFUSED", "the program refused the mod: " + str_field(*refused, "class")};
  // Exit semantics of the capture seam (frame_capture.c): a completed window or an exhausted allowance returns the runner's resource-limit transfer (exit 1), a typed guest stop or guest completion
  // exits 0; 3 (malformed option), 4 (I/O), 5 (mod refused) and anything from a signal (128+) are failures, whatever the program printed before.
  if (rc != 0 && rc != 1) return Refusal{"E_RUN_FAILED", "the program exited with status " + std::to_string(rc) + " (expected 0 or 1)"};
  const auto summary = line_with(text, "CAPTURE_SUMMARY");
  if (!summary) return Refusal{"E_RUN_FAILED", "the program produced no capture summary (exit " + std::to_string(rc) + ")"};
  obs.outcome = str_field(*summary, "outcome");
  obs.frames_captured = num_field(*summary, "frames_captured");
  obs.frames_published = num_field(*summary, "frames_published");
  obs.digests = list_field(*summary, "digests");
  obs.ram_frames = list_field(*summary, "ram_frames");
  obs.final_state = str_field(*summary, "final_work_ram_sha256");
  obs.regions = spec.regions;
  if (const auto m = line_with(text, "MOD_SUMMARY")) {
    obs.mod.present = true;
    obs.mod.id = str_field(*m, "mod");
    obs.mod.faulted = m->find("\"faulted\":true") != std::string::npos;
    obs.mod.fault = str_field(*m, "fault");
    obs.mod.write_digest = str_field(*m, "write_digest");
    obs.mod.overlay_sha256 = str_field(*m, "overlay_sha256");
    obs.mod.callbacks = num_field(*m, "callbacks");
    obs.mod.committed_writes = num_field(*m, "committed_writes");
    obs.mod.overlay_ops = num_field(*m, "overlay_ops");
    obs.mod.nondeterministic = m->find("\"nondeterministic\":true") != std::string::npos;
    const std::size_t rt = m->find("\"roundtrip\":");
    obs.mod.roundtrip_status = rt == std::string::npos ? std::string() : str_field(*m, "status", rt);
    obs.mod.state_sha256 = rt == std::string::npos ? std::string() : str_field(*m, "state_sha256", rt);
  }
  if (spec.expect_mod && (!obs.mod.present || !line_with(text, "MOD_LOADED"))) return Refusal{"E_RUN_FAILED", "the program was built with a mod but did not report it (no MOD_LOADED / MOD_SUMMARY)"};
  if (!spec.expect_mod && obs.mod.present) return Refusal{"E_RUN_FAILED", "a program built without a mod reported one"};
  if (spec.want_pcm) {
    if (const auto s = line_with(text, "SOUND_SUMMARY")) {
      const std::size_t audio = s->find("\"audio\":{");
      if (audio != std::string::npos) obs.pcm = str_field(*s, "sha256", audio);
    }
  }
  if (obs.outcome != "window_complete" && obs.outcome != "incomplete_guest_stop" && obs.outcome != "incomplete_guest_complete")
    return Refusal{"E_RUN_INCOMPLETE", "the run ended with outcome " + obs.outcome + " before the requested frames (raise the budget or lower --frames)"};
  if (obs.frames_captured < spec.frames) return Refusal{"E_RUN_INCOMPLETE", "the program stopped (" + obs.outcome + ") after " + std::to_string(obs.frames_captured) + " of " + std::to_string(spec.frames) + " frames"};
  std::uint64_t ram_bytes = 0;
  for (const Region &r : spec.regions) ram_bytes += r.length;
  if (obs.digests.size() != spec.frames || obs.frames_captured != spec.frames || (ram_bytes != 0 && obs.ram_frames.size() != spec.frames) || (ram_bytes != 0 && obs.final_state.size() != 64))
    return Refusal{"E_RUN_FAILED", "the capture report does not hold the requested " + std::to_string(spec.frames) + " frame digests and RAM samples"};
  for (const std::string &f : obs.ram_frames)
    if (f.size() != ram_bytes * 2) return Refusal{"E_RUN_FAILED", "a RAM sample has the wrong length"};
  if (spec.want_pcm && obs.pcm.size() != 64) return Refusal{"E_RUN_FAILED", "the program reported no PCM digest"};
  return std::nullopt;
}

std::string observed_json(const Observed &o, bool aggregate) {
  std::string j = "{\"outcome\":\"" + esc(o.outcome) + "\",\"frames_captured\":" + std::to_string(o.frames_captured) + ",\"frames_published\":" + std::to_string(o.frames_published) +
                  ",\"final_frame_sha256\":\"" + (o.digests.empty() ? std::string() : o.digests.back()) + "\",\"final_work_ram_sha256\":\"" + o.final_state + "\"";
  if (!o.pcm.empty()) j += ",\"pcm_sha256\":\"" + o.pcm + "\"";
  if (o.mod.present) {
    j += ",\"mod\":{\"id\":\"" + esc(o.mod.id) + "\",\"faulted\":" + (o.mod.faulted ? "true" : "false") + ",\"fault\":\"" + esc(o.mod.fault) + "\",\"callbacks\":" + std::to_string(o.mod.callbacks) +
         ",\"committed_writes\":" + std::to_string(o.mod.committed_writes) + ",\"write_digest\":\"" + o.mod.write_digest + "\",\"overlay_ops\":" + std::to_string(o.mod.overlay_ops) + ",\"overlay_sha256\":\"" +
         o.mod.overlay_sha256 + "\",\"nondeterministic\":" + (o.mod.nondeterministic ? "true" : "false") + "}";
  }
  (void)aggregate;
  return j + "}";
}

// Per-frame comparison. Frame ordinals are 1-based publication ordinals.
struct Divergence {
  bool any = false;
  std::uint64_t frame = 0, diverged_frames = 0;
  bool frame_differs = false, ram_differs = false;
  std::string detail;   // JSON members (no braces)
};

std::string changed_json(const std::vector<Region> &regions, const std::string &a, const std::string &b, bool aggregate) {
  // a, b: hex of the concatenated region bytes of one frame
  std::string out = "\"ram\":[";
  std::size_t at = 0;
  bool first_region = true;
  for (std::size_t r = 0; r < regions.size(); ++r) {
    std::size_t changed = 0;
    std::string bytes;
    for (std::uint32_t i = 0; i < regions[r].length; ++i) {
      const std::string x = a.substr((at + i) * 2, 2), y = b.substr((at + i) * 2, 2);
      if (x == y) continue;
      if (++changed <= kChangedBytesMax && !aggregate) bytes += std::string(bytes.empty() ? "" : ",") + "{\"offset\":" + std::to_string(regions[r].offset + i) + ",\"vanilla\":" + std::to_string(std::stoul(x, nullptr, 16)) + ",\"modded\":" + std::to_string(std::stoul(y, nullptr, 16)) + "}";
    }
    at += regions[r].length;
    if (changed == 0) continue;
    out += std::string(first_region ? "" : ",") + "{\"region\":" + std::to_string(r) + ",\"offset\":" + std::to_string(regions[r].offset) + ",\"length\":" + std::to_string(regions[r].length) + ",\"changed_byte_count\":" + std::to_string(changed);
    if (!aggregate) out += ",\"changed_bytes\":[" + bytes + "],\"truncated\":" + (changed > kChangedBytesMax ? "true" : "false");
    out += "}";
    first_region = false;
  }
  return out + "]";
}

Divergence compare(const Observed &v, const Observed &m, bool aggregate, std::uint64_t first) {
  Divergence d;
  const std::size_t n = std::min(v.digests.size(), m.digests.size());
  for (std::size_t i = 0; i < n; ++i) {
    const bool fd = v.digests[i] != m.digests[i];
    const bool rd = i < v.ram_frames.size() && i < m.ram_frames.size() && v.ram_frames[i] != m.ram_frames[i];
    if (!fd && !rd) continue;
    ++d.diverged_frames;
    if (d.any) continue;
    d.any = true;
    d.frame = first + i;   // the absolute 1-based publication ordinal
    d.frame_differs = fd;
    d.ram_differs = rd;
    d.detail = "\"frame_digest_differs\":" + std::string(fd ? "true" : "false") + ",\"ram_differs\":" + (rd ? "true" : "false");
    if (rd) d.detail += "," + changed_json(v.regions, v.ram_frames[i], m.ram_frames[i], aggregate);
  }
  return d;
}

// A deterministic private work directory (random name; never reaches an output).
fs::path make_work(const RunOptions &o, bool &keep) {
  keep = !o.work.empty();
  std::error_code ec;
  if (keep) { fs::create_directories(o.work, ec); return o.work; }
  std::random_device entropy;
  char suffix[17];
  std::snprintf(suffix, sizeof suffix, "%08x%08x", entropy(), entropy());
  const fs::path dir = fs::temp_directory_path() / ("segarecomp-modrun-" + std::string(suffix));
  fs::create_directories(dir, ec);
  return dir;
}

struct Target {
  mod::Package package;
  std::vector<std::uint8_t> package_bytes;
  fs::path project_dir;
};

std::optional<Refusal> load_target(const std::string &target, Target &t) {
  std::error_code ec;
  if (fs::is_directory(target, ec)) {
    const mod::Checked c = mod::check_project(target);
    if (!c.ok) return Refusal{c.rejection.code, c.rejection.message};
    t.package = c.reread;
    t.package_bytes = c.package;
    t.project_dir = target;
    return std::nullopt;
  }
  if (!read_file(target, t.package_bytes, mod::kMaxArchiveBytes)) return Refusal{"E_ARCHIVE_ENTRY", "the package cannot be read"};
  t.package = mod::read_package(t.package_bytes);
  if (!t.package.ok) return Refusal{t.package.rejection.code, t.package.rejection.message};
  return std::nullopt;
}

struct Prepared {
  std::vector<std::uint8_t> original, effective;
  fs::path original_path, modded_input, package_path;
  std::string identity;
  bool luau = false;
};

// Validates the common arguments and prepares the two build inputs (the original image; the package or the effective image).
std::optional<Refusal> prepare(const RunOptions &o, const Target &t, const fs::path &work, Prepared &p) {
  if (!read_file(o.rom, p.original, mod::kMaxImageBytes)) return Refusal{"E_IMAGE_MISMATCH", "the original image cannot be read"};
  const mod::EffectiveImage e = mod::effective_image(t.package, p.original);
  if (!e.ok) return Refusal{e.rejection.code, "the image is not one the package declares: " + e.rejection.message};
  p.effective = e.image;
  p.original_path = o.rom;
  p.identity = t.package.identity;
  const std::string trust = mod::trust_class(t.package.manifest);
  p.luau = trust == "luau";
  if (trust == "native") return Refusal{"E_LIVE_ABI", "a native ABI 2 provider is not linked into a built program yet; diff-run and test accept data-only and Luau-only packages"};
  std::error_code ec;
  fs::create_directories(work, ec);
  if (p.luau) {
    p.package_path = work / "mod.segapatch";
    std::ofstream out(p.package_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(t.package_bytes.data()), static_cast<std::streamsize>(t.package_bytes.size()));
    if (!out) return Refusal{"E_ARCHIVE_ENTRY", "the work directory cannot be written"};
  } else {
    p.modded_input = work / "effective.rom";
    std::ofstream out(p.modded_input, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(p.effective.data()), static_cast<std::streamsize>(p.effective.size()));
    if (!out) return Refusal{"E_ARCHIVE_ENTRY", "the work directory cannot be written"};
  }
  return std::nullopt;
}

std::optional<Refusal> common_args(const RunOptions &o) {
  if (o.target.empty() || o.rom.empty()) return Refusal{"E_ARGUMENT", "a package or project and --rom <original image> are required"};
  if (o.cc.empty() || o.sdk.empty()) return Refusal{"E_ARGUMENT", "--cc <compiler> and --sdk-dir <platforms-dir> are required (the programs are built with the unmodified product)"};
  return std::nullopt;
}

std::optional<Refusal> bounds(std::uint64_t frames, std::uint64_t budget, std::uint64_t timeout, bool has_frames, bool has_budget, bool has_timeout) {
  if (!has_budget || budget == 0 || budget > kBudgetMax) return Refusal{"E_BUDGET", "an explicit --budget <instructions> from 1 to " + std::to_string(kBudgetMax) + " is required"};
  if (!has_timeout || timeout == 0 || timeout > kTimeoutMax) return Refusal{"E_TIMEOUT", "an explicit --timeout <seconds> from 1 to " + std::to_string(kTimeoutMax) + " is required"};
  if (!has_frames || frames == 0 || frames > kFramesMax) return Refusal{"E_FRAMES", "--frames must be from 1 to " + std::to_string(kFramesMax)};
  return std::nullopt;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ diff-run
int run_diff(const RunOptions &o) {
  if (const auto r = common_args(o)) return refuse(*r);
  if (const auto r = bounds(o.frames, o.budget, o.timeout, o.has_frames, o.has_budget, o.has_timeout)) return refuse(*r);
  if (!pad_script_ok(o.input)) return refuse({"E_INPUT_SCRIPT", "--input is \"<frame>:<player 1|2>:<mask 0..255>[,...]\" with non-decreasing frames"});
  for (const std::string &r : o.ram) if (!r.empty() && r[0] == '@') return refuse({"E_SYMBOL_SPACE", "work-RAM regions are numeric: a symbol names an image address and is never reinterpreted as work RAM"});
  const auto regions = parse_regions(o.ram, 65536);
  if (!regions) return refuse({"E_REGION", "--ram is \"<offset>:<length>\" inside the 64 KiB work RAM, at most 8 regions and 1024 bytes in total"});
  std::vector<std::string> code_texts = o.code;
  const auto code_probe = [&] { return parse_regions(code_texts, 0xFFFFFF); };
  bool have_symbols = false;
  for (const std::string &c : o.code) have_symbols = have_symbols || (!c.empty() && c[0] == '@');
  if (have_symbols) {   // resolved after the package is loaded (below)
  } else if (!code_probe()) return refuse({"E_REGION", "--code is \"<offset>:<length>\" or \"@<verified-symbol>[+0xN]:<length>\" inside the image, at most 8 regions and 1024 bytes in total"});
  std::optional<std::vector<Region>> code;
  if (!have_symbols) code = code_probe();
  Target t;
  if (const auto r = load_target(o.target, t)) return refuse(*r);
  Workspace ws;
  ws.dir = make_work(o, ws.keep);
  Prepared p;
  if (const auto r = prepare(o, t, ws.dir, p)) return refuse(*r);
  if (have_symbols) {   // `@name[+0xN]:length`: the VERIFIED symbol of the exact revision this image is (never a manual or imported-hint entry), an image address; numeric regions pass through unchanged
    std::string label;
    for (const auto &rev : t.package.manifest.revisions) if (rev.sha256 == sha256_hex(p.original)) label = rev.label;
    if (label.empty()) return refuse({"E_SYMBOL_REVISION", "the image is not a revision the package declares, so no symbol applies"});
    for (std::string &text : code_texts) {
      if (text.empty() || text[0] != '@') continue;
      const std::size_t colon = text.find(':');
      if (colon == std::string::npos || colon < 2) return refuse({"E_REGION", "a symbol region is \"@<name>[+0xN]:<length>\""});
      std::string name = text.substr(1, colon - 1);
      std::uint64_t addend = 0;
      if (const std::size_t plus = name.find('+'); plus != std::string::npos) {
        const std::string hex = name.substr(plus + 1);
        name.resize(plus);
        if (hex.size() < 3 || hex.size() > 10 || hex.compare(0, 2, "0x") != 0 || hex.find_first_not_of("0123456789abcdef", 2) != std::string::npos) return refuse({"E_REGION", "the symbol addend is \"+0x\" and lower-case hex digits"});
        addend = std::stoull(hex.substr(2), nullptr, 16);
      }
      mod::Rejection why;
      const auto address = mod::verified_symbol(t.package.manifest, name, label, why);
      if (!address) return refuse({why.code, why.message});
      text = std::to_string(*address + addend) + text.substr(colon);
    }
  }
  code = code_probe();
  if (!code) return refuse({"E_REGION", "--code is \"<offset>:<length>\" or \"@<verified-symbol>[+0xN]:<length>\" inside the image, at most 8 regions and 1024 bytes in total"});

  if (o.first == 0 || o.first > 2000000) return refuse({"E_FRAMES", "--first must be from 1 to 2000000"});
  RunSpec spec{o.frames, o.budget, o.timeout, o.input, *regions, 0, true, o.first, false};
  RunSpec modded_spec = spec;
  modded_spec.expect_mod = p.luau;
  Observed vanilla, modded;
  if (const auto r = build_program(o, p.original_path, ws.dir / "vanilla", {}, {})) return refuse(*r);
  if (const auto r = p.luau ? build_program(o, p.original_path, ws.dir / "modded", p.package_path, p.identity) : build_program(o, p.modded_input, ws.dir / "modded", {}, {})) return refuse(*r);
  if (const auto r = run_program(ws.dir / "vanilla", spec, ws.dir / "vanilla-frames", vanilla)) return refuse({r->code, "vanilla: " + r->message});
  if (const auto r = run_program(ws.dir / "modded", modded_spec, ws.dir / "modded-frames", modded)) return refuse({r->code, "modded: " + r->message});
  const Divergence d = compare(vanilla, modded, o.aggregate, o.first);
  std::string codes = "[";
  for (std::size_t i = 0; i < code->size(); ++i) {
    const Region &r = (*code)[i];
    std::size_t changed = 0;
    std::string bytes;
    for (std::uint32_t k = 0; k < r.length; ++k) {
      const std::uint64_t at = static_cast<std::uint64_t>(r.offset) + k;
      const int a = at < p.original.size() ? p.original[at] : -1, b = at < p.effective.size() ? p.effective[at] : -1;
      if (a == b) continue;
      if (++changed <= kChangedBytesMax && !o.aggregate) bytes += std::string(bytes.empty() ? "" : ",") + "{\"offset\":" + std::to_string(at) + ",\"vanilla\":" + std::to_string(a) + ",\"modded\":" + std::to_string(b) + "}";
    }
    codes += std::string(i ? "," : "") + "{\"offset\":" + std::to_string(r.offset) + ",\"length\":" + std::to_string(r.length) + ",\"changed_byte_count\":" + std::to_string(changed);
    if (!o.aggregate) codes += ",\"changed_bytes\":[" + bytes + "]";
    codes += "}";
  }
  codes += "]";
  const bool state_differs = vanilla.final_state != modded.final_state;
  const bool pcm_differs = vanilla.pcm != modded.pcm;
  std::cout << "{\"ok\":true,\"command\":\"diff-run\",\"identical\":" << (!d.any && !state_differs && !pcm_differs && !modded.mod.faulted ? "true" : "false") << ",\"package_identity\":\"" << t.package.identity
            << "\",\"first_frame\":" << o.first << ",\"frames\":" << o.frames << ",\"bounds\":{\"budget\":" << o.budget << ",\"timeout_seconds\":" << o.timeout << ",\"build_timeout_seconds\":" << kBuildTimeoutSeconds
            << ",\"slice_dispatches\":" << kSliceDispatches << "},\"input\":\"" << esc(o.input) << "\",\"aggregate\":" << (o.aggregate ? "true" : "false") << ",\"vanilla\":" << observed_json(vanilla, o.aggregate)
            << ",\"modded\":" << observed_json(modded, o.aggregate) << ",\"diverged_frames\":" << d.diverged_frames << ",\"mod_faulted\":" << (modded.mod.faulted ? "true" : "false") << ",\"final_state_differs\":" << (state_differs ? "true" : "false")
            << ",\"pcm_differs\":" << (pcm_differs ? "true" : "false") << ",\"first_divergence\":";
  if (d.any) std::cout << "{\"frame\":" << d.frame << "," << d.detail << "}";
  else std::cout << "null";
  std::cout << ",\"code\":" << codes << "}\n";
  return 0;
}

// ------------------------------------------------------------------------------------------------ test
namespace {

struct Scenario {
  std::string name, input;
  std::uint64_t frames = 0, budget = 0, timeout = 0, checkpoint = 0;
  bool has_frames = false, has_budget = false, has_timeout = false;
  std::vector<std::string> ram, expect_frames, expect_ram_frames;
  std::string expect_frame, expect_state, expect_pcm;
};

// A closed TOML subset for tests.toml: `[[scenario]]` tables with string, integer and string-array values (the same shape as mod.toml; no inline tables, no multi-line strings).
std::optional<Refusal> parse_scenarios(const std::string &text, std::vector<Scenario> &out) {
  std::istringstream in(text);
  std::string line;
  std::size_t number = 0;
  const auto fail = [&](const std::string &m) { return Refusal{"E_TEST_SYNTAX", "tests.toml line " + std::to_string(number) + ": " + m}; };
  Scenario *cur = nullptr;
  std::set<std::string> names;
  while (std::getline(in, line)) {
    ++number;
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (line == "[[scenario]]") {
      if (out.size() >= 32) return fail("at most 32 scenarios");
      out.emplace_back();
      cur = &out.back();
      continue;
    }
    if (cur == nullptr) return fail("expected [[scenario]]");
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) return fail("expected key = value");
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    const auto as_string = [&](const std::string &v, std::string &dst) -> bool {
      if (v.size() < 2 || v.front() != '"' || v.back() != '"' || v.find('"', 1) != v.size() - 1 || v.find('\\') != std::string::npos) return false;
      dst = v.substr(1, v.size() - 2);
      return true;
    };
    const auto as_list = [&](const std::string &v, std::vector<std::string> &dst) -> bool {
      if (v.size() < 2 || v.front() != '[' || v.back() != ']') return false;
      std::size_t at = 1;
      while (at < v.size() - 1) {
        if (v[at] == ' ' || v[at] == ',') { ++at; continue; }
        if (v[at] != '"') return false;
        const std::size_t end = v.find('"', at + 1);
        if (end == std::string::npos || end >= v.size() - 1 + 0) return false;
        dst.push_back(v.substr(at + 1, end - at - 1));
        at = end + 1;
      }
      return true;
    };
    const auto as_number = [&](const std::string &v, std::uint64_t &dst) -> bool {
      if (v.empty() || v.size() > 10 || v.find_first_not_of("0123456789") != std::string::npos) return false;
      dst = std::stoull(v);
      return true;
    };
    bool ok = true;
    if (key == "name") ok = as_string(value, cur->name);
    else if (key == "input") ok = as_string(value, cur->input);
    else if (key == "frames") { ok = as_number(value, cur->frames); cur->has_frames = true; }
    else if (key == "budget") { ok = as_number(value, cur->budget); cur->has_budget = true; }
    else if (key == "timeout") { ok = as_number(value, cur->timeout); cur->has_timeout = true; }
    else if (key == "checkpoint") ok = as_number(value, cur->checkpoint);
    else if (key == "ram") ok = as_list(value, cur->ram);
    else if (key == "expect_frames") ok = as_list(value, cur->expect_frames);
    else if (key == "expect_ram_frames") ok = as_list(value, cur->expect_ram_frames);
    else if (key == "expect_frame") ok = as_string(value, cur->expect_frame);
    else if (key == "expect_state") ok = as_string(value, cur->expect_state);
    else if (key == "expect_pcm") ok = as_string(value, cur->expect_pcm);
    else return fail("unknown key \"" + key + "\"");
    if (!ok) return fail("the value of \"" + key + "\" is malformed");
  }
  if (out.empty()) return Refusal{"E_TEST_SYNTAX", "tests.toml declares no scenario"};
  for (const Scenario &s : out) {
    if (s.name.empty() || s.name.size() > 64 || s.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) return Refusal{"E_TEST_SYNTAX", "a scenario name is a token of 1 to 64 characters"};
    if (!names.insert(s.name).second) return Refusal{"E_TEST_SYNTAX", "the scenario \"" + s.name + "\" is declared twice"};
    if (const auto r = bounds(s.frames, s.budget, s.timeout, s.has_frames, s.has_budget, s.has_timeout)) return Refusal{r->code, "scenario \"" + s.name + "\": " + r->message};
    if (!pad_script_ok(s.input)) return Refusal{"E_INPUT_SCRIPT", "scenario \"" + s.name + "\": invalid input script"};
    for (const std::string &h : s.expect_frames) if (!is_hex(h, 64)) return Refusal{"E_TEST_SYNTAX", "scenario \"" + s.name + "\": expect_frames holds 64-digit lower-case hex digests"};
    if (s.expect_frames.size() > s.frames || s.expect_ram_frames.size() > s.frames) return Refusal{"E_TEST_SYNTAX", "scenario \"" + s.name + "\": more per-frame expectations than frames"};
    for (const std::string &h : s.expect_ram_frames) if (h.empty() || h.size() % 2 != 0 || h.find_first_not_of("0123456789abcdef") != std::string::npos) return Refusal{"E_TEST_SYNTAX", "scenario \"" + s.name + "\": expect_ram_frames holds lower-case hex"};
    if (!s.expect_ram_frames.empty() && s.ram.empty()) return Refusal{"E_TEST_SYNTAX", "scenario \"" + s.name + "\": expect_ram_frames needs ram regions"};
    for (const std::string *h : {&s.expect_frame, &s.expect_state, &s.expect_pcm}) if (!h->empty() && !is_hex(*h, 64)) return Refusal{"E_TEST_SYNTAX", "scenario \"" + s.name + "\": digests are 64 lower-case hex digits"};
    if (!parse_regions(s.ram, 65536)) return Refusal{"E_REGION", "scenario \"" + s.name + "\": invalid ram region"};
    if (s.checkpoint > s.frames) return Refusal{"E_TEST_SYNTAX", "scenario \"" + s.name + "\": the checkpoint lies beyond the frames"};
  }
  return std::nullopt;
}

}  // namespace

int run_test(const RunOptions &o) {
  if (const auto r = common_args(o)) return refuse(*r);
  Target t;
  if (const auto r = load_target(o.target, t)) return refuse(*r);
  if (t.project_dir.empty()) return refuse({"E_ARGUMENT", "mod test needs a project directory (its tests.toml declares the scenarios)"});
  std::vector<std::uint8_t> raw;
  if (!read_file(t.project_dir / "tests.toml", raw, 262144)) return refuse({"E_TEST_SYNTAX", "the project has no readable tests.toml"});
  std::vector<Scenario> scenarios;
  if (const auto r = parse_scenarios(std::string(raw.begin(), raw.end()), scenarios)) return refuse(*r);
  if (!o.scenario.empty() && std::none_of(scenarios.begin(), scenarios.end(), [&](const Scenario &s) { return s.name == o.scenario; })) return refuse({"E_TEST_SCENARIO", "no scenario is named \"" + o.scenario + "\""});
  Workspace ws;
  ws.dir = make_work(o, ws.keep);
  Prepared p;
  if (const auto r = prepare(o, t, ws.dir, p)) return refuse(*r);
  for (const Scenario &sc : scenarios)   // a checkpoint needs a scheduled mod (the MODSTATE round trip): refuse explicitly rather than let a scenario appear to pass
    if (sc.checkpoint != 0 && !p.luau && (o.scenario.empty() || o.scenario == sc.name))
      return refuse({"E_TEST_CHECKPOINT_UNSUPPORTED", "scenario \"" + sc.name + "\" requests a checkpoint, which needs a Luau mod linked into the program (this package is data-only)"});
  if (const auto r = p.luau ? build_program(o, p.original_path, ws.dir / "modded", p.package_path, p.identity) : build_program(o, p.modded_input, ws.dir / "modded", {}, {})) return refuse(*r);

  bool all = true;
  std::string results = "[";
  bool first = true;
  for (const Scenario &s : scenarios) {
    if (!o.scenario.empty() && s.name != o.scenario) continue;
    RunSpec spec{s.frames, s.budget, static_cast<unsigned>(s.timeout), s.input, *parse_regions(s.ram, 65536), s.checkpoint, !s.expect_pcm.empty(), 1, p.luau};
    Observed obs;
    std::string failure;   // the first divergence, as JSON members
    if (const auto r = run_program(ws.dir / "modded", spec, ws.dir / ("frames-" + s.name), obs)) {
      failure = "\"class\":\"" + esc(r->code) + "\",\"message\":\"" + esc(r->message) + "\"";
    } else {
      if (obs.mod.present && obs.mod.faulted) failure = "\"field\":\"mod_fault\",\"fault\":\"" + esc(obs.mod.fault) + "\"";
      for (std::size_t i = 0; failure.empty() && i < s.expect_frames.size(); ++i)
        if (obs.digests[i] != s.expect_frames[i]) failure = "\"field\":\"frame\",\"frame\":" + std::to_string(i + 1) + ",\"expected\":\"" + s.expect_frames[i] + "\",\"actual\":\"" + obs.digests[i] + "\"";
      for (std::size_t i = 0; failure.empty() && i < s.expect_ram_frames.size(); ++i)
        if (i >= obs.ram_frames.size() || obs.ram_frames[i] != s.expect_ram_frames[i])
          failure = "\"field\":\"ram\",\"frame\":" + std::to_string(i + 1) + ",\"expected\":\"" + s.expect_ram_frames[i] + "\",\"actual\":\"" + (i < obs.ram_frames.size() ? obs.ram_frames[i] : std::string()) + "\"";
      if (failure.empty() && !s.expect_frame.empty() && obs.digests.back() != s.expect_frame)
        failure = "\"field\":\"final_frame\",\"frame\":" + std::to_string(s.frames) + ",\"expected\":\"" + s.expect_frame + "\",\"actual\":\"" + obs.digests.back() + "\"";
      if (failure.empty() && !s.expect_state.empty() && obs.final_state != s.expect_state)
        failure = "\"field\":\"state\",\"expected\":\"" + s.expect_state + "\",\"actual\":\"" + obs.final_state + "\"";
      if (failure.empty() && !s.expect_pcm.empty() && obs.pcm != s.expect_pcm) failure = "\"field\":\"pcm\",\"expected\":\"" + s.expect_pcm + "\",\"actual\":\"" + obs.pcm + "\"";
      // A requested checkpoint passes only with evidence that the capture/restore ran and was lossless: an executed round trip with status ok and the digest of the state it carried.
      if (failure.empty() && s.checkpoint != 0 && !(obs.mod.present && obs.mod.roundtrip_status == "ok" && obs.mod.state_sha256.size() == 64))
        failure = "\"field\":\"checkpoint\",\"status\":\"" + esc(obs.mod.present ? (obs.mod.roundtrip_status.empty() ? std::string("none") : obs.mod.roundtrip_status) : std::string("no_mod")) + "\"";
    }
    all = all && failure.empty();
    results += std::string(first ? "" : ",") + "{\"name\":\"" + s.name + "\",\"passed\":" + (failure.empty() ? "true" : "false") + ",\"frames\":" + std::to_string(s.frames) + ",\"budget\":" + std::to_string(s.budget) +
               ",\"timeout_seconds\":" + std::to_string(s.timeout);
    if (!failure.empty()) results += ",\"first_divergence\":{" + failure + "}";
    results += ",\"observed\":{\"final_frame_sha256\":\"" + (obs.digests.empty() ? std::string() : obs.digests.back()) + "\",\"final_work_ram_sha256\":\"" + obs.final_state + "\"";
    if (!obs.pcm.empty()) results += ",\"pcm_sha256\":\"" + obs.pcm + "\"";
    if (!obs.ram_frames.empty() && !o.aggregate) { results += ",\"ram_frames\":["; for (std::size_t i = 0; i < obs.ram_frames.size(); ++i) results += (i ? ",\"" : "\"") + obs.ram_frames[i] + "\""; results += "]"; }
    results += "}}";
    first = false;
  }
  results += "]";
  std::cout << "{\"ok\":" << (all ? "true" : "false") << (all ? "" : ",\"class\":\"E_TEST_FAILED\"") << ",\"command\":\"test\",\"package_identity\":\"" << t.package.identity << "\",\"bounds\":{\"build_timeout_seconds\":" << kBuildTimeoutSeconds
            << ",\"slice_dispatches\":" << kSliceDispatches << "},\"scenarios\":" << results << "}\n";
  return all ? 0 : 1;
}

}  // namespace segarecomp_cli
