// Segarecomp consumer launcher: pick or drop a ROM, recompile it with the packaged toolchain, play it.
// Thin UI over launcher_core; it owns no recompilation logic.

#include "launcher_core.hpp"
#include "library_catalog.hpp"
#include "rom_zip.hpp"
#include "sega_cd.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "sv_input_diag.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <functional>
#include <future>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace launcher;

#ifndef SEGARECOMP_LAUNCHER_VERSION
#define SEGARECOMP_LAUNCHER_VERSION "dev"
#endif
#ifndef SEGARECOMP_ZIG_VERSION
#define SEGARECOMP_ZIG_VERSION "unknown"
#endif

namespace {

std::string u8s(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

fs::path from_u8(const char *text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text))); }

// ---- non-interactive mode (package smoke test): same core, no window ----

int headless(int argc, char **argv) {
  fs::path rom_path, report_path;
  std::string mapper;                  // Master System: explicit mapper declaration (never defaulted)
  std::string aot_policy = "compatibility";  // compatibility | optimized (Genesis); Compatibility is the default
  std::string aot_model = "v1";              // experimental opt-in of Optimized (v1 | gen2); forwarded verbatim to the product
  bool hint_discovery_arg = false;     // --hint-alias-discovery: advanced strict-H-interrupt preparation (default off)
  std::string region_arg;              // --region <usa|japan>: console region (run time, not part of the build identity)
  std::vector<std::string> game_args;  // forwarded to the launched game (automation passes a finite bound)
  std::vector<fs::path> imports;       // --import <rom>: add to the library without building
  bool run = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--build" && i + 1 < argc) rom_path = from_u8(argv[++i]);
    else if (arg == "--report" && i + 1 < argc) report_path = from_u8(argv[++i]);
    else if (arg == "--import" && i + 1 < argc) imports.push_back(from_u8(argv[++i]));
    else if (arg == "--run") run = true;
    else if (arg == "--mapper" && i + 1 < argc) mapper = argv[++i];
    else if (arg == "--aot-policy" && i + 1 < argc && (std::string(argv[i + 1]) == "compatibility" || std::string(argv[i + 1]) == "optimized")) aot_policy = argv[++i];
    else if (arg == "--aot-model" && i + 1 < argc && (std::string(argv[i + 1]) == "v1" || std::string(argv[i + 1]) == "gen2")) aot_model = argv[++i];
    else if (arg == "--region" && i + 1 < argc && (std::string(argv[i + 1]) == "usa" || std::string(argv[i + 1]) == "japan" || std::string(argv[i + 1]) == "europe")) region_arg = argv[++i];
    else if (arg == "--hint-alias-discovery") hint_discovery_arg = true;
    else if (arg == "--game-arg" && i + 1 < argc) game_args.emplace_back(argv[++i]);
    else { std::fprintf(stderr, "usage: Segarecomp [--import <rom>]... [--build <rom> [--mapper <sega|rom_only>] [--aot-policy <compatibility|optimized> [--aot-model <v1|gen2>]] [--hint-alias-discovery] [--run [--region <usa|japan|europe>] [--game-arg <arg>]...] [--report <file>]]\n"); return 2; }
  }
  if (aot_model != "v1" && aot_policy != "optimized") { std::fprintf(stderr, "--aot-model requires --aot-policy optimized\n"); return 2; }
  std::string report;
  const auto emit = [&](const std::string &line) { report += line + "\n"; std::puts(line.c_str()); };
  if (!imports.empty()) {
    // Add ROMs to the persistent library without building (never modifies them; a ZIP's ROM is extracted into the library). Same classification as the window.
    const Layout import_layout = locate_layout();
    Catalog catalog(library_root() / "library.json");
    catalog.load();
    int bad = 0;
    for (const auto &path : imports) {
      const RomView view = inspect_rom_or_zip(path, import_layout, library_root() / "roms");
      const auto result = catalog.import_rom(view, static_cast<long long>(std::time(nullptr)));
      if (result.entry == nullptr) { emit("import=failed path=" + u8s(path) + " reason=" + result.error); ++bad; }
      else emit(std::string("import=") + (result.added ? "added" : (result.path_added ? "location" : "duplicate")) + " id=" + result.entry->id() + " title=" + result.entry->title());
    }
    std::string save_error;
    if (!catalog.save(&save_error)) { emit("import=failed reason=" + save_error); bad = 1; }
    if (rom_path.empty()) { if (!report_path.empty()) { std::ofstream(report_path, std::ios::binary) << report; } return bad == 0 ? 0 : 1; }
  }
  int rc = 0;
  const Layout layout = locate_layout();
  emit("layout_root=" + u8s(layout.root));
  emit("compiler=" + u8s(layout.cc));
  if (!layout.problem.empty()) { emit("result=failed problem=" + layout.problem); rc = 1; }
  else {
    RomView rom = inspect_rom_or_zip(rom_path, layout, library_root() / "roms");
    if (rom.platform_id == "master-system" && !mapper.empty()) rom.mapper = mapper;
    // The product CLI states whether the experimental Master System Optimized policy exists; a request for it is honoured only then.
    if (rom.platform_id == "master-system" && aot_model != "v1") { emit("result=failed problem=--aot-model is a Genesis option"); rc = 2; }
    if (rc == 0 && rom.platform_id == "master-system" && aot_policy == "optimized") rom.sms_optimized_available = sms_optimized_available_blocking(layout);
    if (rom.supports_optimized()) { rom.aot_policy = aot_policy; if (rom.platform_id == "genesis") rom.aot_model = aot_model; }
    else if (aot_policy == "optimized") emit("aot_policy_unavailable=Optimized is not available for this platform (building Compatibility)");
    if (!region_arg.empty() && rom.supports_region()) rom.region = region_arg;
    if (hint_discovery_arg && rom.supports_hint_alias_discovery()) rom.hint_alias_discovery = true;
    if (rc != 0) {}
    else if (!rom.error.empty()) { emit("result=failed problem=" + rom.error); rc = 1; }
    else {
      emit("rom_sha256=" + rom.sha256);
      emit("platform=" + (rom.platform_id.empty() ? std::string("unsupported") : rom.platform_id));
      if (rom.platform_id == "master-system")
        emit(std::string("mapper=") + (!rom.mapper.empty() ? rom.mapper : rom.mapper_manifest.empty() ? "undeclared" : "manifest"));
      // Build (or reuse) the entry of `view` and report its machine-readable policy; returns the entry or an empty path on failure.
      const auto prepare = [&](const RomView &view, const char *tag) -> fs::path {
        const fs::path entry = entry_dir(view, layout);
        emit(std::string(tag) + "aot_policy_requested=" + view.aot_policy);
        if (view.optimized() && view.platform_id == "genesis") emit(std::string(tag) + "aot_policy_requested_model=" + view.aot_model);
        const bool cached = entry_ready_for(entry, view, layout);
        emit(std::string(tag) + "cache=" + (cached ? "hit" : "miss"));
        if (view.optimized() && rom_policy_identity(layout, view).empty())
          emit(std::string(tag) + "cache_note=policy_identity_unavailable (cached Optimized builds are not reused; this build is fresh)");
        if (!cached) {
          BuildJob job(view, layout);
          job.wait();
          emit(std::string(tag) + "build=" + (job.succeeded() ? "ok" : "failed"));
          if (!job.succeeded()) {
            emit(std::string(tag) + "message=" + job.message());
            emit(std::string(tag) + "entry=" + u8s(entry));  // as before: the (absent) entry is still reported after a failed build
            return {};
          }
        }
        emit(std::string(tag) + "entry=" + u8s(entry));
        const PolicyReport policy = read_policy_report(entry);
        if (policy.present) {
          emit(std::string(tag) + "aot_policy_effective=" + policy.effective);
          emit(std::string(tag) + "aot_policy_fallback=" + (policy.fallback ? "yes" : "no"));
          emit(std::string(tag) + "aot_policy_reason=" + policy.reason);
          emit(std::string(tag) + "aot_policy_fallback_class=" + policy.fallback_class);
          emit(std::string(tag) + "aot_policy_model_family=" + policy.model_family);
          emit(std::string(tag) + "aot_policy_validator=" + policy.validator);
          emit(std::string(tag) + "aot_policy_selected_ppm=" + std::to_string(policy.selected_ppm));
          emit(std::string(tag) + "aot_policy_admitted_ppm=" + std::to_string(policy.admitted_ppm));
          emit(std::string(tag) + "aot_policy_policy_identity=" + policy.policy_identity);
          emit(std::string(tag) + "aot_policy_identity=" + policy.identity);
        }
        return entry;
      };
      fs::path entry = prepare(rom, "");
      if (entry.empty()) rc = 3;
      if (rom.supports_region()) emit("region=" + rom.region);
      if (rc == 0 && run && !launch_refusal(rom).empty()) { emit("run=refused reason=" + launch_refusal(rom)); rc = 5; }
      else if (rc == 0 && run) {
        // Run; a Master System Optimized program that stops on the typed unadmitted_pc outcome restarts ONCE, from reset, in Compatibility.
        RomView current = rom;
        bool restarted = false;
        for (;;) {
          GameRun game(current, layout, entry, game_args);
          if (!game.started()) { emit("run=failed_to_start"); rc = 4; break; }
          while (!game.poll()) SDL_Delay(50);
          emit("run=exited code=" + std::to_string(game.exit_code()));  // the historical line, unchanged
          const PolicyReport ran = read_policy_report(entry);
          emit("run_requested_mode=" + current.aot_policy);  // what was asked for
          emit("run_mode=" + effective_mode(ran));           // what actually ran (a build-time fallback runs Compatibility)
          const RestartDecision next = decide_after_exit(running_view(current, ran), game.exit_code(), restarted);
          if (!next.restart) { if (game.exit_code() != 0) rc = 4; break; }
          emit("run=unadmitted_pc");
          emit("fallback=compatibility_restart");
          emit("notice=" + next.notice);
          restarted = true;
          current = next.next;
          entry = prepare(current, "fallback_");
          if (entry.empty()) { rc = 3; break; }
        }
      }
    }
  }
  if (!report_path.empty()) { std::ofstream(report_path, std::ios::binary) << report; }
  return rc;
}

