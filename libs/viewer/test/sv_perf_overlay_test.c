/* SEG-125 (ADR 0273): the optional performance overlay of the shared SDL3 host, read back from the rendered window output (dummy video driver, software renderer; no
 * display, audio device or ROM). Properties: with a collector attached but the overlay off the output is bit-identical to a host with no collector (and the collector stays idle:
 * no clock read); the overlay is a small top-left box (the rest of the picture is untouched); switching it on/off needs no restart (menu setting or F3); the host times its present into
 * the collector; "developer" shows more than "player"; a pause is excluded from frame intervals. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_perf.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

enum { W = 320, H = 224 };
static uint64_t g_ns = 1000000, g_reads;
static uint64_t fake_clock(void) { ++g_reads; g_ns += 250000; return g_ns; } /* every read advances 0.25 ms so spans have a duration */

static void make_native(uint8_t *rgb) {
  int x, y;
  for (y = 0; y < H; ++y)
    for (x = 0; x < W; ++x) { uint8_t *p = rgb + ((size_t)y * W + (size_t)x) * 3u; p[0] = (uint8_t)(30 + x % 100); p[1] = (uint8_t)(40 + y % 90); p[2] = (uint8_t)(60 + (x + y) % 80); }
}
static SvHost *open_host(const char *settings_file) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "perf overlay test"; cfg.picture_w = W; cfg.picture_h = H; cfg.max_w = W; cfg.max_h = 240; cfg.default_scale = 3;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings_file, 1);
  return sv_host_open(&cfg);
}
static uint64_t hash_rect(SDL_Surface *s, int x0, int y0, int w, int h) {
  uint64_t hsh = 1469598103934665603ull;
  int x, y;
  for (y = y0; y < y0 + h; ++y)
    for (x = x0; x < x0 + w; ++x) { Uint8 r = 0, g = 0, b = 0, a = 0; SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a); hsh = (hsh ^ (uint64_t)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16))) * 1099511628211ull; }
  return hsh;
}
static int changed_px(SDL_Surface *a, SDL_Surface *b) {
  int x, y, n = 0;
  for (y = 0; y < a->h; ++y)
    for (x = 0; x < a->w; ++x) {
      Uint8 r1, g1, b1, a1, r2, g2, b2, a2;
      SDL_ReadSurfacePixel(a, x, y, &r1, &g1, &b1, &a1); SDL_ReadSurfacePixel(b, x, y, &r2, &g2, &b2, &a2);
      if (r1 != r2 || g1 != g2 || b1 != b2) ++n;
    }
  return n;
}
static SDL_Surface *shoot(SvHost *h) { sv_host_toast(h, ""); return sv_host_capture_output(h); }
static void set_overlay(SvHost *h, int mode) { SvSettings s = *sv_host_settings(h); s.video.perf_overlay = mode; s.video.show_fps = 0; sv_host_apply_settings(h, &s); }
static void service(SvHost *h) { SvSample sample; sv_host_service(h, &sample); }
static void feed(SvPerf *p, int frames) {
  int i;
  for (i = 0; i < frames; ++i) {
    uint64_t t = sv_perf_span_begin(p);
    sv_perf_span_end(p, SV_SPAN_EMULATION, t);
    g_ns += (i % 11 == 10) ? 30000000u : 16000000u; /* one slow frame in eleven */
    sv_perf_frames(p, (uint64_t)i + 1u, 1, i % 11 == 10);
  }
}

