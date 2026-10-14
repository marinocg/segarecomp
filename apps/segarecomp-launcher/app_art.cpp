// The launcher's art: the repository's own sunset artwork composed responsively behind the window (hero banner, calm water, restrained foliage), and the
// neutral hand-drawn cartridge shown for a game that has no cover. Nothing here is game art, a game screenshot or fabricated metadata.

#include "app.hpp"

#include "app_helpers.hpp"
#include "pixel_assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;
using namespace ui;

namespace launcher {

namespace {

// background.png is 675x900; its sun/horizon line sits near row 368 and the foliage frame fills the last ~140 rows.
constexpr float kArtW = 675.0F, kArtH = 900.0F, kHorizon = 368.0F;

ImU32 with_alpha(ImU32 c, int a) { return (c & 0x00FFFFFFU) | (static_cast<ImU32>(a) << 24U); }

SDL_Texture *art_texture(Ctx &ctx) {
  SDL_Texture *bg = ctx.asset("background.png");
  static bool smoothed = false;
  if (bg != nullptr && !smoothed) {  // the artwork is scaled by arbitrary fractions: bilinear avoids uneven pixel columns (icons and covers stay nearest)
    SDL_SetTextureScaleMode(bg, SDL_SCALEMODE_LINEAR);
    smoothed = true;
  }
  return bg;
}

std::string initials(const std::string &title) {
  std::string out;
  bool start = true;
  for (const char c : title) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      if (start && out.size() < 3) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      start = false;
    } else if (c == ' ' || c == '-' || c == '_' || c == ':') start = true;
  }
  return out.empty() ? "?" : out;
}

} // namespace

float App::hero_height(float, float h) const { return std::clamp(h * 0.29F, ctx_.u(180.0F), ctx_.u(240.0F)); }

void App::draw_background(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 navy = rgba(3, 8, 22), water_navy = rgba(5, 12, 44);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), navy);
  SDL_Texture *bg = art_texture(ctx_);
  if (bg == nullptr) return;  // a missing decorative asset leaves the flat page, never a crash
  const float s = w / kArtW;  // the artwork covers the window width: never distorted, never tiled
  const float hero = hero_height(w, h);
  const ImTextureID tex = reinterpret_cast<ImTextureID>(bg);
  // 1. hero: the sun and the horizon (rows around the horizon line, 70 % above it)
  const float above = hero * 0.70F / s, below = hero * 0.30F / s;
  const float v0 = std::max(0.0F, kHorizon - above) / kArtH, v1 = std::min(kArtH, kHorizon + below) / kArtH;
  dl->AddImage(tex, ImVec2(0, 0), ImVec2(w, hero), ImVec2(0, v0), ImVec2(1, v1));
  // 2. calm water for the rest of the page: the artwork continues below the horizon, dimmed so content stays legible
  const float water_top = hero;
  const float row0 = kHorizon + below;
  const float rows = (h - water_top) / s;
  const float row1 = std::min(kArtH, row0 + rows);
  const float draw_h = (row1 - row0) * s;
  dl->AddImage(tex, ImVec2(0, water_top), ImVec2(w, water_top + draw_h), ImVec2(0, row0 / kArtH), ImVec2(1, row1 / kArtH), rgba(120, 132, 180));
  if (water_top + draw_h < h) dl->AddRectFilled(ImVec2(0, water_top + draw_h), ImVec2(w, h), water_navy);
  dl->AddRectFilled(ImVec2(0, water_top), ImVec2(w, h), rgba(3, 8, 28, 120));
  // 3. foliage and rocks frame the bottom corners (the artwork's own last rows), restrained
  const float fol_h = std::clamp(h * 0.20F, ctx_.u(70.0F), ctx_.u(150.0F));
  const float fol_rows = fol_h / s, fol_top = std::max(row0, kArtH - fol_rows);
  dl->AddImage(tex, ImVec2(0, h - fol_h), ImVec2(w, h), ImVec2(0, fol_top / kArtH), ImVec2(1, 1.0F), rgba(150, 160, 200, 210));
  dl->AddRectFilledMultiColor(ImVec2(0, h - fol_h), ImVec2(w, h - fol_h * 0.45F), with_alpha(water_navy, 255), with_alpha(water_navy, 255), with_alpha(water_navy, 90), with_alpha(water_navy, 90));
  // 4. readable edges: the wordmark side darkens, and the hero melts into the water
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.55F, hero), rgba(3, 8, 22, 170), rgba(3, 8, 22, 0), rgba(3, 8, 22, 0), rgba(3, 8, 22, 170));
  dl->AddRectFilledMultiColor(ImVec2(0, hero * 0.55F), ImVec2(w, hero + ctx_.u(26.0F)), rgba(3, 8, 28, 0), rgba(3, 8, 28, 0), rgba(3, 8, 28, 150), rgba(3, 8, 28, 150));
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, ctx_.u(14.0F)), rgba(3, 8, 22, 120), rgba(3, 8, 22, 120), rgba(3, 8, 22, 0), rgba(3, 8, 22, 0));
}

