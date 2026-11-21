/* ADR 0318: a window-plane HUD band at the bottom of the picture stays centred in the planned total and is bordered by the backdrop, whatever the picture's placement above it. Real frames are presented
 * through the shared host in an integer 2x window and the composed output is read back. Expected pixels come from independently built buffers, never from the renderer. Strict C11. */
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
#define BAND 32   /* the bottom four cell rows */

static uint8_t g_native[NW * NH * 3];
static uint8_t g_surface[(NW + 2 * 700) * NH * 3];

/* native pixels never have red >= 130; extension pixels always do, and depend on the column, so a misplaced or surviving column is visible */
static void native_px(int x, int y, uint8_t *o) { o[0] = (uint8_t)((x * 3 + y) & 127); o[1] = (uint8_t)((y * 5 + (x >> 3) * 17) & 127); o[2] = (uint8_t)(((x ^ y) * 7) & 127); }
static void ext_px(int x, int y, uint8_t *o) { o[0] = (uint8_t)(130 + (x & 63)); o[1] = (uint8_t)(60 + (y & 31)); o[2] = (uint8_t)(20 + ((x >> 6) & 31)); }

static int read_is(SDL_Surface *s, int x, int y, const uint8_t want[3]) {
  int k;
  for (k = 0; k < 4; ++k) { Uint8 r = 0, g = 0, b = 0, a = 0; (void)SDL_ReadSurfacePixel(s, 2 * x + (k & 1), 2 * y + (k >> 1), &r, &g, &b, &a); if (r != want[0] || g != want[1] || b != want[2]) return 0; }
  return 1;
}

static void pure_surface_check(void) {
  static uint8_t buf[(NW + 2 * 39) * NH * 3];
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  e.rgb24 = buf; e.size = sizeof buf; e.width = NW + 78; e.height = NH; e.pitch = (NW + 78) * 3; e.side = 39; e.sequence = 5; e.mode = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; e.status = SV_WS_PARTIAL;
  CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_OK, "a surface without a band is valid (zero-initialised hud_rows)");
  e.hud_rows = BAND; CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_OK, "a band inside the picture is valid");
  e.hud_rows = NH; CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_OK, "a band as tall as the picture is valid");
  e.hud_rows = NH + 1; CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_GEOMETRY, "a band taller than the picture is rejected");
  e.hud_rows = -1; CHECK(sv_extended_surface_check(&e, NW, NH, 5, 320) == SV_EXT_GEOMETRY, "a negative band is rejected");
}

/* what the surface holds at column x when the picture starts at `left` and the band rows are `rows` (the producer's output: the band keeps the picture at `left`, the margins are the extension's) */
static void build(int total, int left) {
  int x, y;
  for (y = 0; y < NH; ++y) for (x = 0; x < NW; ++x) native_px(x, y, g_native + ((size_t)y * NW + (size_t)x) * 3u);
  for (y = 0; y < NH; ++y)
    for (x = 0; x < NW + total; ++x) {
      uint8_t *o = g_surface + ((size_t)y * (size_t)(NW + total) + (size_t)x) * 3u;
      if (x >= left && x < left + NW) native_px(x - left, y, o); else ext_px(x, y, o);
    }
}

