/* SEG-140-T004 (ADR 0303): bounded, positioned, layered overlays. Part 1 is the pure planner and blend (anchors at 4:3 and 32:9 against hand-calculated positions, every rejection reason, z order,
 * budget, clip). Part 2 presents real frames through the shared host in an integer 2x window and reads the composed output back: anchors at 4:3 and 32:9, layer order above the fixed HUD, an
 * out-of-bounds layer rejected with a diagnostic while the others are still drawn, and the fixed HUD alone presented exactly as before. Expected positions come from layout arithmetic, never
 * from the renderer. Strict C11. */
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

#define NW 320
#define NH 224

static uint8_t g_img[4][256 * 256 * 4];

static void solid(uint8_t *p, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  int i;
  for (i = 0; i < w * h; ++i) { p[i * 4] = r; p[i * 4 + 1] = g; p[i * 4 + 2] = b; p[i * 4 + 3] = a; }
}
static SvOverlayLayer layer(const uint8_t *img, int w, int h, int anchor, int ref, int z, int dx, int dy) {
  SvOverlayLayer l;
  memset(&l, 0, sizeof l);
  l.rgba = img; l.w = w; l.h = h; l.anchor = anchor; l.reference = ref; l.z = z; l.dx = dx; l.dy = dy;
  return l;
}

/* ---------------------------------------------------------------------------------------------------------------- part 1: the pure planner */
static void plan_anchors(void) {
  /* hand-calculated: presentation width = 320 + 2 * side; a 16 x 8 image at each anchor, no offset */
  static const int sides[2] = {0, 238};   /* 4:3 and 32:9 */
  int si, a, ref;
  for (si = 0; si < 2; ++si)
    for (ref = 0; ref < 2; ++ref)
      for (a = 0; a < SV_OVA_COUNT; ++a) {
        const int side = sides[si];
        const int rx = ref == SV_OVREF_PICTURE ? side : 0, rw = ref == SV_OVREF_PICTURE ? NW : NW + 2 * side;
        const int hx[3] = {rx, rx + (rw - 16) / 2, rx + rw - 16};
        const int hy[3] = {0, (NH - 8) / 2, NH - 8};
        SvOverlayLayer l = layer(g_img[0], 16, 8, a, ref, 0, 0, 0);
        SvOverlayPlan p;
        char what[120];
        sv_overlay_plan(&l, 1, NW, NH, side, &p);
        snprintf(what, sizeof what, "side %d ref %d anchor %d is placed at the hand-calculated position", side, ref, a);
        CHECK(p.placed == 1 && p.rejected == 0 && p.place[0].x == hx[a % 3] && p.place[0].y == hy[a / 3] && p.place[0].w == 16 && p.place[0].h == 8, what);
      }
  { /* the headline cases spelled out: top-right of the presentation is the picture's own corner at 4:3 and the far edge of the extended surface at 32:9 */
    SvOverlayLayer l = layer(g_img[0], 16, 16, SV_OVA_TOP_RIGHT, SV_OVREF_PRESENTATION, 0, 0, 0);
    SvOverlayPlan p;
    sv_overlay_plan(&l, 1, NW, NH, 0, &p);
    CHECK(p.placed == 1 && p.place[0].x == 304, "4:3: top-right is x = 320 - 16 = 304");
    sv_overlay_plan(&l, 1, NW, NH, 238, &p);
    CHECK(p.placed == 1 && p.place[0].x == 780, "32:9: top-right is x = 796 - 16 = 780");
    l.reference = SV_OVREF_PICTURE;
    sv_overlay_plan(&l, 1, NW, NH, 238, &p);
    CHECK(p.placed == 1 && p.place[0].x == 238 + 320 - 16, "32:9 anchored to the picture: the picture's own top-right corner (542)");
  }
}

