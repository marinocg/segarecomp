/* SEG-007-T049 checkpoint C1: tile-pattern and palette decoding.
 * See vdp_render.h for the cited public sources and the labeled project
 * channel-scaling policy this file implements. */

#include "vdp_render.h"

#include <stddef.h>
#include <string.h>

int genesis_vdp_decode_tile_pixel(const uint8_t tile[GENESIS_VDP_TILE_BYTES], unsigned row,
                                   unsigned col, uint8_t *index_out) {
  if (tile == NULL || index_out == NULL) {
    return -1;
  }
  if (row >= GENESIS_VDP_TILE_HEIGHT || col >= GENESIS_VDP_TILE_WIDTH) {
    return -1;
  }
  /* GTO1 pp. 53-54: 4 bytes per row, high nibble = left pixel (even column),
   * low nibble = right pixel (odd column). */
  unsigned byte_index = row * 4U + (col / 2U);
  uint8_t byte_value = tile[byte_index];
  uint8_t nibble = (col % 2U == 0U) ? (uint8_t)(byte_value >> 4) : (uint8_t)(byte_value & 0x0FU);
  *index_out = (uint8_t)(nibble & 0x0FU);
  return 0;
}

int genesis_vdp_decode_tile(const uint8_t tile[GENESIS_VDP_TILE_BYTES],
                             uint8_t indices_out[GENESIS_VDP_TILE_WIDTH * GENESIS_VDP_TILE_HEIGHT]) {
  if (tile == NULL || indices_out == NULL) {
    return -1;
  }
  for (unsigned row = 0; row < GENESIS_VDP_TILE_HEIGHT; ++row) {
    for (unsigned col = 0; col < GENESIS_VDP_TILE_WIDTH; ++col) {
      uint8_t index_value = 0;
      /* Bounds are fixed and already validated by the loop ranges above;
       * this call can only fail on NULL, already excluded. */
      (void)genesis_vdp_decode_tile_pixel(tile, row, col, &index_value);
      indices_out[row * GENESIS_VDP_TILE_WIDTH + col] = index_value;
    }
  }
  return 0;
}

/* value is a 3-bit channel (0-7); scaled to 0-255 by value * 255 / 7, the
 * labeled project compatibility policy documented in vdp_render.h. */
static uint8_t genesis_vdp_scale_3bit_channel(uint8_t value) {
  return (uint8_t)((unsigned)(value & 0x07U) * 255U / 7U);
}

GenesisRgb888 genesis_vdp_decode_cram_word(uint16_t raw_word) {
  /* ----BBB-GGG-RRR- : bits 11-9 = Blue, bits 7-5 = Green, bits 3-1 = Red.
   * See vdp_render.h for the cited, cross-corroborated bit layout. */
  uint8_t blue3 = (uint8_t)((raw_word >> 9) & 0x07U);
  uint8_t green3 = (uint8_t)((raw_word >> 5) & 0x07U);
  uint8_t red3 = (uint8_t)((raw_word >> 1) & 0x07U);
  GenesisRgb888 color;
  color.r = genesis_vdp_scale_3bit_channel(red3);
  color.g = genesis_vdp_scale_3bit_channel(green3);
  color.b = genesis_vdp_scale_3bit_channel(blue3);
  return color;
}

int genesis_vdp_decode_cram_entry(const uint8_t cram[GENESIS_VDP_CRAM_BYTES], unsigned entry_index,
                                   GenesisRgb888 *color_out) {
  if (cram == NULL || color_out == NULL) {
    return -1;
  }
  if (entry_index >= GENESIS_VDP_CRAM_ENTRY_COUNT) {
    return -1;
  }
  unsigned byte_offset = entry_index * 2U;
  /* GenesisVdpState.cram is documented as big-endian halfword order. */
  uint16_t raw_word = (uint16_t)(((uint16_t)cram[byte_offset] << 8) | (uint16_t)cram[byte_offset + 1U]);
  *color_out = genesis_vdp_decode_cram_word(raw_word);
  return 0;
}

/* --- SEG-007-T049 checkpoint C2: plane/nametable addressing and bound
 * scrolling behavior. See vdp_render.h for the cited public sources. --- */

/* Decodes one register #16 width/height field value (0-3) into a cell
 * count. Returns 0 and writes `*cells_out` (32/64/128) on success; returns
 * a negative value for the reserved field value
 * GENESIS_VDP_PLANE_SIZE_RESERVED_FIELD. */
