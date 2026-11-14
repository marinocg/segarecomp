/* SEG-140-T001..T003 (ADR 0303): authored virtual sprites. See virtual_sprite.h for the contract. */
#include "virtual_sprite.h"

#include <string.h>

static int vs_fail(GenesisVirtualSpriteDiag *diag, unsigned reason, uint32_t index) {
  if (diag != NULL) {
    diag->reason = (uint8_t)reason;
    diag->index = index;
  }
  return 1;
}

int genesis_virtual_sprites_validate(const GenesisVirtualSpriteSet *set, unsigned host_flags, unsigned width, unsigned height, int im2, GenesisVirtualSpriteDiag *diag) {
  const unsigned tile_limit = im2 ? 1024U : 2048U; /* patterns addressable without wrapping the 64 KiB image */
  if (diag != NULL) memset(diag, 0, sizeof(*diag));
  if (set == NULL) return vs_fail(diag, GENESIS_VSPRITE_REJ_ARGUMENTS, 0U);
  if (set->count > GENESIS_VSPRITE_MAX) return vs_fail(diag, GENESIS_VSPRITE_REJ_TOO_MANY, set->count);
  if (set->budget_mode > GENESIS_VSPRITE_BUDGET_EXTENDED) return vs_fail(diag, GENESIS_VSPRITE_REJ_BUDGET_MODE, 0U);
  if (set->reserved[0] != 0U || set->reserved[1] != 0U || set->reserved[2] != 0U) return vs_fail(diag, GENESIS_VSPRITE_REJ_RESERVED, 0U);
  for (uint32_t i = 0U; i < set->count; ++i) {
    const GenesisVirtualSprite *s = &set->sprites[i];
    if (s->provenance != GENESIS_VSPRITE_GAME_STATE_VERIFIED && s->provenance != GENESIS_VSPRITE_MOD_AUTHORED) return vs_fail(diag, GENESIS_VSPRITE_REJ_PROVENANCE, i);
    if (s->provenance == GENESIS_VSPRITE_GAME_STATE_VERIFIED && (host_flags & GENESIS_VSPRITE_HOST_STATE_VERIFIED) == 0U) return vs_fail(diag, GENESIS_VSPRITE_REJ_UNVERIFIED_STATE, i);
    if (s->reserved != 0U) return vs_fail(diag, GENESIS_VSPRITE_REJ_RESERVED, i);
    if (s->width_cells < 1U || s->width_cells > 4U || s->height_cells < 1U || s->height_cells > 4U) return vs_fail(diag, GENESIS_VSPRITE_REJ_SIZE, i);
    if (s->h_flip > 1U || s->v_flip > 1U || s->has_clip > 1U) return vs_fail(diag, GENESIS_VSPRITE_REJ_RESERVED, i);
    if ((unsigned)s->tile_index + (unsigned)s->width_cells * (unsigned)s->height_cells > tile_limit) return vs_fail(diag, GENESIS_VSPRITE_REJ_TILE_RANGE, i);
    if (s->palette > 3U) return vs_fail(diag, GENESIS_VSPRITE_REJ_PALETTE, i);
    if (s->layer > GENESIS_VSPRITE_LAYER_ABOVE_ALL) return vs_fail(diag, GENESIS_VSPRITE_REJ_LAYER, i);
    if (s->x < -GENESIS_VSPRITE_COORD_LIMIT || s->x > GENESIS_VSPRITE_COORD_LIMIT || s->y < -GENESIS_VSPRITE_COORD_LIMIT || s->y > GENESIS_VSPRITE_COORD_LIMIT)
      return vs_fail(diag, GENESIS_VSPRITE_REJ_POSITION, i);
    if (s->has_clip != 0U) {
      if (s->clip_w == 0U || s->clip_h == 0U || s->clip_x < 0 || s->clip_y < 0 || (unsigned)s->clip_x >= width || (unsigned)s->clip_y >= height ||
          (unsigned)s->clip_x + s->clip_w > width || (unsigned)s->clip_y + s->clip_h > height)
        return vs_fail(diag, GENESIS_VSPRITE_REJ_CLIP, i);
    }
  }
  return 0;
}

