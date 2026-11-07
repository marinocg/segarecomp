/*
 * SEG-007-T254: production seam between a generated Genesis bridge program and the
 * viewer. The viewer-mode build (tools/genesis_startup_bridge.py --viewer) compiles the
 * UNMODIFIED generated C with -Dgenesis_runtime_run=genesis_viewer_hook_run, so the
 * generated main hands its own GenesisRuntime, dispatcher, and runner allowance to this
 * function instead of the one-shot runner. Headless builds never define that macro and
 * never link this file. No decoding, no second dispatcher, no guest-state access other
 * than genesis_runtime_run (via the viewer core) and the T255 live frame observer.
 *
 * Viewer options arrive via SEGARECOMP_VIEWER_UNTHROTTLED / SEGARECOMP_VIEWER_SLICE
 * (already validated by the Python driver, re-validated here) so the generated argv ABI
 * is unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_persistence.h"
#include "genesis_viewer_state.h"
#include "runtime.h"
#include "vdp_render.h"
#include "viewer.h"
#include "sv_host.h"
#include "sv_perf.h"
#include "viewer_sdl3.h"
#ifdef SEGARECOMP_NATIVE_PROVIDER_CAPS
#include "native_compat.h" /* SEG-124: the provider of a verified package is linked only into a program built from it */
#endif
#ifdef SEGARECOMP_GENESIS_SOUND
#include "genesis_audio_present.h" /* SEG-032-T009: the shared mixer is presented, never advanced, by the viewer */
#include "genesis_sound.h"         /* SEG-032-T008: the sound-carrying link set of `segarecomp build` */
#endif

GenesisControlTransfer genesis_viewer_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                               uint32_t dispatch_allowance);

static GenesisFrameArtifact g_latest_frame;
static GenesisSdl3Viewer *g_sdl;                 /* SEG-131: the session's window (the restore hook resets its audio queue) */
static GenesisViewerOptions g_options;
static GenesisLiveFrameObserver *g_observer;
static GenesisViewerHost g_host;                 /* the final host table (the baseline frame is presented through it) */
static int g_discontinuity;
static GenesisRasterCapture g_raster_capture; /* SEG-055: host-owned per-scanline capture buffer */

#ifdef SEGARECOMP_GENESIS_SOUND
/* SEG-032-T009 (ADR 0075): after each slice the viewer drains what the mixer already produced into the host sink. The mixer is fed by
 * the devices on the runtime's own synchronization points, exactly as in the headless program; nothing flows back. */
static GenesisAudioPresenter g_presenter;

static void viewer_after_slice(void *ctx, GenesisRuntime *runtime) {
  GenesisMixer *mixer = genesis_sound_mixer();
  (void)ctx;
  (void)runtime;
  if (mixer != NULL) genesis_audio_present(&g_presenter, mixer);
}
#endif

/* ---- SEG-125 (ADR 0273): the optional performance collector. Host-time observation only: it wraps the host's callbacks around the existing loop
 * (genesis_viewer_run is unchanged), reads the clock at slice / frame / present boundaries (never per instruction), and feeds nothing back to the guest. It is
 * idle (one branch per callback) unless SEGARECOMP_PERF_REPORT asks for a report or the player switches the overlay on. ---- */
static SvPerf g_perf;
static GenesisViewerHost g_inner;     /* the SDL host's own callbacks, wrapped below */
static GenesisPacer g_pacer;
static char g_renderer[48];
static GenesisRuntime *g_runtime;
static uint64_t g_slice_t0;

static uint64_t perf_clock(void) { return g_inner.now_ns(g_inner.ctx); }

/* Slice start (the host's once-per-slice poll): the previous boundaries' pacer verdicts are settled, the observation window is established BEFORE the guest runs (so the first
 * slice is counted, and nothing from while the collector was inactive is attributed), and the emulation clock starts. */
