/* ADR 0315: the asymmetric widescreen viewport. The planned total of extra source pixels (2 * side) is unchanged; a surface may place the native picture anywhere inside it (`shift`). Part 1 is the pure
 * layer (side fill with independent widths, the overlay planner with independent visible columns, the surface validation). Part 2 presents real frames through the shared host in an integer 2x window and
 * reads the composed output back: the picture is where the shift says in the texture and on screen for every split of a 32:9 view (0/476, 100/376, 238/238, 476/0), for Raw, Comfort and Borders, with the
 * fixed HUD and positioned layers anchored to the picture or to the whole presentation, a zero-width region, an invalid shift, an aspect change while playing and the legacy centred behaviour unchanged.
 * Expected positions come from layout arithmetic and from independently built surfaces, never from the renderer. Strict C11. */
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
#define SIDE 238   /* 32:9 */
#define TOTAL (2 * SIDE)

/* ---------------------------------------------------------------------------------------------------------------- part 1: the pure layer */
static void pure_side_fill(void) {
  static uint8_t native[NW * NH * 3];
  static uint8_t l1[TOTAL * NH * 3], r1[TOTAL * NH * 3], l2[TOTAL * NH * 3], r2[TOTAL * NH * 3];
  static int32_t scratch[(NH * 12 + 2 * (672 + 1)) + 64];
  int x, y, widths[3] = {39, 238, 5}, i;
  for (y = 0; y < NH; ++y) for (x = 0; x < NW; ++x) { uint8_t *p = native + ((size_t)y * NW + (size_t)x) * 3u; p[0] = (uint8_t)(x * 3 + y); p[1] = (uint8_t)(y * 5); p[2] = (uint8_t)(x ^ y); }
  CHECK(sv_side_scratch_bytes(NH) <= sizeof scratch, "scratch for the test");
  for (i = 0; i < 3; ++i) {
    const int s = widths[i];
    int t;
    for (t = 0; t < 2; ++t) {
      const SvSideTreatment tr = t == 0 ? SV_TREAT_COMFORT : SV_TREAT_BORDERS;
      memset(l1, 1, sizeof l1); memset(r1, 2, sizeof r1); memset(l2, 3, sizeof l2); memset(r2, 4, sizeof r2);
      CHECK(sv_side_fill(tr, native, NW, NH, NW * 3, s, l1, r1, scratch) == 0 && sv_side_fill2(tr, native, NW, NH, NW * 3, s, s, l2, r2, scratch) == 0 &&
            memcmp(l1, l2, (size_t)s * NH * 3u) == 0 && memcmp(r1, r2, (size_t)s * NH * 3u) == 0, "equal widths: sv_side_fill2 is the historical fill bit for bit");
    }
  }
  {   /* each side's fade spans its own width: a side of width W is the same pixels whatever the other side is */
    int lw = 100, rw = 376;
    CHECK(sv_side_fill2(SV_TREAT_COMFORT, native, NW, NH, NW * 3, lw, rw, l1, r1, scratch) == 0, "asymmetric fill");
    CHECK(sv_side_fill(SV_TREAT_COMFORT, native, NW, NH, NW * 3, lw, l2, r2, scratch) == 0 && memcmp(l1, l2, (size_t)lw * NH * 3u) == 0, "the left strip equals the symmetric fill of its own width");
    CHECK(sv_side_fill(SV_TREAT_COMFORT, native, NW, NH, NW * 3, rw, l2, r2, scratch) == 0 && memcmp(r1, r2, (size_t)rw * NH * 3u) == 0, "the right strip equals the symmetric fill of its own width");
    CHECK(sv_side_fill2(SV_TREAT_COMFORT, native, NW, NH, NW * 3, 0, 476, NULL, r1, scratch) == 0 && sv_side_fill2(SV_TREAT_BORDERS, native, NW, NH, NW * 3, 476, 0, l1, NULL, scratch) == 0, "a zero-width side needs no buffer");
    CHECK(sv_side_fill2(SV_TREAT_COMFORT, native, NW, NH, NW * 3, 0, 0, l1, r1, scratch) != 0 && sv_side_fill2(SV_TREAT_COMFORT, native, NW, NH, NW * 3, -1, 5, l1, r1, scratch) != 0 &&
          sv_side_fill2(SV_TREAT_COMFORT, native, NW, NH, NW * 3, 5, 673, l1, r1, scratch) != 0 && sv_side_fill2(SV_TREAT_PASSTHROUGH, native, NW, NH, NW * 3, 5, 5, l1, r1, scratch) != 0 &&
          sv_side_fill2(SV_TREAT_COMFORT, native, NW, NH, NW * 3, 5, 5, NULL, r1, scratch) != 0, "no width, a negative or oversized width, passthrough and a missing buffer are refused");
  }
}

