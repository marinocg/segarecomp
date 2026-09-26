#ifndef SEGARECOMP_VIEWER_GENESIS_FRAME_CAPTURE_H
#define SEGARECOMP_VIEWER_GENESIS_FRAME_CAPTURE_H

/*
 * SEG-021-T031: headless, bounded, deterministic live-frame capture.
 *
 * A host-side observation tool over the SEG-007-T255 live-frame observer: it runs the
 * UNMODIFIED generated program through `genesis_runtime_run` in dispatch slices (the same
 * slicing contract the SEG-007-T254 viewer core uses), and writes every frame the runtime
 * publishes whose 1-based publication ordinal is `first_frame + k * frame_stride` for
 * `0 <= k < frame_count` (stride 1 = a contiguous window) as a P6 PPM through the existing
 * `genesis_frame_export_ppm_to_path` (no second renderer, no second encoder).
 *
 * Selection rule: frame ordinals only (the Nth frame the runtime publishes at a genuine
 * virtual frame boundary). No PC, address, label, game-state or pixel heuristic takes part
 * in selection. Observation stops as soon as the window is complete (never by raising the
 * runner allowance); if the allowance ends or the guest stops first, the result reports the
 * shortfall and fails closed (`GENESIS_FRAME_CAPTURE_INCOMPLETE_*`).
 *
 * Attaching the capture does not alter execution: the producer wrapper only renders
 * already-current VDP state into host memory (exactly what the viewer does) and writes a
 * host file; guest state is never read back into or modified by the capture.
 *
 * The producer callback has no context parameter, so a capture run uses one file-scope
 * session; captures must not run concurrently (a host tool, never generated code).
 */
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_FRAME_CAPTURE_MAX_FRAMES 64U
#define GENESIS_FRAME_CAPTURE_PATH_MAX 1024U

typedef enum GenesisFrameCaptureOutcome {
  GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE = 0,
  GENESIS_FRAME_CAPTURE_INCOMPLETE_RUNNER_EXHAUSTED = 1,
  GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_STOP = 2,
  GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_COMPLETE = 3,
  GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT = 4,
  GENESIS_FRAME_CAPTURE_IO_ERROR = 5
} GenesisFrameCaptureOutcome;

typedef struct GenesisFrameCaptureOptions {
  uint64_t first_frame;       /* 1-based publication ordinal of the first captured frame (> 0) */
  uint32_t frame_count;       /* 1..GENESIS_FRAME_CAPTURE_MAX_FRAMES */
  uint32_t frame_stride;      /* >= 1: ordinal distance between captured frames */
  uint32_t slice_dispatches;  /* > 0: dispatches per genesis_runtime_run slice */
  const char *out_dir;        /* existing directory receiving frame-<ordinal>.ppm */
} GenesisFrameCaptureOptions;

typedef struct GenesisFrameCaptureResult {
  GenesisFrameCaptureOutcome outcome;
  uint64_t frames_published;  /* frames the runtime published during this capture run */
  uint32_t frames_captured;
  uint64_t dispatches;        /* lower bound when the final slice ended with a guest stop */
  GenesisControlTransfer transfer; /* the last slice's transfer */
  uint8_t digests[GENESIS_FRAME_CAPTURE_MAX_FRAMES][32]; /* frame_digest of each captured frame */
} GenesisFrameCaptureResult;

/* Runs until the window is complete, the allowance is exhausted, or the guest stops.
   `runtime->live_frame_observer` must be NULL on entry; it is restored to NULL on return. */
GenesisFrameCaptureResult genesis_frame_capture_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                    const GenesisFrameCaptureOptions *options,
                                                    uint64_t total_dispatch_allowance);

#ifdef __cplusplus
}
#endif

#endif