int main(void) {
  char dir[512], settings_file[600];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  uint8_t *native = (uint8_t *)malloc((size_t)W * H * 3u);
  static SvPerf perf;
  SvHost *host;
  SDL_Surface *plain, *off, *player, *dev, *after_off;
  snprintf(dir, sizeof dir, "%s/sv_perf_overlay_test_%d", tmp, (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(settings_file, sizeof settings_file, "%s/viewer-settings.cfg", dir);
  remove(settings_file);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  host = open_host(settings_file);
  CHECK(host != NULL && native != NULL, "setup");
  if (host == NULL || native == NULL) return 1;
  make_native(native);
  SDL_SetWindowSize(host->window, 1280, 720);

  /* 1. no collector at all: the reference picture */
  CHECK(sv_host_present(host, native, W, H, W * 3) == 0, "present without a collector");
  plain = shoot(host);

  /* 2. collector attached, overlay off, collector idle: bit-identical output, not a single clock read, nothing offered in the menu yet is irrelevant here */
  sv_perf_init(&perf, fake_clock, 16683333U, 0);
  sv_host_set_perf(host, &perf);
  g_reads = 0;
  CHECK(sv_host_present(host, native, W, H, W * 3) == 0, "present with an idle collector");
  service(host);
  off = shoot(host);
  CHECK(plain != NULL && off != NULL && changed_px(plain, off) == 0, "an attached but idle collector leaves the output bit-identical");
  CHECK(!perf.active && g_reads == 0 && perf.frames_presented == 0, "the idle collector does not read the clock or count");

  /* 3. overlay on (player): the collector activates at the next service, the box appears top-left, the rest of the picture is untouched */
  set_overlay(host, 1);
  service(host);
  CHECK(perf.active, "switching the overlay on activates the collector without a restart");
  feed(&perf, 140);
  CHECK(sv_host_present(host, native, W, H, W * 3) == 0, "present with the overlay");
  CHECK(perf.span[SV_SPAN_PRESENT].count == 1 && perf.span[SV_SPAN_TEXTURE_UPLOAD].count >= 1 && perf.frames_presented == 1, "the host times its present and texture upload");
  CHECK(perf.span[SV_SPAN_TEXTURE_UPLOAD].total_ns <= perf.span[SV_SPAN_PRESENT].total_ns, "the inclusive upload span is contained in the present span");
  player = shoot(host);
  CHECK(player != NULL && changed_px(plain, player) > 200, "the overlay draws something");
  CHECK(hash_rect(plain, plain->w / 2, plain->h / 2, plain->w / 2 - 1, plain->h / 2 - 1) == hash_rect(player, plain->w / 2, plain->h / 2, plain->w / 2 - 1, plain->h / 2 - 1),
        "the lower-right of the picture is untouched");
  CHECK(changed_px(plain, player) < plain->w * plain->h / 5, "the overlay covers only a small corner");

  /* 4. developer view shows more than the player view */
  set_overlay(host, 2);
  service(host);
  dev = shoot(host);
  CHECK(dev != NULL && changed_px(plain, dev) > changed_px(plain, player), "the developer view is larger than the player view");

  /* 5. off again: output identical to the reference, collector idle again (no restart, no rebuild) */
  set_overlay(host, 0);
  service(host);
  after_off = shoot(host);
  CHECK(!perf.active && after_off != NULL && changed_px(plain, after_off) == 0, "switching the overlay off restores the exact picture and idles the collector");

  /* 6. F3 cycles off -> player -> developer -> off from the keyboard */
  {
    SDL_Event ev;
    int expect[3] = {1, 2, 0}, i;
    for (i = 0; i < 3; ++i) {
      memset(&ev, 0, sizeof ev);
      ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = SDL_SCANCODE_F3; ev.key.key = SDLK_F3; ev.key.down = true; ev.key.repeat = false;
      SDL_PushEvent(&ev);
      service(host);
      CHECK(sv_host_settings(host)->video.perf_overlay == expect[i], "F3 cycles the overlay");
    }
  }

  /* 7. a requested report keeps the collector active whatever the overlay says */
  sv_perf_enable(&perf);
  service(host);
  CHECK(perf.active && perf.pinned, "a report request pins the collector");
  set_overlay(host, 0);
  service(host);
  CHECK(perf.active, "...even with the overlay off");

  /* 8. a pause is not a slow frame */
  {
    const uint64_t before = perf.interval.max_ns;
    const uint64_t t = sv_perf_span_begin(&perf);
    g_ns += 60000000000ULL;
    sv_perf_resync(&perf, t);
    g_ns += 16000000U;
    sv_perf_frames(&perf, 9999, 1, 0);
    CHECK(perf.interval.max_ns <= before + 1000000ULL && perf.paused_ns >= 60000000000ULL, "the pause is excluded from the frame interval");
  }
  SDL_DestroySurface(plain); SDL_DestroySurface(off); SDL_DestroySurface(player); SDL_DestroySurface(dev); SDL_DestroySurface(after_off);
  sv_host_close(host);
  free(native);
  if (failures == 0) puts("sv_perf_overlay_test: ok");
  return failures == 0 ? 0 : 1;
}
