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

/* The command/set transport types (GenesisVirtualSprite, GenesisVirtualSpriteSet, the provenance/layer/budget enums and limits) are defined ONCE, in the public SDK header every authoring route shares
 * (SEG-140-T008, ADR 0306): the compositor below and the ABI 1 / ABI 2 provider transports use the same structure, so there is no second copy that could drift. */
#include "../sdk/segarecomp_genesis_vsprite.h"

typedef char genesis_vsprite_vram_matches_vdp[(GENESIS_VSPRITE_VRAM_BYTES == GENESIS_VDP_VRAM_BYTES) ? 1 : -1];
typedef char genesis_vsprite_cram_matches_vdp[(GENESIS_VSPRITE_CRAM_BYTES == GENESIS_VDP_CRAM_BYTES) ? 1 : -1];

#define GENESIS_VSPRITE_HOST_STATE_VERIFIED 1U /* host flag: the frame's guest snapshot is valid and exactly current */
#define GENESIS_VSPRITE_HOST_EXT_SPRITES 2U    /* host flag (ADR 0313): the frame's extension carries the hardware sprite layer, so HARDWARE_PIECE commands are omitted from it */

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
  GENESIS_VSPRITE_REJ_ARGUMENTS = 12, /* compose: a missing/inconsistent frame or side data */
  GENESIS_VSPRITE_REJ_SIDE_PRIORITY = 13 /* SEG-140-T009 (ADR 0307): a BEHIND_PLANES / BETWEEN command reaches into a presented extension column, where no native priority class exists */
} GenesisVirtualReason;

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
  /* SEG-140-T009: of the pixels above, those drawn in the left / right extension columns (the rest are in the picture); zero unless the set used the PRESENTATION space on a presented extension. */
  uint32_t pixels_left_verified, pixels_left_authored, pixels_right_verified, pixels_right_authored;
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

/* SEG-140-T009 (ADR 0307, ADR 0312): the presented extension surface a set in the PRESENTATION space is composed onto. `rgb` is the produced surface ((width + 2 * side) x height RGB24, its centre the native picture), `side` the
 * produced columns per side and `visible` how many of them (per side, <= side) the host actually presents: the only extension pixels that are ever drawn. The output is a surface of the SAME layout (the base is
 * `rgb`, the centre re-derived from the native artifact), so the host can present it unchanged.
 * `layer` (ADR 0312) is the producer's own record, for the SAME frame and the same surface layout ((width + 2 * side) x height bytes, one GENESIS_VDP_LAYER_* per pixel), of which class of content won each extension
 * pixel under the VDP's six-layer order (backdrop, low or high priority). With it, BEHIND_PLANES and BETWEEN commands are ordered against the extension columns exactly as against the native picture. Without it (NULL), or when
 * a visible extension pixel holds a value that is not a class, no priority information exists for the frame and such a command that reaches an extension column rejects the whole set (GENESIS_VSPRITE_REJ_SIDE_PRIORITY);
 * priority is never inferred from colour or neighbouring pixels. ABOVE_ALL needs none. A NULL surface, `side` 0 or the PICTURE space is exactly genesis_virtual_sprites_compose (picture-sized output). */
typedef struct GenesisVirtualSpriteSurface {
  const uint8_t *rgb;
  unsigned side, visible;
  const uint8_t *layer; /* ADR 0312: per-pixel winner class of the produced surface, or NULL when the producer recorded none */
} GenesisVirtualSpriteSurface;
int genesis_virtual_sprites_compose_wide(const GenesisVirtualSpriteSet *set, unsigned host_flags, const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisFrameArtifact *native,
                                         const GenesisVdpFrameAux *aux, const GenesisVirtualSpriteSurface *surface, uint8_t *rgb_out, size_t rgb_capacity, GenesisVirtualSpriteReport *report);

#ifdef __cplusplus
}
#endif
#endif
