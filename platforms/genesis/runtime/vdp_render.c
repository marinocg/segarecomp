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

/* --- SEG-007-T049 checkpoint C2 (generalized by SEG-055): plane/nametable addressing and bound
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

/* SEG-055: H32 is the documented `00` RS1/RS0 combination; the mixed `01`/`10` combinations are reserved. */
int genesis_vdp_hres_is_h32(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  return (int)((registers[12] & (GENESIS_VDP_REG12_RS0_BIT | GENESIS_VDP_REG12_RS1_BIT)) == 0U);
}

int genesis_vdp_interlace_is_none(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  int lsm1 = (registers[12] & GENESIS_VDP_REG12_LSM1_BIT) != 0U;
  int lsm0 = (registers[12] & GENESIS_VDP_REG12_LSM0_BIT) != 0U;
  return (int)(!lsm1 && !lsm0);
}

/* SEG-055: interlace mode 2 ("double resolution", 8x16 cells) is LSM1 = LSM0 = 1. */
int genesis_vdp_interlace_is_mode2(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  int lsm1 = (registers[12] & GENESIS_VDP_REG12_LSM1_BIT) != 0U;
  int lsm0 = (registers[12] & GENESIS_VDP_REG12_LSM0_BIT) != 0U;
  return (int)(lsm1 && lsm0);
}

int genesis_vdp_display_geometry(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                  GenesisVdpDisplayGeometry *geometry_out) {
  if (registers == NULL || geometry_out == NULL) {
    return -1;
  }
  if (!genesis_vdp_mode_is_mode5(registers)) {
    return -1;
  }
  /* Register #1 bit 3 (M2) selects the 30-cell (240-line) display, which exists only on 50 Hz consoles; the NTSC
   * schedule here has 224 active lines, so the combination is not representable and stays rejected. */
  if ((registers[1] & GENESIS_VDP_REG1_V30_BIT) != 0U) {
    return -1;
  }
  const int h40 = genesis_vdp_hres_is_h40(registers);
  if (!h40 && !genesis_vdp_hres_is_h32(registers)) {
    return -1; /* RS0/RS1 reserved combination. */
  }
  const int im2 = genesis_vdp_interlace_is_mode2(registers);
  if (!im2 && !genesis_vdp_interlace_is_none(registers)) {
    return -1; /* interlace mode 1 and the reserved LSM combination are not implemented. */
  }
  geometry_out->h40 = (unsigned)h40;
  geometry_out->im2 = (unsigned)im2;
  geometry_out->width = h40 ? 320U : 256U;
  geometry_out->height = im2 ? 448U : 224U;
  geometry_out->cell_height = im2 ? 16U : 8U;
  return 0;
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

/* SEG-055: register #11 bits 1-0 == `10`: one horizontal-scroll entry per 8-line cell row. */
int genesis_vdp_hscroll_mode_is_cell(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  return (int)((registers[11] & GENESIS_VDP_HSCROLL_MODE_MASK) == GENESIS_VDP_HSCROLL_MODE_CELL);
}

int genesis_vdp_vscroll_mode_is_2cell(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  if (registers == NULL) {
    return 0;
  }
  return (int)(((registers[11] >> 2) & 0x01U) == 1U);
}

/* Sign-extends the documented low-`bits` two's-complement h/v scroll field
 * of a raw 16-bit scroll word into a plain signed value. Horizontal and
 * ordinary vertical scroll are 10 bits (-512..511); interlace mode 2 vertical
 * scroll is 11 bits (-1024..1023). */
static int32_t genesis_vdp_sign_extend_scroll_bits(uint16_t raw_word, unsigned bits) {
  const uint32_t mask = (1U << bits) - 1U;
  const uint32_t sign = 1U << (bits - 1U);
  uint32_t masked = (uint32_t)raw_word & mask;
  if ((masked & sign) != 0U) {
    return (int32_t)masked - (int32_t)(1U << bits);
  }
  return (int32_t)masked;
}

/* Horizontal-scroll table entry index for one OUTPUT row (SEG-055). The table is indexed by FIELD line: output row
 * `row` in interlace mode 2 belongs to field line `row / 2`. Mode `00` always uses entry 0, `10` the first line of
 * each 8-line group, `11` the line itself; the prohibited mode `01` fails closed. */
static int genesis_vdp_hscroll_table_entry(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT], unsigned row,
                                           uint32_t *entry_out) {
  const int im2 = genesis_vdp_interlace_is_mode2(registers);
  const unsigned field_line = im2 ? row / 2U : row;
  if (genesis_vdp_hscroll_mode_is_fullscreen(registers)) {
    *entry_out = 0U;
    return 0;
  }
  if (!genesis_vdp_hscroll_mode_is_line(registers) && !genesis_vdp_hscroll_mode_is_cell(registers)) {
    return -1;
  }
  /* Reject untrusted off-display values before converting them into table-row arithmetic, which would otherwise
   * permit unsigned multiplication to wrap and alias a valid VRAM row. */
  if (field_line >= GENESIS_FRAME_HEIGHT) {
    return -1;
  }
  *entry_out = genesis_vdp_hscroll_mode_is_cell(registers) ? (uint32_t)(field_line & ~7U) : (uint32_t)field_line;
  return 0;
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
  uint32_t row;
  if (genesis_vdp_hscroll_table_entry(registers, screen_y, &row) != 0) {
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
  *value_out = genesis_vdp_sign_extend_scroll_bits(raw_word, GENESIS_VDP_HSCROLL_VSCROLL_SIGN_BITS);
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
  *value_out = genesis_vdp_sign_extend_scroll_bits(
      raw_word, genesis_vdp_interlace_is_mode2(registers) ? GENESIS_VDP_VSCROLL_IM2_SIGN_BITS
                                                           : GENESIS_VDP_HSCROLL_VSCROLL_SIGN_BITS);
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
  GenesisVdpDisplayGeometry geometry;
  if (genesis_vdp_display_geometry(registers, &geometry) != 0) {
    return -1;
  }
  unsigned width_cells = 0U;
  unsigned height_cells = 0U;
  if (genesis_vdp_decode_plane_size(registers, &width_cells, &height_cells) != 0) {
    return -1;
  }
  unsigned width_px = width_cells * GENESIS_VDP_TILE_WIDTH;
  unsigned height_px = height_cells * geometry.cell_height;

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
  unsigned cell_row = plane_y / geometry.cell_height;

  GenesisVdpNametableEntry entry;
  if (genesis_vdp_read_nametable_entry(vram, registers, plane, cell_col, cell_row, &entry) != 0) {
    return -1;
  }

  location_out->cell_col = cell_col;
  location_out->cell_row = cell_row;
  location_out->tile_x = plane_x % GENESIS_VDP_TILE_WIDTH;
  location_out->tile_y = plane_y % geometry.cell_height;
  location_out->entry = entry;
  return 0;
}

/* --- SEG-007-T049 checkpoint C3 (generalized by SEG-055): sprite-attribute-table decoding and
 * placement. See vdp_render.h for the cited public sources. --- */

static unsigned genesis_vdp_sat_max_sprites(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]) {
  return genesis_vdp_hres_is_h40(registers) ? GENESIS_VDP_SAT_H40_MAX_SPRITES : GENESIS_VDP_SAT_H32_MAX_SPRITES;
}

