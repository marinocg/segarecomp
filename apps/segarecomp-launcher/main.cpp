// Segarecomp consumer launcher: pick or drop a ROM, recompile it with the packaged toolchain, play it.
// Thin UI over launcher_core; it owns no recompilation logic.

#include "launcher_core.hpp"
#include "pixel_assets.hpp"

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

// ---- design-space layout (points at the reference window size; independent of DPI/resize) ----

constexpr float LW = 560.0F;   // logical design canvas: matches the pixel-art reference's proportions
constexpr float LH = 760.0F;

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// Palette, taken from the pixel-art reference (deliberately limited: one interactive accent family,
// the wordmark/background art carry the rest of the color).
const ImU32 panel_fill        = rgba(8, 20, 47);      // #08142F
const ImU32 panel_fill_hover  = rgba(13, 28, 62);
const ImU32 panel_border_dark = rgba(7, 16, 38);       // #071026
const ImU32 panel_edge        = rgba(35, 62, 120);     // #233E78
const ImU32 panel_highlight   = rgba(77, 103, 167);    // #4D67A7
const ImU32 dash_color        = rgba(94, 132, 209, 215);
const ImU32 dash_color_hover  = rgba(70, 220, 255, 255);
const ImU32 text_primary      = rgba(220, 232, 255);   // #DCE8FF
const ImU32 text_secondary    = rgba(169, 183, 221);   // #A9B7DD
const ImU32 text_dim          = rgba(120, 133, 170);
const ImU32 text_footer       = rgba(202, 211, 235, 255);   // brighter than text_dim: footer/legal text over busy background art
const ImU32 col_ok            = rgba(104, 224, 158);
const ImU32 col_err           = rgba(255, 129, 119);
const ImU32 link_normal       = rgba(62, 167, 255);    // #3EA7FF
const ImU32 link_hover        = rgba(121, 203, 255);   // #79CBFF
const ImU32 link_pressed      = rgba(37, 131, 217);    // #2583D9
const ImU32 btn_fill          = rgba(21, 87, 201);     // #1557C9
const ImU32 btn_fill_hover    = rgba(27, 111, 231);    // #1B6FE7
const ImU32 btn_top_highlight = rgba(64, 191, 245);    // #40BFF5
const ImU32 btn_inner_edge    = rgba(38, 141, 238);    // #268DEE
const ImU32 btn_bottom_shadow = rgba(8, 41, 110);      // #08296E
const ImU32 btn_outer_shadow  = rgba(3, 21, 56);       // #031538
const ImU32 btn_text          = rgba(244, 247, 255);   // #F4F7FF
const ImU32 focus_ring        = rgba(140, 224, 255);

// A design-space -> current-window-point-space transform. Fonts, drop shadows and dashed borders are all
// drawn through this so the whole composition scales uniformly (letterboxed, centered) as the window resizes,
// exactly as the DPI transform in `SDL_SetRenderScale` (applied at present time) then maps points -> pixels.
struct Layout2D {
  float scale, ox, oy;
  [[nodiscard]] ImVec2 at(float x, float y) const { return ImVec2(ox + x * scale, oy + y * scale); }
  [[nodiscard]] float len(float v) const { return v * scale; }
};

float text_width(ImFont *font, float px, const char *text) {
  ImGui::PushFont(font, px);
  const float w = ImGui::CalcTextSize(text).x;
  ImGui::PopFont();
  return w;
}

void draw_text(ImDrawList *dl, ImFont *font, float px, ImU32 color, ImVec2 pos, const char *text) {
  dl->AddText(font, px, pos, color, text);
}

void draw_text_centered(ImDrawList *dl, ImFont *font, float px, ImU32 color, float center_x, float top_y, const char *text) {
  draw_text(dl, font, px, color, ImVec2(center_x - text_width(font, px, text) * 0.5F, top_y), text);
}

