#include "launcher_core.hpp"

#include <cctype>

#include "segarecomp/rom.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

#ifndef SEGARECOMP_LAUNCHER_VERSION
#define SEGARECOMP_LAUNCHER_VERSION "dev"
#endif
#ifndef SEGARECOMP_ZIG_VERSION
#define SEGARECOMP_ZIG_VERSION "unknown"
#endif
// Linux: the glibc the generated program links against. Release builds keep 2.35 (the bundled SDL3 is built on Ubuntu 22.04); a developer
// who built SDL3 on a newer distribution passes -DSEGARECOMP_ZIG_GLIBC=<version> so a locally staged package links and runs.
#ifndef SEGARECOMP_ZIG_GLIBC
#define SEGARECOMP_ZIG_GLIBC "2.35"
#endif

namespace launcher {
namespace {

#if defined(_WIN32)
constexpr const char *exe_suffix = ".exe";
#else
constexpr const char *exe_suffix = "";
#endif

// Pinned zig target for the host (explicit: zig's native-target detection lags new OS releases).
const char *zig_target() {
#if defined(__APPLE__) && defined(__aarch64__)
  return "aarch64-macos";
#elif defined(__APPLE__)
  return "x86_64-macos";
#elif defined(_WIN32)
  return "x86_64-windows-gnu";
// Linux: the bundled libSDL3.so is built on Ubuntu 22.04 (glibc 2.35) and references symbols up to GLIBC_2.34, so
// the generated program must be linked against at least that glibc (release builds require glibc >= 2.35).
#elif defined(__aarch64__)
  return "aarch64-linux-gnu." SEGARECOMP_ZIG_GLIBC;
#else
  return "x86_64-linux-gnu." SEGARECOMP_ZIG_GLIBC;
#endif
}

const char *host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#else
  return "x86_64";
#endif
}

fs::path path_from_utf8(const char *text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text)));
}

std::string utf8(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

std::string json_safe(std::string text) {
  for (auto &c : text)
    if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20U) c = ' ';
  return text;
}

std::string read_all(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

} // namespace

std::string host_description() {
  return std::string(host_arch()) + " " + SDL_GetPlatform();
}

Layout locate_layout() {
  Layout layout;
  if (const char *home = SDL_getenv("SEGARECOMP_HOME")) {
    layout.root = path_from_utf8(home);
  } else {
    const char *base = SDL_GetBasePath();
    const fs::path exe_dir = base ? path_from_utf8(base) : fs::path{};
    // macOS .app: Contents/MacOS/Segarecomp with everything else in Contents/Resources.
    const fs::path bundle_resources = exe_dir.parent_path() / "Resources";
    layout.root = fs::is_directory(bundle_resources / "toolchain") ? bundle_resources : exe_dir;
  }
  layout.cli = layout.root / "bin" / (std::string("segarecomp") + exe_suffix);
  layout.cc = layout.root / "toolchain" / (std::string("zig") + exe_suffix);
  layout.runtime = layout.root / "runtime" / "platforms" / "genesis";
  layout.runtime_sms = layout.root / "runtime" / "platforms" / "master-system";
  layout.sdl_include = layout.root / "sdl3" / "include";
  layout.sdl_lib = layout.root / "sdl3" / "lib";
  std::error_code ec;
  for (const auto &required : {layout.cli, layout.cc, layout.runtime / "runtime" / "runtime.c",
                               layout.runtime / "viewer" / "viewer_sdl3.c",
                               layout.runtime_sms / "viewer" / "sms_viewer_sdl3.c", layout.root / "runtime" / "libs" / "viewer" / "src" / "sv_host.c",
                               layout.sdl_include / "SDL3" / "SDL.h"}) {
    if (!fs::exists(required, ec)) { layout.problem = "This Segarecomp installation is incomplete (missing " + utf8(required.filename()) + "). Please download it again."; break; }
  }
  return layout;
}

fs::path cache_root() {
  if (const char *override_dir = SDL_getenv("SEGARECOMP_CACHE_DIR")) return path_from_utf8(override_dir);
#if defined(_WIN32)
  if (const char *local = SDL_getenv("LOCALAPPDATA")) return path_from_utf8(local) / "Segarecomp";
#endif
  char *pref = SDL_GetPrefPath(nullptr, "Segarecomp");  // macOS ~/Library/Application Support/Segarecomp/, Linux ~/.local/share/Segarecomp/
  fs::path result = pref ? path_from_utf8(pref) : fs::temp_directory_path() / "Segarecomp";
  SDL_free(pref);
  return result;
}