static int genesis_vdp_decode_plane_size_field(unsigned field, unsigned *cells_out) {
  switch (field) {
    case 0U:
      *cells_out = 32U;
      return 0;
    case 1U:
      *cells_out = 64U;
      return 0;
    case 3U:
      *cells_out = 128U;
      return 0;
    default:
      /* field == GENESIS_VDP_PLANE_SIZE_RESERVED_FIELD (2): reserved. */
      return -1;
  }
}

int genesis_vdp_decode_plane_size(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                   unsigned *width_cells_out, unsigned *height_cells_out) {
  if (registers == NULL || width_cells_out == NULL || height_cells_out == NULL) {
    return -1;
  }
  uint16_t reg16 = registers[16];
  unsigned width_field = (unsigned)(reg16 & 0x03U);
  unsigned height_field = (unsigned)((reg16 >> 4) & 0x03U);
  unsigned width_cells = 0U;
  unsigned height_cells = 0U;
  if (genesis_vdp_decode_plane_size_field(width_field, &width_cells) != 0) {
    return -1;
  }
  if (genesis_vdp_decode_plane_size_field(height_field, &height_cells) != 0) {
    return -1;
  }
  if (width_cells * height_cells > GENESIS_VDP_PLANE_MAX_CELLS) {
    return -1;
  }
  *width_cells_out = width_cells;
  *height_cells_out = height_cells;
  return 0;
}

int genesis_vdp_plane_base_address(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                    GenesisVdpPlaneSelector plane, uint32_t *base_addr_out) {
  if (registers == NULL || base_addr_out == NULL) {
    return -1;
  }
  if (plane == GENESIS_VDP_PLANE_A) {
    /* Register #2 bits 5-3 = SA15-13. */
    *base_addr_out = ((uint32_t)registers[2] << 10) & 0xE000U;
    return 0;
  }
  if (plane == GENESIS_VDP_PLANE_B) {
    /* Register #4 bits 2-0 = SB15-13 (see vdp_render.h's discrepancy note). */
    *base_addr_out = ((uint32_t)registers[4] << 13) & 0xE000U;
    return 0;
  }
  return -1;
}

GenesisVdpNametableEntry genesis_vdp_decode_nametable_word(uint16_t raw_word) {
  GenesisVdpNametableEntry entry;
  entry.tile_index = (uint16_t)(raw_word & GENESIS_VDP_TILE_INDEX_MASK);
  entry.h_flip = (uint8_t)((raw_word >> 11) & 0x01U);
  entry.v_flip = (uint8_t)((raw_word >> 12) & 0x01U);
  entry.palette = (uint8_t)((raw_word >> 13) & 0x03U);
  entry.priority = (uint8_t)((raw_word >> 15) & 0x01U);
  return entry;
}

int genesis_vdp_nametable_cell_address(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                        GenesisVdpPlaneSelector plane, unsigned cell_col,
                                        unsigned cell_row, uint32_t *addr_out) {
  if (registers == NULL || addr_out == NULL) {
    return -1;
  }
  unsigned width_cells = 0U;
  unsigned height_cells = 0U;
  if (genesis_vdp_decode_plane_size(registers, &width_cells, &height_cells) != 0) {
    return -1;
  }
  if (cell_col >= width_cells || cell_row >= height_cells) {
    return -1;
  }
  uint32_t base_addr = 0U;
  if (genesis_vdp_plane_base_address(registers, plane, &base_addr) != 0) {
    return -1;
  }
  uint32_t cell_index = (uint32_t)cell_row * (uint32_t)width_cells + (uint32_t)cell_col;
  uint32_t addr = base_addr + cell_index * 2U;
  if (addr + 1U >= GENESIS_VDP_VRAM_BYTES) {
    return -1;
  }
  *addr_out = addr;
  return 0;
}

int genesis_vdp_read_nametable_entry(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                      const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                      GenesisVdpPlaneSelector plane, unsigned cell_col,
                                      unsigned cell_row, GenesisVdpNametableEntry *entry_out) {
  if (vram == NULL || entry_out == NULL) {
    return -1;
  }
  uint32_t addr = 0U;
  if (genesis_vdp_nametable_cell_address(registers, plane, cell_col, cell_row, &addr) != 0) {
    return -1;
  }
  uint16_t raw_word = (uint16_t)(((uint16_t)vram[addr] << 8) | (uint16_t)vram[addr + 1U]);
  *entry_out = genesis_vdp_decode_nametable_word(raw_word);
  return 0;
}

int genesis_vdp_mode_is_mode5(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  return (int)((registers[1] & GENESIS_VDP_REG1_MODE5_BIT) != 0U);
}

