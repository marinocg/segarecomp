/* SEG-139: the shared SDL3 host presents extended surfaces planned for every supported aspect (4:3, 16:9, 21:9, 32:9 and a custom rational) under the dummy video
 * driver and the software renderer (no display, audio device or ROM). Expectations come from the planner and the scene; the picture is read back from the rendered
 * window. Covers: the planned side is what is shown, the native centre is pixel-identical at every aspect, the presented surface has the requested aspect in a window of
 * that shape and only letterboxes in any other, a 16:9 exact ratio equals the legacy 16:9 setting pixel for pixel, a rejected plan shows only the native picture, and
 * the memory the host holds per aspect (printed for the record). Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

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
static uint64_t full_hash(SDL_Surface *s) { SvRect a; a.x = 0; a.y = 0; a.w = s->w; a.h = s->h; return region_hash(s, a); }
static int colour_is(SDL_Surface *s, int x, int y, int r0, int g0, int b0) { int r, g, b; return px(s, x, y, &r, &g, &b) && r == r0 && g == g0 && b == b0; }

static void make_native(uint8_t *rgb, int w, int h) {
  int x, y;
  for (y = 0; y < h; ++y)
    for (x = 0; x < w; ++x) {
      uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
      const int on = ((x / 8) & 1) != 0;
      p[0] = on ? 240 : 20; p[1] = on ? 240 : 40; p[2] = on ? 20 : 220;
    }
}
/* extension: 16-pixel magenta/green stripes by absolute column, and a unique grey centre */
static void make_ext(uint8_t *s, int w, int side, int h, int pitch) {
  int x, y;
  memset(s, 0, (size_t)pitch * (size_t)h);
  for (y = 0; y < h; ++y)
    for (x = 0; x < w + 2 * side; ++x) {
      uint8_t *p = s + (size_t)y * (size_t)pitch + (size_t)x * 3u;
      const int a = ((x / 16) & 1) != 0;
      if (x >= side && x < side + w) { p[0] = 77; p[1] = 77; p[2] = 77; }
      else { p[0] = a ? 255 : 0; p[1] = a ? 0 : 255; p[2] = a ? 255 : 0; }
    }
}
static SvExtendedSurface describe(const uint8_t *s, int w, int side, int h, int pitch, uint64_t seq) {
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  e.rgb24 = s; e.size = (size_t)pitch * (size_t)h; e.width = w + 2 * side; e.height = h; e.pitch = pitch; e.side = side; e.sequence = seq;
  e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_AUTHORED; /* authored: the untreated surface, so columns are checkable */
  return e;
}

static char g_settings[700];
static SvHost *open_host(int w, int h) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "aspect test"; cfg.picture_w = w; cfg.picture_h = h <= 240 ? h : 224; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  return sv_host_open(&cfg);
}
static void set(SvHost *h, SvDisplayRatio ratio, int width, SvAspect asp, SvScaleMode sc) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY; s.video.widescreen_side_style = SV_SIDE_RAW; s.video.widescreen_width = width; s.video.widescreen_ratio = ratio;
  s.video.aspect = asp; s.video.scale = sc;
  sv_host_apply_settings(h, &s);
}
static SDL_Surface *shoot(SvHost *h) { sv_host_toast(h, ""); return sv_host_capture_output(h); }
static SvDisplayRatio R(int n, int d) { SvDisplayRatio r; r.num = n; r.den = d; return r; }

