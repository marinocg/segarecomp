#include "launcher_core.hpp"
#include "cartridge_recognition.hpp"

#include <cctype>

#include "json_lite.hpp"
#include "sega_cd.hpp"
#include "segarecomp/rom.hpp"
#include "segarecomp/patch.hpp"
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
  layout.runtime_root = layout.root / "runtime";
  layout.sdl_include = layout.root / "sdl3" / "include";
  layout.sdl_lib = layout.root / "sdl3" / "lib";
  std::error_code ec;
  for (const auto &required : {layout.cli, layout.cc, layout.runtime / "runtime" / "runtime.c",
                               layout.runtime / "viewer" / "viewer_sdl3.c",
                               layout.runtime_sms / "viewer" / "sms_viewer_sdl3.c", layout.root / "runtime" / "libs" / "viewer" / "src" / "sv_host.c",
                               layout.root / "runtime" / "libs" / "viewer" / "src" / "sv_input_diag.c",
                               layout.sdl_include / "SDL3" / "SDL.h"}) {
    if (!fs::exists(required, ec)) { layout.problem = "This Segarecomp installation is incomplete (missing " + utf8(required.filename()) + "). Please download it again."; break; }
  }
  for (const auto &required : {layout.runtime_root / "platforms" / "sega-cd" / "runtime" / "scd_main.c", layout.runtime_root / "platforms" / "sega-cd" / "viewer" / "scd_viewer.c",
                               layout.runtime / "runtime" / "vdp_render.c"}) {
    if (!fs::exists(required, ec)) { layout.scd_build_problem = "This installation does not contain the Sega CD runtime (missing " + utf8(required.filename()) + ")."; break; }
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
  // The shared viewer (libs/viewer): where its persistent settings live, that it was started by the launcher (the menu says "Return to
  // Launcher"), and the identity shown in its System page. None of this reaches the guest; it is host presentation only.
  SDL_SetEnvironmentVariable(environment, "SEGARECOMP_LAUNCHED", "1", true);
  const std::string viewer_settings = utf8(library_root() / "viewer-settings.cfg");
  SDL_SetEnvironmentVariable(environment, "SEGARECOMP_VIEWER_SETTINGS", viewer_settings.c_str(), true);
  {  // the launcher's UI icons, which the viewer menu shows when present (decorative: the menu works without them)
    const char *base = SDL_GetBasePath();
    const fs::path exe_dir = base != nullptr ? path_from_utf8(base) : fs::path{};
    std::error_code ec;
    for (const fs::path &candidate : {exe_dir.parent_path() / "Resources" / "assets", exe_dir / "assets"})
      if (fs::is_directory(candidate / "ui", ec)) { SDL_SetEnvironmentVariable(environment, "SEGARECOMP_ASSETS", utf8(candidate).c_str(), true); break; }
  }
  // SEGARECOMP_ROM_SHA256 is the digest of the image the program was built from (for a Patched variant: the verified patched image, never the original
  // digest passed off as patched bytes). The viewer keys the per-game widescreen profile on SEGARECOMP_PROFILE_ID when present (the existing explicit identity
  // field, also used for Sega CD): a Patched variant sets it to the ORIGINAL library game, so Vanilla and Patched share one set of preferences.
  if (rom.effective_sha256().size() == 64U) SDL_SetEnvironmentVariable(environment, "SEGARECOMP_ROM_SHA256", rom.effective_sha256().c_str(), true);
  SDL_UnsetEnvironmentVariable(environment, "SEGARECOMP_PATCH_GEOMETRY");   // SEG-139-T004: a package's geometry declaration is compiled into its own program; an inherited value can never restrict another game
  if ((rom.patched() || rom.luau_mod()) && rom.sha256.size() == 64U) SDL_SetEnvironmentVariable(environment, "SEGARECOMP_PROFILE_ID", rom.sha256.c_str(), true);
  else SDL_UnsetEnvironmentVariable(environment, "SEGARECOMP_PROFILE_ID");
  // SEG-122: the patch's presentation defaults reach the viewer only for a selected Patched variant (the launch path has already required its verified build); Vanilla clears them.
  // A widescreen patch whose package states no default still gets Enhanced Widescreen + Raw Extension (at full 16:9 width in the viewer): Comfort would hide the patch's effect.
  if (rom.patched()) {
    SDL_SetEnvironmentVariable(environment, "SEGARECOMP_PATCH_ID", rom.patch_identity.c_str(), true);
    SDL_SetEnvironmentVariable(environment, "SEGARECOMP_PATCH_WIDESCREEN", rom.patch_widescreen.empty() ? "enhanced_gameplay" : rom.patch_widescreen.c_str(), true);
    SDL_SetEnvironmentVariable(environment, "SEGARECOMP_PATCH_SIDE_STYLE", rom.patch_side_style.empty() ? "raw" : rom.patch_side_style.c_str(), true);
  } else {
    SDL_UnsetEnvironmentVariable(environment, "SEGARECOMP_PATCH_ID");
    SDL_UnsetEnvironmentVariable(environment, "SEGARECOMP_PATCH_WIDESCREEN");
    SDL_UnsetEnvironmentVariable(environment, "SEGARECOMP_PATCH_SIDE_STYLE");
  }
  {  // SEG-131: the player's data for this game and version; a non-Genesis game (or an invalid identity) gets none, and an inherited value can never leak in
    const fs::path saves = game_save_dir(rom);
    if (!saves.empty()) {
      std::error_code ec;
      fs::create_directories(saves, ec);   // best effort: the viewer creates what is missing and reports a failure
      SDL_SetEnvironmentVariable(environment, "SEGARECOMP_SAVE_DIR", utf8(saves).c_str(), true);
      SDL_SetEnvironmentVariable(environment, "SEGARECOMP_SAVE_VARIANT", rom.patched() ? rom.patch_identity.c_str() : rom.luau_mod() ? rom.luau_identity.c_str() : "vanilla", true);
    } else {
      SDL_UnsetEnvironmentVariable(environment, "SEGARECOMP_SAVE_DIR");
      SDL_UnsetEnvironmentVariable(environment, "SEGARECOMP_SAVE_VARIANT");
    }
  }
  if (!rom.title.empty()) {  // the window title names the version that runs: a Patched program is never presented as plain Vanilla
    const std::string title = rom.patched() ? rom.title + " (Patched: " + rom.patch_label + ")" : rom.title;
    SDL_SetEnvironmentVariable(environment, "SEGARECOMP_GAME_TITLE", title.c_str(), true);
  }
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
      const CartridgeFinding cartridge = recognize_genesis_cartridge(std::span<const std::uint8_t>(bytes.data(), bytes.size()));  // the one recognition, for the window and headless alike
      view.mapper = cartridge.mapper();
      view.cartridge_note = cartridge.note;
    }
    view.compat_known = fs::is_regular_file(layout.runtime / "compat" / (view.sha256 + ".json"), ec);
  } catch (const std::exception &error) {
    view.error = error.what();
  }
  return view;
}

