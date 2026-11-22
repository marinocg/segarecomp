/* SEG-140-T008 (review of F28): a picture-region virtual-sprite contribution rides on the extension's SINGLE admission verdict (ADR 0304 decision 3b, ADR 0306): a surface the host refuses (bad pitch, bad
 * geometry, produced above the requested level, another frame's) presents the native picture alone and credits nothing, while Original / native-only frames, admitted 32:9 margins and the declared-range
 * fallback keep their behaviour. Every case presents a synthetic positive-pixel signal through the real host and compares the FINAL pixels with references presented through the same host, and the
 * coverage JSON (offered/accepted/presented/covered and the virtual_sprites counters) with hand-derived values. Dummy video driver, software renderer, no ROM. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define W 320
#define H 224
#define SIDE_MAX 238
static char g_settings[700];
static uint8_t g_native[W * H * 3], g_comp[W * H * 3], g_ext[(W + 2 * SIDE_MAX) * H * 3];

typedef struct Case {
  const char *name;
  int level, num, den;          /* requested widescreen level and ratio */
  int surf_level;               /* the level the surface was produced as */
  int stale, bad_pitch, bad_geom;
  const char *decl;
  int left, right;              /* drawn pixels in each margin (of 64 authored in total) */
  int expect_composed;          /* the composed picture must be the one presented */
} Case;

static SvHost *open_host(const char *decl) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "vsprite admission"; cfg.picture_w = W; cfg.picture_h = H; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  cfg.geometry_declaration = decl;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  return sv_host_open(&cfg);
}
static void apply(SvHost *h, const Case *c) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = (SvWidescreenMode)c->level; s.video.widescreen_side_style = SV_SIDE_RAW; s.video.widescreen_width = 100;
  s.video.widescreen_ratio.num = c->num; s.video.widescreen_ratio.den = c->den;
  s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(h, &s);
}
static int side_for(int num, int den) { SvGeomPlan p; SvDisplayRatio r; r.num = num; r.den = den; return sv_geom_plan(W, H, SV_ASPECT_ORIGINAL, r, &p) == SV_GEOM_OK ? p.side : 0; }

static uint64_t out_hash(SvHost *h) {
  SDL_Surface *s = sv_host_capture_output(h);
  uint64_t x = 14695981039346656037ull;
  int y;
  if (s == NULL) return 0;   /* a missing capture is 0: the callers refuse it, it can never equal a native reference */
  for (y = s->h / 2; y < s->h; ++y) { const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch; int i; for (i = 0; i < s->w * 2; ++i) x = (x ^ row[i]) * 1099511628211ull; }   /* the lower-left quadrant: the host's once-per-change notice sits at the top right and is not what is compared */
  SDL_DestroySurface(s);
  return x;
}
static char *json_of(const SvHost *h) { static char buf[16384]; (void)sv_host_coverage_json(h, buf, sizeof buf); return buf; }
static long jnum(const char *json, const char *from, const char *key) {
  char pat[64];
  const char *p = from != NULL ? strstr(json, from) : json;
  if (p == NULL) return -1;
  snprintf(pat, sizeof pat, "\"%s\":", key);
  p = strstr(p, pat);
  return p == NULL ? -1 : strtol(p + strlen(pat), NULL, 10);
}
static long actors(const char *json, const char *key) { const char *a = strstr(json, "\"actors\":{"); return a == NULL ? -1 : jnum(a, NULL, key); }
static long vsn(const char *json, const char *key) { const char *v = strstr(json, "\"virtual_sprites\":{"); return v == NULL ? -1 : jnum(v, NULL, key); }

