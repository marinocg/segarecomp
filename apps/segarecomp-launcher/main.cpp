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

constexpr float window_w = 520.0F;   // logical size at 100% scale; the window is fixed
constexpr float window_h = 440.0F;
constexpr float margin = 28.0F;

float g_scale = 1.0F;
float S(float v) { return v * g_scale; }
ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// Palette: one interactive accent; the logo alone carries the richer colors.
const ImU32 col_bg = rgba(19, 21, 27), col_panel = rgba(26, 29, 37), col_border = rgba(52, 57, 70);
const ImU32 col_text = rgba(226, 229, 238), col_muted = rgba(139, 145, 162), col_dim = rgba(103, 109, 127);
const ImU32 col_accent = rgba(61, 123, 253), col_accent_hi = rgba(92, 145, 255), col_ok = rgba(80, 200, 130), col_err = rgba(240, 120, 112);

void text_at(ImDrawList *dl, float size, ImU32 color, float x, float y, const char *text, bool bold = false) {
  dl->AddText(nullptr, S(size), ImVec2(x, y), color, text);
  if (bold) dl->AddText(nullptr, S(size), ImVec2(x + S(0.7F), y), color, text);
}

// Background: flat dark with a very faint 16-bit tile grid. No gradients, blobs or arcs.
void draw_background(ImDrawList *dl) {
  const float w = S(window_w), h = S(window_h), cell = S(16);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), col_bg);
  for (float x = 0; x <= w; x += cell) dl->AddLine(ImVec2(x, 0), ImVec2(x, h), rgba(255, 255, 255, 5));
  for (float y = 0; y <= h; y += cell) dl->AddLine(ImVec2(0, y), ImVec2(w, y), rgba(255, 255, 255, 5));
}

// Logo mark (kept from the previous design, smaller and flat): play triangle inside a two-color ring.
void draw_mark(ImDrawList *dl, float cx, float cy, float r) {
  dl->AddCircleFilled(ImVec2(cx, cy), r, rgba(28, 64, 170), 48);
  dl->AddCircle(ImVec2(cx, cy), r, rgba(120, 170, 255), 48, S(1.5F));
  dl->PathArcTo(ImVec2(cx, cy), r - S(5), -1.2F, 1.0F, 24);
  dl->PathStroke(rgba(20, 220, 235), 0, S(2.5F));
  dl->PathArcTo(ImVec2(cx, cy), r - S(5), 1.94F, 4.14F, 24);
  dl->PathStroke(rgba(255, 190, 40), 0, S(2.5F));
  const float k = r / 38.0F;
  dl->AddTriangleFilled(ImVec2(cx - S(9) * k, cy - S(13) * k), ImVec2(cx - S(9) * k, cy + S(13) * k), ImVec2(cx + S(15) * k, cy), rgba(255, 255, 255));
}

// x, y, width, height are in logical (100% scale) units.
bool button(const char *label, float x, float y, float width, bool primary, float height = 34.0F) {
  ImGui::SetCursorPos(ImVec2(S(x), S(y)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, primary ? 0.0F : S(1));
  ImGui::PushStyleColor(ImGuiCol_Button, primary ? col_accent : col_panel);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? col_accent_hi : rgba(38, 42, 53));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? rgba(45, 100, 220) : rgba(32, 36, 46));
  ImGui::PushStyleColor(ImGuiCol_Border, col_border);
  ImGui::PushStyleColor(ImGuiCol_Text, primary ? rgba(255, 255, 255) : col_text);
  const bool pressed = ImGui::Button(label, ImVec2(width > 0 ? S(width) : 0, S(height)));
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(2);
  return pressed;
}

// Subtle text-only action (used for the footer).
bool text_button(const char *label, float right_x, float y) {
  ImGui::PushFont(nullptr, S(12.5F));
  const float w = ImGui::CalcTextSize(label).x + S(12);
  ImGui::SetCursorPos(ImVec2(right_x - w, y));
  ImGui::PushStyleColor(ImGuiCol_Button, rgba(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(255, 255, 255, 14));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(255, 255, 255, 24));
  ImGui::PushStyleColor(ImGuiCol_Text, col_muted);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), S(2)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(4));
  const bool pressed = ImGui::Button(label);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(4);
  ImGui::PopFont();
  return pressed;
}