static void plan_rejections(void) {
  SvOverlayPlan p;
  SvOverlayLayer l, set[6];
  /* each rule, alone */
  struct { const char *name; SvOvReject want; SvOverlayLayer l; } c[12];
  int i, n = 0;
  c[n].name = "no image"; c[n].want = SV_OVR_NO_IMAGE; c[n].l = layer(NULL, 8, 8, 0, 0, 0, 0, 0); ++n;
  c[n].name = "zero width"; c[n].want = SV_OVR_SIZE; c[n].l = layer(g_img[0], 0, 8, 0, 0, 0, 0, 0); ++n;
  c[n].name = "width 257"; c[n].want = SV_OVR_SIZE; c[n].l = layer(g_img[0], 257, 8, 0, 0, 0, 0, 0); ++n;
  c[n].name = "anchor 9"; c[n].want = SV_OVR_ANCHOR; c[n].l = layer(g_img[0], 8, 8, 9, 0, 0, 0, 0); ++n;
  c[n].name = "negative anchor"; c[n].want = SV_OVR_ANCHOR; c[n].l = layer(g_img[0], 8, 8, -1, 0, 0, 0, 0); ++n;
  c[n].name = "reference 2"; c[n].want = SV_OVR_REFERENCE; c[n].l = layer(g_img[0], 8, 8, 0, 2, 0, 0, 0); ++n;
  c[n].name = "z 4"; c[n].want = SV_OVR_Z; c[n].l = layer(g_img[0], 8, 8, 0, 0, 4, 0, 0); ++n;
  c[n].name = "offset 2049"; c[n].want = SV_OVR_OFFSET; c[n].l = layer(g_img[0], 8, 8, 0, 0, 0, 2049, 0); ++n;
  c[n].name = "left of the reference"; c[n].want = SV_OVR_OUT_OF_BOUNDS; c[n].l = layer(g_img[0], 8, 8, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, -1, 0); ++n;
  c[n].name = "below the picture"; c[n].want = SV_OVR_OUT_OF_BOUNDS; c[n].l = layer(g_img[0], 8, 8, SV_OVA_BOTTOM_RIGHT, SV_OVREF_PICTURE, 0, 0, 1); ++n;
  c[n].name = "taller than the picture"; c[n].want = SV_OVR_OUT_OF_BOUNDS; c[n].l = layer(g_img[0], 8, 230, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 0, 0); ++n;
  c[n].name = "empty clip"; c[n].want = SV_OVR_CLIP; c[n].l = layer(g_img[0], 8, 8, 0, 0, 0, 0, 0); c[n].l.has_clip = 1; c[n].l.clip_w = 0; c[n].l.clip_h = 4; ++n;
  for (i = 0; i < n; ++i) {
    char what[120];
    sv_overlay_plan(&c[i].l, 1, NW, NH, 0, &p);
    snprintf(what, sizeof what, "rejected with the stable reason: %s", c[i].name);
    CHECK(p.placed == 0 && p.rejected == 1 && p.reject[0].index == 0 && p.reject[0].reason == c[i].want, what);
  }
  /* a layer exactly on the boundary is accepted (inclusive), one pixel more is not; the picture reference excludes the side, the presentation reference includes it */
  l = layer(g_img[0], 8, 8, SV_OVA_TOP_LEFT, SV_OVREF_PRESENTATION, 0, 0, 0);
  sv_overlay_plan(&l, 1, NW, NH, 38, &p);
  CHECK(p.placed == 1 && p.place[0].x == 0, "presentation: the left edge of the visible side is inside");
  l.reference = SV_OVREF_PICTURE; l.dx = -1;
  sv_overlay_plan(&l, 1, NW, NH, 38, &p);
  CHECK(p.placed == 0 && p.reject[0].reason == SV_OVR_OUT_OF_BOUNDS, "picture: one pixel into the side is out of bounds");
  /* clip rectangle: exactly inside the image is fine, one pixel past is not; the drawn part is reported */
  l = layer(g_img[0], 8, 8, 0, 0, 0, 0, 0); l.has_clip = 1; l.clip_x = 2; l.clip_y = 3; l.clip_w = 6; l.clip_h = 5;
  sv_overlay_plan(&l, 1, NW, NH, 0, &p);
  CHECK(p.placed == 1 && p.place[0].cx0 == 2 && p.place[0].cy0 == 3 && p.place[0].cx1 == 8 && p.place[0].cy1 == 8, "a clip inside the image is accepted and reported");
  l.clip_w = 7;
  sv_overlay_plan(&l, 1, NW, NH, 0, &p);
  CHECK(p.placed == 0 && p.reject[0].reason == SV_OVR_CLIP, "a clip one pixel past the image is rejected");
  /* count: the fifth layer is rejected as COUNT, the first four are placed */
  for (i = 0; i < 6; ++i) set[i] = layer(g_img[0], 8, 8, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, i % 4, i * 10, 0);
  sv_overlay_plan(set, 6, NW, NH, 0, &p);
  CHECK(p.placed == 4 && p.rejected == 2 && p.reject[0].index == 4 && p.reject[0].reason == SV_OVR_COUNT && p.reject[1].index == 5 && p.reject[1].reason == SV_OVR_COUNT, "layers beyond four are rejected as count");
  /* budget: 200 x 200 = 40000; a second one would make 80000 > 65536; a small one after the rejected one still fits */
  set[0] = layer(g_img[0], 200, 200, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 0, 0);
  set[1] = layer(g_img[1], 200, 200, SV_OVA_TOP_RIGHT, SV_OVREF_PICTURE, 1, 0, 0);
  set[2] = layer(g_img[2], 100, 100, SV_OVA_CENTER, SV_OVREF_PICTURE, 2, 0, 0);
  sv_overlay_plan(set, 3, NW, NH, 0, &p);
  CHECK(p.placed == 2 && p.rejected == 1 && p.reject[0].index == 1 && p.reject[0].reason == SV_OVR_BUDGET && p.pixels == 40000 + 10000, "over-budget layer rejected; the one that still fits is placed");
  CHECK(sv_ovl_budget_ok(0, 256, 256) == 1 && sv_ovl_budget_ok(1, 256, 256) == 0 && sv_ovl_budget_ok(0, 257, 1) == 0 && sv_ovl_budget_ok(-1, 1, 1) == 0, "the budget is exactly 65536 pixels and refuses invalid sizes");
  /* nothing planned for no input */
  sv_overlay_plan(NULL, 3, NW, NH, 0, &p);
  CHECK(p.placed == 0 && p.rejected == 0, "no layers, no plan");
}

