/* SEG-124 (ADR 0267): a verified native compatibility provider attached to the real SDL3 Genesis viewer adapter: the provider's frame verdict (and only that) decides
 * the provenance the host sees (authored scenery bypasses Comfort; a declined frame keeps Comfort), its scenery words fill the side columns, its HUD is drawn as an
 * overlay in visible side space only, and the end-of-run counters add up. A real GenesisRuntime publishes frames through the real live-frame/raster path; the adapter's
 * request sampling, present callback and the shared SvHost presenter run unmodified (dummy video driver, software renderer). Mock provider; no ROM, no clock. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_compat.h"
#include "runtime.h"
#include "sv_host.h"
#include "sv_layout.h"
#include "vdp_render.h"
#include "viewer_sdl3.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

typedef struct Rig {
  GenesisRuntime *r;
  GenesisFrameArtifact *frame;
  GenesisLiveFrameObserver observer;
  GenesisRasterCapture *capture;
  GenesisSdl3Viewer *viewer;
  GenesisViewerHost host;
  GenesisFrameExtension *ext;
  uint32_t pc;
  uint64_t presented;
} Rig;

static void put_be16(uint8_t *vram, unsigned addr, unsigned v) { vram[addr] = (uint8_t)(v >> 8); vram[addr + 1] = (uint8_t)v; }

/* Plane B: solid tile 1 (colour index 1 = dark red) everywhere; Plane A empty. The provider's scenery word is solid tile 2 (colour index 2 = green): clearly distinguishable. */
static void build_scene(GenesisRuntime *r) {
  uint16_t *regs = r->devices.vdp.registers;
  unsigned row, c;
  memset(r->devices.vdp.vram, 0, GENESIS_VDP_VRAM_BYTES);
  memset(&r->devices.vdp.vram[1 * 32], 0x11, 32);
  memset(&r->devices.vdp.vram[2 * 32], 0x22, 32);
  for (row = 0; row < 32; ++row)
    for (c = 0; c < 64; ++c) put_be16(r->devices.vdp.vram, 0xE000 + (row * 64 + c) * 2, 1);
  put_be16(r->devices.vdp.cram, 1 * 2, 7 << 1);              /* index 1: red */
  put_be16(r->devices.vdp.cram, 2 * 2, 7 << 5);              /* index 2: green */
  memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[1] = 0x04; regs[2] = 0x30; regs[3] = 0x34; regs[4] = 0x07; regs[5] = 0x7C; regs[12] = 0x81; regs[13] = 0x3F; regs[16] = 0x01;
}

static int rig_open(Rig *g) {
  memset(g, 0, sizeof *g);
  g->r = (GenesisRuntime *)calloc(1, sizeof *g->r);
  g->frame = (GenesisFrameArtifact *)calloc(1, sizeof *g->frame);
  g->capture = (GenesisRasterCapture *)calloc(1, sizeof *g->capture);
  g->viewer = genesis_sdl3_viewer_open("native compat test", 3);
  if (g->r == NULL || g->frame == NULL || g->capture == NULL || g->viewer == NULL) return -1;
  g->r->pc = 0x200u; g->r->sr = 0x2000u; g->r->a[7] = 0x00FF0000u + 0x8000u;
  build_scene(g->r);
  g->observer.producer = genesis_vdp_produce_frame;
  g->observer.latest = g->frame;
  g->capture->producer = genesis_vdp_produce_frame_raster;
  g->ext = genesis_sdl3_viewer_extension(g->viewer, &g->observer.sequence);
  g->capture->extension = g->ext;
  g->r->live_frame_observer = &g->observer;
  g->r->live_raster = g->capture;
  genesis_sdl3_viewer_host(g->viewer, &g->host);
  g->pc = 0x200u;
  return g->ext != NULL ? 0 : -1;
}
static void rig_close(Rig *g) {
  if (g->viewer != NULL) genesis_sdl3_viewer_close(g->viewer);
  free(g->r); free(g->frame); free(g->capture);
}
static int run_frame(Rig *g) {
  const uint64_t vf = g->r->virtual_frame_count;
  if (g->host.window_closed(g->host.ctx)) return -1;
  while (g->r->virtual_frame_count == vf) g->pc = genesis_runtime_retire_m68k_instruction(g->r, 4u, g->pc).next_pc;
  if (g->observer.sequence != g->presented && genesis_frame_artifact_is_populated(g->frame)) {
    if (g->host.present(g->host.ctx, g->frame) != 0) return -2;
    g->presented = g->observer.sequence;
  }
  return 0;
}
static void set_mode(Rig *g, SvWidescreenMode m, SvSideStyle style, int width) {
  SvHost *h = genesis_sdl3_viewer_sv_host(g->viewer);
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = m; s.video.widescreen_side_style = style; s.video.widescreen_width = width; s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(h, &s);
}

