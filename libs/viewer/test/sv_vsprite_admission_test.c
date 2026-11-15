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
  uint64_t x = 1469598103934665603ull;
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
  if (failures != 0) { printf("%d failure(s)\n", failures); return 1; }
  printf("sv_vsprite_admission_test: ok\n");
  return 0;
}
