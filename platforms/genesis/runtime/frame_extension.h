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
#include <string.h>

#include "checkpoint_evidence.h"
#include "native_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_EXT_MAX_SIDE 672U /* SEG-139: equals SV_EXT_MAX_SIDE in libs/viewer/sv_layout.h (the runtime does not depend on the viewer); 32:9 of the widest Genesis picture needs 668 */
#define GENESIS_EXT_MAX_WIDTH (GENESIS_FRAME_MAX_WIDTH + 2U * GENESIS_EXT_MAX_SIDE)
#define GENESIS_EXT_SURFACE_BYTES ((size_t)GENESIS_EXT_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT * 3U)

/* SEG-121: what the adapter asks for (`request`) and what a frame actually contains (`result.mode`). A completed frame is Level 3 only when its own
 * result says so; the saved preference never decides that. Level 2 callers (request == 1) are unchanged. */
typedef enum GenesisExtensionMode {
  GENESIS_EXT_MODE_NONE = 0,
  GENESIS_EXT_MODE_BACKGROUNDS = 1, /* Level 2: Plane A/B scenery and backdrop only */
  GENESIS_EXT_MODE_SPRITES = 2      /* Level 3: Level 2 plus sprites the frame's own SAT state places over the extra columns */
} GenesisExtensionMode;

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
  GENESIS_EXT_REASON_NO_RASTER = 5,       /* extension needs the scanline-aware raster capture */
  GENESIS_EXT_REASON_SPRITE_STATE = 6     /* only in `sprite_reason`: a Level 3 request was served as Level 2 because a line's sprite state was invalid/ambiguous */
} GenesisExtensionReason;

/* Caveats of a PARTIAL result (bit mask). Each means "the extension is real VDP data but a part of it cannot be derived exactly". */
#define GENESIS_EXT_FLAG_WINDOW_EDGE 0x01U     /* the window plane covers a native edge on some rows: Plane A is omitted off-screen on those rows */
#define GENESIS_EXT_FLAG_COLUMN_VSCROLL 0x02U  /* per-column vertical scroll differs across columns: the nearest edge column's value is used */
#define GENESIS_EXT_FLAG_NO_VRAM_JOURNAL 0x04U /* no VRAM journal: mid-frame VRAM changes cannot be replayed (same limit as the native render) */
#define GENESIS_EXT_FLAG_SPRITES_BEST_EFFORT 0x08U /* Level 3: sprites appear only where the game's own SAT places them; a generic producer never reports Available */

typedef struct GenesisExtensionResult {
  uint64_t sequence;   /* the frame sequence this result belongs to (stamped by the runtime at publication) */
  uint8_t status;      /* GenesisExtensionStatus */
  uint8_t reason;      /* GenesisExtensionReason (UNAVAILABLE) */
  uint8_t flags;       /* GENESIS_EXT_FLAG_* (PARTIAL) */
  uint8_t mode;        /* GenesisExtensionMode actually produced (AVAILABLE/PARTIAL only): the identity of the frame's content */
  uint16_t native_w, native_h; /* the native picture the surface was built around */
  uint16_t side;       /* extension width in source pixels on EACH side; surface width = native_w + 2 * side */
  uint8_t sprite_reason; /* a Level 3 request served as Level 2: GENESIS_EXT_REASON_SPRITE_STATE; else 0 */
  uint8_t reserved;
  uint32_t pitch;      /* bytes per surface row */
  uint32_t sprite_pixels; /* Level 3: visible sprite pixels in the extra columns of this frame (0 = none placed there; not a failure) */
  /* SEG-124 (ADR 0267): what a verified native compatibility provider contributed to THIS frame. All zero without a provider or when it declined the frame. */
  uint8_t compat_accepted; /* GENESIS_COMPAT_* components served for this frame */
  uint8_t compat_reason;   /* GenesisCompatReason when a provider was attached but declined (or partially failed) the frame */
  uint8_t authored;        /* 1 = both sides' scenery came from the provider AND nothing else degraded the frame: the only case presented as authored */
  uint8_t reserved2;
  uint16_t overlay_w, overlay_h; /* HUD overlay in the adapter's overlay buffer (0 = none) */
  uint16_t overlay_fx, overlay_fy; /* where the host draws it inside the native picture when the visible side cannot hold it */
} GenesisExtensionResult;

struct GenesisFrameExtension;
/* Fills `out` (status, reason, flags, geometry, pitch) and writes the surface; returns 0 for AVAILABLE/PARTIAL, nonzero for UNAVAILABLE. */
typedef int (*GenesisExtensionProducerFunction)(const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster,
                                                const GenesisFrameArtifact *native, const struct GenesisFrameExtension *target,
                                                GenesisExtensionResult *out);