fs::path library_root() {
  if (const char *override_dir = SDL_getenv("SEGARECOMP_LIBRARY_DIR")) return path_from_utf8(override_dir);
  return cache_root() / "library";
}

std::string clean_title(const std::string &raw) {
  // Header titles are fixed-width, space padded and may carry control bytes or non-ASCII garbage: keep printable ASCII only
  // (anything else acts as a separator), collapse every whitespace run to one space and trim.
  std::string out;
  bool pending_space = false;
  for (const char raw_char : raw) {
    const auto c = static_cast<unsigned char>(raw_char);
    if (c > 0x20 && c < 0x7F) {
      if (pending_space && !out.empty()) out.push_back(' ');
      pending_space = false;
      out.push_back(static_cast<char>(c));
    } else {
      pending_space = true;
    }
  }
  return out;
}

namespace {
// One header title field as readable ASCII, or nullopt when it holds anything that cannot be shown faithfully. Besides ASCII this
// understands the Shift-JIS full-width Latin letters, digits and space some Japanese cartridges use ("\x82`" = 'A'); katakana and every
// other multi-byte code is not representable by the launcher's Latin font and is reported as unreadable rather than shown as escapes.
std::optional<std::string> decode_header_title(const std::vector<std::uint8_t> &bytes, std::size_t offset, std::size_t length) {
  if (bytes.size() < offset + length) return std::nullopt;
  std::string out;
  for (std::size_t i = 0; i < length; ++i) {
    const unsigned b = bytes[offset + i];
    if (b < 0x80U) { out.push_back(b >= 0x20U && b < 0x7FU ? static_cast<char>(b) : ' '); continue; }
    if (i + 1 >= length) return std::nullopt;
    const unsigned t = bytes[offset + ++i];
    if (b == 0x81U && t == 0x40U) out.push_back(' ');
    else if (b == 0x82U && t >= 0x4FU && t <= 0x58U) out.push_back(static_cast<char>('0' + (t - 0x4FU)));
    else if (b == 0x82U && t >= 0x60U && t <= 0x79U) out.push_back(static_cast<char>('A' + (t - 0x60U)));
    else if (b == 0x82U && t >= 0x81U && t <= 0x9AU) out.push_back(static_cast<char>('a' + (t - 0x81U)));
    else return std::nullopt;
  }
  return out;
}

std::string strip_tags(const std::string &name) {  // "Game (JUE) [!]" -> "Game"
  std::string out;
  int depth = 0;
  for (const char c : name) {
    if (c == '(' || c == '[') ++depth;
    else if ((c == ')' || c == ']') && depth > 0) --depth;
    else if (depth == 0) out.push_back(c);
  }
  return out;
}
}  // namespace

std::string genesis_header_title(const std::vector<std::uint8_t> &bytes, bool domestic_first, const std::string &file_stem) {
  const auto domestic = decode_header_title(bytes, 0x120, 48);
  const auto overseas = decode_header_title(bytes, 0x150, 48);
  for (const auto *candidate : domestic_first ? std::array{&domestic, &overseas} : std::array{&overseas, &domestic}) {
    if (!candidate->has_value()) continue;
    if (const std::string title = clean_title(**candidate); !title.empty()) return title;
  }
  if (const std::string stem = clean_title(strip_tags(file_stem)); !stem.empty()) return stem;
  return clean_title(file_stem);
}

RegionHint region_hint_from_header_field(const std::string &raw) {
  const segarecomp::GenesisRegionHint hint = segarecomp::genesis_region_hint(raw);  // the single shared policy (libs/media)
  return RegionHint{hint.profile, hint.ambiguous};
}

// Every profile executes; Europe PAL runs on the 60 Hz schedule (50 Hz timing is not implemented), see region_note.
bool region_executable(const std::string &region) { return region == "usa" || region == "japan" || region == "europe"; }

std::string region_note(const std::string &region) {
  return region == "europe" ? "PAL enabled, but 60 Hz is forced: 50 Hz is not supported." : std::string();
}

