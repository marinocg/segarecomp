/* SEG-078-T001 (ADR 0204): see scd_viewer.h. */
#include "scd_viewer.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_audio_policy.h"
#include "scd_live.h"
#include "scd_snapshot.h"
#include "scd_viewer_state.h"
#include "sv_host.h"
#include "sv_perf.h"
#include "viewer.h"
#include "viewer_sdl3.h"

#define KEY_SCRIPT_MAX 4096U
#define CDDA_PRIME_FRAMES 1176U   /* two sectors: a run's first frames wait (at most two video frames) until the device queue can be fed without a gap */
#define CDDA_PENDING_FRAMES 4096U /* about 93 ms: several video frames of CD-DA (a frame carries 1 or 2 sectors, 588 frames each) */

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
  /* SEG-119 (ADR 0262): Level 2. The Genesis adapter owns the surface and the request sampling; `frame_sequence` is the number of the frame being
   * presented (the onset number the driver hands over), which the adapter compares with the stamp the machine put on the extension result. */
  GenesisFrameExtension *extension;
  uint64_t frame_sequence;
  /* SEG-104 (ADR 0244): the CD-DA stream. The machine hands over one drive sector at a time (scd_live.h `audio`); the frames wait here
   * until the end of the video frame and are then presented under the Genesis audio policy (queue limit, underrun, mute counters). A one-way
   * flow: nothing here is read by the machine, and a frame that does not fit is counted and dropped, never waited for. */
  GenesisAudioPresenter presenter;
  int16_t pending[CDDA_PENDING_FRAMES * 2U];
  uint32_t pending_frames, held_frames;
  uint64_t pending_dropped, delivered;
  /* SEG-125 (ADR 0273): the optional performance collector. The driver polls once per frame boundary, so the poll is the frame boundary here; emulation (both CPUs, the
   * gate array, CD drive and PCM) runs from the end of the poll to the first host callback that follows. Host time only; nothing flows to the machine. */
  SvPerf perf;
  SvPerfRequest perf_request;
  char renderer[48];
  uint64_t emu_t0, frame;
  int target_set;
  /* SEG-132 (ADR 0279): whole-machine save states and visual rewind. The service is created when the live driver hands over the machine (`started`) and registered with the
   * shared host when the window opens; every provider call happens inside the poll, the frame barrier. */
  ScdMachine *machine;
  ScdViewerState *state;
  int unthrottled;
} ScdSdlHost;

static ScdSdlHost g_host;