/* ---- the mock provider ---- */
typedef struct Mock { unsigned accept; uint8_t reason; unsigned prepared; } Mock;
static void mock_prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  Mock *m = (Mock *)state;
  ++m->prepared;
  out->accepted = m->accept & f->requested;
  out->reason = m->reason;
}
static int mock_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  (void)state; (void)row; (void)x; (void)hscroll; (void)vscroll;
  *word = plane == 0U ? 0U : 2U;   /* Plane A blank, Plane B: solid tile 2 */
  return 1;
}
static int mock_hud(void *state, GenesisCompatOverlay *o) {
  unsigned i;
  (void)state;
  o->width = 12; o->height = 9;
  for (i = 0; i < 12U * 9U; ++i) { o->rgba[i * 4] = 10; o->rgba[i * 4 + 1] = 200; o->rgba[i * 4 + 2] = 233; o->rgba[i * 4 + 3] = 255; }
  return 1;
}
static const GenesisNativeCompat k_mock = {GENESIS_NATIVE_COMPAT_ABI, "genesis.fixture.native-widescreen.v1", 1, GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD, mock_prepare, mock_word, mock_hud};

static int rgb_at(SDL_Surface *a, int x, int y, int *r, int *g, int *b) {
  Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
  if (!SDL_ReadSurfacePixel(a, x, y, &rr, &gg, &bb, &aa)) return 0;
  *r = rr; *g = gg; *b = bb;
  return 1;
}
static int count_colour(SDL_Surface *a, int x0, int y0, int w, int h, int cr, int cg, int cb) {
  int x, y, r, g, b, n = 0;
  for (y = y0; y < y0 + h; ++y)
    for (x = x0; x < x0 + w; ++x) { rgb_at(a, x, y, &r, &g, &b); if (r == cr && g == cg && b == cb) ++n; }
  return n;
}

