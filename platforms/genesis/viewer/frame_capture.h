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
 *
 * SEG-068-T007: an optional, deterministic, frame-keyed PAD SCRIPT. Each entry names a virtual frame number
 * (`runtime->scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_FRAME`, guest time, never a host clock), a player (1 or 2)
 * and the host button mask for `genesis_runtime_set_pad1/2`. Entries are applied ONLY between runner slices, at the first slice
 * boundary at or after the keyed frame, in script order (the last entry for a player wins). The script touches nothing but the
 * host pad mask: emulated time, scheduling, interrupts and every other guest-visible state are those of the same run without a
 * script, so the same script + the same slice size reproduces identical frame digests, and a run whose script is empty or never
 * reached is byte-identical to a run without one. Slice granularity: a key is honoured at slice resolution
 * (`slice_dispatches` dispatches), not at an arbitrary instruction; a smaller slice moves the application point earlier.
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

#define GENESIS_PAD_SCRIPT_MAX_ENTRIES 4096U

typedef struct GenesisPadScriptEntry {
  uint64_t frame;  /* virtual frame number at/after which the mask applies */
  uint8_t player;  /* 1 or 2 */
  uint8_t mask;    /* GENESIS_PAD_* bits */
} GenesisPadScriptEntry;

typedef struct GenesisFrameCaptureOptions {
  uint64_t first_frame;       /* 1-based publication ordinal of the first captured frame (> 0) */
  uint32_t frame_count;       /* 1..GENESIS_FRAME_CAPTURE_MAX_FRAMES */
  uint32_t frame_stride;      /* >= 1: ordinal distance between captured frames */
  uint32_t slice_dispatches;  /* > 0: dispatches per genesis_runtime_run slice */
  const char *out_dir;        /* existing directory receiving frame-<ordinal>.ppm */
  const GenesisPadScriptEntry *pad_script; /* optional (NULL = none): non-decreasing `frame`, see the header comment */
  uint32_t pad_script_count;               /* <= GENESIS_PAD_SCRIPT_MAX_ENTRIES */
} GenesisFrameCaptureOptions;

typedef struct GenesisFrameCaptureResult {
  GenesisFrameCaptureOutcome outcome;
  uint64_t frames_published;  /* frames the runtime published during this capture run */
  uint32_t frames_captured;
  uint64_t dispatches;        /* lower bound when the final slice ended with a guest stop */
  GenesisControlTransfer transfer; /* the last slice's transfer */
  uint64_t render_rejections;      /* SEG-055: virtual frame boundaries whose render failed closed */
  uint64_t vram_journal_overflows; /* SEG-055: frames whose VRAM journal overflowed (frame-end image used) */
  uint32_t pad_entries_applied;    /* SEG-068-T007: pad-script entries applied before the run ended */
  uint8_t digests[GENESIS_FRAME_CAPTURE_MAX_FRAMES][32]; /* frame_digest of each captured frame */
} GenesisFrameCaptureResult;

/* Runs until the window is complete, the allowance is exhausted, or the guest stops.
   `runtime->live_frame_observer` must be NULL on entry; it is restored to NULL on return. */
GenesisFrameCaptureResult genesis_frame_capture_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                    const GenesisFrameCaptureOptions *options,
                                                    uint64_t total_dispatch_allowance);

/* SEG-068-T007: nonzero when the capture environment (SEGARECOMP_CAPTURE_DIR) is present. A hook that owns another run loop
   (the sound hook) delegates to genesis_frame_capture_run_from_environment when it is. */
int genesis_frame_capture_requested(void);

/* The environment-driven entry used by every generated-main hook: parses SEGARECOMP_CAPTURE_DIR / _FIRST / _COUNT / _STRIDE /
   _SLICE and the optional SEGARECOMP_PAD_SCRIPT ("<frame>:<player>:<mask>[,...]", mask decimal or 0x-hex), runs
   genesis_frame_capture_run, prints one CAPTURE_SUMMARY line on stderr and returns the transfer the generated main expects
   (a malformed option exits 3 before the run). */
GenesisControlTransfer genesis_frame_capture_run_from_environment(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                                                  uint32_t dispatch_allowance);

#ifdef __cplusplus
}
#endif

#endif
