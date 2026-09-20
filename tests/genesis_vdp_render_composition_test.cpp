// SEG-007-T049 checkpoint C4: focused unit coverage for
// platforms/genesis/runtime/vdp_render.c's priority/transparency/composition
// (genesis_vdp_compose_pixel and its helpers). All fixtures are
// project-authored synthetic VRAM/VSRAM/CRAM/register byte patterns with
// independently hand-derived expected results -- no commercial ROM content
// is read or referenced. Which candidate (plane A, plane B, or a sprite)
// wins at each queried coordinate is decided by hand from this file's own
// documented six-layer priority order, not by calling the composition
// function under test; C1's already-independently-validated
// `genesis_vdp_decode_cram_word` is reused only as the trusted color-decode
// oracle for a hand-picked CRAM entry index.

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

bool colors_equal(GenesisRgb888 a, GenesisRgb888 b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

// Mode 5, H40, non-interlaced; Plane A base 0xC000 (reg2=0x30), Plane B base
// 0x8000 (reg4=0x04); 32x32 plane size (reg16=0x00, so cell(0,0) maps to
// screen pixel (0,0) with zero scroll); full-screen H-scroll (table base 0,
// all-zero so hscroll=0) and 2-cell V-scroll (all-zero VSRAM so vscroll=0);
// SAT base 0x5000 (reg5=0x28, H40 granularity).
void make_default_registers(uint16_t regs[GENESIS_VDP_REGISTER_COUNT]) {
  std::memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[1] = 0x04;  // Mode 5 select.
  regs[2] = 0x30;  // Plane A base -> 0xC000.
  regs[4] = 0x04;  // Plane B base -> 0x8000.
  regs[5] = 0x28;  // SAT base -> 0x5000.
  regs[11] = 0x04; // full-screen H, 2-cell V.
  regs[12] = 0x81; // H40, non-interlaced.
  regs[13] = 0x00; // H-scroll table base = 0.
  regs[16] = 0x00; // 32x32 plane size.
}

// Writes a big-endian 16-bit word at `addr` into `buf`.
void write_word(uint8_t *buf, uint32_t addr, uint16_t value) {
  buf[addr] = (uint8_t)(value >> 8);
  buf[addr + 1] = (uint8_t)(value & 0xFF);
}

// Writes a Mode-5 nametable entry word (priority/palette/flip/tile_index)
// at plane base + cell(0,0), i.e. at `base_addr` itself.
void write_nametable_entry(uint8_t *vram, uint32_t base_addr, uint8_t priority, uint8_t palette,
                            uint16_t tile_index) {
  uint16_t word = (uint16_t)(((unsigned)priority << 15) | ((unsigned)palette << 13) |
                              ((unsigned)tile_index & GENESIS_VDP_TILE_INDEX_MASK));
  write_word(vram, base_addr, word);
}

// Writes a single opaque or transparent 8x8 tile's pixel(0,0) (top-left) to
// `pattern_index` (0-15); all other pixels of the tile are left at whatever
// `vram` was already initialized to (callers memset vram to 0 first, so all
// other pixels of a freshly-touched tile decode to palette index 0).
void write_tile_pixel00(uint8_t *vram, unsigned tile_number, uint8_t pattern_index) {
  uint32_t tile_addr = tile_number * GENESIS_VDP_TILE_BYTES;
  // Row 0, byte 0, high nibble = pixel (0,0).
  vram[tile_addr] = (uint8_t)((vram[tile_addr] & 0x0FU) | (uint8_t)(pattern_index << 4));
}

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

uint16_t sprite_tile_word(uint8_t priority, uint8_t palette, uint16_t tile_index) {
  return (uint16_t)(((unsigned)priority << 15) | ((unsigned)palette << 13) |
                     ((unsigned)tile_index & GENESIS_VDP_TILE_INDEX_MASK));
}

// Writes one 3-bit-per-channel CRAM color at `entry_index` (0-63).
void write_cram_color(uint8_t *cram, unsigned entry_index, uint8_t r3, uint8_t g3, uint8_t b3) {
  uint16_t word = (uint16_t)(((uint16_t)(b3 & 0x07U) << 9) | ((uint16_t)(g3 & 0x07U) << 5) |
                              ((uint16_t)(r3 & 0x07U) << 1));
  write_word(cram, entry_index * 2U, word);
}

// A "no sprites visible at screen (0,0)/(8,0)/etc." fixture: sprite 0 is a
// valid, well-formed 1x1 sprite placed far off-screen (so SAT traversal
// succeeds -- it always needs to read sprite 0 -- but it never covers any
// queried coordinate in these tests).
void write_no_visible_sprite(uint8_t *vram, uint32_t sat_base) {
  write_sprite_entry(vram, sat_base, (uint16_t)(1000 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/0, sprite_tile_word(0, 0, 0),
                      (uint16_t)(1000 + GENESIS_VDP_SPRITE_COORD_BIAS));
}

struct Fixture {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];

  Fixture() {
    std::memset(vram, 0, sizeof(vram));
    std::memset(vsram, 0, sizeof(vsram));
    std::memset(cram, 0, sizeof(cram));
    make_default_registers(regs);
    write_no_visible_sprite(vram, 0x5000);
  }
};

void test_transparent_plane_a_over_opaque_plane_b() {
  Fixture f;
  // Plane A cell(0,0): tile 1, palette 0, priority 0, but pattern pixel(0,0)
  // left at 0 (transparent) -- tile 1 is never touched by write_tile_pixel00.
  write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/0, /*tile_index=*/1);
  // Plane B cell(0,0): tile 2, palette 1, priority 0, opaque pattern index 5.
  write_nametable_entry(f.vram, 0x8000, /*priority=*/0, /*palette=*/1, /*tile_index=*/2);
  write_tile_pixel00(f.vram, 2, 5);
  // Expected CRAM entry: palette 1, index 5 -> 1*16+5 = 21.
  write_cram_color(f.cram, 21, 3, 5, 7);

  GenesisRgb888 expected = genesis_vdp_decode_cram_word(
      (uint16_t)((7U << 9) | (5U << 5) | (3U << 1)));
  GenesisRgb888 actual;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
        "compose succeeds (transparent A over opaque B)");
  check(colors_equal(actual, expected),
        "transparent Plane A pixel does not occlude opaque Plane B: Plane B wins");
}

