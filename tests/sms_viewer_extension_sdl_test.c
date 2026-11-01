/* SEG-120 (ADR 0263) / SEG-121 (ADR 0264): the real SDL3 Master System viewer adapter showing the Level 2 background extension and the Level 3 edge-sprite spill, read back from the rendered output.
 *
 * Project-authored synthetic scene (a name table of solid-colour tiles, one distinct colour/palette per column, poked straight into the VDP of a real
 * SmsViewerRig whose CPU is a fake that only retires cycles). The real scanline hook, renderer, adapter request sampling, present callback, capability
 * report and the shared SvHost presenter run unmodified (dummy video driver, software renderer). The expected colour of every probed pixel comes from
 * the tile arithmetic of the scene, not from the renderer. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_viewer_sdl3.h"
#include "sv_host.h"
#include "sv_layout.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

Z80Outcome z80_run(Z80Runtime *rt, uint64_t deadline) {
  rt->state.deadline = deadline;
  while (rt->state.cycles < rt->state.deadline && rt->outcome == Z80_OUTCOME_NONE) rt->state.cycles += 4;
  return rt->outcome;
}

static SmsViewerRig rig;
static uint8_t rom[32768];

static const char kNote[] = "Background repeats existing scenery; this does not reveal more of the game world.";
static const char kSpriteNote[] = "Background repeats existing scenery. Only sprites crossing the screen edges can extend; this does not expand the game world.";

/* 0: the scene has no sprites; 1: it has two edge sprites that exist in the native picture only; 2: ... and they also spill beside it (a Level 3 frame).
 * The shift bit (x - 8) and zoom (16 x 16 pixels) are on. Sprite A: X = 255 (columns 247 .. 262), solid colour 15; sprite B: X = 4 (columns -4 .. 11),
 * solid colour 13; both on lines 91..106. */
static int scene_sprites;

/* column c shows pattern 1 + c % 15 (a solid colour index) in palette (c / 15) & 1: 30 distinct (palette, colour) pairs over the 32 columns */
static void build_scene(int tall, int hscroll) {
  SmsVdp *v = &rig.vdp;
  unsigned k, row, c, i;
  memset(v->vram, 0, SMS_VDP_VRAM_SIZE);
  for (k = 1; k <= 15; ++k)
    for (row = 0; row < 8; ++row)
      for (i = 0; i < 4; ++i) v->vram[k * 32 + row * 4 + i] = (uint8_t)(((k >> i) & 1u) ? 0xFF : 0x00);
  {
    const unsigned name_base = tall ? 0x3700u : 0x3800u;
    for (row = 0; row < (tall ? 28u : 24u); ++row)
      for (c = 0; c < 32; ++c) {
        const unsigned entry = (1u + c % 15u) | (((c / 15u) & 1u) << 11);
        v->vram[name_base + row * 64 + c * 2] = (uint8_t)entry;
        v->vram[name_base + row * 64 + c * 2 + 1] = (uint8_t)(entry >> 8);
      }
  }
  for (i = 0; i < 64; ++i) v->vram[0x3F00 + i] = 0xD0; /* no sprites */
  if (scene_sprites && !tall) {
    for (i = 0; i < 4; ++i) { v->vram[16 * 32 + i * 4 + 0] = 0xFF; }
    for (row = 0; row < 8; ++row) for (i = 0; i < 4; ++i) { v->vram[16 * 32 + row * 4 + i] = 0xFF; v->vram[17 * 32 + row * 4 + i] = (uint8_t)((13u >> i) & 1u ? 0xFF : 0x00); }
    v->vram[0x3F00] = 90; v->vram[0x3F80] = 255; v->vram[0x3F81] = 16;
    v->vram[0x3F01] = 90; v->vram[0x3F82] = 4;   v->vram[0x3F83] = 17;
  }
  for (i = 0; i < 32; ++i) v->cram[i] = (uint8_t)((i * 7u + 5u) & 63u);
  v->reg[0] = (uint8_t)((tall ? 0x06 : 0x04) | (scene_sprites ? 0x08 : 0));
  v->reg[1] = (uint8_t)((tall ? 0x50 : 0x40) | ((scene_sprites && !tall) ? 0x01 : 0));
  v->reg[2] = (uint8_t)(tall ? 0xFF : 0xFF);
  v->reg[5] = 0xFF;
  v->reg[7] = 0;
  v->reg[8] = (uint8_t)hscroll;
  v->reg[9] = 0;
}

