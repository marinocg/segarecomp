#include "ui_kit.hpp"

#include "pixel_assets.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace ui {

ImFont *bold_font = nullptr;

SDL_Texture *Ctx::asset(const std::string &relative) {
  const auto found = textures.find(relative);
  if (found != textures.end()) return found->second;
  SDL_Texture *texture = launcher::load_pixel_texture(renderer, relative);
  textures[relative] = texture;  // a missing decorative asset is remembered as nullptr, never retried every frame
  return texture;
}

SDL_Texture *Ctx::icon(const std::string &name) { return asset("ui/icon-" + name + ".png"); }

void Ctx::release() {
  for (auto &entry : textures)
    if (entry.second != nullptr) SDL_DestroyTexture(entry.second);
  textures.clear();
}

float text_width(ImFont *font, float px, const char *text) { return font->CalcTextSizeA(px, FLT_MAX, 0.0F, text).x + (font == bold_font ? 0.6F : 0.0F); }

std::string ellipsize(ImFont *font, float px, float max_px, const std::string &text) {
  if (text_width(font, px, text.c_str()) <= max_px) return text;
  std::string cut = text;
  while (cut.size() > 1 && text_width(font, px, (cut + "...").c_str()) > max_px) cut.pop_back();
  return cut + "...";
}

void draw_text(ImDrawList *dl, ImFont *font, float px, ImU32 color, ImVec2 pos, const char *text) {
  dl->AddText(font, px, pos, color, text);
  if (font == bold_font) dl->AddText(font, px, ImVec2(pos.x + 0.6F, pos.y), color, text);
}

void draw_text_bold(Ctx &ctx, ImDrawList *dl, float px, ImU32 color, ImVec2 pos, const char *text) { draw_text(dl, ctx.bold, px, color, pos, text); }

void bevel(ImDrawList *dl, ImVec2 a, ImVec2 b, float step, ImU32 outer, ImU32 edge, ImU32 top_left, ImU32 bottom_right, ImU32 fill) {
  (void)outer; (void)bottom_right;
  dl->AddRectFilled(a, b, fill, step * 3.0F);
  dl->AddRect(a, b, edge, step * 3.0F, 0, std::max(1.0F, step * 0.75F));
  dl->AddLine(ImVec2(a.x + step * 3.0F, a.y + step), ImVec2(b.x - step * 3.0F, a.y + step), top_left & 0x60FFFFFF, 1.0F);
}

void panel(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b, bool raised) {
  const float r = ctx.u(7.0F);
  dl->AddRectFilled(ImVec2(a.x - ctx.u(2.0F), a.y - ctx.u(2.0F)), ImVec2(b.x + ctx.u(2.0F), b.y + ctx.u(2.0F)), col::glow_soft, r + ctx.u(2.0F));
  dl->AddRectFilled(a, b, col::panel, r);
  dl->AddRect(a, b, raised ? col::btn_inner : col::glow, r, 0, std::max(1.0F, ctx.u(1.5F)));
}

void card(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b, bool selected, bool hovered, ImU32 fill) {
  const float r = ctx.u(6.0F);
  if (selected) dl->AddRectFilled(ImVec2(a.x - ctx.u(2.0F), a.y - ctx.u(2.0F)), ImVec2(b.x + ctx.u(2.0F), b.y + ctx.u(2.0F)), rgba(64, 191, 245, 70), r + ctx.u(2.0F));
  dl->AddRectFilled(a, b, fill != 0 ? fill : (hovered ? col::panel_hover : col::panel_solid), r);
  dl->AddRect(a, b, selected ? col::btn_top : (hovered ? col::highlight : rgba(34, 70, 140)), r, 0, std::max(1.0F, ctx.u(selected ? 2.0F : 1.2F)));
}

void dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash, float gap) {
  for (float x = a.x; x < b.x; x += dash + gap) {
    const float x2 = std::min(x + dash, b.x);
    dl->AddRectFilled(ImVec2(std::floor(x), std::floor(a.y)), ImVec2(std::floor(x2), std::floor(a.y) + thickness), color);
    dl->AddRectFilled(ImVec2(std::floor(x), std::floor(b.y)), ImVec2(std::floor(x2), std::floor(b.y) + thickness), color);
  }
  for (float y = a.y; y < b.y; y += dash + gap) {
    const float y2 = std::min(y + dash, b.y);
    dl->AddRectFilled(ImVec2(std::floor(a.x), std::floor(y)), ImVec2(std::floor(a.x) + thickness, std::floor(y2)), color);
    dl->AddRectFilled(ImVec2(std::floor(b.x), std::floor(y)), ImVec2(std::floor(b.x) + thickness, std::floor(y2)), color);
  }
}