void test_priority_bit_matrix_planes_only() {
  // Case 1: Plane A low priority, Plane B high priority, both opaque ->
  // Plane B (high) wins over Plane A (low), since B-high precedes A-low in
  // the six-layer order.
  {
    Fixture f;
    write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/0, /*tile_index=*/1);
    write_tile_pixel00(f.vram, 1, 3); // Plane A opaque, low priority.
    write_nametable_entry(f.vram, 0x8000, /*priority=*/1, /*palette=*/2, /*tile_index=*/2);
    write_tile_pixel00(f.vram, 2, 7); // Plane B opaque, high priority.
    // Expected: Plane B, palette 2, index 7 -> cram 2*16+7=39.
    write_cram_color(f.cram, 39, 1, 2, 3);
    GenesisRgb888 expected =
        genesis_vdp_decode_cram_word((uint16_t)((3U << 9) | (2U << 5) | (1U << 1)));
    GenesisRgb888 actual;
    check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
          "compose succeeds (A-low vs B-high)");
    check(colors_equal(actual, expected),
          "Plane B high-priority beats Plane A low-priority (B-high precedes A-low)");
  }
  // Case 2: both planes high priority, both opaque -> Plane A wins (A-high
  // precedes B-high).
  {
    Fixture f;
    write_nametable_entry(f.vram, 0xC000, /*priority=*/1, /*palette=*/1, /*tile_index=*/1);
    write_tile_pixel00(f.vram, 1, 4); // Plane A opaque, high priority.
    write_nametable_entry(f.vram, 0x8000, /*priority=*/1, /*palette=*/2, /*tile_index=*/2);
    write_tile_pixel00(f.vram, 2, 9 & 0x0F); // Plane B opaque, high priority.
    // Expected: Plane A, palette 1, index 4 -> cram 1*16+4=20.
    write_cram_color(f.cram, 20, 5, 1, 6);
    GenesisRgb888 expected =
        genesis_vdp_decode_cram_word((uint16_t)((6U << 9) | (1U << 5) | (5U << 1)));
    GenesisRgb888 actual;
    check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
          "compose succeeds (A-high vs B-high)");
    check(colors_equal(actual, expected),
          "Plane A high-priority beats Plane B high-priority (A-high precedes B-high)");
  }
}

