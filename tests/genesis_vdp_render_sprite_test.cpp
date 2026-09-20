// SEG-007-T049 checkpoint C3: focused unit coverage for
// platforms/genesis/runtime/vdp_render.c's sprite-attribute-table decoding and
// placement. All fixtures are project-authored synthetic VRAM/register byte
// patterns with hand-computed expected values -- no commercial ROM content
// is read or referenced, and no expected value is computed by calling the
// implementation under test.

#include "vdp_render.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

void make_default_registers(uint16_t regs[GENESIS_VDP_REGISTER_COUNT]) {
  std::memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[5] = 0x28;   // SAT base -> (0x28 << 9) & 0xFC00 = 0x5000.
  regs[12] = 0x81;  // RS0 (bit 7) + RS1 (bit 0) set -> H40, required for the
                     // SAT base to resolve at all (see genesis_vdp_sat_base_address).
}

// Writes one 8-byte sprite entry at `addr` into `vram`.
void write_sprite_entry(uint8_t *vram, uint32_t addr, uint16_t raw_y, uint8_t size_byte,
                         uint8_t link, uint16_t tile_word, uint16_t raw_x) {
  vram[addr + 0] = (uint8_t)(raw_y >> 8);
  vram[addr + 1] = (uint8_t)(raw_y & 0xFF);
  vram[addr + 2] = size_byte;
  vram[addr + 3] = link;
  vram[addr + 4] = (uint8_t)(tile_word >> 8);
  vram[addr + 5] = (uint8_t)(tile_word & 0xFF);
  vram[addr + 6] = (uint8_t)(raw_x >> 8);
  vram[addr + 7] = (uint8_t)(raw_x & 0xFF);
}

void test_sat_base_address() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint32_t base = 0xFFFFFFFFU;
  check(genesis_vdp_sat_base_address(regs, &base) == 0, "SAT base decodes");
  check(base == 0x5000U, "SAT base value (reg5=0x28 -> 0x5000)");

  check(genesis_vdp_sat_base_address(nullptr, &base) < 0, "SAT base rejects NULL registers");
  check(genesis_vdp_sat_base_address(regs, nullptr) < 0, "SAT base rejects NULL output");
}

void test_sat_base_address_h40_bit0_ignored() {
  // Register #5 values 0x70 and 0x71 differ only in bit 0. In H40 that bit
  // is ignored (1024-byte SAT-base granularity), so both must resolve to
  // the same base address.
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);

  regs[5] = 0x70;
  uint32_t base_even = 0xFFFFFFFFU;
  check(genesis_vdp_sat_base_address(regs, &base_even) == 0, "SAT base decodes for reg5=0x70");

  regs[5] = 0x71;
  uint32_t base_odd = 0xFFFFFFFFU;
  check(genesis_vdp_sat_base_address(regs, &base_odd) == 0, "SAT base decodes for reg5=0x71");

  check(base_even == base_odd,
        "H40 SAT base ignores register #5 bit 0 (0x70 and 0x71 resolve identically)");
  check(base_even == (((uint32_t)0x70U << 9) & 0xFC00U),
        "sanity: base matches (reg5<<9)&0xFC00");

  // Highest permitted H40 base: maximal reg5 bit pattern once bit 0 is
  // masked out is 0x7E (0111 1110). Confirm all 80 possible 8-byte sprite
  // entries still fit within GENESIS_VDP_VRAM_BYTES.
  regs[5] = 0x7E;
  uint32_t max_base = 0xFFFFFFFFU;
  check(genesis_vdp_sat_base_address(regs, &max_base) == 0, "SAT base decodes for max reg5=0x7E");
  uint64_t sat_end = (uint64_t)max_base + (uint64_t)GENESIS_VDP_SAT_H40_MAX_SPRITES *
                                               GENESIS_VDP_SPRITE_ENTRY_BYTES;
  check(sat_end <= GENESIS_VDP_VRAM_BYTES,
        "highest H40 SAT base keeps all 80 sprite entries within VRAM bounds");
}

void test_sat_base_address_fails_closed_without_h40() {
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  regs[12] = 0x00; // Neither RS0 nor RS1 set -> H32, not H40.

  uint32_t base = 0xDEADBEEFU; // sentinel pattern.
  check(genesis_vdp_sat_base_address(regs, &base) < 0,
        "SAT base fails closed when H40 is not selected");
  check(base == 0xDEADBEEFU, "SAT base leaves output unmodified on H40 gate failure");
}