int genesis_vdp_hres_is_h40(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  int rs0 = (registers[12] & GENESIS_VDP_REG12_RS0_BIT) != 0U;
  int rs1 = (registers[12] & GENESIS_VDP_REG12_RS1_BIT) != 0U;
  return (int)(rs0 && rs1);
}

int genesis_vdp_interlace_is_none(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  int lsm1 = (registers[12] & GENESIS_VDP_REG12_LSM1_BIT) != 0U;
  int lsm0 = (registers[12] & GENESIS_VDP_REG12_LSM0_BIT) != 0U;
  return (int)(!lsm1 && !lsm0);
}

int genesis_vdp_hscroll_mode_is_fullscreen(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  return (int)((registers[11] & GENESIS_VDP_HSCROLL_MODE_MASK) == GENESIS_VDP_HSCROLL_MODE_FULLSCREEN);
}

/* SEG-007-T050: see vdp_render.h's dedicated "line" horizontal-scroll-mode
 * citation for the public sources. */
int genesis_vdp_hscroll_mode_is_line(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  return (int)((registers[11] & GENESIS_VDP_HSCROLL_MODE_MASK) == GENESIS_VDP_HSCROLL_MODE_LINE);
}

int genesis_vdp_vscroll_mode_is_2cell(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  return (int)(((registers[11] >> 2) & 0x01U) == 1U);
}

/* Sign-extends the documented low-10-bit two's-complement h/v scroll field
 * of a raw 16-bit scroll word into a plain signed value (-512..511). */
static int32_t genesis_vdp_sign_extend_scroll(uint16_t raw_word) {
  uint16_t masked = (uint16_t)(raw_word & 0x03FFU); /* low 10 bits (GENESIS_VDP_HSCROLL_VSCROLL_SIGN_BITS). */
  if ((masked & 0x0200U) != 0U) {
    return (int32_t)masked - 1024;
  }
  return (int32_t)masked;
}

int genesis_vdp_read_hscroll(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                              const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                              GenesisVdpPlaneSelector plane, unsigned screen_y,
                              int32_t *value_out) {
  if (vram == NULL || registers == NULL || value_out == NULL) {
    return -1;
  }
  if (plane != GENESIS_VDP_PLANE_A && plane != GENESIS_VDP_PLANE_B) {
    return -1;
  }
  /* SEG-007-T050: row 0 for full-screen mode (unchanged), `screen_y` itself
   * for line mode (this bound surface's 0-223 scanline range never exceeds
   * Genesis Plus GX's general `line & 0xFF` mask -- see vdp_render.h's
   * dedicated "line" horizontal-scroll-mode citation). Any other mode
   * (`01`/`10`) fails closed rather than guessing. */
  uint32_t row;
  if (genesis_vdp_hscroll_mode_is_fullscreen(registers)) {
    row = 0U;
  } else if (genesis_vdp_hscroll_mode_is_line(registers)) {
    /* This renderer's bound H40 display has exactly 224 scanlines. Reject
     * untrusted off-display values before converting them into table-row
     * arithmetic, which would otherwise permit unsigned multiplication to
     * wrap and alias a valid VRAM row. */
    if (screen_y >= GENESIS_FRAME_HEIGHT) {
      return -1;
    }
    row = (uint32_t)screen_y;
  } else {
    return -1;
  }
  /* Register #13 bits 5-0 = HS15-10. */
  uint32_t table_base = ((uint32_t)(registers[13] & 0x3FU)) << 10;
  uint32_t offset = (plane == GENESIS_VDP_PLANE_A) ? 0U : 2U;
  uint32_t addr = table_base + row * GENESIS_VDP_HSCROLL_TABLE_ROW_BYTES + offset;
  if (addr + 1U >= GENESIS_VDP_VRAM_BYTES) {
    return -1;
  }
  uint16_t raw_word = (uint16_t)(((uint16_t)vram[addr] << 8) | (uint16_t)vram[addr + 1U]);
  *value_out = genesis_vdp_sign_extend_scroll(raw_word);
  return 0;
}

int genesis_vdp_read_vscroll(const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                              const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                              GenesisVdpPlaneSelector plane, unsigned screen_cell_col,
                              int32_t *value_out) {
  if (vsram == NULL || registers == NULL || value_out == NULL) {
    return -1;
  }
  if (plane != GENESIS_VDP_PLANE_A && plane != GENESIS_VDP_PLANE_B) {
    return -1;
  }
  unsigned offset = (plane == GENESIS_VDP_PLANE_A) ? 0U : 2U;
  if (genesis_vdp_vscroll_mode_is_2cell(registers)) {
    unsigned group = screen_cell_col / 2U;
    if (group >= GENESIS_VDP_VSRAM_GROUP_COUNT) {
      return -1;
    }
    offset += group * GENESIS_VDP_VSRAM_GROUP_BYTES;
  }
  if (offset + 1U >= GENESIS_VDP_VSRAM_BYTES) {
    return -1;
  }
  uint16_t raw_word = (uint16_t)(((uint16_t)vsram[offset] << 8) | (uint16_t)vsram[offset + 1U]);
  *value_out = genesis_vdp_sign_extend_scroll(raw_word);
  return 0;
}