static uint64_t scd_perf_clock(void) { return g_host.host.now_ns(g_host.host.ctx); }
static void perf_emulation_done(ScdSdlHost *h) {
  if (h->emu_t0 != 0U) sv_perf_span_end(&h->perf, SV_SPAN_EMULATION, h->emu_t0);
  h->emu_t0 = 0U;
}
static void perf_boundary(ScdSdlHost *h) {
  if (!h->perf.active) { h->perf.observing = 0; h->emu_t0 = 0U; return; }
  sv_perf_pace_settle(&h->perf, h->pacer.sleep_calls, h->pacer.unthrottled); /* the previous frame's pacer verdict */
  if (!h->perf.observing) { h->frame = 0U; sv_perf_observe_begin(&h->perf, 0U); }   /* the window starts at this poll: the next poll is one whole frame later */
  else sv_perf_observe_end(&h->perf, ++h->frame, h->pacer.sleep_calls);              /* one frame boundary per poll: an exact frame time */
  h->emu_t0 = h->perf.now();
}
static void perf_write(ScdSdlHost *h, const char *status, const char *outcome, const ScdLiveSummary *summary) {
  SvPerfMeta meta;
  memset(&meta, 0, sizeof meta);
  meta.platform = "sega_cd";
  meta.entry = "viewer";
  meta.status = status;
  meta.outcome = outcome;
  meta.renderer = h->renderer[0] != '\0' ? h->renderer : NULL;
  meta.privacy = h->perf_request.privacy;
  meta.paced = !h->pacer.unthrottled;
  meta.slice_dispatches = 1; /* one frame boundary per driver iteration */
  h->perf.resyncs = h->pacer.resyncs;
  h->perf.counters[0] = summary != NULL ? summary->boundaries : 0U;
  h->perf.counters[1] = summary != NULL ? summary->paused_polls : 0U;
  h->perf.counters[2] = h->delivered;
  h->perf.counters[3] = h->pending_dropped;
  h->perf.counters[4] = h->presenter.underruns;
  meta.counter_names[0] = "boundaries";
  meta.counter_names[1] = "paused_polls";
  meta.counter_names[2] = "cdda_frames_delivered";
  meta.counter_names[3] = "cdda_frames_dropped";
  meta.counter_names[4] = "audio_underruns";
  meta.unavailable[0][0] = "main_sub_cpu_split"; meta.unavailable[0][1] = "the machine exposes no per-CPU time or retirement counter; emulation covers both CPUs";
  meta.unavailable[1][0] = "guest_master_ticks"; meta.unavailable[1][1] = "the Sega CD guest clock is not exposed to the viewer adapter";
  meta.unavailable[2][0] = "vdp_compose"; meta.unavailable[2][1] = "composition happens inside the machine frame: aggregated in emulation";
  meta.unavailable[3][0] = "gpu_time"; meta.unavailable[3][1] = "present is host submission time, not GPU execution time";
  meta.unavailable[4][0] = "dma_interrupt_counts"; meta.unavailable[4][1] = "no existing inexpensive counter";
  (void)sv_perf_write_file(&h->perf, &meta, h->perf_request.path);
}

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
    h->sdl = genesis_sdl3_viewer_open_platform(title, 3, "sega-cd");
    if (h->sdl == NULL) {
      fprintf(stderr, "viewer: SDL3 window could not be opened\n");
      h->failed = 1;
      return -1;
    }
    genesis_sdl3_viewer_host(h->sdl, &h->host);
    if (h->state != NULL) {   /* SEG-132: F5 / F8 quick save and load, F7 rewind, the pause menu rows and the gamepad shortcut */
      SvStateProvider provider;
      scd_viewer_state_provider(h->state, &provider);
      sv_host_set_state_provider(genesis_sdl3_viewer_sv_host(h->sdl), &provider);
    }
    h->extension = genesis_sdl3_viewer_extension(h->sdl, &h->frame_sequence); /* NULL: the viewer stays Original, reporting Unavailable */
    genesis_pacer_init(&h->pacer, unthrottled != NULL && strcmp(unthrottled, "1") == 0);
    {
      const char *mute = getenv("SEGARECOMP_VIEWER_MUTE");
      const int muted = mute != NULL && strcmp(mute, "1") == 0;
      /* A failed open, a missing audio subsystem or mute leaves the viewer silent; the stream is still consumed so it never accumulates. */
      genesis_audio_presenter_init(&h->presenter, muted ? NULL : genesis_sdl3_viewer_audio_open(h->sdl), muted);
      genesis_sdl3_viewer_report_audio_drops(h->sdl, &h->presenter.overrun_dropped);
    }
    {   /* SEG-125: the collector is armed always, active only for a requested report or while the overlay is shown */
      if (sv_perf_request_from_env(&h->perf_request) != 0) {
        fprintf(stderr, "viewer: malformed SEGARECOMP_PERF_MODE / SEGARECOMP_PERF_PRIVACY\n");
        exit(3);
      }
      sv_perf_init(&h->perf, scd_perf_clock, 16683333U, h->perf_request.detailed);   /* refined at the first pace callback from the machine's own frame length */
      genesis_sdl3_viewer_set_perf(h->sdl, &h->perf);
      snprintf(h->renderer, sizeof h->renderer, "%s", genesis_sdl3_viewer_renderer_name(h->sdl) != NULL ? genesis_sdl3_viewer_renderer_name(h->sdl) : "");
      if (h->perf_request.path != NULL) {
        sv_perf_enable(&h->perf);
        perf_write(h, "incomplete", "running", NULL);   /* a killed session still leaves a marked report */
      }
    }
    h->opened = 1;
    load_key_script(h);
  }
  return 0;
}

/* F6 holds / releases the machine, F9 advances one frame while held (SEG-132: F7 is the rewind key on every console, F5 / F8 quick save / load). Both stop the host loop only. The key presses are PEEKED from the event
 * queue (the shared host drains the same events for the controller mapping and its own keys). The pause menu (Esc / Back) also blocks inside
 * the shared host's service call, so the guest does not advance meanwhile either. */