static void expected_rgb(int x, int hscroll, uint8_t out[3]) {
  const int fine = hscroll & 7;
  if (scene_sprites && ((x >= 247 && x < 263) || (x >= -4 && x < 12)) && (scene_sprites == 2 || (x >= 0 && x < 256))) {
    const unsigned c = rig.vdp.cram[16u + ((x >= 247) ? 15u : 13u)] & 63u;
    out[0] = (uint8_t)((c & 3u) * 85u); out[1] = (uint8_t)(((c >> 2) & 3u) * 85u); out[2] = (uint8_t)(((c >> 4) & 3u) * 85u);
    return;
  }
  const int xm = ((x % 256) + 256) % 256;
  const int src = (((x - hscroll) % 256) + 256) % 256;
  const unsigned col = (unsigned)(src / 8);
  unsigned cram;
  if (xm < fine) cram = rig.vdp.cram[16] & 63u; /* the fine-scroll gap is the backdrop */
  else cram = rig.vdp.cram[(((col / 15u) & 1u) * 16u) + 1u + col % 15u] & 63u;
  out[0] = (uint8_t)((cram & 3u) * 85u);
  out[1] = (uint8_t)(((cram >> 2) & 3u) * 85u);
  out[2] = (uint8_t)(((cram >> 4) & 3u) * 85u);
}

static SmsSdl3Viewer *viewer;
static SmsViewerHost host;
static SmsPacer pacer;

static void step(unsigned n) {
  unsigned i;
  for (i = 0; i < n; ++i) {
    const SmsViewerResult r = sms_viewer_run(&rig, &host, &pacer, 1);
    CHECK(r.outcome == SMS_VIEWER_FRAME_LIMIT, "one guest frame");
  }
}

static void set_mode(SvWidescreenMode m, SvAspect aspect, SvScaleMode scale) {
  SvHost *h = sms_sdl3_viewer_sv_host(viewer);
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = m;
  s.video.aspect = aspect;
  s.video.scale = scale;
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
  for (y = 0; y < s->h; ++y)
    for (x = 0; x < s->w; ++x) {
      pixel(s, x, y, p);
      h = (h ^ (uint64_t)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16))) * UINT64_C(1099511628211);
    }
  return h;
}
static uint64_t capture_hash(void) {
  SvHost *h = sms_sdl3_viewer_sv_host(viewer);
  SDL_Surface *a;
  uint64_t v = 0;
  sv_host_toast(h, "");
  a = sv_host_capture_output(h);
  if (a != NULL) { v = surface_hash(a); SDL_DestroySurface(a); }
  return v;
}

/* the native centre equals the scene, and (when side > 0) the scenery on both sides equals the arithmetic of the wrapping name table */
static void check_output(const char *name, int height, int side, int hscroll, SvAspect aspect, SvScaleMode scale) {
  SvHost *h = sms_sdl3_viewer_sv_host(viewer);
  SvExtendedLayout lay;
  SDL_Surface *a;
  int x, native_ok = 1, scenery_ok = 1, step_x;
  char what[160];
  sv_host_toast(h, "");
  a = sv_host_capture_output(h);
  snprintf(what, sizeof what, "%s: output captured", name);
  CHECK(a != NULL, what);
  if (a == NULL) return;
  CHECK(sv_layout_extended_dest(a->w, a->h, 256, height, side, aspect, scale, &lay) == 0, "layout");
  step_x = 1;
  for (x = -side; x < 256 + side; x += step_x) {
    uint8_t got[3], want[3];
    const int sx = lay.native.x + (int)(((double)x + 0.5) * (double)lay.native.w / 256.0);
    const int sy = lay.surface.y + lay.surface.h / 2;
    uint8_t prev[3], next[3];
    expected_rgb(x, hscroll, want);
    expected_rgb(x - 1, hscroll, prev);
    expected_rgb(x + 1, hscroll, next);
    if (memcmp(want, prev, 3) != 0 || memcmp(want, next, 3) != 0) continue; /* a colour edge: a non-integer scale may round the edge by one screen pixel */
    if (!pixel(a, sx, sy, got) || memcmp(got, want, 3) != 0) { if (x < 0 || x >= 256) scenery_ok = 0; else native_ok = 0; }
  }
  snprintf(what, sizeof what, "%s: native centre matches the scene", name);
  CHECK(native_ok, what);
  snprintf(what, sizeof what, "%s: wrapped scenery on both sides matches the wrapping name table", name);
  CHECK(scenery_ok, what);
  SDL_DestroySurface(a);
}