/* True (always non-negative, wrap-around) modulo: -1 % 320 == 319, not -1. */
static unsigned genesis_vdp_wrap_coordinate(int64_t value, unsigned dimension_px) {
  int64_t dim = (int64_t)dimension_px;
  int64_t wrapped = value % dim;
  if (wrapped < 0) {
    wrapped += dim;
  }
  return (unsigned)wrapped;
}

unsigned genesis_vdp_plane_x(unsigned screen_x, int32_t hscroll, unsigned plane_width_px) {
  /* Documented subtraction direction -- see vdp_render.h citations. */
  int64_t value = (int64_t)screen_x - (int64_t)hscroll;
  return genesis_vdp_wrap_coordinate(value, plane_width_px);
}

unsigned genesis_vdp_plane_y(unsigned screen_y, int32_t vscroll, unsigned plane_height_px) {
  /* Documented addition direction -- see vdp_render.h citations. */
  int64_t value = (int64_t)screen_y + (int64_t)vscroll;
  return genesis_vdp_wrap_coordinate(value, plane_height_px);
}

int genesis_vdp_resolve_plane_pixel(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                     const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                                     const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                     GenesisVdpPlaneSelector plane, unsigned screen_x,
                                     unsigned screen_y, GenesisVdpPlanePixelLocation *location_out) {
  if (vram == NULL || vsram == NULL || registers == NULL || location_out == NULL) {
    return -1;
  }
  if (plane != GENESIS_VDP_PLANE_A && plane != GENESIS_VDP_PLANE_B) {
    return -1;
  }
  if (!genesis_vdp_mode_is_mode5(registers) || !genesis_vdp_hres_is_h40(registers) ||
      !genesis_vdp_interlace_is_none(registers)) {
    return -1;
  }
  unsigned width_cells = 0U;
  unsigned height_cells = 0U;
  if (genesis_vdp_decode_plane_size(registers, &width_cells, &height_cells) != 0) {
    return -1;
  }
  unsigned width_px = width_cells * GENESIS_VDP_TILE_WIDTH;
  unsigned height_px = height_cells * GENESIS_VDP_TILE_HEIGHT;

  int32_t hscroll = 0;
  if (genesis_vdp_read_hscroll(vram, registers, plane, screen_y, &hscroll) != 0) {
    return -1;
  }
  unsigned screen_cell_col = screen_x / GENESIS_VDP_TILE_WIDTH;
  int32_t vscroll = 0;
  if (genesis_vdp_read_vscroll(vsram, registers, plane, screen_cell_col, &vscroll) != 0) {
    return -1;
  }

  unsigned plane_x = genesis_vdp_plane_x(screen_x, hscroll, width_px);
  unsigned plane_y = genesis_vdp_plane_y(screen_y, vscroll, height_px);
  unsigned cell_col = plane_x / GENESIS_VDP_TILE_WIDTH;
  unsigned cell_row = plane_y / GENESIS_VDP_TILE_HEIGHT;

  GenesisVdpNametableEntry entry;
  if (genesis_vdp_read_nametable_entry(vram, registers, plane, cell_col, cell_row, &entry) != 0) {
    return -1;
  }

  location_out->cell_col = cell_col;
  location_out->cell_row = cell_row;
  location_out->tile_x = plane_x % GENESIS_VDP_TILE_WIDTH;
  location_out->tile_y = plane_y % GENESIS_VDP_TILE_HEIGHT;
  location_out->entry = entry;
  return 0;
}

/* --- SEG-007-T049 checkpoint C3: sprite-attribute-table decoding and
 * placement. See vdp_render.h for the cited public sources. --- */

int genesis_vdp_sat_base_address(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                  uint32_t *base_addr_out) {
  if (registers == NULL || base_addr_out == NULL) {
    return -1;
  }
  if (!genesis_vdp_hres_is_h40(registers)) {
    /* Fail closed: this checkpoint's SAT base/size assumes H40 (see
     * citation above); do not guess an H32 SAT layout. */
    return -1;
  }
  /* H40: register #5 bit 0 is ignored -- 1024-byte SAT-base granularity
   * (see citation above). */
  *base_addr_out = ((uint32_t)registers[5] << 9) & GENESIS_VDP_REG5_SAT_BASE_H40_MASK;
  return 0;
}

