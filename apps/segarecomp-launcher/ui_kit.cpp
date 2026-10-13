#include "ui_kit.hpp"

#include "pixel_assets.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace ui {

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

float text_width(ImFont *font, float px, const char *text) { return font->CalcTextSizeA(px, FLT_MAX, 0.0F, text).x; }

std::string ellipsize(ImFont *font, float px, float max_px, const std::string &text) {
  if (text_width(font, px, text.c_str()) <= max_px) return text;
  std::string cut = text;
  while (cut.size() > 1 && text_width(font, px, (cut + "...").c_str()) > max_px) cut.pop_back();
  return cut + "...";
}

void draw_text(ImDrawList *dl, ImFont *font, float px, ImU32 color, ImVec2 pos, const char *text) { dl->AddText(font, px, pos, color, text); }

void bevel(ImDrawList *dl, ImVec2 a, ImVec2 b, float step, ImU32 outer, ImU32 edge, ImU32 top_left, ImU32 bottom_right, ImU32 fill) {
  const float r = step * 1.0F;
  dl->AddRectFilled(a, b, outer, r);
  const ImVec2 i0(a.x + step, a.y + step), i1(b.x - step, b.y - step);
  dl->AddRectFilled(i0, i1, edge, r * 0.6F);
  dl->AddLine(ImVec2(i0.x, i0.y), ImVec2(i1.x, i0.y), top_left, step);
  dl->AddLine(ImVec2(i0.x, i0.y), ImVec2(i0.x, i1.y), top_left, step);
  dl->AddLine(ImVec2(i0.x, i1.y), ImVec2(i1.x, i1.y), bottom_right, step);
  dl->AddLine(ImVec2(i1.x, i0.y), ImVec2(i1.x, i1.y), bottom_right, step);
  dl->AddRectFilled(ImVec2(i0.x + step, i0.y + step), ImVec2(i1.x - step, i1.y - step), fill, r * 0.4F);
}

void panel(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b, bool raised) {
  bevel(dl, a, b, std::max(1.0F, ctx.u(2.0F)), col::border_dark, col::edge, raised ? col::highlight : col::edge, col::edge, col::panel);
}

void dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash, float gap) {
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

void focus_ring(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b) {
  const float o = ctx.u(2.0F);
  dl->AddRect(ImVec2(a.x - o, a.y - o), ImVec2(b.x + o, b.y + o), col::focus, ctx.u(3.0F), 0, std::max(1.0F, ctx.u(1.5F)));
}

void icon_image(ImDrawList *dl, SDL_Texture *tex, ImVec2 a, float size, ImU32 tint) {
  if (tex == nullptr) return;
  dl->AddImage(reinterpret_cast<ImTextureID>(tex), a, ImVec2(a.x + size, a.y + size), ImVec2(0, 0), ImVec2(1, 1), tint);
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
  const float step = std::max(1.0F, ctx.u(2.0F));
  ImU32 fill, top, bottom, inner, outer, text_col = col::btn_text;
  if (style == Style::primary) { fill = hovered || active ? col::btn_hover : col::btn; top = active ? col::btn_bottom : col::btn_top; bottom = active ? col::btn_top : col::btn_bottom; inner = col::btn_inner; outer = col::btn_outer; }
  else if (style == Style::danger) { fill = hovered || active ? col::danger_hover : col::danger; top = active ? col::danger_bottom : col::danger_top; bottom = active ? col::danger_top : col::danger_bottom; inner = col::danger_top; outer = col::border_dark; }
  else if (style == Style::ghost) { fill = hovered ? col::panel_hover : rgba(8, 20, 47, 120); top = hovered ? col::highlight : col::edge; bottom = col::edge; inner = col::edge; outer = col::border_dark; text_col = col::text; }
  else { fill = hovered || active ? col::panel_hover : col::panel_solid; top = hovered ? col::highlight : col::edge; bottom = col::edge; inner = col::edge; outer = col::border_dark; text_col = col::text; }
  if (!enabled) { fill = rgba(10, 22, 48); top = bottom = inner = col::edge; text_col = col::dim; }
  bevel(dl, a, b, step, outer, inner, top, bottom, fill);
  const float px = ctx.u(13.0F);
  const float icon_size = icon != nullptr ? ctx.u(16.0F) : 0.0F, gap = icon != nullptr ? ctx.u(6.0F) : 0.0F;
  const std::string shown = ellipsize(ctx.bold, px, std::max(8.0F, (b.x - a.x) - icon_size - gap - ctx.u(14.0F)), label);
  const float tw = text_width(ctx.bold, px, shown.c_str());
  const float total = icon_size + gap + tw;
  float x = (a.x + b.x) * 0.5F - total * 0.5F;
  const float y = (a.y + b.y) * 0.5F;
  const float nudge = active ? ctx.u(1.0F) : 0.0F;
  if (icon != nullptr) { icon_image(dl, icon, ImVec2(x + nudge, y - icon_size * 0.5F + nudge), icon_size, enabled ? IM_COL32_WHITE : rgba(120, 133, 170)); x += icon_size + gap; }
  draw_text(dl, ctx.bold, px, text_col, ImVec2(x + nudge, y - px * 0.58F + nudge), shown.c_str());
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
  const float step = std::max(1.0F, ctx.u(2.0F));
  bevel(dl, a, b, step, col::border_dark, active ? col::btn_inner : col::edge, active ? col::btn_top : (hovered ? col::highlight : col::edge), active ? col::btn_bottom : col::edge,
        active ? col::btn : (hovered ? col::panel_hover : col::panel_solid));
  const float isz = std::floor(size * 0.55F / 16.0F) >= 1.0F ? std::floor(size * 0.55F / 16.0F) * 16.0F : size * 0.55F;
  icon_image(dl, icon, ImVec2((a.x + b.x) * 0.5F - isz * 0.5F, (a.y + b.y) * 0.5F - isz * 0.5F), isz, enabled ? IM_COL32_WHITE : rgba(120, 133, 170));
  if (focused) focus_ring(ctx, dl, a, b);
  if (tip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) tooltip(ctx, tip);
  return pressed && enabled;
}

bool chip(Ctx &ctx, const char *id, const char *label, bool active, float height) {
  const float px = ctx.u(12.0F), h = height > 0.0F ? height : ctx.u(26.0F);
  const float w = text_width(ctx.bold, px, label) + ctx.u(20.0F);
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##c", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const float step = std::max(1.0F, ctx.u(2.0F));
  bevel(dl, a, b, step, col::border_dark, active ? col::btn_inner : col::edge, active ? col::btn_top : (hovered ? col::highlight : col::edge), active ? col::btn_bottom : col::edge,
        active ? col::btn : (hovered ? col::panel_hover : col::inactive));
  draw_text(dl, ctx.bold, px, active ? col::btn_text : col::text2, ImVec2(a.x + ctx.u(10.0F), (a.y + b.y) * 0.5F - px * 0.58F), label);
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
  const float step = std::max(1.0F, ctx.u(2.0F));
  bevel(dl, b0, b1, step, col::border_dark, col::edge, hovered ? col::highlight : col::edge, col::edge, *value && enabled ? col::btn : col::inactive);
  if (*value) icon_image(dl, ctx.icon("check"), ImVec2(b0.x + ctx.u(1.0F), b0.y + ctx.u(1.0F)), box - ctx.u(2.0F), enabled ? IM_COL32_WHITE : rgba(120, 133, 170));
  draw_text(dl, ctx.regular, px, enabled ? col::text : col::dim, ImVec2(b1.x + ctx.u(8.0F), (b0.y + b1.y) * 0.5F - px * 0.58F), label);
  if (focused) focus_ring(ctx, dl, b0, ImVec2(a.x + w, b1.y));
  if (pressed && enabled) { *value = !*value; return true; }
  return false;
}

bool segmented(Ctx &ctx, const char *id, const std::vector<std::string> &items, int *selected, float item_w) {
  bool changed = false;
  ImGui::PushID(id);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) ImGui::SameLine(0.0F, ctx.u(4.0F));
    const float px = ctx.u(12.0F), w = item_w > 0.0F ? item_w : text_width(ctx.bold, px, items[i].c_str()) + ctx.u(22.0F);
    ImGui::PushID(static_cast<int>(i));
    const bool pressed = ImGui::InvisibleButton("##s", ImVec2(w, ctx.u(28.0F)));
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused(), on = static_cast<int>(i) == *selected;
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float step = std::max(1.0F, ctx.u(2.0F));
    bevel(dl, a, b, step, col::border_dark, on ? col::btn_inner : col::edge, on ? col::btn_top : (hovered ? col::highlight : col::edge), on ? col::btn_bottom : col::edge, on ? col::btn : (hovered ? col::panel_hover : col::inactive));
    const std::string shown = ellipsize(ctx.bold, px, w - ctx.u(10.0F), items[i]);
    draw_text(dl, ctx.bold, px, on ? col::btn_text : col::text2, ImVec2((a.x + b.x) * 0.5F - text_width(ctx.bold, px, shown.c_str()) * 0.5F, (a.y + b.y) * 0.5F - px * 0.58F), shown.c_str());
    if (focused) focus_ring(ctx, dl, a, b);
    if (pressed && !on) { *selected = static_cast<int>(i); changed = true; }
  }
  ImGui::PopID();
  return changed;
}

void badge(Ctx &ctx, ImDrawList *dl, ImVec2 pos, const char *text, ImU32 color, float px, ImVec2 *size_out) {
  const float w = text_width(ctx.bold, px, text) + ctx.u(12.0F), h = px + ctx.u(8.0F);
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), rgba(5, 12, 30, 220), ctx.u(2.0F));
  dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), color, ctx.u(2.0F), 0, std::max(1.0F, ctx.u(1.0F)));
  draw_text(dl, ctx.bold, px, color, ImVec2(pos.x + ctx.u(6.0F), pos.y + ctx.u(4.0F) - px * 0.08F), text);
  if (size_out != nullptr) *size_out = ImVec2(w, h);
}

bool input_text(Ctx &ctx, const char *id, char *buffer, std::size_t capacity, float width, const char *hint, bool *active) {
  ImGui::PushID(id);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, rgba(5, 12, 30, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, col::text);
  ImGui::PushStyleColor(ImGuiCol_Border, col::edge);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0F, ctx.u(1.0F)));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ctx.u(10.0F), ctx.u(7.0F)));
  ImGui::PushFont(ctx.regular, ctx.u(13.0F));
  ImGui::SetNextItemWidth(width);
  const bool changed = hint != nullptr ? ImGui::InputTextWithHint("##t", hint, buffer, capacity) : ImGui::InputText("##t", buffer, capacity);
  if (active != nullptr) *active = ImGui::IsItemActive();
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
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
  bevel(dl, a, ImVec2(a.x + w, a.y + h), std::max(1.0F, ctx.u(1.0F)), col::border_dark, col::edge, col::highlight, col::edge, col::panel_dark);
  draw_text(dl, ctx.regular, px, col::text, ImVec2(a.x + ctx.u(8.0F), a.y + ctx.u(6.0F)), text);
}

} // namespace ui