std::string launch_refusal(const RomView &rom) {
  if (rom.supports_region() && !region_executable(rom.region)) return "Unknown console region: choose USA, Japan or Europe.";
  return {};
}

void apply_game_environment(SDL_Environment *environment, const RomView &rom) {
  // Always written for Genesis (also for USA) so an inherited SEGARECOMP_GENESIS_REGION can never override the visible selection.
  if (rom.supports_region()) SDL_SetEnvironmentVariable(environment, "SEGARECOMP_GENESIS_REGION", rom.region.c_str(), true);
}

RomView inspect_rom_file(const fs::path &path, const Layout &layout) {
  RomView view;
  view.path = path;
  try {
    const auto bytes = segarecomp::read_binary(path);
    view.sha256 = segarecomp::sha256_hex(bytes);
    const auto info = segarecomp::inspect_rom(bytes);
    const bool recognized = info.outcome == segarecomp::ClassificationOutcome::recognized;
    if (recognized && info.platform == segarecomp::Platform::master_system) {
      view.supported_platform = true;
      view.platform_id = "master-system";
      view.platform = "Master System";
      view.title = clean_title(utf8(path.stem()));
      const fs::path sidecar = fs::path(path).replace_extension(".mapper.json");
      std::error_code manifest_ec;
      if (fs::is_regular_file(sidecar, manifest_ec)) {
        const std::string text = read_all(sidecar);
        view.mapper_manifest = sidecar;
        view.mapper_manifest_sha256 = segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
      }
      return view;
    }
    view.supported_platform = info.platform == segarecomp::Platform::genesis;
    view.platform_id = view.supported_platform ? "genesis" : "";
    view.platform = view.supported_platform ? "Genesis / Mega Drive" : "Unrecognized file";
    view.title = clean_title(utf8(path.stem()));
    std::error_code ec;
    if (view.supported_platform && bytes.size() >= 0x200) {
      const RegionHint hint = region_hint_from_header_field(std::string(reinterpret_cast<const char *>(&bytes[0x1F0]), 16));
      view.region = hint.region;
      view.region_ambiguous = hint.ambiguous;
      // A Japan-only cartridge shows its domestic name first; every other one the international (overseas) name.
      view.title = genesis_header_title(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), hint.region == "japan", utf8(path.stem()));
    }
    view.compat_known = fs::is_regular_file(layout.runtime / "compat" / (view.sha256 + ".json"), ec);
  } catch (const std::exception &error) {
    view.error = error.what();
  }
  return view;
}

// Genesis keeps its historical key segment; Master System adds the platform, profile and declared mapper (and the sidecar
// manifest's content digest), so a different declaration or manifest always selects a fresh cache entry.
std::string platform_key(const RomView &rom, const Layout *layout_for_key) {
  // Compatibility keeps the historical Genesis key; Optimized adds the policy, so the two never share a cache entry (ADR 0096).
  if (rom.platform_id != "master-system") {
    const std::string discovery = rom.hint_alias_discovery ? ":hint-alias-discovery=1" : "";  // changes the generated program
    if (!rom.optimized()) return "genesis" + discovery;
    // The model family and the product-provided policy identity (which binds model, schema, token contract, rank policy and analysis contract) are part
    // of the key: a different request or a changed shipped model can never select the same entry.
    return "genesis:aot=optimized:model=" + rom.aot_model + ":pid=" + (layout_for_key != nullptr ? product_policy_identity(*layout_for_key, "optimized", rom.aot_model) : std::string()) + discovery;
  }
  // SEG-057-T002: an Optimized Master System build never shares an entry with a Compatibility one, and the product-provided policy identity (which
  // binds the model, rank policy, schema and admission contract) keys it, so a changed shipped model selects a fresh entry.
  const std::string base = std::string("master-system:") + sms_profile + ":mapper=" + rom.mapper + ":manifest=" + rom.mapper_manifest_sha256;
  if (!rom.optimized()) return base;
  return base + ":aot=optimized:pid=" + (layout_for_key != nullptr ? rom_policy_identity(*layout_for_key, rom) : std::string());
}

