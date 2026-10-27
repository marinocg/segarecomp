/* SEG-078-T001 (ADR 0204): see scd_viewer.h. */
#include "scd_viewer.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_live.h"
#include "viewer.h"
#include "viewer_sdl3.h"

#define KEY_SCRIPT_MAX 4096U

/* Automation seam (like the launcher's SEGARECOMP_GUI_SCRIPT): SEGARECOMP_VIEWER_KEY_SCRIPT names a file of `<poll> down|up <SDL key name>` (or `<poll> quit x`)
 * lines; at the start of that poll of the driver the key event is pushed onto SDL's queue, so the REAL keyboard mapping, settings and
 * pause handling are exercised without a person. Unset in every shipped use. */
typedef struct KeyLine {
  uint64_t poll;
  SDL_Scancode scancode;
  int down, quit;   /* quit: the window's close button (SDL_EVENT_QUIT) */
} KeyLine;

typedef struct ScdSdlHost {
  KeyLine keys[KEY_SCRIPT_MAX];
  size_t key_count, key_next;
  uint64_t polls;
  GenesisSdl3Viewer *sdl;
  GenesisViewerHost host;
  GenesisPacer pacer;
  int opened, failed;
  int paused;
} ScdSdlHost;

static ScdSdlHost g_host;

static void load_key_script(ScdSdlHost *h) {
  const char *path = getenv("SEGARECOMP_VIEWER_KEY_SCRIPT");
  FILE *f = path != NULL ? fopen(path, "r") : NULL;
  char text[128];
  if (f == NULL) return;
  while (h->key_count < KEY_SCRIPT_MAX && fgets(text, sizeof text, f) != NULL) {
    unsigned long long poll = 0U;
    char action[8] = "", name[64] = "";
    KeyLine *k = &h->keys[h->key_count];
    if (sscanf(text, "%llu %7s %63s", &poll, action, name) != 3) continue;
    k->poll = (uint64_t)poll;
    k->down = strcmp(action, "down") == 0;
    k->quit = strcmp(action, "quit") == 0;
    k->scancode = SDL_GetScancodeFromName(name);
    if (!k->quit && (k->scancode == SDL_SCANCODE_UNKNOWN || (!k->down && strcmp(action, "up") != 0))) continue;
    h->key_count += 1U;
  }
  fclose(f);
}

static void push_scripted_keys(ScdSdlHost *h) {
  while (h->key_next < h->key_count && h->keys[h->key_next].poll <= h->polls) {
    SDL_Event e;
    SDL_zero(e);
    e.type = (Uint32)(h->keys[h->key_next].quit ? SDL_EVENT_QUIT : h->keys[h->key_next].down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP);
    e.common.timestamp = SDL_GetTicksNS();
    e.key.scancode = h->keys[h->key_next].scancode;
    e.key.down = h->keys[h->key_next].down != 0;
    SDL_PushEvent(&e);
    h->key_next += 1U;
  }
}

/* The window opens at the first poll, after the program has accepted its arguments, disc and BIOS: a refusal never flashes a window. */
static int ensure_open(ScdSdlHost *h) {
  if (h->opened) return 0;
  if (h->failed) return -1;
  {
    char title[160];
    const char *game = getenv("SEGARECOMP_GAME_TITLE");
    const char *unthrottled = getenv("SEGARECOMP_VIEWER_UNTHROTTLED");
    if (game != NULL && *game != '\0') snprintf(title, sizeof title, "%.100s - Segarecomp", game);
    else snprintf(title, sizeof title, "segarecomp Sega CD viewer");
    h->sdl = genesis_sdl3_viewer_open(title, 3);
    if (h->sdl == NULL) {
      fprintf(stderr, "viewer: SDL3 window could not be opened\n");
      h->failed = 1;
      return -1;
    }
    genesis_sdl3_viewer_host(h->sdl, &h->host);
    genesis_pacer_init(&h->pacer, unthrottled != NULL && strcmp(unthrottled, "1") == 0);
    h->opened = 1;
    load_key_script(h);
  }
  return 0;
}