static void plan_order_and_blend(void) {
  SvOverlayPlan p;
  SvOverlayLayer set[3];
  uint8_t rgb[16 * 16 * 3];
  uint8_t px[4] = {200, 100, 50, 128};
  memset(set, 0, sizeof set);
  set[0] = layer(g_img[0], 8, 8, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 2, 0, 0);
  set[1] = layer(g_img[1], 8, 8, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 0, 0);
  set[2] = layer(g_img[2], 8, 8, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 0, 0);
  sv_overlay_plan(set, 3, NW, NH, 0, &p);
  CHECK(p.placed == 3 && p.place[0].index == 1 && p.place[1].index == 2 && p.place[2].index == 0, "draw order is z ascending, equal z keeps submission order");
  /* blend: straight alpha over (10, 20, 30) with (200, 100, 50, 128): (10*127 + 200*128 + 127) / 255 = 105 etc. */
  { uint8_t one[4 * 4] = {0}; SvOverlayLayer l; SvOverlayPlan q; int i;
    for (i = 0; i < 4; ++i) memcpy(one + i * 4, px, 4);
    one[3] = 0; one[7] = 255; one[11] = 128;   /* pixel 0 fully transparent, 1 opaque, 2 half, 3 half */
    memset(rgb, 0, sizeof rgb);
    for (i = 0; i < 4; ++i) { rgb[i * 3] = 10; rgb[i * 3 + 1] = 20; rgb[i * 3 + 2] = 30; }
    l = layer(one, 4, 1, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 0, 0);
    sv_overlay_plan(&l, 1, 4, 1, 0, &q);
    sv_overlay_compose(rgb, 4, 1, 12, &l, &q);
    CHECK(rgb[0] == 10 && rgb[1] == 20 && rgb[2] == 30, "a fully transparent pixel leaves the surface unchanged");
    CHECK(rgb[3] == 200 && rgb[4] == 100 && rgb[5] == 50, "an opaque pixel replaces it");
    CHECK(rgb[6] == 105 && rgb[7] == 60 && rgb[8] == 40, "a half-transparent pixel blends with the fixed overlay's rounding (105, 60, 40)");
  }
}