void test_sprite_entry_decode_fields() {
  // Hand-constructed entry: raw Y=228 (screen Y = 100), size byte
  // width_field=0 (1 cell), height_field=0 (1 cell) -> size_byte=0x00,
  // link=5, tile word: priority=1, palette=2 (bits 14-13=10), v_flip=1,
  // h_flip=0, tile_index=0x123.
  // tile word bits: P=1<<15, palette(2)=10 -> <<13 = 0x4000, v_flip=1<<12,
  // h_flip=0, tile_index=0x123 (<=0x7FF).
  uint16_t tile_word = (uint16_t)(0x8000U | 0x4000U | 0x1000U | 0x0123U);
  uint16_t raw_y = 228; // 228 - 128 = 100
  uint16_t raw_x = 178; // 178 - 128 = 50
  uint8_t raw[GENESIS_VDP_SPRITE_ENTRY_BYTES];
  raw[0] = (uint8_t)(raw_y >> 8);
  raw[1] = (uint8_t)(raw_y & 0xFF);
  raw[2] = 0x00;
  raw[3] = 5;
  raw[4] = (uint8_t)(tile_word >> 8);
  raw[5] = (uint8_t)(tile_word & 0xFF);
  raw[6] = (uint8_t)(raw_x >> 8);
  raw[7] = (uint8_t)(raw_x & 0xFF);

  GenesisVdpSpriteEntry entry = genesis_vdp_decode_sprite_entry(raw);
  check(entry.y == 100, "sprite Y offset-corrected (228-128=100)");
  check(entry.x == 50, "sprite X offset-corrected (178-128=50)");
  check(entry.width_cells == 1U, "sprite width 1x1 field decodes to 1 cell");
  check(entry.height_cells == 1U, "sprite height 1x1 field decodes to 1 cell");
  check(entry.link == 5U, "sprite link field decodes to 5");
  check(entry.tile_index == 0x123U, "sprite tile_index decodes to 0x123");
  check(entry.h_flip == 0U, "sprite h_flip decodes to 0");
  check(entry.v_flip == 1U, "sprite v_flip decodes to 1");
  check(entry.palette == 2U, "sprite palette decodes to 2");
  check(entry.priority == 1U, "sprite priority decodes to 1");
}

void test_sprite_entry_decode_non_1x1_size() {
  // width_field=3 (4 cells), height_field=1 (2 cells) -> size_byte bits
  // 3-2=11, bits 1-0=01 -> 0b1101 = 0x0D.
  uint8_t raw[GENESIS_VDP_SPRITE_ENTRY_BYTES];
  std::memset(raw, 0, sizeof(raw));
  raw[2] = 0x0D;
  GenesisVdpSpriteEntry entry = genesis_vdp_decode_sprite_entry(raw);
  check(entry.width_cells == 4U, "sprite width field 3 -> 4 cells");
  check(entry.height_cells == 2U, "sprite height field 1 -> 2 cells");

  // width_field=1 (2 cells), height_field=1 (2 cells): 2x2.
  raw[2] = 0x05; // bits 3-2=01, bits1-0=01
  entry = genesis_vdp_decode_sprite_entry(raw);
  check(entry.width_cells == 2U, "sprite width field 1 -> 2 cells (2x2 case)");
  check(entry.height_cells == 2U, "sprite height field 1 -> 2 cells (2x2 case)");
}

void test_sprite_entry_y_field_is_9_bits() {
  // Raw Y word with bit 9 SET while the low 9 bits encode a known value
  // (128 decimal = 0b010000000). Bits 9-0 = 0b1010000000 = 0x280. Bit 9
  // must be ignored (not part of the coordinate) for this bounded
  // non-interlaced surface: the decoded Y must equal the same value as
  // when bit 9 is 0.
  uint8_t raw_with_bit9[GENESIS_VDP_SPRITE_ENTRY_BYTES];
  std::memset(raw_with_bit9, 0, sizeof(raw_with_bit9));
  uint16_t raw_y_bit9_set = 0x0280; // bit 9 set, low 9 bits = 128.
  raw_with_bit9[0] = (uint8_t)(raw_y_bit9_set >> 8);
  raw_with_bit9[1] = (uint8_t)(raw_y_bit9_set & 0xFF);
  GenesisVdpSpriteEntry entry_bit9_set = genesis_vdp_decode_sprite_entry(raw_with_bit9);

  uint8_t raw_without_bit9[GENESIS_VDP_SPRITE_ENTRY_BYTES];
  std::memset(raw_without_bit9, 0, sizeof(raw_without_bit9));
  uint16_t raw_y_bit9_clear = 0x0080; // bit 9 clear, low 9 bits = 128.
  raw_without_bit9[0] = (uint8_t)(raw_y_bit9_clear >> 8);
  raw_without_bit9[1] = (uint8_t)(raw_y_bit9_clear & 0xFF);
  GenesisVdpSpriteEntry entry_bit9_clear = genesis_vdp_decode_sprite_entry(raw_without_bit9);

  check(entry_bit9_set.y == entry_bit9_clear.y,
        "sprite Y field is 9 bits: bit 9 is ignored, not treated as part of the coordinate");
  check(entry_bit9_set.y == (128 - GENESIS_VDP_SPRITE_COORD_BIAS),
        "sprite Y with bit 9 set still decodes to raw-128 using only the low 9 bits");
}

