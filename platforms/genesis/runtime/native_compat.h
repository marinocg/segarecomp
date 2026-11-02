#ifndef SEGARECOMP_GENESIS_NATIVE_COMPAT_H
#define SEGARECOMP_GENESIS_NATIVE_COMPAT_H

/*
 * SEG-124 (ADR 0267): the generic, game-independent interface of a verified native compatibility provider.
 *
 * A provider is a statically compiled component (never loaded, never supplied by a package; a package only SELECTS one of the audited
 * capabilities the build knows) that turns a bounded, read-only snapshot of guest work RAM into presentation data for the extended surface:
 *   - the authentic name-table word of the world cell beneath an off-screen column (the generic VDP code still decodes patterns, palettes, flips,
 *     priorities and the sprite layer from the frame's own VRAM/CRAM/SAT), and
 *   - a small RGBA overlay (the HUD) the viewer draws after side treatment, only into visible side space.
 * It never writes guest state, never runs guest code, never changes CPU/VDP/Z80 semantics and never supplies hardware state. Every frame is validated by
 * `prepare` first; a frame the provider does not accept is produced by the generic path, never half by each.
 *
 * Frame synchronization: the frame is published at the VBlank onset, before the VBlank handler runs, so the state the game used to prepare the displayed frame
 * is the work RAM at the PREVIOUS onset. The runtime snapshots work RAM right after each publication (`compat_ram`, adapter-owned, stamped with the virtual
 * frame counter) and offers it to the provider only when the stamp is exactly one frame old.
 */
#include <stddef.h>
#include <stdint.h>

#include "checkpoint_evidence.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_NATIVE_COMPAT_ABI 1U
#define GENESIS_COMPAT_RAM_BYTES 65536U
#define GENESIS_COMPAT_OVERLAY_MAX_W 128U
#define GENESIS_COMPAT_OVERLAY_MAX_H 256U
#define GENESIS_COMPAT_OVERLAY_BYTES (GENESIS_COMPAT_OVERLAY_MAX_W * GENESIS_COMPAT_OVERLAY_MAX_H * 4U)

/* Component bits; identical to segarecomp::patch::kNativeComponent* (libs/media/include/segarecomp/patch.hpp). */
#define GENESIS_COMPAT_HUD 1U
#define GENESIS_COMPAT_SCENERY 2U

/* Why a provider declined a frame (diagnostic only; a declined frame is served by the generic path with its own accurate status). */
typedef enum GenesisCompatReason {
  GENESIS_COMPAT_OK = 0,
  GENESIS_COMPAT_STALE_STATE = 1,    /* no snapshot taken exactly one frame earlier */
  GENESIS_COMPAT_UNSUPPORTED_SCENE = 2, /* a game mode, zone or screen this provider is not verified for */
  GENESIS_COMPAT_BAD_STATE = 3,      /* the game state is inconsistent or out of range */
  GENESIS_COMPAT_BAD_SCROLL = 4,     /* the VDP scroll/plane configuration is not the verified one, or disagrees with the camera */
  GENESIS_COMPAT_FAULT = 5,          /* the provider failed to answer for a cell it accepted (never expected; fails the frame's authored claim) */
  GENESIS_COMPAT_MISMATCH = 6        /* the frame's own picture (the VDP nametables in view) disagrees with the world state: a fade, a transition or another level */
} GenesisCompatReason;

typedef struct GenesisCompatFrame {
  const uint8_t *work_ram;              /* GENESIS_COMPAT_RAM_BYTES, read-only, the state at the previous VBlank onset */
  const GenesisRasterFrameState *raster; /* per-line registers/CRAM/VSRAM of the frame */
  const uint8_t *vram;                  /* GENESIS_VDP_VRAM_BYTES: the VRAM as the frame's first line saw it (scroll tables) */
  unsigned width, height, side;
  unsigned requested;                   /* GENESIS_COMPAT_* components the adapter/descriptor enabled */
} GenesisCompatFrame;

typedef struct GenesisCompatDecision {
  unsigned accepted;                    /* GENESIS_COMPAT_* components the provider will serve for THIS frame (subset of requested) */
  uint8_t reason;                       /* GenesisCompatReason when nothing was accepted */
} GenesisCompatDecision;

typedef struct GenesisCompatOverlay {
  uint8_t *rgba;                        /* adapter-owned, GENESIS_COMPAT_OVERLAY_BYTES */
  size_t capacity;
  uint16_t width, height;               /* written by the provider; 0 = nothing to draw */
  uint16_t fallback_x, fallback_y;      /* picture pixels where the overlay's top-left sits when the host draws it inside the picture (no visible side): the place of the HUD it replaces */
} GenesisCompatOverlay;

typedef struct GenesisNativeCompat {
  uint32_t abi_version;                 /* GENESIS_NATIVE_COMPAT_ABI */
  const char *capability;               /* e.g. "genesis.sonic1.native-widescreen.v1" */
  uint32_t implementation_version;
  unsigned components;                  /* GENESIS_COMPAT_* the implementation can serve */
  /* Validates every input the frame will use; accepts a subset of `frame->requested` (0 = decline the whole frame). */
  void (*prepare)(void *state, const GenesisCompatFrame *frame, GenesisCompatDecision *out);
  /* Plane (0 = A, 1 = B) name-table word for the world cell under screen column `x` (outside [0, width)) of output line `row`, given the
   * horizontal/vertical scroll the VDP applies to that line (plane_x = x - hscroll, plane_y = row + vscroll). Returns 1 with a word (0 = blank cell). */
  int (*scenery_word)(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word);
  /* Draws the HUD panel into the overlay (RGBA8, tightly packed). Returns 1 when something is drawn. */
  int (*hud)(void *state, GenesisCompatOverlay *out);
} GenesisNativeCompat;

#ifdef __cplusplus
}
#endif
#endif
