#ifndef SEGARECOMP_VIEWER_GENESIS_EXECUTION_COVERAGE_H
#define SEGARECOMP_VIEWER_GENESIS_EXECUTION_COVERAGE_H

/*
 * SEG-026-T001: headless, frame-bounded execution-PC coverage run (measurement only).
 *
 * Runs the UNMODIFIED generated program one guest dispatch at a time through `genesis_runtime_step` (exactly
 * the loop `genesis_runtime_run` performs) until a fixed number of virtual frames has been published, the
 * dispatch allowance is exhausted, or the guest stops/completes. With `coverage_enabled` it attaches the
 * runtime's GenesisExecutionCoverage observer (complete retired-PC bitmap + first-entry witnesses); without it
 * the identical loop runs with the observer absent, so the two runs can be compared for zero semantic effect
 * and overhead. Frame publication uses the SEG-007-T255 live-frame observer with the normal renderer; the
 * frame ordinal is the only schedule input (no PC, address, label or game-state heuristic).
 *
 * Aggregates (counts, SHA-256 digests) are reported per epoch of `epoch_frames` published frames. The exact
 * PC bitmap and witnesses are written only to the caller's private directory; callers must keep that
 * directory ignored and never persist its contents.
 */
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_EXECUTION_COVERAGE_MAX_EPOCHS 256U
#define GENESIS_EXECUTION_COVERAGE_PATH_MAX 1024U

typedef enum GenesisExecutionCoverageOutcome {
  GENESIS_EXECUTION_COVERAGE_FRAMES_REACHED = 0,
  GENESIS_EXECUTION_COVERAGE_RUNNER_EXHAUSTED = 1,
  GENESIS_EXECUTION_COVERAGE_GUEST_STOP = 2,
  GENESIS_EXECUTION_COVERAGE_GUEST_COMPLETE = 3,
  GENESIS_EXECUTION_COVERAGE_INVALID_ARGUMENT = 4,
  GENESIS_EXECUTION_COVERAGE_IO_ERROR = 5
} GenesisExecutionCoverageOutcome;

typedef struct GenesisExecutionCoverageOptions {
  uint64_t target_frames;   /* > 0: stop once this many frames have been published */
  uint64_t epoch_frames;    /* > 0: aggregate checkpoint interval in published frames */
  int coverage_enabled;     /* 0: identical loop with no observer attached */
  uint64_t witness_capacity;
  const char *private_dir;  /* existing private directory (coverage_enabled only); NULL: write nothing */
} GenesisExecutionCoverageOptions;

typedef struct GenesisExecutionCoverageEpoch {
  uint64_t frame;
  uint64_t dispatches;
  uint64_t retirements;
  uint64_t distinct;
  uint8_t digest[32]; /* SHA-256 of the bitmap at this checkpoint */
} GenesisExecutionCoverageEpoch;

typedef struct GenesisExecutionCoverageResult {
  GenesisExecutionCoverageOutcome outcome;
  uint64_t frames_published;
  uint64_t dispatches;
  GenesisControlTransfer transfer; /* last step's transfer */
  GenesisExecutionCoverage coverage; /* counters (bitmap/witness pointers cleared on return) */
  uint32_t epoch_count;
  GenesisExecutionCoverageEpoch epochs[GENESIS_EXECUTION_COVERAGE_MAX_EPOCHS];
  uint8_t coverage_digest[32];     /* final bitmap SHA-256 (zero when disabled) */
  uint8_t frame_stream_digest[32]; /* SHA-256 over every published frame digest, in order */
  uint8_t final_state_digest[32];  /* SHA-256 over CPU registers, work RAM, device and scheduler state */
} GenesisExecutionCoverageResult;

/* `runtime->execution_coverage` and `runtime->live_frame_observer` must be NULL on entry and are NULL on return. */
void genesis_execution_coverage_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                    const GenesisExecutionCoverageOptions *options,
                                    uint64_t total_dispatch_allowance, GenesisExecutionCoverageResult *result);

#ifdef __cplusplus
}
#endif

#endif