GenesisVdpSpriteEntry genesis_vdp_decode_sprite_entry(
    const uint8_t raw[GENESIS_VDP_SPRITE_ENTRY_BYTES]) {
  GenesisVdpSpriteEntry entry;
  uint16_t word0 = (uint16_t)(((uint16_t)raw[0] << 8) | (uint16_t)raw[1]);
  uint8_t size_byte = raw[2];
  uint8_t link_byte = raw[3];
  uint16_t word4 = (uint16_t)(((uint16_t)raw[4] << 8) | (uint16_t)raw[5]);
  uint16_t word6 = (uint16_t)(((uint16_t)raw[6] << 8) | (uint16_t)raw[7]);

  uint16_t raw_y = (uint16_t)(word0 & GENESIS_VDP_SPRITE_Y_MASK);
  uint16_t raw_x = (uint16_t)(word6 & GENESIS_VDP_SPRITE_X_MASK);
  entry.y = (int32_t)raw_y - GENESIS_VDP_SPRITE_COORD_BIAS;
  entry.x = (int32_t)raw_x - GENESIS_VDP_SPRITE_COORD_BIAS;

  entry.width_cells = (unsigned)((size_byte >> 2) & 0x03U) + 1U;
  entry.height_cells = (unsigned)(size_byte & 0x03U) + 1U;
  entry.link = (unsigned)(link_byte & GENESIS_VDP_SPRITE_LINK_MASK);

  entry.tile_index = (uint16_t)(word4 & GENESIS_VDP_TILE_INDEX_MASK);
  entry.h_flip = (uint8_t)((word4 >> 11) & 0x01U);
  entry.v_flip = (uint8_t)((word4 >> 12) & 0x01U);
  entry.palette = (uint8_t)((word4 >> 13) & 0x03U);
  entry.priority = (uint8_t)((word4 >> 15) & 0x01U);
  return entry;
}

int genesis_vdp_read_sprite_entry(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                   const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                   unsigned sprite_index, GenesisVdpSpriteEntry *entry_out) {
  if (vram == NULL || registers == NULL || entry_out == NULL) {
    return -1;
  }
  if (sprite_index >= GENESIS_VDP_SAT_H40_MAX_SPRITES) {
    return -1;
  }
  uint32_t sat_base = 0U;
  if (genesis_vdp_sat_base_address(registers, &sat_base) != 0) {
    return -1;
  }
  uint32_t addr = sat_base + (uint32_t)sprite_index * GENESIS_VDP_SPRITE_ENTRY_BYTES;
  if (addr + GENESIS_VDP_SPRITE_ENTRY_BYTES > GENESIS_VDP_VRAM_BYTES) {
    return -1;
  }
  *entry_out = genesis_vdp_decode_sprite_entry(&vram[addr]);
  return 0;
}

int genesis_vdp_sat_traverse(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                              const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                              unsigned visited_out[GENESIS_VDP_SAT_H40_MAX_SPRITES],
                              unsigned *visited_count_out) {
  if (vram == NULL || registers == NULL || visited_out == NULL || visited_count_out == NULL) {
    return -1;
  }

  unsigned visited_flags[GENESIS_VDP_SAT_H40_MAX_SPRITES];
  for (unsigned i = 0; i < GENESIS_VDP_SAT_H40_MAX_SPRITES; ++i) {
    visited_flags[i] = 0U;
  }

  unsigned count = 0U;
  unsigned current = 0U; /* Sprite 0 is always first (see citation). */

  for (unsigned step = 0; step < GENESIS_VDP_SAT_H40_MAX_SPRITES; ++step) {
    if (visited_flags[current] != 0U) {
      /* Cycle defense: this index was already visited earlier in this same
       * traversal. Stop rather than looping forever (project-added
       * defensive bound; see the traversal-order citation above). */
      break;
    }
    GenesisVdpSpriteEntry entry;
    if (genesis_vdp_read_sprite_entry(vram, registers, current, &entry) != 0) {
      if (count == 0U) {
        /* Reading sprite index 0 itself failed: a genuine error. */
        return -1;
      }
      break;
    }
    visited_flags[current] = 1U;
    visited_out[count] = current;
    ++count;

    if (entry.link == 0U) {
      break; /* Documented end-of-list. */
    }
    if (entry.link >= GENESIS_VDP_SAT_H40_MAX_SPRITES) {
      /* Out-of-range link for this H40 SAT: treated as implicit end-of-list
       * rather than followed (see citation above). */
      break;
    }
    current = entry.link;
  }

  *visited_count_out = count;
  return 0;
}