namespace {
// "impl<N>/cat<M>" read from the runtime files the build compiles in (gems_native.h's implementation version, the catalog's revision); "-" when they cannot be read.
std::string number_after(const fs::path &file, const std::string &key) {
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);)
    if (line.rfind(key, 0) == 0) {
      std::string digits;
      for (std::size_t i = key.size(); i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) digits += line[i];
      return digits;
    }
  return {};
}
std::string native_audio_identity(const Layout &layout) {
  const fs::path runtime = layout.runtime / "runtime";
  const std::string impl = number_after(runtime / "gems_native.h", "#define GEMS_NATIVE_IMPLEMENTATION_VERSION ");
  const std::string catalog = number_after(runtime / "gems_driver_catalog.inc", "GEMS_CATALOG_REVISION(");
  return "impl" + (impl.empty() ? std::string("-") : impl) + "/cat" + (catalog.empty() ? std::string("-") : catalog);
}
}  // namespace

// Genesis keeps its historical key segment; Master System adds the platform, profile and declared mapper (and the sidecar
// manifest's content digest), so a different declaration or manifest always selects a fresh cache entry.
std::string platform_key(const RomView &rom, const Layout *layout_for_key) {
  // Compatibility keeps the historical Genesis key; Optimized adds the policy, so the two never share a cache entry (ADR 0096).
  if (rom.platform_id != "master-system") {
    // SEG-122-T009: a lock-on build never shares an entry with a standard one; a standard key stays exactly the historical key.
    const std::string discovery = (rom.hint_alias_discovery ? std::string(":hint-alias-discovery=1") : std::string()) + (rom.mapper.empty() ? std::string() : ":mapper=" + rom.mapper);  // all change the generated program
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

std::string build_options_key(const RomView &rom) { return platform_key(rom, nullptr); }

fs::path entry_dir(const RomView &rom, const Layout &layout) {
  // Everything that changes the produced binary is part of the key; a mismatch selects a fresh directory.
  // The build driver itself (which embeds the code generator) is part of the key: a different segarecomp
  // build must never reuse a program generated by another one, even at the same version string.
  std::error_code ec;
  const auto cli_size = fs::is_regular_file(layout.cli, ec) ? fs::file_size(layout.cli, ec) : 0U;
  const auto cli_time = fs::last_write_time(layout.cli, ec).time_since_epoch().count();
  // SEG-122: a Patched variant adds its full identity (package manifest digest, which names every payload digest, and the verified patched image
  // digest). A Vanilla key is byte-for-byte what it always was.
  // SEG-124: a native variant also names the provider source digest, the ABI version and the required capabilities (the package identity already covers them; the explicit fields keep
  // a changed provider from ever sharing an entry with an older build).
  std::string variant = rom.patched() ? "|patch=" + rom.patch_identity + ":" + rom.patch_target_sha256 : std::string();
  if (rom.patch_native())
    variant += "|native=" + rom.patch_native_source_sha256 + "@abi" + std::to_string(rom.patch_native_abi) + "/" + std::to_string(rom.patch_native_caps);
  // ADR 0291: the sound implementation is chosen by the build itself (no user option), so the identity of what it may choose from is part of the key: the native
  // replacement's implementation version and the revision of the verified-driver catalog. A product that ships a changed replacement or catalog never reuses an older program.
  // SEG-137-T007 (ADR 0296): a Luau-only mod adds its package identity (which names every payload digest, the capabilities and the ABI), the digest of the image that is compiled and the pinned compiler (with
  // the fixed compile options it determines the bytecode). Absent for Vanilla and schema 1/2 variants: their keys are what they always were.
  if (rom.luau_mod()) variant += "|luau=" + rom.luau_identity + ":" + rom.luau_effective_sha256 + ":luau-" + rom.luau_pin;
  if (rom.platform_id != "master-system") variant += "|audio=" + native_audio_identity(layout);
  const std::string key = rom.sha256 + variant + "|cli-" + std::to_string(static_cast<unsigned long long>(cli_size)) + "-" + std::to_string(static_cast<long long>(cli_time)) + "|" SEGARECOMP_LAUNCHER_VERSION "|" + platform_key(rom, &layout) + "|" + host_arch() + "|" +
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

namespace {
// The text of the object that starts at `at` (the position of its '{'), brace-balanced; empty when it never closes.
std::string balanced_object(const std::string &text, std::size_t at) {
  int depth = 0;
  for (std::size_t i = at; i < text.size(); ++i) {
    if (text[i] == '{') ++depth;
    else if (text[i] == '}' && --depth == 0) return text.substr(at, i - at + 1);
  }
  return {};
}
bool one_of(const std::string &value, std::initializer_list<const char *> allowed) {
  for (const char *item : allowed)
    if (value == item) return true;
  return false;
}
}  // namespace

AudioReport read_audio_report(const fs::path &entry) {
  AudioReport report;
  const std::string status = read_all(entry / "status.json");
  const std::string needle = "\"audio_implementation\":{";
  const auto at = status.find(needle);
  if (at == std::string::npos) return report;   // an older build, a failed or an imported one: nothing was recorded
  report.state = AudioReport::State::unknown;
  const std::string object = balanced_object(status, at + needle.size() - 1);
  const std::string schema = object.empty() ? std::string() : policy_field(object, "schema");
  if (schema != "1" && schema != "2") return report;   // an unknown (older or newer) layout is never interpreted
  const std::string kind = policy_field(object, "kind");
  const std::string audio_status = policy_field(object, "audio_status");
  const auto number = [&object](const char *key) {
    const std::string value = policy_field(object, key);
    return value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 15 ? -1LL : std::atoll(value.c_str());
  };
  if (schema == "2") {
    report.selection = policy_field(object, "selection");
    report.reason = policy_field(object, "reason");
    if (!one_of(report.selection, {"automatic", "developer_override"})) return report;
    if (!one_of(report.reason, {"original_supported", "no_verified_replacement", "original_aot_failure_verified_replacement", "developer_override"})) return report;
    const std::string states = policy_field(object, "save_states");
    if (!states.empty() && states != "supported") return report;
    report.save_states = states;
    report.implementation_version = number("native_implementation_version");
  } else {
    report.selection = policy_field(object, "selection") == "default" ? "default" : "explicit";   // schema 1: the explicit option of an earlier build
  }
  if (kind == "native_driver") {
    const std::string driver = policy_field(object, "driver"), verification = policy_field(object, "verification");
    if (driver != "gems" || !one_of(verification, {"build_qualified", "reference_checked"}) || !one_of(audio_status, {"supported", "degraded"}) ||
        policy_field(object, "coverage") != "complete")
      return report;
    report.driver = driver;
    report.verification = verification;
    report.version = policy_field(object, "version");
    if (report.version.empty() || report.version.size() > 12 || report.version.find_first_not_of("0123456789.") != std::string::npos) report.version = "unknown";
    const std::string rewrites = policy_field(object, "original_driver_rewrites_own_code");
    report.rewrites_own_code_known = rewrites == "true" || rewrites == "false";
    report.rewrites_own_code = rewrites == "true";
    report.observed_fault = policy_field(object, "fault");
    if (report.observed_fault.empty() || report.observed_fault.size() > 40 || report.observed_fault.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) report.observed_fault = "unknown";
    report.commands = number("commands");
    report.sequences = number("sequences");
    report.dac_samples = number("dac_samples");
  } else if (kind == "original_z80_aot") {
    if (!one_of(audio_status, {"supported", "degraded"})) return report;
    const auto alt = object.find("\"structural_alternatives\":{");
    if (alt != std::string::npos) report.alternative_forms = [&] {
      const std::string inner = balanced_object(object, alt + 26);
      const std::string value = policy_field(inner, "forms");
      return value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 15 ? -1LL : std::atoll(value.c_str());
    }();
  } else {
    return report;
  }
  report.schema = schema == "2" ? 2 : 1;
  report.kind = kind;
  report.audio_status = audio_status;
  report.state = AudioReport::State::known;
  return report;
}

std::string audio_headline(const AudioReport &report) {
  if (report.state == AudioReport::State::unknown) return "Unknown";
  if (report.state == AudioReport::State::not_recorded) return "Not recorded";
  if (report.kind == "native_driver") return "Native GEMS " + report.version;   // the version is the verified catalog's text ("unknown" when it was not recorded in a form this reader accepts)
  return "Original Z80 (generated-native)";
}

std::string audio_detail(const AudioReport &report) {
  if (report.state == AudioReport::State::unknown) return "The recorded audio details were not understood; nothing is assumed about this executable.";
  if (report.state == AudioReport::State::not_recorded) return "No audio details were recorded for this build; nothing is inferred.";
  if (report.kind == "native_driver") {
    std::string text;
    if (report.selection == "automatic") text += "- Automatically selected for a verified GEMS driver image" + std::string(report.rewrites_own_code_known && report.rewrites_own_code ? " (the original driver rewrites its own executable code, which the generated-native Z80 path cannot run).\n" : ".\n");
    else if (report.selection == "developer_override") text += "- Selected by a developer override for a verified GEMS driver image.\n";
    else text += "- Selected explicitly for a verified GEMS driver image (an earlier build).\n";
    text += "- Independently reimplemented in native C11";
    text += report.version == "unknown" ? " (driver version not recorded).\n" : "; matched to the published GEMS " + report.version + " layout and tables.\n";
    text += report.verification == "reference_checked"
                ? "- Verified against the original driver's command and chip-access behavior for this driver image (compared operation by operation with an independent implementation); this executable's playback has not itself been verified.\n"
                : "- Build-qualified only (identity, address map and protocol checked), not independently audio-verified.\n";
    text += "- Original Z80 not executed for that driver.\n";
    text += report.save_states == "supported" ? "- Save states and rewind supported.\n" : "- Save states are not available in this older build; rebuild to get them.\n";
    text += "- Timing is event-accurate rather than instruction-cycle-accurate.\n";
    text += "- Coverage: the driver's complete command and sequence-event set (FM, PSG, noise and digital patches, envelopes, tempo, loops).";
    if (report.implementation_version >= 0) text += " Implementation version " + std::to_string(report.implementation_version) + ".";
    text += "\n";
    if (report.observed_fault == "none") text += "- The build's start-up run met no unsupported operation";
    else text += "- The build's start-up run stopped the replacement at a malformed or unreadable resource (" + report.observed_fault + "); the rest of that run was silent";
    if (report.commands >= 0 && report.sequences >= 0) text += " (" + std::to_string(report.commands) + " commands, " + std::to_string(report.sequences) + " sequences";
    if (report.commands >= 0 && report.sequences >= 0) text += report.dac_samples >= 0 ? ", " + std::to_string(report.dac_samples) + " DAC samples)." : ").";
    else text += ".";
    return text;
  }
  std::string text;
  if (report.audio_status == "degraded")
    text += std::string("Degraded: structural code mutation - the sound driver rewrites its own executable code, which is not supported, and no verified replacement applies. Music is not supported in this executable.") +
            (report.alternative_forms > 0 ? " Compiled structural alternatives did not cover it.\n" : "\n");
  if (report.alternative_forms > 0)
    text += "The game's own sound driver runs as generated native code, with " + std::to_string(report.alternative_forms) + " compiled structural alternative forms of its self-modifying code. Not a replacement driver.";
  else text += "The game's own sound driver runs as generated native code. Not a replacement driver.";
  if (report.schema == 2 && report.selection == "automatic") text += report.reason == "original_supported" ? " No replacement was needed." : "";
  return text;
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

std::string recorded_cartridge_mapper(const fs::path &entry) {
  std::error_code ec;
  const fs::path file = entry / "metadata.json";
  if (!fs::is_regular_file(file, ec)) return {};
  if (fs::file_size(file, ec) > 16384U) return "?oversized";   // never equal to a declaration: such a record is not reusable
  const auto meta = json::parse(read_all(file));
  return meta && meta->is_object() ? meta->str("mapper") : std::string();
}

bool lowercase_hex_64(const std::string &text) {
  if (text.size() != 64U) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string read_variant_identity(const fs::path &entry) {
  std::error_code ec;
  const fs::path file = entry / "metadata.json";
  if (!fs::is_regular_file(file, ec)) return {};
  if (fs::file_size(file, ec) > 16384U) return "?oversized";   // never equal to any identity: such a record is not reusable
  const auto meta = json::parse(read_all(file));
  return meta && meta->is_object() ? meta->str("patch_identity") : std::string();  // an unreadable record has no variant, exactly as before this existed
}

// SEG-124: a native variant's recorded provider source digest, ABI version and capabilities must be exactly the requested ones; any other build (including a
// BPS-only or Vanilla one that records none) is not reusable for it, and a variant without a native capability never accepts a program that records one.
bool native_record_matches(const fs::path &entry, const RomView &rom) {
  std::error_code ec;
  const fs::path file = entry / "metadata.json";
  std::string source;
  long long abi = 0, caps = 0;
  if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) <= 16384U) {
    const auto meta = json::parse(read_all(file));
    if (meta && meta->is_object()) {
      source = meta->str("native_source_sha256");
      abi = meta->num("native_abi");
      caps = meta->num("native_caps");
    }
  }
  if (!rom.patch_native()) return source.empty();
  return source == rom.patch_native_source_sha256 && abi == static_cast<long long>(rom.patch_native_abi) && caps == static_cast<long long>(rom.patch_native_caps);
}

// SEG-137-T007: a Luau variant's recorded package identity, image digest, compiler pin and bytecode digest must be exactly the requested ones; any other build (Vanilla, BPS-only, another mod) is not
// reusable for it, and a request without a mod never accepts a program that records one.
bool luau_record_matches(const fs::path &entry, const RomView &rom) {
  std::error_code ec;
  const fs::path file = entry / "metadata.json";
  std::string identity, effective, pin, digest;
  if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) <= 16384U) {
    const auto meta = json::parse(read_all(file));
    if (meta && meta->is_object()) {
      identity = meta->str("luau_identity");
      effective = meta->str("luau_effective_sha256");
      pin = meta->str("luau_pin");
      digest = meta->str("luau_bytecode_digest");
    }
  }
  if (!rom.luau_mod()) return identity.empty();
  if (!(identity == rom.luau_identity && effective == rom.luau_effective_sha256 && pin == rom.luau_pin && lowercase_hex_64(digest))) return false;
  // The digest in the metadata must be the one the build itself recorded in status.json (the key already determines it; this catches a record that was altered or mixed up).
  const std::string status = read_all(entry / "status.json");
  const std::string key = "\"bytecode_digest\":\"";
  const std::size_t at = status.find(key);
  return at != std::string::npos && status.compare(at + key.size(), 64U, digest) == 0;
}

bool entry_ready_for(const fs::path &entry, const RomView &rom, const Layout &layout) {
  if (!entry_ready(entry)) return false;
  if (!luau_record_matches(entry, rom)) return false;
  // SEG-122: defence in depth on top of the cache key. A build is reusable only for the variant it records: a Vanilla request never accepts a patched
  // program and a Patched request accepts only the program of exactly this package.
  if (read_variant_identity(entry) != rom.patch_identity) return false;
  if (!native_record_matches(entry, rom)) return false;
  if (rom.platform_id == "genesis" && recorded_cartridge_mapper(entry) != rom.mapper) return false;  // SEG-122-T009: never a standard build as a lock-on one, or the reverse
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

// ---- ScdBuildJob (SEG-078-T003, ADR 0205) ----

const char *ScdBuildJob::stage_label(int index) {
  static const char *labels[stage_count] = {"Generate", "Compile", "Link", "Run discovery pass", "Confirm", "Viewer"};
  return index >= 0 && index < stage_count ? labels[index] : "";
}

fs::path scd_build_program(const fs::path &out_dir) { return out_dir / (std::string("program-viewer") + exe_suffix); }

const char *scd_build_mode_code(ScdBuildMode mode) {
  switch (mode) {
    case ScdBuildMode::file_first: return "file_first";
    case ScdBuildMode::optimized: return "optimized";
    default: return "compatibility";
  }
}

std::vector<std::string> scd_build_policy_arguments(ScdBuildMode mode) {
  switch (mode) {
    case ScdBuildMode::file_first: return {"--file-first", "structural"};   // the product's own option, nothing else (never an aot policy)
    case ScdBuildMode::optimized: return {"--aot-policy", "optimized", "--aot-model", "gen2"};
    default: return {};
  }
}

namespace {
// The text of the object that starts at `key`":{` in a JSON text (balanced braces, strings skipped), or empty when the member is absent or not an object (null).
std::string scd_json_object(const std::string &text, const std::string &key) {
  const std::string needle = "\"" + key + "\":{";
  const auto at = text.find(needle);
  if (at == std::string::npos) return {};
  const std::size_t begin = at + needle.size() - 1U;
  int depth = 0;
  bool in_string = false;
  for (std::size_t i = begin; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) { if (c == '\\') ++i; else if (c == '"') in_string = false; continue; }
    if (c == '"') in_string = true;
    else if (c == '{') ++depth;
    else if (c == '}' && --depth == 0) return text.substr(begin, i + 1U - begin);
  }
  return {};
}
std::string scd_json_field(const std::string &object, const std::string &key) {
  const auto k = object.find("\"" + key + "\":");
  if (k == std::string::npos) return {};
  std::size_t p = k + key.size() + 3U;
  if (p < object.size() && object[p] == '"') { const auto e = object.find('"', p + 1U); return e == std::string::npos ? std::string() : object.substr(p + 1U, e - p - 1U); }
  std::size_t e = p;
  while (e < object.size() && object[e] != ',' && object[e] != '}') ++e;
  return object.substr(p, e - p);
}
} // namespace

ScdFileFirstReport read_scd_file_first_report(const std::string &status_text) {
  ScdFileFirstReport out;
  const std::string object = scd_json_object(status_text, "file_first");
  if (object.empty()) return out;   // absent, null (not a file-first build) or malformed: nothing is claimed
  out.present = true;
  out.complete = scd_json_field(object, "complete") == "true";
  out.policy = scd_json_field(object, "policy");
  out.files = std::atoll(scd_json_field(object, "files").c_str());
  out.images = std::atoll(scd_json_field(object, "images").c_str());
  out.oversize_files = std::atoll(scd_json_field(object, "oversize_files").c_str());
  out.no_structure_files = std::atoll(scd_json_field(object, "no_structure_files").c_str());
  out.duplicate_files = std::atoll(scd_json_field(object, "duplicate_files").c_str());
  out.compiled_offsets = std::atoll(scd_json_field(object, "compiled_offsets").c_str());
  return out;
}

std::string scd_file_first_sentence(const ScdFileFirstReport &r) {
  if (!r.present) return {};
  std::string text = "File-first (structural): " + std::to_string(r.images) + " disc image(s) from " + std::to_string(r.files) + " disc file(s) were compiled before the game ran; their code is bound at run time only when the bytes the game loads match them exactly. ";
  if (!r.complete)
    text += "The inventory is incomplete (" + std::to_string(r.oversize_files) + " oversize, " + std::to_string(r.no_structure_files) + " without code structure, " + std::to_string(r.duplicate_files) +
            " duplicate file(s) were not compiled): code outside it still stops at an uncompiled entry. ";
  else
    text += "The inventory covered every eligible file; code the disc does not contain verbatim is still not compiled. ";
  return text;
}

std::string scd_effective_build_mode(const std::string &status_text) {
  if (read_scd_file_first_report(status_text).present) return "file_first";
  const ScdPolicyReport policy = read_scd_policy_report(status_text);
  return policy.present && policy.requested == "optimized" && policy.effective == "selective" ? "optimized" : "compatibility";
}

ScdPolicyReport read_scd_policy_report(const std::string &status_text) {
  ScdPolicyReport out;
  const auto at = status_text.find("\"aot_policy\":{");
  if (at == std::string::npos) return out;
  const auto end = status_text.find("\"images\":[", at);   // the scalar members come first; the per-image rows are not read
  const std::string object = status_text.substr(at, end == std::string::npos ? std::string::npos : end - at);
  const auto field = [&](const std::string &key) -> std::string {
    const auto k = object.find("\"" + key + "\":");
    if (k == std::string::npos) return {};
    std::size_t p = k + key.size() + 3U;
    if (p < object.size() && object[p] == '"') { const auto e = object.find('"', p + 1U); return e == std::string::npos ? std::string() : object.substr(p + 1U, e - p - 1U); }
    std::size_t e = p;
    while (e < object.size() && object[e] != ',' && object[e] != '}') ++e;
    return object.substr(p, e - p);
  };
  out.present = true;
  out.requested = field("requested");
  out.effective = field("effective");
  out.reason = field("reason");
  out.failed_image = field("failed_image");
  out.fallback = field("fallback") == "true";
  out.broad_entries = std::atoll(field("broad_entries").c_str());
  out.admitted_entries = std::atoll(field("admitted_entries").c_str());
  out.m3_images = std::atoll(field("m3_images").c_str());
  out.m3_broad_entries = std::atoll(field("m3_broad_entries").c_str());
  out.m3_admitted_entries = std::atoll(field("m3_admitted_entries").c_str());
  return out;
}

std::string scd_policy_sentence(const ScdPolicyReport &r) {
  if (!r.present || r.requested != "optimized") return {};
  if (r.effective == "selective" && r.m3_images == 0)
    return "Experimental Optimized was used by an earlier release (all images were optimized, including the BIOS); rebuild to use the current game-module scope. ";
  if (r.effective == "selective")
    return "Experimental Optimized: only the game's own loaded modules are optimized (" + std::to_string(r.m3_images) + " module image(s): " + std::to_string(r.m3_admitted_entries) +
           " of the " + std::to_string(r.m3_broad_entries) + " instruction entries Compatibility would compile); the BIOS and the game's boot code stay fully compiled. "
           "It may still omit module code the game needs and then stops at an uncompiled entry; rebuild with Compatibility if it does. ";
  return "Experimental Optimized was requested but this build is Compatibility (the product fell back: " + (r.reason.empty() ? std::string("unspecified") : r.reason) + "). ";
}

std::string scd_frontier_text(const std::string &refusal) {
  if (refusal == "no_source") return "The game runs code that the disc does not contain verbatim (generated, decompressed or modified at run time); it is not compiled.";
  if (refusal == "ambiguous_source") return "The game runs code that is identical in several places on the disc, so its origin cannot be proven; it is not compiled.";
  if (refusal == "no_delivery_evidence" || refusal == "no_evidence" || refusal == "ledger_overflow")
    return "The build could not establish where the code came from (no or too much load evidence); it is not compiled.";
  if (refusal == "extent_bound_exceeded") return "The game loads more separate code modules than a build admits; the rest is not compiled.";
  if (refusal == "no_progress") return "The same code module was explained twice without progress; the rest is not compiled.";
  return "A code region at the end of the covered path could not be proven to come from the disc; it is not compiled.";
}

ScdBuildJob::ScdBuildJob(ScdBuildRequest request, Layout layout, bool cancelled_at_start) : request_(std::move(request)), layout_(std::move(layout)) {
  cancel_.store(cancelled_at_start);
  thread_ = std::thread([this] { run(); });
}

ScdBuildJob::~ScdBuildJob() { wait(); }

void ScdBuildJob::wait() {
  if (thread_.joinable()) thread_.join();
}

BuildOutcome ScdBuildJob::outcome() const {
  if (!finished_.load()) return BuildOutcome::running;
  if (ok_.load()) return BuildOutcome::succeeded;
  return cancelled_.load() ? BuildOutcome::cancelled : BuildOutcome::failed;
}

bool ScdBuildJob::request_cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (publishing_ || ok_.load()) return false;
    if (finished_.load()) return cancelled_.load();
    cancel_.store(true);
  }
  tree_.terminate();
  return true;
}