/* presents the frame twice in a fresh host: once with a positive-pixel actor signal (returns its hash and JSON), once without (the native reference) */
static uint64_t run(const Case *c, int with_signal, char *json_out, size_t cap) {
  SvHost *h = open_host(c->decl);
  SvExtendedSurface e;
  const int side = side_for(c->num, c->den);
  const uint64_t seq = 7;
  uint64_t hash;
  apply(h, c);
  sv_host_set_scene(h, "a");
  memset(&e, 0, sizeof e);
  e.rgb24 = g_ext; e.size = sizeof g_ext; e.width = W + 2 * side + (c->bad_geom ? 2 : 0); e.height = H; e.pitch = (W + 2 * side) * 3 - (c->bad_pitch ? 1 : 0); e.side = side;
  e.sequence = c->stale ? seq - 1u : seq; e.mode = (SvWidescreenMode)c->surf_level; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_GENERIC;
  if (with_signal) {
    e.actor_stage = SV_ACT_COMPOSED; e.actor_provenance = 2; e.actor_pixels_authored = 64; e.actor_left_authored = (uint32_t)c->left; e.actor_right_authored = (uint32_t)c->right; e.actor_picture = g_comp;
  }
  CHECK(sv_host_present_extended(h, g_native, W, H, W * 3, seq, &e) == 0, "the host presents the frame (a refused extension still presents the native picture)");
  hash = out_hash(h);
  CHECK(hash != 0u, "the final output was captured");
  if (json_out != NULL) snprintf(json_out, cap, "%s", json_of(h));
  sv_host_close(h);
  return hash;
}

/* W04: the side treatment decides what survives. Under Comfort a GENERIC surface keeps the provider's pixels only on its authored rows, and the compositor reports margin pixels per frame, not per row; the host therefore never credits
 * margin pixels under Comfort (it under-claims rather than assert a pixel survived), while Raw credits them and the picture region is credited either way. */
static void partial_rows_counters(void) {
  static const SvSideStyle styles[3] = {SV_SIDE_RAW, SV_SIDE_COMFORT, SV_SIDE_BORDERS};
  size_t k;
  for (k = 0; k < 3; ++k) {
    SvHost *h = open_host("max=32:9;scene=a:actors+hud");
    SvSettings st;
    SvExtendedSurface e;
    uint8_t rows[(H + 7) / 8];
    const int side = side_for(32, 9);
    char what[160];
    size_t y;
    memset(rows, 0, sizeof rows);
    for (y = 0; y < 192; ++y) rows[y >> 3] |= (uint8_t)(1u << (y & 7));
    st = *sv_host_settings(h);
    st.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY; st.video.widescreen_side_style = styles[k]; st.video.widescreen_width = 100;
    st.video.widescreen_ratio.num = 32; st.video.widescreen_ratio.den = 9; st.video.aspect = SV_ASPECT_ORIGINAL; st.video.scale = SV_SCALE_INTEGER;
    sv_host_apply_settings(h, &st);
    sv_host_set_scene(h, "a");
    memset(&e, 0, sizeof e);
    e.rgb24 = g_ext; e.size = sizeof g_ext; e.width = W + 2 * side; e.height = H; e.pitch = (W + 2 * side) * 3; e.side = side; e.sequence = 9; e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL;
    e.source = SV_EXTSRC_GENERIC; e.authored_rows = rows; e.authored_row_count = 192;
    e.actor_stage = SV_ACT_COMPOSED; e.actor_provenance = 1; e.actor_state_verified = 1; e.actor_pixels_verified = 64; e.actor_left_verified = 16; e.actor_right_verified = 16; e.actor_picture = g_comp;
    CHECK(sv_host_present_extended(h, g_native, W, H, W * 3, 9, &e) == 0, "partial rows: presented");
    snprintf(what, sizeof what, "style %d: margin pixels are credited only where the whole margin is shown (Raw), the picture part always", (int)k);
    CHECK(vsn(json_of(h), "pixels_left") == (k == 0 ? 16 : 0) && vsn(json_of(h), "pixels_right") == (k == 0 ? 16 : 0) && vsn(json_of(h), "pixels_picture") == 32, what);
    sv_host_close(h);
  }
}


/* ADR 0318 (review of #247): the presenter blacks out the side columns of a window-plane HUD band, and the compositor reports side pixels per frame, not per row, so with a band the host credits NO side pixel
 * (a conservative under-claim: side actors above the HUD are shown but not counted), keeps crediting the picture's pixels, and never credits what the band removed. Every style, with and without a band, centred and
 * asymmetric (the picture placed at 100 of 476 columns). */