void test_sat_traversal_short_chain() {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint32_t base = 0x5000;

  // Chain: 0 -> 2 -> 1 -> (link 0, end).
  write_sprite_entry(vram, base + 0 * 8, 128, 0, /*link=*/2, 0, 128);
  write_sprite_entry(vram, base + 1 * 8, 128, 0, /*link=*/0, 0, 128);
  write_sprite_entry(vram, base + 2 * 8, 128, 0, /*link=*/1, 0, 128);

  unsigned visited[GENESIS_VDP_SAT_H40_MAX_SPRITES];
  unsigned count = 0;
  check(genesis_vdp_sat_traverse(vram, regs, visited, &count) == 0, "traversal succeeds");
  check(count == 3U, "traversal visits exactly 3 sprites");
  check(visited[0] == 0U && visited[1] == 2U && visited[2] == 1U,
        "traversal order is 0, 2, 1");
}

void test_sat_traversal_capped_at_80() {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint32_t base = 0x5000;

  // Every sprite i links to (i+1) % 80, so link never hits 0 except from
  // sprite 79 wrapping back to 0 -- which is itself a revisit of 0.
  for (unsigned i = 0; i < GENESIS_VDP_SAT_H40_MAX_SPRITES; ++i) {
    unsigned next = (i + 1U) % GENESIS_VDP_SAT_H40_MAX_SPRITES;
    write_sprite_entry(vram, base + i * 8, 128, 0, (uint8_t)next, 0, 128);
  }

  unsigned visited[GENESIS_VDP_SAT_H40_MAX_SPRITES];
  unsigned count = 0;
  check(genesis_vdp_sat_traverse(vram, regs, visited, &count) == 0,
        "traversal of full 80-sprite ring succeeds");
  check(count == GENESIS_VDP_SAT_H40_MAX_SPRITES,
        "traversal caps at exactly 80 visited sprites");
}

void test_sat_traversal_cycle_detected() {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  make_default_registers(regs);
  uint32_t base = 0x5000;

  // Genuine non-zero-link cycle: 0 -> 1 -> 2 -> 1. Link value 0 is the
  // documented end-of-list terminator, so a chain that eventually links to
  // 0 (e.g. 0->1->2->0) terminates normally via the `link == 0` branch and
  // does NOT exercise revisit-based cycle detection at all. Here sprite 2
  // links back to sprite 1, a non-zero link value that was already visited,
  // which specifically exercises the `visited_flags[current]`-style
  // revisit-detection branch rather than the `link == 0` terminator branch.
  write_sprite_entry(vram, base + 0 * 8, 128, 0, /*link=*/1, 0, 128);
  write_sprite_entry(vram, base + 1 * 8, 128, 0, /*link=*/2, 0, 128);
  write_sprite_entry(vram, base + 2 * 8, 128, 0, /*link=*/1, 0, 128); // revisits 1, not 0.

  unsigned visited[GENESIS_VDP_SAT_H40_MAX_SPRITES];
  unsigned count = 0;
  check(genesis_vdp_sat_traverse(vram, regs, visited, &count) == 0,
        "cyclic traversal returns success (bounded, not an error)");
  check(count == 3U, "cyclic traversal (0->1->2->1) visits exactly 3 before stopping");
  check(visited[0] == 0U && visited[1] == 1U && visited[2] == 2U,
        "cyclic traversal order is 0, 1, 2 before the revisit of 1 stops it (not the link==0 terminator)");
}