static int sdl_poll(void *ctx, uint64_t boundary, ScdLivePoll *out) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  SDL_Event pending[32];
  int i, n;
  (void)boundary;
  if (ensure_open(h) != 0) return -1;
  scd_viewer_state_after_slice(h->state);   /* SEG-132: the rewind cadence, at the frame barrier: the previous frame is run, presented and paced, the host has not been polled yet */
  push_scripted_keys(h);
  h->polls += 1U;
  SDL_PumpEvents();
  n = SDL_PeepEvents(pending, 32, SDL_PEEKEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_DOWN);
  for (i = 0; i < n; ++i) {
    if (pending[i].key.repeat) continue;
    if (pending[i].key.scancode == SDL_SCANCODE_F6) h->paused = !h->paused;
    else if (pending[i].key.scancode == SDL_SCANCODE_F9 && h->paused) out->step = 1U;
  }
  if (h->host.window_closed(h->host.ctx)) {
    out->quit = 1U;
    return 0;
  }
  out->pause = (uint8_t)h->paused;
  out->pad1 = h->host.pad1 != NULL ? h->host.pad1(h->host.ctx) : 0U;
  if (h->paused && !out->step) SDL_Delay(4U); /* a held machine sleeps; it never spins and never feeds wall time into the guest */
  perf_boundary(h);
  return 0;
}

static int sdl_present(void *ctx, const struct GenesisFrameArtifact *frame, uint64_t onset) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  if (ensure_open(h) != 0) return -1;
  h->frame_sequence = onset;
  scd_viewer_state_note_frame(h->state, frame);   /* SEG-132: the picture a state's thumbnail shows (valid until the machine next runs) */
  perf_emulation_done(h);
  return h->host.present(h->host.ctx, frame);
}

static GenesisFrameExtension *sdl_extension(void *ctx) { return ((ScdSdlHost *)ctx)->extension; }

static void flush_cdda(ScdSdlHost *h, int force) {
  if (h->pending_frames == 0U) {
    h->held_frames = 0U;
    return;
  }
  /* A sector is 13.3 ms and a video frame 16.7 ms: delivered one sector at a time the host queue would run dry between frames. Into an empty
   * queue a run's first frames therefore wait for a second sector (never more than two frames); afterwards the queue stays ahead. */
  if (h->presenter.sink != NULL && !h->presenter.muted && h->presenter.sink->queued_frames(h->presenter.sink->context) == 0U &&
      h->pending_frames < CDDA_PRIME_FRAMES && h->held_frames < 2U && !force) {
    h->held_frames += 1U;
    return;
  }
  h->held_frames = 0U;
  genesis_audio_present_chunk(&h->presenter, h->pending, h->pending_frames);
  h->delivered += h->pending_frames;
  h->pending_frames = 0U;
}

/* SEG-104: called by the machine for every CD-DA sector the guest's drive plays. */
static int sdl_audio(void *ctx, const int16_t *frames, uint32_t count) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  if (!h->opened || count > CDDA_PENDING_FRAMES - h->pending_frames) {
    h->pending_dropped += count;
    return 1;
  }
  memcpy(h->pending + 2U * h->pending_frames, frames, (size_t)count * 4U);
  h->pending_frames += count;
  return 0;
}

static int sdl_pace(void *ctx, uint64_t frame_ticks, uint64_t ticks_per_second) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  int rc;
  uint64_t t;
  if (h->opened) {
    if (!h->target_set) {   /* the machine's own frame length, by the pacer's overflow-safe exact reduction */
      uint64_t num = 0U, den = 1U;
      if (genesis_pacer_period(frame_ticks, ticks_per_second, &num, &den) == 0) h->perf.target_period_ns = num / den;
      h->target_set = 1;
    }
    perf_emulation_done(h);
    t = sv_perf_span_begin(&h->perf);
    flush_cdda(h, 0); /* the frame's CD-DA reaches the device before the pacer waits */
    sv_perf_span_end(&h->perf, SV_SPAN_AUDIO_PRESENT, t);
  }
  if (ensure_open(h) != 0) return -1;
  t = sv_perf_span_begin(&h->perf);
  rc = genesis_pacer_wait_period(&h->pacer, &h->host, frame_ticks, ticks_per_second);
  sv_perf_span_end(&h->perf, SV_SPAN_PACING_WAIT, t);
  return rc;
}