/* ---------------------------------------------------------------------------------------------------------------- part 2: through the host */
static char g_settings[700];
static uint8_t g_native[NW * NH * 3];
static uint8_t g_surface[(NW + 2 * 238) * NH * 3];
static uint8_t g_hud[32 * 16 * 4];

static void native_px(int x, int y, uint8_t *o) { o[0] = (uint8_t)((x * 3 + y) & 127); o[1] = (uint8_t)((y * 5 + (x >> 3) * 17) & 127); o[2] = (uint8_t)(((x ^ y) * 7) & 127); }
static void ext_px(int d, int y, uint8_t *o) { o[0] = (uint8_t)(20 + (d & 31)); o[1] = (uint8_t)(60 + (y & 31)); o[2] = 20; }
static void build(int side) {
  int x, y;
  const int w = NW + 2 * side;
  for (y = 0; y < NH; ++y)
    for (x = 0; x < NW; ++x) native_px(x, y, g_native + ((size_t)y * NW + (size_t)x) * 3u);
  for (y = 0; y < NH; ++y)
    for (x = 0; x < w; ++x) {
      uint8_t *p = g_surface + ((size_t)y * (size_t)w + (size_t)x) * 3u;
      if (x >= side && x < side + NW) memcpy(p, g_native + ((size_t)y * NW + (size_t)(x - side)) * 3u, 3);
      else ext_px(x < side ? side - 1 - x : x - side - NW, y, p);
    }
}
/* what the surface shows at presentation (x, y), without any overlay */
static void base_px(int side, int x, int y, uint8_t *o) {
  if (x >= side && x < side + NW) native_px(x - side, y, o); else ext_px(x < side ? side - 1 - x : x - side - NW, y, o);
}
static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) { size_t i; for (i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull; return h; }
static uint64_t surface_hash(SDL_Surface *s) {
  uint64_t h = 1469598103934665603ull;
  int x, y;
  for (y = 0; y < s->h; ++y)
    for (x = 0; x < s->w; ++x) { Uint8 r = 0, g = 0, b = 0, a = 0; uint8_t px[3]; (void)SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a); px[0] = r; px[1] = g; px[2] = b; h = fnv(px, 3, h); }
  return h;
}
/* the 2x output pixel (2x, 2y) holds presentation pixel (x, y); true when it equals `want` (and so do the other three replicas) */
static int is_colour(SDL_Surface *s, int x, int y, const uint8_t want[3]) {
  int k;
  for (k = 0; k < 4; ++k) { Uint8 r = 0, g = 0, b = 0, a = 0; (void)SDL_ReadSurfacePixel(s, 2 * x + (k & 1), 2 * y + (k >> 1), &r, &g, &b, &a); if (r != want[0] || g != want[1] || b != want[2]) return 0; }
  return 1;
}
static int is_base(SDL_Surface *s, int side, int x, int y) { uint8_t b[3]; base_px(side, x, y, b); return is_colour(s, x, y, b); }

static int present(SvHost *h, int side, uint64_t *seq, SvWidescreenMode mode, const SvOverlayLayer *layers, int n, int with_hud) {
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  ++*seq;
  e.rgb24 = g_surface; e.size = (size_t)(NW + 2 * side) * NH * 3u; e.width = NW + 2 * side; e.height = NH; e.pitch = (NW + 2 * side) * 3; e.side = side; e.sequence = *seq;
  e.mode = mode; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_AUTHORED;
  if (with_hud) { e.overlay_rgba = g_hud; e.overlay_w = 32; e.overlay_h = 16; e.overlay_fx = 4; e.overlay_fy = 4; }
  e.layers = layers; e.layer_count = n;
  return sv_host_present_extended(h, g_native, NW, NH, NW * 3, *seq, &e);
}
static void configure(SvHost *h, SvDisplayRatio ratio, SvWidescreenMode ws, int surface_w) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = ws; s.video.widescreen_side_style = SV_SIDE_RAW; s.video.widescreen_ratio = ratio; s.video.widescreen_width = 100;
  s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(h, &s);
  SDL_SetWindowSize(h->window, surface_w * 2, NH * 2);
}
static SDL_Surface *shoot(SvHost *h) { sv_host_toast(h, ""); return sv_host_capture_output(h); }