void test_transparent_sprite_does_not_occlude_plane() {
  Fixture f;
  // Plane A opaque, low priority, at cell(0,0).
  write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/3, /*tile_index=*/1);
  write_tile_pixel00(f.vram, 1, 2);
  // Sprite 0: covers screen (0,0), high priority, but transparent pattern
  // pixel (index 0, never written -> stays 0).
  write_sprite_entry(f.vram, 0x5000, (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/0, sprite_tile_word(/*priority=*/1, /*palette=*/0, /*tile_index=*/9),
                      (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS));
  // Expected: Plane A, palette 3, index 2 -> cram 3*16+2=50.
  write_cram_color(f.cram, 50, 2, 4, 1);
  GenesisRgb888 expected = genesis_vdp_decode_cram_word((uint16_t)((1U << 9) | (4U << 5) | (2U << 1)));
  GenesisRgb888 actual;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
        "compose succeeds (transparent high-priority sprite over opaque plane)");
  check(colors_equal(actual, expected),
        "transparent sprite pixel (index 0) does not occlude an opaque plane pixel underneath");
}

void test_low_priority_sprite_vs_both_planes() {
  Fixture f;
  // Plane A transparent (tile 1, never written). Plane B opaque, low
  // priority. Sprite opaque, low priority, covering (0,0).
  write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/0, /*tile_index=*/1);
  write_nametable_entry(f.vram, 0x8000, /*priority=*/0, /*palette=*/1, /*tile_index=*/2);
  write_tile_pixel00(f.vram, 2, 6); // Plane B opaque low.
  write_sprite_entry(f.vram, 0x5000, (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/0, sprite_tile_word(/*priority=*/0, /*palette=*/2, /*tile_index=*/9),
                      (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS));
  write_tile_pixel00(f.vram, 9, 8); // Sprite opaque low.
  // Expected: sprite (low) wins over Plane B (low), since sprite-low
  // precedes both plane-low layers -- palette 2, index 8 -> cram 2*16+8=40.
  write_cram_color(f.cram, 40, 4, 4, 4);
  GenesisRgb888 expected = genesis_vdp_decode_cram_word((uint16_t)((4U << 9) | (4U << 5) | (4U << 1)));
  GenesisRgb888 actual;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
        "compose succeeds (low-priority sprite vs opaque low-priority Plane B)");
  check(colors_equal(actual, expected),
        "low-priority sprite beats opaque low-priority Plane B (sprite-low precedes plane-low layers)");
}

void test_high_priority_sprite_vs_both_planes() {
  Fixture f;
  // Both planes opaque, high priority. Sprite opaque, high priority,
  // covering (0,0) -> sprite-high wins over both plane-high layers.
  write_nametable_entry(f.vram, 0xC000, /*priority=*/1, /*palette=*/0, /*tile_index=*/1);
  write_tile_pixel00(f.vram, 1, 3);
  write_nametable_entry(f.vram, 0x8000, /*priority=*/1, /*palette=*/1, /*tile_index=*/2);
  write_tile_pixel00(f.vram, 2, 4);
  write_sprite_entry(f.vram, 0x5000, (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/0, sprite_tile_word(/*priority=*/1, /*palette=*/3, /*tile_index=*/9),
                      (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS));
  write_tile_pixel00(f.vram, 9, 11);
  // Expected: sprite, palette 3, index 11 -> cram 3*16+11=59.
  write_cram_color(f.cram, 59, 6, 2, 0);
  GenesisRgb888 expected = genesis_vdp_decode_cram_word((uint16_t)((0U << 9) | (2U << 5) | (6U << 1)));
  GenesisRgb888 actual;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
        "compose succeeds (high-priority sprite vs both high-priority planes)");
  check(colors_equal(actual, expected),
        "high-priority sprite beats both high-priority planes (sprite-high is the topmost layer)");
}

