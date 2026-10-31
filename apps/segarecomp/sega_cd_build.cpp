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
#include <array>
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
#include "segarecomp/machine/genesis/z80_images.hpp"
#include "segarecomp/sha256.hpp"

namespace fs = std::filesystem;
namespace gz80 = segarecomp::machine::genesis::z80;
using segarecomp_cli::kProcessTimedOut;
using segarecomp_cli::run_process;

namespace {

constexpr std::uint64_t kSystemAreaBytes = 0x8000;   // the Sega CD system area (the boot area lies in it)
constexpr unsigned kMaxEpochs = 4, kMaxTemplates = 64, kMaxTemplateSlots = 256, kMaxRunSlots = 128, kMaxExtents = 64;   // SEG-098-T004 (ADR 0213): 16 -> 64, measured
constexpr unsigned kMaxStaticExtents = 48;   // SEG-098-T002 (12), T004 (48, ADR 0213): of the 64 disc-image slots (shared with the resolved extents); more is the typed static_extent_bound_exceeded, admitting none
constexpr unsigned kMaxProduced = 8;   // SEG-111 (ADR 0255): produced-code images admitted from build passes (kMaxProducedImages of the emitter)
constexpr unsigned kMaxRounds = 1 + kMaxEpochs + kMaxTemplates + kMaxExtents + kMaxProduced;
constexpr const char *kCacheVersion = "segarecomp.sega_cd_build.objects.v1";
constexpr const char *kViewerFoundation[] = {"sv_settings.c", "sv_layout.c", "sv_menu.c", "sv_input_state.c", "sv_input_diag.c", "sv_font.c", "sv_font_data.c", "sv_ui.c", "sv_host.c", "sv_pages.c", "sv_draw.c"};

struct Options {
  fs::path cue, bios, output, runtime_dir, cache_dir, input_script, backup_ram;
  std::string cc, cxx;
  std::vector<std::string> cc_args, cxx_args, link_args;
  bool cxx_args_given = false;
  fs::path sdl3_include, sdl3_lib, viewer_host_source;   // a project-authored test host replaces the SDL3 viewer sources (tests only)
  std::vector<std::string> pass_sets;                    // register presets of the build pass (project-authored fixtures select a case this way)
  bool viewer = false;
  unsigned jobs = 0, pass_timeout = 900, compile_timeout = 1800;
  std::uint64_t budget = 300000000ULL, until_tick = 0;
  std::string optimize = "1";
  std::string aot_policy = "compatibility";   // SEG-097-T003 (ADR 0208): `optimized` (experimental) selects the frozen gen2 M3 + B35 proposal
  bool aot_model_given = false;
  bool aot_all_images = false;
  bool static_recursion = true;   // SEG-099-T004: follow the proven entries into the modules (0 reproduces the M2 scan)
  std::string static_range = "hull";   // SEG-098-T004: `loaded` admits every loaded byte of a proven module (computed entries beyond the traversed hull), `hull` only the traversed subrange
  std::vector<std::string> emit_args;   // tests only: extra arguments forwarded to every `emit` (e.g. a manual `--disc-image` extent that shadows an address)
  std::string file_first;   // SEG-100 (ADR 0221): "" = off, else the candidate policy (all|structural) of the file-first dynamic images
  std::string file_first_max_bytes, file_first_max_offsets;
  bool boot_fixtures = false;     // SEG-113, tests only: `--boot-fixture-profiles 1` lets a tagged project fixture BIOS match (an assumed, unverified contract); never set by the product
  bool z80_audio = true;          // SEG-115 (ADR 0259): `--z80-audio 0` is the control: no YM2612/PSG devices (as M18: their accesses are accepted and discarded)
  bool z80_alternatives = true;   // SEG-115 (ADR 0259): `--z80-alternatives 0` is the control: the plain RAM guard (no alternative forms)
  bool main_z80 = true;           // SEG-114 (ADR 0258): `--main-z80 0` is the control: no main-side Z80 (the legacy rule: a main read of the Z80 area after it ran is a typed stop)
  bool static_boot = true;        // SEG-113 (ADR 0257): `--static-boot 0` is the control: the boot images are discovered by execution (M16 behaviour)
  bool static_producers = true;   // SEG-112 (ADR 0256): `--static-producers 0` is the control: no generation-time producer proofs (the M15 observed fallback only)
  bool reconstruction = true;     // SEG-111 (ADR 0255): `--reconstruction 0` is the control (and the escape hatch): the build as it was before relocated-fragment and produced-image admission
  bool keep_passes = false;       // SEG-111 diagnostics/tests: keep each round's frontier capture (capture.json, region.bin) under <output>/passes/<round>/
  bool static_discovery = true;   // SEG-098-T002: `--static-discovery 0` is the control (and the escape hatch): the build as it was before the static producer
};

// SEG-115: the C++ driver for the vendored YM2612 core, derived from the C driver exactly as `segarecomp build` does (`<zig> cc` -> `<zig> c++`, cc/gcc/clang -> c++/g++/clang++,
// any other driver compiles the sources with `-x c++`).
void resolve_cxx(Options &o) {
  if (!o.cxx.empty()) return;
  if (!o.cxx_args_given) o.cxx_args = o.cc_args;
  if (!o.cc_args.empty() && o.cc_args.front() == "cc" && !o.cxx_args_given) {
    o.cxx = o.cc;
    o.cxx_args.front() = "c++";
    return;
  }
  const fs::path cc(o.cc);
  std::string name = cc.filename().string();
  std::string suffix;
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) { suffix = ".exe"; name.resize(name.size() - 4); }
  std::string derived;
  if (name == "cc") derived = "c++";
  else if (name == "gcc") derived = "g++";
  else if (name == "clang") derived = "clang++";
  else if (name.size() > 4 && name.compare(0, 4, "gcc-") == 0) derived = "g++" + name.substr(3);
  else if (name.size() > 6 && name.compare(0, 6, "clang-") == 0) derived = "clang++" + name.substr(5);
  else if (name.size() > 4 && name.compare(name.size() - 4, 4, "-gcc") == 0) derived = name.substr(0, name.size() - 3) + "g++";
  else if (name.size() > 3 && name.compare(name.size() - 3, 3, "-cc") == 0) derived = name.substr(0, name.size() - 2) + "c++";
  if (derived.empty()) {
    o.cxx = o.cc;
    if (!o.cxx_args_given) o.cxx_args.insert(o.cxx_args.end(), {"-x", "c++"});
    return;
  }
  o.cxx = (cc.has_parent_path() ? (cc.parent_path() / (derived + suffix)).string() : derived + suffix);
}

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