std::string ScdBuildJob::message() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_;
}

std::string ScdBuildJob::summary() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return summary_;
}

fs::path ScdBuildJob::program() const { return ok_.load() ? scd_build_program(request_.out_dir) : fs::path{}; }

void ScdBuildJob::run() {
  const auto set_message = [this](const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    message_ = text;
  };
  const auto finish = [this](bool ok) { ok_.store(ok); finished_.store(true); };
  std::error_code ec;
  const fs::path partial = request_.out_dir.parent_path() / (request_.out_dir.filename().string() + ".partial");
  const auto finish_cancelled = [&] {
    std::error_code remove_ec;
    for (int attempt = 0; attempt < 40; ++attempt) {
      fs::remove_all(partial, remove_ec);
      if (!remove_ec && !fs::exists(partial, remove_ec)) break;
      SDL_Delay(50);
    }
    set_message("Build cancelled.");
    cancelled_.store(true);
    finish(false);
  };
  if (cancel_.load()) { set_message("Build cancelled."); cancelled_.store(true); finish(false); return; }
  if (!layout_.scd_build_problem.empty()) { set_message(layout_.scd_build_problem); finish(false); return; }
  fs::remove_all(partial, ec);
  fs::create_directories(partial.parent_path(), ec);
  if (ec) { set_message("Cannot create the folder for this game's build."); finish(false); return; }
  if (cancel_.load()) { finish_cancelled(); return; }

  // The build pass runs the console as the launcher will: from a new console's formatted, empty internal backup RAM (a BIOS with an unformatted one stops at its
  // backup manager instead of starting the disc) and with no controller input (the build does not invent input).
  const fs::path backup_ram = request_.out_dir.parent_path() / "build-backup-ram.bin";
  if (!scd::write_pristine_backup_ram(backup_ram)) { set_message("Cannot prepare the backup RAM image for the build."); finish(false); return; }
  const fs::path objects = cache_root() / "sega-cd-objects";
  std::vector<std::string> args{utf8(layout_.cli), "sega-cd", "build", "--cue", utf8(request_.cue), "--bios", utf8(request_.bios), "--output", utf8(partial),
                                "--cc", utf8(layout_.cc), "--cc-arg", "cc", "--cc-arg", "-target", "--cc-arg", zig_target(),
                                "--runtime-dir", utf8(layout_.runtime_root), "--viewer", "1", "--cache-dir", utf8(objects), "--backup-ram", utf8(backup_ram),
                                "--sdl3-include", utf8(layout_.sdl_include), "--sdl3-lib", utf8(layout_.sdl_lib)};
#if defined(_WIN32)
  args.insert(args.end(), {"--link-arg", "-Wl,--stack,8388608"});
#else
  args.insert(args.end(), {"--link-arg", "-Wl,-rpath," + utf8(layout_.sdl_lib)});
#endif
  for (const auto &option : scd_build_policy_arguments(request_.mode)) args.push_back(option);
  if (const char *extra = SDL_getenv("SEGARECOMP_SCD_BUILD_ARGS")) {  // TEST SEAM (documented in ADR 0205; local environment only, never set by Segarecomp): whitespace-separated extra product-CLI options (budget, host source, ...)
    std::istringstream words(extra);
    for (std::string word; words >> word;) args.push_back(word);
  }
  std::vector<const char *> argv;
  for (const auto &arg : args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);

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
  if (!process) {
    if (cancel_.load()) { finish_cancelled(); return; }
    set_message("Could not start the Segarecomp build tool.");
    finish(false);
    return;
  }
  tree_.attach(process);
  if (cancel_.load()) tree_.terminate();

  std::string result_line, pending;
  char buffer[512];
  SDL_IOStream *output = SDL_GetProcessOutput(process);
  const auto set_stage = [this](int index, StageState state) { stages_[index].store(static_cast<int>(state)); };
  const auto handle_line = [&](const std::string &line) {
    if (line.rfind("@round ", 0) == 0) {
      round_.store(std::atoi(line.c_str() + 7) + 1);
      for (int i = 0; i < 4; ++i) set_stage(i, StageState::pending);
      set_stage(0, StageState::running);
      set_message("Discovery round " + std::to_string(round_.load()));
    } else if (line.rfind("@stage ", 0) == 0) {
      static const struct { const char *name; int index; } map[] = {{"analyze", 0}, {"generate", 0}, {"compile", 1}, {"link", 2}, {"pass", 3}, {"confirm", 4}, {"viewer", 5}};
      for (const auto &m : map) {
        const std::string prefix = std::string("@stage ") + m.name + ' ';
        if (line.rfind(prefix, 0) != 0) continue;
        const bool done = line.substr(prefix.size()) == "done";
        if (std::string(m.name) == "analyze") break;   // the first generate belongs to the same stage; "analyze done" carries no extra information
        set_stage(m.index, done ? StageState::done : StageState::running);
        if (!done && m.index + 1 < stage_count && m.index < 3) {}  // later stages of the round are reset by the next @round
        break;
      }
    } else if (line.rfind("@result failed", 0) == 0) {
      const auto at = line.find("message=");
      result_line = at == std::string::npos ? "The build failed." : line.substr(at + 8);
    }
  };
  while (output && !cancel_.load()) {
    const std::size_t got = SDL_ReadIO(output, buffer, sizeof(buffer));
    if (got == 0) {
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
  if (cancel_.load()) tree_.terminate();
  int exit_code = -1;
  SDL_WaitProcess(process, true, &exit_code);
  tree_.release();
  SDL_DestroyProcess(process);

  bool publish = false;
  std::string status_text;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cancel_.load()) {
      publishing_ = true;
      status_text = read_all(partial / "status.json");
      publish = exit_code == 0 && status_text.find("\"status\":\"ok\"") != std::string::npos && fs::exists(scd_build_program(partial), ec);
      if (publish && request_.mode == ScdBuildMode::file_first && !read_scd_file_first_report(status_text).present) {
        // SEG-103-T007: a file-first request is never published as file-first on the strength of the request: the product's structured inventory must say so.
        publish = false;
        result_line = "File-first was requested but the product did not report a file-first inventory for this disc, so the program was not accepted as file-first.";
      }
    }
  }
  if (cancel_.load()) { finish_cancelled(); return; }
  if (publish) {
    // What the program covers, in plain language, from the product's own typed fields (never parsed from human text).
    const auto field = [&](const std::string &key) -> std::string {
      const auto at = status_text.find("\"" + key + "\":");
      if (at == std::string::npos) return {};
      std::size_t p = at + key.size() + 3U;
      if (p < status_text.size() && status_text[p] == '"') { const auto e = status_text.find('"', p + 1U); return e == std::string::npos ? std::string() : status_text.substr(p + 1U, e - p - 1U); }
      std::size_t e = p;
      while (e < status_text.size() && status_text[e] != ',' && status_text[e] != '}') ++e;
      return status_text.substr(p, e - p);
    };
    const ScdPolicyReport policy = read_scd_policy_report(status_text);
    built_experimental_.store(policy.present && policy.requested == "optimized" && policy.effective == "selective");
    const ScdFileFirstReport inventory = read_scd_file_first_report(status_text);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      built_mode_ = scd_effective_build_mode(status_text);
      file_first_ = inventory;
    }
    std::string text = "Built in " + field("rounds") + " round(s). " + scd_policy_sentence(policy) + scd_file_first_sentence(inventory);
    const auto frontier = status_text.find("\"frontier\":{");
    if (frontier != std::string::npos) {
      const auto refusal = status_text.find("\"refusal\":\"", frontier);
      const auto end = refusal == std::string::npos ? std::string::npos : status_text.find('"', refusal + 11U);
      text += "The program covers the part of the game the build could prove, and stops at the first code it cannot: " +
              scd_frontier_text(end == std::string::npos ? std::string() : status_text.substr(refusal + 11U, end - refusal - 11U));
    } else {
      text += "No unexplained code region was reached while the build ran.";
      const std::string stop = field("stop");
      if (stop == "pcm_unsupported") text += " The run reached sound playback (PCM), which is not supported, and the program stops there with a typed stop.";
      else if (!stop.empty() && stop != "none") text += " The run ended at the typed stop \"" + stop + "\"; the program stops there.";
      else text += " The program stops with a typed stop if it later runs into code outside what was proven.";
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      summary_ = text;
    }
    // Publish without ever losing the previous build: move it aside, move the new one in, and put the old one back if that fails.
    const fs::path aside = request_.out_dir.parent_path() / (request_.out_dir.filename().string() + ".old");
    fs::remove_all(aside, ec);
    const bool had_old = fs::exists(request_.out_dir, ec);
    if (had_old) {
      fs::rename(request_.out_dir, aside, ec);
      if (ec) { set_message("Cannot store the build in the game's folder (the previous build is kept)."); finish(false); return; }
    }
    fs::rename(partial, request_.out_dir, ec);
    if (ec) {
      std::error_code restore_ec;
      if (had_old) fs::rename(aside, request_.out_dir, restore_ec);
      set_message("Cannot store the build in the game's folder (the previous build is kept).");
      finish(false);
      return;
    }
    fs::remove_all(aside, ec);
    finish(true);
    return;
  }
  const fs::path failed = request_.out_dir.parent_path() / (request_.out_dir.filename().string() + ".failed");
  fs::remove_all(failed, ec);
  fs::rename(partial, failed, ec);
  std::string failure = result_line.empty() ? "The build failed unexpectedly." : result_line;
  if (request_.mode == ScdBuildMode::file_first)   // SEG-103-T007: no silent fallback; the user decides whether to try Compatibility
  {
    if (failure.find("\"error\":\"iso9660\"") != std::string::npos) failure = "this disc has no ISO9660 file system the product can read, so its files cannot be inventoried";
    while (!failure.empty() && (failure.back() == '.' || failure.back() == ' ')) failure.pop_back();
    failure = "File-first build failed: " + failure + ". Nothing was substituted; choose Compatibility to build this game without file-first.";
  }
  set_message(failure);
  finish(false);
}

