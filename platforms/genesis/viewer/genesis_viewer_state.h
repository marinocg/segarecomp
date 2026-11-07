/*
 * SEG-131 (ADR 0278): the Genesis adapter's save-state service. It owns the slot files (`quick.sgstate`, `slot-1.sgstate` ... `slot-5.sgstate` in the session's
 * state directory), the thumbnails taken from the frame the guest last published, and -- from Checkpoint E -- the bounded in-memory rewind timeline. It exposes
 * all of it to the shared viewer through the machine-independent SvStateProvider (libs/viewer/include/sv_state.h); the viewer never sees a GenesisRuntime.
 *
 * Every entry point runs at the snapshot barrier (the viewer's once-per-slice service point, see docs/architecture/genesis-save-states-persistence-and-rewind.md
 * section 2): the guest is not executing and no host callback is mid-flight. Plain C11 apart from the SvStateProvider type.
 */
#ifndef SEGARECOMP_GENESIS_VIEWER_STATE_H
#define SEGARECOMP_GENESIS_VIEWER_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "genesis_snapshot.h"
#include "sv_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rewind policy defaults (docs: section 7). The interval is GUEST time. */
#define GENESIS_REWIND_INTERVAL_MS 10000U   /* one snapshot every 10 s of guest time ... */
#define GENESIS_REWIND_TARGET_MS 600000U    /* ... for 10 minutes: 60 moments */
#define GENESIS_REWIND_BUDGET_BYTES (96U * 1024U * 1024U)

typedef struct GenesisViewerStateConfig {
  GenesisSnapshotMachine machine;
  const GenesisFrameArtifact *latest_frame;   /* the newest published frame (thumbnails); may be NULL until one exists */
  GenesisSnapshotIdentity identity;
  const char *state_dir;                      /* directory of the slot files; NULL = no slots */
  const char *unavailable_reason;             /* shown for every state feature when the service cannot work (no identity, no save location, ...) */
  /* Called after every successful restore (slot or rewind), still at the barrier: the adapter resets everything that belonged to the abandoned future
   * (host audio queue, pacing, frame baseline, input latches). */
  void (*after_restore)(void *context);
  void *after_restore_context;
  /* Rewind: budget 0 disables it. `interval_ms` and `target_ms` are guest milliseconds. */
  uint32_t rewind_budget_bytes;
  uint32_t rewind_interval_ms;
  uint32_t rewind_target_ms;
} GenesisViewerStateConfig;

typedef struct GenesisViewerState GenesisViewerState;

GenesisViewerState *genesis_viewer_state_create(const GenesisViewerStateConfig *config);
void genesis_viewer_state_destroy(GenesisViewerState *state);

/* The provider the viewer host registers (valid for the life of `state`). */
void genesis_viewer_state_provider(GenesisViewerState *state, SvStateProvider *out);

/* Called once per slice at the barrier, after the frame/audio of the slice were presented: captures a rewind snapshot when a full interval of guest time has
 * passed since the last one. Does nothing when rewind is disabled or unavailable. Never touches the disk. */
void genesis_viewer_state_after_slice(GenesisViewerState *state);

/* Counters for tests and diagnostics. */
typedef struct GenesisViewerStateStats {
  uint32_t history_entries;
  uint64_t history_bytes;
  uint64_t captures;
  uint64_t evictions;
  uint64_t capture_failures;
  uint64_t restores;
  uint64_t last_capture_ns_unused;
} GenesisViewerStateStats;
void genesis_viewer_state_stats(const GenesisViewerState *state, GenesisViewerStateStats *out);

/* Pure helpers (exposed for tests). `rgb565_out` receives at most 160x112 pixels; returns the produced size or 0. */
int genesis_viewer_state_make_thumbnail(const GenesisFrameArtifact *frame, uint8_t *rgb565_out, size_t capacity, uint16_t *width, uint16_t *height);
/* Guest milliseconds of a master-tick count (NTSC master clock). */
uint64_t genesis_viewer_state_ticks_to_ms(uint64_t master_ticks);

#ifdef __cplusplus
}
#endif
#endif