static int toast_has(const SvHost *h, const char *needle) { int i; for (i = 0; i < h->toast_count; ++i) if (strstr(h->toasts[i].text, needle) != NULL) return 1; return 0; }

static SvHost *band_host(SvSideStyle style) {
  SvHost *h = open_host("max=32:9;scene=a:actors+hud");
  SvSettings st = *sv_host_settings(h);
  st.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY; st.video.widescreen_side_style = style; st.video.widescreen_width = 100;
  st.video.widescreen_ratio.num = 32; st.video.widescreen_ratio.den = 9; st.video.aspect = SV_ASPECT_ORIGINAL; st.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(h, &st);
  sv_host_set_scene(h, "a");
  return h;
}
static int band_present(SvHost *h, uint64_t seq, int shift, int hud_rows, uint32_t picture, uint32_t left, uint32_t right) {
  SvExtendedSurface e;
  const int side = side_for(32, 9);
  memset(&e, 0, sizeof e);
  e.rgb24 = g_ext; e.size = sizeof g_ext; e.width = W + 2 * side; e.height = H; e.pitch = (W + 2 * side) * 3; e.side = side; e.shift = shift; e.sequence = seq;
  e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_GENERIC; e.hud_rows = hud_rows;
  e.actor_stage = SV_ACT_COMPOSED; e.actor_provenance = 2; e.actor_pixels_authored = picture + left + right; e.actor_left_authored = left; e.actor_right_authored = right; e.actor_picture = g_comp;
  return sv_host_present_extended(h, g_native, W, H, W * 3, seq, &e);
}

static void hud_band_credit(void) {
  static const SvSideStyle styles[3] = {SV_SIDE_RAW, SV_SIDE_COMFORT, SV_SIDE_BORDERS};
  static const char *names[3] = {"Raw", "Comfort", "Borders"};
  const int side = side_for(32, 9);
  size_t k;
  int placed;
  for (k = 0; k < 3; ++k)
    for (placed = 0; placed < 2; ++placed) {
      const int shift = placed ? 100 - side : 0;   /* the picture at 100 of 476 columns, or centred */
      char what[200];
      SvHost *h;
      /* no band: the previous accounting, exactly (Raw credits the margins, Comfort and Borders under-claim them) */
      h = band_host(styles[k]);
      CHECK(band_present(h, 20 + (uint64_t)k, shift, 0, 32, 16, 16) == 0, "no band: presented");
      snprintf(what, sizeof what, "%s, %s, no band: picture 32 always, margins 16/16 only under Raw", names[k], placed ? "asymmetric" : "centred");
      CHECK(vsn(json_of(h), "pixels_picture") == 32 && vsn(json_of(h), "pixels_left") == (k == 0 ? 16 : 0) && vsn(json_of(h), "pixels_right") == (k == 0 ? 16 : 0), what);
      sv_host_close(h);
      /* a band: the picture's pixels are credited, no side pixel is (shown above the band or removed inside it: not distinguishable) */
      h = band_host(styles[k]);
      CHECK(band_present(h, 30 + (uint64_t)k, shift, 32, 32, 16, 16) == 0, "band: presented");
      snprintf(what, sizeof what, "%s, %s, HUD band: picture 32 credited, side pixels never credited", names[k], placed ? "asymmetric" : "centred");
      CHECK(vsn(json_of(h), "pixels_picture") == 32 && vsn(json_of(h), "pixels_left") == 0 && vsn(json_of(h), "pixels_right") == 0, what);
      sv_host_close(h);
      /* an actor drawn only in the side columns (inside the masked margin or above it): nothing is credited, nothing is presented as composed */
      h = band_host(styles[k]);
      CHECK(band_present(h, 40 + (uint64_t)k, shift, 32, 0, 16, 16) == 0, "side-only actor with a band: presented");
      snprintf(what, sizeof what, "%s, %s, side-only actor with a band: no pixel, no presentation credit, diagnosed as unpresented", names[k], placed ? "asymmetric" : "centred");
      CHECK(vsn(json_of(h), "pixels_left") == 0 && vsn(json_of(h), "pixels_right") == 0 && vsn(json_of(h), "presented") == 0 && actors(json_of(h), "presented") == 0 &&
                strstr(json_of(h), "\"last_reason\":\"virtual_sprite_unpresented\"") != NULL, what);
      sv_host_close(h);
    }
  /* a band as tall as the picture behaves the same */
  {
    SvHost *h = band_host(SV_SIDE_RAW);
    CHECK(band_present(h, 50, 100 - side, H, 32, 16, 16) == 0, "full-height band: presented");
    CHECK(vsn(json_of(h), "pixels_picture") == 32 && vsn(json_of(h), "pixels_left") == 0 && vsn(json_of(h), "pixels_right") == 0, "full-height band: side pixels never credited");
    sv_host_close(h);
  }
}