std::string ScdBuildJob::built_mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return built_mode_;
}

ScdFileFirstReport ScdBuildJob::file_first_report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_first_;
}

// ---- BuildJob ----

BuildJob::BuildJob(RomView rom, Layout layout, bool cancelled_at_start)
    : rom_(std::move(rom)), layout_(std::move(layout)), entry_(entry_dir(rom_, layout_)) {
  cancel_.store(cancelled_at_start);
  thread_ = std::thread([this] { run(); });
}

BuildJob::~BuildJob() { wait(); }

void BuildJob::wait() {
  if (thread_.joinable()) thread_.join();
}

BuildOutcome BuildJob::outcome() const {
  if (!finished_.load()) return BuildOutcome::running;
  if (ok_.load()) return BuildOutcome::succeeded;
  return cancelled_.load() ? BuildOutcome::cancelled : BuildOutcome::failed;
}

bool BuildJob::request_cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (publishing_ || ok_.load()) return false;            // the build is already being published / published: it stands
    if (finished_.load()) return cancelled_.load();         // already ended: idempotent for a cancel, refused for a failure
    cancel_.store(true);
  }
  tree_.terminate();
  return true;
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
  // A cancelled attempt owns exactly its `<entry>.partial` folder: remove it (a just-killed compiler may hold a file open for a moment on
  // Windows, hence the short retries) and report `cancelled`. The completed entry, `.failed` diagnostics of an earlier attempt and the shared
  // compiler cache are never touched.
  const auto finish_cancelled = [&](const fs::path &partial_dir) {
    std::error_code remove_ec;
    for (int attempt = 0; attempt < 40; ++attempt) {
      fs::remove_all(partial_dir, remove_ec);
      if (!remove_ec && !fs::exists(partial_dir, remove_ec)) break;
      SDL_Delay(50);
    }
    set_message("Build cancelled.");
    cancelled_.store(true);
    finish(false);
  };
  // Build beside the final entry and publish by rename, so a crash never leaves a half-built entry "ready".
  const fs::path partial = entry_.parent_path() / (entry_.filename().string() + ".partial");
  if (cancel_.load()) { set_message("Build cancelled."); cancelled_.store(true); finish(false); return; }  // before anything was touched
  fs::remove_all(partial, ec);
  fs::create_directories(partial.parent_path(), ec);
  if (ec) { set_message("Cannot create the Segarecomp cache folder."); finish(false); return; }
  if (cancel_.load()) { finish_cancelled(partial); return; }  // cancelled before the build tool was started: it never spawns

  const bool sms = rom_.platform_id == "master-system";
  // SEG-122: a Patched variant compiles the verified patched image, materialized from the original file and the installed package inside this attempt's own
  // partial folder (removed again before publishing). The original ROM is only read; the previous published build is never touched.
  fs::path build_rom = rom_.path;
  if (rom_.patched()) {
    std::error_code input_ec;
    const fs::path input = partial / "input";
    fs::create_directories(input, input_ec);
    const auto applied = segarecomp::patch::apply_package_file(rom_.patch_package, rom_.path, input / "patched.rom");
    std::string refusal;
    if (input_ec) refusal = "Cannot create the Segarecomp cache folder.";
    else if (!applied.ok) refusal = "The patch cannot be applied: " + applied.error + ".";
    else if (applied.info.identity != rom_.patch_identity) refusal = "The installed patch package is not the one selected (its identity changed).";
    else if (applied.rom_sha256 != rom_.sha256 || applied.target_sha256 != rom_.patch_target_sha256) refusal = "The original game or the patched image does not match the installed patch.";
    else if (applied.info.has_native != rom_.patch_native() ||
             (applied.info.has_native && (applied.info.native_sha256 != rom_.patch_native_source_sha256 || applied.info.native_abi != rom_.patch_native_abi || applied.info.native_caps != rom_.patch_native_caps)))
      refusal = "The native provider of the installed patch does not match what is selected.";
    else if (rom_.patch_native() && !rom_.patch_native_approved)
      refusal = "This patch contains executable code and has not been approved. Install the package again and approve it.";
    if (!refusal.empty()) {
      fs::remove_all(partial, ec);
      set_message(refusal);
      finish(false);
      return;
    }
    build_rom = input / "patched.rom";
    if (cancel_.load()) { finish_cancelled(partial); return; }
  }
  std::vector<std::string> args{utf8(layout_.cli), "build", "--rom", utf8(build_rom), "--output", utf8(partial),
                                "--cc", utf8(layout_.cc), "--cc-arg", "cc", "--cc-arg", "-target",
                                "--cc-arg", zig_target(), "--runtime-dir", utf8(sms ? layout_.runtime_sms : layout_.runtime),
                                "--sdl3-include", utf8(layout_.sdl_include), "--sdl3-lib", utf8(layout_.sdl_lib)};
  if (sms) {
    // The platform is selected by the recognized header; the mapper only by an explicit declaration (never a default).
    if (!rom_.mapper.empty()) args.insert(args.end(), {"--mapper", rom_.mapper});
    if (!rom_.mapper_manifest.empty()) args.insert(args.end(), {"--mapper-manifest", utf8(rom_.mapper_manifest)});
  } else if (!rom_.mapper.empty()) {
    args.insert(args.end(), {"--mapper", rom_.mapper});  // SEG-122-T009: the launcher's selection, explicit to the build tool (ADR 0268); a standard cartridge passes nothing, as ever
  }
  if (rom_.supports_hint_alias_discovery() && rom_.hint_alias_discovery) args.insert(args.end(), {"--hint-alias-discovery", "1"});
  if (rom_.patch_native())   // SEG-124 (ADR 0267): a REFERENCE to the installed package and the identity the user approved; the build re-verifies the package itself and extracts the provider from it
    args.insert(args.end(), {"--patch-package", utf8(rom_.patch_package), "--patch-identity", rom_.patch_identity});
  if (rom_.luau_mod())       // SEG-137-T007: a REFERENCE to the installed package and its identity; the build re-verifies it, derives the effective image and compiles the scripts itself
    args.insert(args.end(), {"--luau-package", utf8(rom_.luau_package), "--luau-identity", rom_.luau_identity});
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
  if (!process) {
    if (cancel_.load()) { finish_cancelled(partial); return; }
    set_message("Could not start the Segarecomp build tool.");
    finish(false);
    return;
  }
  tree_.attach(process);
  if (cancel_.load()) tree_.terminate();  // a request that arrived while the process was being created

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
  while (output && !cancel_.load()) {
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
  if (cancel_.load()) tree_.terminate();  // idempotent; also covers a cancel that ended the read loop
  int exit_code = -1;
  SDL_WaitProcess(process, true, &exit_code);
  tree_.release();
  SDL_DestroyProcess(process);

  // The decision point. After it the build is published and a cancel request is refused; before it a request wins and nothing is published.
  bool publish = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cancel_.load()) {
      publishing_ = true;  // decided: a later cancel request is refused (the outcome, success or failure, stands)
      publish = exit_code == 0 && entry_ready(partial);
    }
  }
  if (cancel_.load()) { finish_cancelled(partial); return; }
  const bool ok = publish;
  if (ok) {
    std::ofstream meta(partial / "metadata.json", std::ios::binary);
    meta << "{\"rom_sha256\":\"" << rom_.sha256 << "\",\"title\":\"" << json_safe(rom_.title)
         << "\",\"segarecomp_version\":\"" SEGARECOMP_LAUNCHER_VERSION "\",\"platform\":\""
         << (sms ? "master-system" : "genesis") << "\",";
    if (sms)
      meta << "\"profile\":\"" << sms_profile << "\",\"mapper\":\"" << json_safe(rom_.mapper) << "\",\"mapper_declaration\":\""
           << (rom_.mapper.empty() ? "manifest" : "option") << "\",\"mapper_manifest_sha256\":\"" << (rom_.mapper.empty() ? rom_.mapper_manifest_sha256 : std::string()) << "\",";
    else if (!rom_.mapper.empty())  // SEG-122-T009: absent for a standard Genesis cartridge, so its metadata is unchanged
      meta << "\"mapper\":\"" << json_safe(rom_.mapper) << "\",\"mapper_declaration\":\"option\",";
    // SEG-069-T005: what a later release needs to decide that this program can still be played (see older_build.hpp).
    meta << "\"runtime_contract\":" << runtime_contract << ",\"hint_alias_discovery\":" << (rom_.supports_hint_alias_discovery() && rom_.hint_alias_discovery ? "true" : "false") << ",";
    if (rom_.patched())  // SEG-122: the variant this program was built for (checked again by entry_ready_for; absent for Vanilla)
      meta << "\"patch_identity\":\"" << rom_.patch_identity << "\",\"patch_target_sha256\":\"" << rom_.patch_target_sha256 << "\",\"patch_label\":\"" << json_safe(rom_.patch_label) << "\",";
    if (rom_.patch_native())   // SEG-124: what native capability this program was built with (checked again by entry_ready_for)
      meta << "\"native_source_sha256\":\"" << rom_.patch_native_source_sha256 << "\",\"native_abi\":" << rom_.patch_native_abi << ",\"native_caps\":" << rom_.patch_native_caps << ",";
    if (rom_.luau_mod()) {   // SEG-137-T007: what this program was built for (checked again by entry_ready_for); the digest is the one the build recorded after it compiled and verified the scripts
      const std::string status_text = read_all(partial / "status.json");
      const std::string key = "\"bytecode_digest\":\"";
      const std::size_t at = status_text.find(key);
      const std::string digest = at == std::string::npos ? std::string() : status_text.substr(at + key.size(), 64U);
      meta << "\"luau_identity\":\"" << rom_.luau_identity << "\",\"luau_effective_sha256\":\"" << rom_.luau_effective_sha256 << "\",\"luau_pin\":\"" << json_safe(rom_.luau_pin)
           << "\",\"luau_bytecode_digest\":\"" << digest << "\",\"luau_label\":\"" << json_safe(rom_.luau_label) << "\",";
    }
    meta << "\"host\":\"" << host_description() << "\",\"toolchain\":\"zig " SEGARECOMP_ZIG_VERSION "\",\"rom_path\":\"referenced, not copied\"}\n";
    meta.close();
    fs::remove_all(partial / "input", ec);  // the materialized patched image is derivative game data: it never outlives the build
    fs::remove_all(entry_, ec);
    fs::rename(partial, entry_, ec);
    if (ec) { set_message("Cannot store the build in the Segarecomp cache folder."); finish(false); return; }
    finish(true);
    return;
  }
  // Keep the failed attempt's diagnostics (build.log) where "View diagnostics" can find them.
  const fs::path failed = entry_.parent_path() / (entry_.filename().string() + ".failed");
  fs::remove_all(partial / "input", ec);
  fs::remove_all(failed, ec);
  fs::rename(partial, failed, ec);
  set_message(result_line.empty() ? "The build failed unexpectedly." : result_line);
  finish(false);
}

