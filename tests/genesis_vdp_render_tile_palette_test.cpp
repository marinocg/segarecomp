// SEG-007-T049 checkpoint C1: focused unit coverage for
// platforms/genesis/runtime/vdp_render.c's tile-pattern and CRAM-palette decode. All
// fixtures are project-authored synthetic byte patterns with hand-computed
// expected pixel/color values -- no commercial ROM content is read or
// referenced.

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

// --- Tile-pattern decode -----------------------------------------------

// Synthetic tile: row 0 = 0x01 0x23 0x45 0x67, i.e. nibbles
// 0,1,2,3,4,5,6,7 left-to-right (high nibble first per GTO1 pp. 53-54).
// Remaining rows are 0x89 0xAB 0xCD 0xEF repeated, i.e. nibbles
// 8,9,A,B,C,D,E,F, to exercise the full 0-15 palette-index range.
void test_decode_tile_row0_ascending_nibbles() {
  uint8_t tile[GENESIS_VDP_TILE_BYTES] = {0};
  tile[0] = 0x01;
  tile[1] = 0x23;
  tile[2] = 0x45;
  tile[3] = 0x67;

  const uint8_t expected_row0[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  for (unsigned col = 0; col < 8; ++col) {
    uint8_t index_value = 0xFF;
    int rc = genesis_vdp_decode_tile_pixel(tile, 0, col, &index_value);
    check(rc == 0, "decode_tile_pixel row0 succeeds");
    check(index_value == expected_row0[col], "decode_tile_pixel row0 nibble value");
  }
}

void test_decode_tile_full_range_and_whole_tile_helper_agree() {
  uint8_t tile[GENESIS_VDP_TILE_BYTES];
  for (unsigned row = 0; row < 8; ++row) {
    tile[row * 4 + 0] = 0x89;
    tile[row * 4 + 1] = 0xAB;
    tile[row * 4 + 2] = 0xCD;
    tile[row * 4 + 3] = 0xEF;
  }

  uint8_t indices[GENESIS_VDP_TILE_WIDTH * GENESIS_VDP_TILE_HEIGHT];
  int rc = genesis_vdp_decode_tile(tile, indices);
  check(rc == 0, "decode_tile succeeds");

  const uint8_t expected_row[8] = {8, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
  for (unsigned row = 0; row < 8; ++row) {
    for (unsigned col = 0; col < 8; ++col) {
      check(indices[row * 8 + col] == expected_row[col], "decode_tile whole-tile pixel value");

      uint8_t single_index = 0xFF;
      int single_rc = genesis_vdp_decode_tile_pixel(tile, row, col, &single_index);
      check(single_rc == 0, "decode_tile_pixel agrees rc");
      check(single_index == expected_row[col], "decode_tile_pixel agrees with decode_tile");
    }
  }
}

void test_decode_tile_pixel_rejects_malformed_input() {
  uint8_t tile[GENESIS_VDP_TILE_BYTES] = {0};
  uint8_t index_value = 0;

  check(genesis_vdp_decode_tile_pixel(nullptr, 0, 0, &index_value) != 0,
        "decode_tile_pixel rejects NULL tile");
  check(genesis_vdp_decode_tile_pixel(tile, 0, 0, nullptr) != 0,
        "decode_tile_pixel rejects NULL index_out");
  check(genesis_vdp_decode_tile_pixel(tile, GENESIS_VDP_TILE_HEIGHT, 0, &index_value) != 0,
        "decode_tile_pixel rejects row == height (boundary)");
  check(genesis_vdp_decode_tile_pixel(tile, 0, GENESIS_VDP_TILE_WIDTH, &index_value) != 0,
        "decode_tile_pixel rejects col == width (boundary)");
  check(genesis_vdp_decode_tile_pixel(tile, 0xFFFFFFFFU, 0, &index_value) != 0,
        "decode_tile_pixel rejects grossly out-of-range row");

  check(genesis_vdp_decode_tile(nullptr, nullptr) != 0, "decode_tile rejects NULL tile");
  uint8_t indices[GENESIS_VDP_TILE_WIDTH * GENESIS_VDP_TILE_HEIGHT];
  check(genesis_vdp_decode_tile(tile, nullptr) != 0, "decode_tile rejects NULL indices_out");
  check(genesis_vdp_decode_tile(nullptr, indices) != 0, "decode_tile rejects NULL tile (2)");
}

// --- CRAM color decode ---------------------------------------------------

// Hand-computed: raw word 0x0000 -> R=G=B=0 -> (0,0,0).
void test_decode_cram_word_black() {
  GenesisRgb888 color = genesis_vdp_decode_cram_word(0x0000U);
  check(color.r == 0 && color.g == 0 && color.b == 0, "cram word 0x0000 decodes to black");
}

// Hand-computed per the documented ----BBB-GGG-RRR- layout:
// raw word 0x0E00 -> bits 11-9 = 111 (B=7) -> scaled 7*255/7=255; G=R=0.
void test_decode_cram_word_pure_blue_max() {
  GenesisRgb888 color = genesis_vdp_decode_cram_word(0x0E00U);
  check(color.r == 0, "cram word 0x0E00 red channel is 0");
  check(color.g == 0, "cram word 0x0E00 green channel is 0");
  check(color.b == 255, "cram word 0x0E00 blue channel scales 7 -> 255");
}

// raw word 0x00E0 -> bits 7-5 = 111 (G=7) -> scaled 255; R=B=0.
void test_decode_cram_word_pure_green_max() {
  GenesisRgb888 color = genesis_vdp_decode_cram_word(0x00E0U);
  check(color.r == 0, "cram word 0x00E0 red channel is 0");
  check(color.g == 255, "cram word 0x00E0 green channel scales 7 -> 255");
  check(color.b == 0, "cram word 0x00E0 blue channel is 0");
}

// raw word 0x000E -> bits 3-1 = 111 (R=7) -> scaled 255; G=B=0.
void test_decode_cram_word_pure_red_max() {
  GenesisRgb888 color = genesis_vdp_decode_cram_word(0x000EU);
  check(color.r == 255, "cram word 0x000E red channel scales 7 -> 255");
  check(color.g == 0, "cram word 0x000E green channel is 0");
  check(color.b == 0, "cram word 0x000E blue channel is 0");
}

// raw word 0x0EEE -> full white: R=G=B=7 -> (255,255,255). This is the
// well-known "white" CRAM constant used across Genesis VDP tooling.
void test_decode_cram_word_white() {
  GenesisRgb888 color = genesis_vdp_decode_cram_word(0x0EEEU);
  check(color.r == 255 && color.g == 255 && color.b == 255, "cram word 0x0EEE decodes to white");
}

// Mid-scale value: 3-bit channel 3 -> 3*255/7 = 109 (integer division,
// hand-computed).
void test_decode_cram_word_mid_scale_channel() {
  // R=3 (bits3-1 = 011), G=0, B=0.
  uint16_t raw_word = (uint16_t)(3U << 1);
  GenesisRgb888 color = genesis_vdp_decode_cram_word(raw_word);
  check(color.r == 109, "cram word mid-scale red channel: 3*255/7 == 109");
  check(color.g == 0, "cram word mid-scale green channel is 0");
  check(color.b == 0, "cram word mid-scale blue channel is 0");
}

// Unused bits (bit 0, bit 4, bit 8, and bits 15-12) must be ignored, never
// rejected or folded into a channel value.
void test_decode_cram_word_ignores_unused_bits() {
  GenesisRgb888 base = genesis_vdp_decode_cram_word(0x0EEEU);
  GenesisRgb888 with_unused_bits_set = genesis_vdp_decode_cram_word((uint16_t)0xFFFFU);
  check(base.r == with_unused_bits_set.r && base.g == with_unused_bits_set.g &&
            base.b == with_unused_bits_set.b,
        "cram word decode ignores unused bits (0,4,8,15-12)");
}

// --- CRAM entry decode (big-endian halfword storage, indexed 0-63) ------

void test_decode_cram_entry_reads_big_endian_halfword() {
  uint8_t cram[GENESIS_VDP_CRAM_BYTES] = {0};
  // Entry 5: raw word 0x0E00 (pure blue max), stored big-endian at
  // byte offset 10/11.
  cram[10] = 0x0E;
  cram[11] = 0x00;

  GenesisRgb888 color = {0, 0, 0};
  int rc = genesis_vdp_decode_cram_entry(cram, 5, &color);
  check(rc == 0, "decode_cram_entry succeeds for entry 5");
  check(color.r == 0 && color.g == 0 && color.b == 255, "decode_cram_entry entry 5 is pure blue max");

  // Entry 0 remains black (all-zero fixture).
  GenesisRgb888 entry0 = {1, 1, 1};
  rc = genesis_vdp_decode_cram_entry(cram, 0, &entry0);
  check(rc == 0, "decode_cram_entry succeeds for entry 0");
  check(entry0.r == 0 && entry0.g == 0 && entry0.b == 0, "decode_cram_entry entry 0 is black");

  // Last entry (63), byte offset 126/127 -- boundary check.
  cram[126] = 0x00;
  cram[127] = 0x0EU; // raw word 0x000E -> pure red max.
  GenesisRgb888 entry63 = {0, 0, 0};
  rc = genesis_vdp_decode_cram_entry(cram, GENESIS_VDP_CRAM_ENTRY_COUNT - 1, &entry63);
  check(rc == 0, "decode_cram_entry succeeds for entry 63 (last valid index)");
  check(entry63.r == 255 && entry63.g == 0 && entry63.b == 0,
        "decode_cram_entry entry 63 is pure red max");
}

void test_decode_cram_entry_rejects_malformed_input() {
  uint8_t cram[GENESIS_VDP_CRAM_BYTES] = {0};
  GenesisRgb888 color = {0, 0, 0};

  check(genesis_vdp_decode_cram_entry(nullptr, 0, &color) != 0,
        "decode_cram_entry rejects NULL cram");
  check(genesis_vdp_decode_cram_entry(cram, 0, nullptr) != 0,
        "decode_cram_entry rejects NULL color_out");
  check(genesis_vdp_decode_cram_entry(cram, GENESIS_VDP_CRAM_ENTRY_COUNT, &color) != 0,
        "decode_cram_entry rejects entry_index == count (boundary)");
  check(genesis_vdp_decode_cram_entry(cram, 0xFFFFFFFFU, &color) != 0,
        "decode_cram_entry rejects grossly out-of-range entry_index");
}

} // namespace

int main() {
  test_decode_tile_row0_ascending_nibbles();
  test_decode_tile_full_range_and_whole_tile_helper_agree();
  test_decode_tile_pixel_rejects_malformed_input();

  test_decode_cram_word_black();
  test_decode_cram_word_pure_blue_max();
  test_decode_cram_word_pure_green_max();
  test_decode_cram_word_pure_red_max();
  test_decode_cram_word_white();
  test_decode_cram_word_mid_scale_channel();
  test_decode_cram_word_ignores_unused_bits();

  test_decode_cram_entry_reads_big_endian_halfword();
  test_decode_cram_entry_rejects_malformed_input();

  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