static uint8_t g_img[2][16 * 8 * 4];
static SvOverlayLayer layer(const uint8_t *img, int anchor, int ref, int dx, int dy) {
  SvOverlayLayer l;
  memset(&l, 0, sizeof l);
  l.rgba = img; l.w = 16; l.h = 8; l.anchor = anchor; l.reference = ref; l.dx = dx; l.dy = dy;
  return l;
}
static void pure_overlay_plan(void) {
  static const int splits[4][2] = {{0, 476}, {100, 376}, {238, 238}, {476, 0}};
  int i, k;
  for (i = 0; i < 16 * 8 * 4; ++i) g_img[0][i] = 255;
  for (k = 0; k < 4; ++k) {
    const int vl = splits[k][0], vr = splits[k][1], total = vl + NW + vr;
    SvOverlayPlan p;
    SvOverlayLayer a = layer(g_img[0], SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 0), b = layer(g_img[0], SV_OVA_TOP_RIGHT, SV_OVREF_PICTURE, 0, 0), c = layer(g_img[0], SV_OVA_TOP_LEFT, SV_OVREF_PRESENTATION, 0, 0),
                   d = layer(g_img[0], SV_OVA_TOP_RIGHT, SV_OVREF_PRESENTATION, 0, 0);
    sv_overlay_plan2(&a, 1, NW, NH, vl, vr, &p);
    CHECK(p.placed == 1 && p.place[0].x == vl, "picture-referenced top-left sits at the picture's left edge, wherever the picture is");
    sv_overlay_plan2(&b, 1, NW, NH, vl, vr, &p);
    CHECK(p.placed == 1 && p.place[0].x == vl + NW - 16, "picture-referenced top-right sits at the picture's right edge");
    sv_overlay_plan2(&c, 1, NW, NH, vl, vr, &p);
    CHECK(p.placed == 1 && p.place[0].x == 0, "presentation-referenced top-left is the surface's left edge");
    sv_overlay_plan2(&d, 1, NW, NH, vl, vr, &p);
    CHECK(p.placed == 1 && p.place[0].x == total - 16, "presentation-referenced top-right is the surface's right edge (the visible total)");
    if (vl == vr) {
      SvOverlayPlan q;
      sv_overlay_plan(&b, 1, NW, NH, vl, &q);
      sv_overlay_plan2(&b, 1, NW, NH, vl, vr, &p);
      CHECK(memcmp(&p, &q, sizeof p) == 0, "equal visible columns are exactly sv_overlay_plan");
    }
  }
  {   /* a layer wider than the picture-referenced region is rejected; the presentation region of a zero-width view is the picture */
    SvOverlayLayer wide = layer(g_img[0], SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 0);
    SvOverlayPlan p;
    wide.w = 128; wide.h = 8;
    sv_overlay_plan2(&wide, 1, 64, NH, 0, 0, &p);
    CHECK(p.placed == 0 && p.rejected == 1, "out of bounds is still rejected with no visible side");
    sv_overlay_plan2(&wide, 1, NW, NH, -1, 0, &p);
    CHECK(p.placed == 0, "a negative visible width plans nothing");
  }
}

static void pure_surface_check(void) {
  static uint8_t buf[(NW + 2 * 39) * NH * 3];
  SvExtendedSurface e;
  int shift;
  memset(&e, 0, sizeof e);
  e.rgb24 = buf; e.size = sizeof buf; e.width = NW + 78; e.height = NH; e.pitch = (NW + 78) * 3; e.side = 39; e.sequence = 5; e.mode = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; e.status = SV_WS_PARTIAL;
  for (shift = -39; shift <= 39; ++shift) { e.shift = shift; if (sv_extended_surface_check(&e, NW, NH, 5, 320) != SV_EXT_OK) { CHECK(0, "every shift inside the planned total is valid"); break; } }
  e.shift = -40; CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_GEOMETRY, "a shift past the planned total on the left is rejected");
  e.shift = 40; CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_GEOMETRY, "a shift past the planned total on the right is rejected");
  e.shift = 1 << 30; CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_GEOMETRY, "a huge shift is rejected");
  e.shift = -(1 << 30); CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_GEOMETRY, "a hugely negative shift is rejected");
}