static void through_host(void) {
  SvHostConfig cfg;
  SvHost *h;
  char dir[512];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  uint64_t seq = 1;
  const uint8_t red[3] = {255, 0, 0}, green[3] = {0, 255, 0}, blue[3] = {0, 0, 255}, white[3] = {255, 255, 255};
  SDL_Surface *o;
  SvOverlayLayer set[4];
  int x, y;
  snprintf(dir, sizeof dir, "%s/sv_overlay_layers_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(g_settings, sizeof g_settings, "%s/o.cfg", dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "layers"; cfg.picture_w = NW; cfg.picture_h = NH; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  h = sv_host_open(&cfg);
  CHECK(h != NULL, "host opens");
  if (h == NULL) return;
  solid(g_img[0], 16, 8, 255, 0, 0, 255);      /* red 16 x 8 */
  solid(g_img[1], 16, 8, 0, 255, 0, 255);      /* green */
  solid(g_img[2], 16, 8, 0, 0, 255, 255);      /* blue */
  solid(g_img[3], 16, 8, 255, 255, 255, 255);  /* white */
  solid((uint8_t *)g_hud, 32, 16, 255, 255, 255, 255);   /* the fixed HUD: opaque white 32 x 16 */
  memset(set, 0, sizeof set);

  /* ---- 4:3 / Original: the presentation is the picture ---- */
  build(0);
  configure(h, (SvDisplayRatio){4, 3}, SV_WIDESCREEN_ORIGINAL, NW);
  set[0] = layer(g_img[0], 16, 8, SV_OVA_TOP_RIGHT, SV_OVREF_PRESENTATION, 0, 0, 0);
  set[1] = layer(g_img[1], 16, 8, SV_OVA_BOTTOM_LEFT, SV_OVREF_PICTURE, 0, 0, 0);
  CHECK(present(h, 0, &seq, SV_WIDESCREEN_ORIGINAL, set, 2, 0) == 0 && sv_host_extension_side(h) == 0, "4:3 frame presented");
  o = shoot(h);
  CHECK(o != NULL && o->w == NW * 2 && o->h == NH * 2, "4:3 output is the 2x picture");
  if (o != NULL) {
    int wrong = 0;
    for (y = 0; y < NH; ++y)
      for (x = 0; x < NW; ++x) {
        const int in0 = x >= 304 && x < 320 && y < 8, in1 = x < 16 && y >= NH - 8;
        if (in0 ? !is_colour(o, x, y, red) : (in1 ? !is_colour(o, x, y, green) : !is_base(o, 0, x, y))) ++wrong;
      }
    CHECK(wrong == 0, "4:3: the top-right layer is at x 304..319 and the bottom-left at y 216..223; every other pixel is the picture");
    SDL_DestroySurface(o);
  }

  /* ---- 32:9: presentation = 796 wide (side 238) ---- */
  build(238);
  configure(h, (SvDisplayRatio){32, 9}, SV_WIDESCREEN_ENHANCED_GAMEPLAY, NW + 2 * 238);
  set[0] = layer(g_img[0], 16, 8, SV_OVA_TOP_RIGHT, SV_OVREF_PRESENTATION, 0, 0, 0);      /* x 780..795 */
  set[1] = layer(g_img[1], 16, 8, SV_OVA_TOP_RIGHT, SV_OVREF_PICTURE, 0, 0, 20);          /* picture's right edge: x 542..557, y 20..27 */
  set[2] = layer(g_img[2], 16, 8, SV_OVA_BOTTOM_LEFT, SV_OVREF_PRESENTATION, 0, 0, 0);    /* x 0..15, y 216..223 */
  CHECK(present(h, 238, &seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, set, 3, 0) == 0 && sv_host_extension_side(h) == 238 && sv_host_visible_side(h) == 238, "32:9 frame presented with the full side visible");
  o = shoot(h);
  CHECK(o != NULL && o->w == 796 * 2 && o->h == NH * 2, "32:9 output is the 2x surface");
  if (o != NULL) {
    int wrong = 0;
    for (y = 0; y < NH; ++y)
      for (x = 0; x < 796; ++x) {
        const int a = x >= 780 && y < 8, b = x >= 542 && x < 558 && y >= 20 && y < 28, c = x < 16 && y >= NH - 8;
        if (a ? !is_colour(o, x, y, red) : b ? !is_colour(o, x, y, green) : c ? !is_colour(o, x, y, blue) : !is_base(o, 238, x, y)) ++wrong;
      }
    CHECK(wrong == 0, "32:9: top-right of the presentation is x 780..795, the picture-anchored layer sits at the picture's right edge (542..557), bottom-left is the surface corner");
    SDL_DestroySurface(o);
  }

  /* ---- order above the fixed HUD, z order, clip, and the HUD alone unchanged ---- */
  {
    uint64_t hud_only, hud_zero_layers;
    SvOverlayLayer clipped;
    /* the fixed HUD alone: the layers array being absent, empty, or a zero count must all present identically (existing providers are unaffected) */
    CHECK(present(h, 238, &seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, NULL, 0, 1) == 0, "HUD-only frame");
    o = shoot(h); hud_only = surface_hash(o);
    { /* the HUD sits at the left edge of the visible side: presentation x 0..31, y 0..15 */
      int wrong = 0;
      for (y = 0; y < 24; ++y) for (x = 0; x < 40; ++x) { const int in = x < 32 && y < 16; if (in ? !is_colour(o, x, y, white) : !is_base(o, 238, x, y)) ++wrong; }
      CHECK(wrong == 0, "the fixed HUD alone is drawn exactly where it always was (left edge of the visible side)");
    }
    SDL_DestroySurface(o);
    CHECK(present(h, 238, &seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, set, 0, 1) == 0, "HUD with a zero layer count");
    o = shoot(h); hud_zero_layers = surface_hash(o); SDL_DestroySurface(o);
    CHECK(hud_only == hud_zero_layers, "a zero layer count presents byte for byte like the fixed HUD alone");
    /* layers overlapping the HUD: they are drawn above it; the rest of the HUD is still there */
    set[0] = layer(g_img[0], 16, 8, SV_OVA_TOP_LEFT, SV_OVREF_PRESENTATION, 1, 8, 4);      /* x 8..23, y 4..11 over the HUD, z 1 */
    set[1] = layer(g_img[1], 16, 8, SV_OVA_TOP_LEFT, SV_OVREF_PRESENTATION, 0, 12, 6);     /* z 0: below the red one where they overlap */
    clipped = layer(g_img[2], 16, 8, SV_OVA_TOP_LEFT, SV_OVREF_PRESENTATION, 2, 24, 8);    /* x 24..39, y 8..15; clip keeps image columns 4..9 and rows 2..5 -> x 28..33, y 10..13 */
    clipped.has_clip = 1; clipped.clip_x = 4; clipped.clip_y = 2; clipped.clip_w = 6; clipped.clip_h = 4;
    set[2] = clipped;
    CHECK(present(h, 238, &seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, set, 3, 1) == 0, "HUD plus layers frame");
    o = shoot(h);
    if (o != NULL) {
      int wrong = 0;
      for (y = 0; y < 24; ++y)
        for (x = 0; x < 44; ++x) {
          const int red_in = x >= 8 && x < 24 && y >= 4 && y < 12, green_in = x >= 12 && x < 28 && y >= 6 && y < 14, blue_in = x >= 28 && x < 34 && y >= 10 && y < 14, hud_in = x < 32 && y < 16;
          const uint8_t *want = red_in ? red : (blue_in ? blue : (green_in ? green : (hud_in ? white : NULL)));
          if (want != NULL ? !is_colour(o, x, y, want) : !is_base(o, 238, x, y)) ++wrong;
        }
      CHECK(wrong == 0, "layers are drawn above the fixed HUD in z order (red z1 over green z0), and a clip keeps only its rectangle");
      SDL_DestroySurface(o);
    }
  }

  /* ---- rejection: an out-of-bounds and an over-budget layer are rejected with diagnostics; the valid ones are still drawn ---- */
  {
    uint32_t rejected0 = 0, rejected1 = 0;
    int last_index = -9, last_reason = -9;
    sv_host_overlay_diag(h, &rejected0, &last_index, &last_reason);
    set[0] = layer(g_img[0], 16, 8, SV_OVA_TOP_RIGHT, SV_OVREF_PRESENTATION, 0, 1, 0);       /* one pixel past the right edge: out of bounds */
    set[1] = layer(g_img[1], 16, 8, SV_OVA_TOP_LEFT, SV_OVREF_PRESENTATION, 0, 100, 100);   /* valid: x 100..115, y 100..107 */
    set[2] = layer(g_img[2], 16, 8, SV_OVA_CENTER, SV_OVREF_PICTURE, 0, 400, 0);            /* far outside the picture */
    CHECK(present(h, 238, &seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, set, 3, 0) == 0, "frame with invalid layers still presents");
    o = shoot(h);
    sv_host_overlay_diag(h, &rejected1, &last_index, &last_reason);
    CHECK(rejected1 == rejected0 + 2 && last_index == 2 && last_reason == SV_OVR_OUT_OF_BOUNDS, "two layers rejected as out_of_bounds; the diagnostic names the last one");
    CHECK(strcmp(sv_ovl_reject_name((SvOvReject)last_reason), "out_of_bounds") == 0, "the reason has a stable name");
    if (o != NULL) {
      int wrong = 0;
      for (y = 90; y < 120; ++y) for (x = 90; x < 130; ++x) { const int in = x >= 100 && x < 116 && y >= 100 && y < 108; if (in ? !is_colour(o, x, y, green) : !is_base(o, 238, x, y)) ++wrong; }
      for (y = 0; y < 12; ++y) for (x = 770; x < 796; ++x) if (!is_base(o, 238, x, y)) ++wrong;   /* nothing drawn for the rejected top-right layer */
      CHECK(wrong == 0, "the valid layer is drawn, the rejected ones leave no pixel");
      SDL_DestroySurface(o);
    }
    /* over budget: two 200 x 200 images (80000 pixels) cannot both be kept; the second is rejected, the first is drawn */
    {
      static uint8_t big[2][200 * 200 * 4];
      SvOverlayLayer b[2];
      solid(big[0], 200, 200, 255, 0, 0, 255);
      solid(big[1], 200, 200, 0, 255, 0, 255);
      b[0] = layer(big[0], 200, 200, SV_OVA_CENTER, SV_OVREF_PICTURE, 0, 0, 0);
      b[1] = layer(big[1], 200, 200, SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 1, 0, 0);
      sv_host_overlay_diag(h, &rejected0, NULL, NULL);
      CHECK(present(h, 238, &seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, b, 2, 0) == 0, "over-budget frame presents");
      o = shoot(h);
      sv_host_overlay_diag(h, &rejected1, &last_index, &last_reason);
      CHECK(rejected1 == rejected0 + 1 && last_index == 1 && last_reason == SV_OVR_BUDGET, "the second 200 x 200 layer is rejected as over budget");
      if (o != NULL) {
        const int cx = 238 + (NW - 200) / 2, cy = (NH - 200) / 2;
        CHECK(is_colour(o, cx + 100, cy + 100, red) && is_base(o, 238, 238 + 5, 5), "the first layer is drawn, the over-budget one is not");
        SDL_DestroySurface(o);
      }
    }
  }
  sv_host_close(h);
  remove(g_settings);
}

int main(void) {
  solid(g_img[0], 16, 16, 255, 0, 0, 255);
  solid(g_img[1], 16, 16, 0, 255, 0, 255);
  solid(g_img[2], 16, 16, 0, 0, 255, 255);
  solid(g_img[3], 16, 16, 255, 255, 255, 255);
  plan_anchors();
  plan_rejections();
  plan_order_and_blend();
  through_host();
  printf(failures == 0 ? "sv_overlay_layers_test: all checks passed\n" : "sv_overlay_layers_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