// ---- SEG-131: the player's data of a Genesis game ----

namespace {
bool lowercase_hex(const std::string &text, std::size_t length) {
  if (text.size() != length) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
}  // namespace

fs::path genesis_save_dir(const RomView &rom) {
  if (rom.platform_id != "genesis" || !lowercase_hex(rom.sha256, 64)) return {};
  fs::path dir = library_root() / "saves" / "genesis" / rom.sha256;
  if (rom.luau_mod()) return lowercase_hex(rom.luau_identity, 64) ? dir / "mods" / rom.luau_identity : fs::path();   // SEG-137-T007: a mod has its own data, never Vanilla's
  if (!rom.patched()) return dir / "vanilla";
  if (!lowercase_hex(rom.patch_identity, 64)) return {};
  return dir / "patches" / rom.patch_identity;
}

fs::path sms_save_dir(const RomView &rom) {
  if (rom.platform_id != "master-system" || !lowercase_hex(rom.sha256, 64)) return {};
  std::string family = rom.mapper;
  if (family.empty() && !rom.mapper_manifest.empty() && lowercase_hex(rom.mapper_manifest_sha256, 64)) family = "manifest-" + rom.mapper_manifest_sha256.substr(0, 16);
  if (family.empty() || family.size() > 40) return {};
  for (const char ch : family)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return {};   // a path component of a known shape, never user text
  return library_root() / "saves" / "master-system" / rom.sha256 / family;
}

fs::path game_save_dir(const RomView &rom) { return rom.platform_id == "master-system" ? sms_save_dir(rom) : genesis_save_dir(rom); }

SaveOutcome read_save_outcome(const fs::path &run_log) {
  SaveOutcome outcome;
  std::ifstream in(run_log, std::ios::binary);
  std::string line;
  std::size_t scanned = 0;
  while (in && std::getline(in, line) && ++scanned < 200000) {
    if (line.rfind("VIEWER_PERSIST ", 0) != 0 || line.size() > 4096) continue;
    const auto root = json::parse(line.substr(15));
    if (!root || !root->is_object()) continue;
    const std::string event = root->str("event"), status = root->str("status");
    if (event == "load" && status == "refused") { outcome.load_refused = true; outcome.detail = root->str("detail"); }
    else if (event == "commit" && status == "stored") outcome.committed = true;
    else if (event == "commit" && status == "not_clean_exit") outcome.not_saved = true;
    else if (event == "commit" && status == "kept_previous") { outcome.commit_failed = true; outcome.detail = root->str("detail"); }
  }
  return outcome;
}

// ---- GameRun ----

GameRun::GameRun(const RomView &rom, const Layout &layout, const fs::path &entry, const std::vector<std::string> &extra_args, bool bind_installed_runtime) {
  start(rom, layout, utf8(entry_executable(entry)), entry / "run.log", extra_args, {}, bind_installed_runtime);
}

GameRun::GameRun(const RomView &rom, const Layout &layout, const fs::path &executable, const fs::path &log_file, const std::vector<std::string> &args,
                 const std::vector<std::pair<std::string, std::string>> &extra_environment, bool bind_installed_runtime) {
  start(rom, layout, utf8(executable), log_file, args, extra_environment, bind_installed_runtime);
}

void GameRun::start(const RomView &rom, const Layout &layout, const std::string &exe, const fs::path &log_file, const std::vector<std::string> &extra_args,
                    const std::vector<std::pair<std::string, std::string>> &extra_environment, bool bind_installed_runtime) {
  std::vector<const char *> argv{exe.c_str()};
  for (const auto &arg : extra_args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  SDL_Environment *environment = SDL_CreateEnvironment(true);
  apply_game_environment(environment, rom);
  for (const auto &[name, value] : extra_environment) SDL_SetEnvironmentVariable(environment, name.c_str(), value.c_str(), true);
#if defined(_WIN32)
  // The game links SDL3.dll; find it next to the package rather than requiring it on PATH.
  const char *old_path = SDL_getenv("PATH");
  const std::string path = utf8(layout.sdl_lib) + ";" + (old_path ? old_path : "");
  SDL_SetEnvironmentVariable(environment, "PATH", path.c_str(), true);
#else
  if (bind_installed_runtime) {
#if defined(__APPLE__)
    const char *variable = "DYLD_FALLBACK_LIBRARY_PATH";
#else
    const char *variable = "LD_LIBRARY_PATH";
#endif
    const char *old_value = SDL_getenv(variable);
    const std::string search = utf8(layout.sdl_lib) + (old_value != nullptr && *old_value != '\0' ? std::string(":") + old_value : std::string());
    SDL_SetEnvironmentVariable(environment, variable, search.c_str(), true);
  }
#endif
  (void)bind_installed_runtime;
  SDL_IOStream *log = SDL_IOFromFile(utf8(log_file).c_str(), "wb");
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
