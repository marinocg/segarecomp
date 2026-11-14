#ifndef SEGARECOMP_GENESIS_VIRTUAL_SPRITE_H
#define SEGARECOMP_GENESIS_VIRTUAL_SPRITE_H

/*
 * SEG-140-T001..T003 (ADR 0303): authored virtual sprites. A virtual sprite is PRESENTATION ONLY: a bounded, per-frame command that asks the presenter to draw a sprite-shaped
 * pattern block over the finished native picture. It is never written into VRAM or the sprite attribute table, never visible to the guest, never counted by VDP status, and drawing it
 * does not activate a game object or make any art resident (a verified sprite shows whatever the frame's VRAM holds at that tile; the host does not check that the game's object exists).
 *
 * Pattern, flip and palette decoding is the VDP's own (`genesis_vdp_resolve_sprite_pixel`, `genesis_vdp_decode_cram_entry` in vdp_render.c): there is no second decoder. The two data origins
 * select the art source and nothing else:
 *   GAME_STATE_VERIFIED  patterns and palette come from the FRAME's VRAM/CRAM (the game's own art). Only accepted when the host says the frame's guest-state snapshot is valid
 *                        (GENESIS_VSPRITE_HOST_STATE_VERIFIED); otherwise the whole set is rejected (UNVERIFIED_STATE). What the position depicts remains the author's claim.
 *   MOD_AUTHORED         patterns and palette come ONLY from the set's own authored pool (`authored_vram`/`authored_cram`, same layouts as VRAM/CRAM); never from the frame.
 * A set is validated as a whole and fails closed: one bad command rejects every command of the frame (nothing is approximated), with the first offending index and a stable reason.
 *
 * Strict C11; no allocation; deterministic; the native picture and the set are never modified.
 */
#include <stddef.h>
#include <stdint.h>

#include "vdp_render.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#define GENESIS_VSPRITE_HOST_STATE_VERIFIED 1U /* host flag: the frame's guest snapshot is valid and exactly current */

typedef enum GenesisVirtualReason {
  GENESIS_VSPRITE_OK = 0,
  GENESIS_VSPRITE_REJ_TOO_MANY = 1,
  GENESIS_VSPRITE_REJ_PROVENANCE = 2,
  GENESIS_VSPRITE_REJ_UNVERIFIED_STATE = 3,
  GENESIS_VSPRITE_REJ_SIZE = 4,
  GENESIS_VSPRITE_REJ_TILE_RANGE = 5,
  GENESIS_VSPRITE_REJ_PALETTE = 6,
  GENESIS_VSPRITE_REJ_LAYER = 7,
  GENESIS_VSPRITE_REJ_POSITION = 8,
  GENESIS_VSPRITE_REJ_CLIP = 9,
  GENESIS_VSPRITE_REJ_RESERVED = 10,
  GENESIS_VSPRITE_REJ_BUDGET_MODE = 11,
  GENESIS_VSPRITE_REJ_ARGUMENTS = 12 /* compose: a missing/inconsistent frame or side data */
} GenesisVirtualReason;

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
  uint8_t authored_vram[GENESIS_VDP_VRAM_BYTES]; /* the authored pattern pool: the same layout/addressing as VRAM */
  uint8_t authored_cram[GENESIS_VDP_CRAM_BYTES]; /* the authored palettes: the same layout as CRAM */
} GenesisVirtualSpriteSet;

typedef struct GenesisVirtualSpriteDiag {
  uint8_t reason;     /* GenesisVirtualReason; OK = accepted */
  uint32_t index;     /* offending command (REJ_TOO_MANY: the count) */
} GenesisVirtualSpriteDiag;

/* Checks the whole set for a picture of `width` x `height` with `im2` (interlace mode 2) and the host flags. Pure. Returns 0 when accepted; otherwise nonzero with `diag` filled. */
int genesis_virtual_sprites_validate(const GenesisVirtualSpriteSet *set, unsigned host_flags, unsigned width, unsigned height, int im2, GenesisVirtualSpriteDiag *diag);

typedef enum GenesisVirtualStatus {
  GENESIS_VSPRITE_STATUS_NONE = 0,     /* no set / no commands: the output is the native picture, byte for byte */
  GENESIS_VSPRITE_STATUS_COMPOSED = 1,
  GENESIS_VSPRITE_STATUS_REJECTED = 2  /* the set failed validation: the output is the native picture, byte for byte */
} GenesisVirtualStatus;

typedef struct GenesisVirtualSpriteReport {
  uint8_t status;                 /* GenesisVirtualStatus */
  uint8_t reason;                 /* GenesisVirtualReason when REJECTED */
  uint32_t reject_index;
  uint32_t commands;              /* accepted commands */
  uint32_t pixels_drawn_verified; /* native pixels replaced by a game-state-verified sprite */
  uint32_t pixels_drawn_authored; /* native pixels replaced by a mod-authored sprite */
  /* Original-budget accounting (hardware sprites the VDP selected for the row + the virtual sprites that touch it), whatever the mode: */
  uint32_t rows_over_sprite_limit; /* rows whose total sprite demand exceeds the original per-line sprite limit */
  uint32_t rows_over_cell_budget;  /* rows whose total sprite-cell demand exceeds the original per-line cell budget */
  uint32_t sprite_rows_dropped;    /* LIMIT mode: (virtual sprite, row) pairs not drawn because no per-line slot was left */
  uint32_t sprite_rows_truncated;  /* LIMIT mode: (virtual sprite, row) pairs drawn with fewer cells than their width because the cell budget ran out */
  uint8_t sprite_limit, cell_budget; /* the original limits of the frame's display width */
  uint16_t rows;                  /* rows described by the arrays below */
  uint8_t row_sprites[GENESIS_FRAME_MAX_HEIGHT]; /* per row: hardware + virtual sprite demand (saturating at 255) */
  uint8_t row_cells[GENESIS_FRAME_MAX_HEIGHT];   /* per row: hardware + virtual cell demand (saturating at 255) */
} GenesisVirtualSpriteReport;

/*
 * Composes `set` over the finished native `frame` into `rgb_out` (width * height * 3 bytes, tightly packed). `vram` is the VRAM verified sprites read (the frame-end image, the same limit as
 * the extension producer); `aux` is the side data from genesis_vdp_produce_frame*_aux for THE SAME frame. With a NULL/empty/rejected set the output is exactly the native picture.
 * Returns 0 on success (the report says COMPOSED, NONE or REJECTED); nonzero only for unusable arguments (no output, a frame without side data, a short buffer).
 */
int genesis_virtual_sprites_compose(const GenesisVirtualSpriteSet *set, unsigned host_flags, const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                    const GenesisFrameArtifact *native, const GenesisVdpFrameAux *aux, uint8_t *rgb_out, size_t rgb_capacity,
                                    GenesisVirtualSpriteReport *report);

#ifdef __cplusplus
}
#endif
#endif