static int perf_window_closed(void *ctx) {
  const int closed = g_inner.window_closed(ctx);
  if (g_perf.active) {
    sv_perf_pace_settle(&g_perf, g_pacer.sleep_calls, g_pacer.unthrottled);
    sv_perf_observe_begin(&g_perf, g_runtime->virtual_frame_count);
    g_slice_t0 = g_perf.now();
  } else {
    g_perf.observing = 0;
    g_slice_t0 = 0U;
  }
  return closed;
}

static void perf_sleep(void *ctx, uint64_t ns) {
  const uint64_t t = sv_perf_span_begin(&g_perf);
  g_inner.sleep_ns(ctx, ns);
  sv_perf_span_end(&g_perf, SV_SPAN_PACING_WAIT, t);
}

static int perf_raster_producer(const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster, GenesisFrameArtifact *out) {
  const uint64_t t = sv_perf_span_begin(&g_perf);
  const int rc = genesis_vdp_produce_frame_raster_undigested(vram, raster, out); /* SEG-127-T003: presentation never reads frame_digest */
  sv_perf_span_end(&g_perf, SV_SPAN_VDP_COMPOSE, t);
  return rc;
}

/* End of every guest slice: emulation time of the slice, then the observation (frame boundaries crossed, if any). */
static void perf_slice_done(GenesisRuntime *runtime) {
  if (!g_perf.active) { g_perf.observing = 0; return; }
  if (g_slice_t0 != 0U) sv_perf_span_end(&g_perf, SV_SPAN_EMULATION, g_slice_t0);
  g_slice_t0 = 0U;
  g_perf.guest_master_ticks = runtime->scheduler.master_ticks;
  sv_perf_observe_end(&g_perf, runtime->virtual_frame_count, g_pacer.sleep_calls);
}

#ifdef SEGARECOMP_GENESIS_SOUND
static void viewer_after_slice(void *ctx, GenesisRuntime *runtime);
static void perf_after_slice(void *ctx, GenesisRuntime *runtime) {
  uint64_t t;
  perf_slice_done(runtime);
  t = sv_perf_span_begin(&g_perf);
  viewer_after_slice(ctx, runtime);
  sv_perf_span_end(&g_perf, SV_SPAN_AUDIO_PRESENT, t);
}
#else
static void perf_after_slice(void *ctx, GenesisRuntime *runtime) { (void)ctx; perf_slice_done(runtime); }
#endif

static void perf_write_report(const char *path, const char *status, const char *outcome, const GenesisViewerResult *r, const GenesisRuntime *runtime, const GenesisLiveFrameObserver *observer,
                              const GenesisRasterCapture *raster, const char *renderer, int unthrottled, uint32_t slice, const char *privacy) {
  SvPerfMeta meta;
  memset(&meta, 0, sizeof meta);
  meta.platform = "genesis";
  meta.entry = "viewer";
  meta.status = status;
  meta.outcome = outcome;
  meta.renderer = renderer[0] != '\0' ? renderer : NULL;
  meta.privacy = privacy;
  meta.paced = !unthrottled;
  meta.slice_dispatches = slice;
  if (r != NULL) {
    g_perf.counters[0] = r->dispatches;
    g_perf.counters[1] = r->slices;
    g_perf.resyncs = g_pacer.resyncs;
  }
  g_perf.counters[2] = runtime->z80_epoch.epoch_count;
  g_perf.counters[3] = observer->sequence;
  g_perf.counters[4] = raster->render_rejections;
  g_perf.counters[5] = raster->vram_journal_overflows;
  meta.counter_names[0] = "m68k_dispatches";
  meta.counter_names[1] = "runner_slices";
  meta.counter_names[2] = "z80_epochs";
  meta.counter_names[3] = "frames_published";
  meta.counter_names[4] = "render_rejections";
  meta.counter_names[5] = "vram_journal_overflows";
#ifdef SEGARECOMP_GENESIS_SOUND
  g_perf.counters[6] = genesis_sound_mixer() != NULL ? genesis_sound_mixer()->frames : 0U;
  g_perf.counters[7] = g_presenter.underruns;
  g_perf.counters[8] = g_presenter.overrun_dropped;
  meta.counter_names[6] = "audio_frames";
  meta.counter_names[7] = "audio_underruns";
  meta.counter_names[8] = "audio_overrun_dropped";
#endif
  meta.unavailable[0][0] = "m68k_time"; meta.unavailable[0][1] = "aggregated in emulation: timing it would need a clock read per instruction";
  meta.unavailable[1][0] = "z80_time"; meta.unavailable[1][1] = "aggregated in emulation: the Z80 is synchronized inside M68K retirement";
  meta.unavailable[2][0] = "dma_interrupt_counts"; meta.unavailable[2][1] = "no existing inexpensive counter";
  meta.unavailable[3][0] = "gpu_time"; meta.unavailable[3][1] = "present is host submission time, not GPU execution time";
  (void)sv_perf_write_file(&g_perf, &meta, path);
}



