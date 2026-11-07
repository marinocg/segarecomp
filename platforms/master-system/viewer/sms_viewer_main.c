/* Interactive Master System viewer: the generated program's `main` when built as a viewer (SEG-009-T009; ADR 0070).
 *
 *   <game> [--viewer-unthrottled] [--viewer-mute] [--viewer-frames <n>] [--viewer-scale <n>] [--viewer-record <file>]
 *
 * Keys (rebindable in the pause menu): arrows = D-pad, Z = button 1, X = button 2, P = the console Pause button; Esc / gamepad Back opens the
 * host pause menu (Resume, Quick Settings, Video, Controls, Audio, System incl. Reset, Return/Quit); window close = quit. `--viewer-record` writes the
 * input stream the run consumed as a headless `--input` script (the run since the last reset). Exit: 0 quit or frame
 * limit; 3 guest stop; 4 SMS_ERROR at start; 5 viewer/host failure; 64 usage. The ROM is the build-time embedded array. */
#include <stdio.h>
#include <string.h>

#include "sms_viewer_sdl3.h"
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

static uint64_t perf_clock(void) { return g_inner.now_ns(g_inner.ctx); }
static void emulation_done(void) {
  if (g_emu_t0 != 0U) sv_perf_span_end(&g_perf, SV_SPAN_EMULATION, g_emu_t0);
  g_emu_t0 = 0U;
}
static int perf_poll(void *ctx, SmsViewerInput *in) {
  const int rc = g_inner.poll(ctx, in);
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