void App::draw_cover(ImDrawList *dl, const LibraryEntry &entry, ImVec2 a, ImVec2 b, float rounding, ImU32 tint) {
  const float w = b.x - a.x, h = b.y - a.y;
  SDL_Texture *cover = entry.cover.empty() ? nullptr : cover_texture(entry);
  if (cover == nullptr && !entry.cover.empty() && cover_missing_.count(entry.cover) == 0) {  // the chosen image is still being decoded: a plain tile, not a cartridge flash
    dl->AddRectFilled(a, b, rgba(11, 25, 58), rounding);
    return;
  }
  if (cover != nullptr) {
    float tw = 0, th = 0;
    SDL_GetTextureSize(cover, &tw, &th);
    const float frame_ratio = w / h, tex_ratio = tw / th;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (tex_ratio > frame_ratio) { const float k = frame_ratio / tex_ratio; uv0.x = (1 - k) * 0.5F; uv1.x = uv0.x + k; }
    else { const float k = tex_ratio / frame_ratio; uv0.y = (1 - k) * 0.5F; uv1.y = uv0.y + k; }
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(cover), a, b, uv0, uv1, tint, rounding);
    return;
  }
  // ---- the neutral cartridge ----
  const bool sms = entry.platform_id == "master-system";
  const ImU32 accent = sms ? rgba(155, 161, 180) : rgba(120, 160, 195);
  const ImU32 accent_dark = rgba(static_cast<int>((accent >> 0U) & 0xFFU) * 40 / 100, static_cast<int>((accent >> 8U) & 0xFFU) * 40 / 100, static_cast<int>((accent >> 16U) & 0xFFU) * 40 / 100);
  dl->AddRectFilled(a, b, rgba(11, 25, 58), rounding);
  dl->PushClipRect(ImVec2(a.x + rounding * 0.3F, a.y + rounding * 0.3F), ImVec2(b.x - rounding * 0.3F, b.y - rounding * 0.3F), true);
  dl->AddRectFilledMultiColor(a, b, rgba(18, 38, 84), rgba(18, 38, 84), rgba(8, 18, 44), rgba(8, 18, 44));
  for (float x = a.x - h; x < b.x; x += ctx_.u(14.0F)) dl->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), rgba(60, 90, 160, 14), 1.0F);  // faint pixel-diagonal texture
  dl->PopClipRect();
  dl->AddRect(a, b, rgba(34, 70, 140), rounding, 0, std::max(1.0F, ctx_.u(1.2F)));
  // the cartridge on a 30 x 36 pixel grid
  const float p = std::max(1.0F, std::floor(std::min(w * 0.70F / 30.0F, h * 0.74F / 36.0F)));
  const float cw = 30.0F * p, ch = 36.0F * p;
  const float ox = std::floor(a.x + (w - cw) * 0.5F), oy = std::floor(a.y + (h - ch) * 0.46F);
  const auto rect = [&](float x0, float y0, float x1, float y1, ImU32 c) { dl->AddRectFilled(ImVec2(ox + std::round(x0 * p), oy + std::round(y0 * p)), ImVec2(ox + std::round(x1 * p), oy + std::round(y1 * p)), c); };
  rect(0, 1, 30, 36, rgba(4, 10, 26));                       // outline
  rect(1, 0, 29, 36, rgba(4, 10, 26));
  rect(1, 2, 29, 35, rgba(34, 58, 112));                     // shell
  rect(1, 2, 29, 4, rgba(77, 103, 167));                     // lit top edge
  rect(1, 2, 2, 35, rgba(58, 84, 146));                      // lit left edge
  rect(28, 4, 29, 35, rgba(20, 36, 76));                     // shaded right edge
  rect(4, 6, 26, 24, rgba(7, 16, 38));                       // label bed
  rect(5, 7, 25, 23, accent_dark);                           // label
  rect(5, 7, 25, 8, with_alpha(accent, 200));                // label top stripe
  rect(5, 22, 25, 23, with_alpha(accent, 120));
  rect(3, 26, 27, 35, rgba(7, 16, 38));                      // contact bay
  rect(5, 28, 25, 35, rgba(18, 32, 72));
  for (int k = 0; k < 8; ++k) rect(6.0F + 2.4F * static_cast<float>(k), 31, 7.4F + 2.4F * static_cast<float>(k), 35, rgba(120, 150, 205));
  // initials in the label (the one per-title mark)
  const std::string text = initials(entry.title());
  const float label_h = 14.0F * p, label_w = 18.0F * p;
  float px = label_h * 0.78F;
  while (px > ctx_.u(8.0F) && text_width(ctx_.display, px, text.c_str()) > label_w) px -= 1.0F;
  draw_text(dl, ctx_.display, px, rgba(236, 244, 255), ImVec2(ox + 15.0F * p - text_width(ctx_.display, px, text.c_str()) * 0.5F, oy + 15.0F * p - px * 0.5F), text.c_str());
  if (h > ctx_.u(70.0F)) {
    const float cap = std::clamp(ctx_.u(8.5F), 6.0F, h * 0.08F);
    const char *note = "NO COVER";
    draw_text(dl, ctx_.display, cap, rgba(120, 140, 190), ImVec2((a.x + b.x) * 0.5F - text_width(ctx_.display, cap, note) * 0.5F, b.y - cap - ctx_.u(7.0F)), note);
  }
  (void)tint;
}

} // namespace launcher