int genesis_vdp_sat_base_address(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                  uint32_t *base_addr_out) {
  if (registers == NULL || base_addr_out == NULL) {
    return -1;
  }
  if (genesis_vdp_hres_is_h40(registers)) {
    /* H40: register #5 bit 0 is ignored -- 1024-byte SAT-base granularity (the 80-entry table is 640 bytes). */
    *base_addr_out = ((uint32_t)registers[5] << 9) & GENESIS_VDP_REG5_SAT_BASE_H40_MASK;
    return 0;
  }
  if (genesis_vdp_hres_is_h32(registers)) {
    /* H32: all of register #5 bits 6-0 are significant -- 512-byte granularity (the 64-entry table is 512 bytes). */
    *base_addr_out = ((uint32_t)registers[5] << 9) & GENESIS_VDP_REG5_SAT_BASE_H32_MASK;
    return 0;
  }
  return -1; /* reserved RS0/RS1 combination */
}

GenesisVdpSpriteEntry genesis_vdp_decode_sprite_entry_ex(const uint8_t raw[GENESIS_VDP_SPRITE_ENTRY_BYTES],
                                                         int interlace_mode2) {
  GenesisVdpSpriteEntry entry;
  uint16_t word0 = (uint16_t)(((uint16_t)raw[0] << 8) | (uint16_t)raw[1]);
  uint8_t size_byte = raw[2];
  uint8_t link_byte = raw[3];
  uint16_t word4 = (uint16_t)(((uint16_t)raw[4] << 8) | (uint16_t)raw[5]);
  uint16_t word6 = (uint16_t)(((uint16_t)raw[6] << 8) | (uint16_t)raw[7]);

  /* Interlace mode 2 doubles the vertical coordinate system: the Y field is 10 bits and its bias is 2 * 128. */
  uint16_t raw_y = (uint16_t)(word0 & (interlace_mode2 ? GENESIS_VDP_SPRITE_Y_MASK_IM2 : GENESIS_VDP_SPRITE_Y_MASK));
  uint16_t raw_x = (uint16_t)(word6 & GENESIS_VDP_SPRITE_X_MASK);
  entry.y = (int32_t)raw_y - (interlace_mode2 ? 2 * GENESIS_VDP_SPRITE_COORD_BIAS : GENESIS_VDP_SPRITE_COORD_BIAS);
  entry.x = (int32_t)raw_x - GENESIS_VDP_SPRITE_COORD_BIAS;

  entry.width_cells = (unsigned)((size_byte >> 2) & 0x03U) + 1U;
  entry.height_cells = (unsigned)(size_byte & 0x03U) + 1U;
  entry.link = (unsigned)(link_byte & GENESIS_VDP_SPRITE_LINK_MASK);

  entry.tile_index = (uint16_t)(word4 & GENESIS_VDP_TILE_INDEX_MASK);
  entry.h_flip = (uint8_t)((word4 >> 11) & 0x01U);
  entry.v_flip = (uint8_t)((word4 >> 12) & 0x01U);
  entry.palette = (uint8_t)((word4 >> 13) & 0x03U);
  entry.priority = (uint8_t)((word4 >> 15) & 0x01U);
  entry.interlace_mode2 = (uint8_t)(interlace_mode2 ? 1U : 0U);
  return entry;
}

