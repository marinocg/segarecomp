#ifndef SEGARECOMP_VIEWER_GENESIS_VIEWER_H
#define SEGARECOMP_VIEWER_GENESIS_VIEWER_H

/*
 * SEG-007-T254: optional host-side Genesis viewer core. Owns ONLY host
 * cooperation slices, completed-frame presentation pacing, and window-close
 * polling. It never advances guest time, never reads wall time into guest
 * state, and never touches VDP/IRQ/device state: the only runtime mutation is
 * genesis_runtime_run() itself, which is identical to headless execution.
 * Clock, sleeper, and presenter are injected so tests use fakes and the
 * headless runtime never links this library.
 */
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GenesisViewerHost {
  void *ctx;
  uint64_t (*now_ns)(void *ctx);            /* monotonic clock */
  void (*sleep_ns)(void *ctx, uint64_t ns); /* may be NULL only if unthrottled */
  int (*present)(void *ctx, const GenesisFrameArtifact *frame); /* 0 == ok */
  int (*window_closed)(void *ctx);          /* pumps events; nonzero == closed */
  uint8_t (*pad1)(void *ctx);               /* optional GENESIS_PAD_* mask; NULL == released */
} GenesisViewerHost;

typedef struct GenesisViewerOptions {
  uint8_t unthrottled;       /* presentation policy only */
  uint32_t slice_dispatches; /* guest dispatches per host slice, > 0 */
} GenesisViewerOptions;

/* Fixed player-1 key map (SDL3 viewer): arrows = D-pad, Z = A, X = B, C = C,
 * Return = Start. Pure translation of held-key flags to a GENESIS_PAD_* mask,
 * kept free of SDL types so it is testable without a window. */
typedef struct GenesisViewerKeys {
  uint8_t up, down, left, right, a, b, c, start; /* nonzero == held */
} GenesisViewerKeys;
uint8_t genesis_viewer_pad_from_keys(const GenesisViewerKeys *keys);

#define GENESIS_VIEWER_DEFAULT_SLICE UINT32_C(20000)
/* A wake later than this many frame periods resynchronizes the schedule. */
#define GENESIS_VIEWER_MAX_LATE_PERIODS UINT64_C(4)

/* Parses viewer flags from argv. Recognized: --viewer-unthrottled,
 * --viewer-slice <n>. Unknown args are ignored (left to the caller).
 * Returns 0 on success, -1 on malformed/zero/overflowing option. */
int genesis_viewer_options_parse(int argc, const char *const *argv, GenesisViewerOptions *out);

/* Accumulated-deadline frame pacer with exact rational NTSC period. */
typedef struct GenesisPacer {
  uint64_t deadline_ns;
  uint64_t rem;
  uint64_t last_now_ns;
  uint8_t started;
  uint8_t unthrottled;
  uint32_t sleep_calls;
  uint32_t resyncs;
} GenesisPacer;

void genesis_pacer_init(GenesisPacer *pacer, int unthrottled);
uint64_t genesis_pacer_period_floor_ns(void);

/* Called when a completed frame is ready. Sleeps until the accumulated
 * deadline (unless unthrottled/late), then advances the deadline by one
 * period. Returns 0 on success, -1 (state unmodified) on null args or a
 * non-monotonic clock. */
int genesis_pacer_wait(GenesisPacer *pacer, const GenesisViewerHost *host);

typedef enum GenesisViewerOutcome {
  GENESIS_VIEWER_GUEST_STOP = 1,      /* guest GENESIS_STOP */
  GENESIS_VIEWER_GUEST_COMPLETE = 2,  /* guest GENESIS_COMPLETE */
  GENESIS_VIEWER_RUNNER_EXHAUSTED = 3,/* total dispatch allowance used up */
  GENESIS_VIEWER_WINDOW_CLOSED = 4,
  GENESIS_VIEWER_INVALID_ARGUMENT = 5,
  GENESIS_VIEWER_HOST_ERROR = 6       /* present/clock failure */
} GenesisViewerOutcome;

typedef struct GenesisViewerResult {
  GenesisViewerOutcome outcome;
  GenesisControlTransfer transfer; /* last runner transfer (guest stop detail) */
  uint64_t dispatches;             /* total guest dispatches executed */
  uint32_t slices;
  uint64_t frames_presented;
} GenesisViewerResult;

/* Runs `runtime` in bounded slices. `runtime->live_frame_observer` must be
 * set by the caller. Slice exhaustion is a resumable yield: it is never a
 * guest stop or runner exhaustion, and preserves exact runtime state. */
GenesisViewerResult genesis_viewer_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch,
                                       const GenesisViewerOptions *options,
                                       const GenesisViewerHost *host, GenesisPacer *pacer,
                                       uint64_t total_dispatch_allowance);

#ifdef __cplusplus
}
#endif
#endif