static void sdl_finished(void *ctx, const struct ScdLiveSummary *summary, const ScdMachine *m) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  char detail[240];
  if (!h->opened) return;
  flush_cdda(h, 1);
  sv_perf_pace_settle(&h->perf, h->pacer.sleep_calls, h->pacer.unthrottled);
  sv_perf_finish(&h->perf);
  if (h->perf_request.path != NULL) {
    const int stopped = summary->outcome == SCD_LIVE_MACHINE_STOP && summary->result == SCD_RUN_STOPPED;
    const int failed = summary->outcome == SCD_LIVE_HOST_ERROR || summary->outcome == SCD_LIVE_RECORDING_REFUSED;
    perf_write(h, stopped ? "incomplete" : (failed ? "failed" : "complete"), stopped ? "guest_stop" : scd_live_outcome_name(summary->outcome), summary);
  }
  if (h->state != NULL) {   /* observed rewind statistics (counts, not estimates) for the measurement tools */
    SvStateServiceStats st;
    scd_viewer_state_stats(h->state, &st);
    fprintf(stderr, "VIEWER_REWIND_STATS {\"captures\":%llu,\"evictions\":%llu,\"failures\":%llu,\"history_entries\":%u,\"history_bytes\":%llu,\"capture_ns_total\":%llu,\"capture_ns_max\":%llu,\"restores\":%llu,\"last_capture_bytes\":%llu,\"retained_span_ms\":%llu}\n",
            (unsigned long long)st.captures, (unsigned long long)st.evictions, (unsigned long long)st.capture_failures, (unsigned)st.history_entries, (unsigned long long)st.history_bytes,
            (unsigned long long)st.capture_ns_total, (unsigned long long)st.capture_ns_max, (unsigned long long)st.restores, (unsigned long long)st.last_capture_bytes, (unsigned long long)st.retained_span_ms);
  }
  {  /* SEG-132 automation: the final guest state of the session, for the end-to-end tests (unset in every shipped use) */
    const char *report = getenv("SEGARECOMP_VIEWER_STATE_REPORT");
    if (report != NULL && *report != '\0') {
      FILE *f = fopen(report, "wb");
      if (f != NULL) {
        uint8_t digest[32];
        unsigned i;
        const int have = h->state != NULL && scd_viewer_state_canonical_sha256(h->state, digest) == 0;
        fprintf(f, "{\"onsets\":%llu,\"ticks\":%llu,\"retired_main\":%llu,\"retired_sub\":%llu,\"canonical_sha256\":\"", (unsigned long long)m->vdp_frames, (unsigned long long)m->now,
                (unsigned long long)m->cpu[0].retired, (unsigned long long)m->cpu[1].retired);
        if (have) for (i = 0U; i < 32U; ++i) fprintf(f, "%02x", (unsigned)digest[i]);
        fprintf(f, "\",\"result0\":%u}\n", (unsigned)m->work_ram[0xC000U & 0xFFFFU]);
        fclose(f);
      }
    }
  }
  if (h->extension != NULL && h->extension->produced + h->extension->failed != 0U) /* only a run that requested Level 2 */
    fprintf(stderr, "viewer: level2 extension produced=%llu failed=%llu\n", (unsigned long long)h->extension->produced,
            (unsigned long long)h->extension->failed);
  if (h->delivered + h->pending_dropped != 0U)
    fprintf(stderr, "viewer: cdda frames offered=%llu submitted=%llu overrun_dropped=%llu refused=%llu discarded=%llu underruns=%llu pending_dropped=%llu\n",
            (unsigned long long)(h->delivered + h->pending_dropped), (unsigned long long)h->presenter.submitted,
            (unsigned long long)h->presenter.overrun_dropped, (unsigned long long)h->presenter.refused, (unsigned long long)h->presenter.discarded,
            (unsigned long long)h->presenter.underruns, (unsigned long long)h->pending_dropped);
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

/* ---- SEG-132 (ADR 0279): whole-machine save states and visual rewind. The service owns the slot files and the timeline; this file supplies what is session-specific: the save
 * folder from the launcher, the rewind knobs and what must be reset around a restored machine on the host side. The game's identity is intrinsic: the codec binds the verified
 * disc, the BIOS image and the executable-image contract, so no identity travels through the environment. ---- */
static uint32_t env_u32(const char *name, uint32_t low, uint32_t high, uint32_t fallback) {
  const char *text = getenv(name);
  unsigned long value = 0UL;
  size_t i;
  if (text == NULL || *text == '\0' || strlen(text) > 9U) return fallback;
  for (i = 0U; text[i] != '\0'; ++i) {
    if (text[i] < '0' || text[i] > '9') return fallback;
    value = value * 10UL + (unsigned long)(text[i] - '0');
  }
  return value >= low && value <= high ? (uint32_t)value : fallback;
}

