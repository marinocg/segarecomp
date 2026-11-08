/* Interactive Master System viewer: the generated program's `main` when built as a viewer (SEG-009-T009; ADR 0070).
 *
 *   <game> [--viewer-unthrottled] [--viewer-mute] [--viewer-frames <n>] [--viewer-scale <n>] [--viewer-record <file>]
 *
 * Keys (rebindable in the pause menu): arrows = D-pad, Z = button 1, X = button 2, P = the console Pause button; Esc / gamepad Back opens the
 * host pause menu (Resume, Quick Settings, Video, Controls, Audio, System incl. Reset, Return/Quit); window close = quit. `--viewer-record` writes the
 * input stream the run consumed as a headless `--input` script (the run since the last reset). Exit: 0 quit or frame
 * limit; 3 guest stop; 4 SMS_ERROR at start; 5 viewer/host failure; 64 usage. The ROM is the build-time embedded array. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_viewer_sdl3.h"
#include "sms_viewer_state.h"
#include "sv_host.h"
#include "sv_perf.h"

extern const uint8_t sms_rom_data[];
extern const uint32_t sms_rom_size;
extern const uint32_t sms_rom_mapper_family;

static SmsViewerRig rig;
static char script[SMS_VIEWER_INPUT_CAPACITY * 40u];

/* ---- SEG-125 (ADR 0273): the optional performance collector. It wraps the host callbacks around the unchanged sms_viewer_run: one guest frame per poll, so the poll
 * is the frame boundary; emulation runs from the end of the poll to the first host callback that follows (audio drain, pacing sleep or present). Idle unless
 * SEGARECOMP_PERF_REPORT asks for a report or the player switches the overlay on (Video > Performance Overlay or F3). Host time only; nothing flows to the guest. ---- */
static SvPerf g_perf;
static SmsViewerHost g_inner;
static SmsPacer g_pacer;
static uint64_t g_emu_t0, g_frame;
static SmsViewerState *g_state;   /* SEG-132 (ADR 0279): slot files and the rewind timeline of this session */
static SmsSdl3Viewer *g_viewer;
static SmsViewerOptions g_options;

static uint64_t perf_clock(void) { return g_inner.now_ns(g_inner.ctx); }
static void emulation_done(void) {
  if (g_emu_t0 != 0U) sv_perf_span_end(&g_perf, SV_SPAN_EMULATION, g_emu_t0);
  g_emu_t0 = 0U;
}
static int perf_poll(void *ctx, SmsViewerInput *in) {
  int rc;
  sms_viewer_state_after_slice(g_state);   /* SEG-132: the rewind cadence, at the frame barrier: the previous frame is run, drained, paced and presented, the host has not been polled yet */
  rc = g_inner.poll(ctx, in);
  if (!g_perf.active) { g_perf.observing = 0; g_emu_t0 = 0U; return rc; }
  sv_perf_pace_settle(&g_perf, g_pacer.sleep_calls, g_pacer.unthrottled); /* the previous frame's pacer verdict */
  if (!g_perf.observing) { g_frame = 0U; sv_perf_observe_begin(&g_perf, 0U); }   /* the window starts at this poll: the next poll is one whole frame later */
  else sv_perf_observe_end(&g_perf, ++g_frame, g_pacer.sleep_calls);              /* one guest frame per poll: an exact frame time */
  g_emu_t0 = g_perf.now();
  return rc;
}
static void perf_sleep(void *ctx, uint64_t ns) {
  uint64_t t;
  emulation_done();
  t = sv_perf_span_begin(&g_perf);
  g_inner.sleep_ns(ctx, ns);
  sv_perf_span_end(&g_perf, SV_SPAN_PACING_WAIT, t);
}
static int perf_present(void *ctx, const uint8_t *framebuffer, uint32_t height) {
  emulation_done();
  return g_inner.present(ctx, framebuffer, height);
}
static int perf_audio(void *ctx, const int16_t *samples, uint32_t count) {
  int rc;
  uint64_t t;
  emulation_done();
  t = sv_perf_span_begin(&g_perf);
  rc = g_inner.audio(ctx, samples, count);
  sv_perf_span_end(&g_perf, SV_SPAN_AUDIO_PRESENT, t);
  return rc;
}
static void perf_write(const SvPerfRequest *request, const char *status, const char *outcome, const SmsViewerResult *r, const char *renderer, int unthrottled) {
  SvPerfMeta meta;
  memset(&meta, 0, sizeof meta);
  meta.platform = "master_system";
  meta.entry = "viewer";
  meta.status = status;
  meta.outcome = outcome;
  meta.renderer = renderer;
  meta.privacy = request->privacy;
  meta.paced = !unthrottled;
  meta.slice_dispatches = 1; /* one guest frame per loop iteration */
  g_perf.resyncs = g_pacer.resyncs;
  g_perf.counters[0] = r != NULL ? r->frames : 0U;
  g_perf.counters[1] = r != NULL ? r->audio_samples : 0U;
  g_perf.counters[2] = r != NULL ? r->audio_failures : 0U;
  g_perf.counters[3] = rig.machine.rt.state.cycles;
  meta.counter_names[0] = "guest_frames";
  meta.counter_names[1] = "audio_samples";
  meta.counter_names[2] = "audio_failures";
  meta.counter_names[3] = "z80_cycles";
  meta.unavailable[0][0] = "guest_master_ticks"; meta.unavailable[0][1] = "Master System guest time is reported as the z80_cycles counter";
  meta.unavailable[1][0] = "vdp_compose"; meta.unavailable[1][1] = "the renderer composes inside the guest frame: aggregated in emulation";
  meta.unavailable[2][0] = "rgb_convert"; meta.unavailable[2][1] = "the Master System renderer produces the presented bytes directly";
  meta.unavailable[3][0] = "gpu_time"; meta.unavailable[3][1] = "present is host submission time, not GPU execution time";
  meta.unavailable[4][0] = "dma_interrupt_counts"; meta.unavailable[4][1] = "no existing inexpensive counter";
  (void)sv_perf_write_file(&g_perf, &meta, request->path);
}

