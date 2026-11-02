/* SEG-124 (ADR 0267): the HUD overlay a verified native compatibility implementation attaches to an extended surface (it replaces the game's own HUD), read back from the
 * rendered window output (dummy video driver, software renderer; no display, audio device or ROM). The overlay is drawn after the side treatment in the LEFT side space while the
 * visible side holds it with its margins, and otherwise inside the native picture at the replaced HUD's place (4:3, a narrow slider, Original); it is independent of the side
 * style and of the surface provenance, never outlives the surface it came with, and never touches the native picture while it is in the side. Expected positions come from the layout arithmetic, never from the renderer. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

enum { W = 320, H = 224, SIDE = 39, PITCH = (W + 2 * SIDE) * 3, OW = 12, OH = 9 };
static const uint8_t FILL[3] = {10, 200, 233}, EDGE[3] = {250, 120, 10};

static int px(SDL_Surface *s, int x, int y, int *r, int *g, int *b) {
  Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
  if (!SDL_ReadSurfacePixel(s, x, y, &rr, &gg, &bb, &aa)) return 0;
  *r = rr; *g = gg; *b = bb;
  return 1;
}
static uint64_t region_hash(SDL_Surface *s, SvRect rc) {
  uint64_t h = 1469598103934665603ull;
  int x, y, r, g, b;
  for (y = rc.y; y < rc.y + rc.h; ++y)
    for (x = rc.x; x < rc.x + rc.w; ++x) { px(s, x, y, &r, &g, &b); h = (h ^ (uint64_t)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16))) * 1099511628211ull; }
  return h;
}
static int count_colour(SDL_Surface *s, SvRect rc, const uint8_t c[3]) {
  int x, y, r, g, b, n = 0;
  for (y = rc.y; y < rc.y + rc.h; ++y)
    for (x = rc.x; x < rc.x + rc.w; ++x) { px(s, x, y, &r, &g, &b); if (r == c[0] && g == c[1] && b == c[2]) ++n; }
  return n;
}
static void make_native(uint8_t *rgb) {
  int x, y;
  for (y = 0; y < H; ++y)
    for (x = 0; x < W; ++x) { uint8_t *p = rgb + ((size_t)y * W + (size_t)x) * 3u; p[0] = (uint8_t)(30 + x % 100); p[1] = (uint8_t)(40 + y % 90); p[2] = (uint8_t)(60 + (x + y) % 80); }
}
/* hard green/blue stripes at the sides (no colour of the overlay), a grey centre that must never show */
static void make_ext(uint8_t *s) {
  int x, y;
  for (y = 0; y < H; ++y)
    for (x = 0; x < W + 2 * SIDE; ++x) {
      uint8_t *p = s + (size_t)y * PITCH + (size_t)x * 3u;
      const int a = ((x / 8) & 1) != 0;
      if (x >= SIDE && x < SIDE + W) { p[0] = 77; p[1] = 77; p[2] = 77; } else { p[0] = 0; p[1] = a ? 255 : 90; p[2] = a ? 60 : 255; }
    }
}
/* an OW x OH panel: an EDGE ring around a FILL interior, fully opaque; a corner pixel transparent */
static void make_overlay(uint8_t *o, unsigned alpha_fill) {
  int x, y;
  for (y = 0; y < OH; ++y)
    for (x = 0; x < OW; ++x) {
      uint8_t *p = o + ((size_t)y * OW + (size_t)x) * 4u;
      const int ring = x == 0 || y == 0 || x == OW - 1 || y == OH - 1;
      memcpy(p, ring ? EDGE : FILL, 3);
      p[3] = ring ? 255 : (uint8_t)alpha_fill;
    }
  o[3] = 0; /* the top-left corner is transparent */
}
static SvExtendedSurface describe(const uint8_t *s, uint64_t seq, SvWidescreenMode mode, SvExtSource src) {
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  e.rgb24 = s; e.size = (size_t)PITCH * H; e.width = W + 2 * SIDE; e.height = H; e.pitch = PITCH; e.side = SIDE; e.sequence = seq; e.mode = mode; e.status = SV_WS_PARTIAL; e.source = src;
  return e;
}
static SvHost *open_host(const char *settings_file) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "overlay test"; cfg.picture_w = W; cfg.picture_h = H; cfg.max_w = W; cfg.max_h = 240; cfg.default_scale = 3;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings_file, 1);
  return sv_host_open(&cfg);
}
static void set(SvHost *h, SvSideStyle st, SvWidescreenMode ws, int width) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen_side_style = st; s.video.widescreen = ws; s.video.widescreen_width = width; s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(h, &s);
}
static SDL_Surface *shoot(SvHost *h) { sv_host_toast(h, ""); return sv_host_capture_output(h); }