// ---- SEG-078-T002 (ADR 0204): the Sega CD flow without a window (package smoke test, automation): the same core as the window.
//   Segarecomp --sega-cd --bios <file> --cue <file> [--program <viewer program>] [--record <script> | --replay <script>] [--windowless] [--report <file>]
// Adds the game to the library (by reference), verifies the program against the BIOS and disc, and - when a program was given and accepted - runs it.
// Output: `key=value` lines. Exit: 0 ran and closed / finished, 1 usage-level failure, 3 the inputs were refused, 4 the program ended with a failure status,
// 5 the program was refused before launch, 6 the program could not be started.
int sega_cd_headless(int argc, char **argv) {
  fs::path bios, cue, program, record, replay, report_path;
  bool windowless = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--bios" && i + 1 < argc) bios = from_u8(argv[++i]);
    else if (arg == "--cue" && i + 1 < argc) cue = from_u8(argv[++i]);
    else if (arg == "--program" && i + 1 < argc) program = from_u8(argv[++i]);
    else if (arg == "--record" && i + 1 < argc) record = from_u8(argv[++i]);
    else if (arg == "--replay" && i + 1 < argc) replay = from_u8(argv[++i]);
    else if (arg == "--report" && i + 1 < argc) report_path = from_u8(argv[++i]);
    else if (arg == "--windowless") windowless = true;
    else { std::fprintf(stderr, "usage: Segarecomp --sega-cd --bios <file> --cue <file> [--program <file>] [--record <script> | --replay <script>] [--windowless] [--report <file>]\n"); return 2; }
  }
  if (!record.empty() && !replay.empty()) { std::fprintf(stderr, "--record and --replay are exclusive\n"); return 2; }
  std::string report;
  const auto emit = [&](const std::string &line) { report += line + "\n"; std::puts(line.c_str()); };
  const auto finish = [&](int rc) { if (!report_path.empty()) std::ofstream(report_path, std::ios::binary) << report; return rc; };
  Catalog catalog(library_root() / "library.json");
  catalog.load();
  scd::ImportRequest request;
  request.bios = bios; request.cue = cue; request.program = program;
  const scd::ImportResult imported = scd::import_entry(request);
  if (imported.record.bios_sha256.empty() || imported.record.cue_sha256.empty()) {
    emit(std::string("result=refused refusal=") + scd::code(imported.refusal) + " detail=" + imported.detail);
    return finish(3);
  }
  const auto added = catalog.import_sega_cd(imported.record, imported.title, static_cast<long long>(std::time(nullptr)));
  std::string save_error;
  if (added.entry == nullptr || !catalog.save(&save_error)) { emit("result=failed problem=" + (added.entry == nullptr ? added.error : save_error)); return finish(1); }
  emit("platform=sega-cd");
  emit("entry=" + added.entry->id());
  emit(std::string("added=") + (added.added ? "yes" : "no"));
  emit(std::string("program=") + (imported.program_attached ? "verified" : (program.empty() ? "none" : std::string("refused ") + scd::code(imported.refusal))));
  const scd::EntryRecord &stored = added.entry->sega_cd;
  const scd::Status quick = scd::quick_status(stored);
  emit(std::string("state=") + scd::state_label(quick.state));
  emit(std::string("refusal=") + scd::code(quick.refusal));
  if (quick.state != scd::State::ready) {
    if (quick.state == scd::State::no_program) emit(std::string("build=") + scd::code(scd::Refusal::unsupported_build));
    return finish(quick.state == scd::State::no_program && imported.refusal == scd::Refusal::none ? 0 : 5);
  }
  const scd::Plan plan = scd::prepare_launch(stored, false);
  if (plan.status.state != scd::State::ready) { emit(std::string("result=refused refusal=") + scd::code(plan.status.refusal)); return finish(5); }
  scd::RunOptions options;
  const fs::path dir = library_root() / "sega-cd" / added.entry->sha256.substr(0, 16);
  std::error_code ec;
  fs::create_directories(dir, ec);
  options.report = dir / "last-report.json";
  options.record_to = record;
  options.replay_from = replay;
  fs::remove(options.report, ec);
  std::vector<std::pair<std::string, std::string>> environment;
  if (windowless) environment = {{"SDL_VIDEODRIVER", "dummy"}, {"SDL_AUDIODRIVER", "dummy"}, {"SEGARECOMP_VIEWER_UNTHROTTLED", "1"}, {"SEGARECOMP_VIEWER_NO_STOP_SCREEN", "1"}};
  const Layout layout = locate_layout();
  GameRun game(view_from_entry(*added.entry), layout, plan.program, dir / "run.log", scd::run_arguments(plan, options), environment, true);
  if (!game.started()) { emit("run=failed_to_start"); return finish(6); }
  while (!game.poll()) SDL_Delay(50);
  const scd::Outcome outcome = scd::read_outcome(options.report, game.exit_code());
  emit("run=exited code=" + std::to_string(game.exit_code()));
  emit("outcome=" + scd::describe_outcome(outcome));
  if (!outcome.stop_class.empty()) emit("stop=" + outcome.stop_class);
  if (!outcome.live_outcome.empty()) emit("live=" + outcome.live_outcome + " frames=" + std::to_string(outcome.frames) + " presented=" + std::to_string(outcome.presented) + " input_events=" + std::to_string(outcome.input_events));
  return finish(game.exit_code() == 0 || game.exit_code() == 10 || game.exit_code() == 11 ? 0 : 4);
}