void focus_ring(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b) {
  const float o = ctx.u(2.0F);
  dl->AddRect(ImVec2(a.x - o, a.y - o), ImVec2(b.x + o, b.y + o), col::focus, ctx.u(7.0F), 0, std::max(1.0F, ctx.u(1.5F)));
}

void icon_image(ImDrawList *dl, SDL_Texture *tex, ImVec2 a, float size, ImU32 tint) {
  if (tex == nullptr) return;
  dl->AddImage(reinterpret_cast<ImTextureID>(tex), a, ImVec2(a.x + size, a.y + size), ImVec2(0, 0), ImVec2(1, 1), tint);
}

void pill(Ctx &ctx, ImDrawList *dl, ImVec2 pos, const char *text, ImU32 color, SDL_Texture *icon, float px, ImVec2 *size_out) {
  if (px <= 0.0F) px = ctx.u(11.0F);
  const float isz = icon != nullptr ? px + ctx.u(3.0F) : 0.0F, gap = icon != nullptr ? ctx.u(5.0F) : 0.0F;
  const float w = text_width(ctx.bold, px, text) + isz + gap + ctx.u(14.0F), h = px + ctx.u(9.0F);
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), rgba(8, 18, 42, 235), ctx.u(4.0F));
  dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), color == col::text2 ? rgba(52, 84, 150) : (color & 0x00FFFFFF) | 0xB0000000, ctx.u(4.0F), 0, std::max(1.0F, ctx.u(1.0F)));
  float x = pos.x + ctx.u(7.0F);
  if (icon != nullptr) { icon_image(dl, icon, ImVec2(x, pos.y + (h - isz) * 0.5F), isz, color); x += isz + gap; }
  draw_text(dl, ctx.bold, px, color, ImVec2(x, pos.y + (h - px) * 0.5F - px * 0.08F), text);
  if (size_out != nullptr) *size_out = ImVec2(w, h);
}

void step_circle(Ctx &ctx, ImDrawList *dl, ImVec2 c, float r, int number) {
  dl->AddCircleFilled(c, r, col::btn);
  dl->AddCircle(c, r, col::btn_top, 24, std::max(1.0F, ctx.u(1.2F)));
  char s[8];
  std::snprintf(s, sizeof s, "%d", number);
  const float px = r * 1.15F;
  draw_text(dl, ctx.bold, px, col::btn_text, ImVec2(c.x - text_width(ctx.bold, px, s) * 0.5F, c.y - px * 0.55F), s);
}

void progress_bar(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b, float fraction, ImU32 color) {
  const float r = (b.y - a.y) * 0.5F;
  dl->AddRectFilled(a, b, col::field, r);
  fraction = std::clamp(fraction, 0.0F, 1.0F);
  if (fraction > 0.0F) dl->AddRectFilled(a, ImVec2(a.x + std::max(r * 2.0F, (b.x - a.x) * fraction), b.y), color, r);
  (void)ctx;
}

bool button(Ctx &ctx, const char *id, const char *label, ImVec2 size, Style style, bool enabled, SDL_Texture *icon) {
  ImGui::PushID(id);
  if (!enabled) ImGui::BeginDisabled();
  const bool pressed = ImGui::InvisibleButton("##b", size);
  if (!enabled) ImGui::EndDisabled();
  const bool hovered = ImGui::IsItemHovered() && enabled, active = ImGui::IsItemActive() && enabled, focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const float r = ctx.u(5.0F);
  ImU32 fill, edge, text_col = col::btn_text;
  if (style == Style::primary) { fill = active ? col::btn_bottom : (hovered ? col::btn_hover : col::btn); edge = col::btn_top; }
  else if (style == Style::danger) { fill = hovered || active ? col::danger_hover : col::danger; edge = col::danger_top; }
  else if (style == Style::ghost) { fill = hovered ? col::panel_hover : rgba(10, 24, 56, 150); edge = hovered ? col::highlight : rgba(40, 78, 150); text_col = col::text; }
  else { fill = hovered || active ? col::panel_hover : rgba(12, 28, 64); edge = hovered ? col::btn_top : rgba(46, 96, 190); text_col = col::text; }
  if (!enabled) { fill = rgba(10, 20, 44); edge = rgba(30, 50, 96); text_col = col::dim; }
  dl->AddRectFilled(a, b, fill, r);
  dl->AddRect(a, b, edge, r, 0, std::max(1.0F, ctx.u(1.2F)));
  if (style == Style::primary && enabled) dl->AddLine(ImVec2(a.x + r, a.y + ctx.u(1.5F)), ImVec2(b.x - r, a.y + ctx.u(1.5F)), rgba(160, 220, 255, 120), 1.0F);
  const float px = ctx.u(13.0F);
  const float icon_size = icon != nullptr ? ctx.u(16.0F) : 0.0F, gap = icon != nullptr ? ctx.u(7.0F) : 0.0F;
  const std::string shown = ellipsize(ctx.bold, px, std::max(8.0F, (b.x - a.x) - icon_size - gap - ctx.u(14.0F)), label);
  const float tw = text_width(ctx.bold, px, shown.c_str());
  float x = (a.x + b.x) * 0.5F - (icon_size + gap + tw) * 0.5F;
  const float y = (a.y + b.y) * 0.5F;
  const float nudge = active ? ctx.u(1.0F) : 0.0F;
  if (icon != nullptr) { icon_image(dl, icon, ImVec2(x + nudge, y - icon_size * 0.5F + nudge), icon_size, enabled ? IM_COL32_WHITE : rgba(100, 113, 150)); x += icon_size + gap; }
  draw_text(dl, ctx.bold, px, text_col, ImVec2(x + nudge, y - px * 0.55F + nudge), shown.c_str());
  if (focused) focus_ring(ctx, dl, a, b);
  return pressed && enabled;
}