// The stop classes that name an unmodelled DEVICE capability (hardware/audio/DMA/register behaviour), as opposed to a code-coverage, CPU-architectural or machine-evidence stop.
// An explicit list (runtime stop names, platforms/sega-cd/runtime/scd_machine.c): `instruction_unsupported`, `trace_unsupported` and the exception classes are CPU stops, not device stops.
bool is_device_stop_class(const std::string &cls) {
  static const char *const kDevice[] = {"wordram_cell_image_unsupported", "wordram_dot_image_unsupported", "backup_ram_unsupported", "pcm_unsupported", "gate_array_register_unsupported",
                                        "vdp_access_unsupported", "vdp_dma_unsupported", "cdc_destination_unsupported", "cdd_command_unsupported", "main_device_unsupported", "z80_unsupported"};
  for (const char *name : kDevice) if (cls == name) return true;
  return false;
}

class Builder {
 public:
  Builder(Options options, std::string self) : o_(std::move(options)), self_(std::move(self)) {}

  int run() {
    std::error_code ec;
    if (o_.main_z80 && o_.z80_audio) resolve_cxx(o_);
    fs::create_directories(o_.output, ec);
    if (!fs::is_directory(o_.output)) return fail({"setup", "the output folder cannot be created"});
    log_.open(o_.output / "build.log", std::ios::trunc);
    if (o_.cache_dir.empty()) o_.cache_dir = o_.output / "objects";
    fs::create_directories(o_.cache_dir, ec);
    if (o_.jobs == 0) {
      o_.jobs = std::max(1U, std::thread::hardware_concurrency());
      // The largest generated units need about 0.6 GB of compiler memory each at -O1: unlimited threads on a many-core machine with little free memory
      // end in "out of memory" / "SystemResources" compile failures. Bound the default by the memory available now (1 GiB per job).
      if (const std::uint64_t available = segarecomp_cli::available_memory_bytes(); available != 0U)
        o_.jobs = std::max(1U, std::min<unsigned>(o_.jobs, static_cast<unsigned>(std::min<std::uint64_t>(available >> 30U, 1024U))));
    }
    const std::string bios_text = read_text(o_.bios, 0x20001U);
    const std::string cue_text = read_text(o_.cue, 1U << 20U);
    if (bios_text.empty() || cue_text.empty()) return fail({"setup", "the BIOS or the CUE file cannot be read"});
    bios_sha_ = sha_of(bios_text);
    if (const auto f = key_base()) return fail(*f);

    stage("analyze", "running");
    std::vector<std::string> admits;
    if (!o_.static_discovery) static_status_ = "disabled";
    if (o_.static_boot) static_boot(admits);             // SEG-113: the BIOS boot images, first in the chain: no pass identity, no round
    if (o_.static_discovery) static_discovery(admits);   // SEG-098-T002: first in the chain, so every later admission's identity includes them
    if (o_.static_producers) static_producers(admits);   // SEG-112: proven before the first emission: no pass identity, no round
    unsigned epochs = 0, templates = 0, slot_total = 0;
    for (unsigned round = 0; round < kMaxRounds; ++round) {
      std::cout << "@round " << round << " start" << std::endl;
      frontier_.clear();   // SEG-114: a round that is repeated for new Z80 images must not leave the previous round's candidate frontier behind
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
      history_ += (history_.empty() ? "" : ",") + std::string("{\"round\":") + std::to_string(round) + ",\"program_identity\":\"" + program_identity + "\",\"stop\":\"" + stop_class(report) + "\"";
      unsigned z80_added = 0;   // SEG-114 (ADR 0258): image epochs this pass recorded that the program did not know
      if (const auto f = ingest_z80(o_.output / "pass", z80_added)) return fail(*f);
      if (z80_added != 0) history_ += ",\"z80_added\":" + std::to_string(z80_added);
      const auto saved_refusals = relocation_refusals_;   // a round repeated only for Z80 images must not count the same refusal twice
      const unsigned saved_unresolved = boot_unresolved_;
      if (o_.keep_passes) {
        const fs::path keep = o_.output / "passes" / std::to_string(round);
        fs::create_directories(keep, ec);
        for (const char *name : {"capture.json", "region.bin"}) fs::copy_file(o_.output / "pass" / name, keep / name, fs::copy_options::overwrite_existing, ec);
      }
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
        auto spec = resolve(capture_text, cpu, pc, extents_.size() + static_extents_.size() + boot_extents_.size());
        // An extent that is already admitted explains nothing new: the program stopped inside bytes it has authority for but did not compile (an
        // experimental selective policy omits code on purpose), so the stop is the frontier, not a reason to admit the same bytes again.
        if (spec.extent && (std::find(extents_.begin(), extents_.end(), *spec.extent) != extents_.end() ||
                            std::find(static_extents_.begin(), static_extents_.end(), *spec.extent) != static_extents_.end() ||
                            std::find(boot_extents_.begin(), boot_extents_.end(), *spec.extent) != boot_extents_.end())) {
          spec.extent.reset();
          spec.refusal = "uncompiled_entry_in_admitted_extent";
        }
        // SEG-111 (ADR 0255): only when the exact-source resolver found nothing (its 32/64-byte floors are unchanged), explain the frontier from the
        // build pass's bounded write streams - evidence collected during this very pass, resolved here, admitted through the existing machinery.
        std::string relocation_error;
        bool relocated = false;
        if (o_.reconstruction && !spec.extent && (spec.refusal == "no_source" || spec.refusal == "too_short")) {
          const auto rel = relocate(cpu, program_identity, extents_.size() + static_extents_.size() + boot_extents_.size());
          if (rel.extent) {
            spec.extent = rel.extent;
            relocated = true;
          } else if (rel.produced) {
            produced_.push_back(rel.produced_key);
            admits.push_back("produced:" + program_identity + ":" + *rel.produced);
            ++produced_count_;
            history_ += ",\"admitted\":\"produced\"}";
            continue;
          } else {
            relocation_error = rel.error;
            ++relocation_refusals_[rel.error];
          }
        }
        if (spec.extent) {
          if (!relocated) note_boot_extent(*spec.extent);
          extents_.push_back(*spec.extent);
          admits.push_back("extent:" + program_identity + ":" + *spec.extent);
          if (relocated) ++relocated_count_;
          history_ += relocated ? ",\"admitted\":\"relocated_extent\"}" : ",\"admitted\":\"extent\"}";
          continue;
        }
        note_boot_unresolved(cpu, pc);
        frontier_ = "{\"cpu\":\"" + cpu + "\",\"pc\":" + std::to_string(pc) + ",\"refusal\":\"" + spec.refusal + "\"" +
                    (relocation_error.empty() ? std::string() : ",\"relocation\":\"" + relocation_error + "\"") + "}";
        history_ += ",\"frontier\":" + frontier_;
      }
      history_ += "}";
      if (z80_added != 0) {   // SEG-114: the candidate must run with every Z80 image it executes: rebuild with the registry before confirming
        relocation_refusals_ = saved_refusals;
        boot_unresolved_ = saved_unresolved;
        frontier_.clear();
        continue;
      }
      // the candidate: a confirming pass must reproduce its outcome exactly
      stage("confirm", "running");
      std::string confirm_report, confirm_capture;
      if (const auto f = pass_run(exe, o_.output / "confirm", confirm_report, confirm_capture)) return fail(*f);
      if (confirm_report != report || confirm_capture != capture_text) return fail({"confirm", "materialization_nondeterministic"});   // SEG-111: the evidence itself (streams included) must reproduce, not only its presence
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
    std::ofstream(o_.output / "status.json", std::ios::binary | std::ios::trunc) << "{\"status\":\"failed\",\"stage\":\"" << f.stage << "\",\"message\":\"" << json_escape(f.message) << "\",\"inventory\":" << inventory_json(std::string(), f.stage) << "}\n";
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
    out << "],\"static_extents\":" << static_extents_.size() << ",\"static_discovery\":" << static_json_ << ",\"inventory\":" << inventory_json(report) << ",\"final_stop\":" << final_stop_json(report) << ",\"aot_policy\":" << (aot_policy_json_.empty() ? "null" : aot_policy_json_) << ",\"file_first\":" << (file_first_json_.empty() ? "null" : file_first_json_) << ",\"frontier\":" << (frontier_.empty() ? "null" : frontier_)
        << ",\"reconstruction\":" << reconstruction_json() << ",\"static_boot\":" << static_boot_json() << ",\"z80\":" << z80_json()
        << ",\"result\":\"" << raw_value(report, "result").value_or("") << "\",\"stop\":\"" << stop_class(report)
        << "\",\"viewer\":" << (o_.viewer ? "true" : "false") << ",\"history\":[" << history_ << "]}\n";
    std::ofstream(o_.output / "status.json", std::ios::binary | std::ios::trunc) << out.str();
    std::cout << "@result ok" << std::endl;
    return 0;
  }