// ---- `Segarecomp --input-report`: the input diagnostics without a window (package smoke test, support) ----
// Prints which SDL library this process really loaded, what that build declares about the Switch 2 USB driver, whether libusb can be loaded the way
// SDL loads it, and the same copyable summary the Input diagnostics page shows. It opens no window and never touches a ROM.
int input_report() {
  std::string library = "unknown";
#if defined(_WIN32)
  if (HMODULE module = GetModuleHandleW(L"SDL3.dll")) {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(module, buffer, static_cast<DWORD>(sizeof buffer / sizeof buffer[0]));
    if (n > 0 && n < sizeof buffer / sizeof buffer[0]) library = u8s(fs::path(std::wstring(buffer, n)));
  }
#else
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(&SDL_Init), &info) != 0 && info.dli_fname != nullptr) library = info.dli_fname;
#endif
  const bool ok = SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
  SvDiag *diag = sv_diag_create();
  sv_diag_note_init(diag, 1, ok ? 1 : 0, ok ? nullptr : SDL_GetError());
  if (ok) sv_diag_scan(diag);
  char text[2600];
  (void)sv_diag_summary(diag, text, sizeof text);
  std::printf("sdl_library=%s\nsdl_revision=%s\nswitch2_driver_declared=%s\nlibusb_loadable=%s\n%s", library.c_str(), SDL_GetRevision(),
              sv_diag_switch2_driver_declared() ? "yes" : "no", sv_diag_libusb_available(diag) ? "yes" : "no", text);
  sv_diag_destroy(diag);
  SDL_Quit();
  return ok ? 0 : 1;
}