bool icon_button(Ctx &ctx, const char *id, SDL_Texture *icon, float size, bool active, bool enabled, const char *tip) {
  ImGui::PushID(id);
  if (!enabled) ImGui::BeginDisabled();
  const bool pressed = ImGui::InvisibleButton("##i", ImVec2(size, size));
  if (!enabled) ImGui::EndDisabled();
  const bool hovered = ImGui::IsItemHovered() && enabled, focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const float r = ctx.u(5.0F);
  dl->AddRectFilled(a, b, active ? col::btn : (hovered ? col::panel_hover : rgba(12, 28, 64)), r);
  dl->AddRect(a, b, active ? col::btn_top : (hovered ? col::highlight : rgba(46, 96, 190)), r, 0, std::max(1.0F, ctx.u(1.2F)));
  const float isz = std::floor(size * 0.55F / 16.0F) >= 1.0F ? std::floor(size * 0.55F / 16.0F) * 16.0F : size * 0.55F;
  icon_image(dl, icon, ImVec2((a.x + b.x) * 0.5F - isz * 0.5F, (a.y + b.y) * 0.5F - isz * 0.5F), isz, enabled ? IM_COL32_WHITE : rgba(100, 113, 150));
  if (focused) focus_ring(ctx, dl, a, b);
  if (tip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) tooltip(ctx, tip);
  return pressed && enabled;
}

bool chip(Ctx &ctx, const char *id, const char *label, bool active, float height) {
  const float px = ctx.u(12.0F), h = height > 0.0F ? height : ctx.u(26.0F);
  const float w = text_width(ctx.bold, px, label) + ctx.u(22.0F);
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##c", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  dl->AddRectFilled(a, b, active ? col::btn : (hovered ? col::panel_hover : col::inactive), ctx.u(5.0F));
  dl->AddRect(a, b, active ? col::btn_top : rgba(46, 96, 190), ctx.u(5.0F), 0, std::max(1.0F, ctx.u(1.2F)));
  draw_text(dl, ctx.bold, px, active ? col::btn_text : col::text2, ImVec2(a.x + ctx.u(11.0F), (a.y + b.y) * 0.5F - px * 0.55F), label);
  if (focused) focus_ring(ctx, dl, a, b);
  return pressed;
}

bool checkbox(Ctx &ctx, const char *id, const char *label, bool *value, bool enabled) {
  const float px = ctx.u(13.0F), box = ctx.u(18.0F);
  const float w = box + ctx.u(8.0F) + text_width(ctx.regular, px, label);
  ImGui::PushID(id);
  if (!enabled) ImGui::BeginDisabled();
  const bool pressed = ImGui::InvisibleButton("##k", ImVec2(w, box + ctx.u(4.0F)));
  if (!enabled) ImGui::EndDisabled();
  const bool hovered = ImGui::IsItemHovered() && enabled, focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin();
  const ImVec2 b0(a.x, a.y + ctx.u(2.0F)), b1(a.x + box, a.y + ctx.u(2.0F) + box);
  dl->AddRectFilled(b0, b1, *value && enabled ? col::btn : col::field, ctx.u(4.0F));
  dl->AddRect(b0, b1, *value ? col::btn_top : (hovered ? col::highlight : rgba(46, 96, 190)), ctx.u(4.0F), 0, std::max(1.0F, ctx.u(1.2F)));
  if (*value) icon_image(dl, ctx.icon("check"), ImVec2(b0.x + ctx.u(1.0F), b0.y + ctx.u(1.0F)), box - ctx.u(2.0F), enabled ? IM_COL32_WHITE : rgba(120, 133, 170));
  draw_text(dl, ctx.regular, px, enabled ? col::text : col::dim, ImVec2(b1.x + ctx.u(8.0F), (b0.y + b1.y) * 0.5F - px * 0.6F), label);
  if (focused) focus_ring(ctx, dl, b0, ImVec2(a.x + w, b1.y));
  if (pressed && enabled) { *value = !*value; return true; }
  return false;
}