static void ext_state(const char *name, int expect_side) {
  SvHost *h = sms_sdl3_viewer_sv_host(viewer);
  char what[160];
  snprintf(what, sizeof what, "%s: extension side %d shown", name, expect_side);
  CHECK(sv_host_extension_side(h) == expect_side, what);
}

typedef struct Digest {
  uint8_t machine[32];
  uint64_t frames, trace;
  uint32_t records;
  uint8_t shas[64][32];
  char script[2048];
  size_t script_len;
} Digest;

static void take_digest(Digest *d) {
  uint32_t i;
  memset(d, 0, sizeof *d);
  sms_machine_digest(&rig.machine, d->machine);
  d->frames = rig.renderer.frames_completed;
  d->trace = rig.vdp.trace_count;
  d->records = rig.renderer.record_count;
  for (i = 0; i < d->records && i < 64u; ++i) memcpy(d->shas[i], rig.records[i].sha256, 32);
  d->script_len = sms_viewer_format_script(&rig, d->script, sizeof d->script);
}

static void run_schedule(int toggling, Digest *d) {
  unsigned f;
  SmsError e;
  CHECK(sms_viewer_rig_init(&rig, rom, sizeof rom, SMS_MAPPER_ROM_ONLY, &e), "rig init");
  build_scene(0, 5);
  /* a sprite pair that collides and an overflowing row, so the status flags are part of the digest */
  { unsigned i; SmsVdp *v = &rig.vdp;
    for (i = 0; i < 10; ++i) { v->vram[0x3F00 + i] = 40; v->vram[0x3F80 + 2 * i] = (uint8_t)(100 + i); v->vram[0x3F80 + 2 * i + 1] = 3; }
    v->vram[0x3F00 + 10] = 80; v->vram[0x3F80 + 20] = 255; v->vram[0x3F80 + 21] = 3;
    v->vram[0x3F00 + 11] = 80; v->vram[0x3F80 + 22] = 250; v->vram[0x3F80 + 23] = 4;
    v->vram[0x3F00 + 12] = 0xD0; }
  for (f = 0; f < 40; ++f) {
    SvWidescreenMode m = SV_WIDESCREEN_ORIGINAL;
    if (toggling) m = (f / 5) % 3 == 0 ? SV_WIDESCREEN_ORIGINAL : ((f / 5) % 3 == 1 ? SV_WIDESCREEN_EXTENDED_BACKGROUNDS : SV_WIDESCREEN_ENHANCED_GAMEPLAY);
    set_mode(m, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
    if (f == 11) rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] | 0x08); /* the shift bit: an X = 4 sprite would spill left */
    if (f == 17) rig.vdp.reg[8] = 77; /* identical guest-side scene changes in both runs */
    if (f == 23) rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] | 0x20); /* mask: the extension is unavailable for a while */
    if (f == 29) rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] & ~0x20);
    step(1);
  }
  take_digest(d);
}