GenesisVdpSpriteEntry genesis_vdp_decode_sprite_entry(
    const uint8_t raw[GENESIS_VDP_SPRITE_ENTRY_BYTES]) {
  return genesis_vdp_decode_sprite_entry_ex(raw, 0);
}

int genesis_vdp_read_sprite_entry(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                   const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                   unsigned sprite_index, GenesisVdpSpriteEntry *entry_out) {
  if (vram == NULL || registers == NULL || entry_out == NULL) {
    return -1;
  }
  uint32_t sat_base = 0U;
  if (genesis_vdp_sat_base_address(registers, &sat_base) != 0) {
    return -1;
  }
  if (sprite_index >= genesis_vdp_sat_max_sprites(registers)) {
    return -1;
  }
  uint32_t addr = sat_base + (uint32_t)sprite_index * GENESIS_VDP_SPRITE_ENTRY_BYTES;
  if (addr + GENESIS_VDP_SPRITE_ENTRY_BYTES > GENESIS_VDP_VRAM_BYTES) {
    return -1;
  }
  *entry_out = genesis_vdp_decode_sprite_entry_ex(&vram[addr], genesis_vdp_interlace_is_mode2(registers));
  return 0;
}

int genesis_vdp_sat_traverse(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                              const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                              unsigned visited_out[GENESIS_VDP_SAT_H40_MAX_SPRITES],
                              unsigned *visited_count_out) {
  if (vram == NULL || registers == NULL || visited_out == NULL || visited_count_out == NULL) {
    return -1;
  }
  const unsigned max_sprites = genesis_vdp_sat_max_sprites(registers);

  unsigned visited_flags[GENESIS_VDP_SAT_H40_MAX_SPRITES];
  for (unsigned i = 0; i < GENESIS_VDP_SAT_H40_MAX_SPRITES; ++i) {
    visited_flags[i] = 0U;
  }

  unsigned count = 0U;
  unsigned current = 0U; /* Sprite 0 is always first (see citation). */

  for (unsigned step = 0; step < max_sprites; ++step) {
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
    if (entry.link >= max_sprites) {
      /* Out-of-range link for this SAT: treated as implicit end-of-list
       * rather than followed (see citation above). */
      break;
    }
    current = entry.link;
  }

  *visited_count_out = count;
  return 0;
}

static unsigned genesis_vdp_sprite_cell_height_px(const GenesisVdpSpriteEntry *sprite) {
  return sprite->interlace_mode2 ? 2U * GENESIS_VDP_TILE_HEIGHT : GENESIS_VDP_TILE_HEIGHT;
}

int genesis_vdp_sprite_covers_pixel(const GenesisVdpSpriteEntry *sprite, unsigned screen_x,
                                     unsigned screen_y) {
  if (sprite == NULL) {
    return 0;
  }
  int64_t x = (int64_t)screen_x;
  int64_t y = (int64_t)screen_y;
  int64_t width_px = (int64_t)sprite->width_cells * (int64_t)GENESIS_VDP_TILE_WIDTH;
  int64_t height_px = (int64_t)sprite->height_cells * (int64_t)genesis_vdp_sprite_cell_height_px(sprite);
  if (x < sprite->x || x >= sprite->x + width_px) {
    return 0;
  }
  if (y < sprite->y || y >= sprite->y + height_px) {
    return 0;
  }
  return 1;
}

/* One pattern pixel of cell (`cell_col`, `cell_row`) of `sprite` in sprite-local pattern space (after flipping). In
 * interlace mode 2 a pattern is 8x16 (64 bytes, rows stored consecutively). Every address wraps in the 64 KiB VRAM. */
static uint8_t genesis_vdp_sprite_pattern_pixel(const GenesisVdpSpriteEntry *sprite,
                                                const uint8_t vram[GENESIS_VDP_VRAM_BYTES], unsigned mirrored_x,
                                                unsigned mirrored_y) {
  const unsigned cell_h = genesis_vdp_sprite_cell_height_px(sprite);
  const unsigned cell_col = mirrored_x / GENESIS_VDP_TILE_WIDTH;
  const unsigned cell_row = mirrored_y / cell_h;
  const unsigned tile_col = mirrored_x % GENESIS_VDP_TILE_WIDTH;
  const unsigned tile_row = mirrored_y % cell_h;
  /* Column-major multi-cell tile order (see citation); the 11-bit tile number wraps. */
  const uint32_t tile_offset = (uint32_t)cell_col * (uint32_t)sprite->height_cells + (uint32_t)cell_row;
  const uint32_t effective_tile = ((uint32_t)sprite->tile_index + tile_offset) & GENESIS_VDP_TILE_INDEX_MASK;
  const uint32_t tile_bytes = sprite->interlace_mode2 ? 2U * GENESIS_VDP_TILE_BYTES : GENESIS_VDP_TILE_BYTES;
  const uint32_t addr = (effective_tile * tile_bytes + tile_row * 4U + tile_col / 2U) & (GENESIS_VDP_VRAM_BYTES - 1U);
  const uint8_t byte_value = vram[addr];
  return (tile_col % 2U == 0U) ? (uint8_t)(byte_value >> 4) : (uint8_t)(byte_value & 0x0FU);
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
  unsigned height_px = sprite->height_cells * genesis_vdp_sprite_cell_height_px(sprite);

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

  *index_out = genesis_vdp_sprite_pattern_pixel(sprite, vram, mirrored_x, mirrored_y);
  return 0;
}