fs::path entry_dir(const RomView &rom, const Layout &layout) {
  // Everything that changes the produced binary is part of the key; a mismatch selects a fresh directory.
  // The build driver itself (which embeds the code generator) is part of the key: a different segarecomp
  // build must never reuse a program generated by another one, even at the same version string.
  std::error_code ec;
  const auto cli_size = fs::is_regular_file(layout.cli, ec) ? fs::file_size(layout.cli, ec) : 0U;
  const auto cli_time = fs::last_write_time(layout.cli, ec).time_since_epoch().count();
  const std::string key = rom.sha256 + "|cli-" + std::to_string(static_cast<unsigned long long>(cli_size)) + "-" + std::to_string(static_cast<long long>(cli_time)) + "|" SEGARECOMP_LAUNCHER_VERSION "|" + platform_key(rom, &layout) + "|" + host_arch() + "|" +
                          SDL_GetPlatform() + "|zig-" SEGARECOMP_ZIG_VERSION "|O2|" + utf8(layout.root);
  const std::string digest = segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(key.data()), key.size()});
  return cache_root() / "games" / digest.substr(0, 32);
}

fs::path entry_executable(const fs::path &entry) { return entry / (std::string("game") + exe_suffix); }

bool entry_ready(const fs::path &entry) {
  std::error_code ec;
  return fs::is_regular_file(entry_executable(entry), ec) &&
         read_all(entry / "status.json").find("\"status\":\"ok\"") != std::string::npos;
}

namespace {
// Extracts `"key":"value"` / `"key":true|false` from the aot_policy object of status.json (a flat, sanitized, machine-written object).
std::string policy_field(const std::string &object, const std::string &key) {
  const std::string needle = "\"" + key + "\":";
  const auto at = object.find(needle);
  if (at == std::string::npos) return {};
  std::size_t begin = at + needle.size();
  if (begin < object.size() && object[begin] == '"') {
    const auto end = object.find('"', begin + 1);
    return end == std::string::npos ? std::string() : object.substr(begin + 1, end - begin - 1);
  }
  const auto end = object.find_first_of(",}", begin);
  return object.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}
}  // namespace

PolicyReport read_policy_report(const fs::path &entry) {
  PolicyReport report;
  const std::string status = read_all(entry / "status.json");
  const auto at = status.find("\"aot_policy\":{");
  if (at == std::string::npos) return report;
  const auto end = status.find('}', at);
  const std::string object = status.substr(at, end == std::string::npos ? std::string::npos : end - at + 1);
  report.requested = policy_field(object, "requested");
  report.effective = policy_field(object, "effective");
  report.reason = policy_field(object, "reason");
  report.requested_model = policy_field(object, "requested_model");
  report.fallback_class = policy_field(object, "fallback_class");
  report.model_family = policy_field(object, "model_family");
  report.validator = policy_field(object, "validator");
  report.model = policy_field(object, "model");
  report.rank_policy = policy_field(object, "rank_policy");
  report.policy_identity = policy_field(object, "policy_identity");
  report.identity = policy_field(object, "identity");
  const auto number = [&object](const char *key) {
    const std::string value = policy_field(object, key);
    return value.empty() || value.find_first_not_of("0123456789") != std::string::npos ? -1LL : std::atoll(value.c_str());
  };
  report.selected_ppm = number("selected_ppm");
  report.admitted_ppm = number("admitted_ppm");
  report.fallback = policy_field(object, "fallback") == "true";
  report.present = !report.requested.empty() && !report.effective.empty();
  return report;
}

std::string read_observed_region(const fs::path &entry) {
  // `z80.observed_region` of status.json: the single console region the build-time Z80 observation ran under.
  const std::string status = read_all(entry / "status.json");
  const auto at = status.find("\"observed_region\":\"");
  if (at == std::string::npos) return {};
  const auto begin = at + 19;
  const auto end = status.find('"', begin);
  const std::string value = end == std::string::npos ? std::string() : status.substr(begin, end - begin);
  return value == "usa" || value == "japan" || value == "europe" ? value : std::string();
}

std::string region_coverage_note(const std::string &observed, const std::string &selected) {
  // Z80 (sound) images are materialized from ONE observed run. A different run-time region is not observed, so its sound coverage is not
  // established (it may stop with z80_unknown_image if that region uploads a different driver).
  if (observed.empty() || selected.empty() || observed == selected) return {};
  return "Sound was analyzed for the " + observed + " console; " + selected + " is not covered.";
}