/* the "virtual sprites not presented" diagnostic never raises a notice, but stays fully diagnosed; every other presentation failure still does, also right after it */
static void unpresented_is_silent(void) {
  SvHost *h = band_host(SV_SIDE_RAW);
  SvExtendedSurface e;
  int i;
  const int side = side_for(32, 9);
  for (i = 0; i < 60; ++i) {
    CHECK(band_present(h, 100 + (uint64_t)i, 0, 32, 0, 16, 16) == 0, "an unpresented set is presented");
    if (i == 0) sv_host_toast(h, "");   /* the host's one-time "Pause menu" hint on the very first picture is not what is under test */
  }
  CHECK(h->toast_count == 0 && !toast_has(h, "virtual sprites"), "virtual sprites not presented: no notice, however often it repeats");
  CHECK(strstr(json_of(h), "\"last_reason\":\"virtual_sprite_unpresented\"") != NULL && strstr(json_of(h), "\"virtual_sprite_unpresented\":60") != NULL, "the diagnostic is intact: last_reason and 60 reason frames");
  /* a genuine failure right after it is reported on its own transition */
  memset(&e, 0, sizeof e);
  e.rgb24 = g_ext; e.size = sizeof g_ext; e.width = W + 2 * side; e.height = H; e.pitch = (W + 2 * side) * 3 - 1; e.side = side; e.sequence = 200; e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL;
  CHECK(sv_host_present_extended(h, g_native, W, H, W * 3, 200, &e) == 0 && toast_has(h, "extension surface invalid"), "a transition from the suppressed reason to a genuine one is reported");
  /* and the suppressed reason after a genuine one stays silent while the genuine one is not repeated */
  sv_host_toast(h, "");
  CHECK(band_present(h, 201, 0, 32, 0, 16, 16) == 0 && h->toast_count == 0, "back to the unpresented set: still no notice");
  /* a refused (virtual-sprite REJECTED) set is a genuine compositor rejection and is still announced */
  memset(&e, 0, sizeof e);
  e.rgb24 = g_ext; e.size = sizeof g_ext; e.width = W + 2 * side; e.height = H; e.pitch = (W + 2 * side) * 3; e.side = side; e.sequence = 202; e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL;
  e.actor_stage = SV_ACT_REJECTED; e.actor_reject = 1;
  CHECK(sv_host_present_extended(h, g_native, W, H, W * 3, 202, &e) == 0 && toast_has(h, "virtual sprites rejected"), "a compositor rejection is still announced");
  sv_host_close(h);
}

