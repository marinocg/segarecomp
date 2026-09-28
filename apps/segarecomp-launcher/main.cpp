// Segarecomp consumer launcher: pick or drop a ROM, recompile it with the packaged toolchain, play it.
// Thin UI over launcher_core; it owns no recompilation logic.

#include "launcher_core.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <cmath>
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

constexpr float window_w = 560.0F;   // logical size at 100% scale; the window is fixed
constexpr float window_h = 770.0F;

float g_scale = 1.0F;
float S(float v) { return v * g_scale; }

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// Decorative full-window background: gradient, soft circles, rings, speed stripes and a dot grid.
void draw_background(ImDrawList *dl, float t) {
  const float w = S(window_w), h = S(window_h);
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), rgba(9, 11, 24), rgba(9, 11, 24), rgba(27, 14, 48), rgba(20, 10, 40));
  const float drift = std::sin(t * 0.35F), drift2 = std::cos(t * 0.27F);
  dl->AddCircleFilled(ImVec2(w * 0.10F + S(10) * drift, h * 0.08F), w * 0.55F, rgba(47, 107, 255, 34), 96);
  dl->AddCircleFilled(ImVec2(w * 1.02F, h * 0.50F + S(14) * drift2), w * 0.58F, rgba(214, 51, 132, 30), 96);
  dl->AddCircleFilled(ImVec2(w * 0.05F, h * 1.00F), w * 0.50F, rgba(20, 200, 220, 26), 96);
  dl->AddCircleFilled(ImVec2(w * 0.85F, h * 0.06F), w * 0.16F, rgba(255, 190, 40, 22), 64);
  for (int i = 0; i < 4; ++i)
    dl->AddCircle(ImVec2(w * 0.10F + S(10) * drift, h * 0.08F), w * (0.66F + 0.09F * static_cast<float>(i)),
                  rgba(120, 160, 255, 26 - i * 5), 128, S(1.5F));
  // Speed stripes rising from the bottom-right corner.
  static const ImU32 stripe[] = {rgba(47, 107, 255, 46), rgba(214, 51, 132, 40), rgba(255, 190, 40, 34), rgba(20, 200, 220, 34)};
  for (int i = 0; i < 4; ++i) {
    const float x = w * (0.52F + 0.13F * static_cast<float>(i)) + S(6) * drift;
    dl->AddQuadFilled(ImVec2(x, h), ImVec2(x + w * 0.09F, h), ImVec2(x + w * 0.09F + h * 0.16F, h * 0.80F),
                      ImVec2(x + h * 0.16F, h * 0.80F), stripe[i]);
  }
  for (int gy = 0; gy < 9; ++gy)
    for (int gx = 0; gx < 6; ++gx)
      dl->AddCircleFilled(ImVec2(S(26) + S(16) * static_cast<float>(gx), h - S(120) + S(16) * static_cast<float>(gy)), S(1.6F),
                          rgba(255, 255, 255, 30));
}

// Logo: a play triangle inside a ring made of two arcs (recompile loop) plus a faux-bold letter-spaced wordmark.
void draw_logo(ImDrawList *dl, float cx, float cy, float t) {
  const float r = S(38);
  dl->AddCircleFilled(ImVec2(cx, cy), r + S(10), rgba(47, 107, 255, 40), 64);
  dl->AddCircleFilled(ImVec2(cx, cy), r, rgba(28, 64, 170), 64);
  dl->AddCircle(ImVec2(cx, cy), r, rgba(120, 170, 255), 64, S(2));
  const float a0 = t * 0.9F;
  dl->PathArcTo(ImVec2(cx, cy), r - S(8), a0, a0 + 2.4F, 32);
  dl->PathStroke(rgba(20, 220, 235), 0, S(4));
  dl->PathArcTo(ImVec2(cx, cy), r - S(8), a0 + 3.14159F, a0 + 3.14159F + 2.4F, 32);
  dl->PathStroke(rgba(255, 190, 40), 0, S(4));
  dl->AddTriangleFilled(ImVec2(cx - S(9), cy - S(13)), ImVec2(cx - S(9), cy + S(13)), ImVec2(cx + S(15), cy), rgba(255, 255, 255));

  const char *word = "SEGARECOMP";
  const float size = S(34), spacing = S(5);
  float total = 0;
  for (const char *c = word; *c; ++c) total += ImGui::GetFont()->CalcTextSizeA(size, 1e9F, 0, c, c + 1).x + spacing;
  total -= spacing;
  float x = cx - total * 0.5F;
  const float y = cy + r + S(20);
  for (const char *c = word; *c; ++c) {
    for (int dx = 0; dx <= 1; ++dx)   // faux bold
      dl->AddText(nullptr, size, ImVec2(x + S(0.9F) * static_cast<float>(dx), y), rgba(245, 247, 255), c, c + 1);
    x += ImGui::GetFont()->CalcTextSizeA(size, 1e9F, 0, c, c + 1).x + spacing;
  }
}