void test_overlapping_sprites_earlier_traversal_order_wins() {
  Fixture f;
  // Sprite 0 (traversed first) and sprite 1 (linked from 0) both cover
  // (0,0), both opaque, same priority. Sprite 0 must win.
  write_sprite_entry(f.vram, 0x5000, (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/1, sprite_tile_word(/*priority=*/0, /*palette=*/0, /*tile_index=*/9),
                      (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS));
  write_tile_pixel00(f.vram, 9, 3); // Sprite 0 opaque.
  write_sprite_entry(f.vram, 0x5008, (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/0, sprite_tile_word(/*priority=*/0, /*palette=*/1, /*tile_index=*/10),
                      (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS));
  write_tile_pixel00(f.vram, 10, 5); // Sprite 1 opaque, would-be-different color.
  // Expected: sprite 0 (earlier traversal order), palette 0, index 3 ->
  // cram 0*16+3=3.
  write_cram_color(f.cram, 3, 7, 0, 1);
  GenesisRgb888 expected = genesis_vdp_decode_cram_word((uint16_t)((1U << 9) | (0U << 5) | (7U << 1)));
  GenesisRgb888 actual;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
        "compose succeeds (two overlapping opaque sprites)");
  check(colors_equal(actual, expected),
        "earlier SAT-traversal-order sprite (index 0) wins over a later overlapping sprite (index 1)");
}

void test_backdrop_when_everything_transparent() {
  Fixture f;
  // Plane A/B nametable entries point at never-touched tiles (all pixels
  // transparent, index 0). Sprite 0 is the default off-screen fixture.
  write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/0, /*tile_index=*/1);
  write_nametable_entry(f.vram, 0x8000, /*priority=*/0, /*palette=*/0, /*tile_index=*/2);
  // Register #7: palette 2 (bits 5-4), index 9 (bits 3-0) -> reg7 = (2<<4)|9
  // = 0x29 -> backdrop CRAM index = 2*16+9 = 41.
  f.regs[7] = 0x29;
  write_cram_color(f.cram, 41, 1, 1, 1);
  GenesisRgb888 expected = genesis_vdp_decode_cram_word((uint16_t)((1U << 9) | (1U << 5) | (1U << 1)));
  GenesisRgb888 actual;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
        "compose succeeds (all layers transparent)");
  check(colors_equal(actual, expected),
        "backdrop color (register #7 palette 2, index 9) used when no plane or sprite is opaque");

  unsigned backdrop_index = 0xFFFFFFFFU;
  check(genesis_vdp_backdrop_cram_index(f.regs, &backdrop_index) == 0 && backdrop_index == 41U,
        "genesis_vdp_backdrop_cram_index decodes register #7 to CRAM index 41");
}

void test_palette_selection_preserved_independently() {
  // Plane A at screen (0,0) uses palette 2; Plane B at screen (8,0) uses
  // palette 1. Confirm each independently decodes to its own distinct CRAM
  // color (i.e. palette selection is not lost/aliased between planes).
  Fixture f;
  write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/2, /*tile_index=*/1);
  write_tile_pixel00(f.vram, 1, 4); // Plane A opaque, palette 2, index 4.
  // Plane A is transparent at cell(1,0) (screen x=8..15): tile 3, never
  // touched.
  write_word(f.vram, 0xC000 + 1 * 2, (uint16_t)(0U << 13 | (3U & GENESIS_VDP_TILE_INDEX_MASK)));

  // Plane B at cell(0,0) transparent (tile 4, never touched); at cell(1,0)
  // (screen x=8..15) opaque, palette 1, index 7.
  write_word(f.vram, 0x8000, (uint16_t)(0U << 13 | (4U & GENESIS_VDP_TILE_INDEX_MASK)));
  write_nametable_entry(f.vram, 0x8000 + 1 * 2, /*priority=*/0, /*palette=*/1, /*tile_index=*/5);
  write_tile_pixel00(f.vram, 5, 7);

  // CRAM: palette2/index4 -> 2*16+4=36; palette1/index7 -> 1*16+7=23.
  write_cram_color(f.cram, 36, 1, 3, 5);
  write_cram_color(f.cram, 23, 6, 2, 4);
  GenesisRgb888 expected_a = genesis_vdp_decode_cram_word((uint16_t)((5U << 9) | (3U << 5) | (1U << 1)));
  GenesisRgb888 expected_b = genesis_vdp_decode_cram_word((uint16_t)((4U << 9) | (2U << 5) | (6U << 1)));

  GenesisRgb888 actual_a;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual_a) == 0,
        "compose succeeds at (0,0) (Plane A palette 2)");
  check(colors_equal(actual_a, expected_a),
        "Plane-A-palette-2 pixel decodes to its own correct, distinct CRAM color");

  GenesisRgb888 actual_b;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 8, 0, &actual_b) == 0,
        "compose succeeds at (8,0) (Plane B palette 1)");
  check(colors_equal(actual_b, expected_b),
        "Plane-B-palette-1 pixel decodes to its own correct, distinct CRAM color");

  check(!colors_equal(actual_a, actual_b),
        "Plane A palette-2 and Plane B palette-1 pixels decode to distinct colors");
}