void test_multicell_tile_selection_unflipped() {
  // A 2x2 sprite at (0,0), tile_index=10, no flip. Column-major layout:
  // cell(0,0)=10, cell(0,1)=11, cell(1,0)=12, cell(1,1)=13.
  GenesisVdpSpriteEntry sprite;
  std::memset(&sprite, 0, sizeof(sprite));
  sprite.x = 0;
  sprite.y = 0;
  sprite.width_cells = 2;
  sprite.height_cells = 2;
  sprite.tile_index = 10;
  sprite.h_flip = 0;
  sprite.v_flip = 0;

  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  // Fill tile 10 with palette index 1 at (0,0), tile 13 with index 2 at
  // (0,0) local-to-tile.
  // Tile 10 occupies bytes [10*32, 10*32+32). Row0 byte0 high nibble =
  // pixel (0,0).
  vram[10 * 32 + 0] = 0x10; // pixel(row0,col0)=1, pixel(row0,col1)=0
  vram[13 * 32 + 0] = 0x20; // pixel(row0,col0)=2

  uint8_t index_value = 0xFF;
  // Screen pixel (0,0) -> local (0,0) -> cell(0,0) -> tile 10.
  check(genesis_vdp_resolve_sprite_pixel(&sprite, vram, 0, 0, &index_value) == 0 &&
            index_value == 1,
        "unflipped 2x2 sprite top-left pixel resolves to tile 10");

  // Screen pixel (8,8) -> local(8,8) -> cell(1,1) -> tile_offset =
  // 1*2+1=3 -> tile 13.
  index_value = 0xFF;
  check(genesis_vdp_resolve_sprite_pixel(&sprite, vram, 8, 8, &index_value) == 0 &&
            index_value == 2,
        "unflipped 2x2 sprite bottom-right cell pixel resolves to tile 13");
}

void test_multicell_tile_selection_hflip() {
  // Same 2x2 sprite, but h_flip=1. The visually-left column (screen x
  // 0-7) should now sample the *last* column's tiles (cell_col=1 after
  // mirroring), i.e. tile 12 (cell(1,0)) instead of tile 10.
  GenesisVdpSpriteEntry sprite;
  std::memset(&sprite, 0, sizeof(sprite));
  sprite.x = 0;
  sprite.y = 0;
  sprite.width_cells = 2;
  sprite.height_cells = 2;
  sprite.tile_index = 10;
  sprite.h_flip = 1;
  sprite.v_flip = 0;

  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));

  uint8_t index_value = 0xFF;
  // Screen pixel (0,0): local_x=0 -> mirrored_x = width_px-1-0 = 15 ->
  // cell_col = 15/8 = 1, tile_col = 15%8=7. tile_offset = 1*2+0=2 ->
  // tile 12. Sample tile 12 at (row=0,col=7): need that pixel set too.
  vram[12 * 32 + 0 * 4 + (7 / 2)] |= 0x03; // low nibble of byte 3 = col7
  check(genesis_vdp_resolve_sprite_pixel(&sprite, vram, 0, 0, &index_value) == 0 &&
            index_value == 3,
        "h-flipped 2x2 sprite left-edge pixel resolves to mirrored tile 12");
}

void test_multicell_tile_selection_vflip() {
  // 2x1 sprite (2 wide, 1 tall), v_flip=1. Since height_cells=1, vertical
  // mirroring only affects the intra-tile row (mirrored within the single
  // row of cells), verifying v_flip's tile-row mirroring path.
  GenesisVdpSpriteEntry sprite;
  std::memset(&sprite, 0, sizeof(sprite));
  sprite.x = 0;
  sprite.y = 0;
  sprite.width_cells = 2;
  sprite.height_cells = 1;
  sprite.tile_index = 20;
  sprite.h_flip = 0;
  sprite.v_flip = 1;

  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  // Screen (0,0): local(0,0); height_px=8 -> mirrored_y = 8-1-0=7 ->
  // tile_row=7, cell_row=0. tile_offset = cell_col(0)*1+0=0 -> tile 20.
  // Row 7 is byte_index = 7*4 + 0 = 28.
  vram[20 * 32 + 28] = 0x40; // pixel(row7,col0)=4

  uint8_t index_value = 0xFF;
  check(genesis_vdp_resolve_sprite_pixel(&sprite, vram, 0, 0, &index_value) == 0 &&
            index_value == 4,
        "v-flipped 2x1 sprite top pixel resolves to mirrored tile row 7");
}