/* --- SEG-007-T049 checkpoint C4 (generalized by SEG-055): priority/transparency/composition. See
 * vdp_render.h for the cited public sources. --- */

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

/* Per-render context: the display geometry, the (frame-constant) plane dimensions and the immutable VRAM. The mutable
 * per-scanline state (registers/VSRAM/CRAM) is passed per row as a GenesisRasterLine. */
typedef struct GenesisVdpRenderContext {
  const uint8_t *vram;
  GenesisVdpDisplayGeometry geometry;
} GenesisVdpRenderContext;

/* Validates everything a row needs that could otherwise fail mid-frame (so rendering itself is infallible): display
 * mode, plane size and horizontal-scroll mode. */
static int genesis_vdp_validate_line(const GenesisRasterLine *line, GenesisVdpDisplayGeometry *geometry_out) {
  unsigned width_cells = 0U;
  unsigned height_cells = 0U;
  uint32_t entry = 0U;
  if (genesis_vdp_display_geometry(line->registers, geometry_out) != 0) return -1;
  if (genesis_vdp_decode_plane_size(line->registers, &width_cells, &height_cells) != 0) return -1;
  if (genesis_vdp_hscroll_table_entry(line->registers, 0U, &entry) != 0) return -1;
  return 0;
}

static unsigned genesis_vdp_be16(const uint8_t *vram, uint32_t addr) {
  return ((unsigned)vram[addr & 0xFFFFU] << 8) | (unsigned)vram[(addr + 1U) & 0xFFFFU];
}

/* One plane/window nametable entry + tile row -> a candidate pixel. `tile_x`/`tile_y` are pre-flip pattern
 * coordinates (tile_y in [0, cell_height)). */
static GenesisVdpCandidatePixel genesis_vdp_fetch_cell_pixel(const GenesisVdpRenderContext *ctx,
                                                             GenesisVdpNametableEntry entry, unsigned tile_x,
                                                             unsigned tile_y) {
  const unsigned cell_h = ctx->geometry.cell_height;
  GenesisVdpCandidatePixel candidate;
  if (entry.h_flip) tile_x = GENESIS_VDP_TILE_WIDTH - 1U - tile_x;
  if (entry.v_flip) tile_y = cell_h - 1U - tile_y;
  {
    const uint32_t tile_bytes = ctx->geometry.im2 ? 2U * GENESIS_VDP_TILE_BYTES : GENESIS_VDP_TILE_BYTES;
    const uint32_t addr = ((uint32_t)entry.tile_index * tile_bytes + tile_y * 4U + tile_x / 2U) &
                          (GENESIS_VDP_VRAM_BYTES - 1U);
    const uint8_t byte_value = ctx->vram[addr];
    const uint8_t pattern = (tile_x % 2U == 0U) ? (uint8_t)(byte_value >> 4) : (uint8_t)(byte_value & 0x0FU);
    candidate.opaque = (uint8_t)(pattern != GENESIS_VDP_TRANSPARENT_PALETTE_INDEX);
    candidate.priority = entry.priority;
    candidate.palette = entry.palette;
    candidate.index = pattern;
  }
  return candidate;
}

/* The window covers an on-screen pixel when it is inside the vertical window range OR inside the horizontal one
 * (register #17 / #18, see the Window citation in vdp_render.h). Boundaries are 2 cells (16 px) horizontally and 1
 * cell (8 output rows; 16 in interlace mode 2) vertically; bit 7 of each register mirrors the window to the
 * far side of the boundary. */
static int genesis_vdp_window_covers(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                     const GenesisVdpDisplayGeometry *geometry, unsigned x, unsigned row) {
  const unsigned vboundary = (unsigned)(registers[18] & 0x1FU) * geometry->cell_height;
  const int vbelow = (registers[18] & 0x80U) != 0U;
  const unsigned hboundary = (unsigned)(registers[17] & 0x1FU);
  const int hright = (registers[17] & 0x80U) != 0U;
  const int vertical = vbelow ? (row >= vboundary) : (row < vboundary);
  const int horizontal = hright ? ((x >> 4) >= hboundary) : ((x >> 4) < hboundary);
  return vertical || horizontal;
}

/* The Window nametable: register #3 bits 5-1 (bit 1 ignored in H40: a 64-cell-wide table is 4 KiB aligned). Its row
 * stride is the display width in cells (64 in H40, 32 in H32); it never scrolls. */
static GenesisVdpNametableEntry genesis_vdp_window_entry(const GenesisVdpRenderContext *ctx,
                                                         const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                                         unsigned x, unsigned row) {
  const uint32_t mask = ctx->geometry.h40 ? 0xF000U : 0xF800U;
  const uint32_t base = ((uint32_t)registers[3] << 10) & mask;
  const unsigned stride_cells = ctx->geometry.h40 ? 64U : 32U;
  const unsigned cell_col = x / GENESIS_VDP_TILE_WIDTH;
  const unsigned cell_row = row / ctx->geometry.cell_height;
  const uint32_t addr = base + ((uint32_t)cell_row * stride_cells + cell_col) * 2U;
  return genesis_vdp_decode_nametable_word((uint16_t)genesis_vdp_be16(ctx->vram, addr));
}