std::string policy_summary(const PolicyReport &report) {
  if (!report.present) return {};
  if (report.requested != "optimized") return "Compatibility build";
  if (report.fallback) return "Optimized unavailable for this game: built Compatibility";
  if (report.effective == "admission_plan") return "Optimized build (exact map)";
  if (report.effective == "ml_region_gen2") return "Optimized build (experimental model)";
  if (report.effective == "ml_region_z80") return "Optimized build (experimental CNN)";
  return "Optimized build";
}

std::string policy_details(const PolicyReport &r) {
  if (!r.present) return {};
  const auto percent = [](long long ppm) { return ppm < 0 ? std::string("n/a") : std::to_string(ppm / 10000) + "." + std::to_string((ppm / 1000) % 10) + " %"; };
  std::string text = "Requested: " + r.requested + (r.requested == "optimized" && !r.requested_model.empty() ? " (model " + r.requested_model + ")" : "") + "\nEffective: " + r.effective;
  if (r.fallback) text += "\nOptimized fell back to the Compatibility program. Reason: " + r.reason + " (" + r.fallback_class + ")";
  if (!r.model_family.empty() && r.model_family != "none") text += "\nModel family: " + r.model_family + (r.rank_policy.empty() ? "" : ", rank policy " + r.rank_policy);
  if (!r.model.empty()) text += "\nModel: " + r.model;
  if (r.selected_ppm >= 0 || r.admitted_ppm >= 0) text += "\nSelected: " + percent(r.selected_ppm) + "   Admitted: " + percent(r.admitted_ppm);
  if (!r.validator.empty()) text += "\nValidator: " + r.validator;
  if (!r.identity.empty()) text += "\nBuild identity: " + r.identity.substr(0, 16) + "...";
  if (!r.policy_identity.empty()) text += "\nPolicy identity: " + r.policy_identity.substr(0, 16) + "...";
  return text;
}

namespace {
std::map<std::string, std::string> &identity_memo() { static std::map<std::string, std::string> memo; return memo; }
std::mutex &identity_memo_mutex() { static std::mutex m; return m; }
}  // namespace

std::string product_policy_identity_memo_lookup(const std::string &key) {
  std::lock_guard<std::mutex> lock(identity_memo_mutex());
  const auto found = identity_memo().find(key);
  return found == identity_memo().end() ? std::string() : found->second;
}

std::string product_policy_identity(const Layout &layout, const std::string &policy, const std::string &model, const std::string &platform) {
  const std::string key = utf8(layout.cli) + "|" + policy + "|" + model + (platform == "genesis" ? std::string() : "|" + platform);
  if (const std::string known = product_policy_identity_memo_lookup(key); !known.empty()) return known;
  std::string identity;
  std::error_code ec;
  if (fs::is_regular_file(layout.cli, ec)) {
    const std::string exe = utf8(layout.cli);
    std::vector<const char *> argv{exe.c_str(), "aot-policy-identity", "--aot-policy", policy.c_str()};
    if (platform != "genesis") { argv.push_back("--platform"); argv.push_back(platform.c_str()); }
    if (policy == "optimized" && platform == "genesis") { argv.push_back("--aot-model"); argv.push_back(model.c_str()); }
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (process) {
      std::string out;
      char buffer[256];
      SDL_IOStream *stream = SDL_GetProcessOutput(process);
      const Uint64 deadline = SDL_GetTicks() + 5000U;  // the tool prints one short line in milliseconds; never block the UI on a hung tool
      bool timed_out = false;
      while (stream) {
        if (SDL_GetTicks() > deadline) { timed_out = true; break; }
        const std::size_t got = SDL_ReadIO(stream, buffer, sizeof(buffer));
        if (got == 0) {
          if (SDL_GetIOStatus(stream) == SDL_IO_STATUS_NOT_READY) { SDL_Delay(5); continue; }
          break;
        }
        if (out.size() < 4096U) out.append(buffer, got);  // keep the head, but always drain the pipe to EOF
      }
      int code = -1;
      if (timed_out) SDL_KillProcess(process, true);
      SDL_WaitProcess(process, true, &code);
      SDL_DestroyProcess(process);
      const auto at = out.find("policy_identity=");
      if (!timed_out && code == 0 && at != std::string::npos) {
        const auto end = out.find_first_of(" \r\n", at);
        identity = out.substr(at + 16, end == std::string::npos ? std::string::npos : end - at - 16);
        if (identity.size() != 64U || identity.find_first_not_of("0123456789abcdef") != std::string::npos) identity.clear();
      }
    }
  }
  if (identity.empty()) return identity;  // a failure is never memoized: the next call retries
  std::lock_guard<std::mutex> lock(identity_memo_mutex());
  identity_memo()[key] = identity;
  return identity;
}

