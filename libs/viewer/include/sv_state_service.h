#ifndef SEGARECOMP_VIEWER_SV_STATE_SERVICE_H
#define SEGARECOMP_VIEWER_SV_STATE_SERVICE_H

/*
 * SEG-132 (ADR 0279): the platform-neutral save-state service. It is the generic implementation of the shared viewer's SvStateProvider (sv_state.h) for a console that
 * owns its own complete state codec: the six slot files (`quick.sgstate`, `slot-1.sgstate` ... `slot-5.sgstate`), the thumbnails and guest-time stamps taken from the
 * containers themselves, and the bounded in-memory rewind history (cadence in GUEST milliseconds, byte budget, age limit, oldest-first eviction, branch truncation on a
 * restore, a note about the real retained span, no disk writes).
 *
 * It knows nothing about any machine. The console supplies six callbacks (SvStateOps); every one runs at the adapter's snapshot barrier, while the guest is not
 * executing. The Genesis adapter (SEG-131) keeps its own earlier, equivalent copy; the Master System and Sega CD adapters share this one.
 */
#include <stddef.h>
#include <stdint.h>

#include "sv_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults of the rewind policy (the same product defaults as Genesis). The interval and the target are GUEST time. */
#define SV_STATE_REWIND_INTERVAL_MS 500U
#define SV_STATE_REWIND_TARGET_MS 60000U
#define SV_STATE_REWIND_BUDGET_BYTES (96U * 1024U * 1024U)

typedef struct SvStateInfo {
  uint64_t guest_ms;            /* guest playtime at the capture */
  uint16_t thumb_w, thumb_h;
  const uint8_t *thumb;         /* RGB565, refers into the inspected bytes; NULL when none */
} SvStateInfo;

enum { SV_CAPTURE_OK = 0, SV_CAPTURE_FAILED = 1, SV_CAPTURE_SKIP = 2, SV_CAPTURE_NO_MEMORY = 3 };

typedef struct SvStateOps {
  void *ctx;
  /* Builds a complete container (thumbnail included) into a malloc'd buffer the service owns afterwards. SV_CAPTURE_SKIP: the machine is in a latched stop (quietly
   * skipped for rewind, reported for a manual save); SV_CAPTURE_NO_MEMORY: shorten the history; anything else: rewind stops with `why`. */
  int (*capture)(void *ctx, uint8_t **data, uint32_t *size, char *why, size_t cap);
  /* Structure + integrity + header only (no machine needed). 0 = ok. */
  int (*peek)(void *ctx, const uint8_t *data, size_t size, SvStateInfo *info);
  /* Everything restore decides before it commits, for THIS machine (identity, compatibility, staged value validation). 0 = restore would succeed; else `why`. */
  int (*validate)(void *ctx, const uint8_t *data, size_t size, SvStateInfo *info, char *why, size_t cap);
  /* Two-phase restore. 0 = the machine now runs from the state; on failure the machine is unchanged and `why` says why. */
  int (*restore)(void *ctx, const uint8_t *data, size_t size, char *why, size_t cap);
  /* After every successful restore, still at the barrier: drop everything that belonged to the abandoned future (host audio, pacing, frame baseline, extension). */
  void (*after_restore)(void *ctx);
  /* The current guest playtime in milliseconds (never wall-clock). */
  uint64_t (*guest_ms)(void *ctx);
} SvStateOps;

typedef struct SvStateServiceConfig {
  SvStateOps ops;
  const char *state_dir;            /* directory of the slot files; NULL = no slots */
  const char *unavailable_reason;   /* shown for every state feature when the service cannot work (no identity, no save location, a recording session ...) */
  size_t max_file_bytes;            /* the largest container accepted from disk */
  uint32_t rewind_budget_bytes;     /* 0 disables rewind */
  uint32_t rewind_interval_ms;      /* 0 = default */
  uint32_t rewind_target_ms;        /* 0 = default */
  const char *rewind_unavailable_reason;   /* non-NULL: states work but rewind does not (and this is why) */
} SvStateServiceConfig;

typedef struct SvStateService SvStateService;

typedef struct SvStateServiceStats {
  uint32_t history_entries;
  uint64_t history_bytes;
  uint64_t captures, evictions, capture_failures, restores;
  uint64_t capture_ns_total, capture_ns_max;   /* host time spent inside captures, for measurement only */
  uint64_t retained_span_ms;                   /* guest time between the oldest retained moment and now */
  uint64_t last_capture_bytes;
} SvStateServiceStats;

SvStateService *sv_state_service_create(const SvStateServiceConfig *config);
void sv_state_service_destroy(SvStateService *service);
/* The provider the viewer host registers (valid for the life of `service`). */
void sv_state_service_provider(SvStateService *service, SvStateProvider *out);
/* Once per slice at the barrier, BEFORE the host's service point: captures a rewind moment when a full interval of guest time has passed. Never touches the disk. */
void sv_state_service_after_slice(SvStateService *service);
void sv_state_service_stats(const SvStateService *service, SvStateServiceStats *out);

/* Pure helper: nearest-neighbour reduction of an RGB888 row source is console-specific; this one fits (src_w x src_h) inside 160x112 keeping the aspect. */
void sv_state_thumb_size(int src_w, int src_h, int *out_w, int *out_h);
/* RGB565 little-endian pixel from 8-bit components. */
static inline void sv_state_put_rgb565(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  dst[0] = (uint8_t)(v & 0xFFU);
  dst[1] = (uint8_t)(v >> 8);
}

#ifdef __cplusplus
}
#endif
#endif
