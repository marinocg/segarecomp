/* SEG-140-T005 (ADR 0304): deterministic scene rejection and per-component coverage against an independently constructed ground truth.
 * A scripted sequence of synthetic frames (aspects 4:3 .. 32:9, three scenes, rejection variants, virtual-sprite stages) is presented through the real host and every counter, every reason and
 * the rendered output are compared with a reference model written here from the written rules, not from the host's code. Nothing in this test proves a game object exists or that a
 * virtual sprite reached the live viewer: the actor signal is a synthetic fixture of the contribution stage. Dummy video driver, software renderer, no ROM. Strict C11. */
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

#define W 320
#define H 224
#define SIDE_MAX 238
static const char *DECL = "max=21:9;scene=a:actors+hud+scenery;scene=b:hud";
static char g_settings[700];
static uint8_t g_native[W * H * 3], g_ext[(W + 2 * SIDE_MAX) * H * 3], g_ovl[8 * 8 * 4];

/* one scripted frame */
typedef struct Frame {
  const char *scene;      /* "a", "b" or "zz" (unknown) */
  int num, den;           /* requested aspect */
  int level;              /* the requested widescreen level (1 or 2) */
  int surf_level;         /* the level the surface was produced as */
  int stale;              /* the surface is for another frame */
  int bad_pitch, bad_geom;
  int scenery, hud;       /* the provider supplies authored scenery / a HUD overlay */
  int stage, prov, state_ok, reject;   /* the virtual-sprite contribution */
  int hud_only;           /* the surface carries no extension at all (the normal HUD-only surface) */
} Frame;

typedef struct Tally { uint64_t cov, miss, und, out, req, off, acc, pres, rej; } Tally;
typedef struct Truth { uint64_t frames, outside, reason[SV_REJ_COUNT]; Tally t[3][3]; /* a, b, * x comp */ } Truth;

static SvHost *open_host(const char *decl) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "scene rejection"; cfg.picture_w = W; cfg.picture_h = H; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  cfg.geometry_declaration = decl;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  return sv_host_open(&cfg);
}
static void set(SvHost *h, int num, int den, int level, SvSideStyle style) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = (SvWidescreenMode)level; s.video.widescreen_side_style = style; s.video.widescreen_width = 100;
  s.video.widescreen_ratio.num = num; s.video.widescreen_ratio.den = den;
  s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(h, &s);
}
static int side_for(int num, int den) { SvGeomPlan p; SvDisplayRatio r; r.num = num; r.den = den; return sv_geom_plan(W, H, SV_ASPECT_ORIGINAL, r, &p) == SV_GEOM_OK ? p.side : 0; }

static int present(SvHost *h, const Frame *f, uint64_t seq) {
  SvExtendedSurface e;
  const int side = side_for(f->num, f->den);
  memset(&e, 0, sizeof e);
  e.rgb24 = f->hud_only ? NULL : g_ext; e.size = f->hud_only ? 0u : sizeof g_ext; e.width = W + 2 * side + (f->bad_geom ? 2 : 0); e.height = H; e.pitch = (W + 2 * side) * 3 - (f->bad_pitch ? 1 : 0); e.side = f->hud_only ? 0 : side; e.sequence = f->stale ? seq - 1u : seq;
  e.mode = (SvWidescreenMode)f->surf_level; e.status = SV_WS_PARTIAL; e.source = f->scenery ? SV_EXTSRC_AUTHORED : SV_EXTSRC_GENERIC;
  if (f->hud) { e.overlay_rgba = g_ovl; e.overlay_w = 8; e.overlay_h = 8; }
  e.actor_stage = (uint8_t)f->stage; e.actor_provenance = (uint8_t)f->prov; e.actor_state_verified = (uint8_t)f->state_ok; e.actor_reject = (uint8_t)f->reject;
  sv_host_set_scene(h, strcmp(f->scene, "zz") == 0 ? "zz" : f->scene);
  return sv_host_present_extended(h, g_native, W, H, W * 3, seq, &e);
}