bool toggle(Ctx &ctx, const char *id, const char *label, bool *value, float width, bool enabled) {
  const float px = ctx.u(13.0F), sw = ctx.u(38.0F), sh = ctx.u(20.0F);
  ImGui::PushID(id);
  if (!enabled) ImGui::BeginDisabled();
  const bool pressed = ImGui::InvisibleButton("##t", ImVec2(width, ctx.u(26.0F)));
  if (!enabled) ImGui::EndDisabled();
  const bool hovered = ImGui::IsItemHovered() && enabled, focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  draw_text(dl, ctx.regular, px, enabled ? col::text : col::dim, ImVec2(a.x, (a.y + b.y) * 0.5F - px * 0.6F), label);
  const ImVec2 s0(b.x - sw, (a.y + b.y) * 0.5F - sh * 0.5F), s1(b.x, s0.y + sh);
  dl->AddRectFilled(s0, s1, *value ? (enabled ? col::btn_hover : rgba(40, 60, 100)) : rgba(20, 36, 72), sh * 0.5F);
  dl->AddRect(s0, s1, *value ? col::btn_top : (hovered ? col::highlight : rgba(60, 90, 150)), sh * 0.5F, 0, std::max(1.0F, ctx.u(1.2F)));
  dl->AddCircleFilled(ImVec2(*value ? s1.x - sh * 0.5F : s0.x + sh * 0.5F, s0.y + sh * 0.5F), sh * 0.38F, *value ? rgba(244, 247, 255) : rgba(150, 168, 205));
  if (focused) focus_ring(ctx, dl, a, b);
  if (pressed && enabled) { *value = !*value; return true; }
  return false;
}

