#ifndef SEGARECOMP_VIEWER_SV_STATE_H
#define SEGARECOMP_VIEWER_SV_STATE_H

/*
 * SEG-131 (ADR 0278): the machine-independent capability interface of whole-machine save states and visual rewind.
 *
 * The shared viewer (libs/viewer) owns the menus, the slot lists, the confirmation pages, the quick-save / quick-load keys and the rewind surface; it never
 * reads a machine. A console adapter that CAN snapshot its machine supplies one SvStateProvider; one that cannot (Master System, Sega CD) supplies none, and the
 * menus then show accurate disabled rows. The provider is only ever called from the host's once-per-slice service point (sv_host_service), i.e. at the
 * adapter's snapshot barrier, while the guest is not running; thumbnails returned by it stay valid until the next provider call that changes state.
 *
 * Slot 0 is the quick slot; slots 1..SV_STATE_SLOTS-1 are the numbered ones.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SV_STATE_SLOTS 6
#define SV_STATE_THUMB_MAX_W 160
#define SV_STATE_THUMB_MAX_H 112

/* A borrowed RGB565 little-endian, row-major image. w == 0 means "none". */
typedef struct SvStateImage {
  int w, h;
  const uint8_t *rgb565;
} SvStateImage;

typedef struct SvStateSlotInfo {
  int occupied;       /* a valid state file is present */
  int damaged;        /* a file is present but unusable (corrupt, other game or other build): shown, not loadable */
  uint64_t guest_ms;  /* guest playtime at the capture (never wall-clock) */
  SvStateImage thumb; /* the frame of the snapshot itself */
  char note[64];      /* for a damaged slot: the reason */
} SvStateSlotInfo;

typedef struct SvRewindEntry {
  int64_t age_ms;     /* guest milliseconds before the present moment (> 0); the present itself is the host's NOW card */
  SvStateImage thumb;
} SvRewindEntry;

typedef struct SvStateProvider {
  void *ctx;
  /* Whole-machine save states. Return 1 when available; otherwise 0 and the reason (shown to the player) in `why`. */
  int (*states_available)(void *ctx, char *why, size_t why_cap);
  int (*slot_info)(void *ctx, int slot, SvStateSlotInfo *out);                 /* 0 on success */
  int (*save_slot)(void *ctx, int slot, char *error, size_t error_cap);        /* 0 on success; a failure never claims success */
  int (*load_slot)(void *ctx, int slot, char *error, size_t error_cap);        /* 0 on success; a failure leaves the running game unchanged */
  /* Visual rewind (any of these may be NULL when the adapter has none). */
  int (*rewind_available)(void *ctx, char *why, size_t why_cap);
  int (*history_count)(void *ctx);                                             /* entries older than now, oldest first (index 0 = oldest) */
  int (*history_info)(void *ctx, int index, SvRewindEntry *out);               /* 0 on success */
  int (*restore_entry)(void *ctx, int index, char *error, size_t error_cap);   /* 0: the machine now runs from that entry; failure leaves it unchanged */
  void (*rewind_opened)(void *ctx);                                            /* the rewind surface opened (guest frozen) */
  void (*rewind_cancelled)(void *ctx);                                         /* closed without a restore: nothing changed */
  int (*history_note)(void *ctx, char *note, size_t note_cap);                 /* non-zero + text when the retained history is shorter than the target (memory pressure) */
} SvStateProvider;

#ifdef __cplusplus
}
#endif
#endif