/* ------------------------------------------------------------------ the reference model (from the written rules) */
static int model_in_range(int num, int den) { return num * 3 >= 4 * den && num * 9 <= 21 * den; }   /* 4:3 <= n/d <= 21:9 */
static int model_scene(const char *s) { return strcmp(s, "a") == 0 ? 0 : strcmp(s, "b") == 0 ? 1 : 2; }
static int model_declared(int scene, int comp) { /* comps: 0 scenery, 1 hud, 2 actors */ return scene == 0 ? 1 : (scene == 1 && comp == 1); }
static void model_apply(Truth *g, const Frame *f) {
  const int sc = model_scene(f->scene), in = model_in_range(f->num, f->den), side = side_for(f->num, f->den);
  int off[3], acc[3], c;
  unsigned why = 0;
  int claim, surface_ok;
  if (f->stale) { g->reason[SV_REJ_UNVERIFIED_STATE]++; return; }   /* refused before accounting: reasons only */
  if (f->hud_only) { off[0] = 0; } else off[0] = f->scenery && side > 0; off[1] = f->hud; off[2] = f->stage != 0;
  claim = !f->hud_only && side > 0 && f->surf_level != 0;
  /* the host presents a surface only when it is a real extension of this frame at no more than the requested level and structurally valid: nothing riding on a refused surface is delivered */
  surface_ok = claim && f->surf_level <= f->level && !f->bad_pitch && !f->bad_geom;
  acc[0] = off[0] && surface_ok;   /* a surface without side columns claims no extension: never refused as one */
  acc[1] = f->hud;
  acc[2] = f->stage == SV_ACT_COMPOSED && (f->prov == 2 || (f->prov == 1 && f->state_ok)) && surface_ok;
  if (claim && f->surf_level > f->level) why |= 1u << SV_REJ_UNSUPPORTED_MODE; else if (claim && (f->bad_pitch || f->bad_geom)) why |= 1u << SV_REJ_INVALID_SURFACE;
  if (f->stage == SV_ACT_SUBMITTED || f->stage == SV_ACT_VALIDATED) why |= 1u << SV_REJ_VSPRITE_UNPRESENTED;
  if (f->stage == SV_ACT_REJECTED) why |= 1u << SV_REJ_VSPRITE_REJECTED;
  if (f->stage == SV_ACT_COMPOSED && (f->prov == 2 || (f->prov == 1 && f->state_ok)) && !surface_ok) why |= 1u << SV_REJ_VSPRITE_UNPRESENTED;
  if (f->stage > SV_ACT_REJECTED || (f->stage == SV_ACT_COMPOSED && f->prov != 1 && f->prov != 2)) why |= 1u << SV_REJ_UNSUPPORTED_MODE;
  if (f->stage == SV_ACT_COMPOSED && f->prov == 1 && !f->state_ok) why |= 1u << SV_REJ_UNVERIFIED_STATE;
  if (!in) why |= 1u << SV_REJ_OVER_DECLARED;
  g->frames++;
  if (!in) g->outside++;
  for (c = 0; c < 3; ++c) {
    Tally *t = &g->t[sc][c];
    const int decl = model_declared(sc, c);
    if (!in) t->out++;
    else if (decl) { if (acc[c]) t->cov++; else t->miss++; }
    else if (acc[c]) { t->und++; why |= 1u << SV_REJ_UNDECLARED; }
    if (in && decl) t->req++;
    if (off[c]) t->off++;
    if (acc[c]) t->acc++;
    if (acc[c] && in) t->pres++;
    if (off[c] && !acc[c]) t->rej++;
  }
  for (c = 1; c < SV_REJ_COUNT; ++c) if (why & (1u << c)) g->reason[c]++;
}

