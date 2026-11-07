/* SEG-131 (ADR 0278): measures the rewind surface of the shared viewer: the time to open it (a 119-moment history, the full minute at the default cadence) and the time of
 * one frame while the selection moves (preview switch), with the SDL dummy video driver and the SOFTWARE renderer at the requested window size. Observed medians and worst
 * cases only; nothing is asserted. A real GPU renderer is faster; this is the pessimistic bound. Not a test, never shipped.
 *   sv_state_bench [WxH] */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "../src/sv_internal.h"

typedef struct Bench { uint8_t thumbs[119][160 * 112 * 2]; } Bench;
static Bench *g_bench;
static double g_times[400];
static int g_count;
static Uint64 g_last, g_first;

static int f_ok(void *ctx, char *why, size_t cap) { (void)ctx; (void)why; (void)cap; return 1; }
static int f_count(void *ctx) { (void)ctx; return 119; }
static int f_info(void *ctx, int i, SvRewindEntry *out) {
  (void)ctx;
  memset(out, 0, sizeof *out);
  out->age_ms = 500 * (119 - i);
  out->thumb.w = 160; out->thumb.h = 112; out->thumb.rgb565 = g_bench->thumbs[i];
  return 0;
}
static int f_restore(void *ctx, int i, char *e, size_t c) { (void)ctx; (void)i; (void)e; (void)c; return 0; }
static int f_slot(void *ctx, int s, SvStateSlotInfo *o) { (void)ctx; (void)s; memset(o, 0, sizeof *o); return 0; }
static int f_sl(void *ctx, int s, char *e, size_t c) { (void)ctx; (void)s; (void)e; (void)c; return 0; }

static int cmp(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : (x > y); }

static int hook(void *ctx, SvHost *host, int iteration) {
  const Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
  SDL_Event e;
  (void)ctx; (void)iteration;
  if (g_first == 0u) g_first = now;
  if (host->rewind == NULL || host->rewind->anim < 1.0f) { g_last = now; return 0; }
  if (g_last != 0u && g_count < 400) g_times[g_count++] = (double)(now - g_last) * 1000.0 / (double)freq;
  g_last = now;
  if (g_count >= 300) return 1;
  SDL_zero(e);
  e.type = SDL_EVENT_KEY_DOWN;
  e.common.timestamp = SDL_GetTicksNS();
  e.key.scancode = (g_count & 63) < 32 ? SDL_SCANCODE_LEFT : SDL_SCANCODE_RIGHT;
  e.key.down = true;
  SDL_PushEvent(&e);
  return 0;
}

int main(int argc, char **argv) {
  SvHostConfig cfg;
  SvHost *host;
  SvStateProvider p;
  SvSample smp;
  uint8_t rgb[320 * 224 * 3];
  int i, w = 1080, h = 740;
  double open_ms;
  Uint64 t0, freq;
  if (argc > 1 && sscanf(argv[1], "%dx%d", &w, &h) != 2) return 2;
  g_bench = (Bench *)calloc(1u, sizeof *g_bench);
  for (i = 0; i < 119; ++i) { size_t k; for (k = 0; k < sizeof g_bench->thumbs[i]; ++k) g_bench->thumbs[i][k] = (uint8_t)(k * 7 + (size_t)i * 13); }
  for (i = 0; i < 320 * 224 * 3; ++i) rgb[i] = (uint8_t)(i * 5);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", "sv-state-bench.cfg", 1);
  { SvSettings s; sv_settings_defaults(&s); s.video.window_w = w; s.video.window_h = h; sv_settings_save(&s, "sv-state-bench.cfg"); }
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "bench"; cfg.picture_w = 320; cfg.picture_h = 224; cfg.max_w = 320; cfg.max_h = 240; cfg.default_scale = 3;
  host = sv_host_open(&cfg);
  if (host == NULL) return 1;
  sv_host_present(host, rgb, 320, 224, 320 * 3);
  memset(&p, 0, sizeof p);
  p.states_available = f_ok; p.slot_info = f_slot; p.save_slot = f_sl; p.load_slot = f_sl;
  p.rewind_available = f_ok; p.history_count = f_count; p.history_info = f_info; p.restore_entry = f_restore;
  sv_host_set_state_provider(host, &p);
  sv_host_set_loop_hook(host, hook, NULL);
  freq = SDL_GetPerformanceFrequency();
  {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN; e.common.timestamp = SDL_GetTicksNS(); e.key.scancode = SDL_SCANCODE_F7; e.key.down = true;
    SDL_PushEvent(&e);
  }
  t0 = SDL_GetPerformanceCounter();
  sv_host_service(host, &smp);
  open_ms = (double)(g_first - t0) * 1000.0 / (double)freq;
  qsort(g_times, (size_t)g_count, sizeof g_times[0], cmp);
  printf("window %dx%d: open (history of 119 listed, first surface frame drawn) %.2f ms; frame while switching the preview: median %.2f ms, worst %.2f ms over %d frames\n", w, h, open_ms,
         g_count ? g_times[g_count / 2] : 0.0, g_count ? g_times[g_count - 1] : 0.0, g_count);
  sv_host_close(host);
  free(g_bench);
  remove("sv-state-bench.cfg");
  return 0;
}