int genesis_vdp_sprite_covers_pixel(const GenesisVdpSpriteEntry *sprite, unsigned screen_x,
                                     unsigned screen_y) {
  if (sprite == NULL) {
    return 0;
  }
  int64_t x = (int64_t)screen_x;
  int64_t y = (int64_t)screen_y;
  int64_t width_px = (int64_t)sprite->width_cells * (int64_t)GENESIS_VDP_TILE_WIDTH;
  int64_t height_px = (int64_t)sprite->height_cells * (int64_t)GENESIS_VDP_TILE_HEIGHT;
  if (x < sprite->x || x >= sprite->x + width_px) {
    return 0;
  }
  if (y < sprite->y || y >= sprite->y + height_px) {
    return 0;
  }
  return 1;
}

int genesis_vdp_resolve_sprite_pixel(const GenesisVdpSpriteEntry *sprite,
                                      const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                      unsigned screen_x, unsigned screen_y, uint8_t *index_out) {
  if (sprite == NULL || vram == NULL || index_out == NULL) {
    return -1;
  }
  if (!genesis_vdp_sprite_covers_pixel(sprite, screen_x, screen_y)) {
    return -1;
  }

  unsigned width_px = sprite->width_cells * GENESIS_VDP_TILE_WIDTH;
  unsigned height_px = sprite->height_cells * GENESIS_VDP_TILE_HEIGHT;

  /* Both are guaranteed in [0, width_px)/[0, height_px) by the covers-check
   * above, since sprite->x/y may be negative but screen_x/y - sprite->x/y
   * cannot be once covered. */
  unsigned local_x = (unsigned)((int64_t)screen_x - sprite->x);
  unsigned local_y = (unsigned)((int64_t)screen_y - sprite->y);

  /* Mirror the whole bounding box before splitting into cell/tile-local
   * coordinates -- see the column-major-order citation above for why this
   * is applied before, not after, the column-major split. */
  unsigned mirrored_x = sprite->h_flip ? (width_px - 1U - local_x) : local_x;
  unsigned mirrored_y = sprite->v_flip ? (height_px - 1U - local_y) : local_y;

  unsigned cell_col = mirrored_x / GENESIS_VDP_TILE_WIDTH;
  unsigned cell_row = mirrored_y / GENESIS_VDP_TILE_HEIGHT;
  unsigned tile_col = mirrored_x % GENESIS_VDP_TILE_WIDTH;
  unsigned tile_row = mirrored_y % GENESIS_VDP_TILE_HEIGHT;

  /* Column-major multi-cell tile order (see citation above). The effective
   * tile number is an 11-bit field (GENESIS_VDP_TILE_INDEX_MASK, the same
   * width already used for a nametable entry's tile index above); per the
   * independently-implemented Genesis Plus GX sprite-tile-address behavior,
   * an overflowing sum wraps modulo 2048 rather than being rejected. */
  uint32_t tile_offset = (uint32_t)cell_col * (uint32_t)sprite->height_cells + (uint32_t)cell_row;
  uint32_t effective_tile =
      ((uint32_t)sprite->tile_index + tile_offset) & GENESIS_VDP_TILE_INDEX_MASK;
  uint32_t tile_addr = effective_tile * GENESIS_VDP_TILE_BYTES;
  if (tile_addr + GENESIS_VDP_TILE_BYTES > GENESIS_VDP_VRAM_BYTES) {
    return -1;
  }

  return genesis_vdp_decode_tile_pixel(&vram[tile_addr], tile_row, tile_col, index_out);
}

/* --- SEG-007-T049 checkpoint C4: priority/transparency/composition. See
 * vdp_render.h for the cited public sources. This section calls only C1/C2/
 * C3 functions above; it performs no addressing or tile/CRAM decode of its
 * own. --- */

unsigned genesis_vdp_cram_index(unsigned palette, unsigned index) {
  return palette * GENESIS_VDP_PALETTE_LINE_COLORS + index;
}

int genesis_vdp_backdrop_cram_index(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                     unsigned *cram_index_out) {
  if (registers == NULL || cram_index_out == NULL) {
    return -1;
  }
  /* Register #7 bits 5-0: bits 5-4 = palette, bits 3-0 = index -- already
   * exactly palette*16+index once masked together (see citation above). */
  *cram_index_out = (unsigned)(registers[7] & GENESIS_VDP_REG7_BACKDROP_MASK);
  return 0;
}

/* Resolves one plane's candidate pixel at (screen_x, screen_y), decoding
 * transparency (palette index 0) per the citation above. Returns 0 on
 * success (including the transparent case, where `*candidate_out.opaque`
 * is 0) and a negative value only on a genuine C2/C1 failure. */
