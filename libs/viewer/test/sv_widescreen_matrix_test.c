/* SEG-139-T006: the repeatable synthetic widescreen matrix. One deterministic fictional scene (no commercial content) is planned, produced and presented through the shared
 * host at 4:3, 16:9, 21:9 and 32:9. Per aspect it records frame correctness (the composed output is compared with an independent construction AND with a committed golden hash),
 * coverage (a fictional provider declares `max=21:9`, so 32:9 is honestly PARTIAL: generic presentation, not authored), the host's per-frame time against a coarse budget, and the
 * surface memory. Integer 2x scale in a window of exactly twice the surface makes the composed output a pixel replication, so the hash does not depend on the renderer's filtering.
 * It prints one `WIDESCREEN_MATRIX {json}` line per aspect for the record. Strict C11. */
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
#define COARSE_BUDGET_MS 250.0 /* a regression tripwire for the software renderer of a slow CI host, not a benchmark */
#define FRAMES 15

static char g_settings[700];
static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) { size_t i; for (i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull; return h; }

static void native_px(int x, int y, uint8_t *o) { o[0] = (uint8_t)((x * 3 + y) & 255); o[1] = (uint8_t)((y * 5 + (x >> 3) * 17) & 255); o[2] = (uint8_t)(((x ^ y) * 7) & 255); }
/* the fictional extension: 16-column stripes by distance from the picture edge, so the outermost column depends on the aspect */
static void ext_px(int d_from_edge, int y, uint8_t *o) { const int a = (d_from_edge / 16) & 1; o[0] = a ? 240 : 30; o[1] = (uint8_t)(60 + (y & 63)); o[2] = a ? 30 : 240; }

typedef struct Row { const char *name; SvDisplayRatio ratio; int side; int surface_w; size_t surface_bytes; uint64_t golden; } Row;
/* sides are the hand calculation floor(224*n/9)=surface, (surface-320)/2; goldens pin the fictional scene generator and were first matched against the independent construction below */
static const Row rows[4] = {
  {"4:3", {4, 3}, 0, 320, 320u * 224u * 3u, 0xa659fbcb2bce5a83ull},
  {"16:9", {16, 9}, 39, 398, 398u * 224u * 3u, 0x2125da0c004f6f63ull},
  {"21:9", {21, 9}, 101, 522, 522u * 224u * 3u, 0xe86edf64ba877763ull},
  {"32:9", {32, 9}, 238, 796, 796u * 224u * 3u, 0x1be6033adcd46e43ull},
};

static uint8_t g_native[NW * NH * 3];
static uint8_t g_surface[(NW + 2 * 238) * NH * 3];
static uint8_t g_hud[8 * 8 * 4];

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

/* the expected composed output, built without the host: every surface pixel replicated 2 x 2 */
static uint64_t expected_hash(int surface_w, int hud) {
  uint64_t h = 1469598103934665603ull;
  int x, y, k;
  for (y = 0; y < NH * 2; ++y)
    for (x = 0; x < surface_w * 2; ++x) {
      uint8_t px[3];
      const int sx = x / 2, sy = y / 2, side = (surface_w - NW) / 2;
      if (hud && sx < 8 && sy < 8) { px[0] = px[1] = px[2] = 255; }   /* the opaque 8 x 8 HUD at the left edge of the visible side */
      else if (sx >= side && sx < side + NW) native_px(sx - side, sy, px); else ext_px(sx < side ? side - 1 - sx : sx - side - NW, sy, px);
      for (k = 0; k < 3; ++k) h = (h ^ px[k]) * 1099511628211ull;
    }
  return h;
}

static int cmp_d(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(void) {
  char dir[512], json[4096];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  SvHostConfig cfg;
  SvHost *h;
  double med[4];
  int i, f;
  uint64_t seq = 1;
  snprintf(dir, sizeof dir, "%s/sv_widescreen_matrix_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(g_settings, sizeof g_settings, "%s/m.cfg", dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  memset(g_hud, 255, sizeof g_hud);
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "matrix"; cfg.picture_w = NW; cfg.picture_h = NH; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  cfg.geometry_declaration = "max=21:9;scene=synthetic:hud+scenery"; /* the fictional provider claims up to 21:9 */
  h = sv_host_open(&cfg);
  CHECK(h != NULL, "host opens");
  if (h == NULL) return 1;
  sv_host_set_scene(h, "synthetic");
  for (i = 0; i < 4; ++i) {
    const Row *r = &rows[i];
    SvGeomPlan plan;
    SvSettings s = *sv_host_settings(h);
    SDL_Surface *out;
    double t[FRAMES];
    const char *verdict;
    int fallback, correct, hit_golden;
    uint64_t got = 1469598103934665603ull, want;
    CHECK(sv_geom_plan(NW, NH, SV_ASPECT_ORIGINAL, r->ratio, &plan) == SV_GEOM_OK && plan.side == r->side && plan.surface_w == r->surface_w && plan.surface_bytes == r->surface_bytes,
          "the planner gives the hand-calculated side, width and bytes");
    CHECK(plan.surface_bytes <= SV_GEOM_GENESIS_MAX_SURFACE_BYTES, "inside the one fixed maximum allocation");
    s.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY; s.video.widescreen_side_style = SV_SIDE_RAW; s.video.widescreen_ratio = r->ratio; s.video.widescreen_width = 100;
    s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_INTEGER;
    sv_host_apply_settings(h, &s);
    SDL_SetWindowSize(h->window, r->surface_w * 2, NH * 2);
    build(r->side);
    for (f = 0; f < FRAMES; ++f) {
      const Uint64 t0 = SDL_GetTicksNS();
      int rc;
      ++seq;
      if (r->side == 0) {
        rc = sv_host_present(h, g_native, NW, NH, NW * 3);   /* nothing to extend at 4:3 for a 320 x 224 square-pixel picture */
      } else {
        SvExtendedSurface e;
        memset(&e, 0, sizeof e);
        e.rgb24 = g_surface; e.size = (size_t)(NW + 2 * r->side) * NH * 3u; e.width = NW + 2 * r->side; e.height = NH; e.pitch = (NW + 2 * r->side) * 3; e.side = r->side; e.sequence = seq;
        e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_AUTHORED;
        e.overlay_rgba = g_hud; e.overlay_w = 8; e.overlay_h = 8;
        rc = sv_host_present_extended(h, g_native, NW, NH, NW * 3, seq, &e);
      }
      t[f] = (double)(SDL_GetTicksNS() - t0) / 1.0e6;
      CHECK(rc == 0, "the frame is presented");
    }
    qsort(t, FRAMES, sizeof t[0], cmp_d);
    med[i] = t[FRAMES / 2];
    CHECK(med[i] < COARSE_BUDGET_MS, "the host meets the coarse per-frame budget");
    sv_host_toast(h, "");
    out = sv_host_capture_output(h);
    CHECK(out != NULL && out->w == r->surface_w * 2 && out->h == NH * 2, "the output is exactly the 2x surface");
    if (out != NULL) {
      int x, y;
      for (y = 0; y < out->h; ++y)
        for (x = 0; x < out->w; ++x) {
          Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
          uint8_t px[3];
          (void)SDL_ReadSurfacePixel(out, x, y, &rr, &gg, &bb, &aa);
          px[0] = rr; px[1] = gg; px[2] = bb;
          got = fnv(px, 3, got);
        }
      SDL_DestroySurface(out);
    }
    fallback = sv_host_geometry_fallback(h);
    want = expected_hash(r->surface_w, r->side > 0 && !fallback);
    /* inside the declared range the relocated HUD is part of the frame; outside it the generic path drops it (and the HUD is then absent from the expected construction too) */
    correct = got == want;
    hit_golden = got == r->golden;
    CHECK(correct, "the composed frame equals the independent construction");
    CHECK(hit_golden, "the composed frame equals the committed golden hash");
    CHECK(sv_host_extension_side(h) == r->side && sv_host_visible_side(h) == r->side, "the planned side is the side shown");
    CHECK((i < 3 && !fallback) || (i == 3 && fallback), "coverage: 4:3, 16:9 and 21:9 are inside the declared 21:9, 32:9 is outside");
    verdict = !correct || !hit_golden ? "fail" : fallback ? "partial" : "pass";
    CHECK((i < 3 && strcmp(verdict, "pass") == 0) || (i == 3 && strcmp(verdict, "partial") == 0), "the matrix verdict is pass, pass, pass, partial");
    (void)sv_host_coverage_json(h, json, sizeof json);
    printf("WIDESCREEN_MATRIX {\"aspect\":\"%s\",\"side\":%d,\"surface_w\":%d,\"surface_bytes\":%zu,\"frame_hash\":\"%016llx\",\"verdict\":\"%s\",\"coverage_fallback\":%s,\"host_median_ms\":%.3f}\n",
           r->name, r->side, r->surface_w, r->surface_bytes, (unsigned long long)got, verdict, fallback ? "true" : "false", med[i]);
  }
  (void)sv_host_coverage_json(h, json, sizeof json);
  CHECK(strstr(json, "\"outside_range_frames\":15") != NULL && strstr(json, "\"scenery\":{\"declared\":true,\"covered\":30,\"missing\":0,\"undeclared\":0,\"outside_range\":15}") != NULL,
        "the coverage report counts 30 covered scenery frames (16:9, 21:9) and 15 outside frames (32:9); the native 4:3 frames deliver no extension");
  printf("WIDESCREEN_MATRIX_DELTA {\"16:9_ms\":%.3f,\"21:9_ms\":%.3f,\"32:9_ms\":%.3f}\n", med[1], med[2], med[3]);
  sv_host_close(h);
  remove(g_settings);
  printf(failures == 0 ? "sv_widescreen_matrix_test: all checks passed\n" : "sv_widescreen_matrix_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