int main(void) {
  SvHost *host_h;
  SvWidescreenCaps caps;
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : "/tmp";
  char file[600];
  uint32_t acc, rej, tex, tex2;
  uint64_t original_hash, masked_hash;
  Digest control, toggled;
  SmsExtension *ext;
  SmsError e;
  snprintf(file, sizeof file, "%s/sms_ext_sdl_%d.cfg", tmp, (int)SDL_GetTicks());
  remove(file);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", file, 1);

  CHECK(sms_viewer_rig_init(&rig, rom, sizeof rom, SMS_MAPPER_ROM_ONLY, &e), "rig init");
  viewer = sms_sdl3_viewer_open("sms extension test", 3, 0);
  if (viewer == NULL) { printf("FAIL: the viewer opens\n"); return 1; }
  host_h = sms_sdl3_viewer_sv_host(viewer);
  CHECK(sms_sdl3_viewer_attach_extension(viewer, &rig) == 0, "extension attaches");
  ext = sms_sdl3_viewer_extension(viewer);
  CHECK(ext != NULL && rig.renderer.ext == ext, "the renderer owns the adapter's target");
  sms_sdl3_viewer_host(viewer, &host);
  sms_pacer_init(&pacer, 1);
  build_scene(0, 0);

  sv_host_widescreen_caps(host_h, &caps);
  CHECK(caps.extended == SV_WS_PARTIAL && caps.enhanced == SV_WS_PARTIAL, "caps: Level 2 and Level 3 at best Partial");
  CHECK(caps.partial_note != NULL && strcmp(caps.partial_note, kNote) == 0, "caps carry the repetition wording");
  CHECK(caps.enhanced_note != NULL && strcmp(caps.enhanced_note, kSpriteNote) == 0, "caps carry the edge-sprite wording for Level 3");

  /* ---- Original: nothing is produced ---- */
  set_mode(SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  step(3);
  CHECK(ext->request_side_192 == 0 && ext->request_side_224 == 0 && ext->result.cls == SMS_EXT_NONE && sv_host_extension_side(host_h) == 0,
        "Original: no extension work and none shown");
  check_output("Original", 192, 0, 0, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  original_hash = capture_hash();

  /* ---- Level 2 (live): wrapped scenery on both sides, exactly centred ---- */
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  step(2);
  ext_state("Level 2", 42);
  CHECK(ext->result.cls == SMS_EXT_WRAPPED && ext->result.sequence == rig.renderer.frames_completed, "Level 2: WRAPPED, stamped with the presented frame");
  check_output("Level 2 / 192 lines", 192, 42, 0, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  sv_host_widescreen_caps(host_h, &caps);
  CHECK(caps.extended == SV_WS_PARTIAL, "caps: WRAPPED is Partial, never Available");
  CHECK(strcmp(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &caps), kNote) == 0, "menu status: the extension repeats existing scenery");
  CHECK(sv_host_widescreen_effective(host_h) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "effective mode is Level 2");
  /* the same wrapping with a scrolled, fine-offset background */
  build_scene(0, 21);
  step(2);
  check_output("Level 2 / hscroll 21", 192, 42, 21, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  build_scene(0, 0);

  /* ---- Level 3: the sprites crossing the screen edges spill over the wrapped background; Partial, wording says what it is ---- */
  scene_sprites = 1;
  build_scene(0, 0);
  step(2);
  CHECK(ext->result.sprites == 0 && ext->result.sprite_pixels == 0, "Level 2 frame carries no sprite spill");
  check_output("Level 2 with edge sprites in the scene", 192, 42, 0, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  set_mode(SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  scene_sprites = 2;
  step(2);
  CHECK(ext->request_sprites == 1 && ext->result.cls == SMS_EXT_WRAPPED && ext->result.sprites == 1, "Level 3: a WRAPPED frame carrying the sprite spill");
  CHECK(ext->result.sprite_pixels == 16u * (4u + 7u), "Level 3: 4 columns left and 7 columns right of the picture, 16 lines each");
  ext_state("Level 3", 42);
  check_output("Level 3 / edge sprites", 192, 42, 0, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  sv_host_widescreen_caps(host_h, &caps);
  CHECK(caps.enhanced == SV_WS_PARTIAL && sv_host_widescreen_effective(host_h) == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "caps: Level 3 Partial and effective");
  CHECK(strcmp(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &caps), kSpriteNote) == 0, "menu status: edge-sprites wording");
  CHECK(sv_host_settings(host_h)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "the Level 3 preference is kept");
  /* a pending Level 3 frame is refused once the user steps down to Level 2, and the next frame has no sprite pixel beside the picture */
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  CHECK(host.present(host.ctx, rig.renderer.last, rig.renderer.last_height) == 0 && sv_host_extension_side(host_h) == 0, "a pending Level 3 frame is not shown at Level 2");
  scene_sprites = 1;
  step(2);
  CHECK(ext->result.sprites == 0 && sv_host_extension_side(host_h) == 42, "Level 2 again: background only");
  check_output("Level 2 after Level 3 (no stale sprite pixels)", 192, 42, 0, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  /* Level 3 over an unavailable background (left-column mask) shows the Original picture and reports both levels unavailable, preference kept */
  set_mode(SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] | 0x20);
  step(2);
  sv_host_widescreen_caps(host_h, &caps);
  CHECK(ext->result.cls == SMS_EXT_UNAVAILABLE && ext->result.sprites == 0 && sv_host_extension_side(host_h) == 0 && caps.extended == SV_WS_UNAVAILABLE &&
            caps.enhanced == SV_WS_UNAVAILABLE && sv_host_widescreen_effective(host_h) == SV_WIDESCREEN_ORIGINAL,
        "Level 3 without a Level 2 background: Original, nothing invented for the sprites");
  rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] & ~0x20);
  scene_sprites = 2;
  step(2);
  CHECK(ext->result.sprites == 1 && sv_host_extension_side(host_h) == 42, "Level 3 recovers");
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  scene_sprites = 0;
  build_scene(0, 0);
  step(2);

  /* ---- integer scaling, 4:3 pixel shape, CRT preset, fullscreen ---- */
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  step(2);
  ext_state("integer scaling", 42);
  check_output("Level 2 / integer", 192, 42, 0, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_4_3, SV_SCALE_FIT);
  step(2);
  ext_state("4:3 aspect", 42);
  { SvSettings s = *sv_host_settings(host_h);
    sv_apply_crt_preset(&s.video, SV_CRT_CLASSIC); s.video.aspect = SV_ASPECT_ORIGINAL; s.video.scale = SV_SCALE_FIT; sv_host_apply_settings(host_h, &s);
    step(2);
    ext_state("CRT preset", 42);
    s.video.fullscreen = 1; sv_host_apply_settings(host_h, &s);
    step(2);
    ext_state("fullscreen", 42);
    s.video.fullscreen = 0; sv_apply_crt_preset(&s.video, SV_CRT_PIXEL_PERFECT); sv_host_apply_settings(host_h, &s); }
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);

  /* ---- stable geometry: no texture (re)creation, no churn ---- */
  step(2);
  sv_host_extension_stats(host_h, &acc, &rej, &tex);
  step(10);
  sv_host_extension_stats(host_h, &acc, &rej, &tex2);
  CHECK(tex2 == tex, "no texture allocation churn at stable geometry");

  /* ---- 224-line mode: its own, larger side ---- */
  build_scene(1, 0);
  step(2);
  ext_state("224 lines", 71);
  CHECK(ext->result.cls == SMS_EXT_WRAPPED && ext->result.height == 224 && ext->result.side == 71, "224 lines: WRAPPED, side 71");
  check_output("Level 2 / 224 lines", 224, 71, 0, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  build_scene(0, 0);
  step(2);
  ext_state("back to 192 lines", 42);

  /* ---- unavailable: the left-column mask has no defined off-screen continuation ---- */
  rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] | 0x20);
  step(2);
  CHECK(ext->result.cls == SMS_EXT_UNAVAILABLE && ext->result.reason == SMS_EXT_REASON_LEFT_MASK, "mask: typed UNAVAILABLE");
  ext_state("mask", 0);
  sv_host_widescreen_caps(host_h, &caps);
  CHECK(caps.extended == SV_WS_UNAVAILABLE && sv_host_widescreen_effective(host_h) == SV_WIDESCREEN_ORIGINAL, "caps: Unavailable, effective Original");
  CHECK(strstr(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &caps), "original picture") != NULL, "status says the original picture is shown");
  { const uint64_t masked_l2 = capture_hash();
    set_mode(SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
    step(2);
    masked_hash = capture_hash();
    CHECK(masked_l2 == masked_hash, "an unavailable frame is exactly the Original picture (no stale extension pixels)"); }
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] & ~0x20);
  step(2);
  ext_state("recovery after the mask", 42);
  sv_host_widescreen_caps(host_h, &caps);
  CHECK(caps.extended == SV_WS_PARTIAL, "caps: Partial again after recovery");
  { rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] | 0x80);
    step(2);
    CHECK(ext->result.cls == SMS_EXT_UNAVAILABLE && ext->result.reason == SMS_EXT_REASON_VSCROLL_LOCK && sv_host_extension_side(host_h) == 0, "vertical scroll lock: typed UNAVAILABLE, Original shown");
    rig.vdp.reg[0] = (uint8_t)(rig.vdp.reg[0] & ~0x80);
    step(2);
    ext_state("recovery after the lock", 42); }

  /* ---- failure injection: each shows the Original picture for that frame only and recovers on the next ---- */
  step(2);
  sv_host_extension_stats(host_h, &acc, &rej, &tex);
  { const uint64_t seq = ext->result.sequence;
    ext->result.sequence = seq - 1; /* the result no longer belongs to the frame being presented */
    CHECK(host.present(host.ctx, rig.renderer.last, rig.renderer.last_height) == 0 && sv_host_extension_side(host_h) == 0, "stale sequence: Original for that frame");
    ext->result.sequence = seq;
    ext->result.side = SMS_EXT_MAX_SIDE + 1u;
    CHECK(host.present(host.ctx, rig.renderer.last, rig.renderer.last_height) == 0 && sv_host_extension_side(host_h) == 0, "oversized side in the result: Original");
    ext->result.side = 42;
    ext->result.height = 224;
    CHECK(host.present(host.ctx, rig.renderer.last, rig.renderer.last_height) == 0 && sv_host_extension_side(host_h) == 0, "height mismatch in the result: Original");
    ext->result.height = 192;
    ext->result.cls = SMS_EXT_UNAVAILABLE;
    CHECK(host.present(host.ctx, rig.renderer.last, rig.renderer.last_height) == 0 && sv_host_extension_side(host_h) == 0, "unavailable class: Original");
    ext->result.cls = SMS_EXT_NONE;
    CHECK(host.present(host.ctx, rig.renderer.last, rig.renderer.last_height) == 0 && sv_host_extension_side(host_h) == 0, "none class: Original"); }
  step(2);
  ext_state("recovery after failures", 42);
  sv_host_extension_stats(host_h, &acc, &rej, &tex);
  CHECK(rej >= 1u, "a stale result is rejected by the shared check and counted (bounded)");

  /* ---- back to Original: the output equals the first Original capture byte for byte ---- */
  set_mode(SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  step(2);
  CHECK(sv_host_extension_side(host_h) == 0 && capture_hash() == original_hash, "Original after Level 2 is byte-identical to Original before");

  /* ---- the unsupported 240-line mode keeps its typed behaviour: the machine stops, nothing is rendered or extended (after everything else) ---- */
  CHECK(sms_viewer_rig_init(&rig, rom, sizeof rom, SMS_MAPPER_ROM_ONLY, &e), "rig init");
  build_scene(0, 0);
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  step(3);
  { const uint64_t completed = rig.renderer.frames_completed;
    const SmsExtResult before = ext->result;
    SmsViewerResult r;
    rig.vdp.reg[0] = 0x06; rig.vdp.reg[1] = 0x48 | 0x40;
    r = sms_viewer_run(&rig, &host, &pacer, 3);
    CHECK(r.outcome == SMS_VIEWER_GUEST_ERROR && r.stop.sms_error == SMS_ERROR_VDP_MODE_UNSUPPORTED, "240 lines: the existing typed unsupported stop");
    CHECK(rig.renderer.frames_completed == completed && memcmp(&ext->result, &before, sizeof before) == 0, "240 lines: nothing rendered, extension untouched"); }

  /* ---- guest invariance: toggling the mode never changes guest time, frames, VDP trace, input recording or the native frame records ---- */
  run_schedule(0, &control);
  run_schedule(1, &toggled);
  CHECK(memcmp(control.machine, toggled.machine, 32) == 0, "machine digest (guest state incl. VDP/PSG) identical to the control run");
  CHECK(control.frames == toggled.frames && control.trace == toggled.trace && control.records == toggled.records && control.frames == 40u,
        "guest frame count, VDP trace count and record count identical");
  CHECK(memcmp(control.shas, toggled.shas, sizeof control.shas) == 0, "native frame SHA-256 records identical for every frame");
  CHECK(control.script_len == toggled.script_len && memcmp(control.script, toggled.script, control.script_len) == 0, "input recording identical");

  sms_sdl3_viewer_close(viewer);

  /* ---- a viewer that never attached an extension (an older build) stays Original and reports Unavailable ---- */
  CHECK(sms_viewer_rig_init(&rig, rom, sizeof rom, SMS_MAPPER_ROM_ONLY, &e), "rig init");
  rig.extension = NULL;
  viewer = sms_sdl3_viewer_open("sms no extension", 3, 0);
  if (viewer == NULL) { printf("FAIL: second viewer\n"); return 1; }
  host_h = sms_sdl3_viewer_sv_host(viewer);
  sms_sdl3_viewer_host(viewer, &host);
  sms_pacer_init(&pacer, 1);
  build_scene(0, 0);
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  step(3);
  sv_host_widescreen_caps(host_h, &caps);
  CHECK(caps.extended == SV_WS_UNAVAILABLE && sv_host_extension_side(host_h) == 0 && sv_host_widescreen_effective(host_h) == SV_WIDESCREEN_ORIGINAL,
        "no extension linked: Unavailable, Original shown with Level 2 requested");
  sms_sdl3_viewer_close(viewer);
  remove(file);
  if (failures != 0) { printf("sms_viewer_extension_sdl_test: %d failure(s)\n", failures); return 1; }
  printf("sms_viewer_extension_sdl_test: ok\n");
  return 0;
}