void test_multicell_tile_selection_wraps_11bit_tile_index() {
  // 2x1 sprite (2 wide, 1 tall) with tile_index near the top of the 11-bit
  // range (0x7FF = 2047). The second cell's raw sum (2047 + 1 = 2048) would
  // overflow the documented 11-bit tile-number field; per the file's
  // documented Genesis Plus GX-corroborated behavior, the effective tile
  // number wraps modulo 2048 (& GENESIS_VDP_TILE_INDEX_MASK) rather than
  // being rejected, landing on tile 0.
  GenesisVdpSpriteEntry sprite;
  std::memset(&sprite, 0, sizeof(sprite));
  sprite.x = 0;
  sprite.y = 0;
  sprite.width_cells = 2;
  sprite.height_cells = 1;
  sprite.tile_index = 0x7FF;
  sprite.h_flip = 0;
  sprite.v_flip = 0;

  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  // Tile 0x7FF (2047) occupies the last 32 bytes below GENESIS_VDP_VRAM_BYTES
  // only if VRAM is large enough; write a marker at tile 0's (0,0) pixel,
  // since cell(1,0)'s effective tile wraps to 0.
  vram[0 * 32 + 0] = 0x60; // pixel(row0,col0)=6 for tile 0.

  uint8_t index_value = 0xFF;
  // Screen pixel (8,0) -> local(8,0) -> cell_col=1, cell_row=0 ->
  // tile_offset = 1*1+0 = 1 -> raw sum = 0x7FF + 1 = 0x800 (2048), which
  // wraps (& 0x7FF) to effective_tile 0.
  check(genesis_vdp_resolve_sprite_pixel(&sprite, vram, 8, 0, &index_value) == 0 &&
            index_value == 6,
        "multi-cell sprite tile number wraps modulo 2048 (11-bit field) instead of "
        "rejecting overflow");
}

void test_screen_bounds_clipping() {
  GenesisVdpSpriteEntry sprite;
  std::memset(&sprite, 0, sizeof(sprite));
  sprite.x = 10;
  sprite.y = 10;
  sprite.width_cells = 1;
  sprite.height_cells = 1;
  sprite.tile_index = 0;

  check(genesis_vdp_sprite_covers_pixel(&sprite, 10, 10) != 0, "covers own top-left pixel");
  check(genesis_vdp_sprite_covers_pixel(&sprite, 17, 17) != 0, "covers own bottom-right pixel");
  check(genesis_vdp_sprite_covers_pixel(&sprite, 18, 10) == 0, "does not cover pixel past right edge");
  check(genesis_vdp_sprite_covers_pixel(&sprite, 9, 10) == 0, "does not cover pixel before left edge");
  check(genesis_vdp_sprite_covers_pixel(&sprite, 5, 5) == 0, "does not cover unrelated far pixel");

  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  std::memset(vram, 0, sizeof(vram));
  uint8_t index_value = 0xFF;
  check(genesis_vdp_resolve_sprite_pixel(&sprite, vram, 100, 100, &index_value) < 0,
        "resolve rejects an uncovered query without reading VRAM garbage");

  // Sprite placed partially off-screen (negative X/Y after offset
  // correction): a covered query in the visible portion must still resolve
  // cleanly without OOB VRAM reads.
  GenesisVdpSpriteEntry off_screen;
  std::memset(&off_screen, 0, sizeof(off_screen));
  off_screen.x = -4; // half off the left edge
  off_screen.y = -4; // half off the top edge
  off_screen.width_cells = 1;
  off_screen.height_cells = 1;
  off_screen.tile_index = 0;
  vram[0] = 0x50; // tile 0, pixel(row0,col0)=5

  index_value = 0xFF;
  // Screen (0,0) is covered (x in [-4,4), y in [-4,4)); local=(4,4).
  check(genesis_vdp_sprite_covers_pixel(&off_screen, 0, 0) != 0,
        "off-screen-placed sprite still covers its visible portion");
  check(genesis_vdp_resolve_sprite_pixel(&off_screen, vram, 0, 0, &index_value) == 0,
        "off-screen-placed sprite resolves its visible portion without OOB read");

  // A query fully outside the sprite's on-screen extent (e.g. before the
  // sprite's negative-origin rectangle) must not be covered.
  check(genesis_vdp_sprite_covers_pixel(&off_screen, 300, 300) == 0,
        "query far outside off-screen-placed sprite is not covered");
}

} // namespace

int main() {
  test_sat_base_address();
  test_sat_base_address_h40_bit0_ignored();
  test_sat_base_address_fails_closed_without_h40();
  test_sprite_entry_decode_fields();
  test_sprite_entry_decode_non_1x1_size();
  test_sprite_entry_y_field_is_9_bits();
  test_sat_traversal_short_chain();
  test_sat_traversal_capped_at_80();
  test_sat_traversal_cycle_detected();
  test_multicell_tile_selection_unflipped();
  test_multicell_tile_selection_hflip();
  test_multicell_tile_selection_vflip();
  test_multicell_tile_selection_wraps_11bit_tile_index();
  test_screen_bounds_clipping();

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
