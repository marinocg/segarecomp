#ifndef SEGARECOMP_VIEWER_SEGA_CD_SCD_VIEWER_STATE_H
#define SEGARECOMP_VIEWER_SEGA_CD_SCD_VIEWER_STATE_H

/*
 * SEG-132 (ADR 0279): the Sega CD adapter of the shared save-state service. It binds the Sega CD-owned codec (scd_snapshot.h) to the live machine and to the machine-independent
 * SvStateService (libs/viewer/include/sv_state_service.h): slot files, thumbnails taken from the frame the guest last published, and the bounded in-memory rewind history.
 * Every entry point runs at the live driver's frame barrier (inside ScdLiveHost::poll).
 */
#include <stddef.h>
#include <stdint.h>

#include "scd_runtime.h"
#include "sv_state_service.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GenesisFrameArtifact;

typedef struct ScdViewerStateConfig {
  ScdMachine *machine;
  const char *state_dir;                   /* directory of the slot files; NULL = no slots */
  const char *unavailable_reason;          /* slots unavailable (no save location ...) */
  const char *rewind_unavailable_reason;   /* rewind unavailable */
  uint32_t rewind_budget_bytes;            /* 0 = off */
  uint32_t rewind_interval_ms, rewind_target_ms;
  /* The host side of a restore, still at the barrier: drop queued host audio and the pending CD-DA frames, restart the audio presenter, rebase pacing and the
   * performance window. The machine itself (mixer queue, drive, processors) was restored by the codec. */
  void (*after_restore)(void *context);
  void *after_restore_context;
} ScdViewerStateConfig;

typedef struct ScdViewerState ScdViewerState;

ScdViewerState *scd_viewer_state_create(const ScdViewerStateConfig *config);
void scd_viewer_state_destroy(ScdViewerState *state);
void scd_viewer_state_provider(ScdViewerState *state, SvStateProvider *out);
/* The frame the machine last published (valid until the machine next runs): the picture a thumbnail shows. */
void scd_viewer_state_note_frame(ScdViewerState *state, const struct GenesisFrameArtifact *frame);
/* Once per iteration at the barrier, before the host's poll: captures a rewind moment when a full interval of guest time has passed. */
void scd_viewer_state_after_slice(ScdViewerState *state);
void scd_viewer_state_stats(const ScdViewerState *state, SvStateServiceStats *out);
/* Diagnostics for the automated tests: SHA-256 of a fresh canonical capture (no thumbnail). 0 on success. */
int scd_viewer_state_canonical_sha256(const ScdViewerState *state, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
