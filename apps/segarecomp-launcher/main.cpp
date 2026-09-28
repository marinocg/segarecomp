// Segarecomp consumer launcher: pick or drop a ROM, recompile it with the packaged toolchain, play it.
// Thin UI over launcher_core; it owns no recompilation logic.

#include "launcher_core.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

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
  bool run = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--build" && i + 1 < argc) rom_path = from_u8(argv[++i]);
    else if (arg == "--report" && i + 1 < argc) report_path = from_u8(argv[++i]);
    else if (arg == "--run") run = true;
    else { std::fprintf(stderr, "usage: Segarecomp [--build <rom> [--run] [--report <file>]]\n"); return 2; }
  }
  std::string report;
  const auto emit = [&](const std::string &line) { report += line + "\n"; std::puts(line.c_str()); };
  int rc = 0;
  const Layout layout = locate_layout();
  emit("layout_root=" + u8s(layout.root));
  emit("compiler=" + u8s(layout.cc));
  if (!layout.problem.empty()) { emit("result=failed problem=" + layout.problem); rc = 1; }
  else {
    const RomView rom = inspect_rom_file(rom_path, layout);
    if (!rom.error.empty()) { emit("result=failed problem=" + rom.error); rc = 1; }
    else {
      const fs::path entry = entry_dir(rom, layout);
      emit("rom_sha256=" + rom.sha256);
      emit(std::string("cache=") + (entry_ready(entry) ? "hit" : "miss"));
      if (!entry_ready(entry)) {
        BuildJob job(rom, layout);
        job.wait();
        emit(std::string("build=") + (job.succeeded() ? "ok" : "failed"));
        if (!job.succeeded()) { emit("message=" + job.message()); rc = 3; }
      }
      emit("entry=" + u8s(entry));
      if (rc == 0 && run) {
        GameRun game(rom, layout, entry);
        if (!game.started()) { emit("run=failed_to_start"); rc = 4; }
        else {
          while (!game.poll()) SDL_Delay(50);
          emit("run=exited code=" + std::to_string(game.exit_code()));
          if (game.exit_code() != 0) rc = 4;
        }
      }
    }
  }
  if (!report_path.empty()) { std::ofstream(report_path, std::ios::binary) << report; }
  return rc;
}

// ---- GUI ----

enum class State { NoRom, RomSelected, Building, Ready, Running, Failed };

struct Pending {
  std::mutex mutex;
  std::string path;   // set by the file dialog / drop, consumed on the UI thread
};

void dialog_callback(void *userdata, const char *const *files, int) {
  if (!files || !files[0]) return;
  auto *pending = static_cast<Pending *>(userdata);
  std::lock_guard<std::mutex> lock(pending->mutex);
  pending->path = files[0];
}

void centered_text(const char *text) {
  const float width = ImGui::CalcTextSize(text).x;
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5F);
  ImGui::TextUnformatted(text);
}

bool centered_button(const char *label, float width) {
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5F);
  return ImGui::Button(label, ImVec2(width, 0));
}

void style_app(float scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 0; style.FrameRounding = 8; style.GrabRounding = 8; style.ChildRounding = 12;
  style.FramePadding = ImVec2(14, 9); style.ItemSpacing = ImVec2(10, 12); style.WindowPadding = ImVec2(28, 24);
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09F, 0.10F, 0.13F, 1);
  style.Colors[ImGuiCol_Button] = ImVec4(0.16F, 0.36F, 0.85F, 1);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22F, 0.44F, 0.95F, 1);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.12F, 0.30F, 0.75F, 1);
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
  style.FontSizeBase = 17.0F;
}