/* ------------------------------------------------------------------ reading the host's JSON */
static long jnum(const char *json, const char *scene, const char *comp, const char *key) {
  char pat[96];
  const char *p;
  snprintf(pat, sizeof pat, "{\"id\":\"%s\",", scene);
  p = strstr(json, pat);
  if (p == NULL) return -1;
  snprintf(pat, sizeof pat, "\"%s\":{", comp);
  p = strstr(p, pat);
  if (p == NULL) return -1;
  snprintf(pat, sizeof pat, "\"%s\":", key);
  p = strstr(p, pat);
  return p == NULL ? -1 : strtol(p + strlen(pat), NULL, 10);
}
static long jtop(const char *json, const char *key) { char pat[64]; const char *p; snprintf(pat, sizeof pat, "\"%s\":", key); p = strstr(json, pat); return p == NULL ? -1 : strtol(p + strlen(pat), NULL, 10); }
static char *json_of(const SvHost *h) { static char buf[16384]; (void)sv_host_coverage_json(h, buf, sizeof buf); return buf; }
static int toast_has(const SvHost *h, const char *needle) { int i; for (i = 0; i < h->toast_count; ++i) if (strstr(h->toasts[i].text, needle) != NULL) return 1; return 0; }
static uint64_t out_hash(SvHost *h) {
  SDL_Surface *s = sv_host_capture_output(h);
  uint64_t x = 1469598103934665603ull;
  int y;
  if (s == NULL) return 0;
  for (y = 0; y < s->h; ++y) { const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch; int i; for (i = 0; i < s->w * 4; ++i) x = (x ^ row[i]) * 1099511628211ull; }
  SDL_DestroySurface(s);
  return x;
}

/* the script: every aspect x scene x variant (deterministic) */
enum { VARIANTS = 22 };
static Frame make(int ratio_i, int scene_i, int v) {
  static const int nums[4] = {4, 16, 21, 32}, dens[4] = {3, 9, 9, 9};
  static const char *scenes[3] = {"a", "b", "zz"};
  Frame f;
  memset(&f, 0, sizeof f);
  f.scene = scenes[scene_i]; f.num = nums[ratio_i]; f.den = dens[ratio_i]; f.level = 2; f.surf_level = 2;
  f.scenery = 1; f.hud = 1;
  switch (v) {
    case 0: break;                                              /* a valid scenery + HUD frame */
    case 1: f.hud = 0; break;                                   /* a declared HUD that is not delivered */
    case 2: f.scenery = 0; break;                               /* scenery not delivered */
    case 3: f.stale = 1; break;                                 /* a surface for another frame */
    case 4: f.surf_level = 2; f.level = 1; break;               /* produced above the requested level */
    case 5: f.bad_pitch = 1; break;                             /* inconsistent surface */
    case 6: f.stage = SV_ACT_SUBMITTED; f.prov = 2; break;      /* submitted only */
    case 7: f.stage = SV_ACT_VALIDATED; f.prov = 2; break;      /* validated, never composed */
    case 8: f.stage = SV_ACT_REJECTED; f.reject = 6; break;     /* rejected by the compositor's validation (a GenesisVirtualReason code) */
    case 9: f.stage = SV_ACT_COMPOSED; f.prov = 2; break;       /* authored sprites composed */
    case 10: f.stage = SV_ACT_COMPOSED; f.prov = 1; f.state_ok = 1; break;   /* game-state sprites with a verified snapshot */
    case 11: f.stage = SV_ACT_COMPOSED; f.prov = 1; f.state_ok = 0; break;   /* game-state sprites WITHOUT a verified snapshot */
    case 12: f.stage = SV_ACT_COMPOSED; f.prov = 9; break;      /* unknown provenance */
    case 13: f.stage = SV_ACT_COMPOSED; f.prov = 2; f.scenery = 0; f.hud = 0; break;   /* actors alone prove nothing about scenery or HUD */
    case 14: f.scenery = 0; f.hud = 1; break;                   /* a HUD alone proves nothing about scenery or actors */
    case 15: f.scenery = 1; f.hud = 0; break;                   /* scenery alone proves nothing about actors or HUD */
    case 16: f.stage = 77; f.prov = 2; break;                   /* an unknown stage is refused */
    case 17: f.hud_only = 1; f.scenery = 0; break;              /* a HUD-only surface without a pixel buffer: no extension, no refusal */
    case 19: f.level = 1; f.surf_level = 2; f.stage = SV_ACT_COMPOSED; f.prov = 2; break;   /* a composed set on a surface produced above the requested level */
    case 20: f.bad_pitch = 1; f.stage = SV_ACT_COMPOSED; f.prov = 2; break;                  /* ... on a surface with an invalid pitch */
    case 21: f.bad_geom = 1; f.stage = SV_ACT_COMPOSED; f.prov = 1; f.state_ok = 1; break;   /* ... on a surface whose geometry is wrong */
    default: f.surf_level = 0; f.scenery = 0; f.bad_pitch = 1; break;   /* what the live viewer hands over in Original: a pixel buffer, produced as Original, plus the HUD: never refused */
  }
  return f;
}