void test_cram_index_helper() {
  check(genesis_vdp_cram_index(0, 0) == 0U, "cram_index(0,0)==0");
  check(genesis_vdp_cram_index(3, 15) == 63U, "cram_index(3,15)==63 (max entry)");
  check(genesis_vdp_cram_index(1, 5) == 21U, "cram_index(1,5)==21");
}

void test_compose_rejects_null_arguments() {
  Fixture f;
  GenesisRgb888 color;
  check(genesis_vdp_compose_pixel(nullptr, f.vsram, f.cram, f.regs, 0, 0, &color) < 0,
        "compose rejects NULL vram");
  check(genesis_vdp_compose_pixel(f.vram, nullptr, f.cram, f.regs, 0, 0, &color) < 0,
        "compose rejects NULL vsram");
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, nullptr, f.regs, 0, 0, &color) < 0,
        "compose rejects NULL cram");
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, nullptr, 0, 0, &color) < 0,
        "compose rejects NULL registers");
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, nullptr) < 0,
        "compose rejects NULL color_out");
}

// Part A1: proves sprite-vs-sprite overlap is resolved purely by SAT
// traversal order, independent of either sprite's own priority bit -- and
// that only AFTER that single surviving sprite candidate is chosen is its
// priority bit compared against the planes. Sprite 0 (earlier in traversal)
// is LOW priority; sprite 1 (later, linked from 0) is HIGH priority; both
// are opaque and both cover screen (0,0). Plane A at (0,0) is opaque with
// its priority bit set (HIGH). Expected: sprite 0 wins the sprite-vs-sprite
// overlap purely due to traversal order (sprite 1 is never even selected as
// the sprite candidate), then Plane A (high priority) beats the surviving
// low-priority sprite candidate per the six-layer order, so Plane A's color
// is the final composite.
void test_sprite_traversal_order_ignores_priority_then_plane_a_wins() {
  Fixture f;
  // Plane A cell(0,0): opaque, HIGH priority, palette 0, index 6.
  write_nametable_entry(f.vram, 0xC000, /*priority=*/1, /*palette=*/0, /*tile_index=*/1);
  write_tile_pixel00(f.vram, 1, 6);
  // Plane B: transparent (never touched tile 2).
  write_nametable_entry(f.vram, 0x8000, /*priority=*/0, /*palette=*/0, /*tile_index=*/2);

  // Sprite 0 (traversed first): opaque, LOW priority, palette 1, index 3.
  write_sprite_entry(f.vram, 0x5000, (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/1, sprite_tile_word(/*priority=*/0, /*palette=*/1, /*tile_index=*/9),
                      (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS));
  write_tile_pixel00(f.vram, 9, 3);
  // Sprite 1 (linked from sprite 0, traversed second): opaque, HIGH
  // priority, palette 2, index 5 -- would win if priority reordered
  // sprite-vs-sprite selection, but it must not.
  write_sprite_entry(f.vram, 0x5008, (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                      /*link=*/0, sprite_tile_word(/*priority=*/1, /*palette=*/2, /*tile_index=*/10),
                      (uint16_t)(0 + GENESIS_VDP_SPRITE_COORD_BIAS));
  write_tile_pixel00(f.vram, 10, 5);

  // Expected final winner: Plane A (high priority beats the surviving
  // low-priority sprite candidate) -- palette 0, index 6 -> cram 0*16+6=6.
  write_cram_color(f.cram, 6, 2, 6, 4);
  // Also seed the (wrong, if the bug existed) sprite-1 CRAM entry
  // (palette 2, index 5 -> 2*16+5=37) with a visibly different color, so a
  // regression that let sprite 1 win would produce a distinct, detectable
  // wrong color rather than accidentally matching by coincidence.
  write_cram_color(f.cram, 37, 7, 1, 3);

  GenesisRgb888 expected = genesis_vdp_decode_cram_word((uint16_t)((4U << 9) | (6U << 5) | (2U << 1)));
  GenesisRgb888 wrong_sprite1_color =
      genesis_vdp_decode_cram_word((uint16_t)((3U << 9) | (1U << 5) | (7U << 1)));
  GenesisRgb888 actual;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual) == 0,
        "compose succeeds (low-priority earlier sprite vs high-priority later sprite vs high-priority "
        "Plane A)");
  check(!colors_equal(actual, wrong_sprite1_color),
        "sprite 1 (later traversal order, high priority) is never selected as the sprite candidate");
  check(colors_equal(actual, expected),
        "Plane A (high priority) beats the surviving low-priority sprite candidate (sprite 0, chosen "
        "purely by earlier traversal order)");
}