/* F6 holds / releases the machine, F7 advances one frame while held. Both stop the host loop only. The key presses are PEEKED from the event
 * queue (the shared host drains the same events for the controller mapping and its own keys). The pause menu (Esc / Back) also blocks inside
 * the shared host's service call, so the guest does not advance meanwhile either. */
static int sdl_poll(void *ctx, uint64_t boundary, ScdLivePoll *out) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  SDL_Event pending[32];
  int i, n;
  (void)boundary;
  if (ensure_open(h) != 0) return -1;
  push_scripted_keys(h);
  h->polls += 1U;
  SDL_PumpEvents();
  n = SDL_PeepEvents(pending, 32, SDL_PEEKEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_DOWN);
  for (i = 0; i < n; ++i) {
    if (pending[i].key.repeat) continue;
    if (pending[i].key.scancode == SDL_SCANCODE_F6) h->paused = !h->paused;
    else if (pending[i].key.scancode == SDL_SCANCODE_F7 && h->paused) out->step = 1U;
  }
  if (h->host.window_closed(h->host.ctx)) {
    out->quit = 1U;
    return 0;
  }
  out->pause = (uint8_t)h->paused;
  out->pad1 = h->host.pad1 != NULL ? h->host.pad1(h->host.ctx) : 0U;
  if (h->paused && !out->step) SDL_Delay(4U); /* a held machine sleeps; it never spins and never feeds wall time into the guest */
  return 0;
}

static int sdl_present(void *ctx, const struct GenesisFrameArtifact *frame, uint64_t onset) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  (void)onset;
  return ensure_open(h) == 0 ? h->host.present(h->host.ctx, frame) : -1;
}

static int sdl_pace(void *ctx, uint64_t frame_ticks, uint64_t ticks_per_second) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  return ensure_open(h) == 0 ? genesis_pacer_wait_period(&h->pacer, &h->host, frame_ticks, ticks_per_second) : -1;
}

static void sdl_finished(void *ctx, const struct ScdLiveSummary *summary, const ScdMachine *m) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  char detail[240];
  if (!h->opened) return;
  if (summary->outcome == SCD_LIVE_MACHINE_STOP && summary->result == SCD_RUN_STOPPED) {
    snprintf(detail, sizeof detail,
             "The generated program stopped with the typed stop %s on the %s processor. Nothing was lost: the machine fails closed rather than guess.",
             scd_stop_class_name(m->stop.stop_class), m->stop.cpu == SCD_SUB ? "sub" : "main");
    genesis_sdl3_viewer_stop_screen(h->sdl, "The game stopped", detail);
  } else if (summary->outcome == SCD_LIVE_MACHINE_STOP && summary->result == SCD_RUN_BUDGET) {
    genesis_sdl3_viewer_stop_screen(h->sdl, "The viewer stopped", "The instruction budget of this run was used up.");
  } else if (summary->outcome == SCD_LIVE_RECORDING_CAPACITY) {
    genesis_sdl3_viewer_stop_screen(h->sdl, "Recording full", "The input recording reached its size limit. The recorded part was kept.");
  }
}

int scd_viewer_main_bios(const ScdProgram *program, const ScdAuthentic *authentic, int argc, char **argv) {
  static ScdLiveHost live;
  int rc;
  memset(&g_host, 0, sizeof g_host);
  memset(&live, 0, sizeof live);
  live.ctx = &g_host;
  live.poll = sdl_poll;
  live.present = sdl_present;
  live.pace = sdl_pace;
  live.finished = sdl_finished;
  scd_live_host_registered = &live;
  rc = scd_program_main_bios(program, authentic, argc, argv);
  scd_live_host_registered = NULL;
  if (g_host.opened) genesis_sdl3_viewer_close(g_host.sdl);
  return rc;
}