/* Per-row constants of one scroll plane, resolved once per output row (the per-pixel path is then plain arithmetic). */
typedef struct GenesisVdpRowPlane {
  uint32_t base;
  unsigned width_cells;
  unsigned width_px;
  unsigned height_px;
  int32_t hscroll;
  int per_column;                                  /* 2-cell-column vertical scroll */
  int32_t vscroll[GENESIS_VDP_VSRAM_GROUP_COUNT];  /* [0] only unless per_column */
} GenesisVdpRowPlane;

static void genesis_vdp_prepare_row_plane(const GenesisVdpRenderContext *ctx, const GenesisRasterLine *line,
                                          GenesisVdpPlaneSelector plane, unsigned row, GenesisVdpRowPlane *out) {
  unsigned width_cells = 0U;
  unsigned height_cells = 0U;
  (void)genesis_vdp_decode_plane_size(line->registers, &width_cells, &height_cells);
  (void)genesis_vdp_plane_base_address(line->registers, plane, &out->base);
  (void)genesis_vdp_read_hscroll(ctx->vram, line->registers, plane, row, &out->hscroll);
  out->width_cells = width_cells;
  out->width_px = width_cells * GENESIS_VDP_TILE_WIDTH;
  out->height_px = height_cells * ctx->geometry.cell_height;
  out->per_column = genesis_vdp_vscroll_mode_is_2cell(line->registers);
  for (unsigned g = 0U; g < (out->per_column ? (unsigned)GENESIS_VDP_VSRAM_GROUP_COUNT : 1U); ++g) {
    out->vscroll[g] = 0;
    /* A group beyond the 20 hardware entries cannot occur on screen (H40 has exactly 20); entry 0 otherwise. */
    (void)genesis_vdp_read_vscroll(line->vsram, line->registers, plane, g * 2U, &out->vscroll[g]);
  }
}

/* Plane A or B candidate at (x, row) from the plane's per-row constants. */
static GenesisVdpCandidatePixel genesis_vdp_plane_candidate(const GenesisVdpRenderContext *ctx,
                                                            const GenesisVdpRowPlane *plane, unsigned x,
                                                            unsigned row) {
  const unsigned group = plane->per_column ? x / 16U : 0U;
  const int32_t vscroll = plane->vscroll[group < GENESIS_VDP_VSRAM_GROUP_COUNT ? group : 0U];
  const unsigned plane_x = genesis_vdp_plane_x(x, plane->hscroll, plane->width_px);
  const unsigned plane_y = genesis_vdp_plane_y(row, vscroll, plane->height_px);
  const unsigned cell_col = plane_x / GENESIS_VDP_TILE_WIDTH;
  const unsigned cell_row = plane_y / ctx->geometry.cell_height;
  const uint32_t addr = plane->base + ((uint32_t)cell_row * plane->width_cells + cell_col) * 2U;
  const GenesisVdpNametableEntry entry =
      genesis_vdp_decode_nametable_word((uint16_t)genesis_vdp_be16(ctx->vram, addr));
  return genesis_vdp_fetch_cell_pixel(ctx, entry, plane_x % GENESIS_VDP_TILE_WIDTH,
                                      plane_y % ctx->geometry.cell_height);
}

/* The sprites that reach one output row, with the hardware per-line limits (H40: 20 sprites / 40 cells; H32: 16 / 32)
 * applied in list order, written as the first-opaque-wins sprite candidate of every x. See the Sprite-limit citation
 * in vdp_render.h. */
static int genesis_vdp_build_sprite_row(const GenesisVdpRenderContext *ctx, const GenesisRasterLine *line,
                                        unsigned row, GenesisVdpCandidatePixel *out) {
  const unsigned width = ctx->geometry.width;
  const unsigned max_sprites = ctx->geometry.h40 ? GENESIS_VDP_SAT_H40_MAX_SPRITES : GENESIS_VDP_SAT_H32_MAX_SPRITES;
  const unsigned per_line_limit = ctx->geometry.h40 ? 20U : 16U;
  const unsigned cell_budget_start = ctx->geometry.h40 ? 40U : 32U;
  GenesisVdpSpriteEntry selected[20];
  unsigned selected_count = 0U;
  unsigned visited[GENESIS_VDP_SAT_H40_MAX_SPRITES] = {0};
  uint32_t sat_base = 0U;
  unsigned current = 0U;
  unsigned cell_budget = cell_budget_start;
  memset(out, 0, (size_t)width * sizeof(out[0]));
  if (genesis_vdp_sat_base_address(line->registers, &sat_base) != 0) return -1;
  for (unsigned step = 0U; step < max_sprites; ++step) {
    GenesisVdpSpriteEntry entry;
    const uint32_t addr = sat_base + (uint32_t)current * GENESIS_VDP_SPRITE_ENTRY_BYTES;
    int64_t top;
    int64_t bottom;
    if (visited[current] != 0U || addr + GENESIS_VDP_SPRITE_ENTRY_BYTES > GENESIS_VDP_VRAM_BYTES) break;
    visited[current] = 1U;
    entry = genesis_vdp_decode_sprite_entry_ex(&ctx->vram[addr], ctx->geometry.im2 ? 1 : 0);
    top = entry.y;
    bottom = top + (int64_t)entry.height_cells * (int64_t)ctx->geometry.cell_height;
    if ((int64_t)row >= top && (int64_t)row < bottom) {
      if (selected_count == per_line_limit) break; /* sprite overflow: the rest of the list is not drawn */
      selected[selected_count++] = entry;
    }
    if (entry.link == 0U || entry.link >= max_sprites) break;
    current = entry.link;
  }
  for (unsigned s = 0U; s < selected_count && cell_budget != 0U; ++s) {
    const GenesisVdpSpriteEntry *sprite = &selected[s];
    const unsigned width_px = sprite->width_cells * GENESIS_VDP_TILE_WIDTH;
    const unsigned height_px = sprite->height_cells * ctx->geometry.cell_height;
    unsigned drawn_cells = sprite->width_cells < cell_budget ? sprite->width_cells : cell_budget;
    unsigned local_y = (unsigned)((int64_t)row - sprite->y);
    unsigned mirrored_y = sprite->v_flip ? (height_px - 1U - local_y) : local_y;
    cell_budget -= drawn_cells;
    for (unsigned local_x = 0U; local_x < drawn_cells * GENESIS_VDP_TILE_WIDTH; ++local_x) {
      const int64_t screen_x = (int64_t)sprite->x + (int64_t)local_x;
      unsigned mirrored_x;
      uint8_t pattern;
      if (screen_x < 0 || screen_x >= (int64_t)width || out[screen_x].opaque) continue;
      mirrored_x = sprite->h_flip ? (width_px - 1U - local_x) : local_x;
      pattern = genesis_vdp_sprite_pattern_pixel(sprite, ctx->vram, mirrored_x, mirrored_y);
      if (pattern == GENESIS_VDP_TRANSPARENT_PALETTE_INDEX) continue;
      out[screen_x].opaque = 1U;
      out[screen_x].priority = sprite->priority;
      out[screen_x].palette = sprite->palette;
      out[screen_x].index = pattern;
    }
  }
  return 0;
}