static void run_script(SvHost *h, Truth *g, uint64_t *seq) {
  int r, s, v;
  for (r = 0; r < 4; ++r)
    for (s = 0; s < 3; ++s)
      for (v = 0; v < VARIANTS; ++v) {
        const Frame f = make(r, s, v);
        set(h, f.num, f.den, f.level, SV_SIDE_RAW);
        ++*seq;
        (void)present(h, &f, *seq);
        model_apply(g, &f);
      }
}

static void compare_truth(const char *json, const Truth *g, const char *label) {
  static const char *scenes[3] = {"a", "b", "*"};
  static const char *comps[3] = {"scenery", "hud", "actors"};
  int s, c, r;
  char what[160];
  snprintf(what, sizeof what, "%s: frames", label); CHECK(jtop(json, "frames") == (long)g->frames, what);
  snprintf(what, sizeof what, "%s: outside_range_frames", label); CHECK(jtop(json, "outside_range_frames") == (long)g->outside, what);
  for (s = 0; s < 3; ++s)
    for (c = 0; c < 3; ++c) {
      const Tally *t = &g->t[s][c];
      const struct { const char *k; uint64_t v; } kv[9] = {{"covered", t->cov}, {"missing", t->miss}, {"undeclared", t->und}, {"outside_range", t->out}, {"requested", t->req},
                                                         {"offered", t->off}, {"accepted", t->acc}, {"presented", t->pres}, {"rejected", t->rej}};
      int k;
      for (k = 0; k < 9; ++k) {
        snprintf(what, sizeof what, "%s: %s/%s/%s", label, scenes[s], comps[c], kv[k].k);
        { const long got = jnum(json, scenes[s], comps[c], kv[k].k); if (got != (long)kv[k].v) printf("  got %ld want %llu\n", got, (unsigned long long)kv[k].v); CHECK(got == (long)kv[k].v, what); }
      }
    }
  for (r = 1; r < SV_REJ_COUNT; ++r) {
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", sv_reject_reason_name((SvRejectReason)r));
    p = strstr(json, "\"reason_frames\":{");
    p = p != NULL ? strstr(p, pat) : NULL;
    snprintf(what, sizeof what, "%s: reason_frames %s", label, sv_reject_reason_name((SvRejectReason)r));
    if (p != NULL && strtol(p + strlen(pat), NULL, 10) != (long)g->reason[r]) printf("  got %ld want %llu\n", strtol(p + strlen(pat), NULL, 10), (unsigned long long)g->reason[r]);
    CHECK(p != NULL && strtol(p + strlen(pat), NULL, 10) == (long)g->reason[r], what);
  }
}

