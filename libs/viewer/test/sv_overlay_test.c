/* SEG-124 (ADR 0267): the overlay a verified native compatibility implementation attaches to an extended surface (the game's HUD, relocated), read back from the rendered window output
 * (dummy video driver, software renderer; no display, audio device or ROM). Its rows are picture rows 1:1; the host draws it after the side treatment with its left edge at the left
 * edge of the VISIBLE side (only the horizontal position differs from the original), or inside the picture at the place of the HUD it replaces when there is no visible side
 * (4:3, a zero slider, Original); fully transparent pixels leave the picture alone (no panel); it is independent of the side style and of the provenance and never outlives its surface.
 * Expected positions come from the layout arithmetic, never from the renderer. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* overlay: OW x OH, an EDGE ring (1 pixel), a transparent interior and one opaque FILL block at (10..19, 10..19) */
enum { W = 320, H = 224, SIDE = 39, PITCH = (W + 2 * SIDE) * 3, OW = 60, OH = 40, FX = 16, FY = 0 };
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
static void make_overlay(uint8_t *o) {
  int x, y;
  memset(o, 0, (size_t)OW * OH * 4u);
  for (y = 0; y < OH; ++y)
    for (x = 0; x < OW; ++x) {
      uint8_t *p = o + ((size_t)y * OW + (size_t)x) * 4u;
      if (x == 0 || y == 0 || x == OW - 1 || y == OH - 1) { memcpy(p, EDGE, 3); p[3] = 255; }
      else if (x >= 10 && x < 20 && y >= 10 && y < 20) { memcpy(p, FILL, 3); p[3] = 255; }
    }
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
  uint8_t ovl[OW * OH * 4], huge[8];
  SvHost *host;
  SvExtendedSurface e;
  SvExtendedLayout lay;
  uint64_t seq = 100;
  SDL_Surface *a, *b;
  const int ring = 2 * OW + 2 * OH - 4, fill_px = 100;
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
  make_overlay(ovl);
  memset(huge, 0xFF, sizeof huge);
  SDL_SetWindowSize(host->window, 1280, 720);

#define PRESENT_WITH(src, o, ow, oh) do { ++seq; e = describe(ext, seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, src); e.overlay_rgba = (o); e.overlay_w = (ow); e.overlay_h = (oh); e.overlay_fx = FX; e.overlay_fy = FY; \
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_extension_side(host) == SIDE, "extended frame accepted"); } while (0)

  /* --- full width: the overlay's left edge is the surface's left edge; it covers the left side and 21 picture columns; rows are picture rows 1:1 --- */
  {
    static const SvSideStyle styles[3] = {SV_SIDE_RAW, SV_SIDE_COMFORT, SV_SIDE_BORDERS};
    static const SvExtSource sources[2] = {SV_EXTSRC_GENERIC, SV_EXTSRC_AUTHORED};
    int si, so;
    for (si = 0; si < 3; ++si)
      for (so = 0; so < 2; ++so) {
        SvRect nat, left, right, outside, in_pic, in_side;
        int scale;
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
        in_side.x = left.x; in_side.y = nat.y; in_side.w = left.w; in_side.h = OH * scale;
        in_pic.x = nat.x; in_pic.y = nat.y; in_pic.w = nat.w; in_pic.h = nat.h;
        outside.x = nat.x + (OW - SIDE) * scale; outside.y = nat.y; outside.w = nat.w - (OW - SIDE) * scale; outside.h = nat.h;
        snprintf(what, sizeof what, "style %d source %d: the picture is bit-identical beyond the overlay's columns", si, so);
        CHECK(region_hash(a, outside) == region_hash(b, outside), what);
        { SvRect below = in_pic; below.y = nat.y + OH * scale; below.h = nat.h - OH * scale; CHECK(region_hash(a, below) == region_hash(b, below), "the picture below the overlay's rows is untouched"); }
        CHECK(count_colour(b, right, FILL) == 0 && count_colour(b, right, EDGE) == 0, "no overlay pixel in the right side");
        CHECK(count_colour(a, left, FILL) == 0 && count_colour(a, in_pic, EDGE) == 0, "without an overlay no overlay colours exist (scene sanity)");
        /* opaque pixels: the fill block at its exact computed place, every ring pixel somewhere inside the overlay's rectangle (left side + 21 picture columns) */
        { SvRect block; block.x = lay.surface.x + 10 * scale; block.y = nat.y + 10 * scale; block.w = 10 * scale; block.h = 10 * scale;
          CHECK(count_colour(b, block, FILL) == fill_px * scale * scale, "the opaque block is at its computed position");
          CHECK(count_colour(b, in_side, EDGE) + count_colour(b, in_pic, EDGE) == ring * scale * scale, "every ring pixel is drawn exactly once");
          CHECK(count_colour(b, in_pic, EDGE) > 0, "the overlay reaches into the picture columns (21 of them) where it is opaque");
        }
        /* a transparent pixel keeps what is behind it: an interior point shows the side/picture exactly as without the overlay */
        { const int tx = lay.surface.x + 30 * scale + scale / 2, ty = nat.y + 30 * scale + scale / 2; int r1, g1, b1, r2, g2, b2;
          px(a, tx, ty, &r1, &g1, &b1); px(b, tx, ty, &r2, &g2, &b2);
          CHECK(r1 == r2 && g1 == g2 && b1 == b2, "transparent pixels leave the picture alone (no panel)"); }
        SDL_DestroySurface(a);
        SDL_DestroySurface(b);
      }
  }

  /* --- the width slider: with a visible side the overlay's left edge follows the visible side; without one it sits inside the picture at its fallback place --- */
  {
    int width, in_side_n = 0, in_pic_n = 0;
    for (width = 0; width <= 100; width += 1) {
      int vis, scale;
      SvRect nat;
      set(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, width);
      PRESENT_WITH(SV_EXTSRC_AUTHORED, ovl, OW, OH);
      vis = sv_host_visible_side(host);
      a = shoot(host);
      if (sv_layout_extended_dest(a->w, a->h, W, H, vis, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) != 0) { CHECK(0, "layout"); return 1; }
      nat = lay.native; scale = nat.w / W;
      { SvRect block;
        if (vis > 0) { ++in_side_n; block.x = lay.surface.x + 10 * scale; }          /* left edge of the visible side */
        else { ++in_pic_n; block.x = nat.x + (FX + 10) * scale; }                    /* the original's place */
        block.y = nat.y + (FY + 10) * scale; block.w = 10 * scale; block.h = 10 * scale;
        CHECK(count_colour(a, block, FILL) == fill_px * scale * scale, "the opaque block is where the visible side (or the fallback place) puts it"); }
      if (vis == 0) CHECK(count_colour(a, nat, FILL) == fill_px * scale * scale && count_colour(a, nat, EDGE) == ring * scale * scale, "with no visible side the whole overlay is inside the picture");
      SDL_DestroySurface(a);
    }
    CHECK(in_side_n > 20 && in_pic_n >= 1, "both regimes are exercised across the slider");
  }

  /* --- lifetime: a style change keeps the overlay; Original keeps it in the picture; the next surface without one shows none; a refused surface's overlay is never shown --- */
  {
    SvRect nat, pic;
    set(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, OW, OH);
    a = shoot(host); sv_layout_extended_dest(a->w, a->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay); nat = lay.native;
    CHECK(count_colour(a, lay.surface, FILL) == fill_px * (nat.w / W) * (nat.w / W), "precondition: the overlay is shown");
    SDL_DestroySurface(a);
    set(host, SV_SIDE_BORDERS, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    a = shoot(host); CHECK(count_colour(a, lay.surface, FILL) == fill_px * (nat.w / W) * (nat.w / W), "a side-style change keeps the retained overlay"); SDL_DestroySurface(a);
    set(host, SV_SIDE_RAW, SV_WIDESCREEN_ORIGINAL, 100);
    ++seq; e = describe(ext, seq, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_EXTSRC_AUTHORED); e.overlay_rgba = ovl; e.overlay_w = OW; e.overlay_h = OH; e.overlay_fx = FX; e.overlay_fy = FY;
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_extension_side(host) == 0, "Original: the extension is not shown");
    a = shoot(host);
    pic = sv_layout_dest(a->w, a->h, W, H, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    CHECK(count_colour(a, pic, FILL) == fill_px * (pic.w / W) * (pic.w / W) && count_colour(a, pic, EDGE) == ring * (pic.w / W) * (pic.w / W), "Original: the HUD is drawn inside the picture at its fallback place");
    SDL_DestroySurface(a);
    set(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    PRESENT_WITH(SV_EXTSRC_GENERIC, NULL, 0, 0);
    a = shoot(host); CHECK(count_colour(a, lay.surface, FILL) == 0, "the next surface without an overlay shows none (no stale overlay)"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, OW, OH);
    ++seq; e = describe(ext, seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC); e.overlay_rgba = ovl; e.overlay_w = OW; e.overlay_h = OH;
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq + 5, &e) == 0 && sv_host_extension_side(host) == 0, "a stale-sequence surface is refused with its overlay");
    a = shoot(host); pic = sv_layout_dest(a->w, a->h, W, H, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    CHECK(count_colour(a, pic, FILL) == 0 && count_colour(a, lay.surface, FILL) == 0, "a refused surface's overlay is never shown"); SDL_DestroySurface(a);
  }

  /* --- invalid overlays are ignored (the extension itself is still accepted); an overlay larger than the surface is clipped, never read past its bound --- */
  {
    SvRect all;
    set(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, 100);
    PRESENT_WITH(SV_EXTSRC_GENERIC, NULL, OW, OH);
    a = shoot(host); all.x = 0; all.y = 0; all.w = a->w; all.h = a->h;
    CHECK(count_colour(a, all, FILL) == 0, "overlay pointer NULL with a size: ignored"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, 0, OH);
    a = shoot(host); CHECK(count_colour(a, all, FILL) == 0, "zero width: ignored"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, ovl, OW, -3);
    a = shoot(host); CHECK(count_colour(a, all, FILL) == 0, "negative height: ignored"); SDL_DestroySurface(a);
    PRESENT_WITH(SV_EXTSRC_GENERIC, huge, SV_OVERLAY_MAX + 1, 1);
    a = shoot(host); CHECK(count_colour(a, all, FILL) == 0, "oversize overlay: ignored, never read past its bound"); SDL_DestroySurface(a);
    {   /* taller than the picture and wider than the surface: clipped */
      static uint8_t big[SV_OVERLAY_MAX * SV_OVERLAY_MAX * 4];
      int i;
      for (i = 0; i < SV_OVERLAY_MAX * SV_OVERLAY_MAX; ++i) { memcpy(big + i * 4, FILL, 3); big[i * 4 + 3] = 255; }
      ++seq; e = describe(ext, seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC); e.overlay_rgba = big; e.overlay_w = SV_OVERLAY_MAX; e.overlay_h = SV_OVERLAY_MAX; e.overlay_fx = 0; e.overlay_fy = 0;
      CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0, "a full-size overlay is accepted");
      a = shoot(host); sv_layout_extended_dest(a->w, a->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay);
      { const int sc = lay.native.w / W; SvRect rc = lay.surface; rc.w = SV_OVERLAY_MAX * sc; if (rc.w > lay.surface.w) rc.w = lay.surface.w; rc.h = lay.surface.h;
        CHECK(count_colour(a, rc, FILL) == rc.w * rc.h, "the oversized overlay covers exactly the surface rows and columns it reaches (clipped at the picture's bottom)"); }
      SDL_DestroySurface(a);
    }
  }
  sv_host_close(host);
  free(native);
  free(ext);
  if (failures != 0) { printf("sv_overlay_test: %d failure(s)\n", failures); return 1; }
  printf("sv_overlay_test: OK\n");
  return 0;
}