/* Six-layer priority order, highest to lowest: sprite-high, planeA/window-high, planeB-high, sprite-low,
 * planeA/window-low, planeB-low, then the backdrop -- see the priority citation in vdp_render.h. */
static unsigned genesis_vdp_select_cram_index(const GenesisVdpCandidatePixel *plane_a,
                                              const GenesisVdpCandidatePixel *plane_b,
                                              const GenesisVdpCandidatePixel *sprite, unsigned backdrop) {
  const GenesisVdpCandidatePixel *winner = NULL;
  if (sprite->opaque && sprite->priority) {
    winner = sprite;
  } else if (plane_a->opaque && plane_a->priority) {
    winner = plane_a;
  } else if (plane_b->opaque && plane_b->priority) {
    winner = plane_b;
  } else if (sprite->opaque && !sprite->priority) {
    winner = sprite;
  } else if (plane_a->opaque && !plane_a->priority) {
    winner = plane_a;
  } else if (plane_b->opaque && !plane_b->priority) {
    winner = plane_b;
  }
  if (winner != NULL) return genesis_vdp_cram_index(winner->palette, winner->index);
  return backdrop;
}

/* Renders one output row of CRAM indices from the scanline state `line`. The state must have passed
 * genesis_vdp_validate_line. */
static void genesis_vdp_render_row(const GenesisVdpRenderContext *ctx, const GenesisRasterLine *line, unsigned row,
                                   uint8_t *pixels_out) {
  GenesisVdpCandidatePixel sprites[GENESIS_FRAME_MAX_WIDTH];
  GenesisVdpRowPlane plane_a_state;
  GenesisVdpRowPlane plane_b_state;
  const unsigned width = ctx->geometry.width;
  unsigned backdrop = 0U;
  (void)genesis_vdp_backdrop_cram_index(line->registers, &backdrop);
  genesis_vdp_prepare_row_plane(ctx, line, GENESIS_VDP_PLANE_A, row, &plane_a_state);
  genesis_vdp_prepare_row_plane(ctx, line, GENESIS_VDP_PLANE_B, row, &plane_b_state);
  (void)genesis_vdp_build_sprite_row(ctx, line, row, sprites);
  for (unsigned x = 0U; x < width; ++x) {
    GenesisVdpCandidatePixel plane_a;
    GenesisVdpCandidatePixel plane_b;
    if ((line->registers[0] & GENESIS_VDP_REG0_LEFT_BLANK_BIT) != 0U && x < GENESIS_VDP_TILE_WIDTH) {
      pixels_out[x] = (uint8_t)backdrop; /* left 8 pixels masked to the backdrop color */
      continue;
    }
    if (genesis_vdp_window_covers(line->registers, &ctx->geometry, x, row)) {
      plane_a = genesis_vdp_fetch_cell_pixel(ctx, genesis_vdp_window_entry(ctx, line->registers, x, row),
                                             x % GENESIS_VDP_TILE_WIDTH, row % ctx->geometry.cell_height);
    } else {
      plane_a = genesis_vdp_plane_candidate(ctx, &plane_a_state, x, row);
    }
    plane_b = genesis_vdp_plane_candidate(ctx, &plane_b_state, x, row);
    pixels_out[x] = (uint8_t)genesis_vdp_select_cram_index(&plane_a, &plane_b, &sprites[x], backdrop);
  }
}