static int genesis_vdp_resolve_plane_candidate(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                                const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                                                const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                                GenesisVdpPlaneSelector plane, unsigned screen_x,
                                                unsigned screen_y,
                                                GenesisVdpCandidatePixel *candidate_out) {
  GenesisVdpPlanePixelLocation location;
  if (genesis_vdp_resolve_plane_pixel(vram, vsram, registers, plane, screen_x, screen_y,
                                       &location) != 0) {
    return -1;
  }
  uint32_t tile_addr = (uint32_t)location.entry.tile_index * GENESIS_VDP_TILE_BYTES;
  uint8_t pattern_index = 0U;
  if (tile_addr + GENESIS_VDP_TILE_BYTES > GENESIS_VDP_VRAM_BYTES) {
    /* Corrupt/out-of-range tile index for otherwise well-formed plane state:
     * fail closed rather than reading past VRAM (untrusted-input policy). */
    return -1;
  }
  unsigned tile_x = location.tile_x;
  unsigned tile_y = location.tile_y;
  if (location.entry.h_flip) {
    tile_x = GENESIS_VDP_TILE_WIDTH - 1U - tile_x;
  }
  if (location.entry.v_flip) {
    tile_y = GENESIS_VDP_TILE_HEIGHT - 1U - tile_y;
  }
  if (genesis_vdp_decode_tile_pixel(&vram[tile_addr], tile_y, tile_x, &pattern_index) != 0) {
    return -1;
  }
  candidate_out->opaque = (uint8_t)(pattern_index != GENESIS_VDP_TRANSPARENT_PALETTE_INDEX);
  candidate_out->priority = location.entry.priority;
  candidate_out->palette = location.entry.palette;
  candidate_out->index = pattern_index;
  return 0;
}

/* Resolves the winning (highest-SAT-traversal-precedence, opaque) sprite
 * candidate pixel at (screen_x, screen_y), per the sprite-overlap citation
 * above. Writes an `opaque == 0` candidate if no traversed sprite covers
 * this coordinate with an opaque pixel. Returns a negative value only on a
 * genuine SAT-traversal failure. */
static int genesis_vdp_resolve_sprite_candidate(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                                 const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                                 unsigned screen_x, unsigned screen_y,
                                                 GenesisVdpCandidatePixel *candidate_out) {
  unsigned visited[GENESIS_VDP_SAT_H40_MAX_SPRITES];
  unsigned visited_count = 0U;
  if (genesis_vdp_sat_traverse(vram, registers, visited, &visited_count) != 0) {
    return -1;
  }

  candidate_out->opaque = 0U;
  candidate_out->priority = 0U;
  candidate_out->palette = 0U;
  candidate_out->index = 0U;

  for (unsigned i = 0; i < visited_count; ++i) {
    GenesisVdpSpriteEntry sprite;
    if (genesis_vdp_read_sprite_entry(vram, registers, visited[i], &sprite) != 0) {
      /* Already successfully read once by sat_traverse; treat a re-read
       * failure here as impossible in practice, but fail closed rather than
       * guess if it somehow occurs. */
      return -1;
    }
    if (!genesis_vdp_sprite_covers_pixel(&sprite, screen_x, screen_y)) {
      continue;
    }
    uint8_t pattern_index = 0U;
    if (genesis_vdp_resolve_sprite_pixel(&sprite, vram, screen_x, screen_y, &pattern_index) != 0) {
      /* covers_pixel already confirmed coverage; a failure here means a
       * corrupt/out-of-range tile -- skip this sprite's pixel rather than
       * treat the whole composition as failed (an earlier or later sprite,
       * or a plane, may still legitimately be visible at this coordinate). */
      continue;
    }
    if (pattern_index == GENESIS_VDP_TRANSPARENT_PALETTE_INDEX) {
      /* Transparent sprite pixel: does not occlude anything underneath;
       * keep looking at later sprites in traversal order. */
      continue;
    }
    /* First opaque sprite pixel found in traversal order wins (earlier
     * SAT-traversal-order sprites take precedence) -- see citation above. */
    candidate_out->opaque = 1U;
    candidate_out->priority = sprite.priority;
    candidate_out->palette = sprite.palette;
    candidate_out->index = pattern_index;
    return 0;
  }
  return 0;
}