int gui() {
  if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }
  SDL_Window *window = SDL_CreateWindow("Segarecomp", 720, 620, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) { std::fprintf(stderr, "window failed: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  style_app(SDL_GetWindowDisplayScale(window));
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  const Layout layout = locate_layout();
  Pending pending;
  State state = State::NoRom;
  RomView rom;
  fs::path entry;
  std::string failure;
  bool show_diagnostics = false;
  std::string diagnostics;
  std::unique_ptr<BuildJob> job;
  std::unique_ptr<GameRun> game;

  const auto select_rom = [&](const std::string &path_text) {
    if (state == State::Building || state == State::Running) return;
    rom = inspect_rom_file(from_u8(path_text.c_str()), layout);
    show_diagnostics = false;
    if (!rom.error.empty()) { failure = "That file could not be read: " + rom.error; diagnostics = failure; state = State::Failed; return; }
    entry = entry_dir(rom, layout);
    state = entry_ready(entry) ? State::Ready : State::RomSelected;
  };
  const auto start_build = [&](bool force) {
    if (force) { std::error_code ec; fs::remove_all(entry, ec); }
    job = std::make_unique<BuildJob>(rom, layout);
    state = State::Building;
  };
  const auto browse = [&] {
    static const SDL_DialogFileFilter filters[] = {{"Genesis / Mega Drive ROMs", "md;bin;gen;smd"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(dialog_callback, &pending, window, filters, 2, nullptr, false);
  };

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) running = false;
      if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) select_rom(event.drop.data);
    }
    {
      std::string chosen;
      { std::lock_guard<std::mutex> lock(pending.mutex); chosen.swap(pending.path); }
      if (!chosen.empty()) select_rom(chosen);
    }
    if (state == State::Building && job->finished()) {
      if (job->succeeded()) state = State::Ready;
      else {
        failure = job->message();
        std::error_code ec;
        diagnostics = read_tail(entry.parent_path() / (entry.filename().string() + ".failed") / "build.log", 24000);
        state = State::Failed;
      }
      job->wait();
    }
    if (state == State::Running && game->poll()) {
      if (game->exit_code() != 0) {
        failure = "The game stopped unexpectedly.";
        diagnostics = read_tail(entry / "run.log", 24000);
        state = State::Failed;
      } else state = State::Ready;
      game.reset();
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.9F * scale);
    centered_text("SEGARECOMP");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65F, 0.68F, 0.75F, 1));
    centered_text("Recompile Sega games to native code.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const float button_width = 260.0F * scale;
    if (state == State::NoRom) {
      ImGui::BeginChild("drop", ImVec2(0, 230.0F * scale), ImGuiChildFlags_Borders);
      ImGui::Dummy(ImVec2(0, 50.0F * scale));
      centered_text("Drop a ROM here"); centered_text("or");
      if (centered_button("Browse...", 180.0F * scale)) browse();
      ImGui::EndChild();
    } else {
      ImGui::BeginChild("game", ImVec2(0, 230.0F * scale), ImGuiChildFlags_Borders);
      ImGui::Dummy(ImVec2(0, 8.0F * scale));
      centered_text(rom.title.c_str());
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65F, 0.68F, 0.75F, 1));
      centered_text(rom.platform.c_str());
      if (rom.supported_platform) centered_text(rom.compat_known ? "Experimental compatibility" : "Compatibility unknown");
      ImGui::PopStyleColor();
      ImGui::Spacing();
      if (!layout.problem.empty()) {
        ImGui::TextWrapped("%s", layout.problem.c_str());
      } else if (state == State::RomSelected) {
        if (!rom.supported_platform) { centered_text("This file is not a supported Genesis / Mega Drive ROM."); }
        if (centered_button(rom.compat_known ? "Recompile & Play" : "Try anyway", button_width)) start_build(false);
      } else if (state == State::Building) {
        static const char *names[BuildJob::stage_count] = {"Analyzing ROM", "Generating native C", "Compiling", "Linking"};
        centered_text("Preparing game...");
        for (int i = 0; i < BuildJob::stage_count; ++i) {
          const StageState s = job->stage(i);
          char line[96];
          std::snprintf(line, sizeof line, "%s %s", names[i], s == StageState::done ? "[done]" : s == StageState::running ? "..." : "");
          centered_text(line);
        }
        centered_text("Large games can take several minutes the first time.");
      } else if (state == State::Ready) {
        centered_text("Native build ready");
        if (centered_button("Play", button_width)) {
          game = std::make_unique<GameRun>(rom, layout, entry);
          if (game->started()) state = State::Running;
          else { failure = "The game could not be started."; diagnostics = failure; game.reset(); state = State::Failed; }
        }
        if (centered_button("Recompile", button_width)) start_build(true);
      } else if (state == State::Running) {
        centered_text("Playing... close the game window to return.");
      } else if (state == State::Failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.55F, 0.5F, 1));
        ImGui::TextWrapped("%s", failure.c_str());
        ImGui::PopStyleColor();
        if (rom.error.empty() && ImGui::Button("Try again")) start_build(true);
        ImGui::SameLine();
        if (ImGui::Button(show_diagnostics ? "Hide diagnostics" : "View diagnostics")) show_diagnostics = !show_diagnostics;
      }
      ImGui::EndChild();
      if (state != State::Building && state != State::Running && ImGui::Button("Choose another ROM...")) browse();
    }
    if (show_diagnostics && state == State::Failed) {
      ImGui::BeginChild("diag", ImVec2(0, 150.0F * scale), ImGuiChildFlags_Borders);
      ImGui::TextUnformatted(diagnostics.c_str());
      ImGui::EndChild();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55F, 0.58F, 0.65F, 1));
    ImGui::TextWrapped("No ROMs are included with Segarecomp. Use only software you are legally entitled to analyze. "
                       "Compatibility is experimental.");
    ImGui::PopStyleColor();
    if (ImGui::CollapsingHeader("Advanced")) {
      ImGui::Text("Segarecomp %s   |   compiler: zig %s   |   SDL %d.%d.%d   |   %s", SEGARECOMP_LAUNCHER_VERSION,
                  SEGARECOMP_ZIG_VERSION, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                  SDL_VERSIONNUM_MICRO(SDL_GetVersion()), host_description().c_str());
      const std::string cache = u8s(cache_root());
      ImGui::TextWrapped("Cache: %s", cache.c_str());
      if (ImGui::Button("Open cache folder")) SDL_OpenURL(("file://" + cache).c_str());
    }
    ImGui::End();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 23, 26, 33, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }
  game.reset();
  job.reset();
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1) return headless(argc, argv);
  return gui();
}