std::string product_policy_identity_cached(const Layout &layout, const std::string &policy, const std::string &model, const std::string &platform) {
  // Same memo as product_policy_identity, but never starts the tool: safe to call every frame on the UI thread.
  return product_policy_identity_memo_lookup(utf8(layout.cli) + "|" + policy + "|" + model + (platform == "genesis" ? std::string() : "|" + platform));
}

void reset_policy_identity_memo_for_tests() {
  std::lock_guard<std::mutex> lock(identity_memo_mutex());
  identity_memo().clear();
}

std::string rom_policy_identity(const Layout &layout, const RomView &rom) {
  return rom.platform_id == "master-system" ? product_policy_identity(layout, "optimized", "", "master-system")
                                            : product_policy_identity(layout, "optimized", rom.aot_model);
}

std::string rom_policy_identity_cached(const Layout &layout, const RomView &rom) {
  return rom.platform_id == "master-system" ? product_policy_identity_cached(layout, "optimized", "", "master-system")
                                            : product_policy_identity_cached(layout, "optimized", rom.aot_model);
}

bool sms_optimized_available_blocking(const Layout &layout) { return !product_policy_identity(layout, "optimized", "", "master-system").empty(); }
bool sms_optimized_available_cached(const Layout &layout) { return !product_policy_identity_cached(layout, "optimized", "", "master-system").empty(); }

RunVerdict classify_game_exit(const RomView &rom, int exit_code) {
  if (exit_code == 0) return RunVerdict::clean;
  if (rom.platform_id == "master-system" && rom.optimized() && exit_code == sms_exit_unadmitted_pc) return RunVerdict::unadmitted_pc;
  return RunVerdict::failed;
}

RomView compatibility_view(RomView rom) {
  rom.aot_policy = "compatibility";
  rom.aot_model = "v1";
  return rom;
}

bool effective_optimized(const PolicyReport &report) {
  return report.present && report.requested == "optimized" && !report.fallback && report.effective != "broad";
}

std::string effective_mode(const PolicyReport &report) { return effective_optimized(report) ? "optimized" : "compatibility"; }

RomView running_view(RomView rom, const PolicyReport &report) {
  rom.aot_policy = effective_mode(report);
  if (rom.aot_policy == "compatibility") rom.aot_model = "v1";
  return rom;
}

std::string running_label(const PolicyReport &report, bool restarted) {
  if (effective_optimized(report)) return "Playing... (Optimized, experimental)";
  if (restarted) return "Playing... (Compatibility, restarted)";
  if (report.present && report.requested == "optimized" && report.fallback) return "Playing... (Compatibility; Optimized fell back at build)";
  return "Playing... (Compatibility)";
}

RestartDecision decide_after_exit(const RomView &rom, int exit_code, bool already_restarted) {
  RestartDecision decision;
  if (!already_restarted && classify_game_exit(rom, exit_code) == RunVerdict::unadmitted_pc) {
    decision.restart = true;
    decision.next = compatibility_view(rom);
    decision.notice = unadmitted_restart_notice();
  }
  return decision;
}

std::string unadmitted_restart_notice() {
  return "Optimized stopped: the game reached code outside the selected set. Restarting from the beginning in Compatibility.";
}

