// The launcher's art: the repository's own sunset artwork composed responsively behind the window (hero banner, calm water, restrained foliage), and the
// title card shown for a game that has no cover (a tinted crop of the same artwork, the platform, the title and an explicit no-cover mark). Nothing here is game art, a game screenshot or fabricated metadata.

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
  // ---- no cover chosen: an honest title card ----
  // A crop of the repository's own scenery (a different window per game, from its digest), tinted to the launcher navy so it reads as a
  // launcher-made card and never as identified box art, with the platform band, the title and an explicit "no cover art" mark.
  const bool sms = entry.platform_id == "master-system";
  const ImU32 accent = sms ? rgba(255, 186, 96) : rgba(64, 191, 245);
  dl->AddRectFilled(a, b, rgba(9, 20, 48), rounding);
  unsigned seed = 0;
  for (std::size_t i = 0; i < entry.sha256.size() && i < 8; ++i) seed = seed * 16U + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(entry.sha256[i])) ? entry.sha256[i] - '0' : (std::tolower(static_cast<unsigned char>(entry.sha256[i])) - 'a' + 10) & 15);
  if (SDL_Texture *bg = art_texture(ctx_); bg != nullptr && w > ctx_.u(24.0F)) {
    // windows of the artwork with a strong silhouette: left palms and cliff, the sun, the waterfalls, the lower palms
    static const float windows[][2] = {{0.00F, 0.12F}, {0.30F, 0.22F}, {0.62F, 0.20F}, {0.70F, 0.62F}, {0.00F, 0.62F}, {0.18F, 0.26F}};
    const auto &win = windows[seed % 6U];
    const float crop_w = 0.42F + static_cast<float>((seed >> 4U) % 4U) * 0.04F;           // fraction of the art width
    const float crop_h = crop_w * kArtW / kArtH * (h / w);                                // keeps the art's aspect inside the card
    const float u0 = std::min(win[0], 1.0F - crop_w), v0 = std::min(win[1], 1.0F - crop_h);
    const bool mirror = ((seed >> 7U) & 1U) != 0U;
    const ImVec2 uv0(mirror ? u0 + crop_w : u0, v0), uv1(mirror ? u0 : u0 + crop_w, v0 + crop_h);
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(bg), a, b, uv0, uv1, sms ? rgba(118, 100, 132) : rgba(84, 104, 160), rounding);
  }
  dl->PushClipRect(a, b, true);
  dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + h * 0.35F), b, rgba(6, 14, 36, 40), rgba(6, 14, 36, 40), rgba(6, 14, 36, 235), rgba(6, 14, 36, 235));
  const float band = std::max(ctx_.u(3.0F), std::floor(h * 0.11F));
  if (w > ctx_.u(70.0F)) {
    dl->AddRectFilled(a, ImVec2(b.x, a.y + band), rgba(4, 10, 26, 235), rounding, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(a.x, a.y + band), ImVec2(b.x, a.y + band + std::max(1.0F, ctx_.u(2.0F))), accent);
    const char *system = sms ? "MASTER SYSTEM" : "GENESIS";
    float spx = std::min(band * 0.52F, ctx_.u(10.0F));
    while (spx > 5.0F && text_width(ctx_.display, spx, system) > w - ctx_.u(12.0F)) spx -= 0.5F;
    draw_text(dl, ctx_.display, spx, with_alpha(accent, 235), ImVec2(a.x + ctx_.u(7.0F), a.y + (band - spx) * 0.5F), system);
  } else {
    dl->AddRectFilled(a, ImVec2(b.x, a.y + std::max(2.0F, ctx_.u(2.0F))), accent);
  }
  if (w > ctx_.u(70.0F)) {
    // the title, wrapped in up to three Silkscreen lines in the lower part of the card
    float px = std::clamp(w * 0.115F, ctx_.u(9.0F), ctx_.u(18.0F));
    std::vector<std::string> lines;
    for (; px >= ctx_.u(8.0F); px -= 1.0F) {
      lines = uih::wrap(ctx_.display, px, w - ctx_.u(16.0F), entry.title());
      bool fits = lines.size() <= 3;
      for (const auto &l : lines) if (text_width(ctx_.display, px, l.c_str()) > w - ctx_.u(16.0F)) fits = false;
      if (fits) break;
    }
    if (lines.size() > 3) { lines.resize(3); lines[2] = ellipsize(ctx_.display, px, w - ctx_.u(16.0F), lines[2] + "..."); }
    const float cap = std::clamp(h * 0.045F, ctx_.u(7.0F), ctx_.u(9.0F));
    // under the platform band: the lower part of the card stays free for the library's build-state badge
    float ty = a.y + band + std::max(ctx_.u(10.0F), h * 0.07F);
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + band), ImVec2(b.x, ty + static_cast<float>(lines.size()) * (px + ctx_.u(3.0F)) + ctx_.u(14.0F)), rgba(4, 10, 28, 170), rgba(4, 10, 28, 170), rgba(4, 10, 28, 0), rgba(4, 10, 28, 0));
    for (const auto &l : lines) {
      draw_text(dl, ctx_.display, px, rgba(2, 6, 18, 230), ImVec2(a.x + ctx_.u(9.0F), ty + ctx_.u(1.5F)), l.c_str());
      draw_text(dl, ctx_.display, px, rgba(236, 244, 255), ImVec2(a.x + ctx_.u(8.0F), ty), l.c_str());
      ty += px + ctx_.u(3.0F);
    }
    const char *note = "NO COVER ART";
    draw_text(dl, ctx_.display, cap, rgba(150, 168, 210, 220), ImVec2(b.x - ctx_.u(8.0F) - text_width(ctx_.display, cap, note), b.y - ctx_.u(9.0F) - cap), note);
  } else {
    // thumbnails: the initials only
    const std::string text = initials(entry.title());
    float px = std::min(h * 0.32F, w * 0.42F);
    while (px > ctx_.u(7.0F) && text_width(ctx_.display, px, text.c_str()) > w - ctx_.u(6.0F)) px -= 1.0F;
    const ImVec2 tp((a.x + b.x) * 0.5F - text_width(ctx_.display, px, text.c_str()) * 0.5F, b.y - px - h * 0.16F);
    draw_text(dl, ctx_.display, px, rgba(2, 6, 18, 230), ImVec2(tp.x + 1.0F, tp.y + 1.0F), text.c_str());
    draw_text(dl, ctx_.display, px, rgba(236, 244, 255), tp, text.c_str());
  }
  dl->PopClipRect();
  dl->AddRect(a, b, with_alpha(accent, 110), rounding, 0, std::max(1.0F, ctx_.u(1.0F)));
  (void)tint;
}

} // namespace launcher