/* ---------------------------------------------------------------------------------------------------------------- part 2: through the host */
static char g_settings[700];
static uint8_t g_native[NW * NH * 3];
static uint8_t g_surface[(NW + TOTAL) * NH * 3];
static uint8_t g_hud[32 * 16 * 4];

static void native_px(int x, int y, uint8_t *o) { o[0] = (uint8_t)((x * 3 + y) & 127); o[1] = (uint8_t)((y * 5 + (x >> 3) * 17) & 127); o[2] = (uint8_t)(((x ^ y) * 7) & 127); }
/* extension pixels are recognisable (red >= 130, never produced by native_px) and depend on the surface column, so a misplaced column is visible */
static void ext_px(int x, int y, uint8_t *o) { o[0] = (uint8_t)(130 + (x & 63)); o[1] = (uint8_t)(60 + (y & 31)); o[2] = (uint8_t)(20 + ((x >> 6) & 31)); }
/* what the surface holds at surface column x when the picture starts at column `left` */
static void surface_px(int left, int x, int y, uint8_t *o) { if (x >= left && x < left + NW) native_px(x - left, y, o); else ext_px(x, y, o); }
static void build(int left) {
  int x, y;
  for (y = 0; y < NH; ++y) for (x = 0; x < NW; ++x) native_px(x, y, g_native + ((size_t)y * NW + (size_t)x) * 3u);
  for (y = 0; y < NH; ++y) for (x = 0; x < NW + TOTAL; ++x) surface_px(left, x, y, g_surface + ((size_t)y * (size_t)(NW + TOTAL) + (size_t)x) * 3u);
}
static int is_colour(SDL_Surface *s, int x, int y, const uint8_t want[3]) {
  int k;
  for (k = 0; k < 4; ++k) { Uint8 r = 0, g = 0, b = 0, a = 0; (void)SDL_ReadSurfacePixel(s, 2 * x + (k & 1), 2 * y + (k >> 1), &r, &g, &b, &a); if (r != want[0] || g != want[1] || b != want[2]) return 0; }
  return 1;
}
static int present(SvHost *h, int shift, uint64_t *seq, SvExtSource src, const SvOverlayLayer *layers, int n, int with_hud) {
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  ++*seq;
  e.rgb24 = g_surface; e.size = sizeof g_surface; e.width = NW + TOTAL; e.height = NH; e.pitch = (NW + TOTAL) * 3; e.side = SIDE; e.shift = shift; e.sequence = *seq;
  e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = src;
  if (with_hud) { e.overlay_rgba = g_hud; e.overlay_w = 32; e.overlay_h = 16; e.overlay_fx = 4; e.overlay_fy = 4; }
  e.layers = layers; e.layer_count = n;
  return sv_host_present_extended(h, g_native, NW, NH, NW * 3, *seq, &e);
}
static void configure(SvHost *h, SvDisplayRatio ratio, SvSideStyle style, int win_w) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY; s.video.widescreen_side_style = style; s.video.widescreen_ratio = ratio; s.video.widescreen_width = 100;
  s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(h, &s);
  SDL_SetWindowSize(h->window, win_w * 2, NH * 2);
}
static SDL_Surface *shoot(SvHost *h) { sv_host_toast(h, ""); return sv_host_capture_output(h); }
/* the whole presentation equals `surface_px(left, ...)` shifted by `origin` (the first visible surface column), except where `skip` says otherwise */
static int count_wrong(SDL_Surface *o, int left, int origin, int width) {
  int x, y, wrong = 0;
  uint8_t want[3];
  for (y = 0; y < NH; ++y) for (x = 0; x < width; ++x) { surface_px(left, origin + x, y, want); if (!is_colour(o, x, y, want)) ++wrong; }
  return wrong;
}
static int count_marker(SDL_Surface *o, int x0, int x1) {
  int x, y, n = 0;
  for (y = 0; y < o->h; y += 2) for (x = x0 * 2; x < x1 * 2 && x < o->w; x += 2) { Uint8 r = 0, g = 0, b = 0, a = 0; (void)SDL_ReadSurfacePixel(o, x, y, &r, &g, &b, &a); if (r >= 130 && r < 194 && b >= 20 && b < 52 && g >= 60 && g < 92) ++n; }
  return n;
}

