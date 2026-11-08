#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_VIEWER_STATE_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_VIEWER_STATE_H

/*
 * SEG-132 (ADR 0279): the Master System adapter of the shared save-state service. It binds the SMS-owned codec (sms_snapshot.h) to the viewer rig and to the
 * machine-independent SvStateService (libs/viewer/include/sv_state_service.h): slot files, thumbnails taken from the frame the guest last published, and the bounded
 * in-memory rewind history. It runs at the viewer's frame barrier (the top of an `sms_viewer_run` iteration, in the host's poll) and never touches the SDL host.
 */
#include <stddef.h>
#include <stdint.h>

#include "sms_snapshot.h"
#include "sms_viewer.h"
#include "sv_state_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmsViewerStateConfig {
  SmsViewerRig *rig;
  SmsSnapshotIdentity identity;
  const char *state_dir;                   /* directory of the slot files; NULL = no slots */
  const char *unavailable_reason;          /* slots unavailable (no save location, no verified identity ...) */
  const char *rewind_unavailable_reason;   /* rewind unavailable (a recording session ...) */
  uint32_t rewind_budget_bytes;            /* 0 = off */
  uint32_t rewind_interval_ms, rewind_target_ms;
  /* Host side of a restore, still at the barrier: clear queued host audio, rebase pacing. (The rig itself -- ring, extension, recorded input, presentation baseline --
   * is reset by the adapter.) */
  void (*after_restore)(void *context);
  void *after_restore_context;
} SmsViewerStateConfig;

typedef struct SmsViewerState SmsViewerState;

SmsViewerState *sms_viewer_state_create(const SmsViewerStateConfig *config);
void sms_viewer_state_destroy(SmsViewerState *state);
void sms_viewer_state_provider(SmsViewerState *state, SvStateProvider *out);
/* Once per iteration at the barrier, before the host's poll: captures a rewind moment when a full interval of guest time has passed. */
void sms_viewer_state_after_slice(SmsViewerState *state);
void sms_viewer_state_stats(const SmsViewerState *state, SvStateServiceStats *out);

/* Diagnostics for the automated tests: SHA-256 of a fresh canonical capture (no thumbnail) of the running machine. Returns 0 on success. */
int sms_viewer_state_canonical_sha256(const SmsViewerState *state, uint8_t out[32]);

/* Pure helper (exposed for tests): the published frame (256 x height CRAM bytes) reduced to <= 160x112 RGB565. Returns the produced size or 0. */
int sms_viewer_state_make_thumbnail(const uint8_t *framebuffer, uint32_t height, uint8_t *rgb565_out, size_t capacity, uint16_t *width, uint16_t *height_out);

#ifdef __cplusplus
}
#endif
#endif