bool entry_ready_for(const fs::path &entry, const RomView &rom, const Layout &layout) {
  if (!entry_ready(entry)) return false;
  const PolicyReport report = read_policy_report(entry);
  if (!report.present) return !rom.optimized();  // an older entry without the member is a Compatibility build
  if (report.requested != (rom.optimized() ? "optimized" : "compatibility")) return false;
  if (rom.optimized()) {
    if (rom.platform_id == "genesis" && report.requested_model != rom.aot_model) return false;
    const std::string current = rom_policy_identity(layout, rom);
    // Fail closed: an Optimized entry is reusable only when the CURRENT product identity is known and equals the recorded one. An unavailable
    // identity (tool missing, failing, timing out or printing garbage) refuses reuse; it never makes the check optional.
    if (current.empty() || report.policy_identity != current) return false;
  }
  return true;
}

std::string read_tail(const fs::path &path, std::size_t max_bytes) {
  std::string text = read_all(path);
  if (text.size() > max_bytes) text = "…" + text.substr(text.size() - max_bytes);
  return text;
}

// ---- BuildJob ----

BuildJob::BuildJob(RomView rom, Layout layout)
    : rom_(std::move(rom)), layout_(std::move(layout)), entry_(entry_dir(rom_, layout_)) {
  thread_ = std::thread([this] { run(); });
}

BuildJob::~BuildJob() { wait(); }

void BuildJob::wait() {
  if (thread_.joinable()) thread_.join();
}

std::string BuildJob::message() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_;
}

void BuildJob::run() {
  const auto set_message = [this](const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = text;
  };
  const auto finish = [this](bool ok) { ok_.store(ok); finished_.store(true); };
  std::error_code ec;
  // Build beside the final entry and publish by rename, so a crash never leaves a half-built entry "ready".
  const fs::path partial = entry_.parent_path() / (entry_.filename().string() + ".partial");
  fs::remove_all(partial, ec);
  fs::create_directories(partial.parent_path(), ec);
  if (ec) { set_message("Cannot create the Segarecomp cache folder."); finish(false); return; }

  const bool sms = rom_.platform_id == "master-system";
  std::vector<std::string> args{utf8(layout_.cli), "build", "--rom", utf8(rom_.path), "--output", utf8(partial),
                                "--cc", utf8(layout_.cc), "--cc-arg", "cc", "--cc-arg", "-target",
                                "--cc-arg", zig_target(), "--runtime-dir", utf8(sms ? layout_.runtime_sms : layout_.runtime),
                                "--sdl3-include", utf8(layout_.sdl_include), "--sdl3-lib", utf8(layout_.sdl_lib)};
  if (sms) {
    // The platform is selected by the recognized header; the mapper only by an explicit declaration (never a default).
    if (!rom_.mapper.empty()) args.insert(args.end(), {"--mapper", rom_.mapper});
    if (!rom_.mapper_manifest.empty()) args.insert(args.end(), {"--mapper-manifest", utf8(rom_.mapper_manifest)});
  }
  if (rom_.supports_hint_alias_discovery() && rom_.hint_alias_discovery) args.insert(args.end(), {"--hint-alias-discovery", "1"});
  if (rom_.optimized()) args.insert(args.end(), {"--aot-policy", "optimized"});
  if (rom_.optimized_gen2()) args.insert(args.end(), {"--aot-model", "gen2"});
#if defined(_WIN32)
  // The runtime keeps whole-machine state in automatic storage; match the POSIX 8 MiB main-thread stack.
  args.insert(args.end(), {"--link-arg", "-Wl,--stack,8388608"});
#else
  args.insert(args.end(), {"--link-arg", "-Wl,-rpath," + utf8(layout_.sdl_lib)});
#endif
  std::vector<const char *> argv;
  for (const auto &arg : args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);

  // zig keeps its own compile cache; keep it inside the Segarecomp cache rather than the user's home.
  SDL_Environment *environment = SDL_CreateEnvironment(true);
  const std::string zig_cache = utf8(cache_root() / "zig-cache");
  SDL_SetEnvironmentVariable(environment, "ZIG_GLOBAL_CACHE_DIR", zig_cache.c_str(), true);
  SDL_SetEnvironmentVariable(environment, "ZIG_LOCAL_CACHE_DIR", zig_cache.c_str(), true);

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  SDL_Process *process = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  SDL_DestroyEnvironment(environment);
  if (!process) { set_message("Could not start the Segarecomp build tool."); finish(false); return; }

  std::string result_line = "";
  std::string pending;
  char buffer[512];
  SDL_IOStream *output = SDL_GetProcessOutput(process);
  const auto handle_line = [&](const std::string &line) {
    if (line.rfind("@stage ", 0) == 0) {
      static const char *names[stage_count] = {"analyze", "generate", "compile", "link"};
      for (int i = 0; i < stage_count; ++i) {
        const std::string prefix = std::string("@stage ") + names[i] + ' ';
        if (line.rfind(prefix, 0) == 0)
          stages_[i].store(line.substr(prefix.size()) == "done" ? static_cast<int>(StageState::done)
                                                                : static_cast<int>(StageState::running));
      }
    } else if (line.rfind("@result failed", 0) == 0) {
      const auto at = line.find("message=");
      result_line = at == std::string::npos ? "The build failed." : line.substr(at + 8);
    }
  };
  while (output) {
    const std::size_t got = SDL_ReadIO(output, buffer, sizeof(buffer));
    if (got == 0) {
      // A pipe with no data yet reports NOT_READY; only EOF/error ends the read loop.
      if (SDL_GetIOStatus(output) == SDL_IO_STATUS_NOT_READY) { SDL_Delay(15); continue; }
      break;
    }
    pending.append(buffer, got);
    for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
      std::string line = pending.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      handle_line(line);
      pending.erase(0, nl + 1);
    }
  }
  int exit_code = -1;
  SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);

  const bool ok = exit_code == 0 && entry_ready(partial);
  if (ok) {
    std::ofstream meta(partial / "metadata.json", std::ios::binary);
    meta << "{\"rom_sha256\":\"" << rom_.sha256 << "\",\"title\":\"" << json_safe(rom_.title)
         << "\",\"segarecomp_version\":\"" SEGARECOMP_LAUNCHER_VERSION "\",\"platform\":\""
         << (sms ? "master-system" : "genesis") << "\",";
    if (sms)
      meta << "\"profile\":\"" << sms_profile << "\",\"mapper\":\"" << json_safe(rom_.mapper) << "\",\"mapper_declaration\":\""
           << (rom_.mapper.empty() ? "manifest" : "option") << "\",";
    meta << "\"host\":\"" << host_description() << "\",\"toolchain\":\"zig " SEGARECOMP_ZIG_VERSION "\",\"rom_path\":\"referenced, not copied\"}\n";
    meta.close();
    fs::remove_all(entry_, ec);
    fs::rename(partial, entry_, ec);
    if (ec) { set_message("Cannot store the build in the Segarecomp cache folder."); finish(false); return; }
    finish(true);
    return;
  }
  // Keep the failed attempt's diagnostics (build.log) where "View diagnostics" can find them.
  const fs::path failed = entry_.parent_path() / (entry_.filename().string() + ".failed");
  fs::remove_all(failed, ec);
  fs::rename(partial, failed, ec);
  set_message(result_line.empty() ? "The build failed unexpectedly." : result_line);
  finish(false);
}