static int present(SvHost *h, int side, int shift, int hud_rows, uint64_t *seq) {
  SvExtendedSurface e;
  const int total = 2 * side;
  memset(&e, 0, sizeof e);
  ++*seq;
  e.rgb24 = g_surface; e.size = sizeof g_surface; e.width = NW + total; e.height = NH; e.pitch = (NW + total) * 3; e.side = side; e.shift = shift; e.sequence = *seq;
  e.mode = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_GENERIC; e.hud_rows = hud_rows;
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

/* Presents a surface of `side` per side with the picture at `left` and a `BAND`-row HUD band, in `style`, and checks the whole composed output: above the band exactly what hud_rows == 0 shows (the picture at `left`),
 * in the band the picture row at column `side` with black on every other column. */
static int check_case(SvHost *h, SvDisplayRatio ratio, SvSideStyle style, int side, int left, uint64_t *seq, const char *label) {
  const int total = 2 * side, wide = NW + total;
  int x, y, wrong_above = 0, wrong_hud = 0, wrong_margin = 0, vl = -1, vr = -1;
  const uint8_t black[3] = {0, 0, 0};
  SDL_Surface *o;
  char what[200];
  build(total, left);
  configure(h, ratio, style, wide);
  if (present(h, side, left - side, BAND, seq) != 0 || sv_host_extension_side(h) != side) { CHECK(0, "the surface with a band is accepted"); return -1; }
  sv_host_visible_columns(h, &vl, &vr);
  snprintf(what, sizeof what, "%s: the whole total is visible (%d + %d)", label, left, total - left);
  CHECK(vl == left && vr == total - left, what);
  o = shoot(h);
  if (o == NULL) { CHECK(0, "capture"); return -1; }
  CHECK(o->w == wide * 2 && o->h == NH * 2, "the output is the 2x surface");
  for (y = 0; y < NH - BAND; ++y)   /* above the band: the native picture at `left` (the side columns are the style's, checked by the existing suites) */
    for (x = 0; x < NW; ++x) { uint8_t want[3]; native_px(x, y, want); if (!read_is(o, left + x, y, want)) ++wrong_above; }
  for (y = NH - BAND; y < NH; ++y) {
    for (x = 0; x < NW; ++x) { uint8_t want[3]; native_px(x, y, want); if (!read_is(o, side + x, y, want)) ++wrong_hud; }
    for (x = 0; x < side; ++x) { if (!read_is(o, x, y, black)) ++wrong_margin; if (!read_is(o, side + NW + x, y, black)) ++wrong_margin; }
  }
  snprintf(what, sizeof what, "%s: above the band the picture is where the placement says", label);
  CHECK(wrong_above == 0, what);
  snprintf(what, sizeof what, "%s: the band's picture row is at column %d, centred in the total, whatever the placement", label, side);
  CHECK(wrong_hud == 0, what);
  snprintf(what, sizeof what, "%s: both band margins (%d columns each) are the backdrop, no extension or tile pixel", label, side);
  CHECK(wrong_margin == 0, what);
  SDL_DestroySurface(o);
  return 0;
}

static void through_host(void) {
  static const int lefts[4] = {0, 100, 238, 476};   /* 32:9: all-right, intermediate, centred, all-left */
  SvHostConfig cfg;
  SvHost *h;
  char dir[512];
  char settings[700];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  uint64_t seq = 1;
  const SvDisplayRatio r329 = {32, 9}, r219 = {21, 9}, r169 = {16, 9};
  static const SvSideStyle styles[3] = {SV_SIDE_RAW, SV_SIDE_COMFORT, SV_SIDE_BORDERS};
  static const char *style_name[3] = {"Raw", "Comfort", "Borders"};
  int k, st, x, y;
  snprintf(dir, sizeof dir, "%s/sv_hud_band_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(settings, sizeof settings, "%s/o.cfg", dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings, 1);
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "hud"; cfg.picture_w = NW; cfg.picture_h = NH; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  h = sv_host_open(&cfg);
  CHECK(h != NULL, "host opens");
  if (h == NULL) return;

  /* ---- 32:9, every placement of the picture (camera scrolling), every side style: the band does not move ---- */
  for (st = 0; st < 3; ++st)
    for (k = 0; k < 4; ++k) {
      char label[120];
      snprintf(label, sizeof label, "32:9 %s, picture at %d", style_name[st], lefts[k]);
      if (check_case(h, r329, styles[st], 238, lefts[k], &seq, label) != 0) { sv_host_close(h); return; }
    }

  /* ---- other planned totals: 21:9 (side 101) and 16:9 (side 39) ---- */
  for (st = 0; st < 3; ++st) {
    static const int l219[3] = {0, 55, 202}, l169[3] = {0, 20, 78};
    for (k = 0; k < 3; ++k) {
      char label[120];
      snprintf(label, sizeof label, "21:9 %s, picture at %d", style_name[st], l219[k]);
      if (check_case(h, r219, styles[st], 101, l219[k], &seq, label) != 0) { sv_host_close(h); return; }
      snprintf(label, sizeof label, "16:9 %s, picture at %d", style_name[st], l169[k]);
      if (check_case(h, r169, styles[st], 39, l169[k], &seq, label) != 0) { sv_host_close(h); return; }
    }
  }

  /* ---- no band: the previous presentation exactly (Raw is the surface), so a game without a window HUD is unchanged ---- */
  {
    SDL_Surface *o;
    int wrong = 0;
    build(2 * 238, 100);
    configure(h, r329, SV_SIDE_RAW, NW + 2 * 238);
    CHECK(present(h, 238, 100 - 238, 0, &seq) == 0, "a surface without a band is accepted");
    o = shoot(h);
    if (o != NULL) {
      for (y = 0; y < NH; ++y)
        for (x = 0; x < NW + 2 * 238; ++x) {
          uint8_t want[3];
          if (x >= 100 && x < 100 + NW) native_px(x - 100, y, want); else ext_px(x, y, want);
          if (!read_is(o, x, y, want)) ++wrong;
        }
      CHECK(wrong == 0, "without a band every pixel is the surface pixel (nothing moved, nothing masked)");
      SDL_DestroySurface(o);
    }
  }

  /* ---- a band taller than the picture is refused whole: the native picture only ---- */
  build(2 * 238, 100);
  configure(h, r329, SV_SIDE_RAW, NW + 2 * 238);
  CHECK(present(h, 238, 100 - 238, NH + 1, &seq) == 0 && sv_host_extension_side(h) == 0, "an out-of-range band is shown as the native picture only");

  /* ---- the native viewport: the extension not shown (4:3 plan / Original) leaves the picture, band rows included, exactly as it is ---- */
  {
    SDL_Surface *o;
    int wrong = 0;
    const SvDisplayRatio r43 = {4, 3};
    build(2 * 238, 100);
    configure(h, r43, SV_SIDE_RAW, NW);
    CHECK(present(h, 238, 100 - 238, BAND, &seq) == 0, "a surface for a plan that shows no extension is accepted");
    o = shoot(h);
    if (o != NULL) {
      for (y = 0; y < NH; ++y) for (x = 0; x < NW; ++x) { uint8_t want[3]; native_px(x, y, want); if (!read_is(o, x, y, want)) ++wrong; }
      CHECK(wrong == 0, "with no extension shown the native picture is untouched, HUD rows included");
      SDL_DestroySurface(o);
    }
  }

  /* ---- an aspect change: an asymmetric surface shown under a narrower plan is the native picture alone, band rows untouched ---- */
  {
    SDL_Surface *o;
    int wrong = 0, vl = -1, vr = -1;
    build(2 * 238, 0);
    configure(h, r329, SV_SIDE_RAW, NW + 2 * 238);
    CHECK(present(h, 238, -238, BAND, &seq) == 0, "32:9 all-right frame with a band");
    configure(h, r169, SV_SIDE_RAW, 600);
    sv_host_restyle(h);
    sv_host_visible_columns(h, &vl, &vr);
    CHECK(vl == 0 && vr == 0, "a narrower plan shows no extension columns");
    o = shoot(h);
    if (o != NULL) {
      const int ox = (o->w / 2 - NW) / 2;   /* the picture is centred in the window */
      for (y = 0; y < NH; ++y) for (x = 0; x < NW; ++x) { uint8_t want[3]; native_px(x, y, want); if (!read_is(o, ox + x, y, want)) ++wrong; }
      CHECK(wrong == 0, "under a narrower plan the whole native picture, band rows included, is shown as produced");
      SDL_DestroySurface(o);
    }
  }
  sv_host_close(h);
}

int main(void) {
  pure_surface_check();
  through_host();
  printf(failures == 0 ? "sv_hud_band_test: all checks passed\n" : "sv_hud_band_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
