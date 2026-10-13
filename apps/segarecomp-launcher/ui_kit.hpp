#pragma once

// Drawing and widget helpers of the launcher UI: the repository's palette, chunky pixel bevels, icons from the shared UI textures, and
// buttons/chips/toggles that own their hit boxes (ImGui::InvisibleButton), so keyboard and gamepad focus work like any ImGui item.

#include <SDL3/SDL.h>

#include "imgui.h"

#include <map>
#include <string>
#include <vector>

namespace ui {

// Palette (apps/segarecomp-launcher/main.cpp's original constants, extended only where a new state needs a color).
inline ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
namespace col {
const ImU32 panel = rgba(8, 20, 47, 240), panel_solid = rgba(8, 20, 47), panel_hover = rgba(13, 28, 62), panel_dark = rgba(5, 12, 30, 245);
const ImU32 border_dark = rgba(7, 16, 38), edge = rgba(35, 62, 120), highlight = rgba(77, 103, 167), dash = rgba(94, 132, 209, 215), dash_hover = rgba(70, 220, 255);
const ImU32 text = rgba(220, 232, 255), text2 = rgba(169, 183, 221), dim = rgba(120, 133, 170), footer = rgba(202, 211, 235);
const ImU32 ok = rgba(104, 224, 158), err = rgba(255, 129, 119), warn = rgba(255, 197, 109), info = rgba(64, 191, 245);
const ImU32 btn = rgba(21, 87, 201), btn_hover = rgba(27, 111, 231), btn_top = rgba(64, 191, 245), btn_inner = rgba(38, 141, 238), btn_bottom = rgba(8, 41, 110), btn_outer = rgba(3, 21, 56), btn_text = rgba(244, 247, 255);
const ImU32 danger = rgba(160, 48, 56), danger_hover = rgba(190, 62, 70), danger_top = rgba(255, 129, 119), danger_bottom = rgba(80, 18, 28);
const ImU32 focus = rgba(140, 224, 255), selected = rgba(57, 124, 219), inactive = rgba(11, 32, 72), scrim = rgba(4, 8, 20, 190);
}

struct Ctx {
  ImFont *regular = nullptr, *bold = nullptr;
  float S = 1.0F;  // UI scale (display scale / pixel density, times the compact factor)
  SDL_Renderer *renderer = nullptr;
  std::map<std::string, SDL_Texture *> textures;  // loaded lazily, owned here
  [[nodiscard]] float u(float v) const { return v * S; }
  SDL_Texture *icon(const std::string &name);       // "play" -> assets/ui/icon-play.png
  SDL_Texture *asset(const std::string &relative);  // any asset path
  void release();
};

enum class Style { primary, secondary, danger, ghost };

float text_width(ImFont *font, float px, const char *text);
// Shortens `text` with "..." to fit `max_px`.
std::string ellipsize(ImFont *font, float px, float max_px, const std::string &text);
void draw_text(ImDrawList *dl, ImFont *font, float px, ImU32 color, ImVec2 pos, const char *text);
// Chunky beveled pixel frame (stepped light top/left, dark bottom/right edges).
void bevel(ImDrawList *dl, ImVec2 a, ImVec2 b, float step, ImU32 outer, ImU32 edge, ImU32 top_left, ImU32 bottom_right, ImU32 fill);
void panel(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b, bool raised = false);
void dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash, float gap);
void focus_ring(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b);
void icon_image(ImDrawList *dl, SDL_Texture *tex, ImVec2 a, float size, ImU32 tint = IM_COL32_WHITE);

// Widgets: all take absolute size in points (already scaled) and draw at the current ImGui cursor; they advance it like ImGui::Button.
bool button(Ctx &ctx, const char *id, const char *label, ImVec2 size, Style style = Style::secondary, bool enabled = true, SDL_Texture *icon = nullptr);
bool icon_button(Ctx &ctx, const char *id, SDL_Texture *icon, float size, bool active = false, bool enabled = true, const char *tooltip = nullptr);
bool chip(Ctx &ctx, const char *id, const char *label, bool active, float height = 0.0F);
bool checkbox(Ctx &ctx, const char *id, const char *label, bool *value, bool enabled = true);
// A row of mutually exclusive options; returns true when `*selected` changed.
bool segmented(Ctx &ctx, const char *id, const std::vector<std::string> &items, int *selected, float item_w);
void badge(Ctx &ctx, ImDrawList *dl, ImVec2 pos, const char *text, ImU32 color, float px, ImVec2 *size_out = nullptr);
bool input_text(Ctx &ctx, const char *id, char *buffer, size_t capacity, float width, const char *hint = nullptr, bool *active = nullptr);
void tooltip(Ctx &ctx, const char *text);

} // namespace ui