// Chunky beveled pixel panel (stepped light top/left, dark bottom/right edges) -- the drop-target frame and
// the primary button both use this, just at different sizes. `rounding` stays tiny so corners read as a
// pixel step rather than a smooth modern radius.
void draw_bevel_frame(ImDrawList *dl, ImVec2 a, ImVec2 b, float step, ImU32 outer, ImU32 edge, ImU32 top_left,
                      ImU32 bottom_right, ImU32 fill, float rounding) {
  dl->AddRectFilled(a, b, outer, rounding);
  const ImVec2 i0(a.x + step, a.y + step), i1(b.x - step, b.y - step);
  dl->AddRectFilled(i0, i1, edge, rounding * 0.6F);
  dl->AddLine(ImVec2(i0.x, i0.y), ImVec2(i1.x, i0.y), top_left, step);
  dl->AddLine(ImVec2(i0.x, i0.y), ImVec2(i0.x, i1.y), top_left, step);
  dl->AddLine(ImVec2(i0.x, i1.y), ImVec2(i1.x, i1.y), bottom_right, step);
  dl->AddLine(ImVec2(i1.x, i0.y), ImVec2(i1.x, i1.y), bottom_right, step);
  const ImVec2 f0(i0.x + step, i0.y + step), f1(i1.x - step, i1.y - step);
  dl->AddRectFilled(f0, f1, fill, rounding * 0.4F);
}

// Short rectangular dashes, no antialiasing niceties -- the inner drop-target boundary.
void draw_dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash, float gap) {
  for (float x = a.x; x < b.x; x += dash + gap) {
    const float x2 = std::min(x + dash, b.x);
    dl->AddLine(ImVec2(x, a.y), ImVec2(x2, a.y), color, thickness);
    dl->AddLine(ImVec2(x, b.y), ImVec2(x2, b.y), color, thickness);
  }
  for (float y = a.y; y < b.y; y += dash + gap) {
    const float y2 = std::min(y + dash, b.y);
    dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, y2), color, thickness);
    dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, y2), color, thickness);
  }
}

// A 16-bit-menu-style beveled push button. Owns its own hit box (ImGui::InvisibleButton), so Tab reaches it
// and Enter/Space activates it like any other ImGui widget (ImGuiConfigFlags_NavEnableKeyboard is enabled
// once in gui()); a keyboard focus ring is drawn outside the bevel, never color-only.
bool bevel_button(const Layout2D &L, const char *id, float x, float y, float w, float h, const char *label,
                  ImFont *font, float label_px) {
  ImGui::SetCursorPos(L.at(x, y));
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##b", ImVec2(L.len(w), L.len(h)));
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
  const float step = L.len(2.5F);
  const ImU32 fill = hovered || active ? btn_fill_hover : btn_fill;
  const ImU32 top_left = active ? btn_bottom_shadow : btn_top_highlight;
  const ImU32 bottom_right = active ? btn_top_highlight : btn_bottom_shadow;
  draw_bevel_frame(dl, p0, p1, step, btn_outer_shadow, btn_inner_edge, top_left, bottom_right, fill, L.len(3));
  if (!active) dl->AddLine(ImVec2(p0.x + step * 2, p0.y + step * 2), ImVec2(p1.x - step * 2, p0.y + step * 2),
                           btn_top_highlight, L.len(1));
  const float lw = text_width(font, L.len(label_px), label);
  const ImVec2 center((p0.x + p1.x) * 0.5F - lw * 0.5F, (p0.y + p1.y) * 0.5F - L.len(label_px) * 0.58F);
  const ImVec2 text_pos = active ? ImVec2(center.x + L.len(1.5F), center.y + L.len(1.5F)) : center;
  draw_text(dl, font, L.len(label_px), btn_text, text_pos, label);
  if (focused) dl->AddRect(ImVec2(p0.x - step, p0.y - step), ImVec2(p1.x + step, p1.y + step), focus_ring,
                           L.len(4), 0, L.len(1.5F));
  return pressed;
}

// Flatter secondary/outline action -- same hit-testing/focus contract as bevel_button, lower visual weight.
bool outline_button(const Layout2D &L, const char *id, float x, float y, float w, float h, const char *label,
                    ImFont *font, float label_px) {
  ImGui::SetCursorPos(L.at(x, y));
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##b", ImVec2(L.len(w), L.len(h)));
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
  dl->AddRectFilled(p0, p1, hovered ? panel_fill_hover : panel_fill, L.len(3));
  dl->AddRect(p0, p1, hovered ? panel_highlight : panel_edge, L.len(3), 0, L.len(1.5F));
  const float lw = text_width(font, L.len(label_px), label);
  draw_text(dl, font, L.len(label_px), text_primary, ImVec2((p0.x + p1.x) * 0.5F - lw * 0.5F, (p0.y + p1.y) * 0.5F - L.len(label_px) * 0.58F), label);
  if (focused) dl->AddRect(ImVec2(p0.x - L.len(2), p0.y - L.len(2)), ImVec2(p1.x + L.len(2), p1.y + L.len(2)),
                           focus_ring, L.len(4), 0, L.len(1.5F));
  return pressed;
}