void centered_text(const char *text, ImU32 color = 0) {
  const float width = ImGui::CalcTextSize(text).x;
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5F);
  if (color) ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(text);
  if (color) ImGui::PopStyleColor();
}

bool primary_button(const char *label, float width) {
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5F);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(20), S(14)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(16));
  ImGui::PushStyleColor(ImGuiCol_Button, rgba(47, 107, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(76, 132, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(30, 84, 220));
  const bool pressed = ImGui::Button(label, ImVec2(width, 0));
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);
  return pressed;
}

bool secondary_button(const char *label, float width) {
  ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5F);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(16), S(10)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(14));
  ImGui::PushStyleColor(ImGuiCol_Button, rgba(255, 255, 255, 22));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(255, 255, 255, 44));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(255, 255, 255, 30));
  const bool pressed = ImGui::Button(label, ImVec2(width, 0));
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);
  return pressed;
}

// Stage row icon: done = green disc with a check, running = spinning arc, pending = hollow ring.
void stage_row(const char *label, StageState state, float t) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float row = S(22), width = ImGui::GetWindowWidth();
  const ImVec2 c(origin.x + width * 0.5F - S(105), origin.y + row * 0.5F);
  const float r = S(9);
  if (state == StageState::done) {
    dl->AddCircleFilled(c, r, rgba(52, 199, 120), 24);
    dl->PathLineTo(ImVec2(c.x - r * 0.45F, c.y)); dl->PathLineTo(ImVec2(c.x - r * 0.1F, c.y + r * 0.4F));
    dl->PathLineTo(ImVec2(c.x + r * 0.5F, c.y - r * 0.35F));
    dl->PathStroke(rgba(255, 255, 255), 0, S(2));
  } else if (state == StageState::running) {
    dl->AddCircle(c, r, rgba(255, 255, 255, 40), 24, S(2));
    dl->PathArcTo(c, r, t * 6.0F, t * 6.0F + 1.9F, 16);
    dl->PathStroke(rgba(76, 150, 255), 0, S(2.5F));
  } else {
    dl->AddCircle(c, r, rgba(255, 255, 255, 50), 24, S(1.5F));
  }
  const ImU32 color = state == StageState::pending ? rgba(140, 146, 165) : rgba(235, 238, 250);
  dl->AddText(ImVec2(c.x + r + S(14), origin.y + (row - ImGui::GetFontSize()) * 0.5F), color, label);
  ImGui::Dummy(ImVec2(0, row));
}

// Dashed rounded rectangle for the drop target; brighter while a file is dragged over the window.
void dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness) {
  const float dash = S(9), gap = S(7);
  for (float x = a.x + S(16); x < b.x - S(16); x += dash + gap) {
    dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + dash, b.x - S(16)), a.y), color, thickness);
    dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + dash, b.x - S(16)), b.y), color, thickness);
  }
  for (float y = a.y + S(16); y < b.y - S(16); y += dash + gap) {
    dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + dash, b.y - S(16))), color, thickness);
    dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + dash, b.y - S(16))), color, thickness);
  }
  const float r = S(16);
  dl->PathArcTo(ImVec2(a.x + r, a.y + r), r, 3.14159F, 4.71239F, 8); dl->PathStroke(color, 0, thickness);
  dl->PathArcTo(ImVec2(b.x - r, a.y + r), r, 4.71239F, 6.28318F, 8); dl->PathStroke(color, 0, thickness);
  dl->PathArcTo(ImVec2(b.x - r, b.y - r), r, 0, 1.5708F, 8); dl->PathStroke(color, 0, thickness);
  dl->PathArcTo(ImVec2(a.x + r, b.y - r), r, 1.5708F, 3.14159F, 8); dl->PathStroke(color, 0, thickness);
}