  // SEG-098-T004 (ADR 0213): where the final stop's program counter lies relative to what is admitted - the classes the next step depends on:
  // inside an admitted extent (a known image whose start was not compiled), inside a proven module's loaded window but outside its admitted range (a computed
  // entry beyond the traversed hull), or outside every admitted image (an unknown source or a not yet admitted overlay).
  std::string final_stop_json(const std::string &report) const {
    const auto at = report.find("\"stop\":{");
    if (at == std::string::npos) return "null";
    const std::string cls = raw_value(report, "class", at).value_or("none");
    if (cls == "none") return "null";
    const auto cpu = number_value(report, "cpu", at).value_or(0);
    const auto pc = number_value(report, "pc", at).value_or(0);
    const char *want = cpu == 0 ? "main" : "sub";
    const auto inside_at = [&](std::uint64_t pc, const std::vector<std::string> &list, bool window) {
      for (const auto &e : list) {
        std::vector<std::string> f;
        std::stringstream ss(e);
        for (std::string part; std::getline(ss, part, ':');) f.push_back(part);
        if (f.size() < (window ? 3U : 4U) || f[0] != want) continue;
        std::uint64_t base = 0, len = 0;
        try { base = std::stoull(window ? f[1] : f[2]); len = std::stoull(window ? f[2] : f[3]); } catch (const std::exception &) { continue; }
        if (pc >= base && pc < base + len) return true;
      }
      return false;
    };
    const auto locate = [&](std::uint64_t at_pc) {
      return inside_at(at_pc, extents_, false) || inside_at(at_pc, static_extents_, false) || inside_at(at_pc, boot_extents_, false) ? "inside_admitted_extent" : inside_at(at_pc, static_windows_, true) ? "inside_static_module_window" : "outside_admitted_images";
    };
    const bool device = is_device_stop_class(cls);   // a hardware-capability stop is not about code coverage
    const char *where = device ? "device_capability" : locate(pc);
    std::string out = std::string("{\"class\":\"") + cls + "\",\"cpu\":\"" + want + "\",\"location\":\"" + where + "\"";
    // SEG-099-T005 (ADR 0219): an uncompiled COMPUTED destination is reported separately from the transfer instruction (`location` is always about the instruction's pc). The runtime's detail field is the
    // destination only for this stop class; it is absent (0) for a stop that has no computed destination, and then nothing is reported.
    const auto detail = number_value(report, "address", at).value_or(0);
    if (cls == "entry_not_compiled" && detail != 0 && detail != pc) out += ",\"destination\":" + std::to_string(detail) + ",\"destination_location\":\"" + locate(detail) + "\"";
    return out + "}";
  }