// Part A2: proves genesis_vdp_compose_pixel correctly applies a plane
// nametable entry's h_flip/v_flip bits when sampling the tile, by
// constructing a tile with distinguishable ascending-nibble pixel values and
// confirming an h-flipped / v-flipped tile samples the mirrored column/row.
void test_plane_h_flip_samples_mirrored_column() {
  Fixture f;
  // Plane A cell(0,0) points at tile 1. Fill tile 1's row 0 with ascending
  // nibbles 1..8 across columns 0..7 (col0=1, col1=2, ..., col7=8).
  uint32_t tile_addr = 1U * GENESIS_VDP_TILE_BYTES;
  f.vram[tile_addr + 0] = 0x12; // col0=1, col1=2
  f.vram[tile_addr + 1] = 0x34; // col2=3, col3=4
  f.vram[tile_addr + 2] = 0x56; // col4=5, col5=6
  f.vram[tile_addr + 3] = 0x78; // col6=7, col7=8
  // Unflipped nametable entry: screen (0,0) -> tile_x=0 -> pixel value 1.
  write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/0, /*tile_index=*/1);
  // Plane B transparent so it never interferes.
  write_nametable_entry(f.vram, 0x8000, /*priority=*/0, /*palette=*/0, /*tile_index=*/2);

  write_cram_color(f.cram, 1, 1, 0, 0);  // palette0,index1 -> cram 1.
  write_cram_color(f.cram, 8, 4, 4, 4);  // palette0,index8 -> cram 8.
  GenesisRgb888 expected_unflipped =
      genesis_vdp_decode_cram_word((uint16_t)((0U << 9) | (0U << 5) | (1U << 1)));
  GenesisRgb888 expected_flipped =
      genesis_vdp_decode_cram_word((uint16_t)((4U << 9) | (4U << 5) | (4U << 1)));

  GenesisRgb888 actual_unflipped;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual_unflipped) == 0,
        "compose succeeds (unflipped Plane A tile)");
  check(colors_equal(actual_unflipped, expected_unflipped),
        "unflipped Plane A tile samples column 0's stored pixel value (1)");

  // Now set h_flip on the same nametable entry; column 0 on screen must now
  // sample the mirrored column 7 (stored value 8) instead.
  write_word(f.vram, 0xC000,
             (uint16_t)((0x01U << 11) | (0U << 13) | (1U & GENESIS_VDP_TILE_INDEX_MASK)));
  GenesisRgb888 actual_flipped;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual_flipped) == 0,
        "compose succeeds (h-flipped Plane A tile)");
  check(colors_equal(actual_flipped, expected_flipped),
        "h-flipped Plane A tile samples the horizontally-mirrored column (stored col 7, value 8) at "
        "screen column 0");
}