int gui() {
  if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }
  g_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (g_scale < 1.0F) g_scale = 1.0F;
  // Fixed-size window (not resizable): the whole window is the composition.
  SDL_Window *window = SDL_CreateWindow("Segarecomp", static_cast<int>(S(window_w)), static_cast<int>(S(window_h)),
                                        SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) { std::fprintf(stderr, "window failed: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(0, 0); style.ItemSpacing = ImVec2(S(10), S(10)); style.ChildRounding = S(20);
  style.FontScaleDpi = g_scale;
  style.FontSizeBase = 18.0F;
  // Prefer the platform UI font when present (never redistributed); fall back to ImGui's built-in font.
  for (const char *candidate : {"/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Supplemental/Arial.ttf",
                                "C:\\Windows\\Fonts\\segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"}) {
    if (SDL_GetPathInfo(candidate, nullptr) && ImGui::GetIO().Fonts->AddFontFromFileTTF(candidate, 0.0F)) break;
  }
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  const Layout layout = locate_layout();
  Pending pending;
  State state = State::NoRom;
  RomView rom;
  fs::path entry;
  std::string failure;
  bool show_diagnostics = false, dragging = false;
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
      if (event.type == SDL_EVENT_DROP_BEGIN) dragging = true;
      if (event.type == SDL_EVENT_DROP_COMPLETE) dragging = false;
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

    const float t = static_cast<float>(SDL_GetTicks()) / 1000.0F;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(S(window_w), S(window_h)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList *bg = ImGui::GetWindowDrawList();
    draw_background(bg, t);
    draw_logo(bg, S(window_w) * 0.5F, S(84), t);
    const ImU32 muted = rgba(160, 166, 188), dim = rgba(120, 126, 148);
    ImGui::SetCursorPos(ImVec2(0, S(196)));
    centered_text("Recompile Sega games to native code.", muted);

    // Card
    const float card_x = S(40), card_y = S(240), card_w = S(window_w) - S(80), card_h = S(350);
    bg->AddRectFilled(ImVec2(card_x + S(2), card_y + S(8)), ImVec2(card_x + card_w + S(2), card_y + card_h + S(8)), rgba(0, 0, 0, 70), S(22));
    bg->AddRectFilled(ImVec2(card_x, card_y), ImVec2(card_x + card_w, card_y + card_h), rgba(18, 20, 34, 214), S(22));
    bg->AddRect(ImVec2(card_x, card_y), ImVec2(card_x + card_w, card_y + card_h), rgba(255, 255, 255, 30), S(22), 0, S(1.2F));
    ImGui::SetCursorPos(ImVec2(card_x, card_y));
    ImGui::BeginChild("card", ImVec2(card_w, card_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const float button_width = card_w - S(120);
    if (state == State::NoRom) {
      const ImVec2 o = ImGui::GetWindowPos();
      const ImVec2 a(o.x + S(24), o.y + S(24)), b(o.x + card_w - S(24), o.y + card_h - S(24));
      dashed_rect(ImGui::GetWindowDrawList(), a, b, dragging ? rgba(90, 150, 255) : rgba(255, 255, 255, 70), S(dragging ? 2.5F : 1.6F));
      if (dragging) ImGui::GetWindowDrawList()->AddRectFilled(a, b, rgba(47, 107, 255, 28), S(16));
      // "tray + down arrow" icon
      const float cx = o.x + card_w * 0.5F, cy = o.y + S(92);
      ImDrawList *dl = ImGui::GetWindowDrawList();
      const ImU32 ic = dragging ? rgba(120, 175, 255) : rgba(190, 200, 235);
      dl->AddLine(ImVec2(cx, cy - S(26)), ImVec2(cx, cy + S(10)), ic, S(3.5F));
      dl->AddLine(ImVec2(cx - S(14), cy - S(4)), ImVec2(cx, cy + S(10)), ic, S(3.5F));
      dl->AddLine(ImVec2(cx + S(14), cy - S(4)), ImVec2(cx, cy + S(10)), ic, S(3.5F));
      dl->PathLineTo(ImVec2(cx - S(26), cy + S(2))); dl->PathLineTo(ImVec2(cx - S(26), cy + S(24)));
      dl->PathLineTo(ImVec2(cx + S(26), cy + S(24))); dl->PathLineTo(ImVec2(cx + S(26), cy + S(2)));
      dl->PathStroke(ic, 0, S(3.5F));
      ImGui::SetCursorPosY(S(150));
      centered_text(dragging ? "Release to load the ROM" : "Drop a ROM here", rgba(240, 243, 255));
      centered_text("or", dim);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2));
      if (secondary_button("Browse...", S(170))) browse();
    } else {
      ImGui::SetCursorPosY(S(30));
      std::string title = rom.title;
      if (ImGui::CalcTextSize(title.c_str()).x > card_w - S(40)) title = title.substr(0, 34) + "...";
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25F * g_scale);
      centered_text(title.c_str(), rgba(245, 247, 255));
      ImGui::PopFont();
      centered_text(rom.platform.c_str(), muted);
      if (rom.supported_platform)
        centered_text(rom.compat_known ? "Experimental compatibility" : "Compatibility unknown",
                      rom.compat_known ? rgba(255, 190, 40) : dim);
      ImGui::SetCursorPosY(S(128));
      if (!layout.problem.empty()) {
        ImGui::SetCursorPosX(S(24));
        ImGui::PushTextWrapPos(card_w - S(24));
        ImGui::TextUnformatted(layout.problem.c_str());
        ImGui::PopTextWrapPos();
      } else if (state == State::RomSelected) {
        if (!rom.supported_platform) centered_text("This is not a supported Genesis / Mega Drive ROM.", rgba(255, 140, 130));
        ImGui::SetCursorPosY(S(160));
        if (primary_button(rom.compat_known ? "Recompile & Play" : "Try anyway", button_width)) start_build(false);
      } else if (state == State::Building) {
        static const char *names[BuildJob::stage_count] = {"Analyzing ROM", "Generating native C", "Compiling", "Linking"};
        centered_text("Preparing game...", rgba(240, 243, 255));
        ImGui::SetCursorPosY(S(150));
        for (int i = 0; i < BuildJob::stage_count; ++i) stage_row(names[i], job->stage(i), t);
        ImGui::SetCursorPosY(S(300));
        centered_text("Large games can take several minutes the first time.", dim);
      } else if (state == State::Ready) {
        centered_text("Native build ready", rgba(52, 199, 120));
        ImGui::SetCursorPosY(S(160));
        if (primary_button("Play", button_width)) {
          game = std::make_unique<GameRun>(rom, layout, entry);
          if (game->started()) state = State::Running;
          else { failure = "The game could not be started."; diagnostics = failure; game.reset(); state = State::Failed; }
        }
        if (secondary_button("Recompile", button_width)) start_build(true);
      } else if (state == State::Running) {
        ImGui::SetCursorPosY(S(170));
        centered_text("Playing...", rgba(240, 243, 255));
        centered_text("Close the game window to return.", dim);
      } else if (state == State::Failed) {
        ImGui::SetCursorPos(ImVec2(S(28), S(124)));
        ImGui::PushStyleColor(ImGuiCol_Text, rgba(255, 140, 130));
        ImGui::PushTextWrapPos(card_w - S(28));
        ImGui::TextUnformatted(failure.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::SetCursorPosY(S(186));
        if (rom.error.empty() && primary_button("Try again", button_width)) start_build(true);
        if (secondary_button(show_diagnostics ? "Hide diagnostics" : "View diagnostics", button_width)) show_diagnostics = !show_diagnostics;
      }
      if (state != State::Building && state != State::Running) {
        ImGui::SetCursorPosY(card_h - S(52));
        if (secondary_button("Choose another ROM...", S(220))) browse();
      }
    }
    ImGui::EndChild();

    if (show_diagnostics && state == State::Failed) {
      ImGui::SetCursorPos(ImVec2(card_x, card_y + card_h + S(10)));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(8, 9, 16, 235));
      ImGui::BeginChild("diag", ImVec2(card_w, S(70)), ImGuiChildFlags_Borders);
      ImGui::PushTextWrapPos(card_w - S(20));
      ImGui::TextUnformatted(diagnostics.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ImGui::PopStyleColor();
    } else {
      ImGui::SetCursorPos(ImVec2(card_x + S(16), card_y + card_h + S(24)));
      ImGui::PushStyleColor(ImGuiCol_Text, dim);
      ImGui::PushTextWrapPos(S(window_w) - S(56));
      ImGui::TextUnformatted("No ROMs are included with Segarecomp. Use only software you are legally entitled to analyze. "
                             "Compatibility is experimental.");
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
    }

    // Footer: version info and cache folder.
    char info[160];
    std::snprintf(info, sizeof info, "v%s  -  zig %s  -  SDL %d.%d.%d  -  %s", SEGARECOMP_LAUNCHER_VERSION, SEGARECOMP_ZIG_VERSION,
                  SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
                  host_description().c_str());
    ImGui::SetCursorPos(ImVec2(0, S(window_h) - S(60)));
    centered_text(info, dim);
    ImGui::SetCursorPosY(S(window_h) - S(40));
    ImGui::PushStyleColor(ImGuiCol_Button, rgba(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::SetCursorPosX((S(window_w) - ImGui::CalcTextSize("Open cache folder").x - S(20)) * 0.5F);
    if (ImGui::Button("Open cache folder")) SDL_OpenURL(("file://" + u8s(cache_root())).c_str());
    ImGui::PopStyleColor(2);
    ImGui::End();

    ImGui::Render();
    // High-DPI: ImGui works in window points, the renderer in pixels.
    SDL_SetRenderScale(renderer, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 9, 11, 24, 255);
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