static void aspect_tests(int W, int H, SvAspect asp, int wanted_side[5]) {
  /* produced surface = the widest request (32:9); the visible side is what each aspect plans */
  static const SvDisplayRatio ratios[5] = {{4, 3}, {16, 9}, {21, 9}, {32, 9}, {64, 27}};
  static const struct { int w, h; } wins[] = {{1920, 1080}, {2560, 1080}, {3840, 1080}, {1024, 768}};
  SvGeomPlan widest;
  SvHost *host;
  uint8_t *native, *ext;
  uint64_t seq = 10;
  int i, k, wi;
  size_t pitch;
  (void)sv_geom_plan(W, H, asp, R(32, 9), &widest);
  pitch = (size_t)(W + 2 * widest.side) * 3u;
  native = (uint8_t *)malloc((size_t)W * (size_t)H * 3u);
  ext = (uint8_t *)malloc(pitch * (size_t)H);
  host = open_host(W, H);
  CHECK(host != NULL && native != NULL && ext != NULL && widest.side > 0, "matrix setup");
  if (host == NULL || native == NULL || ext == NULL) { free(native); free(ext); if (host != NULL) sv_host_close(host); return; }
  make_native(native, W, H);
  make_ext(ext, W, widest.side, H, (int)pitch);
  for (wi = 0; wi < (int)(sizeof wins / sizeof wins[0]); ++wi) {
    SDL_SetWindowSize(host->window, wins[wi].w, wins[wi].h);
    for (i = 0; i < 5; ++i) {
      SvGeomPlan plan;
      SvExtendedSurface e;
      SDL_Surface *a;
      SvExtendedLayout lay;
      CHECK(sv_geom_plan(W, H, asp, ratios[i], &plan) == SV_GEOM_OK, "the preset plans");
      CHECK(i == 4 || plan.side == wanted_side[i], "the planned side is the hand-calculated one");
      set(host, ratios[i], 100, asp, SV_SCALE_FIT);
      e = describe(ext, W, widest.side, H, (int)pitch, ++seq);
      CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_extension_side(host) == widest.side, "the extended frame is accepted");
      CHECK(sv_host_visible_side(host) == plan.side, "the visible side is the planned side");
      a = shoot(host);
      CHECK(a != NULL, "capture");
      if (a == NULL) continue;
      if (plan.side > 0) {
        CHECK(sv_layout_extended_dest(a->w, a->h, W, H, plan.side, asp, SV_SCALE_FIT, &lay) == 0, "layout");
        if (plan.side > 0) {
          const int col = widest.side - plan.side, par = ((col / 16) & 1) != 0;
          k = lay.native.w / W;
          CHECK(colour_is(a, lay.surface.x + 1, lay.native.y + 5 * (k > 0 ? k : 1), par ? 255 : 0, par ? 0 : 255, par ? 255 : 0), "the outermost visible column is the matching surface column");
        }
        /* the presented surface has the requested aspect to within the side rounding when the window is wide enough; otherwise it is only letterboxed */
        { const double got = (double)lay.surface.w / (double)lay.surface.h;
          const double base = asp == SV_ASPECT_4_3 ? (4.0 / 3.0) : (double)W / (double)H;
          const double expect = base * (double)plan.surface_w / (double)W;
          CHECK(got > expect * 0.98 && got < expect * 1.02, "the presented surface keeps the planned aspect (letterboxed, never stretched)");
          CHECK(lay.surface.x >= 0 && lay.surface.x + lay.surface.w <= a->w && lay.surface.y >= 0 && lay.surface.y + lay.surface.h <= a->h, "the presented surface is inside the window"); }
      } else {
        lay.native = sv_layout_dest(a->w, a->h, W, H, asp, SV_SCALE_FIT);
      }
      SDL_DestroySurface(a);
    }
  }
  /* the native centre is exactly the native picture at every aspect (square pixels, integer scale): every sampled source pixel lands on its own colour */
  if (asp == SV_ASPECT_ORIGINAL) {
    SDL_SetWindowSize(host->window, 1920, 1080);
    for (i = 0; i < 4; ++i) {
      SvGeomPlan plan;
      SvExtendedSurface e;
      SDL_Surface *a;
      SvExtendedLayout lay;
      int sx, sy, bad = 0, kk;
      (void)sv_geom_plan(W, H, asp, ratios[i], &plan);
      set(host, ratios[i], 100, asp, SV_SCALE_INTEGER);
      e = describe(ext, W, widest.side, H, (int)pitch, ++seq);
      CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0, "accepted");
      a = shoot(host);
      if (a == NULL) { CHECK(0, "capture"); continue; }
      if (plan.side > 0) (void)sv_layout_extended_dest(a->w, a->h, W, H, plan.side, asp, SV_SCALE_INTEGER, &lay);
      else lay.native = sv_layout_dest(a->w, a->h, W, H, asp, SV_SCALE_INTEGER);
      kk = lay.native.w / W;
      CHECK(kk >= 1 && lay.native.w == kk * W && lay.native.h == kk * H, "the native rectangle is a whole-number multiple of the picture");
      for (sy = 0; sy < H; sy += 13)
        for (sx = 0; sx < W; sx += 5) {
          const int on = ((sx / 8) & 1) != 0;
          if (!colour_is(a, lay.native.x + sx * kk + kk / 2, lay.native.y + sy * kk + kk / 2, on ? 240 : 20, on ? 240 : 40, on ? 20 : 220)) ++bad;
        }
      CHECK(bad == 0, "the native centre is pixel-exact at every aspect");
      SDL_DestroySurface(a);
    }
  }
  sv_host_close(host);
  free(native); free(ext);
}

