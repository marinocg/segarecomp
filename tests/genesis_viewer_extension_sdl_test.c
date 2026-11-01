/* SEG-118 (ADR 0262): the real SDL3 Genesis viewer adapter showing Level 2 extended backgrounds, read back from the rendered output.
 *
 * Project-authored synthetic VDP scene (Plane B columns of solid colour tiles; no ROM, no clock). A real GenesisRuntime publishes frames through
 * the real live-frame/raster path with the adapter's extension target attached; the adapter's own request sampling, present callback, capability
 * report and the shared SvHost presenter run unmodified (dummy video driver, software renderer). The expected colour of every probed pixel is derived
 * from the tile/cell arithmetic of the scene, not from the renderer. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "sv_host.h"
#include "sv_layout.h"
#include "vdp_render.h"
#include "viewer_sdl3.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static const unsigned kColors[5][3] = {{0, 0, 0}, {7, 0, 0}, {0, 7, 0}, {0, 0, 7}, {7, 7, 0}};

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

/* Plane B (0xE000, 64x32): cell column c shows solid tile 1 + (c % 4) (colour index 1..4); Plane A and the window are empty (transparent). */
static void build_scene(GenesisRuntime *r, int h40, int window_left) {
  uint16_t *regs = r->devices.vdp.registers;
  unsigned i, row, c;
  memset(r->devices.vdp.vram, 0, GENESIS_VDP_VRAM_BYTES);
  for (i = 1; i <= 4; ++i) memset(&r->devices.vdp.vram[i * 32], (int)((i << 4) | i), 32);
  for (row = 0; row < 32; ++row)
    for (c = 0; c < 64; ++c) put_be16(r->devices.vdp.vram, 0xE000 + (row * 64 + c) * 2, 1 + (c % 4));
  for (i = 1; i <= 4; ++i) put_be16(r->devices.vdp.cram, i * 2, (kColors[i][2] << 9) | (kColors[i][1] << 5) | (kColors[i][0] << 1));
  memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[1] = 0x04; regs[2] = 0x30; regs[3] = 0x34; regs[4] = 0x07; regs[5] = 0x7C; regs[11] = 0x00;
  regs[12] = (uint16_t)(h40 ? 0x81 : 0x00); regs[13] = 0x3F; regs[16] = 0x01;
  if (window_left) regs[17] = 2; /* the window plane owns the left columns */
}

static void expected_rgb(int x, uint8_t out[3]) { /* screen column x (any integer) -> colour of the scene, hscroll 0, 64 cells wide */
  int px = x % 512; if (px < 0) px += 512;
  { const unsigned t = 1 + (unsigned)(px / 8) % 4;
    out[0] = (uint8_t)(kColors[t][0] * 255 / 7); out[1] = (uint8_t)(kColors[t][1] * 255 / 7); out[2] = (uint8_t)(kColors[t][2] * 255 / 7); }
}

static int rig_open(Rig *g, int h40, int window_left) {
  memset(g, 0, sizeof *g);
  g->r = (GenesisRuntime *)calloc(1, sizeof *g->r);
  g->frame = (GenesisFrameArtifact *)calloc(1, sizeof *g->frame);
  g->capture = (GenesisRasterCapture *)calloc(1, sizeof *g->capture);
  g->viewer = genesis_sdl3_viewer_open("extension test", 3);
  if (g->r == NULL || g->frame == NULL || g->capture == NULL || g->viewer == NULL) return -1;
  g->r->pc = 0x200u; g->r->sr = 0x2000u; g->r->a[7] = 0x00FF0000u + 0x8000u;
  build_scene(g->r, h40, window_left);
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

/* One host slice as genesis_viewer_run does it: poll/sample (requested mode read here), run the guest to the next virtual frame boundary, then present. */
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

static void set_mode(Rig *g, SvWidescreenMode m, SvAspect aspect, SvScaleMode scale) {
  SvHost *h = genesis_sdl3_viewer_sv_host(g->viewer);
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = m; s.video.aspect = aspect; s.video.scale = scale;
  sv_host_apply_settings(h, &s);
}

static int pixel(SDL_Surface *a, int x, int y, uint8_t out[3]) {
  Uint8 r = 0, g = 0, b = 0, al = 0;
  if (!SDL_ReadSurfacePixel(a, x, y, &r, &g, &b, &al)) return 0;
  out[0] = r; out[1] = g; out[2] = b;
  return 1;
}
static uint64_t surface_hash(SDL_Surface *s) {
  uint64_t h = UINT64_C(1469598103934665603);
  int x, y;
  uint8_t p[3];
  for (y = 0; y < s->h; ++y) for (x = 0; x < s->w; ++x) { pixel(s, x, y, p); h = (h ^ (uint64_t)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16))) * UINT64_C(1099511628211); }
  return h;
}