void test_plane_v_flip_samples_mirrored_row() {
  Fixture f;
  // Plane B cell(0,0) points at tile 2. Fill tile 2's column 0 with
  // ascending nibbles 1..8 across rows 0..7 (row0=1, row1=2, ..., row7=8);
  // all other columns left at 0 (transparent-irrelevant, only column 0 is
  // queried at screen x=0).
  uint32_t tile_addr = 2U * GENESIS_VDP_TILE_BYTES;
  for (unsigned row = 0; row < GENESIS_VDP_TILE_HEIGHT; ++row) {
    uint8_t value = (uint8_t)(row + 1U); // 1..8
    f.vram[tile_addr + row * 4U] = (uint8_t)(value << 4); // high nibble = col0.
  }
  // Plane A transparent so it never interferes.
  write_nametable_entry(f.vram, 0xC000, /*priority=*/0, /*palette=*/0, /*tile_index=*/1);
  // Unflipped Plane B nametable entry at cell(0,0): screen (0,0) -> tile_y=0
  // -> pixel value 1.
  write_nametable_entry(f.vram, 0x8000, /*priority=*/0, /*palette=*/1, /*tile_index=*/2);

  write_cram_color(f.cram, 17, 2, 1, 0); // palette1,index1 -> cram 1*16+1=17.
  write_cram_color(f.cram, 24, 5, 5, 5); // palette1,index8 -> cram 1*16+8=24.
  GenesisRgb888 expected_unflipped =
      genesis_vdp_decode_cram_word((uint16_t)((0U << 9) | (1U << 5) | (2U << 1)));
  GenesisRgb888 expected_flipped =
      genesis_vdp_decode_cram_word((uint16_t)((5U << 9) | (5U << 5) | (5U << 1)));

  GenesisRgb888 actual_unflipped;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual_unflipped) == 0,
        "compose succeeds (unflipped Plane B tile)");
  check(colors_equal(actual_unflipped, expected_unflipped),
        "unflipped Plane B tile samples row 0's stored pixel value (1)");

  // Now set v_flip on the same nametable entry; screen row 0 must now sample
  // the mirrored row 7 (stored value 8) instead.
  write_word(f.vram, 0x8000,
             (uint16_t)((0x01U << 12) | (1U << 13) | (2U & GENESIS_VDP_TILE_INDEX_MASK)));
  GenesisRgb888 actual_flipped;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &actual_flipped) == 0,
        "compose succeeds (v-flipped Plane B tile)");
  check(colors_equal(actual_flipped, expected_flipped),
        "v-flipped Plane B tile samples the vertically-mirrored row (stored row 7, value 8) at screen "
        "row 0");
}

void test_compose_fails_closed_outside_bound_display_mode() {
  Fixture f;
  f.regs[12] = 0x00; // Neither RS0 nor RS1: H32, outside this checkpoint's bound surface.
  GenesisRgb888 color;
  color.r = 0xAB;
  color.g = 0xCD;
  color.b = 0xEF;
  check(genesis_vdp_compose_pixel(f.vram, f.vsram, f.cram, f.regs, 0, 0, &color) < 0,
        "compose fails closed when display mode is outside the bound Mode-5/H40/non-interlaced surface");
  check(color.r == 0xAB && color.g == 0xCD && color.b == 0xEF,
        "compose leaves color_out unmodified on a display-mode gate failure");
}

} // namespace

int main() {
  test_transparent_plane_a_over_opaque_plane_b();
  test_priority_bit_matrix_planes_only();
  test_transparent_sprite_does_not_occlude_plane();
  test_low_priority_sprite_vs_both_planes();
  test_high_priority_sprite_vs_both_planes();
  test_overlapping_sprites_earlier_traversal_order_wins();
  test_sprite_traversal_order_ignores_priority_then_plane_a_wins();
  test_plane_h_flip_samples_mirrored_column();
  test_plane_v_flip_samples_mirrored_row();
  test_backdrop_when_everything_transparent();
  test_palette_selection_preserved_independently();
  test_cram_index_helper();
  test_compose_rejects_null_arguments();
  test_compose_fails_closed_outside_bound_display_mode();

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