// A desktop-chrome text link (the footer's "Open cache folder"): icon + text, underlines on hover, no button box.
bool text_link(const Layout2D &L, const char *id, float center_x, float y, SDL_Texture *icon, const char *label,
              ImFont *font, float px) {
  const float icon_size = icon ? px * 0.95F : 0.0F, gap = icon ? 6.0F : 0.0F;
  const float lw_design = text_width(font, L.len(px), label) / L.scale;
  const float total = icon_size + gap + lw_design;
  const float x = center_x - total * 0.5F;
  ImGui::SetCursorPos(L.at(x, y - 2.0F));
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##link", ImVec2(L.len(total), L.len(px + 6.0F)));
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 color = active ? link_pressed : (hovered ? link_hover : link_normal);
  const ImVec2 p0 = ImGui::GetItemRectMin();
  if (icon) dl->AddImage(reinterpret_cast<ImTextureID>(icon), ImVec2(p0.x, p0.y + L.len(1)),
                         ImVec2(p0.x + L.len(icon_size), p0.y + L.len(icon_size) + L.len(1)));
  const ImVec2 text_pos(p0.x + L.len(icon_size + gap), p0.y + L.len(3));
  draw_text(dl, font, L.len(px), color, text_pos, label);
  if (hovered) {
    const float tw = text_width(font, L.len(px), label);
    dl->AddLine(ImVec2(text_pos.x, text_pos.y + L.len(px) + L.len(1)), ImVec2(text_pos.x + tw, text_pos.y + L.len(px) + L.len(1)), color, L.len(1));
  }
  if (focused) dl->AddRect(ImVec2(p0.x - L.len(2), p0.y - L.len(2)),
                           ImVec2(p0.x + L.len(total) + L.len(2), p0.y + L.len(px + 6) + L.len(2)), focus_ring, L.len(3), 0, L.len(1.2F));
  return pressed;
}

// Build-stage row: check for done, spinning arc for running, hollow dot for pending. Left aligned.
void draw_stage_row(const Layout2D &L, ImDrawList *dl, float x, float y, const char *label, StageState state,
                    ImFont *font, float t) {
  const ImVec2 c = L.at(x + 7.0F, y + 9.0F);
  const float r = L.len(7.0F);
  if (state == StageState::done) {
    dl->PathLineTo(ImVec2(c.x - r * 0.7F, c.y)); dl->PathLineTo(ImVec2(c.x - r * 0.1F, c.y + r * 0.6F));
    dl->PathLineTo(ImVec2(c.x + r * 0.85F, c.y - r * 0.55F));
    dl->PathStroke(col_ok, 0, L.len(2));
  } else if (state == StageState::running) {
    dl->PathArcTo(c, r, t * 6.0F, t * 6.0F + 4.2F, 16);
    dl->PathStroke(link_hover, 0, L.len(2));
  } else {
    dl->AddCircle(c, r * 0.62F, text_dim, 16, L.len(1.3F));
  }
  draw_text(dl, font, L.len(15), state == StageState::pending ? text_dim : text_primary, L.at(x + 26.0F, y), label);
}

// Debug aid for reviewing the layout without a display: SEGARECOMP_SHOT=<file.bmp> saves a frame and exits.
void maybe_save_screenshot(SDL_Renderer *renderer, int frame) {
  const char *path = SDL_getenv("SEGARECOMP_SHOT");
  if (!path || frame < 12) return;
  if (SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr)) { SDL_SaveBMP(surface, path); SDL_DestroySurface(surface); }
  SDL_Event quit; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
}