/* an exact 16:9 ratio is the legacy 16:9 setting, pixel for pixel; a rejected plan shows only the native picture */
static void legacy_and_rejection_tests(void) {
  enum { W = 320, H = 224, SIDE = 39, PITCH = (W + 2 * SIDE) * 3 };
  uint8_t *native = (uint8_t *)malloc((size_t)W * H * 3u), *ext = (uint8_t *)malloc((size_t)PITCH * H);
  SvHost *host = open_host(W, H);
  SvExtendedSurface e;
  SDL_Surface *legacy, *exact;
  uint64_t seq = 500;
  CHECK(host != NULL && native != NULL && ext != NULL, "legacy: setup");
  if (host == NULL || native == NULL || ext == NULL) { free(native); free(ext); if (host != NULL) sv_host_close(host); return; }
  SDL_SetWindowSize(host->window, 1920, 1080);
  make_native(native, W, H);
  make_ext(ext, W, SIDE, H, PITCH);
  e = describe(ext, W, SIDE, H, PITCH, ++seq);
  set(host, R(0, 0), 100, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_visible_side(host) == SIDE, "legacy 16:9 shows the produced side");
  legacy = shoot(host);
  set(host, R(16, 9), 0, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER); /* the exact ratio overrides whatever the legacy width says */
  CHECK(sv_host_visible_side(host) == SIDE, "exact 16:9 shows the same side");
  exact = shoot(host);
  CHECK(legacy != NULL && exact != NULL && full_hash(legacy) == full_hash(exact), "exact 16:9 equals the legacy 16:9 setting pixel for pixel");
  if (legacy != NULL) SDL_DestroySurface(legacy);
  if (exact != NULL) SDL_DestroySurface(exact);
  /* a request wider than was produced shows what exists, never more */
  set(host, R(32, 9), 100, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_visible_side(host) == SIDE, "a wider request never shows more than the producer supplied");
  sv_host_close(host);
  free(native); free(ext);

  /* the rejection: a 200 x 448 picture at 32:9 needs 696 per side (> 672); the host shows the native picture only, whatever surface a producer might hand over */
  { enum { W2 = 200, H2 = 448, S2 = 400 };
    const int pitch2 = (W2 + 2 * S2) * 3;
    uint8_t *n2 = (uint8_t *)malloc((size_t)W2 * H2 * 3u), *x2 = (uint8_t *)malloc((size_t)pitch2 * H2);
    SvHost *h2 = open_host(W2, H2);
    SvExtendedSurface e2;
    CHECK(h2 != NULL && n2 != NULL && x2 != NULL, "rejection: setup");
    if (h2 != NULL && n2 != NULL && x2 != NULL) {
      make_native(n2, W2, H2);
      make_ext(x2, W2, S2, H2, pitch2);
      e2 = describe(x2, W2, S2, H2, pitch2, ++seq);
      set(h2, R(32, 9), 100, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
      CHECK(sv_host_present_extended(h2, n2, W2, H2, W2 * 3, seq, &e2) == 0 && sv_host_visible_side(h2) == 0, "a rejected plan shows only the native picture");
    }
    if (h2 != NULL) sv_host_close(h2);
    free(n2); free(x2); }
}

/* memory held by the host for the largest surface of each aspect (recorded; the allocation is fixed at open, never per frame) */
static void memory_report(void) {
  static const SvDisplayRatio ratios[4] = {{4, 3}, {16, 9}, {21, 9}, {32, 9}};
  int i;
  for (i = 0; i < 4; ++i) {
    SvGeomPlan p;
    (void)sv_geom_plan(320, 448, SV_ASPECT_ORIGINAL, ratios[i], &p);
    printf("SEG-139 host memory: 320x448 %d:%d -> producer surface %zu bytes, side buffers %zu bytes, texture %zu bytes\n", ratios[i].num, ratios[i].den, p.surface_bytes,
           (size_t)2u * (size_t)p.side * 448u * 3u, p.surface_bytes);
  }
  printf("SEG-139 host fixed allocation (any aspect): ext_raw %zu bytes, side buffers %zu bytes\n", (size_t)(320 + 2 * SV_EXT_MAX_SIDE) * 448u * 3u, (size_t)2u * SV_EXT_MAX_SIDE * 448u * 3u);
}

int main(void) {
  char dir[512];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  /* hand-calculated planned sides (source pixels per side) for 4:3, 16:9, 21:9, 32:9 */
  int h40_square[5] = {0, 39, 101, 238, 0};
  int h40_corrected[5] = {0, 53, 120, 266, 0};
  int h40_interlaced[5] = {138, 238, 362, 636, 0};
  snprintf(dir, sizeof dir, "%s/sv_aspect_matrix_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  snprintf(g_settings, sizeof g_settings, "%s/m1.cfg", dir);
  aspect_tests(320, 224, SV_ASPECT_ORIGINAL, h40_square);
  remove(g_settings);
  snprintf(g_settings, sizeof g_settings, "%s/m2.cfg", dir);
  aspect_tests(320, 224, SV_ASPECT_4_3, h40_corrected);
  remove(g_settings);
  snprintf(g_settings, sizeof g_settings, "%s/m3.cfg", dir);
  aspect_tests(320, 448, SV_ASPECT_ORIGINAL, h40_interlaced);
  remove(g_settings);
  snprintf(g_settings, sizeof g_settings, "%s/m4.cfg", dir);
  legacy_and_rejection_tests();
  remove(g_settings);
  memory_report();
  if (failures != 0) { printf("sv_aspect_matrix_test: %d failure(s)\n", failures); return 1; }
  printf("sv_aspect_matrix_test: OK\n");
  return 0;
}
