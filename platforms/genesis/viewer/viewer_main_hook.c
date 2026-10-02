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

#include "runtime.h"
#include "vdp_render.h"
#include "viewer.h"
#include "viewer_sdl3.h"
#ifdef SEGARECOMP_GENESIS_SOUND
#include "genesis_audio_present.h" /* SEG-032-T009: the shared mixer is presented, never advanced, by the viewer */
#include "genesis_sound.h"         /* SEG-032-T008: the sound-carrying link set of `segarecomp build` */
#endif

GenesisControlTransfer genesis_viewer_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                               uint32_t dispatch_allowance);

static GenesisFrameArtifact g_latest_frame;

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

GenesisControlTransfer genesis_viewer_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                               uint32_t dispatch_allowance) {
  const char *argv[3];
  int argc = 0;
  const char *unthrottled = getenv("SEGARECOMP_VIEWER_UNTHROTTLED");
  const char *slice = getenv("SEGARECOMP_VIEWER_SLICE");
  GenesisViewerOptions options;
  if (unthrottled != NULL && strcmp(unthrottled, "1") == 0) argv[argc++] = "--viewer-unthrottled";
  if (slice != NULL && *slice != '\0') { argv[argc++] = "--viewer-slice"; argv[argc++] = slice; }
  if (genesis_viewer_options_parse(argc, argv, &options) != 0) {
    fprintf(stderr, "viewer: malformed viewer options\n");
    exit(3);
  }
#ifdef SEGARECOMP_GENESIS_SOUND
  (void)genesis_sound_attach(runtime); /* the Z80 machine and the shared PSG/YM2612, identical to the headless program */
#endif
  GenesisSdl3Viewer *sdl = genesis_sdl3_viewer_open("segarecomp Genesis viewer", 2);
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

  GenesisViewerHost host;
  GenesisPacer pacer;
  genesis_sdl3_viewer_host(sdl, &host);
#ifdef SEGARECOMP_GENESIS_SOUND
  {
    const char *mute = getenv("SEGARECOMP_VIEWER_MUTE");
    const int muted = mute != NULL && strcmp(mute, "1") == 0;
    /* A failed open, a missing audio subsystem or mute leaves the viewer silent; the ring is still drained. */
    genesis_audio_presenter_init(&g_presenter, muted ? NULL : genesis_sdl3_viewer_audio_open(sdl), muted);
    host.after_slice = viewer_after_slice;
  }
#endif
  genesis_pacer_init(&pacer, options.unthrottled);
  GenesisViewerResult r = genesis_viewer_run(runtime, dispatch, &options, &host, &pacer,
                                                dispatch_allowance == UINT32_MAX ? UINT64_MAX : dispatch_allowance);
  runtime->live_frame_observer = NULL;
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