typedef struct GenesisFrameExtension {
  /* Adapter-owned inputs, changed only between guest slices (the runtime reads `request` once per publication). */
  uint8_t request;                  /* GenesisExtensionMode: 0 = no work; 1 = Level 2 scenery; 2 = Level 3 (scenery + sprites) for the next completed frame */
  GenesisExtensionProducerFunction producer; /* normally genesis_vdp_produce_extension */
  /* Extension width per side for a native size (the adapter calls its layout math); returns nonzero for "no extension". */
  int (*plan)(void *ctx, unsigned native_w, unsigned native_h, unsigned *side_out);
  void *plan_ctx;
  uint8_t *pixels;                  /* RGB24, `capacity` bytes, owned by the adapter */
  size_t capacity;
  /* SEG-124: the optional verified native compatibility provider (NULL = none = exactly the previous behaviour). Selected statically by the build from a verified
   * package; `compat_state` is the provider's own storage. `compat_ram` (GENESIS_COMPAT_RAM_BYTES, adapter-owned) receives a work-RAM snapshot at every publication
   * and `compat_ram_stamp` the virtual frame counter it was taken at; the runtime sets `compat_ram_valid` for the frame being produced only when the stamp is exactly
   * one frame old. `overlay_rgba` (GENESIS_COMPAT_OVERLAY_BYTES, adapter-owned) receives the HUD. */
  const GenesisNativeCompat *compat;
  void *compat_state;
  unsigned compat_components;       /* GENESIS_COMPAT_* components the host will use (derived from the granted capabilities) */
  unsigned compat_caps;             /* GENESIS_CAP_* the verified package declared and the host grants */
  uint8_t *compat_ram;
  uint64_t compat_ram_stamp;
  uint8_t compat_ram_valid;
  uint8_t *overlay_rgba;
  /* Runtime-written. */
  GenesisExtensionResult result;    /* the latest publication's result */
  uint64_t produced;                /* AVAILABLE/PARTIAL results */
  uint64_t failed;                  /* UNAVAILABLE results of requested frames (bounded counters; saturate at UINT64_MAX) */
} GenesisFrameExtension;

/* SEG-119: the one dispatch around the producer, shared by every frame path that publishes an extension (the Genesis live frame and the Sega CD
 * presentation frame). Header-only so a translation unit that links no producer (tests compile runtime.c alone) needs nothing new. `ext` NULL or not
 * requested -> NONE, no work at all; requested but not produced -> UNAVAILABLE with a typed reason; else the producer's PARTIAL/AVAILABLE result.
 * Writes nothing but `*out` and the adapter's own surface; the caller stamps the sequence. */
static inline void genesis_frame_extension_run(const GenesisFrameExtension *ext, const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster,
                                               const GenesisFrameArtifact *native, GenesisExtensionResult *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  if (ext == NULL || ext->request == 0U) {
    out->status = GENESIS_EXT_NONE;
    return;
  }
  if (ext->producer == NULL || ext->pixels == NULL) {
    out->status = GENESIS_EXT_UNAVAILABLE;
    out->reason = GENESIS_EXT_REASON_INVALID_REQUEST;
  } else if (ext->producer(vram, raster, native, ext, out) != 0 ||
             (out->status != GENESIS_EXT_PARTIAL && out->status != GENESIS_EXT_AVAILABLE)) {
    const uint8_t reason = out->status == GENESIS_EXT_UNAVAILABLE && out->reason != 0U ? out->reason : (uint8_t)GENESIS_EXT_REASON_INVALID_REQUEST;
    memset(out, 0, sizeof(*out));
    out->status = GENESIS_EXT_UNAVAILABLE;
    out->reason = reason;
  } else if (out->mode == GENESIS_EXT_MODE_NONE) {
    out->mode = GENESIS_EXT_MODE_BACKGROUNDS; /* a Level 2-only producer predating the typed mode */
  }
  if (out->status == GENESIS_EXT_PARTIAL || out->status == GENESIS_EXT_AVAILABLE) {
    /* A frame can never contain more than was asked for, and Level 3 content is never Available (a generic producer cannot know the game submits objects). */
    if (out->mode > ext->request || out->mode > GENESIS_EXT_MODE_SPRITES || (out->mode == GENESIS_EXT_MODE_SPRITES && out->status != GENESIS_EXT_PARTIAL)) {
      memset(out, 0, sizeof(*out));
      out->status = GENESIS_EXT_UNAVAILABLE;
      out->reason = GENESIS_EXT_REASON_INVALID_REQUEST;
    }
  }
}

#ifdef __cplusplus
}
#endif
#endif