bool dropdown(Ctx &ctx, const char *id, const std::vector<std::string> &items, int *selected, float width, SDL_Texture *icon) {
  bool changed = false;
  ImGui::PushID(id);
  const float h = ctx.u(32.0F), px = ctx.u(13.0F);
  const bool pressed = ImGui::InvisibleButton("##d", ImVec2(width, h));
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  dl->AddRectFilled(a, b, col::field, ctx.u(5.0F));
  dl->AddRect(a, b, hovered ? col::btn_top : rgba(46, 96, 190), ctx.u(5.0F), 0, std::max(1.0F, ctx.u(1.2F)));
  float x = a.x + ctx.u(10.0F);
  if (icon != nullptr) { icon_image(dl, icon, ImVec2(x, (a.y + b.y) * 0.5F - ctx.u(8.0F)), ctx.u(16.0F)); x += ctx.u(24.0F); }
  const std::string shown = ellipsize(ctx.regular, px, b.x - x - ctx.u(26.0F), items.empty() ? std::string() : items[static_cast<std::size_t>(std::clamp(*selected, 0, static_cast<int>(items.size()) - 1))]);
  draw_text(dl, ctx.regular, px, col::text, ImVec2(x, (a.y + b.y) * 0.5F - px * 0.6F), shown.c_str());
  const ImVec2 c(b.x - ctx.u(14.0F), (a.y + b.y) * 0.5F);
  dl->AddTriangleFilled(ImVec2(c.x - ctx.u(4.0F), c.y - ctx.u(2.0F)), ImVec2(c.x + ctx.u(4.0F), c.y - ctx.u(2.0F)), ImVec2(c.x, c.y + ctx.u(3.0F)), col::text2);
  if (focused) focus_ring(ctx, dl, a, b);
  if (pressed) ImGui::OpenPopup("##list");
  ImGui::SetNextWindowPos(ImVec2(a.x, b.y + ctx.u(2.0F)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, col::panel_dark);
  ImGui::PushStyleColor(ImGuiCol_Border, col::glow);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ctx.u(4.0F), ctx.u(4.0F)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ctx.u(5.0F));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, std::max(1.0F, ctx.u(1.2F)));
  if (ImGui::BeginPopup("##list")) {
    for (std::size_t i = 0; i < items.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      const bool on = static_cast<int>(i) == *selected;
      const bool click = ImGui::InvisibleButton("##o", ImVec2(width - ctx.u(8.0F), ctx.u(28.0F)));
      const ImVec2 oa = ImGui::GetItemRectMin(), ob = ImGui::GetItemRectMax();
      ImDrawList *pl = ImGui::GetWindowDrawList();
      if (on || ImGui::IsItemHovered()) pl->AddRectFilled(oa, ob, on ? col::btn : col::panel_hover, ctx.u(4.0F));
      draw_text(pl, ctx.regular, px, on ? col::btn_text : col::text, ImVec2(oa.x + ctx.u(8.0F), (oa.y + ob.y) * 0.5F - px * 0.6F), items[i].c_str());
      if (click) { if (!on) { *selected = static_cast<int>(i); changed = true; } ImGui::CloseCurrentPopup(); }
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
  ImGui::PopID();
  return changed;
}

bool segmented(Ctx &ctx, const char *id, const std::vector<std::string> &items, int *selected, float item_w) {
  bool changed = false;
  ImGui::PushID(id);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) ImGui::SameLine(0.0F, ctx.u(5.0F));
    const float px = ctx.u(12.0F), w = item_w > 0.0F ? item_w : text_width(ctx.bold, px, items[i].c_str()) + ctx.u(24.0F);
    ImGui::PushID(static_cast<int>(i));
    const bool pressed = ImGui::InvisibleButton("##s", ImVec2(w, ctx.u(28.0F)));
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused(), on = static_cast<int>(i) == *selected;
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, on ? col::btn : (hovered ? col::panel_hover : col::inactive), ctx.u(5.0F));
    dl->AddRect(a, b, on ? col::btn_top : rgba(46, 96, 190), ctx.u(5.0F), 0, std::max(1.0F, ctx.u(1.2F)));
    const std::string shown = ellipsize(ctx.bold, px, w - ctx.u(10.0F), items[i]);
    draw_text(dl, ctx.bold, px, on ? col::btn_text : col::text2, ImVec2((a.x + b.x) * 0.5F - text_width(ctx.bold, px, shown.c_str()) * 0.5F, (a.y + b.y) * 0.5F - px * 0.55F), shown.c_str());
    if (focused) focus_ring(ctx, dl, a, b);
    if (pressed && !on) { *selected = static_cast<int>(i); changed = true; }
  }
  ImGui::PopID();
  return changed;
}

void badge(Ctx &ctx, ImDrawList *dl, ImVec2 pos, const char *text, ImU32 color, float px, ImVec2 *size_out) { pill(ctx, dl, pos, text, color, nullptr, px, size_out); }

bool input_text(Ctx &ctx, const char *id, char *buffer, std::size_t capacity, float width, const char *hint, bool *active) {
  ImGui::PushID(id);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, col::field);
  ImGui::PushStyleColor(ImGuiCol_Text, col::text);
  ImGui::PushStyleColor(ImGuiCol_Border, rgba(46, 96, 190));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0F, ctx.u(1.2F)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ctx.u(5.0F));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ctx.u(34.0F), ctx.u(8.0F)));
  ImGui::PushFont(ctx.regular, ctx.u(13.0F));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::SetNextItemWidth(width);
  const bool changed = hint != nullptr ? ImGui::InputTextWithHint("##t", hint, buffer, capacity) : ImGui::InputText("##t", buffer, capacity);
  if (active != nullptr) *active = ImGui::IsItemActive();
  icon_image(ImGui::GetWindowDrawList(), ctx.icon("search"), ImVec2(p.x + ctx.u(10.0F), p.y + ctx.u(8.0F)), ctx.u(16.0F), rgba(150, 175, 220));
  ImGui::PopFont();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(3);
  ImGui::PopID();
  return changed;
}

void tooltip(Ctx &ctx, const char *text) {
  const float px = ctx.u(12.0F);
  const ImVec2 mouse = ImGui::GetMousePos();
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const float w = text_width(ctx.regular, px, text) + ctx.u(16.0F), h = px + ctx.u(14.0F);
  const ImVec2 a(mouse.x + ctx.u(12.0F), mouse.y + ctx.u(16.0F));
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col::panel_dark, ctx.u(4.0F));
  dl->AddRect(a, ImVec2(a.x + w, a.y + h), col::glow, ctx.u(4.0F));
  draw_text(dl, ctx.regular, px, col::text, ImVec2(a.x + ctx.u(8.0F), a.y + ctx.u(6.0F)), text);
}

} // namespace ui