int genesis_vdp_compose_pixel(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                               const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                               const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                               const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                               unsigned screen_x, unsigned screen_y, GenesisRgb888 *color_out) {
  if (vram == NULL || vsram == NULL || cram == NULL || registers == NULL || color_out == NULL) {
    return -1;
  }
  GenesisRasterLine line;
  GenesisVdpRenderContext ctx;
  uint8_t row_pixels[GENESIS_FRAME_MAX_WIDTH];
  memcpy(line.registers, registers, sizeof(line.registers));
  memcpy(line.cram, cram, sizeof(line.cram));
  memcpy(line.vsram, vsram, sizeof(line.vsram));
  if (genesis_vdp_validate_line(&line, &ctx.geometry) != 0) {
    return -1;
  }
  if (screen_x >= ctx.geometry.width || screen_y >= ctx.geometry.height) {
    return -1;
  }
  ctx.vram = vram;
  genesis_vdp_render_row(&ctx, &line, screen_y, row_pixels);
  return genesis_vdp_decode_cram_entry(cram, row_pixels[screen_x], color_out);
}

/* --- SEG-007-T049 checkpoint C5 (generalized by SEG-055): deterministic frame output. See
 * vdp_render.h for the applied (not redefined) T042 schema and cited behavior. --- */

int genesis_vdp_frame_is_legacy_shape(const GenesisFrameArtifact *frame) {
  return GENESIS_FRAME_ARTIFACT_WIDTH(frame) == GENESIS_FRAME_WIDTH &&
         GENESIS_FRAME_ARTIFACT_HEIGHT(frame) == GENESIS_FRAME_HEIGHT && frame->palette_count <= 1U;
}

void genesis_vdp_frame_digest(const GenesisFrameArtifact *frame, uint8_t digest_out[32]) {
  const size_t pixel_count =
      (size_t)GENESIS_FRAME_ARTIFACT_WIDTH(frame) * (size_t)GENESIS_FRAME_ARTIFACT_HEIGHT(frame);
  GenesisSha256 digest;
  genesis_sha256_init(&digest);
  genesis_sha256_update(&digest, frame->pixels, (uint32_t)pixel_count);
  genesis_sha256_update(&digest, frame->palette_snapshot, sizeof(frame->palette_snapshot));
  if (!genesis_vdp_frame_is_legacy_shape(frame)) {
    /* Every non-legacy shape additionally covers its geometry and every per-row palette so two frames that differ
     * only in resolution or in a mid-frame palette switch can never share a digest. */
    uint8_t header[6];
    const unsigned width = GENESIS_FRAME_ARTIFACT_WIDTH(frame);
    const unsigned height = GENESIS_FRAME_ARTIFACT_HEIGHT(frame);
    header[0] = (uint8_t)(width >> 8);
    header[1] = (uint8_t)width;
    header[2] = (uint8_t)(height >> 8);
    header[3] = (uint8_t)height;
    header[4] = (uint8_t)(frame->palette_count >> 8);
    header[5] = (uint8_t)frame->palette_count;
    genesis_sha256_update(&digest, header, sizeof(header));
    genesis_sha256_update(&digest, frame->row_palette, (uint32_t)height);
    if (frame->palette_count > 1U) {
      genesis_sha256_update(&digest, frame->extra_palettes[0], (uint32_t)((frame->palette_count - 1U) * GENESIS_VDP_CRAM_BYTES));
    }
  }
  genesis_sha256_final(&digest, digest_out);
}

/* The shared frame producer: `raster` holds one GenesisRasterLine per active field line (224) plus the optional VRAM
 * journal, or is NULL with `uniform` describing a single static state for every line (the legacy snapshot producer).
 * Validation happens entirely up front, so a failure leaves `*frame_out` untouched and a success is never partial. */