// ---- GUI: see app.cpp (library, details, import, storage, settings) ----

} // namespace

int gui_main();

#if defined(_WIN32)
namespace {
// Windows creates a window's decorated frame at the real per-monitor DPI regardless of the process's
// declared awareness, but only hands back correct point/pixel sizes -- and paints the whole client area --
// once the process has opted into per-monitor-v2 awareness. Without it, a scaled (e.g. 4K) display can leave
// part of the window unpainted (observed: small, correctly-proportioned content pinned to one corner, the
// rest of the frame showing whatever was behind the window). Resolved dynamically (GetProcAddress) rather
// than through a specific mingw-w64 SDK header vintage, with an older-Windows fallback chain; safe to call
// even when a manifest already declared awareness (the redundant call is simply ignored by Windows).
void enable_dpi_awareness() {
  if (HMODULE user32 = LoadLibraryA("user32.dll")) {
    using SetContextFn = BOOL(WINAPI *)(void *);
    void *per_monitor_v2 = reinterpret_cast<void *>(static_cast<std::intptr_t>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    auto *set_context = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    const bool ok = set_context && set_context(per_monitor_v2);
    FreeLibrary(user32);
    if (ok) return;
  }
  if (HMODULE shcore = LoadLibraryA("shcore.dll")) {
    using SetAwarenessFn = HRESULT(WINAPI *)(int);
    if (auto *set_awareness = reinterpret_cast<SetAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness")))
      set_awareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */);
    FreeLibrary(shcore);
    return;
  }
  SetProcessDPIAware();  // last-resort fallback, present since Windows Vista
}

// The GUI-subsystem launcher has no console, so every console-subsystem child it starts (the build driver, the bundled compiler, curl,
// the product tools) would get a NEW visible console window and pop up around the screen. Owning one hidden console makes every child
// inherit it instead. Skipped when the process already has a console (started from a terminal): that console is the user's own.
void hide_child_console_windows() {
  if (GetConsoleWindow() != nullptr) return;
  if (!AllocConsole()) return;
  if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
}
} // namespace
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  enable_dpi_awareness();
#endif
  if (argc == 2 && std::string(argv[1]) == "--input-report") return input_report();
  if (argc > 1 && std::string(argv[1]) == "--sega-cd") return sega_cd_headless(argc, argv);
  if (argc > 1) return headless(argc, argv);
#if defined(_WIN32)
  hide_child_console_windows();
#endif
  return gui_main();
}