  // SEG-098-T005 (ADR 0214): the machine-readable inventory status. `complete` is only claimed under an explicit proof criterion; `partial` = useful inventory with named
  // unresolved parts; `blocked` = a typed barrier stopped further justified compilation. A device/hardware stop (`*_unsupported`) is reported as `device_stop`, not as a code barrier.
  // Zero frontiers on a bounded run prove nothing: the main processor's loaders are not analyzed, so `complete` cannot currently be reached.
  std::string inventory_json(const std::string &report, const std::string &failure_stage = std::string()) const {
    std::vector<std::string> reasons, barriers;
    bool device_stop = false;
    std::string cls = "none";
    if (!failure_stage.empty()) barriers.push_back("build_failed:" + failure_stage);
    if (failure_stage.empty()) {
      cls = stop_class(report);
      if (cls != "none") {
        if (is_device_stop_class(cls)) device_stop = true;
        else barriers.push_back("stop:" + cls);
      }
      if (!frontier_.empty()) barriers.push_back("frontier:" + raw_value(frontier_, "refusal").value_or("unknown"));
    }
    // SEG-099-T004/T005 (ADR 0218/0219): loaders inside admitted SUB modules are analyzed (the recursive walk) unless recursion is off; the MAIN processor's loaders are never analyzed, so
    // `complete` stays unreachable by construction. A scan that needed joins to finish is named: its precision was reduced (never its soundness).
    if (!(o_.static_discovery && o_.static_recursion)) reasons.push_back("nested_loaders_not_analyzed");
    reasons.push_back("main_processor_loaders_not_analyzed");
    if (static_joined_) reasons.push_back("static_scan_joined");
    if (static_status_ == "disabled") reasons.push_back("static_discovery_disabled");
    else if (static_status_ == "not_run") reasons.push_back("static_discovery_not_run");
    else if (static_status_ != "admitted" && static_status_ != "no_modules") reasons.push_back("static:" + static_status_);
    for (const auto &[k, n] : static_refusals_) { (void)n; reasons.push_back("static_refusal:" + k); }
    if (!extents_.empty()) reasons.push_back("execution_derived_extents");
    if (boot_contradictions_ != 0U) reasons.push_back("static_boot_contradicted");
    if (!o_.file_first.empty()) {   // SEG-100 (ADR 0224): a filtered subset is never called a complete executable inventory; the mappings are decided at run time
      reasons.push_back("file_first_runtime_binding");
      if (o_.file_first == "structural") reasons.push_back("file_first_structural_filter");
      if (raw_value(file_first_json_, "complete").value_or("true") != "true") reasons.push_back("file_first_inventory_incomplete");
    }
    const std::string status = !barriers.empty() ? "blocked" : reasons.empty() ? "complete" : "partial";
    std::ostringstream out;
    out << "{\"schema\":\"segarecomp.sega_cd.inventory.v1\",\"status\":\"" << status << "\",\"device_stop\":" << (device_stop ? "true" : "false") << ",\"stop_class\":\"" << cls
        << "\",\"static_extents\":" << static_extents_.size() << ",\"resolved_extents\":" << extents_.size() << ",\"barriers\":[";
    for (std::size_t i = 0; i < barriers.size(); ++i) out << (i ? "," : "") << "\"" << json_escape(barriers[i]) << "\"";
    out << "],\"unresolved\":[";
    for (std::size_t i = 0; i < reasons.size(); ++i) out << (i ? "," : "") << "\"" << json_escape(reasons[i]) << "\"";
    out << "]}";
    return out.str();
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
    if (o_.main_z80 && o_.z80_audio) {   // SEG-115: the C++ driver compiles the vendored YM2612 core
      material += "cxx " + o_.cxx + "\n";
      for (const auto &a : o_.cxx_args) material += "cxxarg " + a + "\n";
    }
    std::vector<fs::path> files;
    for (const char *rel : {"platforms/sega-cd/runtime", "platforms/sega-cd/viewer", "platforms/genesis/runtime", "platforms/genesis/viewer", "platforms/genesis/machine/include", "libs/cpu/m68k/include",
                            "libs/device/sega/genesis/include", "libs/codegen/c11/include", "libs/viewer/include", "libs/viewer/src",
                            "libs/device/sega/psg/include", "libs/device/sega/ym2612/include"}) {
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
    const fs::path rt = o_.runtime_dir;
    const bool cxx = source.extension() == ".cpp";   // SEG-115: the vendored YM2612 core (ymfm) is C++14 behind a C ABI
    std::vector<std::string> c{cxx ? o_.cxx : o_.cc};
    const auto &driver_args = cxx ? o_.cxx_args : o_.cc_args;
    c.insert(c.end(), driver_args.begin(), driver_args.end());
    if (cxx) {
      for (const char *flag : {"-std=c++14", "-fno-exceptions", "-fno-rtti", "-w"}) c.push_back(flag);
      c.push_back("-I" + (rt / "libs/device/sega/ym2612/third_party/ymfm").string());
    } else {
      for (const char *flag : {"-std=c11", "-w"}) c.push_back(flag);
    }
    c.push_back("-O" + o_.optimize);
    for (const fs::path &inc : {rt / "platforms/sega-cd/runtime", rt / "platforms/genesis/runtime", rt / "libs/codegen/c11/include", rt / "libs/device/sega/psg/include",
                                rt / "libs/device/sega/ym2612/include"}) c.push_back("-I" + inc.string());
    if (source.filename() == "scd_z80.c" && o_.z80_audio) c.push_back("-DSCD_Z80_AUDIO=1");   // SEG-115: the Z80 attach carries the sound devices
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
    const std::string key = sha_of(key_base_ + "\n" + (viewer ? "viewer\n" : "") + source.filename().string() + "\n" + extra_text +
                                   (source.extension() == ".cpp" ? "cxx\n" : "") + (source.filename() == "scd_z80.c" && o_.z80_audio ? "audio\n" : "") +
                                   sha_of(read_text(source, 1ULL << 31U)));
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
    if (failure && o_.jobs > 1U) {
      // A parallel compile failure may be resource exhaustion (memory, handles) rather than the source: the units already built stay cached, so
      // the remainder is retried once on one thread, where a real error fails again with its own log.
      log_ << "parallel compile failed; retrying on one thread" << std::endl;
      failure.reset();
      next.store(0);
      for (std::size_t i = 0; i < sources.size() && !failure; ++i) {
        bool built = false;
        if (const auto f = object_for(sources[i], extra, viewer, objects[i], built)) failure = f;
        else if (built) count.fetch_add(1);
      }
    }
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
    if (o_.main_z80) {   // SEG-114 (ADR 0258): the Genesis Z80 machine and the Sega CD attach; the generated image units are per round (prepare_z80)
      out.push_back(o_.runtime_dir / "platforms/genesis/runtime/z80_machine.c");
      out.push_back(o_.runtime_dir / "platforms/sega-cd/runtime/z80/scd_z80.c");
      if (o_.z80_audio) {   // SEG-115 (ADR 0259): the Genesis sound devices and mixer, unchanged: the PSG, the vendored YM2612 core and the shared device glue
        for (const char *n : {"genesis_audio.c", "genesis_mixer.c"}) out.push_back(o_.runtime_dir / "platforms/genesis/runtime" / n);
        out.push_back(o_.runtime_dir / "libs/device/sega/psg/src/sn76489.c");
        out.push_back(o_.runtime_dir / "libs/device/sega/ym2612/src/cxx_runtime_shim.c");
        for (const char *n : {"ymfm_opn.cpp", "ymfm_adpcm.cpp", "ymfm_ssg.cpp"}) out.push_back(o_.runtime_dir / "libs/device/sega/ym2612/third_party/ymfm" / n);
        out.push_back(o_.runtime_dir / "libs/device/sega/ym2612/src/ym2612.cpp");
      }
    }
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
    if (o_.aot_policy == "optimized") cmd.insert(cmd.end(), {"--aot-policy", "optimized", "--aot-model", "gen2"});
    if (o_.aot_all_images) cmd.insert(cmd.end(), {"--aot-scope", "all-images"});
    if (!o_.file_first.empty()) {   // SEG-100: the same selection on every round (it never depends on a pass), so its units are unchanged and their objects are reused
      cmd.insert(cmd.end(), {"--file-first", o_.file_first});
      if (!o_.file_first_max_bytes.empty()) cmd.insert(cmd.end(), {"--file-first-max-bytes", o_.file_first_max_bytes});
      if (!o_.file_first_max_offsets.empty()) cmd.insert(cmd.end(), {"--file-first-max-offsets", o_.file_first_max_offsets});
      if (o_.jobs != 0U) cmd.insert(cmd.end(), {"--jobs", std::to_string(o_.jobs)});
    }
    for (const auto &a : admits) { cmd.push_back("--admit"); cmd.push_back(a); }
    if (o_.main_z80) cmd.insert(cmd.end(), {"--main-z80", "1"});   // SEG-114
    cmd.insert(cmd.end(), o_.emit_args.begin(), o_.emit_args.end());
    if (o_.boot_fixtures) cmd.insert(cmd.end(), {"--boot-fixture-profiles", "1"});
    const fs::path out = o_.output / "emit.json";
    if (run_process(cmd, out, {}, o_.compile_timeout) != 0) return Failure{"generate", "the program could not be generated: " + read_text(out, 4096)};
    const std::string text = read_text(out);
    if (raw_value(text, "ok").value_or("") != "true") return Failure{"generate", "the program could not be generated: " + text.substr(0, 300)};
    identity = raw_value(text, "program_identity").value_or("");
    if (const auto at = text.find("\"file_first\":{"); at != std::string::npos) {   // SEG-100: the selection and compile statistics of the dynamic images
      const auto end = text.find("},\"aot_policy\"", at);
      if (end != std::string::npos) file_first_json_ = text.substr(at + 13U, end + 1U - (at + 13U));
    }
    if (const auto at = text.find("\"aot_policy\":"); at != std::string::npos) {  // the last member of the report: the object up to the report's closing brace
      const auto begin = at + 13U, end = text.rfind('}');
      if (end != std::string::npos && end > begin) aot_policy_json_ = text.substr(begin, end - begin);
    }
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
    if (const auto f = prepare_z80()) return f;
    stage("compile", "done");
    stage("link", "running");
    std::vector<fs::path> all = objects;
    all.insert(all.end(), rt.begin(), rt.end());
    all.insert(all.end(), z80_objects_.begin(), z80_objects_.end());
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
    all.insert(all.end(), z80_objects_.begin(), z80_objects_.end());
    all.insert(all.end(), viewer_objects.begin(), viewer_objects.end());
    if (run_process(link_command(all, viewer, !host), o_.output / "link-viewer.log", {}, 600) != 0) return Failure{"viewer", "linking the viewer failed: " + read_text(o_.output / "link-viewer.log", 2048)};
    return std::nullopt;
  }

  // ---- SEG-114 (ADR 0258): the main-side Z80 images ----
  // The Z80 images are the image epochs of the Genesis machine (ADR 0073): the 8 KiB sound-RAM snapshot at a runnable transition after an upload, selected at run time by the
  // activation signature of the 68000-written extents. A build pass records every epoch it cannot activate (its image is not compiled yet) and goes on; the images are emitted
  // by the existing broad Z80 AOT into per-round units and the program is rebuilt. A candidate is confirmed only when its pass recorded no unknown epoch.
  std::optional<Failure> prepare_z80() {
    z80_objects_.clear();
    if (!o_.main_z80) return std::nullopt;
    std::error_code ec;
    const fs::path dir = o_.output / "generated-z80";
    fs::remove_all(dir, ec);
    gz80::EmitRequest request;
    request.directory = dir;
    request.structural_alternatives = o_.z80_alternatives;  // SEG-115 (ADR 0259): the statically justified alternative forms of the driver code
    const gz80::EmitOutcome emitted = gz80::emit_registry(z80_registry_, request);
    if (!emitted.ok()) return Failure{"z80", "the Z80 image code could not be generated"};
    z80_alternatives_ = emitted.alternatives;
    z80_alt_stats_ = {emitted.stats.alternative_entries, emitted.stats.alternative_forms, emitted.stats.alternative_images_dropped};
    std::vector<fs::path> sources;
    {
      std::ifstream list(dir / "genesis_z80.units");
      for (std::string line; std::getline(list, line);)
        if (!line.empty()) sources.push_back(dir / line);
    }
    unsigned rebuilt = 0;
    if (const auto f = objects_for(sources, {}, false, z80_objects_, rebuilt)) return f;
    return std::nullopt;
  }

  // Reads the unknown epochs a pass wrote (z80_epochs.txt and the numbered snapshot files) and registers their images. `added` = images new to the registry.
  std::optional<Failure> ingest_z80(const fs::path &dir, unsigned &added) {
    added = 0;
    if (!o_.main_z80) return std::nullopt;
    const std::string text = read_text(dir / "z80_epochs.txt", 4096);
    if (text.empty()) return std::nullopt;
    const auto field = [&](const char *name) -> std::optional<unsigned> {
      const auto at = text.find(std::string(name) + " ");
      if (at == std::string::npos) return std::nullopt;
      return static_cast<unsigned>(std::strtoul(text.c_str() + at + std::string(name).size() + 1U, nullptr, 10));
    };
    const unsigned unknown = field("unknown").value_or(0U);
    if (field("overflow").value_or(0U) != 0U || unknown > gz80::kMaxImages) return Failure{"pass", "z80_epoch_bound_exceeded"};
    for (unsigned k = 0; k < unknown; ++k) {
      const std::string ram = read_text(dir / ("z80_epoch_ram_" + std::to_string(k)), gz80::kRamBytes + 1U);
      const std::string written = read_text(dir / ("z80_epoch_written_" + std::to_string(k)), gz80::kBitmapBytes + 1U);
      if (ram.size() != gz80::kRamBytes || written.size() != gz80::kBitmapBytes) return Failure{"pass", "z80_epoch_unreadable"};
      gz80::Epoch epoch;
      std::copy(ram.begin(), ram.end(), epoch.ram.begin());
      std::copy(written.begin(), written.end(), epoch.written.begin());
      std::uint32_t bound = 0;
      const gz80::AddOutcome outcome = z80_registry_.add(epoch, bound);
      if (outcome == gz80::AddOutcome::bound_exceeded) return Failure{"pass", "z80_image_bound_exceeded"};
      if (outcome == gz80::AddOutcome::added) {
        ++added;
      }
    }
    if (unknown != 0U && added == 0U) return Failure{"pass", "z80_no_progress"};   // epochs the registry already explains cannot be unknown to a program built with it
    return std::nullopt;
  }

  std::string z80_json() const {
    if (!o_.main_z80) return "{\"enabled\":false}";
    std::string digests;
    for (const auto &image : z80_registry_.images()) digests += gz80::to_hex(image.content);
    digests += gz80::to_hex(z80_alternatives_.identity);  // the alternative forms are part of the generated code (ADR 0259)
    return "{\"enabled\":true,\"audio\":" + std::string(o_.z80_audio ? "true" : "false") + ",\"images\":" + std::to_string(z80_registry_.images().size()) + ",\"registry_identity\":\"" + sha_of(digests) +
           "\",\"alternatives\":{\"images\":" + std::to_string(z80_alternatives_.images_with) + ",\"bytes\":" + std::to_string(z80_alternatives_.bytes) +
           ",\"images_over_bound\":" + std::to_string(z80_alternatives_.images_over_bound) + ",\"entries\":" + std::to_string(z80_alt_stats_[0]) +
           ",\"forms\":" + std::to_string(z80_alt_stats_[1]) + ",\"images_dropped\":" + std::to_string(z80_alt_stats_[2]) + ",\"complete\":false}}";
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

  // ---- static module admission (SEG-098-T002, ADR 0211) ----
  // The static loader scan (disc + admitted boot images, module-jump-table roots; executes nothing) proposes the extents of disc modules no pass has
  // loaded. They enter the chain as `static-extent:` admissions; the emitter re-reads every one from the verified disc, and the per-instruction live-byte
  // guard remains the runtime authority. Any failure leaves the build exactly as it was without this step and is reported, never hidden.
  void static_discovery(std::vector<std::string> &admits) {
    static_json_ = "{\"ran\":false}";
    static_status_ = "scan_failed";
    static_refusals_.clear();
    const fs::path list = o_.output / "static-extents.txt", report = o_.output / "static-scan.json";
    std::error_code ec;
    fs::remove(list, ec);
    fs::remove(o_.output / "static-windows.txt", ec);
    std::vector<std::string> scan_args{self_, "sega-cd", "discover-modules", "--cue", o_.cue.string(), "--module-table-roots", "--constant-dispatch", "--work-state"};
    if (o_.static_recursion) scan_args.push_back("--recursive-modules");
    for (const char *a : {"--state-convergence", "--static-extents"}) scan_args.push_back(a);
    for (const auto &a : {list.string(), std::string("--static-scope"), o_.static_range, std::string("--static-windows"), (o_.output / "static-windows.txt").string()}) scan_args.push_back(a);
    const int code = run_process(scan_args, report, {}, 300);
    const std::string text = read_text(report);
    if (code < 0 || text.empty() || text.find("\"complete\":") == std::string::npos) { static_json_ = "{\"ran\":true,\"status\":\"scan_failed\"}"; return; }
    const bool complete = raw_value(text, "complete").value_or("") == "true";
    static_joined_ = raw_value(text, "convergence_used").value_or("") == "true";
    std::map<std::string, unsigned> reasons;
    for (std::size_t at = text.find("\"reason\":\""); at != std::string::npos; at = text.find("\"reason\":\"", at + 1U)) {
      const auto end = text.find('"', at + 10U);
      if (end != std::string::npos) ++reasons[text.substr(at + 10U, end - at - 10U)];
    }
    std::vector<std::string> found;
    static_windows_.clear();
    {
      std::ifstream in(list);
      for (std::string line; std::getline(in, line);) if (!line.empty()) found.push_back(line);
    }
    {
      std::ifstream in(o_.output / "static-windows.txt");
      for (std::string line; std::getline(in, line);) if (!line.empty()) static_windows_.push_back(line);
    }
    std::string status = !complete ? "scan_incomplete" : found.empty() ? "no_modules" : "admitted";
    if (found.size() > kMaxStaticExtents) { status = "static_extent_bound_exceeded"; found.clear(); }
    for (const auto &e : found) { static_extents_.push_back(e); admits.push_back("static-extent:" + e); }
    std::ostringstream out;
    out << "{\"ran\":true,\"status\":\"" << status << "\",\"complete\":" << (complete ? "true" : "false") << ",\"extents\":" << static_extents_.size() << ",\"refusals\":{";
    bool first = true;
    for (const auto &[k, n] : reasons) { out << (first ? "" : ",") << "\"" << k << "\":" << n; first = false; }
    out << "}}";
    static_json_ = out.str();
    static_status_ = status;
    static_refusals_ = reasons;
    log_ << "static discovery: " << status << ", " << static_extents_.size() << " extent(s)" << std::endl;
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

  // ---- SEG-112 (ADR 0256): executable images proven at generation time from the boot images and MC68000 semantics ----
  void static_producers(std::vector<std::string> &admits) {
    const fs::path out = o_.output / "producers.json";
    std::error_code rm_ec;
    fs::remove(out, rm_ec);
    if (run_process({self_, "sega-cd", "discover-producers", "--cue", o_.cue.string()}, out, {}, 600) < 0) return;
    const std::string text = read_text(out);
    if (raw_value(text, "ok").value_or("") != "true") return;
    producer_candidates_ = static_cast<unsigned>(number_value(text, "candidates").value_or(0));
    producer_unknown_writes_ = static_cast<unsigned>(number_value(text, "unknown_writes").value_or(0));
    const auto refusals = text.find("\"refusals\":{");
    if (refusals != std::string::npos) producer_refusals_ = text.substr(refusals + 11U, text.find('}', refusals) - refusals - 10U);
    const auto list = text.find("\"admits\":[");
    if (list == std::string::npos) return;
    for (std::size_t p = list + 10U; p < text.size() && text[p] != ']';) {
      if (text[p] == '"') {
        const auto end = text.find('"', p + 1U);
        if (end == std::string::npos) break;
        admits.push_back(text.substr(p + 1U, end - p - 1U));
        ++static_producer_count_;
        p = end + 1U;
      } else {
        ++p;
      }
    }
  }

  // ---- SEG-113 (ADR 0257): the BIOS boot images (main boot area, sub boot program) established from the BIOS profile and the disc before any code is generated ----
  // `boot-contract` offers an image only for a verified BIOS profile and header form (a pure function of the BIOS and the disc; nothing runs); the emitter recomputes every
  // `static-boot:` admission, and the per-instruction live-byte guard still decides what executes. No offer = the boot images are discovered by execution as before.
  void static_boot(std::vector<std::string> &admits) {
    boot_state_ = "not_offered";
    const fs::path out = o_.output / "boot-contract.json";
    std::error_code rm_ec;
    fs::remove(out, rm_ec);
    std::vector<std::string> contract_cmd{self_, "sega-cd", "boot-contract", "--cue", o_.cue.string(), "--bios", o_.bios.string()};
    if (o_.boot_fixtures) contract_cmd.insert(contract_cmd.end(), {"--boot-fixture-profiles", "1"});
    if (run_process(contract_cmd, out, {}, 300) < 0) { boot_state_ = "tool_failed"; return; }
    const std::string text = read_text(out);
    if (raw_value(text, "ok").value_or("") != "true") { boot_state_ = "tool_failed"; return; }
    boot_profile_ = raw_value(text, "profile").value_or("");
    boot_authority_ = raw_value(text, "authority").value_or("");
    boot_refusal_ = raw_value(text, "refusal").value_or("unknown");
    const auto images = text.find("\"images\":[");
    const auto list = text.find("\"admits\":[");
    if (images == std::string::npos || list == std::string::npos) { boot_state_ = "tool_failed"; return; }
    for (std::size_t p = images; p < list;) {   // {"cpu":"main","class":"conditional","offset":N,"base":N,"length":N}
      const auto at = text.find("{\"cpu\":", p);
      if (at == std::string::npos || at >= list) break;
      const auto end = text.find('}', at);
      const std::string one = text.substr(at, end - at + 1U);
      boot_extents_.push_back(raw_value(one, "cpu").value_or("") + ":" + raw_value(one, "offset").value_or("0") + ":" + raw_value(one, "base").value_or("0") + ":" + raw_value(one, "length").value_or("0"));
      boot_classes_.push_back(raw_value(one, "cpu").value_or("") + ":" + raw_value(one, "class").value_or(""));
      p = end + 1U;
    }
    for (std::size_t p = list + 10U; p < text.size() && text[p] != ']';) {
      if (text[p] == '"') {
        const auto end = text.find('"', p + 1U);
        if (end == std::string::npos) break;
        admits.push_back(text.substr(p + 1U, end - p - 1U));
        p = end + 1U;
      } else {
        ++p;
      }
    }
    boot_state_ = boot_extents_.empty() ? "not_offered" : "admitted";
    log_ << "static boot: " << boot_state_ << " (" << boot_profile_ << ", " << boot_refusal_ << "), " << boot_extents_.size() << " image(s)" << std::endl;
  }

  // What an execution-observed extent says about a statically admitted boot image (same CPU, destination ranges overlapping, even partially):
  //  - source in the boot area, outside this image's own source span, mapped over the image's destination: **contradiction** - the exact-copy resolver found the live bytes in
  //    boot-area bytes the contract does not place there;
  //  - source wholly inside this image's own source span but at a different source-to-destination offset: **internal source** (a copy of the image's own bytes elsewhere in its
  //    span, or a shifted placement - undecidable without write tracing); not a contradiction;
  //  - source elsewhere on the disc: a **later overlay** (not a statement about the boot mapping);
  //  - destination not overlapping: unrelated.
  // A live mismatch for which the resolver found no source at all is neither: it is counted as an *unresolved mismatch* and never as a BIOS disagreement.
  // Limited observation: without write tracing a boot area that copies parts of itself over its own span is indistinguishable from a differing placement.
  void note_boot_extent(const std::string &spec) {
    const auto f = split_spec(spec);
    if (f.size() != 4U) return;
    std::uint64_t offset = 0, base = 0, length = 0;
    try { offset = std::stoull(f[1]); base = std::stoull(f[2]); length = std::stoull(f[3]); } catch (const std::exception &) { return; }
    for (const auto &b : boot_extents_) {
      const auto g = split_spec(b);
      if (g.size() != 4U || g[0] != f[0]) continue;
      std::uint64_t goff = 0, gbase = 0, glen = 0;
      try { goff = std::stoull(g[1]); gbase = std::stoull(g[2]); glen = std::stoull(g[3]); } catch (const std::exception &) { return; }
      if (!(base < gbase + glen && gbase < base + length)) continue;   // destinations do not overlap
      const std::uint64_t boot_area_end = std::max<std::uint64_t>(goff + glen, kSystemAreaBytes);
      const bool from_boot_area = offset < boot_area_end;
      const bool same_mapping = static_cast<std::int64_t>(offset) - static_cast<std::int64_t>(base) == static_cast<std::int64_t>(goff) - static_cast<std::int64_t>(gbase);
      const bool inside_own_source = offset >= goff && offset + length <= goff + glen;   // bytes the contract already places in this image: a copy of them elsewhere is a relocation or a shifted placement, which this observation cannot tell apart
      if (from_boot_area && !same_mapping && inside_own_source) ++boot_internal_;
      else if (from_boot_area && !same_mapping) ++boot_contradictions_;
      else if (!from_boot_area) ++boot_overlays_;
    }
  }

  // A frontier at a pc inside a static boot image's destination for which the resolver found no source relationship at all.
  void note_boot_unresolved(const std::string &cpu, std::uint64_t pc) {
    for (const auto &b : boot_extents_) {
      const auto g = split_spec(b);
      if (g.size() != 4U || g[0] != cpu) continue;
      try { if (pc >= std::stoull(g[2]) && pc < std::stoull(g[2]) + std::stoull(g[3])) { ++boot_unresolved_; return; } } catch (const std::exception &) { return; }
    }
  }

  static std::vector<std::string> split_spec(const std::string &text) {
    std::vector<std::string> f;
    std::stringstream ss(text);
    for (std::string part; std::getline(ss, part, ':');) f.push_back(part);
    return f;
  }

  std::string static_boot_json() const {
    std::ostringstream out;
    out << "{\"enabled\":" << (o_.static_boot ? "true" : "false") << ",\"state\":\"" << (o_.static_boot ? boot_state_ : "disabled") << "\",\"authority\":\"" << boot_authority_ << "\",\"profile\":\"" << boot_profile_
        << "\",\"refusal\":\"" << boot_refusal_ << "\",\"images\":[";
    for (std::size_t i = 0; i < boot_classes_.size(); ++i) {
      const auto colon = boot_classes_[i].find(':');
      out << (i ? "," : "") << "{\"cpu\":\"" << boot_classes_[i].substr(0, colon) << "\",\"class\":\"" << boot_classes_[i].substr(colon + 1U) << "\"}";
    }
    out << "],\"contradictions\":" << boot_contradictions_ << ",\"internal_sources\":" << boot_internal_ << ",\"later_overlays\":" << boot_overlays_ << ",\"unresolved_mismatches\":" << boot_unresolved_ << "}";
    return out.str();
  }

  // ---- SEG-111 (ADR 0255): executable images made by verified guest code ----
  struct Relocation {
    std::optional<std::string> extent;     // "<cpu>:<offset>:<base>:<length>" (an exact copy of admitted disc bytes)
    std::optional<std::string> produced;   // "<cpu>:<base>:<snapshot path>" (bytes a verified producer computed)
    std::string produced_key, error;
  };

  // The admitted disc extents a copy's source may lie in: discovered, static and the testing hook's manual ones.
  std::vector<std::string> source_extents() const {
    std::vector<std::string> out = extents_;
    out.insert(out.end(), static_extents_.begin(), static_extents_.end());
    out.insert(out.end(), boot_extents_.begin(), boot_extents_.end());
    for (std::size_t i = 0; i + 1U < o_.emit_args.size(); ++i) {
      if (o_.emit_args[i] != "--disc-image") continue;
      out.push_back(o_.emit_args[i + 1U]);
    }
    return out;
  }

  Relocation relocate(const std::string &cpu, const std::string &program_identity, std::size_t have) {
    Relocation out;
    if (have >= kMaxExtents) { out.error = "extent_bound_exceeded"; return out; }
    const fs::path snapshot = o_.output / ("produced" + std::to_string(produced_count_) + ".bin");
    std::vector<std::string> cmd{self_, "sega-cd", "derive-relocation", "--cue", o_.cue.string(), "--capture", (o_.output / "pass" / "capture.json").string(), "--region",
                                 (o_.output / "pass" / "region.bin").string(), "--pass", program_identity, "--snapshot", snapshot.string()};
    for (const auto &e : source_extents()) { cmd.push_back("--extent"); cmd.push_back(e); }
    const fs::path result = o_.output / "relocate.json";
    std::error_code rm_ec;
    fs::remove(result, rm_ec);   // never read an earlier round's answer
    fs::remove(snapshot, rm_ec);
    if (run_process(cmd, result, {}, 600) < 0) { out.error = "relocation_failed"; return out; }
    const std::string text = read_text(result);
    if (raw_value(text, "ok").value_or("") != "true") { out.error = raw_value(text, "error").value_or("relocation_failed"); return out; }
    const std::string base = raw_value(text, "base").value_or("0"), length = raw_value(text, "length").value_or("0");
    if (raw_value(text, "class").value_or("") == "copy") {
      const std::string spec = cpu + ":" + raw_value(text, "offset").value_or("0") + ":" + base + ":" + length;
      const auto known = source_extents();
      if (std::find(known.begin(), known.end(), spec) != known.end()) { out.error = "no_progress"; return out; }   // the same extent proposed again explains nothing new
      out.extent = spec;
      return out;
    }
    if (produced_.size() >= kMaxProduced) { out.error = "produced_bound_exceeded"; return out; }
    const std::string bytes = read_text(snapshot, 4096U);
    std::string key = cpu + ":" + base + ":";
    for (const char ch : bytes) { static const char *const digits = "0123456789abcdef"; const auto c = static_cast<unsigned char>(ch); key.push_back(digits[c >> 4U]); key.push_back(digits[c & 15U]); }
    if (bytes.empty() || std::find(produced_.begin(), produced_.end(), key) != produced_.end()) { out.error = "no_progress"; return out; }
    out.produced = cpu + ":" + base + ":" + snapshot.string();
    out.produced_key = key;
    return out;
  }

  // Counts and classes only (never an address, offset or byte).
  std::string reconstruction_json() const {
    std::string out = "{\"static_producer_candidates\":" + std::to_string(producer_candidates_) + ",\"static_fragments\":" + std::to_string(static_producer_count_) + ",\"static_producer_unknown_writes\":" + std::to_string(producer_unknown_writes_) + ",\"static_producer_refusals\":" + producer_refusals_ + ",\"relocated_extents\":" + std::to_string(relocated_count_) + ",\"produced_images\":" + std::to_string(produced_count_) + ",\"refusals\":{";
    bool first = true;
    for (const auto &[error, n] : relocation_refusals_) {
      out += std::string(first ? "" : ",") + "\"" + error + "\":" + std::to_string(n);
      first = false;
    }
    return out + "}}";
  }

  Options o_;
  std::string self_, bios_sha_, key_base_, history_, frontier_;
  std::ofstream log_;
  std::mutex log_mutex_;
  std::atomic<unsigned> counter_{0};
  std::vector<std::string> extents_, static_extents_, static_windows_;
  std::vector<std::string> produced_;   // SEG-111: the bytes of every produced image admitted so far (hex, keyed by processor and base)
  unsigned relocated_count_ = 0, produced_count_ = 0, static_producer_count_ = 0, producer_candidates_ = 0, producer_unknown_writes_ = 0;
  std::string producer_refusals_ = "{}";
  std::map<std::string, unsigned> relocation_refusals_;
  std::vector<std::string> boot_extents_, boot_classes_;   // SEG-113: "<cpu>:<offset>:<base>:<length>" / "<cpu>:<class>" of the statically admitted boot images
  std::string boot_state_ = "not_run", boot_profile_, boot_refusal_ = "none";
  std::string boot_authority_;
  unsigned boot_contradictions_ = 0, boot_internal_ = 0, boot_overlays_ = 0, boot_unresolved_ = 0;
  std::string static_status_ = "not_run";
  bool static_joined_ = false;
  std::map<std::string, unsigned> static_refusals_;
  std::string static_json_ = "{\"ran\":false}";
  std::vector<std::string> unit_names_;
  std::string file_first_json_;   // SEG-100: the last emission's file_first object (status.json), or empty
  std::string aot_policy_json_;   // SEG-097-T003: the last emission's machine-readable aot_policy object (status.json)
  std::vector<fs::path> unit_sources_, unit_objects_, runtime_objects_;
  gz80::Registry z80_registry_;           // SEG-114: the Z80 images discovered so far (first-activation order)
  gz80::AlternativesSummary z80_alternatives_;  // SEG-115 (ADR 0259): the alternative forms of the last emission
  std::array<std::size_t, 3> z80_alt_stats_{};  // entries with alternatives, forms added, images dropped by the per-entry bound
  std::vector<fs::path> z80_objects_;     // SEG-114: the generated image units of the current round
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
    else if (key == "--static-discovery") o.static_discovery = value != "0";
    else if (key == "--static-recursion" && (value == "0" || value == "1")) o.static_recursion = value == "1";
    else if (key == "--static-range" && (value == "hull" || value == "loaded")) o.static_range = value;
    else if (key == "--cc") o.cc = value;
    else if (key == "--cc-arg") o.cc_args.push_back(value);
    else if (key == "--cxx") o.cxx = value;   // SEG-115: the C++ driver for the vendored YM2612 core (derived from --cc when absent)
    else if (key == "--cxx-arg") { o.cxx_args.push_back(value); o.cxx_args_given = true; }
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
    else if (key == "--aot-policy" && (value == "compatibility" || value == "optimized")) o.aot_policy = value;
    else if (key == "--aot-model" && value == "gen2" && !o.aot_model_given) o.aot_model_given = true;
    else if (key == "--file-first" && (value == "all" || value == "structural")) o.file_first = value;   // SEG-100 (ADR 0221)
    else if (key == "--emit-arg") o.emit_args.push_back(value);   // tests only
    else if (key == "--boot-fixture-profiles" && (value == "0" || value == "1")) o.boot_fixtures = value == "1";
    else if (key == "--static-boot" && (value == "0" || value == "1")) o.static_boot = value == "1";
    else if (key == "--static-producers" && (value == "0" || value == "1")) o.static_producers = value == "1";
    else if (key == "--z80-alternatives" && (value == "0" || value == "1")) o.z80_alternatives = value == "1";   // SEG-115 (ADR 0259)
    else if (key == "--z80-audio" && (value == "0" || value == "1")) o.z80_audio = value == "1";   // SEG-115 (ADR 0259)
    else if (key == "--main-z80" && (value == "0" || value == "1")) o.main_z80 = value == "1";   // SEG-114 (ADR 0258)
    else if (key == "--reconstruction" && (value == "0" || value == "1")) o.reconstruction = value == "1";
    else if (key == "--keep-passes" && (value == "0" || value == "1")) o.keep_passes = value == "1";   // SEG-111: diagnostics/tests
    else if (key == "--file-first-max-bytes") o.file_first_max_bytes = value;
    else if (key == "--file-first-max-offsets") o.file_first_max_offsets = value;
    else if (key == "--aot-scope" && value == "all-images") o.aot_all_images = true;   // historical M1 scope, evidence only
    else {
      std::cerr << "usage:\n" << segarecomp_sega_cd_usage;
      return 2;
    }
  }
  if (!have_cue || !have_bios || !have_output || !have_runtime || o.cc.empty() || o.budget == 0 || (argc - 3) % 2 != 0 ||
      ((o.aot_model_given || o.aot_all_images) && o.aot_policy != "optimized") || (!o.file_first.empty() && o.aot_policy == "optimized")) {
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