int main(void) {
  Rig g;
  SvHost *host;
  SvExtendedLayout lay;
  SDL_Surface *a;
  GenesisNativeCompatStats stats;
  Mock mock;
  int i, k;
  char file[600];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : "/tmp";
  snprintf(file, sizeof file, "%s/genesis_native_compat_sdl_%d.cfg", tmp, (int)SDL_GetTicks());
  remove(file);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", file, 1);
  if (rig_open(&g) != 0) { printf("FAIL: the viewer and its extension open\n"); return 1; }
  host = genesis_sdl3_viewer_sv_host(g.viewer);
  memset(&mock, 0, sizeof mock);
  mock.accept = GENESIS_COMPAT_SCENERY | GENESIS_COMPAT_HUD;

  /* attachment is validated: nothing but the viewer's own extension target, an ABI-1 provider and a subset of its components */
  CHECK(genesis_sdl3_viewer_attach_native_compat(NULL, g.ext, &k_mock, &mock, 3) != 0 && genesis_sdl3_viewer_attach_native_compat(g.viewer, NULL, &k_mock, &mock, 3) != 0 &&
            genesis_sdl3_viewer_attach_native_compat(g.viewer, g.ext, NULL, &mock, 3) != 0 && genesis_sdl3_viewer_attach_native_compat(g.viewer, g.ext, &k_mock, &mock, 0) != 0 &&
            genesis_sdl3_viewer_attach_native_compat(g.viewer, g.ext, &k_mock, &mock, 4) != 0,
        "attachment refuses missing arguments, no component and an unknown component");
  { GenesisFrameExtension other; GenesisNativeCompat bad = k_mock; memset(&other, 0, sizeof other);
    CHECK(genesis_sdl3_viewer_attach_native_compat(g.viewer, &other, &k_mock, &mock, 3) != 0, "a foreign extension target is refused");
    bad.abi_version = 7;
    CHECK(genesis_sdl3_viewer_attach_native_compat(g.viewer, g.ext, &bad, &mock, 3) != 0 && g.ext->compat == NULL, "another ABI version is refused and nothing is attached"); }
  CHECK(genesis_sdl3_viewer_attach_native_compat(g.viewer, g.ext, &k_mock, &mock, 3) == 0 && g.ext->compat == &k_mock && g.ext->compat_ram != NULL && g.ext->overlay_rgba != NULL, "a valid provider attaches");

  /* Level 3 + Comfort (the default style): frame 1 has no snapshot (declined: generic + Comfort); from frame 2 the provider serves the frame: authored, Comfort bypassed */
  set_mode(&g, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_COMFORT, 100);
  CHECK(run_frame(&g) == 0, "frame 1");
  CHECK(g.ext->result.compat_accepted == 0 && g.ext->result.compat_reason == GENESIS_COMPAT_STALE_STATE && sv_host_extension_source(host) == SV_EXTSRC_GENERIC && sv_host_side_treatment(host) == SV_TREAT_COMFORT,
        "first frame: no snapshot, generic provenance, Comfort treats the sides");
  CHECK(run_frame(&g) == 0, "frame 2");
  CHECK(g.ext->result.compat_accepted == 3 && g.ext->result.authored == 1 && sv_host_extension_source(host) == SV_EXTSRC_AUTHORED && sv_host_side_treatment(host) == SV_TREAT_PASSTHROUGH,
        "a served frame is authored: Comfort is bypassed");
  CHECK(sv_host_extension_side(host) == 39, "the extension is 39 pixels per side at 16:9");
  sv_host_toast(host, "");
  a = sv_host_capture_output(host);
  CHECK(a != NULL && sv_layout_extended_dest(a->w, a->h, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, "capture and layout");
  if (a == NULL) { rig_close(&g); return 1; }
  k = lay.native.w / 320;
  {
    const int ly = lay.native.y + 20 * k, lw = lay.native.x - lay.surface.x, rx = lay.native.x + lay.native.w, rw = lay.surface.x + lay.surface.w - rx;
    int r, gg, b;
    /* the provider's solid green tile is on both sides (a row away from the HUD); the native centre is the scene's own red */
    CHECK(count_colour(a, lay.surface.x, ly, lw, 1, 0, 255, 0) == lw && count_colour(a, rx, ly, rw, 1, 0, 255, 0) == rw, "the side columns are the provider's cells (solid green), not the VDP's wrapped red");
    rgb_at(a, lay.native.x + lay.native.w / 2, ly, &r, &gg, &b);
    CHECK(r == 255 && gg == 0 && b == 0, "the native centre is the game's own picture");
    /* the HUD overlay replaces the game's own: left edge at the left edge of the visible side, picture rows 1:1 (row 0 here) */
    {
      const int ox = lay.surface.x, oy = lay.native.y;
      CHECK(count_colour(a, ox, oy, 12 * k, 9 * k, 10, 200, 233) == 12 * k * 9 * k, "the HUD overlay is at the left edge of the visible side");
      CHECK(count_colour(a, rx, lay.native.y, rw, lay.native.h, 10, 200, 233) == 0 && count_colour(a, lay.native.x, lay.native.y, lay.native.w, lay.native.h, 10, 200, 233) == 0,
            "no overlay pixel on the right side or over the native picture (it is narrower than the side)");
    }
    SDL_DestroySurface(a);
  }
  /* the same frame retained: Borders replaces the sides (explicit user request) but the overlay stays; Raw shows the provider's cells */
  set_mode(&g, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_BORDERS, 100);
  sv_host_toast(host, ""); a = sv_host_capture_output(host);
  CHECK(a != NULL && sv_host_side_treatment(host) == SV_TREAT_BORDERS && count_colour(a, lay.surface.x, lay.native.y, lay.native.x - lay.surface.x, 9 * k, 10, 200, 233) > 0,
        "Borders keeps the HUD overlay");
  if (a != NULL) SDL_DestroySurface(a);

  /* the width slider: at 4:3 there is no visible side, so no extra scenery; the HUD that replaces the game's own is drawn inside the picture (at the place of the one it replaces, 0,0 here) */
  set_mode(&g, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_RAW, 0);
  sv_host_toast(host, ""); a = sv_host_capture_output(host);
  { const SvRect pic = sv_layout_dest(a != NULL ? a->w : 1, a != NULL ? a->h : 1, 320, 224, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER); const int kk = pic.w / 320;
    CHECK(a != NULL && sv_host_visible_side(host) == 0 && count_colour(a, pic.x, pic.y, pic.w, pic.h, 10, 200, 233) == 12 * kk * 9 * kk && count_colour(a, 0, 0, a->w, a->h, 0, 255, 0) == 0,
          "at 4:3 the HUD is drawn inside the picture and no extra scenery is shown"); }
  if (a != NULL) SDL_DestroySurface(a);
  set_mode(&g, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_RAW, 100);

  /* Level 2 keeps the provider (scenery authored) and the HUD; Original shows no extra scenery but keeps the HUD */
  set_mode(&g, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_SIDE_COMFORT, 100);
  CHECK(run_frame(&g) == 0 && g.ext->result.authored == 1 && sv_host_extension_source(host) == SV_EXTSRC_AUTHORED, "Level 2 frame served by the provider");
  set_mode(&g, SV_WIDESCREEN_ORIGINAL, SV_SIDE_COMFORT, 100);
  { const unsigned before = mock.prepared;
    CHECK(run_frame(&g) == 0 && mock.prepared == before + 1 && g.ext->result.compat_accepted == GENESIS_COMPAT_HUD, "Original: the provider is asked for the HUD only");
    sv_host_toast(host, ""); a = sv_host_capture_output(host);
    CHECK(g.ext->request == 1 && g.ext->compat_components == GENESIS_COMPAT_HUD, "Original with a native HUD: the producer still runs, for the HUD only");
    CHECK(a != NULL && sv_host_extension_side(host) == 0 && count_colour(a, 0, 0, a->w, a->h, 10, 200, 233) > 0 && count_colour(a, 0, 0, a->w, a->h, 0, 255, 0) == 0, "Original shows no extra scenery but keeps the HUD");
    if (a != NULL) SDL_DestroySurface(a); }

  /* a provider that declines: the generic frame with Comfort, no HUD, and the verdict reason is counted */
  set_mode(&g, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_COMFORT, 100);
  mock.accept = 0; mock.reason = GENESIS_COMPAT_UNSUPPORTED_SCENE;
  for (i = 0; i < 3; ++i) CHECK(run_frame(&g) == 0, "declined frame");
  CHECK(g.ext->result.compat_accepted == 0 && g.ext->result.authored == 0 && sv_host_extension_source(host) == SV_EXTSRC_GENERIC && sv_host_side_treatment(host) == SV_TREAT_COMFORT,
        "a declined frame is generic and keeps Comfort");
  sv_host_toast(host, ""); a = sv_host_capture_output(host);
  CHECK(a != NULL && count_colour(a, 0, 0, a->w, a->h, 10, 200, 233) == 0, "a declined frame draws no HUD");
  if (a != NULL) SDL_DestroySurface(a);
  /* HUD only: authored is not claimed, Comfort still treats the (generic) scenery, the overlay is drawn */
  mock.accept = GENESIS_COMPAT_HUD; mock.reason = 0;
  for (i = 0; i < 2; ++i) CHECK(run_frame(&g) == 0, "HUD-only frame");
  CHECK(g.ext->result.compat_accepted == GENESIS_COMPAT_HUD && g.ext->result.authored == 0 && sv_host_extension_source(host) == SV_EXTSRC_GENERIC && sv_host_side_treatment(host) == SV_TREAT_COMFORT,
        "HUD only: generic scenery still gets Comfort");
  sv_host_toast(host, ""); a = sv_host_capture_output(host);
  CHECK(a != NULL && count_colour(a, 0, 0, a->w, a->h, 10, 200, 233) > 0, "HUD only: the overlay is drawn");
  if (a != NULL) SDL_DestroySurface(a);

  genesis_sdl3_viewer_native_compat_stats(g.viewer, &stats);
  CHECK(stats.frames >= 8 && stats.authored >= 2 && stats.hud_frames >= 4 && stats.declined[GENESIS_COMPAT_STALE_STATE] >= 1 && stats.declined[GENESIS_COMPAT_UNSUPPORTED_SCENE] >= 3,
        "the end-of-run counters add up");
  rig_close(&g);
  if (failures != 0) { printf("genesis_native_compat_viewer_sdl_test: %d failure(s)\n", failures); return 1; }
  printf("genesis_native_compat_viewer_sdl_test: OK\n");
  return 0;
}