int gui() {
  if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }
  float content_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (content_scale < 1.0F) content_scale = 1.0F;
  // Window size is in points (screen coordinates), never pre-multiplied by the display content scale --
  // SDL_WINDOW_HIGH_PIXEL_DENSITY alone gives a full-resolution backbuffer on a Retina/HiDPI display.
  SDL_Window *window = SDL_CreateWindow("Segarecomp", static_cast<int>(LW), static_cast<int>(LH),
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_SetWindowMinimumSize(window, static_cast<int>(LW * 0.55F), static_cast<int>(LH * 0.55F));
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) { std::fprintf(stderr, "window failed: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Tab/Enter/Space reach and activate the custom buttons
  ImGui::StyleColorsDark();
  ImGui::GetStyle().FontScaleDpi = content_scale;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  // Fonts: Silkscreen (bold for primary actionable labels, regular for everything else). ImGui 1.92's
  // dynamic font system rasterizes each PushFont(font, size) on demand, so one loaded instance covers every
  // size this layout needs -- no fixed-size atlas baking. A missing font file falls back to ImGui's built-in
  // font rather than failing the launcher (assets are best-effort presentation, never load-bearing).
  ImFont *font_builtin = io.Fonts->AddFontDefault();  // guaranteed present: never leave the atlas fontless
  const auto load_font = [&](const char *relative) -> ImFont * {
    const fs::path path = asset_path(relative);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return nullptr;
    return io.Fonts->AddFontFromFileTTF(path.string().c_str(), 0.0F);
  };
  ImFont *font_bold = load_font("fonts/Silkscreen-Bold.ttf");
  ImFont *font_regular = load_font("fonts/Silkscreen-Regular.ttf");
  if (!font_bold) font_bold = font_builtin;
  if (!font_regular) font_regular = font_builtin;

  SDL_Texture *tex_background = load_pixel_texture(renderer, "background.png");
  SDL_Texture *tex_wordmark = load_pixel_texture(renderer, "wordmark.png");
  SDL_Texture *tex_drop = load_pixel_texture(renderer, "icon-drop.png");
  SDL_Texture *tex_info = load_pixel_texture(renderer, "icon-info.png");
  SDL_Texture *tex_folder = load_pixel_texture(renderer, "icon-folder.png");
  float bg_w = 0, bg_h = 0, wm_w_px = 0, wm_h_px = 0;
  if (tex_background) { float w, h; SDL_GetTextureSize(tex_background, &w, &h); bg_w = w; bg_h = h; }
  if (tex_wordmark) { float w, h; SDL_GetTextureSize(tex_wordmark, &w, &h); wm_w_px = w; wm_h_px = h; }

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
  if (const char *preselect = SDL_getenv("SEGARECOMP_SHOT_ROM")) {  // review aid, see maybe_save_screenshot
    select_rom(preselect);
    if (SDL_getenv("SEGARECOMP_SHOT_BUILD") && state == State::RomSelected) start_build(false);
  }

  bool running = true;
  int frame = 0;
  while (running) {
    ++frame;
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
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    // Uniform scale-to-fit ("letterbox") of the fixed design canvas within the current window size, centered;
    // this is the only concession to resizing -- the background instead covers the full window (below), never
    // letterboxed, so resizing crops scenery rather than shrinking the composition into empty bars.
    const Layout2D L{std::min(static_cast<float>(win_w) / LW, static_cast<float>(win_h) / LH), 0, 0};
    Layout2D centered = L;
    centered.ox = (static_cast<float>(win_w) - LW * L.scale) * 0.5F;
    centered.oy = (static_cast<float>(win_h) - LH * L.scale) * 0.5F;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(win_w), static_cast<float>(win_h)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // ---- background: cover-fill (crop, never distort) the whole window, independent of the letterbox ----
    if (tex_background && bg_w > 0 && bg_h > 0) {
      const float cover = std::max(static_cast<float>(win_w) / bg_w, static_cast<float>(win_h) / bg_h);
      const float dw = bg_w * cover, dh = bg_h * cover;
      const float dx = (static_cast<float>(win_w) - dw) * 0.5F, dy = (static_cast<float>(win_h) - dh) * 0.5F;
      dl->AddImage(reinterpret_cast<ImTextureID>(tex_background), ImVec2(dx, dy), ImVec2(dx + dw, dy + dh));
    } else {
      dl->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(win_w), static_cast<float>(win_h)), rgba(8, 11, 26));
    }

    // ---- header: pixel-art wordmark (or a bold-text fallback if the asset failed to load), compact subtitle ----
    const float wm_w = LW * 0.58F, wm_h = wm_w_px > 0 ? wm_w * (wm_h_px / wm_w_px) : wm_w * 0.2F;
    const float wm_top = 46.0F;
    if (tex_wordmark)
      dl->AddImage(reinterpret_cast<ImTextureID>(tex_wordmark), centered.at((LW - wm_w) * 0.5F, wm_top),
                  centered.at((LW - wm_w) * 0.5F + wm_w, wm_top + wm_h));
    else
      draw_text_centered(dl, font_bold, centered.len(40), text_primary, centered.at(LW * 0.5F, 0).x, centered.at(0, wm_top + wm_h * 0.3F).y, "SEGARECOMP");
    const float subtitle_y = wm_top + wm_h + 14.0F;
    draw_text_centered(dl, font_regular, centered.len(15), text_secondary, centered.at(LW * 0.5F, 0).x, centered.at(0, subtitle_y).y,
                       "Recompile Sega games to native code.");

    // ---- the ROM drop / status panel: same outer frame for every state, only the inner content changes ----
    const float panel_x0 = LW * 0.10F, panel_x1 = LW * 0.90F, panel_w = panel_x1 - panel_x0;
    const float panel_y0 = subtitle_y + 38.0F, panel_h = 256.0F, panel_y1 = panel_y0 + panel_h;
    const bool drop_hover = ImGui::IsMouseHoveringRect(centered.at(panel_x0, panel_y0), centered.at(panel_x1, panel_y1), false);
    const bool drop_active = state == State::NoRom && dragging;
    {
      const float step = centered.len(3);
      const ImU32 fill = drop_active ? rgba(10, 34, 58) : (state == State::NoRom && drop_hover ? panel_fill_hover : panel_fill);
      const ImU32 top_left = drop_active ? dash_color_hover : panel_highlight;
      draw_bevel_frame(dl, centered.at(panel_x0, panel_y0), centered.at(panel_x1, panel_y1), step, panel_border_dark,
                       panel_edge, top_left, panel_edge, fill, centered.len(6));
      draw_dashed_rect(dl, centered.at(panel_x0 + 16, panel_y0 + 16), centered.at(panel_x1 - 16, panel_y1 - 16),
                       drop_active ? dash_color_hover : dash_color, centered.len(2), centered.len(9), centered.len(6));
    }
    const float content_x = panel_x0 + 22.0F;
    const float center_x = (panel_x0 + panel_x1) * 0.5F;

    if (state == State::NoRom) {
      const float icon_size = 56.0F, icon_top = panel_y0 + 40.0F;
      if (tex_drop) {
        const ImU32 tint = drop_active ? rgba(150, 235, 255) : rgba(255, 255, 255);
        dl->AddImage(reinterpret_cast<ImTextureID>(tex_drop), centered.at(center_x - icon_size * 0.5F, icon_top),
                    centered.at(center_x + icon_size * 0.5F, icon_top + icon_size), ImVec2(0, 0), ImVec2(1, 1), tint);
      }
      const float label_y = icon_top + icon_size + 18.0F;
      draw_text_centered(dl, font_bold, centered.len(21), text_primary, centered.at(center_x, 0).x, centered.at(0, label_y).y,
                         drop_active ? "Release to load the ROM" : "Drop a ROM here");
      draw_text_centered(dl, font_regular, centered.len(14), text_secondary, centered.at(center_x, 0).x, centered.at(0, label_y + 32.0F).y, "or");
      if (bevel_button(centered, "browse", center_x - 90.0F, label_y + 58.0F, 180.0F, 46.0F, "Browse...", font_bold, 18.0F))
        browse();
    }

    if (state != State::NoRom) {
      std::string title = rom.title;
      while (title.size() > 4 && text_width(font_bold, centered.len(18), (title + "...").c_str()) > panel_w - 44.0F) title.resize(title.size() - 4);
      if (title != rom.title) title += "...";
      draw_text(dl, font_bold, centered.len(18), text_primary, centered.at(content_x, panel_y0 + 30.0F), title.c_str());
      std::string meta = rom.platform;
      if (rom.supported_platform) meta += rom.compat_known ? "  -  Experimental compatibility" : "  -  Compatibility unknown";
      draw_text(dl, font_regular, centered.len(13), text_secondary, centered.at(content_x, panel_y0 + 54.0F), meta.c_str());
      const float row_y = panel_y0 + 86.0F;

      if (!layout.problem.empty()) {
        ImGui::SetCursorPos(centered.at(content_x, row_y));
        ImGui::PushTextWrapPos(centered.at(panel_x1 - 20.0F, 0).x);
        ImGui::PushStyleColor(ImGuiCol_Text, col_err);
        ImGui::PushFont(font_regular, centered.len(13));
        ImGui::TextUnformatted(layout.problem.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
      } else if (state == State::RomSelected) {
        if (!rom.supported_platform)
          draw_text(dl, font_regular, centered.len(13), col_err, centered.at(content_x, row_y), "This is not a supported Genesis / Mega Drive ROM.");
        if (bevel_button(centered, "recompile", content_x, row_y + 30.0F, 190.0F, 42.0F,
                         rom.compat_known ? "Recompile & Play" : "Try anyway", font_bold, 16.0F))
          start_build(false);
        if (outline_button(centered, "choose1", content_x + 204.0F, row_y + 30.0F, 170.0F, 42.0F, "Choose another...", font_regular, 14.0F))
          browse();
      } else if (state == State::Building) {
        static const char *names[BuildJob::stage_count] = {"Analyzing ROM", "Generating native C", "Compiling", "Linking"};
        draw_text(dl, font_bold, centered.len(15), text_primary, centered.at(content_x, row_y - 6.0F), "Preparing game...");
        for (int i = 0; i < BuildJob::stage_count; ++i)
          draw_stage_row(centered, dl, content_x, row_y + 20.0F + 23.0F * static_cast<float>(i), names[i], job->stage(i), font_regular, t);
        draw_text(dl, font_regular, centered.len(12), text_dim, centered.at(content_x, panel_y0 + panel_h - 34.0F),
                 "Large games can take several minutes the first time.");
      } else if (state == State::Ready) {
        draw_text(dl, font_regular, centered.len(14), col_ok, centered.at(content_x, row_y), "Native build ready");
        if (bevel_button(centered, "play", content_x, row_y + 30.0F, 120.0F, 42.0F, "Play", font_bold, 17.0F)) {
          game = std::make_unique<GameRun>(rom, layout, entry);
          if (game->started()) state = State::Running;
          else { failure = "The game could not be started."; diagnostics = failure; game.reset(); state = State::Failed; }
        }
        if (outline_button(centered, "recompile2", content_x + 134.0F, row_y + 30.0F, 120.0F, 42.0F, "Recompile", font_regular, 14.0F))
          start_build(true);
        if (outline_button(centered, "choose2", content_x + 268.0F, row_y + 30.0F, 140.0F, 42.0F, "Choose...", font_regular, 14.0F))
          browse();
        // Fixed control scheme (the runtime has no remapping UI yet): keep it visible right where "Play" is.
        const float controls_y = row_y + 96.0F;
        draw_text(dl, font_bold, centered.len(13), text_secondary, centered.at(content_x, controls_y), "Controls");
        draw_text(dl, font_regular, centered.len(13), text_footer, centered.at(content_x, controls_y + 20.0F),
                 "Arrows = D-Pad   Z = A   X = B   C = C   Enter = Start");
      } else if (state == State::Running) {
        draw_text(dl, font_bold, centered.len(16), text_primary, centered.at(content_x, row_y + 10.0F), "Playing...");
        draw_text(dl, font_regular, centered.len(13), text_secondary, centered.at(content_x, row_y + 38.0F), "Close the game window to return.");
      } else if (state == State::Failed) {
        ImGui::SetCursorPos(centered.at(content_x, row_y - 6.0F));
        ImGui::PushTextWrapPos(centered.at(panel_x1 - 20.0F, 0).x);
        ImGui::PushStyleColor(ImGuiCol_Text, col_err);
        ImGui::PushFont(font_regular, centered.len(13));
        ImGui::TextUnformatted(failure.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        const float by = panel_y0 + panel_h - 64.0F;
        if (rom.error.empty() && bevel_button(centered, "retry", content_x, by, 130.0F, 40.0F, "Try again", font_bold, 15.0F))
          start_build(true);
        if (outline_button(centered, "diag", content_x + 144.0F, by, 168.0F, 40.0F, show_diagnostics ? "Hide diagnostics" : "View diagnostics",
                           font_regular, 13.0F))
          show_diagnostics = !show_diagnostics;
        if (outline_button(centered, "choose3", content_x + 326.0F, by, 130.0F, 40.0F, "Choose...", font_regular, 13.0F))
          browse();
      }
    }

    // ---- below the panel: diagnostics (when asked for) or the compact legal/info panel; then the footer ----
    const float below_y = panel_y1 + 22.0F;
    if (show_diagnostics && state == State::Failed) {
      const float dh = 130.0F;
      dl->AddRectFilled(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + dh), rgba(5, 9, 20, 235), centered.len(4));
      dl->AddRect(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + dh), panel_edge, centered.len(4));
      ImGui::SetCursorPos(centered.at(panel_x0 + 12.0F, below_y + 10.0F));
      ImGui::PushTextWrapPos(centered.at(panel_x1 - 12.0F, 0).x);
      ImGui::PushStyleColor(ImGuiCol_Text, text_secondary);
      ImGui::PushFont(font_regular, centered.len(12));
      ImGui::BeginChild("diag", ImVec2(centered.len(panel_w - 24.0F), centered.len(dh - 20.0F)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
      ImGui::TextUnformatted(diagnostics.c_str());
      ImGui::EndChild();
      ImGui::PopFont();
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
    } else {
      const float legal_h = 82.0F;
      dl->AddRectFilled(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + legal_h), panel_fill, centered.len(4));
      dl->AddRect(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + legal_h), panel_edge, centered.len(4), 0, centered.len(1.2F));
      if (tex_info) dl->AddImage(reinterpret_cast<ImTextureID>(tex_info), centered.at(panel_x0 + 14.0F, below_y + 14.0F),
                                centered.at(panel_x0 + 40.0F, below_y + 40.0F));
      const float tx = panel_x0 + (tex_info ? 52.0F : 16.0F);
      draw_text(dl, font_regular, centered.len(12.5F), text_footer, centered.at(tx, below_y + 12.0F), "No ROMs are included with Segarecomp. Use only");
      draw_text(dl, font_regular, centered.len(12.5F), text_footer, centered.at(tx, below_y + 30.0F), "software you are legally entitled to analyze.");
      draw_text(dl, font_regular, centered.len(12.5F), text_footer, centered.at(tx, below_y + 48.0F), "Compatibility is experimental.");
    }

    // ---- footer: quiet, centered status metadata, then the cache-folder link -- on its own scrim so it
    // stays legible over the busy background art regardless of what part of the scenery sits behind it ----
    {
      const float fx0 = LW * 0.5F - 220.0F, fx1 = LW * 0.5F + 220.0F, fy0 = LH - 86.0F, fy1 = LH - 16.0F;
      dl->AddRectFilled(centered.at(fx0, fy0), centered.at(fx1, fy1), rgba(6, 10, 22, 165), centered.len(6));
      dl->AddLine(centered.at(fx0 + 10.0F, fy0), centered.at(fx1 - 10.0F, fy0), rgba(60, 75, 120, 120), centered.len(1));
    }
    char info[160];
    std::snprintf(info, sizeof info, "v%s  \xC2\xB7  Zig %s  \xC2\xB7  SDL %d.%d.%d  \xC2\xB7  %s", SEGARECOMP_LAUNCHER_VERSION,
                  SEGARECOMP_ZIG_VERSION, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                  SDL_VERSIONNUM_MICRO(SDL_GetVersion()), host_description().c_str());
    draw_text_centered(dl, font_regular, centered.len(12.5F), text_footer, centered.at(LW * 0.5F, 0).x, centered.at(0, LH - 70.0F).y, info);
    if (text_link(centered, "cache", LW * 0.5F, LH - 42.0F, tex_folder, "Open cache folder", font_regular, 13.5F))
      SDL_OpenURL(("file://" + u8s(cache_root())).c_str());

    ImGui::End();

    ImGui::Render();
    // High-DPI: ImGui/our own draw calls above work in window points; the renderer target is pixels.
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 8, 11, 26, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    maybe_save_screenshot(renderer, frame);
    SDL_RenderPresent(renderer);
  }
  game.reset();
  job.reset();
  if (tex_background) SDL_DestroyTexture(tex_background);
  if (tex_wordmark) SDL_DestroyTexture(tex_wordmark);
  if (tex_drop) SDL_DestroyTexture(tex_drop);
  if (tex_info) SDL_DestroyTexture(tex_info);
  if (tex_folder) SDL_DestroyTexture(tex_folder);
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