// ---- GameRun ----

GameRun::GameRun(const RomView &rom, const Layout &layout, const fs::path &entry, const std::vector<std::string> &extra_args) {
  const std::string exe = utf8(entry_executable(entry));
  std::vector<const char *> argv{exe.c_str()};
  for (const auto &arg : extra_args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  SDL_Environment *environment = SDL_CreateEnvironment(true);
  apply_game_environment(environment, rom);
#if defined(_WIN32)
  // The game links SDL3.dll; find it next to the package rather than requiring it on PATH.
  const char *old_path = SDL_getenv("PATH");
  const std::string path = utf8(layout.sdl_lib) + ";" + (old_path ? old_path : "");
  SDL_SetEnvironmentVariable(environment, "PATH", path.c_str(), true);
#else
  (void)layout;
#endif
  SDL_IOStream *log = SDL_IOFromFile(utf8(entry / "run.log").c_str(), "wb");
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  if (log) {
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  } else {
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
  }
  process_ = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  SDL_DestroyEnvironment(environment);
  if (log) SDL_CloseIO(log);
}

GameRun::~GameRun() {
  if (process_) {
    int code = 0;
    if (!SDL_WaitProcess(process_, false, &code)) SDL_KillProcess(process_, false);
    SDL_DestroyProcess(process_);
  }
}

bool GameRun::poll() {
  if (!process_) return true;
  return SDL_WaitProcess(process_, false, &exit_code_);
}

} // namespace launcher