int main(void) {
  char dir[512], settings_file[600];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  uint8_t *native = (uint8_t *)malloc((size_t)W * H * 3u), *ext = (uint8_t *)malloc((size_t)PITCH * H);
  uint8_t ovl[OW * OH * 4], ovl_half[OW * OH * 4], huge[(SV_OVERLAY_MAX + 1) * 4 * 4];
  SvHost *host;
  SvExtendedSurface e;
  SvExtendedLayout lay;
  uint64_t seq = 100;
  SDL_Surface *a, *b;
  snprintf(dir, sizeof dir, "%s/sv_overlay_test_%d", tmp, (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(settings_file, sizeof settings_file, "%s/viewer-settings.cfg", dir);
  remove(settings_file);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  host = open_host(settings_file);
  CHECK(host != NULL && native != NULL && ext != NULL, "setup");
  if (host == NULL || native == NULL || ext == NULL) return 1;
  make_native(native);
  make_ext(ext);
  make_overlay(ovl, 255);
  make_overlay(ovl_half, 128);
  memset(huge, 0xFF, sizeof huge);
  SDL_SetWindowSize(host->window, 1280, 720);

#define FX 20
#define FY 30
#define PRESENT_WITH(src, o, ow, oh) do { ++seq; e = describe(ext, seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, src); e.overlay_rgba = (o); e.overlay_w = (ow); e.overlay_h = (oh); e.overlay_fx = FX; e.overlay_fy = FY; \
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_extension_side(host) == SIDE, "extended frame accepted"); } while (0)

  /* --- full width: the panel sits in the LEFT side, centred in it, 8 rows below the top; the native picture and the right side never see it --- */
  {
    static const SvSideStyle styles[3] = {SV_SIDE_RAW, SV_SIDE_COMFORT, SV_SIDE_BORDERS};
    static const SvExtSource sources[2] = {SV_EXTSRC_GENERIC, SV_EXTSRC_AUTHORED};
    int si, so;
    for (si = 0; si < 3; ++si)
      for (so = 0; so < 2; ++so) {
        SvRect nat, left, right, panel;
        int scale, sx;
        char what[160];
        set(host, styles[si], SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
        PRESENT_WITH(sources[so], NULL, 0, 0);
        a = shoot(host);
        PRESENT_WITH(sources[so], ovl, OW, OH);
        b = shoot(host);
        CHECK(a != NULL && b != NULL && sv_layout_extended_dest(b->w, b->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, "layout");
        if (a == NULL || b == NULL) return 1;
        nat = lay.native;
        scale = nat.w / W;
        left.x = lay.surface.x; left.y = nat.y; left.w = nat.x - lay.surface.x; left.h = nat.h;
        right.x = nat.x + nat.w; right.y = nat.y; right.w = lay.surface.x + lay.surface.w - right.x; right.h = nat.h;
        sx = (SIDE - OW) / 2;
        panel.x = left.x + sx * scale; panel.y = nat.y + SV_OVERLAY_TOP * scale; panel.w = OW * scale; panel.h = OH * scale;
        snprintf(what, sizeof what, "style %d source %d: the native picture is bit-identical with and without the overlay", si, so);
        CHECK(region_hash(a, nat) == region_hash(b, nat), what);
        CHECK(count_colour(b, right, FILL) == 0 && count_colour(b, right, EDGE) == 0, "no overlay pixel in the right side");
        CHECK(count_colour(b, nat, FILL) == 0 && count_colour(b, nat, EDGE) == 0, "no overlay pixel over the native picture while it fits in the side");
        CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, left, EDGE) == 0, "without an overlay no panel colours exist (scene sanity)");
        {
          SvRect inner; int inner_total;
          inner.x = panel.x + scale; inner.y = panel.y + scale; inner.w = panel.w - 2 * scale; inner.h = panel.h - 2 * scale;
          inner_total = inner.w * inner.h;
          snprintf(what, sizeof what, "style %d source %d: the panel interior is at the computed position (%d of %d)", si, so, count_colour(b, inner, FILL), inner_total);
          CHECK(count_colour(b, inner, FILL) == inner_total, what);
          CHECK(count_colour(b, left, FILL) == (OW - 2) * (OH - 2) * scale * scale && count_colour(b, left, EDGE) == (2 * OW + 2 * OH - 4 - 1) * scale * scale, "exact panel pixel counts: interior and ring (the corner stays transparent)");
          {
            SvRect below, beside;
            below.x = left.x; below.y = panel.y + panel.h; below.w = left.w; below.h = left.y + left.h - below.y;
            beside.x = left.x; beside.y = left.y; beside.w = sx * scale - 1; beside.h = left.h;
            CHECK(region_hash(a, below) == region_hash(b, below) && region_hash(a, beside) == region_hash(b, beside), "the rest of the left side is untouched by the overlay");
          }
        }
        SDL_DestroySurface(a);
        SDL_DestroySurface(b);
      }
  }

  /* --- alpha: a half-transparent interior is the exact blend of the panel colour and what is behind it (Raw: the extension stripes) --- */
  {
    int scale, r, g, bl, x, y, bad = 0, n = 0;
    SvRect nat;
    set(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl_half, OW, OH);
    b = shoot(host);
    sv_layout_extended_dest(b->w, b->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay);
    nat = lay.native; scale = nat.w / W;
    for (y = 1; y < OH - 1; ++y)
      for (x = 1; x < OW - 1; ++x) {
        const int tx = (SIDE - OW) / 2 + x; /* surface column in the left strip */
        const int stripe = ((tx / 8) & 1) != 0;
        const int bg_g = stripe ? 255 : 90, bg_b = stripe ? 60 : 255;
        const int want_r = (0 * 127 + FILL[0] * 128 + 127) / 255, want_g = (bg_g * 127 + FILL[1] * 128 + 127) / 255, want_b = (bg_b * 127 + FILL[2] * 128 + 127) / 255;
        px(b, lay.surface.x + tx * scale + scale / 2, nat.y + (SV_OVERLAY_TOP + y) * scale + scale / 2, &r, &g, &bl);
        ++n;
        if (abs(r - want_r) > 1 || abs(g - want_g) > 1 || abs(bl - want_b) > 1) ++bad;
      }
    CHECK(bad == 0 && n == (OW - 2) * (OH - 2), "a half-transparent panel is the exact blend over the side behind it");
    SDL_DestroySurface(b);
  }

  /* --- width: the panel needs a visible side >= panel width + 2 margins; otherwise (including 4:3) it replaces the game's HUD inside the picture at its fallback place --- */
  {
    int width, in_side_widths = 0, in_picture_widths = 0, first_side = -1;
    for (width = 0; width <= 100; width += 1) {
      int vis, in_side;
      SvRect left, nat, whole_right, panel;
      set(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, width);
      PRESENT_WITH(SV_EXTSRC_AUTHORED, ovl, OW, OH);
      vis = sv_host_visible_side(host);
      in_side = vis >= OW + 2 * SV_OVERLAY_MARGIN;
      a = shoot(host);
      if (sv_layout_extended_dest(a->w, a->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) != 0) { CHECK(0, "layout"); return 1; }
      nat = lay.native;
      left.x = 0; left.y = nat.y; left.w = nat.x; left.h = nat.h;
      whole_right.x = nat.x + nat.w; whole_right.y = nat.y; whole_right.w = a->w - whole_right.x; whole_right.h = nat.h;
      {
        const int scale = nat.w / W;
        const int fill_left = count_colour(a, left, FILL), fill_nat = count_colour(a, nat, FILL), fill_right = count_colour(a, whole_right, FILL);
        panel.x = nat.x + FX * scale; panel.y = nat.y + FY * scale; panel.w = OW * scale; panel.h = OH * scale;
        if (in_side) {
          ++in_side_widths; if (first_side < 0) first_side = width;
          CHECK(fill_left > 0 && fill_nat == 0 && fill_right == 0, "overlay in the left side when it fits");
        } else {
          ++in_picture_widths;
          CHECK(fill_left == 0 && fill_right == 0 && fill_nat == (OW - 2) * (OH - 2) * scale * scale, "overlay at its in-picture fallback place when the side is too narrow (and 4:3)");
          CHECK(count_colour(a, panel, EDGE) == (2 * OW + 2 * OH - 4 - 1) * scale * scale, "the fallback panel is exactly at the fallback position");
        }
      }
      SDL_DestroySurface(a);
    }
    CHECK(in_picture_widths > 20 && in_side_widths > 20 && first_side > 0, "both regimes are exercised across the slider");
  }

  /* --- lifetime: an overlay never outlives its surface; a style change keeps it; Original keeps the HUD in the picture; a stale surface's overlay is never shown --- */
  {
    SvRect left, nat, right;
    set(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, OW, OH);
    a = shoot(host); sv_layout_extended_dest(a->w, a->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay); nat = lay.native;
    left.x = 0; left.y = nat.y; left.w = nat.x; left.h = nat.h;
    right.x = nat.x + nat.w; right.y = nat.y; right.w = a->w - right.x; right.h = nat.h;
    CHECK(count_colour(a, left, FILL) > 0, "precondition: the overlay is shown");
    SDL_DestroySurface(a);
    set(host, SV_SIDE_BORDERS, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    a = shoot(host); CHECK(count_colour(a, left, FILL) > 0, "a side-style change keeps the retained overlay"); SDL_DestroySurface(a);
    set(host, SV_SIDE_RAW, SV_WIDESCREEN_ORIGINAL, 100);
    a = shoot(host);
    sv_layout_extended_dest(a->w, a->h, W, H, 0, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay);
    CHECK(count_colour(a, left, FILL) == 0 || sv_host_visible_side(host) == 0, "Original: no side");
    SDL_DestroySurface(a);
    /* Original with a fresh surface: the extension is refused for the mode, but the HUD that replaces the game's own is drawn in the picture */
    ++seq; e = describe(ext, seq, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_EXTSRC_AUTHORED); e.overlay_rgba = ovl; e.overlay_w = OW; e.overlay_h = OH; e.overlay_fx = FX; e.overlay_fy = FY;
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_extension_side(host) == 0, "Original: the extension is not shown");
    a = shoot(host);
    { SvRect pic; int sc2; sv_layout_dest(a->w, a->h, W, H, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER) ; pic = sv_layout_dest(a->w, a->h, W, H, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER); sc2 = pic.w / W;
      CHECK(count_colour(a, pic, FILL) == (OW - 2) * (OH - 2) * sc2 * sc2, "Original: the HUD is drawn inside the picture at its fallback place"); }
    SDL_DestroySurface(a);
    set(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    PRESENT_WITH(SV_EXTSRC_GENERIC, NULL, 0, 0);
    a = shoot(host); CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, right, FILL) == 0, "the next surface without an overlay shows none (no stale panel)"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, OW, OH);
    ++seq; e = describe(ext, seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC); e.overlay_rgba = ovl; e.overlay_w = OW; e.overlay_h = OH;
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq + 5, &e) == 0 && sv_host_extension_side(host) == 0, "a stale-sequence surface is refused with its overlay");
    a = shoot(host);
    { SvRect pic = sv_layout_dest(a->w, a->h, W, H, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER); CHECK(count_colour(a, pic, FILL) == 0 && count_colour(a, left, FILL) == 0, "a refused surface's overlay is never shown"); }
    SDL_DestroySurface(a);
  }

  /* --- invalid overlays are ignored, the extension itself is still accepted --- */
  {
    SvRect nat, left;
    set(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    PRESENT_WITH(SV_EXTSRC_GENERIC, NULL, OW, OH);
    a = shoot(host); sv_layout_extended_dest(a->w, a->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay); nat = lay.native;
    left.x = 0; left.y = nat.y; left.w = nat.x; left.h = nat.h;
    CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, nat, FILL) == 0, "overlay pointer NULL with a size: ignored"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, 0, OH);
    a = shoot(host); CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, nat, FILL) == 0, "zero width: ignored"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, OW, -3);
    a = shoot(host); CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, nat, FILL) == 0, "negative height: ignored"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, huge, SV_OVERLAY_MAX + 1, 4);
    a = shoot(host); CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, nat, FILL) == 0, "oversize overlay: ignored, never read past its bound"); SDL_DestroySurface(a);
    /* a panel that fits neither the side nor the picture at its fallback place is hidden */
    {
      static uint8_t tall[SV_OVERLAY_MAX * SV_OVERLAY_MAX * 4];
      int i;
      for (i = 0; i < SV_OVERLAY_MAX * SV_OVERLAY_MAX; ++i) { memcpy(tall + i * 4, FILL, 3); tall[i * 4 + 3] = 255; }
      ++seq; e = describe(ext, seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC); e.overlay_rgba = tall; e.overlay_w = 40; e.overlay_h = SV_OVERLAY_MAX; e.overlay_fx = W - 10; e.overlay_fy = 0;
      CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0, "wide panel with an off-picture fallback");
      a = shoot(host); CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, nat, FILL) == 0, "a panel that fits nowhere is hidden (never partly outside the picture)"); SDL_DestroySurface(a);
    }
  }
  sv_host_close(host);
  free(native);
  free(ext);
  if (failures != 0) { printf("sv_overlay_test: %d failure(s)\n", failures); return 1; }
  printf("sv_overlay_test: OK\n");
  return 0;
}
