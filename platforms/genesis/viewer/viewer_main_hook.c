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

GenesisControlTransfer genesis_viewer_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                               uint32_t dispatch_allowance);

static GenesisFrameArtifact g_latest_frame;

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
  genesis_pacer_init(&pacer, options.unthrottled);
  GenesisViewerResult r = genesis_viewer_run(runtime, dispatch, &options, &host, &pacer, dispatch_allowance);
  runtime->live_frame_observer = NULL;
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
    t.runner_dispatch_count = (uint32_t)(r.dispatches > UINT32_MAX ? UINT32_MAX : r.dispatches);
    return t;
  }
  return r.transfer;
}
