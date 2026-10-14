// The launcher's art: the repository's own sunset artwork composed responsively behind the window (hero banner, calm water, restrained foliage), and the
// placeholder painting shown for a game that has no cover (one of four, chosen by digest, under a platform band and an explicit no-cover tag). Nothing here is game art, a game screenshot or fabricated metadata.

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

} // namespace

// The scenery banner: rich enough to carry the identity (sun, horizon, waterfall), short enough that every page's primary content - notably the
// Details page's Build Configuration - fits the first 1080x740 viewport. About 163 px at 1080x740, 114 px at the 720x520 minimum.
float App::hero_height(float, float h) const { return std::clamp(h * 0.22F, ctx_.u(108.0F), ctx_.u(196.0F)); }

void App::draw_background(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 navy = rgba(3, 8, 22), water_navy = rgba(5, 12, 44);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), navy);
  SDL_Texture *bg = art_texture(ctx_);
  if (bg == nullptr) return;  // a missing decorative asset leaves the flat page, never a crash
  const float s = w / kArtW;  // the artwork covers the window width: never distorted, never tiled
  const float hero = hero_height(w, h);
  const ImTextureID tex = reinterpret_cast<ImTextureID>(bg);
  // 1. hero: the whole sun, the mountains and the horizon (rows around the horizon line, 80 % above it, so the sun's disc is never cut)
  const float above = hero * 0.80F / s, below = hero * 0.20F / s;
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
  // ---- no cover chosen: one of the four supplied placeholder paintings ----
  // Chosen per game from its digest (stable, and neighbouring cards differ), shown under a platform band and an explicit "NO COVER ART" tag so a
  // placeholder is never mistaken for the game's identified box art.
  const bool sms = entry.platform_id == "master-system";
  const ImU32 accent = sms ? rgba(255, 186, 96) : rgba(64, 191, 245);
  dl->AddRectFilled(a, b, rgba(9, 20, 48), rounding);
  unsigned seed = 0;
  for (std::size_t i = 0; i < entry.sha256.size() && i < 8; ++i) seed = seed * 16U + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(entry.sha256[i])) ? entry.sha256[i] - '0' : (std::tolower(static_cast<unsigned char>(entry.sha256[i])) - 'a' + 10) & 15);
  char name[40];
  std::snprintf(name, sizeof name, "covers/placeholder-%u.png", seed % 4U + 1U);
  if (SDL_Texture *art = ctx_.asset(name)) {
    float tw = 0, th = 0;
    SDL_GetTextureSize(art, &tw, &th);
    const float frame_ratio = w / h, tex_ratio = tw / th;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (tex_ratio > frame_ratio) { const float k = frame_ratio / tex_ratio; uv0.x = (1 - k) * 0.5F; uv1.x = uv0.x + k; }
    else { const float k = tex_ratio / frame_ratio; uv0.y = (1 - k) * 0.5F; uv1.y = uv0.y + k; }
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(art), a, b, uv0, uv1, tint, rounding);
  }
  if (w > ctx_.u(70.0F)) {
    dl->PushClipRect(a, b, true);
    const float band = std::max(ctx_.u(3.0F), std::floor(h * 0.10F));
    dl->AddRectFilled(a, ImVec2(b.x, a.y + band), rgba(4, 10, 26, 225), rounding, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(a.x, a.y + band), ImVec2(b.x, a.y + band + std::max(1.0F, ctx_.u(2.0F))), accent);
    const char *system = sms ? "MASTER SYSTEM" : "GENESIS";
    float spx = std::min(band * 0.52F, ctx_.u(10.0F));
    while (spx > 5.0F && text_width(ctx_.display, spx, system) > w * 0.6F) spx -= 0.5F;
    draw_text(dl, ctx_.display, spx, with_alpha(accent, 240), ImVec2(a.x + ctx_.u(7.0F), a.y + (band - spx) * 0.5F), system);
    const char *note = "NO COVER ART";
    const float cap = std::clamp(h * 0.042F, ctx_.u(7.0F), ctx_.u(8.5F));
    const float nw = text_width(ctx_.display, cap, note) + ctx_.u(10.0F), ny = a.y + band + ctx_.u(6.0F);
    dl->AddRectFilled(ImVec2(b.x - nw - ctx_.u(6.0F), ny), ImVec2(b.x - ctx_.u(6.0F), ny + cap + ctx_.u(8.0F)), rgba(4, 10, 26, 200), ctx_.u(3.0F));
    draw_text(dl, ctx_.display, cap, rgba(200, 212, 240), ImVec2(b.x - nw - ctx_.u(1.0F), ny + ctx_.u(4.0F)), note);
    dl->PopClipRect();
  } else {
    dl->AddRectFilled(a, ImVec2(b.x, a.y + std::max(2.0F, ctx_.u(2.0F))), accent);
  }
  dl->AddRect(a, b, with_alpha(accent, 110), rounding, 0, std::max(1.0F, ctx_.u(1.0F)));
}

} // namespace launcher