static void through_host(void) {
  static const int lefts[4] = {0, 100, 238, 476};   /* columns left of the picture of a 32:9 view: all-right, intermediate, centred, all-left */
  SvHostConfig cfg;
  SvHost *h;
  char dir[512];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  uint64_t seq = 1;
  const uint8_t red[3] = {255, 0, 0}, green[3] = {0, 255, 0}, white[3] = {255, 255, 255};
  SDL_Surface *o;
  const SvDisplayRatio r329 = {32, 9};
  int k, i, x, y;
  snprintf(dir, sizeof dir, "%s/sv_asym_viewport_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(g_settings, sizeof g_settings, "%s/o.cfg", dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "asym"; cfg.picture_w = NW; cfg.picture_h = NH; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  h = sv_host_open(&cfg);
  CHECK(h != NULL, "host opens");
  if (h == NULL) return;
  for (i = 0; i < 16 * 8; ++i) { memcpy(&g_img[0][i * 4], red, 3); g_img[0][i * 4 + 3] = 255; memcpy(&g_img[1][i * 4], green, 3); g_img[1][i * 4 + 3] = 255; }
  for (i = 0; i < 32 * 16; ++i) { memcpy(&g_hud[i * 4], white, 3); g_hud[i * 4 + 3] = 255; }

  /* ---- Raw: the presentation is the surface, exactly, for every split ---- */
  for (k = 0; k < 4; ++k) {
    const int left = lefts[k], shift = left - SIDE;
    int vl = -1, vr = -1;
    char what[160];
    build(left);
    configure(h, r329, SV_SIDE_RAW, NW + TOTAL);
    CHECK(present(h, shift, &seq, SV_EXTSRC_AUTHORED, NULL, 0, 0) == 0 && sv_host_extension_side(h) == SIDE, "an asymmetric frame is accepted");
    sv_host_visible_columns(h, &vl, &vr);
    snprintf(what, sizeof what, "split %d/%d: the whole total is visible, %d columns left and %d right", left, TOTAL - left, left, TOTAL - left);
    CHECK(vl == left && vr == TOTAL - left, what);
    o = shoot(h);
    CHECK(o != NULL && o->w == (NW + TOTAL) * 2 && o->h == NH * 2, "the output is the 2x surface");
    if (o == NULL) { sv_host_close(h); return; }
    snprintf(what, sizeof what, "split %d/%d, Raw: every presentation pixel is the surface pixel (native picture at x = %d)", left, TOTAL - left, left);
    CHECK(count_wrong(o, left, 0, NW + TOTAL) == 0, what);
    SDL_DestroySurface(o);
  }

  /* ---- Comfort and Borders on a generic surface: the treatment fills each side at ITS OWN width, the picture is untouched ---- */
  for (k = 0; k < 4; ++k) {
    const int left = lefts[k], shift = left - SIDE;
    int style;
    for (style = 0; style < 2; ++style) {
      char what[160];
      int wrong_pic = 0, markers;
      build(left);
      configure(h, r329, style == 0 ? SV_SIDE_COMFORT : SV_SIDE_BORDERS, NW + TOTAL);
      CHECK(present(h, shift, &seq, SV_EXTSRC_GENERIC, NULL, 0, 0) == 0 && sv_host_side_treatment(h) == (style == 0 ? SV_TREAT_COMFORT : SV_TREAT_BORDERS), "treated frame accepted");
      o = shoot(h);
      if (o == NULL) { sv_host_close(h); return; }
      for (y = 0; y < NH; ++y) for (x = 0; x < NW; ++x) { uint8_t want[3]; native_px(x, y, want); if (!is_colour(o, left + x, y, want)) ++wrong_pic; }
      snprintf(what, sizeof what, "split %d/%d, %s: the native picture is untouched at its own position", left, TOTAL - left, style == 0 ? "Comfort" : "Borders");
      CHECK(wrong_pic == 0, what);
      markers = count_marker(o, 0, NW + TOTAL) - count_marker(o, left, left + NW);
      snprintf(what, sizeof what, "split %d/%d, %s: no raw extension pixel survives on either side", left, TOTAL - left, style == 0 ? "Comfort" : "Borders");
      CHECK(markers == 0, what);
      if (style == 1) {   /* Borders: a flat neutral panel on both sides, nothing on a zero-width side */
        const uint8_t panel[3] = {14, 14, 14};
        int bad = 0;
        for (y = 0; y < NH; y += 7) { for (x = 0; x < left; x += 5) bad += !is_colour(o, x, y, panel); for (x = left + NW; x < NW + TOTAL; x += 5) bad += !is_colour(o, x, y, panel); }
        snprintf(what, sizeof what, "split %d/%d, Borders: both sides are the flat panel", left, TOTAL - left);
        CHECK(bad == 0, what);
      } else {   /* Comfort: dark at the outer edge of each side that exists */
        Uint8 r = 0, g = 0, b = 0, a = 0;
        int dark_ok = 1;
        if (left > 0) { (void)SDL_ReadSurfacePixel(o, 0, 2 * (NH / 2), &r, &g, &b, &a); dark_ok = dark_ok && r <= 12 && g <= 12 && b <= 14; }
        if (TOTAL - left > 0) { (void)SDL_ReadSurfacePixel(o, 2 * (NW + TOTAL) - 1, 2 * (NH / 2), &r, &g, &b, &a); dark_ok = dark_ok && r <= 12 && g <= 12 && b <= 14; }
        snprintf(what, sizeof what, "split %d/%d, Comfort: each side fades to the dark neutral at its own outer edge", left, TOTAL - left);
        CHECK(dark_ok, what);
      }
      SDL_DestroySurface(o);
    }
  }

  /* ---- the fixed HUD and positioned layers: picture-referenced and presentation-referenced, at every split, zero-width regions included ---- */
  for (k = 0; k < 4; ++k) {
    const int left = lefts[k], shift = left - SIDE;
    SvOverlayLayer set[3];
    int wrong = 0, hud_wrong = 0;
    build(left);
    configure(h, r329, SV_SIDE_RAW, NW + TOTAL);
    set[0] = layer(g_img[0], SV_OVA_TOP_RIGHT, SV_OVREF_PRESENTATION, 0, 40);   /* x = total - 16 */
    set[1] = layer(g_img[1], SV_OVA_TOP_LEFT, SV_OVREF_PICTURE, 0, 80);         /* x = left */
    set[2] = layer(g_img[0], SV_OVA_TOP_LEFT, SV_OVREF_PRESENTATION, 0, 120);   /* x = 0 */
    CHECK(present(h, shift, &seq, SV_EXTSRC_AUTHORED, set, 3, 1) == 0, "frame with HUD and layers");
    o = shoot(h);
    if (o == NULL) { sv_host_close(h); return; }
    for (y = 0; y < NH; ++y)
      for (x = 0; x < NW + TOTAL; ++x) {
        uint8_t want[3];
        const int hud = x >= 0 && x < 32 && y >= 0 && y < 16;   /* the HUD's left edge is the left edge of the visible surface, wherever the picture is */
        const int a = x >= NW + TOTAL - 16 && y >= 40 && y < 48, b = x >= left && x < left + 16 && y >= 80 && y < 88, c = x < 16 && y >= 120 && y < 128;
        surface_px(left, x, y, want);
        if (hud) { if (!is_colour(o, x, y, white)) ++hud_wrong; }
        else if (a || c) { if (!is_colour(o, x, y, red)) ++wrong; }
        else if (b) { if (!is_colour(o, x, y, green)) ++wrong; }
        else if (!is_colour(o, x, y, want)) ++wrong;
      }
    CHECK(hud_wrong == 0, "the fixed HUD sits at the left edge of the visible surface");
    {
      char what[160];
      snprintf(what, sizeof what, "split %d/%d: presentation-referenced layers hug the surface corners, the picture-referenced one the picture's left edge (x = %d)", left, TOTAL - left, left);
      CHECK(wrong == 0, what);
    }
    SDL_DestroySurface(o);
  }

  /* ---- an invalid shift is refused whole: no extension, no reinterpretation ---- */
  {
    static const int bad[] = {-SIDE - 1, SIDE + 1, 1 << 28, -(1 << 28)};
    for (i = 0; i < 4; ++i) {
      build(SIDE);
      configure(h, r329, SV_SIDE_RAW, NW + TOTAL);
      CHECK(present(h, bad[i], &seq, SV_EXTSRC_AUTHORED, NULL, 0, 0) == 0 && sv_host_extension_side(h) == 0, "an out-of-range shift is shown as the native picture only");
    }
  }

  /* ---- aspect change while playing: an asymmetric surface is shown whole or not at all, never cropped into a centred one ---- */
  {
    const SvDisplayRatio r169 = {16, 9}, r219 = {21, 9};
    int vl = -1, vr = -1, markers;
    build(0);
    configure(h, r329, SV_SIDE_RAW, NW + TOTAL);
    CHECK(present(h, -SIDE, &seq, SV_EXTSRC_AUTHORED, NULL, 0, 0) == 0, "32:9 all-right frame");
    configure(h, r169, SV_SIDE_RAW, 600);   /* the user narrows the view while this frame is on screen */
    sv_host_restyle(h);
    sv_host_visible_columns(h, &vl, &vr);
    CHECK(vl == 0 && vr == 0, "a 16:9 view of a 32:9 asymmetric surface shows no extension columns (it is never cropped to a centred one)");
    o = shoot(h);
    if (o == NULL) { sv_host_close(h); return; }
    markers = count_marker(o, 0, o->w / 2);
    CHECK(markers == 0, "no extension pixel of the asymmetric surface is presented under a narrower aspect");
    SDL_DestroySurface(o);
    configure(h, r219, SV_SIDE_RAW, 600);   /* 21:9 plans a smaller total (side 101) than the 32:9 surface was made for (side 238): never a crop of it */
    sv_host_restyle(h);
    sv_host_visible_columns(h, &vl, &vr);
    CHECK(vl == 0 && vr == 0, "a narrower plan than the one the surface was produced for hides an asymmetric surface");
    configure(h, r329, SV_SIDE_RAW, NW + TOTAL);   /* back to the plan it was produced for: shown whole again, no new frame needed */
    sv_host_restyle(h);
    sv_host_visible_columns(h, &vl, &vr);
    CHECK(vl == 0 && vr == TOTAL, "the plan it was produced for shows the surface whole again");
  }

  /* ---- a surface produced for a NARROWER plan (21:9, side 101, picture 50 columns in) presented under a WIDER one (32:9): a wider request never shows more than was produced, so it is shown whole at its own total ---- */
  {
    const int side = 101, total = 2 * side, left = 50, shift = left - side;
    SvExtendedSurface e;
    int vl = -1, vr = -1;
    for (y = 0; y < NH; ++y) for (x = 0; x < NW; ++x) native_px(x, y, g_native + ((size_t)y * NW + (size_t)x) * 3u);
    for (y = 0; y < NH; ++y) for (x = 0; x < NW + total; ++x) surface_px(left, x, y, g_surface + ((size_t)y * (size_t)(NW + total) + (size_t)x) * 3u);
    memset(&e, 0, sizeof e);
    ++seq;
    e.rgb24 = g_surface; e.size = (size_t)(NW + total) * NH * 3u; e.width = NW + total; e.height = NH; e.pitch = (NW + total) * 3; e.side = side; e.shift = shift; e.sequence = seq;
    e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_AUTHORED;
    configure(h, r329, SV_SIDE_RAW, NW + total);
    CHECK(sv_host_present_extended(h, g_native, NW, NH, NW * 3, seq, &e) == 0 && sv_host_extension_side(h) == side, "a 21:9 asymmetric surface is accepted under a 32:9 request");
    sv_host_visible_columns(h, &vl, &vr);
    CHECK(vl == left && vr == total - left, "a wider plan than produced shows the whole produced total, 50 left and 152 right");
    o = shoot(h);
    CHECK(o != NULL && o->w == (NW + total) * 2 && count_wrong(o, left, 0, NW + total) == 0, "and presents exactly the surface it was given");
    if (o != NULL) SDL_DestroySurface(o);
  }

  /* ---- the legacy centred surface is untouched: shift 0 behaves exactly as before ---- */
  {
    int vl = -1, vr = -1;
    build(SIDE);
    configure(h, r329, SV_SIDE_RAW, NW + TOTAL);
    CHECK(present(h, 0, &seq, SV_EXTSRC_AUTHORED, NULL, 0, 0) == 0 && sv_host_visible_side(h) == SIDE, "centred frame");
    sv_host_visible_columns(h, &vl, &vr);
    CHECK(vl == SIDE && vr == SIDE, "a centred surface shows the same columns on both sides");
    o = shoot(h);
    CHECK(o != NULL && count_wrong(o, SIDE, 0, NW + TOTAL) == 0, "centred Raw presentation is the surface");
    if (o != NULL) SDL_DestroySurface(o);
  }
  sv_host_close(h);
}

int main(void) {
  pure_side_fill();
  pure_overlay_plan();
  pure_surface_check();
  through_host();
  printf(failures == 0 ? "sv_asym_viewport_test: all checks passed\n" : "sv_asym_viewport_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