/* ---- SEG-131 (ADR 0278): whole-machine save states. The service owns the slot files and (from Checkpoint E) the rewind timeline; this hook only supplies what is
 * session-specific: the trusted game identity from the launcher, the state directory, and what must be reset around a restored machine. ---- */
static GenesisViewerState *g_state;

static int parse_hex32(const char *text, uint8_t out[32]) {
  unsigned i;
  if (text == NULL || strlen(text) != 64U) return 0;
  for (i = 0U; i < 32U; ++i) {
    unsigned hi, lo;
    const char a = text[2U * i], b = text[2U * i + 1U];
    if (a >= '0' && a <= '9') hi = (unsigned)(a - '0'); else if (a >= 'a' && a <= 'f') hi = (unsigned)(a - 'a' + 10); else if (a >= 'A' && a <= 'F') hi = (unsigned)(a - 'A' + 10); else return 0;
    if (b >= '0' && b <= '9') lo = (unsigned)(b - '0'); else if (b >= 'a' && b <= 'f') lo = (unsigned)(b - 'a' + 10); else if (b >= 'A' && b <= 'F') lo = (unsigned)(b - 'A' + 10); else return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 1;
}

/* Everything that belonged to the abandoned future is dropped here, at the barrier, before the guest runs again: queued host audio, the pacing deadline, the
 * partially captured frame, the published-frame baseline and the held inputs. The picture shown is rendered from the RESTORED video state so the player never
 * sees a stale frame. */
static void viewer_after_restore(void *ctx) {
  GenesisRuntime *runtime = (GenesisRuntime *)ctx;
  const uint32_t sleeps = g_pacer.sleep_calls, resyncs = g_pacer.resyncs;
  memset(g_raster_capture.captured, 0, sizeof g_raster_capture.captured);   /* the lines captured so far belong to the old timeline */
  g_raster_capture.frame.has_vram_journal = 0U;
#ifdef SEGARECOMP_GENESIS_SOUND
  genesis_sdl3_viewer_audio_clear(g_sdl);
  g_presenter.started = 0;   /* the next accepted chunk is a fresh start, not an underrun */
#endif
  genesis_pacer_init(&g_pacer, g_options.unthrottled);   /* the deadline restarts from the next boundary; the monitoring counters continue */
  g_pacer.sleep_calls = sleeps;
  g_pacer.resyncs = resyncs;
  if (g_observer != NULL) {
    if (genesis_vdp_produce_frame(runtime->devices.vdp.vram, runtime->devices.vdp.vsram, runtime->devices.vdp.cram, runtime->devices.vdp.registers, &g_latest_frame) == 0 &&
        genesis_frame_artifact_is_populated(&g_latest_frame)) {
      ++g_observer->sequence;
      if (g_host.present != NULL) (void)g_host.present(g_host.ctx, &g_latest_frame);
    }
  }
  g_discontinuity = 1;
}

static int viewer_discontinuity(void *ctx) {
  const int flagged = g_discontinuity;
  (void)ctx;
  g_discontinuity = 0;
  return flagged;
}

/* ---- SEG-131 (ADR 0278): battery-backed SRAM / X24C01 EEPROM persistence. The launcher names the directory of the running cartridge and variant in SEGARECOMP_SAVE_DIR for an
 * interactive session only; replays, recordings and the build pass never receive it. The load happens after the generated main installed the cartridge memory and
 * before the first guest instruction; the commit happens only when the player ended the session (Quit, Return to Launcher, window close). A typed guest stop, an
 * exhausted runner budget, a host error, a crash or a kill never commits. Every outcome is reported on one stderr line the launcher reads from the run log. ---- */
static void persist_report(const char *event, const char *status, const char *detail) {
  fprintf(stderr, "VIEWER_PERSIST {\"event\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}\n", event, status, detail);
}

static const char *persist_dir(void) {
  const char *dir = getenv("SEGARECOMP_SAVE_DIR");
  return dir != NULL && *dir != '\0' ? dir : NULL;
}

GenesisControlTransfer genesis_viewer_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                               uint32_t dispatch_allowance) {
  const char *argv[3];
  int argc = 0;
  const char *unthrottled = getenv("SEGARECOMP_VIEWER_UNTHROTTLED");
  const char *slice = getenv("SEGARECOMP_VIEWER_SLICE");
  GenesisViewerOptions options;
  SvPerfRequest perf_request;
  if (sv_perf_request_from_env(&perf_request) != 0) {
    fprintf(stderr, "viewer: malformed SEGARECOMP_PERF_MODE / SEGARECOMP_PERF_PRIVACY\n");
    exit(3);
  }
  const char *perf_path = perf_request.path, *perf_privacy = perf_request.privacy;
  if (unthrottled != NULL && strcmp(unthrottled, "1") == 0) argv[argc++] = "--viewer-unthrottled";
  if (slice != NULL && *slice != '\0') { argv[argc++] = "--viewer-slice"; argv[argc++] = slice; }
  if (genesis_viewer_options_parse(argc, argv, &options) != 0) {
    fprintf(stderr, "viewer: malformed viewer options\n");
    exit(3);
  }
#ifdef SEGARECOMP_GENESIS_SOUND
  (void)genesis_sound_attach(runtime); /* the Z80 machine and the shared PSG/YM2612, identical to the headless program */
#endif
  if (persist_dir() != NULL && genesis_persist_file_name(runtime) != NULL) {
    const GenesisPersistStatus loaded = genesis_persist_load(runtime, persist_dir());
    if (loaded == GENESIS_PERSIST_OK) persist_report("load", "loaded", genesis_persist_file_name(runtime));
    else if (loaded == GENESIS_PERSIST_FRESH) persist_report("load", "fresh", genesis_persist_file_name(runtime));
    else {  /* an unreadable / wrong-size / non-regular save is never overwritten and never silently replaced by a blank one: refuse before any guest instruction */
      persist_report("load", "refused", genesis_persist_status_text(loaded));
      fprintf(stderr, "viewer: the save file %s was refused (%s); it was left untouched\n", genesis_persist_file_name(runtime), genesis_persist_status_text(loaded));
      exit(6);
    }
  }
  char window_title[160];
  const char *game_title = getenv("SEGARECOMP_GAME_TITLE");
  GenesisSdl3Viewer *sdl;
  if (game_title != NULL && *game_title != '\0') snprintf(window_title, sizeof window_title, "%.100s - Segarecomp", game_title);
  else snprintf(window_title, sizeof window_title, "segarecomp Genesis viewer");
  sdl = genesis_sdl3_viewer_open(window_title, 3);
  if (sdl == NULL) {
    fprintf(stderr, "viewer: SDL3 window could not be opened\n");
    exit(3);
  }
  GenesisLiveFrameObserver observer;
  memset(&g_latest_frame, 0, sizeof(g_latest_frame));
  observer.producer = genesis_vdp_produce_frame;
  observer.latest = &g_latest_frame;
  observer.sequence = 0;
  runtime->live_frame_observer = &observer;
  memset(&g_raster_capture, 0, sizeof(g_raster_capture));
  g_raster_capture.producer = perf_raster_producer; /* SEG-055: scanline-aware frames (SEG-125: the wrapper times the composition, then calls genesis_vdp_produce_frame_raster) */
  g_raster_capture.extension = genesis_sdl3_viewer_extension(sdl, &observer.sequence); /* SEG-118: Level 2 extended backgrounds (presentation only) */
  runtime->live_raster = &g_raster_capture;
#ifdef SEGARECOMP_NATIVE_PROVIDER_CAPS
  /* SEG-124 (ADR 0267): the build compiled the provider source of ONE verified package and linked it here; the capabilities it declared are a compile-time constant. A provider that
   * declines to run, has another ABI version or cannot be attached leaves the viewer generic: it never guesses. */
  {
    void *compat_state = NULL;
    const GenesisNativeCompat *compat = segarecomp_patch_provider(&compat_state);
    if (compat == NULL || g_raster_capture.extension == NULL || genesis_sdl3_viewer_attach_native_compat(sdl, g_raster_capture.extension, compat, compat_state, SEGARECOMP_NATIVE_PROVIDER_CAPS) != 0)
      fprintf(stderr, "viewer: native compatibility provider not attached; presenting generically\n");
  }
#endif

  GenesisViewerHost host;
  genesis_sdl3_viewer_host(sdl, &host);
#ifdef SEGARECOMP_GENESIS_SOUND
  {
    const char *mute = getenv("SEGARECOMP_VIEWER_MUTE");
    const int muted = mute != NULL && strcmp(mute, "1") == 0;
    /* A failed open, a missing audio subsystem or mute leaves the viewer silent; the ring is still drained. */
    genesis_audio_presenter_init(&g_presenter, muted ? NULL : genesis_sdl3_viewer_audio_open(sdl), muted);
    genesis_sdl3_viewer_report_audio_drops(sdl, &g_presenter.overrun_dropped);
    host.after_slice = viewer_after_slice;
  }
#endif
  genesis_pacer_init(&g_pacer, options.unthrottled);
  /* SEG-125: the collector wraps the host callbacks (see above); the viewer loop itself is untouched. */
  g_inner = host;
  g_runtime = runtime;
  sv_perf_init(&g_perf, perf_clock, genesis_pacer_period_floor_ns(), perf_request.detailed);
  host.window_closed = perf_window_closed;
  if (host.sleep_ns != NULL) host.sleep_ns = perf_sleep; /* NULL is legal only when unthrottled: keep the pacer's own contract */
  host.after_slice = perf_after_slice;
  host.discontinuity = viewer_discontinuity;
  g_host = host;
  genesis_sdl3_viewer_set_perf(sdl, &g_perf);
  /* SEG-131: whole-machine save states (and, from Checkpoint E, rewind) for this session. */
  g_sdl = sdl;
  g_options = options;
  g_observer = &observer;
  g_discontinuity = 0;
  {
    GenesisViewerStateConfig state_config;
    SvStateProvider provider;
    char state_dir[1100];
    const char *reason = NULL;
    memset(&state_config, 0, sizeof state_config);
    state_config.machine.runtime = runtime;
#ifdef SEGARECOMP_GENESIS_SOUND
    state_config.machine.z80 = genesis_sound_attach(runtime);
    state_config.machine.audio = genesis_sound_audio_mutable();
#endif
    state_config.latest_frame = &g_latest_frame;
    state_config.after_restore = viewer_after_restore;
    state_config.after_restore_context = runtime;
    if (persist_dir() == NULL) {
      reason = "No save location is configured for this session: replays and recordings never use save states.";
    } else if (!parse_hex32(getenv("SEGARECOMP_ROM_SHA256"), state_config.identity.source_sha256)) {
      reason = "This game has no verified identity, so its states cannot be tied to it.";
    } else {
      const char *variant = getenv("SEGARECOMP_SAVE_VARIANT");   /* a patch package identity (64 hex), or absent / "vanilla" */
      if (variant != NULL && *variant != '\0' && strcmp(variant, "vanilla") != 0 && !parse_hex32(variant, state_config.identity.variant_sha256))
        reason = "The version of this game has no valid identity, so its states cannot be tied to it.";
      else if (snprintf(state_dir, sizeof state_dir, "%s/states", persist_dir()) <= 0 || strlen(persist_dir()) + 8U >= sizeof state_dir)
        reason = "The save location path is too long.";
      else
        state_config.state_dir = state_dir;
    }
    state_config.unavailable_reason = reason;
    g_state = genesis_viewer_state_create(&state_config);
    if (g_state != NULL) {
      genesis_viewer_state_provider(g_state, &provider);
      sv_host_set_state_provider(genesis_sdl3_viewer_sv_host(sdl), &provider);
    }
  }
  snprintf(g_renderer, sizeof g_renderer, "%s", genesis_sdl3_viewer_renderer_name(sdl) != NULL ? genesis_sdl3_viewer_renderer_name(sdl) : "");
  if (perf_path != NULL) {
    sv_perf_enable(&g_perf);
    perf_write_report(perf_path, "incomplete", "running", NULL, runtime, &observer, &g_raster_capture, g_renderer, options.unthrottled, options.slice_dispatches, perf_privacy);   /* a killed session still leaves a marked report */
  }
  GenesisViewerResult r = genesis_viewer_run(runtime, dispatch, &options, &host, &g_pacer,
                                                dispatch_allowance == UINT32_MAX ? UINT64_MAX : dispatch_allowance);
  sv_perf_pace_settle(&g_perf, g_pacer.sleep_calls, g_pacer.unthrottled); /* the last boundaries' pacer verdicts */
  sv_perf_finish(&g_perf); /* before the window closes: the session ends when the guest does, not when a stop screen is dismissed */
  runtime->live_frame_observer = NULL;
  genesis_runtime_write_notes(runtime, g_raster_capture.render_rejections, g_raster_capture.vram_journal_overflows, stderr);
  runtime->live_raster = NULL;
#ifdef SEGARECOMP_GENESIS_SOUND
  genesis_sound_finish(runtime, r.outcome == GENESIS_VIEWER_GUEST_STOP);
  viewer_after_slice(NULL, runtime);
  if (genesis_sound_mixer() != NULL) {
    const GenesisMixer *mixer = genesis_sound_mixer();
    uint8_t digest[32];
    char digest_hex[65];
    unsigned i;
    genesis_mixer_digest(mixer, digest);
    for (i = 0U; i < 32U; ++i) snprintf(digest_hex + 2U * i, 3U, "%02x", (unsigned)digest[i]);
    fprintf(stderr,
            "VIEWER_AUDIO_SUMMARY {\"frames\":%llu,\"sha256\":\"%s\",\"clipped\":%llu,\"non_silent\":%s,\"fault\":%d,"
            "\"device\":%s,\"submitted\":%llu,\"overrun_dropped\":%llu,\"refused\":%llu,\"discarded\":%llu,\"underruns\":%llu}\n",
            (unsigned long long)mixer->frames, digest_hex, (unsigned long long)mixer->clipped,
            genesis_mixer_non_silent(mixer) ? "true" : "false", (int)mixer->fault, g_presenter.sink != NULL ? "true" : "false",
            (unsigned long long)g_presenter.submitted, (unsigned long long)g_presenter.overrun_dropped,
            (unsigned long long)g_presenter.refused, (unsigned long long)g_presenter.discarded, (unsigned long long)g_presenter.underruns);
  }
#endif
  if (r.outcome == GENESIS_VIEWER_GUEST_STOP || r.outcome == GENESIS_VIEWER_HOST_ERROR) {
    char detail[160];
    snprintf(detail, sizeof detail, "The generated program stopped with a typed guest stop (code %d) at 68000 PC %06X. Nothing was lost: the machine fails closed rather than guess.",
             (int)r.transfer.kind, (unsigned)runtime->pc);
    genesis_sdl3_viewer_stop_screen(sdl, r.outcome == GENESIS_VIEWER_GUEST_STOP ? "The game stopped" : "The viewer stopped", detail);
  }
#ifdef SEGARECOMP_NATIVE_PROVIDER_CAPS
  {
    GenesisNativeCompatStats stats;
    void *unused_state = NULL;
    const GenesisNativeCompat *selected = segarecomp_patch_provider(&unused_state);
    genesis_sdl3_viewer_native_compat_stats(sdl, &stats);
    fprintf(stderr, "VIEWER_NATIVE_SUMMARY {\"capability\":\"%s\",\"frames\":%llu,\"authored\":%llu,\"hud_frames\":%llu,\"declined\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu]}\n", selected != NULL && selected->name != NULL ? selected->name : "none",
            (unsigned long long)stats.frames, (unsigned long long)stats.authored, (unsigned long long)stats.hud_frames, (unsigned long long)stats.declined[0],
            (unsigned long long)stats.declined[1], (unsigned long long)stats.declined[2], (unsigned long long)stats.declined[3], (unsigned long long)stats.declined[4],
            (unsigned long long)stats.declined[5], (unsigned long long)stats.declined[6]);
  }
#endif
  genesis_viewer_state_destroy(g_state);
  g_state = NULL;
  genesis_sdl3_viewer_close(sdl);

  const char *name = r.outcome == GENESIS_VIEWER_GUEST_STOP        ? "guest_stop"
                     : r.outcome == GENESIS_VIEWER_GUEST_COMPLETE   ? "guest_complete"
                     : r.outcome == GENESIS_VIEWER_RUNNER_EXHAUSTED ? "runner_resource_limit"
                     : r.outcome == GENESIS_VIEWER_WINDOW_CLOSED    ? "window_closed"
                     : r.outcome == GENESIS_VIEWER_HOST_ERROR       ? "host_error"
                                                                    : "invalid_argument";
  fprintf(stderr,
          "VIEWER_SUMMARY {\"viewer_opened\":true,\"frame_publication_observed\":%s,"
          "\"frame_presented\":%s,\"outcome\":\"%s\",\"execution\":\"generated-native\"}\n",
          observer.sequence > 0 ? "true" : "false", r.frames_presented > 0 ? "true" : "false", name);
  if (persist_dir() != NULL && genesis_persist_file_name(runtime) != NULL) {
    if (r.outcome == GENESIS_VIEWER_WINDOW_CLOSED) {
      GenesisPersistStatus stored = genesis_persist_ensure_dir(persist_dir());
      if (stored == GENESIS_PERSIST_OK) stored = genesis_persist_commit(runtime, persist_dir());
      persist_report("commit", stored == GENESIS_PERSIST_OK ? "stored" : "kept_previous", stored == GENESIS_PERSIST_OK ? genesis_persist_file_name(runtime) : genesis_persist_status_text(stored));
    } else {
      persist_report("commit", "not_clean_exit", name);  /* typed guest stop, runner budget, host error: the previous save stays as it was */
    }
  }
  if (perf_path != NULL) {
    /* complete: the session ended as requested (window closed, budget reached, guest finished); incomplete: a typed guest stop ended it (the numbers are valid up to it); failed: a host error */
    const char *status = r.outcome == GENESIS_VIEWER_GUEST_STOP ? "incomplete" : (r.outcome == GENESIS_VIEWER_HOST_ERROR || r.outcome == GENESIS_VIEWER_INVALID_ARGUMENT ? "failed" : "complete");
    perf_write_report(perf_path, status, name, &r, runtime, &observer, &g_raster_capture, g_renderer, options.unthrottled, options.slice_dispatches, perf_privacy);
  }
  if (r.outcome == GENESIS_VIEWER_HOST_ERROR || r.outcome == GENESIS_VIEWER_INVALID_ARGUMENT) exit(4);
  if (r.outcome == GENESIS_VIEWER_WINDOW_CLOSED || r.outcome == GENESIS_VIEWER_RUNNER_EXHAUSTED) {
    GenesisControlTransfer t;
    memset(&t, 0, sizeof(t));
    t.kind = GENESIS_RUNNER_RESOURCE_LIMIT;
    t.next_pc = runtime->pc;
    t.runner_dispatch_count = (uint32_t)(r.dispatches >= UINT32_MAX ? UINT32_MAX - 1U : r.dispatches) /* UINT32_MAX is reserved: "runner allowance exhausted" */;
    return t;
  }
  return r.transfer;
}