/* Host side of a restore: the audio of the abandoned future (queued in the device, waiting in the CD-DA staging buffer, or counted as an underrun) is dropped, pacing restarts
 * from the next boundary (the monitoring counters continue) and the performance window re-opens. The guest-time audio state itself (mixer queue, drive, chips) was restored. */
static void host_after_restore(void *ctx) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  const uint32_t sleeps = h->pacer.sleep_calls, resyncs = h->pacer.resyncs;
  if (h->sdl != NULL) genesis_sdl3_viewer_audio_clear(h->sdl);
  h->pending_frames = 0U;
  h->held_frames = 0U;
  h->presenter.started = 0;
  genesis_pacer_init(&h->pacer, h->unthrottled);
  h->pacer.sleep_calls = sleeps;
  h->pacer.resyncs = resyncs;
  h->perf.observing = 0;
}

static void sdl_started(void *ctx, ScdMachine *machine) {
  ScdSdlHost *h = (ScdSdlHost *)ctx;
  ScdViewerStateConfig cfg;
  char state_dir[1100];
  const char *save_dir = getenv("SEGARECOMP_SAVE_DIR");
  const char *off = getenv("SEGARECOMP_REWIND");
  const char *refusal = scd_snapshot_refusal(machine);
  const char *slots_reason = NULL;
  const char *unthrottled = getenv("SEGARECOMP_VIEWER_UNTHROTTLED");
  h->machine = machine;
  h->unthrottled = unthrottled != NULL && strcmp(unthrottled, "1") == 0;
  memset(&cfg, 0, sizeof cfg);
  cfg.machine = machine;
  cfg.rewind_budget_bytes = SV_STATE_REWIND_BUDGET_BYTES;
  cfg.rewind_interval_ms = SV_STATE_REWIND_INTERVAL_MS;
  cfg.rewind_target_ms = SV_STATE_REWIND_TARGET_MS;
  if (off != NULL && (strcmp(off, "0") == 0 || strcmp(off, "off") == 0)) cfg.rewind_budget_bytes = 0U;
  if (cfg.rewind_budget_bytes != 0U) cfg.rewind_budget_bytes = env_u32("SEGARECOMP_REWIND_BUDGET_MB", 1U, 1024U, cfg.rewind_budget_bytes / (1024U * 1024U)) * 1024U * 1024U;
  cfg.rewind_interval_ms = env_u32("SEGARECOMP_REWIND_INTERVAL_MS", 50U, 60000U, cfg.rewind_interval_ms);
  cfg.rewind_target_ms = env_u32("SEGARECOMP_REWIND_TARGET_MS", 500U, 3600000U, cfg.rewind_target_ms);
  cfg.after_restore = host_after_restore;
  cfg.after_restore_context = h;
  if (refusal != NULL) {   /* a configuration whose future cannot be reproduced exactly: no state and no rewind, and the player is told why */
    slots_reason = refusal;
    cfg.rewind_unavailable_reason = refusal;
  } else if (save_dir == NULL || *save_dir == '\0') {
    slots_reason = "No save location is configured for this session: replays and recordings never use save states.";
  } else if (snprintf(state_dir, sizeof state_dir, "%s/states", save_dir) <= 0 || strlen(save_dir) + 8U >= sizeof state_dir) {
    slots_reason = "The save location path is too long.";
  } else {
    cfg.state_dir = state_dir;
  }
  cfg.unavailable_reason = slots_reason;
  h->state = scd_viewer_state_create(&cfg);
}

/* The host this viewer registers with the live driver (a function of its own so the viewer tests drive exactly the registered callbacks). */
static void fill_live_host(ScdLiveHost *live) {
  memset(live, 0, sizeof *live);
  live->ctx = &g_host;
  live->poll = sdl_poll;
  live->present = sdl_present;
  live->pace = sdl_pace;
  live->finished = sdl_finished;
  live->audio = sdl_audio;
  live->extension = sdl_extension;
  live->started = sdl_started;
}

int scd_viewer_main_bios(const ScdProgram *program, const ScdAuthentic *authentic, int argc, char **argv) {
  static ScdLiveHost live;
  int rc;
  memset(&g_host, 0, sizeof g_host);
  fill_live_host(&live);
  scd_live_host_registered = &live;
  rc = scd_program_main_bios(program, authentic, argc, argv);
  scd_live_host_registered = NULL;
  scd_viewer_state_destroy(g_host.state);
  g_host.state = NULL;
  if (g_host.opened) genesis_sdl3_viewer_close(g_host.sdl);
  return rc;
}