// Stage row: done = check, running = spinner, pending = hollow dot. Left aligned.
void stage_row(ImDrawList *dl, float x, float y, const char *label, StageState state, float t) {
  const float r = S(6);
  const ImVec2 c(x + r, y + S(9));
  if (state == StageState::done) {
    dl->PathLineTo(ImVec2(c.x - r * 0.7F, c.y)); dl->PathLineTo(ImVec2(c.x - r * 0.15F, c.y + r * 0.6F));
    dl->PathLineTo(ImVec2(c.x + r * 0.8F, c.y - r * 0.55F));
    dl->PathStroke(col_ok, 0, S(2));
  } else if (state == StageState::running) {
    dl->PathArcTo(c, r, t * 6.0F, t * 6.0F + 4.2F, 16);
    dl->PathStroke(col_accent_hi, 0, S(2));
  } else {
    dl->AddCircle(c, r * 0.6F, col_dim, 16, S(1.2F));
  }
  text_at(dl, 15, state == StageState::pending ? col_dim : col_text, x + S(26), y, label);
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
  style.WindowPadding = ImVec2(0, 0); style.ItemSpacing = ImVec2(S(8), S(8)); style.FramePadding = ImVec2(S(14), S(7));
  style.FontScaleDpi = g_scale;
  style.FontSizeBase = 16.0F;
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
    ImDrawList *dl = ImGui::GetWindowDrawList();
    draw_background(dl);

    // ---- header: small mark, name, understated subtitle (left aligned) ----
    draw_mark(dl, S(margin + 20), S(44), S(20));
    text_at(dl, 30, col_text, S(margin + 52), S(24), "Segarecomp", true);
    text_at(dl, 14, col_muted, S(margin + 53), S(58), "Recompile Sega games to native code.");
    // one small brand accent: a four-segment cartridge-label stripe under the header
    {
      const ImU32 seg[] = {rgba(47, 107, 255), rgba(20, 220, 235), rgba(255, 190, 40), rgba(214, 51, 132)};
      const float y = S(88), w = S(20);
      for (int i = 0; i < 4; ++i)
        dl->AddRectFilled(ImVec2(S(margin) + static_cast<float>(i) * (w + S(2)), y), ImVec2(S(margin) + static_cast<float>(i) * (w + S(2)) + w, y + S(3)), seg[i]);
    }

    // ---- main control ----
    const float bx = S(margin), by = S(108), bw = S(window_w - 2 * margin);
    float bh = S(112);
    if (state == State::Building) bh = S(176);
    else if (state != State::NoRom) bh = S(150);
    if (state == State::Failed) bh = S(170);
    const bool box_hover = ImGui::IsMouseHoveringRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), false);
    const bool drop_active = state == State::NoRom && (dragging || false);
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), drop_active ? rgba(61, 123, 253, 26) : col_panel, S(8));
    if (state == State::NoRom) {
      // dashed-free simple border, modest radius; accent only while a file is dragged over the window
      dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), drop_active ? col_accent : (box_hover ? rgba(80, 87, 104) : col_border), S(8), 0, S(drop_active ? 1.5F : 1.0F));
    } else {
      dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), col_border, S(8), 0, S(1));
    }
    const float px = bx + S(20);   // content inset inside the control

    if (state == State::NoRom) {
      // small, secondary open-file icon
      const float ix = px, iy = by + S(24);
      const ImU32 ic = drop_active ? col_accent_hi : col_muted;
      dl->AddLine(ImVec2(ix + S(12), iy), ImVec2(ix + S(12), iy + S(15)), ic, S(2));
      dl->AddLine(ImVec2(ix + S(6), iy + S(9)), ImVec2(ix + S(12), iy + S(15)), ic, S(2));
      dl->AddLine(ImVec2(ix + S(18), iy + S(9)), ImVec2(ix + S(12), iy + S(15)), ic, S(2));
      dl->PathLineTo(ImVec2(ix + S(2), iy + S(11))); dl->PathLineTo(ImVec2(ix + S(2), iy + S(22)));
      dl->PathLineTo(ImVec2(ix + S(22), iy + S(22))); dl->PathLineTo(ImVec2(ix + S(22), iy + S(11)));
      dl->PathStroke(ic, 0, S(2));
      text_at(dl, 17, col_text, px + S(38), by + S(20), drop_active ? "Release to load the ROM" : "Drop a ROM here");
      text_at(dl, 13, col_muted, px + S(38), by + S(44), ".bin  .md  .gen  .smd");
      if (button("Browse...", (px - S(0)) / g_scale + 38.0F, (by + S(66)) / g_scale, 104, false, 32)) browse();
    } else {
      std::string title = rom.title;
      while (title.size() > 4 && ImGui::CalcTextSize(title.c_str()).x * 1.15F > bw - S(40)) title.resize(title.size() - 4), title += "...";
      text_at(dl, 18, col_text, px, by + S(16), title.c_str());
      std::string meta = rom.platform;
      if (rom.supported_platform) meta += rom.compat_known ? "  -  Experimental compatibility" : "  -  Compatibility unknown";
      text_at(dl, 13, col_muted, px, by + S(40), meta.c_str());
      const float row_y = by + S(70);
      const float rx = px / g_scale, ry = row_y / g_scale;
      if (!layout.problem.empty()) {
        ImGui::SetCursorPos(ImVec2(px, row_y));
        ImGui::PushTextWrapPos(bx + bw - S(20));
        ImGui::PushStyleColor(ImGuiCol_Text, col_err);
        ImGui::TextUnformatted(layout.problem.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
      } else if (state == State::RomSelected) {
        if (!rom.supported_platform) text_at(dl, 13, col_err, px, row_y - S(4), "This is not a supported Genesis / Mega Drive ROM.");
        if (button(rom.compat_known ? "Recompile & Play" : "Try anyway", rx, ry + 20, 170, true, 38)) start_build(false);
        if (button("Choose another...", rx + 182, ry + 20, 150, false, 38)) browse();
      } else if (state == State::Building) {
        static const char *names[BuildJob::stage_count] = {"Analyzing ROM", "Generating native C", "Compiling", "Linking"};
        for (int i = 0; i < BuildJob::stage_count; ++i) stage_row(dl, px, by + S(68) + S(22) * static_cast<float>(i), names[i], job->stage(i), t);
        text_at(dl, 12, col_dim, px, by + bh - S(22), "Large games can take several minutes the first time.");
      } else if (state == State::Ready) {
        text_at(dl, 13, col_ok, px, by + S(64), "Native build ready");
        if (button("Play", rx, ry + 30, 110, true, 38)) {
          game = std::make_unique<GameRun>(rom, layout, entry);
          if (game->started()) state = State::Running;
          else { failure = "The game could not be started."; diagnostics = failure; game.reset(); state = State::Failed; }
        }
        if (button("Recompile", rx + 122, ry + 30, 110, false, 38)) start_build(true);
        if (button("Choose another...", rx + 244, ry + 30, 150, false, 38)) browse();
      } else if (state == State::Running) {
        text_at(dl, 16, col_text, px, by + S(78), "Playing...");
        text_at(dl, 13, col_muted, px, by + S(102), "Close the game window to return.");
      } else if (state == State::Failed) {
        ImGui::SetCursorPos(ImVec2(px, by + S(62)));
        ImGui::PushTextWrapPos(bx + bw - S(20));
        ImGui::PushStyleColor(ImGuiCol_Text, col_err);
        ImGui::TextUnformatted(failure.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        const float fy = (by + bh - S(52)) / g_scale;
        if (rom.error.empty() && button("Try again", rx, fy, 100, true, 34)) start_build(true);
        if (button(show_diagnostics ? "Hide diagnostics" : "View diagnostics", rx + 112, fy, 150, false, 34)) show_diagnostics = !show_diagnostics;
        if (button("Choose another...", rx + 274, fy, 140, false, 34)) browse();
      }
    }

    // ---- below the control: diagnostics when asked for, otherwise the legal notice (left aligned, compact) ----
    const float below = by + bh + S(16);
    if (show_diagnostics && state == State::Failed) {
      dl->AddRectFilled(ImVec2(bx, below), ImVec2(bx + bw, below + S(120)), rgba(12, 13, 18), S(6));
      dl->AddRect(ImVec2(bx, below), ImVec2(bx + bw, below + S(120)), col_border, S(6));
      ImGui::SetCursorPos(ImVec2(bx + S(10), below + S(8)));
      ImGui::BeginChild("diag", ImVec2(bw - S(20), S(104)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
      ImGui::PushTextWrapPos(bw - S(30));
      ImGui::PushStyleColor(ImGuiCol_Text, col_muted);
      ImGui::TextUnformatted(diagnostics.c_str());
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
    } else {
      text_at(dl, 12.5F, col_muted, bx, below, "No ROMs are included with Segarecomp.");
      text_at(dl, 12.5F, col_muted, bx, below + S(17), "Use only software you are legally entitled to analyze. Compatibility is experimental.");
    }

    // ---- footer: status metadata left, action right ----
    const float fy = S(window_h) - S(44);
    dl->AddLine(ImVec2(S(margin), fy), ImVec2(S(window_w - margin), fy), col_border);
    char info[160];
    std::snprintf(info, sizeof info, "v%s  \xC2\xB7  Zig %s  \xC2\xB7  SDL %d.%d.%d  \xC2\xB7  %s", SEGARECOMP_LAUNCHER_VERSION,
                  SEGARECOMP_ZIG_VERSION, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                  SDL_VERSIONNUM_MICRO(SDL_GetVersion()), host_description().c_str());
    text_at(dl, 12, col_dim, S(margin), fy + S(15), info);
    if (text_button("Open cache folder", S(window_w - margin) + S(6), fy + S(10))) SDL_OpenURL(("file://" + u8s(cache_root())).c_str());
    ImGui::End();

    ImGui::Render();
    // High-DPI: ImGui works in window points, the renderer in pixels.
    SDL_SetRenderScale(renderer, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 19, 21, 27, 255);
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