/* Checks the rendered output: the native picture exactly centred at whole-number scale, and the scenery on both sides. */
static int check_extended_output(Rig *g, const char *name, int native_w, int side) {
  SvHost *h = genesis_sdl3_viewer_sv_host(g->viewer);
  SvExtendedLayout lay;
  SDL_Surface *a;
  int k, x, ok = 1, scenery_ok = 1;
  char what[128];
  sv_host_toast(h, "");
  a = sv_host_capture_output(h);
  snprintf(what, sizeof what, "%s: output captured", name);
  CHECK(a != NULL, what);
  if (a == NULL) return 0;
  snprintf(what, sizeof what, "%s: layout", name);
  CHECK(sv_layout_extended_dest(a->w, a->h, native_w, 224, side, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, what);
  k = lay.native.w / native_w;
  snprintf(what, sizeof what, "%s: whole-number scale and exact centre", name);
  CHECK(k >= 1 && lay.native.w == native_w * k && lay.native.x == lay.surface.x + side * k && lay.surface.w == (native_w + 2 * side) * k, what);
  for (x = -side; x < native_w + side; x += 3) {
    uint8_t got[3], want[3];
    const int sx = lay.native.x + x * k + k / 2;
    const int sy = lay.surface.y + lay.surface.h / 2;
    expected_rgb(x, want);
    if (!pixel(a, sx, sy, got) || memcmp(got, want, 3) != 0) { if (x < 0 || x >= native_w) scenery_ok = 0; else ok = 0; }
  }
  snprintf(what, sizeof what, "%s: real scenery on both sides of the picture", name);
  CHECK(scenery_ok, what);
  snprintf(what, sizeof what, "%s: native centre columns match the scene", name);
  CHECK(ok, what);
  SDL_DestroySurface(a);
  return scenery_ok && ok;
}

int main(void) {
  Rig g;
  SvHost *host;
  SDL_Surface *a;
  uint64_t original_hash = 0;
  SvWidescreenCaps caps;
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : "/tmp";
  char file[600];
  uint32_t acc, rej, tex;
  int i;
  snprintf(file, sizeof file, "%s/genesis_ext_sdl_%d.cfg", tmp, (int)SDL_GetTicks());
  remove(file);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", file, 1);

  if (rig_open(&g, 1, 0) != 0) { printf("FAIL: the viewer and its extension open\n"); return 1; }
  host = genesis_sdl3_viewer_sv_host(g.viewer);

  /* a viewer that attached an extension reports Level 2 capable, Level 3 never (this milestone) */
  sv_host_widescreen_caps(host, &caps);
  CHECK(caps.extended == SV_WS_AVAILABLE && caps.enhanced == SV_WS_UNAVAILABLE, "caps: Level 2 capable, Level 3 unavailable");

  /* ---- Original: nothing is produced, the output is the plain picture ---- */
  set_mode(&g, SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  for (i = 0; i < 3; ++i) CHECK(run_frame(&g) == 0, "Original frame");
  CHECK(g.ext->request == 0 && g.ext->produced == 0 && sv_host_extension_side(host) == 0 && g.ext->result.status == GENESIS_EXT_NONE, "Original: no extension work, no extension shown");
  sv_host_toast(host, "");
  a = sv_host_capture_output(host);
  CHECK(a != NULL, "Original capture");
  if (a != NULL) { original_hash = surface_hash(a); SDL_DestroySurface(a); }

  /* ---- Level 2 (requested live): real scenery on both sides, exactly centred ---- */
  set_mode(&g, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 39, "Level 2: H40 gets 39 source pixels on each side");
  CHECK(g.ext->result.status == GENESIS_EXT_AVAILABLE && g.ext->result.sequence == g.observer.sequence, "Level 2: Available, stamped with the presented frame");
  check_extended_output(&g, "Level 2 H40", 320, 39);
  sv_host_widescreen_caps(host, &caps);
  CHECK(caps.extended == SV_WS_AVAILABLE, "caps: Available after a clean frame");
  CHECK(sv_host_widescreen_effective(host) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "effective mode is Level 2");

  /* ---- the requested Level 3 falls back to Level 2 honestly ---- */
  set_mode(&g, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 39 && sv_host_widescreen_effective(host) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "Level 3 requested: Level 2 effective, scenery still extended");
  CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "the Level 3 preference is kept");

  /* ---- 4:3 pixel shape: more extension, native stays 4:3 ---- */
  set_mode(&g, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_4_3, SV_SCALE_FIT);
  CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 53, "4:3 aspect: 53 source pixels per side");
  set_mode(&g, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);

  /* ---- back to Original: the output equals the first Original capture byte for byte ---- */
  set_mode(&g, SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 0, "back to Original on the next frame");
  sv_host_toast(host, "");
  a = sv_host_capture_output(host);
  CHECK(a != NULL && surface_hash(a) == original_hash, "Original after Level 2 is byte-identical to Original before");
  if (a != NULL) SDL_DestroySurface(a);

  /* ---- H32 <-> H40 transition re-lays out without a crash ---- */
  set_mode(&g, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 39, "extended H40 again");
  g.r->devices.vdp.registers[12] = 0x00; /* the guest switches to H32 */
  for (i = 0; i < 2; ++i) CHECK(run_frame(&g) == 0, "frame after the H32 switch");
  CHECK(sv_host_extension_side(host) == 71, "H32 gets its own, larger extension (71)");
  check_extended_output(&g, "Level 2 H32", 256, 71);
  g.r->devices.vdp.registers[12] = 0x81;
  for (i = 0; i < 2; ++i) CHECK(run_frame(&g) == 0, "frame after returning to H40");
  CHECK(sv_host_extension_side(host) == 39, "and back to H40");

  /* ---- CRT preset on the extended picture ---- */
  { SvSettings s = *sv_host_settings(host); sv_apply_crt_preset(&s.video, SV_CRT_CLASSIC); s.video.scale = SV_SCALE_FIT; sv_host_apply_settings(host, &s);
    CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 39, "CRT preset: extended frame presented");
    a = sv_host_capture_output(host);
    CHECK(a != NULL, "CRT capture");
    if (a != NULL) { uint8_t p[3]; SvExtendedLayout l; sv_layout_extended_dest(a->w, a->h, 320, 224, 39, s.video.aspect, s.video.scale, &l);
      CHECK(pixel(a, l.surface.x + 2, l.surface.y + l.surface.h / 2, p) && (p[0] + p[1] + p[2]) > 0, "CRT preset: scenery visible on the left");
      SDL_DestroySurface(a); }
    sv_apply_crt_preset(&s.video, SV_CRT_PIXEL_PERFECT); s.video.scale = SV_SCALE_INTEGER; sv_host_apply_settings(host, &s); }

  /* ---- failure injection: each failure shows the Original picture for that frame only and recovers on the next ---- */
  { const size_t good_capacity = g.ext->capacity;
    run_frame(&g);
    sv_host_extension_stats(host, &acc, &rej, &tex);
    /* 1. an undersized surface: the producer reports Unavailable for that frame */
    g.ext->capacity = 100;
    CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 0 && g.ext->result.status == GENESIS_EXT_UNAVAILABLE && g.ext->result.reason == GENESIS_EXT_REASON_TARGET_TOO_SMALL, "undersized surface: Original for that frame");
    sv_host_widescreen_caps(host, &caps);
    CHECK(caps.extended == SV_WS_UNAVAILABLE && sv_host_widescreen_effective(host) == SV_WIDESCREEN_ORIGINAL, "caps: Unavailable while it fails");
    sv_host_toast(host, "");
    a = sv_host_capture_output(host);
    CHECK(a != NULL && surface_hash(a) == original_hash, "the failed frame is the exact Original picture");
    if (a != NULL) SDL_DestroySurface(a);
    g.ext->capacity = good_capacity;
    CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 39, "recovers on the next frame");
    sv_host_widescreen_caps(host, &caps);
    CHECK(caps.extended == SV_WS_AVAILABLE, "caps: Available again after recovery");
    /* 2. a stale result (sequence mismatch): rejected by the host, the frame is Original */
    { const uint64_t vf = g.r->virtual_frame_count;
      g.host.window_closed(g.host.ctx);
      while (g.r->virtual_frame_count == vf) g.pc = genesis_runtime_retire_m68k_instruction(g.r, 4u, g.pc).next_pc;
      g.ext->result.sequence -= 1; /* the result no longer belongs to the frame being presented */
      CHECK(g.host.present(g.host.ctx, g.frame) == 0 && sv_host_extension_side(host) == 0, "stale sequence: Original for that frame");
      g.presented = g.observer.sequence; }
    CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 39, "next frame is extended again");
    /* 3. corrupt geometry in the result */
    { const uint64_t vf = g.r->virtual_frame_count;
      g.host.window_closed(g.host.ctx);
      while (g.r->virtual_frame_count == vf) g.pc = genesis_runtime_retire_m68k_instruction(g.r, 4u, g.pc).next_pc;
      g.ext->result.pitch = 12;
      CHECK(g.host.present(g.host.ctx, g.frame) == 0 && sv_host_extension_side(host) == 0, "bad pitch in the result: Original for that frame");
      g.presented = g.observer.sequence; }
    CHECK(run_frame(&g) == 0 && sv_host_extension_side(host) == 39, "and recovers");
    sv_host_extension_stats(host, &acc, &rej, &tex);
    CHECK(rej >= 2u, "rejections are counted (bounded)");
  }

  /* ---- Partial: the window plane owns the left columns ---- */
  rig_close(&g);
  if (rig_open(&g, 1, 1) != 0) { printf("FAIL: second rig\n"); return 1; }
  host = genesis_sdl3_viewer_sv_host(g.viewer);
  set_mode(&g, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(run_frame(&g) == 0 && g.ext->result.status == GENESIS_EXT_PARTIAL && (g.ext->result.flags & GENESIS_EXT_FLAG_WINDOW_EDGE) != 0 && sv_host_extension_side(host) == 39, "window over an edge: Partial, still extended");
  sv_host_widescreen_caps(host, &caps);
  CHECK(caps.extended == SV_WS_PARTIAL, "caps: Partial");
  CHECK(strcmp(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &caps), "") != 0, "status text exists for Partial");

  /* ---- guest invariance: toggling the mode never changes guest time, frames, VDP state or the native frame ---- */
  rig_close(&g);
  if (rig_open(&g, 1, 0) != 0) { printf("FAIL: invariance rig\n"); return 1; }
  { /* the reference is a headless runtime with no extension target at all (only one SDL host can exist at a time) */
    GenesisRuntime *plain = (GenesisRuntime *)calloc(1, sizeof *plain);
    GenesisFrameArtifact *plain_frame = (GenesisFrameArtifact *)calloc(1, sizeof *plain_frame);
    GenesisRasterCapture *plain_capture = (GenesisRasterCapture *)calloc(1, sizeof *plain_capture);
    GenesisLiveFrameObserver plain_obs;
    uint32_t plain_pc = 0x200u;
    int frame_no;
    memset(&plain_obs, 0, sizeof plain_obs);
    plain->pc = 0x200u; plain->sr = 0x2000u; plain->a[7] = 0x00FF0000u + 0x8000u;
    build_scene(plain, 1, 0);
    plain_obs.producer = genesis_vdp_produce_frame; plain_obs.latest = plain_frame;
    plain_capture->producer = genesis_vdp_produce_frame_raster;
    plain->live_frame_observer = &plain_obs; plain->live_raster = plain_capture;
    for (frame_no = 0; frame_no < 12; ++frame_no) {
      const uint64_t vf = plain->virtual_frame_count;
      set_mode(&g, (frame_no / 3) % 2 ? SV_WIDESCREEN_EXTENDED_BACKGROUNDS : SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
      CHECK(run_frame(&g) == 0, "toggled frame");
      while (plain->virtual_frame_count == vf) plain_pc = genesis_runtime_retire_m68k_instruction(plain, 4u, plain_pc).next_pc;
      CHECK(memcmp(g.frame, plain_frame, sizeof *plain_frame) == 0, "native artifact (incl. digest) identical to the extension-less run");
    }
    CHECK(g.r->virtual_frame_count == plain->virtual_frame_count && g.r->scheduler.master_ticks == plain->scheduler.master_ticks && g.pc == plain_pc &&
              g.observer.sequence == plain_obs.sequence, "guest frame count, time and pc unchanged by mode switches");
    CHECK(memcmp(&g.r->devices.vdp, &plain->devices.vdp, sizeof plain->devices.vdp) == 0, "VDP hardware state unchanged by mode switches");
    free(plain); free(plain_frame); free(plain_capture);
  }
  rig_close(&g);
  remove(file);
  if (failures != 0) { printf("genesis_viewer_extension_sdl_test: %d failure(s)\n", failures); return 1; }
  printf("genesis_viewer_extension_sdl_test: ok\n");
  return 0;
}
