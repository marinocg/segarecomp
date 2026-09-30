#include "launcher_core.hpp"

#include "segarecomp/rom.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

#ifndef SEGARECOMP_LAUNCHER_VERSION
#define SEGARECOMP_LAUNCHER_VERSION "dev"
#endif
#ifndef SEGARECOMP_ZIG_VERSION
#define SEGARECOMP_ZIG_VERSION "unknown"
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
  return "aarch64-linux-gnu.2.35";
#else
  return "x86_64-linux-gnu.2.35";
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
                               layout.runtime_sms / "viewer" / "sms_viewer_sdl3.c", layout.sdl_include / "SDL3" / "SDL.h"}) {
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
      view.title = utf8(path.stem());
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
    view.title = info.domestic_title_display;
    while (!view.title.empty() && view.title.back() == ' ') view.title.pop_back();
    if (view.title.empty()) view.title = utf8(path.stem());
    std::error_code ec;
    view.compat_known = fs::is_regular_file(layout.runtime / "compat" / (view.sha256 + ".json"), ec);
  } catch (const std::exception &error) {
    view.error = error.what();
  }
  return view;
}

// Genesis keeps its historical key segment; Master System adds the platform, profile and declared mapper (and the sidecar
// manifest's content digest), so a different declaration or manifest always selects a fresh cache entry.
std::string platform_key(const RomView &rom) {
  if (rom.platform_id != "master-system") return "genesis";
  return std::string("master-system:") + sms_profile + ":mapper=" + rom.mapper + ":manifest=" + rom.mapper_manifest_sha256;
}

fs::path entry_dir(const RomView &rom, const Layout &layout) {
  // Everything that changes the produced binary is part of the key; a mismatch selects a fresh directory.
  // The build driver itself (which embeds the code generator) is part of the key: a different segarecomp
  // build must never reuse a program generated by another one, even at the same version string.
  std::error_code ec;
  const auto cli_size = fs::is_regular_file(layout.cli, ec) ? fs::file_size(layout.cli, ec) : 0U;
  const auto cli_time = fs::last_write_time(layout.cli, ec).time_since_epoch().count();
  const std::string key = rom.sha256 + "|cli-" + std::to_string(static_cast<unsigned long long>(cli_size)) + "-" + std::to_string(static_cast<long long>(cli_time)) + "|" SEGARECOMP_LAUNCHER_VERSION "|" + platform_key(rom) + "|" + host_arch() + "|" +
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

GameRun::GameRun(const RomView &, const Layout &layout, const fs::path &entry, const std::vector<std::string> &extra_args) {
  const std::string exe = utf8(entry_executable(entry));
  std::vector<const char *> argv{exe.c_str()};
  for (const auto &arg : extra_args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  SDL_Environment *environment = SDL_CreateEnvironment(true);
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
