#ifndef SEGARECOMP_GENESIS_FRAME_EXTENSION_H
#define SEGARECOMP_GENESIS_FRAME_EXTENSION_H

/*
 * SEG-118 (ADR 0262): the presentation-only extended-surface contract between the live-frame producer and a viewer.
 *
 * An adapter (the Genesis viewer; later Sega CD) that wants Level 2 widescreen owns ONE `GenesisFrameExtension` with a preallocated RGB24 surface
 * and attaches it to `GenesisRasterCapture::extension`. When it sets `request`, the runtime calls `producer` while the VRAM journal and the
 * per-line registers/CRAM/VSRAM of the frame that just ended are still valid, and the producer writes real Plane A/B scenery to the left and right
 * of the native picture into `pixels`. The finished GenesisFrameArtifact is read-only input to that call and never changes; nothing here is
 * serialized, hashed or read by guest semantics. NULL extension / request == 0 is exactly the unextended behaviour (no extension code runs).
 *
 * Lifetime: `pixels` is owned by the adapter for the whole viewer run. The runtime/producer only write it during the publication call; the
 * result is stamped with the frame sequence the same call publishes, and a consumer must ignore a result whose sequence differs from the frame it is
 * presenting. The surface of a frame whose result is not AVAILABLE/PARTIAL is meaningless (it may hold partial writes).
 */
#include <stddef.h>
#include <stdint.h>

#include "checkpoint_evidence.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_EXT_MAX_SIDE 256U /* equals SV_EXT_MAX_SIDE in libs/viewer/sv_layout.h (the runtime does not depend on the viewer) */
#define GENESIS_EXT_MAX_WIDTH (GENESIS_FRAME_MAX_WIDTH + 2U * GENESIS_EXT_MAX_SIDE)
#define GENESIS_EXT_SURFACE_BYTES ((size_t)GENESIS_EXT_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT * 3U)

typedef enum GenesisExtensionStatus {
  GENESIS_EXT_NONE = 0,        /* not requested for this frame: no work was done */
  GENESIS_EXT_UNAVAILABLE = 1, /* requested but not produced (see reason); the surface is meaningless */
  GENESIS_EXT_PARTIAL = 2,     /* produced from real state, with the caveats in `flags` */
  GENESIS_EXT_AVAILABLE = 3    /* produced from real state with no caveat */
} GenesisExtensionStatus;

typedef enum GenesisExtensionReason {
  GENESIS_EXT_REASON_NONE = 0,
  GENESIS_EXT_REASON_INVALID_REQUEST = 1, /* no producer/surface, bad arguments */
  GENESIS_EXT_REASON_NO_PLAN = 2,         /* the adapter's plan callback is absent or asked for no extension */
  GENESIS_EXT_REASON_TARGET_TOO_SMALL = 3,
  GENESIS_EXT_REASON_NATIVE_GEOMETRY = 4, /* the native artifact and the raster state disagree, or an unsupported display mode */
  GENESIS_EXT_REASON_NO_RASTER = 5        /* extension needs the scanline-aware raster capture */
} GenesisExtensionReason;

/* Caveats of a PARTIAL result (bit mask). Each means "the extension is real VDP data but a part of it cannot be derived exactly". */
#define GENESIS_EXT_FLAG_WINDOW_EDGE 0x01U     /* the window plane covers a native edge on some rows: Plane A is omitted off-screen on those rows */
#define GENESIS_EXT_FLAG_COLUMN_VSCROLL 0x02U  /* per-column vertical scroll differs across columns: the nearest edge column's value is used */
#define GENESIS_EXT_FLAG_NO_VRAM_JOURNAL 0x04U /* no VRAM journal: mid-frame VRAM changes cannot be replayed (same limit as the native render) */

typedef struct GenesisExtensionResult {
  uint64_t sequence;   /* the frame sequence this result belongs to (stamped by the runtime at publication) */
  uint8_t status;      /* GenesisExtensionStatus */
  uint8_t reason;      /* GenesisExtensionReason (UNAVAILABLE) */
  uint8_t flags;       /* GENESIS_EXT_FLAG_* (PARTIAL) */
  uint8_t reserved;
  uint16_t native_w, native_h; /* the native picture the surface was built around */
  uint16_t side;       /* extension width in source pixels on EACH side; surface width = native_w + 2 * side */
  uint32_t pitch;      /* bytes per surface row */
} GenesisExtensionResult;

struct GenesisFrameExtension;
/* Fills `out` (status, reason, flags, geometry, pitch) and writes the surface; returns 0 for AVAILABLE/PARTIAL, nonzero for UNAVAILABLE. */
typedef int (*GenesisExtensionProducerFunction)(const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster,
                                                const GenesisFrameArtifact *native, const struct GenesisFrameExtension *target,
                                                GenesisExtensionResult *out);

typedef struct GenesisFrameExtension {
  /* Adapter-owned inputs, changed only between guest slices (the runtime reads `request` once per publication). */
  uint8_t request;                  /* 0 = no work; 1 = produce Level 2 scenery for the next completed frame */
  GenesisExtensionProducerFunction producer; /* normally genesis_vdp_produce_extension */
  /* Extension width per side for a native size (the adapter calls its layout math); returns nonzero for "no extension". */
  int (*plan)(void *ctx, unsigned native_w, unsigned native_h, unsigned *side_out);
  void *plan_ctx;
  uint8_t *pixels;                  /* RGB24, `capacity` bytes, owned by the adapter */
  size_t capacity;
  /* Runtime-written. */
  GenesisExtensionResult result;    /* the latest publication's result */
  uint64_t produced;                /* AVAILABLE/PARTIAL results */
  uint64_t failed;                  /* UNAVAILABLE results of requested frames (bounded counters; saturate at UINT64_MAX) */
} GenesisFrameExtension;

#ifdef __cplusplus
}
#endif
#endif