/* ---- SEG-132 (ADR 0279): whole-machine save states and visual rewind. The service owns the slot files and the timeline; this file only supplies what is session-specific:
 * the trusted game identity and save folder from the launcher, the rewind knobs, and what must be reset around a restored machine on the host side. ---- */
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

static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int parse_hex32(const char *text, uint8_t out[32]) {
  unsigned i;
  if (text == NULL || strlen(text) != 64U) return 0;
  for (i = 0U; i < 32U; ++i) {
    const int hi = hex_digit(text[2U * i]), lo = hex_digit(text[2U * i + 1U]);
    if (hi < 0 || lo < 0) return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 1;
}

/* Host side of a restore: the queued audio of the abandoned future is dropped, pacing restarts from the next frame (the monitoring counters continue) and the performance
 * window re-opens (a restore is not a slow frame). */
static void viewer_after_restore(void *context) {
  const uint32_t sleeps = g_pacer.sleep_calls, resyncs = g_pacer.resyncs;
  (void)context;
  sms_sdl3_viewer_audio_clear(g_viewer);
  sms_pacer_init(&g_pacer, g_options.unthrottled);
  g_pacer.sleep_calls = sleeps;
  g_pacer.resyncs = resyncs;
  g_perf.observing = 0;
}

static void state_setup(SmsSdl3Viewer *viewer, const SmsViewerOptions *options) {
  SmsViewerStateConfig cfg;
  SvStateProvider provider;
  char state_dir[1100];
  const char *save_dir = getenv("SEGARECOMP_SAVE_DIR");
  const char *variant = getenv("SEGARECOMP_SAVE_VARIANT");
  const char *off = getenv("SEGARECOMP_REWIND");
  const char *slots_reason = NULL;
  memset(&cfg, 0, sizeof cfg);
  cfg.rig = &rig;
  cfg.rewind_budget_bytes = SV_STATE_REWIND_BUDGET_BYTES;
  cfg.rewind_interval_ms = SV_STATE_REWIND_INTERVAL_MS;
  cfg.rewind_target_ms = SV_STATE_REWIND_TARGET_MS;
  if (off != NULL && (strcmp(off, "0") == 0 || strcmp(off, "off") == 0)) cfg.rewind_budget_bytes = 0U;
  if (cfg.rewind_budget_bytes != 0U) cfg.rewind_budget_bytes = env_u32("SEGARECOMP_REWIND_BUDGET_MB", 1U, 1024U, cfg.rewind_budget_bytes / (1024U * 1024U)) * 1024U * 1024U;
  cfg.rewind_interval_ms = env_u32("SEGARECOMP_REWIND_INTERVAL_MS", 50U, 60000U, cfg.rewind_interval_ms);
  cfg.rewind_target_ms = env_u32("SEGARECOMP_REWIND_TARGET_MS", 500U, 3600000U, cfg.rewind_target_ms);
  cfg.after_restore = viewer_after_restore;
  {  /* the identity is the same whether or not a save folder exists: a state taken in memory (rewind) and one on disk describe the same game */
    const int have_source = parse_hex32(getenv("SEGARECOMP_ROM_SHA256"), cfg.identity.source_sha256);
    const int have_variant = variant == NULL || *variant == '\0' || strcmp(variant, "vanilla") == 0 || parse_hex32(variant, cfg.identity.variant_sha256);
    if (options->record != NULL) {   /* a recording describes the run since the last reset: a restore would rewrite the history it describes */
      slots_reason = "Save states are not available while input is being recorded.";
      cfg.rewind_unavailable_reason = slots_reason;
    } else if (save_dir == NULL || *save_dir == '\0') {
      slots_reason = "No save location is configured for this session: replays and recordings never use save states.";
    } else if (!have_source) {
      slots_reason = "This game has no verified identity, so its states cannot be tied to it.";
    } else if (!have_variant) {
      slots_reason = "The version of this game has no valid identity, so its states cannot be tied to it.";
    } else if (snprintf(state_dir, sizeof state_dir, "%s/states", save_dir) <= 0 || strlen(save_dir) + 8U >= sizeof state_dir) {
      slots_reason = "The save location path is too long.";
    } else {
      cfg.state_dir = state_dir;
    }
    if (!have_source) memset(cfg.identity.source_sha256, 0, sizeof cfg.identity.source_sha256);
    if (!have_variant) memset(cfg.identity.variant_sha256, 0, sizeof cfg.identity.variant_sha256);
  }
  cfg.unavailable_reason = slots_reason;
  g_state = sms_viewer_state_create(&cfg);
  if (g_state != NULL) {
    sms_viewer_state_provider(g_state, &provider);
    sv_host_set_state_provider(sms_sdl3_viewer_sv_host(viewer), &provider);
  }
}

int main(int argc, char **argv) {
  SmsViewerOptions options;
  SmsViewerResult result;
  SmsViewerHost host;
  SmsSdl3Viewer *viewer;
  SmsError error;
  SvPerfRequest perf_request;
  char renderer[48];
  int status = 0;
  if (sms_viewer_options_parse(argc - 1, (const char *const *)(argv + 1), &options) != 0) {
    fprintf(stderr, "usage: %s [--viewer-unthrottled] [--viewer-mute] [--viewer-frames <n>] [--viewer-scale <n>] [--viewer-record <file>]\n", argv[0]);
    return 64;
  }
  if (sv_perf_request_from_env(&perf_request) != 0) {
    fprintf(stderr, "viewer: malformed SEGARECOMP_PERF_MODE / SEGARECOMP_PERF_PRIVACY\n");
    return 64;
  }
  if (!sms_viewer_rig_init(&rig, sms_rom_data, sms_rom_size, (SmsMapperFamily)sms_rom_mapper_family, &error)) {
    printf("sms_error %s\n", sms_error_name(error));
    return 4;
  }
  viewer = sms_sdl3_viewer_open("segarecomp Master System", (int)options.scale, !options.mute);
  if (viewer == NULL) {
    fprintf(stderr, "cannot open the SDL3 window\n");
    return 5;
  }
  sms_sdl3_viewer_attach_extension(viewer, &rig); /* Level 2 (presentation only); a failure leaves the viewer Original */
  sms_sdl3_viewer_host(viewer, &host);
  sms_pacer_init(&g_pacer, options.unthrottled);
  g_inner = host;
  sv_perf_init(&g_perf, perf_clock, sms_pacer_period_floor_ns(), perf_request.detailed);
  host.poll = perf_poll;
  if (host.sleep_ns != NULL) host.sleep_ns = perf_sleep; /* NULL is legal only when unthrottled: keep the pacer's own contract */
  host.present = perf_present;
  if (host.audio != NULL) host.audio = perf_audio;
  sms_sdl3_viewer_set_perf(viewer, &g_perf);
  g_viewer = viewer;
  g_options = options;
  state_setup(viewer, &options);
  snprintf(renderer, sizeof renderer, "%s", sms_sdl3_viewer_renderer_name(viewer) != NULL ? sms_sdl3_viewer_renderer_name(viewer) : "");
  if (perf_request.path != NULL) {
    sv_perf_enable(&g_perf);
    perf_write(&perf_request, "incomplete", "running", NULL, renderer, options.unthrottled); /* a killed session still leaves a marked report */
  }
  result = sms_viewer_run(&rig, &host, &g_pacer, options.frames);
  sv_perf_pace_settle(&g_perf, g_pacer.sleep_calls, g_pacer.unthrottled);
  sv_perf_finish(&g_perf);
  if (perf_request.path != NULL) {
    const char *name = result.outcome == SMS_VIEWER_FRAME_LIMIT ? "frame_limit" : result.outcome == SMS_VIEWER_QUIT ? "window_closed" : result.outcome == SMS_VIEWER_GUEST_ERROR ? "guest_stop"
                       : result.outcome == SMS_VIEWER_INPUT_FULL ? "input_full" : "host_error";
    const char *state = result.outcome == SMS_VIEWER_GUEST_ERROR ? "incomplete" : (result.outcome == SMS_VIEWER_FRAME_LIMIT || result.outcome == SMS_VIEWER_QUIT ? "complete" : "failed");
    perf_write(&perf_request, state, name, &result, renderer, options.unthrottled);
  }
  if (result.outcome == SMS_VIEWER_GUEST_ERROR && options.frames == SMS_NO_LIMIT && !sms_stop_is_unadmitted_pc(&result.stop))
    sms_sdl3_viewer_stop_screen(viewer, "The game stopped", "The generated program stopped with a typed guest error. The machine fails closed instead of guessing; the run log has the stable error class.");
  {  /* SEG-132 automation: the final guest state of the session, for the end-to-end tests (unset in every shipped use) */
    const char *report = getenv("SEGARECOMP_VIEWER_STATE_REPORT");
    if (report != NULL && *report != '\0') {
      FILE *f = fopen(report, "wb");
      if (f != NULL) {
        uint8_t digest[32];
        unsigned i;
        const int have = g_state != NULL && sms_viewer_state_canonical_sha256(g_state, digest) == 0;
        fprintf(f, "{\"frames\":%llu,\"cycles\":%llu,\"counter\":%u,\"canonical_sha256\":\"", (unsigned long long)result.frames, (unsigned long long)rig.machine.rt.state.cycles,
                (unsigned)rig.machine.mem.ram[0x100] | ((unsigned)rig.machine.mem.ram[0x101] << 8));
        if (have) for (i = 0U; i < 32U; ++i) fprintf(f, "%02x", (unsigned)digest[i]);
        fprintf(f, "\",\"cram0\":%u,\"pcm_index\":%llu}\n", (unsigned)rig.vdp.cram[0], (unsigned long long)rig.psg.pcm.index);
        fclose(f);
      }
    }
  }
  if (g_state != NULL) {   /* observed rewind statistics (counts, not estimates) for the measurement tools */
    SvStateServiceStats st;
    sms_viewer_state_stats(g_state, &st);
    fprintf(stderr, "VIEWER_REWIND_STATS {\"captures\":%llu,\"evictions\":%llu,\"failures\":%llu,\"history_entries\":%u,\"history_bytes\":%llu,\"capture_ns_total\":%llu,\"capture_ns_max\":%llu,\"restores\":%llu,\"last_capture_bytes\":%llu,\"retained_span_ms\":%llu}\n",
            (unsigned long long)st.captures, (unsigned long long)st.evictions, (unsigned long long)st.capture_failures, (unsigned)st.history_entries, (unsigned long long)st.history_bytes,
            (unsigned long long)st.capture_ns_total, (unsigned long long)st.capture_ns_max, (unsigned long long)st.restores, (unsigned long long)st.last_capture_bytes, (unsigned long long)st.retained_span_ms);
  }
  sms_viewer_state_destroy(g_state);
  g_state = NULL;
  sms_sdl3_viewer_close(viewer);
  printf("viewer outcome %d frames %llu presented %llu audio_samples %llu audio_failures %llu resets %u\n", (int)result.outcome,
         (unsigned long long)result.frames, (unsigned long long)result.frames_presented,
         (unsigned long long)result.audio_samples, (unsigned long long)result.audio_failures, result.resets);
  if (options.record != NULL) {
    const size_t n = sms_viewer_format_script(&rig, script, sizeof script);
    FILE *f = fopen(options.record, "wb");
    if (f == NULL || (n != 0u && fwrite(script, 1, n, f) != n) || fclose(f) != 0) {
      fprintf(stderr, "cannot write %s\n", options.record);
      status = 64;
    }
  }
  if (result.outcome == SMS_VIEWER_GUEST_ERROR && result.stop.kind == SMS_STOP_Z80_ERROR)
    printf("z80_outcome %s pc %04X identity %u\n", z80_outcome_name(result.stop.z80_outcome), (unsigned)result.stop.pc, (unsigned)result.stop.image_identity);
  if (status != 0) return status;
  if (result.outcome == SMS_VIEWER_GUEST_ERROR) return sms_stop_is_unadmitted_pc(&result.stop) ? SMS_EXIT_Z80_UNADMITTED_PC : 3;
  return result.outcome == SMS_VIEWER_FRAME_LIMIT || result.outcome == SMS_VIEWER_QUIT ? 0 : 5;
}
