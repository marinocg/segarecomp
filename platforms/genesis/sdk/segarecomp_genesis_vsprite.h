#ifndef SEGARECOMP_GENESIS_VSPRITE_H
#define SEGARECOMP_GENESIS_VSPRITE_H

/*
 * Genesis virtual sprites: the per-frame command transport shared by every authoring route (SEG-140-T001..T003 ADR 0303; SEG-140-T008 ADR 0306). Strict C11, <stdint.h> only.
 *
 * A virtual sprite is PRESENTATION ONLY: a bounded, per-frame command that asks the presenter to draw a sprite-shaped pattern block over the finished native picture. It is never written into
 * VRAM or the sprite attribute table, never visible to the guest, never counted by VDP status, and does not activate a game object or make any art resident. The set is one-frame: a set submitted
 * for frame N never affects frame N+1.
 *
 * Two data origins select the art source and nothing else:
 *   GAME_STATE_VERIFIED  patterns and palette come from the FRAME's VRAM/CRAM (the game's own art). Accepted only when the host establishes that the frame's guest-state snapshot is valid and
 *                        exactly current; otherwise the whole set is rejected (UNVERIFIED_STATE).
 *   MOD_AUTHORED         patterns and palette come ONLY from the set's own authored pool (`authored_vram` / `authored_cram`, the same layouts as VRAM / CRAM); never from the frame.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_VSPRITE_VRAM_BYTES 65536U /* the authored pattern pool: the same layout/addressing as VRAM */
#define GENESIS_VSPRITE_CRAM_BYTES 128U   /* the authored palettes: the same layout as CRAM */
#define GENESIS_VSPRITE_MAX 64U          /* commands per frame */
#define GENESIS_VSPRITE_COORD_LIMIT 1024 /* |x|, |y|, |clip origin| must not exceed this (picture pixels) */

typedef enum GenesisVirtualProvenance {
  GENESIS_VSPRITE_PROV_NONE = 0, /* invalid */
  GENESIS_VSPRITE_GAME_STATE_VERIFIED = 1,
  GENESIS_VSPRITE_MOD_AUTHORED = 2
} GenesisVirtualProvenance;

/* Where the sprite sits relative to the NATIVE picture's own priority classes (ADR 0303 decision 3). */
typedef enum GenesisVirtualLayer {
  GENESIS_VSPRITE_LAYER_BEHIND_PLANES = 0, /* visible only where the native pixel is the backdrop (behind every plane/sprite pixel) */
  GENESIS_VSPRITE_LAYER_BETWEEN = 1,       /* above low-priority planes/sprites, below high-priority ones */
  GENESIS_VSPRITE_LAYER_ABOVE_ALL = 2      /* above everything native */
} GenesisVirtualLayer;

typedef enum GenesisVirtualBudgetMode {
  GENESIS_VSPRITE_BUDGET_LIMIT = 0,   /* default: virtual sprites obey what is left of the original per-line sprite and cell budget after the hardware sprites */
  GENESIS_VSPRITE_BUDGET_EXTENDED = 1 /* explicit mod opt-in: no per-line limit; over-budget lines are still reported */
} GenesisVirtualBudgetMode;

typedef struct GenesisVirtualSprite {
  uint8_t provenance;               /* GenesisVirtualProvenance */
  uint8_t layer;                    /* GenesisVirtualLayer */
  uint8_t width_cells, height_cells; /* 1..4 each */
  uint8_t palette;                  /* 0..3: palette line of the frame CRAM (verified) or of authored_cram (authored) */
  uint8_t h_flip, v_flip;           /* 0 or 1 */
  uint8_t has_clip;                 /* 0 or 1: when 1 the sprite is drawn only inside the clip rectangle */
  uint8_t reserved;                 /* must be 0 */
  uint16_t tile_index;              /* first pattern (0..2047); the block uses width*height consecutive patterns, column-major, exactly like a hardware sprite */
  int16_t x, y;                     /* picture pixel of the sprite's top-left corner (may be negative: partly off the picture) */
  int16_t clip_x, clip_y;           /* clip rectangle in picture pixels (has_clip only) */
  uint16_t clip_w, clip_h;          /* 1..picture size */
} GenesisVirtualSprite;

typedef struct GenesisVirtualSpriteSet {
  uint8_t budget_mode;              /* GenesisVirtualBudgetMode (0 = LIMIT) */
  uint8_t reserved[3];              /* must be 0 */
  uint32_t count;                   /* commands used, at most GENESIS_VSPRITE_MAX; order is priority within a layer (the first command wins, as in the hardware table) */
  GenesisVirtualSprite sprites[GENESIS_VSPRITE_MAX];
  uint8_t authored_vram[GENESIS_VSPRITE_VRAM_BYTES]; /* the authored pattern pool: the same layout/addressing as VRAM */
  uint8_t authored_cram[GENESIS_VSPRITE_CRAM_BYTES]; /* the authored palettes: the same layout as CRAM */
} GenesisVirtualSpriteSet;


#ifdef __cplusplus
}
#endif
#endif