int main(void) {
  static const Case cases[] = {
    /* name                         level num den surf stale pitch geom decl                          L  R  composed */
    {"1 valid surface",              1, 16, 9, 1, 0, 0, 0, "max=32:9;scene=a:actors+hud",           0, 0, 1},
    {"2 invalid pitch",              1, 16, 9, 1, 0, 1, 0, "max=32:9;scene=a:actors+hud",           0, 0, 0},
    {"3 invalid geometry",           1, 16, 9, 1, 0, 0, 1, "max=32:9;scene=a:actors+hud",           0, 0, 0},
    {"4 produced above requested",   1, 16, 9, 2, 0, 0, 0, "max=32:9;scene=a:actors+hud",           0, 0, 0},
    {"5 stale sequence",             1, 16, 9, 1, 1, 0, 0, "max=32:9;scene=a:actors+hud",           0, 0, 0},
    {"6 Original native-only",       0, 16, 9, 1, 0, 0, 0, "max=32:9;scene=a:actors+hud",           0, 0, 1},
    {"7 32:9 admitted margins",      1, 32, 9, 1, 0, 0, 0, "max=32:9;scene=a:actors+hud",          16, 16, 1},
    {"8 outside declared range",     1, 32, 9, 1, 0, 0, 0, "max=16:9;scene=a:actors+hud",           0, 0, 0},
  };
  size_t i;
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  snprintf(g_settings, sizeof g_settings, "%s/sv_vsprite_admission_settings.cfg", SDL_GetBasePath() != NULL ? SDL_GetBasePath() : ".");
  memset(g_native, 70, sizeof g_native);
  memset(g_comp, 200, sizeof g_comp);
  memset(g_ext, 30, sizeof g_ext);
  for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    const Case *c = &cases[i];
    char with[16384], without[16384];
    const uint64_t signal_hash = run(c, 1, with, sizeof with), native_hash = run(c, 0, without, sizeof without);
    char what[160];
    CHECK(signal_hash != 0u && native_hash != 0u, "both captures exist before any comparison");
    snprintf(what, sizeof what, "%s: the final pixels are the %s picture", c->name, c->expect_composed ? "COMPOSED" : "NATIVE");
    CHECK(c->expect_composed ? signal_hash != native_hash : signal_hash == native_hash, what);
    if (c->expect_composed) {
      /* credited: offered, accepted, presented and covered for the declared actors component; the counters carry the region split (total 64 = picture + margins) */
      snprintf(what, sizeof what, "%s: actors offered/accepted/presented/covered all 1", c->name);
      CHECK(actors(with, "offered") == 1 && actors(with, "accepted") == 1 && actors(with, "presented") == 1 && actors(with, "covered") == 1, what);
      snprintf(what, sizeof what, "%s: virtual_sprites presented 1 with the hand-derived regions", c->name);
      CHECK(vsn(with, "sets") == 1 && vsn(with, "composed") == 1 && vsn(with, "presented") == 1 && vsn(with, "pixels_authored") == 64 && vsn(with, "pixels_left") == c->left &&
                vsn(with, "pixels_right") == c->right && vsn(with, "pixels_picture") == 64 - c->left - c->right,
            what);
    } else if (c->stale) {
      snprintf(what, sizeof what, "%s: a stale surface is neither shown nor counted", c->name);
      CHECK(vsn(with, "sets") == -1 && actors(with, "offered") == 0 && strstr(with, "\"unverified_state\":1") != NULL, what);
    } else if (c->decl != NULL && c->den == 9 && c->num == 32 && strstr(c->decl, "max=16:9") != NULL) {
      snprintf(what, sizeof what, "%s: native-only fallback, nothing presented or covered", c->name);
      CHECK(vsn(with, "presented") == 0 && vsn(with, "pixels_authored") == 0 && actors(with, "presented") == 0 && actors(with, "covered") == 0 && jnum(with, NULL, "outside_range_frames") == 1, what);
    } else {
      snprintf(what, sizeof what, "%s: the refused surface credits no actor presentation and no pixel", c->name);
      CHECK(actors(with, "presented") == 0 && actors(with, "covered") == 0 && vsn(with, "presented") == 0 && vsn(with, "pixels_authored") == 0 && vsn(with, "pixels_picture") == 0, what);
    }
  }
  partial_rows_counters();
  hud_band_credit();
  unpresented_is_silent();
  if (failures != 0) { printf("%d failure(s)\n", failures); return 1; }
  printf("sv_vsprite_admission_test: ok\n");
  return 0;
}