static unsigned vs_sat_u8(unsigned v) { return v > 255U ? 255U : v; }

/* One active (row-touching) sprite: the decoded entry plus how many of its cells the budget lets it draw on this row. */
typedef struct VsActive {
  GenesisVdpSpriteEntry entry;
  const uint8_t *vram;
  const uint8_t *cram;
  unsigned drawn_cells;
  int clip_x0, clip_x1; /* [x0, x1) inside the picture, already intersected with the picture and the clip */
  uint8_t layer, provenance;
} VsActive;

int genesis_virtual_sprites_compose(const GenesisVirtualSpriteSet *set, unsigned host_flags, const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                    const GenesisFrameArtifact *native, const GenesisVdpFrameAux *aux, uint8_t *rgb_out, size_t rgb_capacity,
                                    GenesisVirtualSpriteReport *report) {
  GenesisVirtualSpriteReport local;
  GenesisVirtualSpriteReport *rep = report != NULL ? report : &local;
  GenesisVirtualSpriteDiag diag;
  unsigned width, height;
  int compose = 0;
  memset(rep, 0, sizeof(*rep));
  if (native == NULL || aux == NULL || rgb_out == NULL || vram == NULL) return 1;
  width = GENESIS_FRAME_ARTIFACT_WIDTH(native);
  height = GENESIS_FRAME_ARTIFACT_HEIGHT(native);
  if (width == 0U || width > GENESIS_FRAME_MAX_WIDTH || height == 0U || height > GENESIS_FRAME_MAX_HEIGHT || aux->width != width || aux->height != height ||
      rgb_capacity < (size_t)width * height * 3U)
    return 1;
  rep->sprite_limit = aux->sprite_limit;
  rep->cell_budget = aux->cell_budget;
  rep->rows = (uint16_t)height;
  if (set != NULL && set->count != 0U) {
    if (genesis_virtual_sprites_validate(set, host_flags, width, height, aux->im2 != 0U, &diag) != 0) {
      rep->status = GENESIS_VSPRITE_STATUS_REJECTED;
      rep->reason = diag.reason;
      rep->reject_index = diag.index;
    } else {
      rep->status = GENESIS_VSPRITE_STATUS_COMPOSED;
      rep->commands = set->count;
      compose = 1;
    }
  }
  for (unsigned row = 0U; row < height; ++row) {
    uint8_t *out = rgb_out + (size_t)row * width * 3U;
    VsActive active[GENESIS_VSPRITE_MAX];
    uint8_t claimed[GENESIS_FRAME_MAX_WIDTH];
    unsigned active_count = 0U;
    unsigned slots_used = aux->row_hw_sprites[row];
    unsigned cells_demand = aux->row_hw_cells[row];
    unsigned cells_left = aux->row_hw_cells[row] >= aux->cell_budget ? 0U : (unsigned)aux->cell_budget - aux->row_hw_cells[row];
    unsigned sprite_demand = aux->row_hw_sprites[row];
    const uint8_t *row_cram;
    if (genesis_vdp_frame_row_rgb888(native, row, out) != 0) return 1;
    row_cram = genesis_vdp_frame_row_cram(native, row);
    if (row_cram == NULL) return 1;
    if (compose) {
      for (uint32_t i = 0U; i < set->count; ++i) {
        const GenesisVirtualSprite *s = &set->sprites[i];
        const int h_px = (int)s->height_cells * (int)aux->cell_height;
        const int y0 = s->y;
        int x0, x1, cy0, cy1;
        unsigned drawn;
        VsActive *a;
        if ((int)row < y0 || (int)row >= y0 + h_px) continue;
        cy0 = s->has_clip ? s->clip_y : 0;
        cy1 = s->has_clip ? (int)s->clip_y + (int)s->clip_h : (int)height;
        if ((int)row < cy0 || (int)row >= cy1) continue; /* a sprite clipped away on this row does not occupy it */
        ++sprite_demand;
        cells_demand += s->width_cells;
        if (set->budget_mode == GENESIS_VSPRITE_BUDGET_LIMIT) {
          if (slots_used >= aux->sprite_limit) { ++rep->sprite_rows_dropped; continue; }
          ++slots_used;
          drawn = s->width_cells < cells_left ? s->width_cells : cells_left;
          cells_left -= drawn;
          if (drawn < s->width_cells) ++rep->sprite_rows_truncated;
        } else {
          drawn = s->width_cells;
        }
        if (drawn == 0U) continue;
        x0 = s->has_clip ? s->clip_x : 0;
        x1 = s->has_clip ? (int)s->clip_x + (int)s->clip_w : (int)width;
        a = &active[active_count++];
        a->entry.x = s->x;
        a->entry.y = s->y;
        a->entry.width_cells = s->width_cells;
        a->entry.height_cells = s->height_cells;
        a->entry.link = 0U;
        a->entry.tile_index = s->tile_index;
        a->entry.h_flip = s->h_flip;
        a->entry.v_flip = s->v_flip;
        a->entry.palette = s->palette;
        a->entry.priority = 0U;
        a->entry.interlace_mode2 = aux->im2;
        a->vram = s->provenance == GENESIS_VSPRITE_MOD_AUTHORED ? set->authored_vram : vram;
        a->cram = s->provenance == GENESIS_VSPRITE_MOD_AUTHORED ? set->authored_cram : row_cram;
        a->drawn_cells = drawn;
        a->layer = s->layer;
        a->provenance = s->provenance;
        /* only the first `drawn` cells exist on this line (the hardware's own truncation); beyond them nothing is drawn */
        if (x0 < (int)s->x) x0 = s->x;
        if (x1 > (int)s->x + (int)drawn * (int)GENESIS_VDP_TILE_WIDTH) x1 = (int)s->x + (int)drawn * (int)GENESIS_VDP_TILE_WIDTH;
        if (x0 < 0) x0 = 0;
        if (x1 > (int)width) x1 = (int)width;
        a->clip_x0 = x0;
        a->clip_x1 = x1;
      }
      memset(claimed, 0, width);
      for (int layer = GENESIS_VSPRITE_LAYER_ABOVE_ALL; layer >= GENESIS_VSPRITE_LAYER_BEHIND_PLANES; --layer) {
        for (unsigned i = 0U; i < active_count; ++i) {
          const VsActive *a = &active[i];
          if ((int)a->layer != layer) continue;
          for (int x = a->clip_x0; x < a->clip_x1; ++x) {
            const uint8_t native_class = aux->layer[(size_t)row * width + (unsigned)x];
            uint8_t *px = out + (size_t)x * 3U;
            uint8_t index = 0U;
            GenesisRgb888 color;
            if (claimed[x] != 0U) continue; /* a higher candidate (higher layer, or an earlier command in the same layer) already owns this pixel */
            if (layer == GENESIS_VSPRITE_LAYER_BEHIND_PLANES && native_class != GENESIS_VDP_LAYER_BACKDROP) continue;
            if (layer == GENESIS_VSPRITE_LAYER_BETWEEN && native_class == GENESIS_VDP_LAYER_HIGH) continue;
            if (genesis_vdp_resolve_sprite_pixel(&a->entry, a->vram, (unsigned)x, row, &index) != 0 || index == GENESIS_VDP_TRANSPARENT_PALETTE_INDEX) continue;
            if (genesis_vdp_decode_cram_entry(a->cram, genesis_vdp_cram_index(a->entry.palette, index), &color) != 0) continue;
            px[0] = color.r; px[1] = color.g; px[2] = color.b;
            claimed[x] = 1U;
            if (a->provenance == GENESIS_VSPRITE_GAME_STATE_VERIFIED) ++rep->pixels_drawn_verified; else ++rep->pixels_drawn_authored;
          }
        }
      }
    }
    rep->row_sprites[row] = (uint8_t)vs_sat_u8(sprite_demand);
    rep->row_cells[row] = (uint8_t)vs_sat_u8(cells_demand);
    if (sprite_demand > aux->sprite_limit) ++rep->rows_over_sprite_limit;
    if (cells_demand > aux->cell_budget) ++rep->rows_over_cell_budget;
  }
  return 0;
}