int main(void) {
  char dir[512], first[16384];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  SvHost *h, *h2;
  Truth g, g2;
  uint64_t seq = 1000, seq2 = 1000, base;
  size_t i;
  snprintf(dir, sizeof dir, "%s/sv_scene_rejection_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  snprintf(g_settings, sizeof g_settings, "%s/r1.cfg", dir);
  for (i = 0; i < sizeof g_native; ++i) g_native[i] = (uint8_t)(i * 7u);
  for (i = 0; i < sizeof g_ext; ++i) g_ext[i] = (uint8_t)(i * 13u + 5u);
  for (i = 0; i < sizeof g_ovl; ++i) g_ovl[i] = 255;

  /* 1. the whole script against the reference model, twice (byte-identical reports) */
  memset(&g, 0, sizeof g); memset(&g2, 0, sizeof g2);
  h = open_host(DECL);
  CHECK(h != NULL, "host opens");
  if (h == NULL) return 1;
  run_script(h, &g, &seq);
  snprintf(first, sizeof first, "%s", json_of(h));
  compare_truth(first, &g, "script");
  CHECK(g.frames > 100 && g.outside > 0 && g.reason[SV_REJ_UNVERIFIED_STATE] > 0 && g.reason[SV_REJ_UNSUPPORTED_MODE] > 0 && g.reason[SV_REJ_OVER_DECLARED] > 0 && g.reason[SV_REJ_UNDECLARED] > 0 &&
        g.reason[SV_REJ_VSPRITE_REJECTED] > 0 && g.reason[SV_REJ_VSPRITE_UNPRESENTED] > 0 && g.reason[SV_REJ_INVALID_SURFACE] > 0, "the script exercises every rejection reason");
  CHECK(jtop(first, "last_vsprite_reason") == 0, "a later accepted frame clears the virtual-sprite reason");
  sv_host_close(h);
  remove(g_settings);
  h2 = open_host(DECL);
  CHECK(h2 != NULL, "second host opens");
  if (h2 == NULL) return 1;
  run_script(h2, &g2, &seq2);
  CHECK(strcmp(first, json_of(h2)) == 0, "repeated identical runs give a byte-identical report");
  sv_host_close(h2);
  remove(g_settings);

  /* 2. the rejections one by one: reason, fallback, no stale content, a return to a valid state */
  h = open_host(DECL);
  if (h == NULL) return 1;
  base = 5000;
  { /* a plain reference frame for the native-only output: the same host path without any provider */
    uint64_t native_only;
    Frame f = make(1, 0, 0);
    set(h, 16, 9, 2, SV_SIDE_RAW);
    (void)sv_host_present(h, g_native, W, H, W * 3);
    native_only = out_hash(h);
    CHECK(present(h, &f, ++base) == 0 && h->ovl_w == 8 && sv_host_extension_side(h) == 39 && out_hash(h) != native_only, "a valid 16:9 frame shows the authored extension and the HUD");
    CHECK(strstr(json_of(h), "\"last_reason\":\"none\"") != NULL, "no reason on a valid frame");

    /* unverified state: a stale surface is neither shown nor counted, and the HUD it carried does not survive */
    f = make(1, 0, 3);
    CHECK(present(h, &f, ++base) == 0 && h->ovl_w == 0 && sv_host_extension_side(h) == 0, "a stale surface leaves no extension and no HUD");
    CHECK(strstr(json_of(h), "\"last_reason\":\"unverified_state\"") != NULL && jtop(json_of(h), "frames") == 1, "stale: reason recorded, no frame accounted");
    CHECK(toast_has(h, "not verified"), "stale: a visible notice");
    /* over-declared: 32:9 against max 21:9, under Raw too; only the native picture, and no provider pixel survives */
    set(h, 32, 9, 2, SV_SIDE_RAW);
    f = make(3, 0, 0);
    CHECK(present(h, &f, ++base) == 0 && sv_host_geometry_fallback(h) && sv_host_extension_side(h) == 0 && h->ovl_w == 0, "32:9 under Raw: native picture only, no provider column or HUD");
    CHECK(strstr(json_of(h), "\"last_reason\":\"over_declared_geometry\"") != NULL && jnum(json_of(h), "a", "scenery", "outside_range") == 1 && jnum(json_of(h), "a", "scenery", "presented") == 1,
          "over-declared: outside_range counted, not presented, never covered");
    { /* the rejected frame is byte-identical to the same host's own native-only presentation of that frame */
      const uint64_t rejected = out_hash(h);
      (void)sv_host_present(h, g_native, W, H, W * 3);
      CHECK(rejected == out_hash(h), "rejected content is byte-identical to the native-only presentation");
    }
    /* back to a valid state at once: nothing of the rejection is retained */
    set(h, 16, 9, 2, SV_SIDE_RAW);
    f = make(1, 0, 0);
    CHECK(present(h, &f, ++base) == 0 && !sv_host_geometry_fallback(h) && h->ovl_w == 8 && sv_host_extension_side(h) == 39 && strstr(json_of(h), "\"last_reason\":\"none\"") != NULL, "a return to a valid state is presented again, reason cleared");
    /* unsupported mode: produced above the requested level; the extension is refused, the reason visible once */
    set(h, 16, 9, 1, SV_SIDE_RAW);
    f = make(1, 0, 4);
    sv_host_toast(h, "");
    CHECK(present(h, &f, ++base) == 0 && sv_host_extension_side(h) == 0 && strstr(json_of(h), "\"last_reason\":\"unsupported_mode\"") != NULL && toast_has(h, "not supported"), "a surface produced above the requested level is refused with a reason");
    sv_host_toast(h, "");
    CHECK(present(h, &f, ++base) == 0 && h->toast_count == 0, "the notice is raised on the transition, not on every frame");
    /* Original asks for no extension: a surface produced above it (the normal HUD carrier) is never refused and raises no notice */
    set(h, 4, 3, 0, SV_SIDE_RAW);
    f = make(1, 0, 0);
    sv_host_toast(h, "");
    CHECK(present(h, &f, ++base) == 0 && strstr(json_of(h), "\"last_reason\":\"none\"") != NULL && h->toast_count == 0, "in Original no extension is requested, so none is refused");
    /* undeclared scene: shown content is never support */
    set(h, 16, 9, 2, SV_SIDE_RAW);
    f = make(1, 2, 0);
    CHECK(present(h, &f, ++base) == 0 && jnum(json_of(h), "*", "scenery", "undeclared") == 1 && jnum(json_of(h), "*", "scenery", "covered") == 0 && jnum(json_of(h), "*", "hud", "covered") == 0, "an unknown scene is unscoped and undeclared, never covered");
    /* virtual sprites: the stage reached decides, nothing before COMPOSED is coverage */
    f = make(1, 0, 8);
    CHECK(present(h, &f, ++base) == 0 && jtop(json_of(h), "last_vsprite_reason") == 6 && strstr(json_of(h), "\"last_reason\":\"virtual_sprite_rejected\"") != NULL && jnum(json_of(h), "a", "actors", "covered") == 0, "a rejected set keeps its GenesisVirtualReason and delivers nothing");
    f = make(1, 0, 7);
    CHECK(present(h, &f, ++base) == 0 && strstr(json_of(h), "\"last_reason\":\"virtual_sprite_unpresented\"") != NULL && jnum(json_of(h), "a", "actors", "covered") == 0 && jnum(json_of(h), "a", "actors", "offered") >= 1, "a validated but unpresented set is not delivered");
    f = make(1, 0, 11);
    CHECK(present(h, &f, ++base) == 0 && strstr(json_of(h), "\"last_reason\":\"unverified_state\"") != NULL && jnum(json_of(h), "a", "actors", "covered") == 0, "a game-state set without a verified snapshot delivers nothing");
    { const long before = jnum(json_of(h), "a", "actors", "covered");
      f = make(1, 0, 9);
      CHECK(present(h, &f, ++base) == 0 && jnum(json_of(h), "a", "actors", "covered") == before + 1 && jnum(json_of(h), "a", "scenery", "covered") >= 1, "a composed authored set in a declared scene is the only actor delivery");
      f = make(3, 0, 9);
      set(h, 32, 9, 2, SV_SIDE_RAW);
      CHECK(present(h, &f, ++base) == 0 && jnum(json_of(h), "a", "actors", "covered") == before + 1, "a composed set outside the declared range is not delivery");
    }
  }
  sv_host_close(h);
  remove(g_settings);

  /* 3. a planner rejection (32:9 for a 160 x 448 picture needs 716 > 672 pixels per side) refuses the WHOLE extension presentation even though the supplied surface is structurally valid:
   * native picture only, no columns, no HUD, no composed-sprite credit, output equal to the native-only reference, and a valid aspect restores normal presentation. */
  h = open_host("max=32:9;scene=p:actors+hud+scenery");
  if (h != NULL) {
    static uint8_t nat[160 * 448 * 3];
    static uint8_t big[(160 + 2 * 672) * 448 * 3];
    SvExtendedSurface e;
    uint64_t native_ref, got;
    uint64_t sq = 9;
    size_t k;
    for (k = 0; k < sizeof nat; ++k) nat[k] = (uint8_t)(k * 11u + 3u);
    for (k = 0; k < sizeof big; ++k) big[k] = (uint8_t)(k * 29u + 7u);
    sv_host_set_scene(h, "p");
    set(h, 32, 9, 2, SV_SIDE_RAW);
    (void)sv_host_present(h, nat, 160, 448, 160 * 3);
    sv_host_toast(h, "");   /* the first frame's one-time hint is not part of the reference */
    (void)sv_host_present(h, nat, 160, 448, 160 * 3);
    native_ref = out_hash(h);
    memset(&e, 0, sizeof e);
    e.rgb24 = big; e.size = sizeof big; e.width = 160 + 2 * 100; e.height = 448; e.pitch = (160 + 2 * 100) * 3; e.side = 100; e.sequence = ++sq; e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_AUTHORED;
    e.overlay_rgba = g_ovl; e.overlay_w = 8; e.overlay_h = 8;
    e.actor_stage = SV_ACT_COMPOSED; e.actor_provenance = 2;
    CHECK(sv_extended_surface_check(&e, 160, 448, sq, 320) == SV_EXT_OK, "the fixture surface is structurally valid");
    sv_host_toast(h, "");
    CHECK(sv_host_present_extended(h, nat, 160, 448, 160 * 3, sq, &e) == 0, "the frame is presented");
    CHECK(sv_host_extension_side(h) == 0 && h->ovl_w == 0 && h->ext_side == 0, "planner rejection: zero visible extension side width and no retained HUD");
    CHECK(toast_has(h, "does not fit this picture"), "planner rejection: the user diagnostic names the reason");
    { const char *j = json_of(h);
      CHECK(strstr(j, "\"last_reason\":\"over_declared_geometry\"") != NULL && strstr(j, "\"over_declared_geometry\":1") != NULL, "planner rejection: the stable reason is recorded");
      CHECK(jnum(j, "p", "scenery", "presented") == 0 && jnum(j, "p", "hud", "presented") == 0 && jnum(j, "p", "actors", "presented") == 0 &&
            jnum(j, "p", "scenery", "covered") == 0 && jnum(j, "p", "hud", "covered") == 0 && jnum(j, "p", "actors", "covered") == 0 &&
            jnum(j, "p", "scenery", "missing") == 1 && jnum(j, "p", "hud", "missing") == 1 && jnum(j, "p", "actors", "missing") == 1 &&
            jnum(j, "p", "actors", "offered") == 1 && jnum(j, "p", "actors", "accepted") == 0 && jnum(j, "p", "actors", "rejected") == 1, "planner rejection: nothing presented or covered, every offered component rejected"); }
    { char note[512]; sv_host_geometry_note(h, note, sizeof note);
      CHECK(strstr(note, "so only the native picture is shown") != NULL && strstr(note, "Last frame") == NULL, "planner rejection: the Video-page note states the planner reason once"); }
    /* the same refused frame again (the notice is gone: it is raised once per entry): the pixels are exactly the native-only reference */
    sv_host_toast(h, "");
    e.sequence = ++sq;
    (void)sv_host_present_extended(h, nat, 160, 448, 160 * 3, sq, &e);
    got = out_hash(h);
    CHECK(got == native_ref, "planner rejection: output pixels equal the native-only reference");
    CHECK(h->toast_count == 0, "planner rejection: no repeated notice");
    /* a valid aspect restores normal presentation at once */
    set(h, 16, 9, 2, SV_SIDE_RAW);
    e.width = 160 + 2 * 318; e.pitch = e.width * 3; e.side = 318; e.sequence = ++sq; e.size = (size_t)e.pitch * 448u;
    CHECK(sv_extended_surface_check(&e, 160, 448, sq, 320) == SV_EXT_OK, "the 16:9 surface is valid");
    CHECK(sv_host_present_extended(h, nat, 160, 448, 160 * 3, sq, &e) == 0 && sv_host_extension_side(h) == 318 && h->ovl_w == 8, "a valid aspect: the extension and the HUD are shown again");
    { const char *j = json_of(h);
      CHECK(strstr(j, "\"last_reason\":\"none\"") != NULL && jnum(j, "p", "scenery", "covered") == 1 && jnum(j, "p", "hud", "covered") == 1 && jnum(j, "p", "actors", "covered") == 1, "a valid aspect: coverage resumes and the reason clears"); }
    sv_host_close(h);
    remove(g_settings);
  }

  /* 4. a package that declares nothing still gets a meaningful reason, never a stale one, and normal HUD-only presentation raises nothing */
  h = open_host(NULL);
  if (h != NULL) {
    char note[512];
    Frame f = make(1, 0, 17);   /* a HUD-only surface at a requested 16:9 */
    set(h, 16, 9, 2, SV_SIDE_RAW);
    sv_host_toast(h, "");
    CHECK(present(h, &f, 1) == 0, "legacy: a HUD-only frame is presented");
    sv_host_geometry_note(h, note, sizeof note);
    CHECK(strstr(note, "Last frame") == NULL, "legacy: a HUD-only frame has no rejection reason");
    sv_host_toast(h, "");   /* the one-time pause-menu hint of a first frame is not a rejection */
    CHECK(present(h, &f, 1) == 0 && h->toast_count == 0, "legacy: normal HUD-only presentation raises no notice");
    f = make(1, 0, 19);   /* a surface produced above the requested level */
    set(h, 16, 9, 1, SV_SIDE_RAW);
    CHECK(present(h, &f, 2) == 0, "legacy: a refused frame is presented as the native picture");
    sv_host_geometry_note(h, note, sizeof note);
    CHECK(strstr(note, "Last frame: mode or provenance not supported") != NULL, "legacy: the note explains a real rejection");
    f = make(1, 0, 0);
    set(h, 16, 9, 2, SV_SIDE_RAW);
    CHECK(present(h, &f, 3) == 0, "legacy: a clean frame is presented");
    sv_host_geometry_note(h, note, sizeof note);
    CHECK(strstr(note, "Last frame") == NULL, "legacy: a clean frame shows no stale reason");
    sv_host_close(h);
    remove(g_settings);
  }
  printf(failures == 0 ? "sv_scene_rejection_test: all checks passed\n" : "sv_scene_rejection_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