/* Resolves the final composed CRAM entry index (0-63) at (screen_x,
 * screen_y) per the six-layer priority order and palette-index-0-
 * transparency rule cited above (the identical algorithm
 * `genesis_vdp_compose_pixel` itself uses -- factored out here so checkpoint
 * C5's `genesis_vdp_produce_frame` can reuse the exact same winner-selection
 * logic without reverse-decoding an already-produced RGB color). Returns 0
 * and writes `*cram_index_out` on success; returns a negative value on any
 * genuine C1-C4 failure, matching `genesis_vdp_compose_pixel`'s own failure
 * conditions. */
static int genesis_vdp_resolve_cram_index(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                           const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                                           const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                           unsigned screen_x, unsigned screen_y,
                                           unsigned *cram_index_out) {
  GenesisVdpCandidatePixel plane_a;
  GenesisVdpCandidatePixel plane_b;
  GenesisVdpCandidatePixel sprite;
  if (genesis_vdp_resolve_plane_candidate(vram, vsram, registers, GENESIS_VDP_PLANE_A, screen_x,
                                           screen_y, &plane_a) != 0) {
    return -1;
  }
  if (genesis_vdp_resolve_plane_candidate(vram, vsram, registers, GENESIS_VDP_PLANE_B, screen_x,
                                           screen_y, &plane_b) != 0) {
    return -1;
  }
  if (genesis_vdp_resolve_sprite_candidate(vram, registers, screen_x, screen_y, &sprite) != 0) {
    return -1;
  }

  /* Six-layer priority order, highest to lowest: sprite-high, planeA-high,
   * planeB-high, sprite-low, planeA-low, planeB-low -- see citation above. */
  const GenesisVdpCandidatePixel *winner = NULL;
  if (sprite.opaque && sprite.priority) {
    winner = &sprite;
  } else if (plane_a.opaque && plane_a.priority) {
    winner = &plane_a;
  } else if (plane_b.opaque && plane_b.priority) {
    winner = &plane_b;
  } else if (sprite.opaque && !sprite.priority) {
    winner = &sprite;
  } else if (plane_a.opaque && !plane_a.priority) {
    winner = &plane_a;
  } else if (plane_b.opaque && !plane_b.priority) {
    winner = &plane_b;
  }

  if (winner != NULL) {
    *cram_index_out = genesis_vdp_cram_index(winner->palette, winner->index);
    return 0;
  }
  return genesis_vdp_backdrop_cram_index(registers, cram_index_out);
}

int genesis_vdp_compose_pixel(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                               const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                               const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                               const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                               unsigned screen_x, unsigned screen_y, GenesisRgb888 *color_out) {
  if (vram == NULL || vsram == NULL || cram == NULL || registers == NULL || color_out == NULL) {
    return -1;
  }

  unsigned cram_index;
  if (genesis_vdp_resolve_cram_index(vram, vsram, registers, screen_x, screen_y, &cram_index) != 0) {
    return -1;
  }

  return genesis_vdp_decode_cram_entry(cram, cram_index, color_out);
}

/* --- SEG-007-T049 checkpoint C5: deterministic frame output. See
 * vdp_render.h for the applied (not redefined) T042 schema and cited
 * behavior. This section calls only C1/C4 functions above plus this
 * runtime's existing shared SHA-256 (runtime.h); it performs no addressing,
 * decode, or composition logic of its own. --- */

int genesis_vdp_produce_frame(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                               const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                               const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                               const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                               GenesisFrameArtifact *frame_out) {
  if (vram == NULL || vsram == NULL || cram == NULL || registers == NULL || frame_out == NULL) {
    return -1;
  }

  /* Build into a local scratch artifact first so a mid-frame composition
   * failure never leaves a partially-filled `*frame_out` (fail-closed for
   * the whole frame, per this checkpoint's documented contract). */
  GenesisFrameArtifact scratch;

  for (unsigned row = 0; row < GENESIS_FRAME_HEIGHT; ++row) {
    for (unsigned col = 0; col < GENESIS_FRAME_WIDTH; ++col) {
      unsigned cram_index = 0U;
      if (genesis_vdp_resolve_cram_index(vram, vsram, registers, col, row, &cram_index) != 0) {
        return -1;
      }
      scratch.pixels[row * GENESIS_FRAME_WIDTH + col] = (uint8_t)cram_index;
    }
  }

  memcpy(scratch.palette_snapshot, cram, GENESIS_VDP_CRAM_BYTES);

  GenesisSha256 digest;
  genesis_sha256_init(&digest);
  genesis_sha256_update(&digest, scratch.pixels, sizeof(scratch.pixels));
  genesis_sha256_update(&digest, scratch.palette_snapshot, sizeof(scratch.palette_snapshot));
  genesis_sha256_final(&digest, scratch.frame_digest);

  *frame_out = scratch;
  return 0;
}