static int genesis_vdp_render_frame(const uint8_t *vram, const GenesisRasterFrameState *raster,
                                    const GenesisRasterLine *uniform, GenesisFrameArtifact *frame_out) {
  GenesisVdpRenderContext ctx;
  GenesisVdpDisplayGeometry first;
  unsigned slot_of_line[GENESIS_RASTER_MAX_LINES];
  uint8_t work_vram[GENESIS_VDP_VRAM_BYTES];
  const GenesisRasterLine *lines = raster != NULL ? raster->lines : NULL;
  const int journaled = raster != NULL && raster->has_vram_journal != 0U &&
                        raster->vram_write_count <= GENESIS_RASTER_VRAM_WRITE_CAPACITY;
  unsigned applied = 0U;
  unsigned palette_count = 0U;
  const unsigned line_count = lines != NULL ? GENESIS_RASTER_MAX_LINES : 1U;
  if (genesis_vdp_validate_line(lines != NULL ? &lines[0] : uniform, &first) != 0) return -1;
  for (unsigned i = 1U; i < line_count; ++i) {
    GenesisVdpDisplayGeometry other;
    if (genesis_vdp_validate_line(&lines[i], &other) != 0) return -1;
    /* A display-mode change in the middle of the active period cannot be represented by one rectangular frame. */
    if (other.width != first.width || other.height != first.height) return -1;
  }
  ctx.vram = vram;
  ctx.geometry = first;
  if (journaled) {
    /* VRAM as of line 0; the journal is replayed as the rows advance (a write made during field line k is visible from
       line k + 1 on). */
    memcpy(work_vram, raster->vram_start, sizeof(work_vram));
    ctx.vram = work_vram;
  }
  /* The artifact is a pure function of the inputs: everything beyond the active geometry/palettes is zeroed. */
  memset(frame_out->pixels + (size_t)first.width * first.height, 0,
         sizeof(frame_out->pixels) - (size_t)first.width * first.height);
  memset(frame_out->row_palette, 0, sizeof(frame_out->row_palette));
  memset(frame_out->palette_snapshot, 0, sizeof(frame_out->palette_snapshot));
  memset(frame_out->extra_palettes, 0, sizeof(frame_out->extra_palettes));
  for (unsigned i = 0U; i < GENESIS_RASTER_MAX_LINES; ++i) {
    const GenesisRasterLine *line = lines != NULL ? &lines[i] : uniform;
    unsigned slot = palette_count;
    /* A palette identical to an earlier line's reuses its slot; the previous line is by far the likeliest match. */
    for (unsigned candidate = palette_count; candidate-- > 0U;) {
      const uint8_t *existing = candidate == 0U ? frame_out->palette_snapshot : frame_out->extra_palettes[candidate - 1U];
      if (memcmp(existing, line->cram, GENESIS_VDP_CRAM_BYTES) == 0) {
        slot = candidate;
        break;
      }
    }
    if (slot == palette_count) {
      /* GENESIS_FRAME_MAX_PALETTES (256) > GENESIS_RASTER_MAX_LINES (224): a new slot always fits. */
      uint8_t *target = slot == 0U ? frame_out->palette_snapshot : frame_out->extra_palettes[slot - 1U];
      memcpy(target, line->cram, GENESIS_VDP_CRAM_BYTES);
      ++palette_count;
    }
    slot_of_line[i] = slot;
    if (lines == NULL) break; /* uniform state: one slot covers every line */
  }
  for (unsigned row = 0U; row < first.height; ++row) {
    const unsigned field_line = first.im2 ? row / 2U : row;
    const GenesisRasterLine *line = lines != NULL ? &lines[field_line] : uniform;
    if (journaled) {
      while (applied < raster->vram_write_count && (unsigned)raster->vram_writes[applied].line < field_line) {
        work_vram[raster->vram_writes[applied].address] = raster->vram_writes[applied].value;
        ++applied;
      }
    }
    genesis_vdp_render_row(&ctx, line, row, &frame_out->pixels[(size_t)row * first.width]);
    frame_out->row_palette[row] = (uint8_t)(lines != NULL ? slot_of_line[field_line] : 0U);
  }
  frame_out->width = (uint16_t)first.width;
  frame_out->height = (uint16_t)first.height;
  frame_out->palette_count = (uint16_t)palette_count;
  genesis_vdp_frame_digest(frame_out, frame_out->frame_digest);
  return 0;
}

int genesis_vdp_produce_frame_raster(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                      const GenesisRasterFrameState *raster, GenesisFrameArtifact *frame_out) {
  if (vram == NULL || raster == NULL || frame_out == NULL) {
    return -1;
  }
  return genesis_vdp_render_frame(vram, raster, NULL, frame_out);
}

int genesis_vdp_produce_frame(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                               const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                               const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                               const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                               GenesisFrameArtifact *frame_out) {
  GenesisRasterLine uniform;
  if (vram == NULL || vsram == NULL || cram == NULL || registers == NULL || frame_out == NULL) {
    return -1;
  }
  memcpy(uniform.registers, registers, sizeof(uniform.registers));
  memcpy(uniform.cram, cram, sizeof(uniform.cram));
  memcpy(uniform.vsram, vsram, sizeof(uniform.vsram));
  return genesis_vdp_render_frame(vram, NULL, &uniform, frame_out);
}

/* SEG-055: the single artifact -> RGB conversion shared by every consumer (PPM export, SDL presentation, tests), so
 * headless capture and the window agree by construction. */
/* The CRAM bytes (128) governing one output row (palette slot 0 is `palette_snapshot`, slot k >= 1 is
 * `extra_palettes[k - 1]`), or NULL when the row/slot is outside the artifact. */
static const uint8_t *genesis_vdp_frame_row_palette(const GenesisFrameArtifact *frame, unsigned row) {
  unsigned slot;
  if (row >= GENESIS_FRAME_ARTIFACT_HEIGHT(frame)) return NULL;
  slot = frame->row_palette[row];
  if (slot == 0U) return frame->palette_snapshot;
  if (slot >= GENESIS_FRAME_MAX_PALETTES || slot >= (unsigned)frame->palette_count) return NULL;
  return frame->extra_palettes[slot - 1U];
}

int genesis_vdp_frame_row_rgb888(const GenesisFrameArtifact *frame, unsigned row, uint8_t *rgb_out) {
  if (frame == NULL || rgb_out == NULL) {
    return -1;
  }
  const uint8_t *palette = genesis_vdp_frame_row_palette(frame, row);
  const unsigned width = GENESIS_FRAME_ARTIFACT_WIDTH(frame);
  if (palette == NULL || width > GENESIS_FRAME_MAX_WIDTH ||
      GENESIS_FRAME_ARTIFACT_HEIGHT(frame) > GENESIS_FRAME_MAX_HEIGHT) {
    return -1;
  }
  const uint8_t *pixels = &frame->pixels[(size_t)row * width];
  for (unsigned x = 0U; x < width; ++x) {
    GenesisRgb888 color;
    if (pixels[x] >= GENESIS_VDP_CRAM_ENTRY_COUNT || genesis_vdp_decode_cram_entry(palette, pixels[x], &color) != 0) {
      return -1;
    }
    rgb_out[x * 3U] = color.r;
    rgb_out[x * 3U + 1U] = color.g;
    rgb_out[x * 3U + 2U] = color.b;
  }
  return 0;
}
