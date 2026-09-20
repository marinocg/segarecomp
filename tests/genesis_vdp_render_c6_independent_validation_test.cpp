// SEG-007-T049 checkpoint C6: independent synthetic validation of the
// already-complete C1-C5 renderer/frame path (platforms/genesis/runtime/vdp_render.c).
//
// This file adds NO new rendering features. It builds one project-authored
// synthetic VRAM/VSRAM/CRAM/register fixture exercising Plane A/B
// addressing, full-screen horizontal scroll, per-2-cell-column vertical
// scroll, plane tile H-flip, multi-sprite SAT traversal (including a
// multi-cell sprite and a sprite H-flip), sprite-vs-sprite traversal-order
// precedence, sprite-vs-plane priority interaction (one case per direction),
// palette-index-0 transparency, all four CRAM palette lines, and backdrop
// pixels -- then checks the resulting `GenesisFrameArtifact` at a bounded
// set of hand-picked representative coordinates plus its exact
// `frame_digest`.
//
// Independence of the oracle: every expected value asserted below (each
// coordinate's CRAM-index byte, and the final 32-byte frame_digest) was
// derived by a small Python re-implementation of this checkpoint's own
// documented addressing/compositing math (see vdp_render.h's C1-C5
// citations), written from scratch as an independent, structurally separate
// re-derivation -- not by copying any function from vdp_render.c and not by
// calling genesis_vdp_produce_frame, genesis_vdp_compose_pixel,
// genesis_vdp_resolve_plane_pixel, genesis_vdp_sat_traverse,
// genesis_vdp_resolve_sprite_pixel, or any other function under test. That
// Python re-implementation is not part of this repository (it was a
// one-time offline derivation, mirroring this file's own Part A1 technique
// for precomputing a SHA-256 digest offline); every fixture byte it
// consumed is reproduced exactly by the C++ helpers below, and its logic is
// summarized (not reproduced as runnable source) in this comment block for
// review:
//
//   (fixture construction: identical to build_fixture() below, using the
//   same VRAM/VSRAM/CRAM/register byte values -- see build_fixture()'s own
//   inline citations back to the specific screen coordinate each write
//   supports)
//
//   def decode_plane_size(): ... (mirrors genesis_vdp_decode_plane_size's
//     documented register #16 field table, independently re-typed)
//   def plane_base(plane): ((reg2<<10)&0xE000) for A, ((reg4<<13)&0xE000) for B
//   def decode_nt(word): tile=word&0x7FF; hflip=(word>>11)&1; vflip=(word>>12)&1;
//     pal=(word>>13)&3; pri=(word>>15)&1
//   def read_hscroll(plane): table_base=(reg13&0x3F)<<10; sign-extend the
//     16-bit word at table_base+(0 for A, 2 for B) by its low 10 bits
//   def read_vscroll(plane, screen_cell_col): group=screen_cell_col/2;
//     sign-extend the 16-bit word at vsram[group*4+(0 for A, 2 for B)]
//   def resolve_plane_candidate(plane, sx, sy): plane_x=(sx-hscroll) mod
//     width_px; plane_y=(sy+vscroll) mod height_px; decode nametable entry
//     at the resulting cell; apply h/v flip to the tile-local (tile_x,
//     tile_y) before the nibble decode; opaque iff decoded index != 0
//   def sat_traverse(): iterate the SAT linked list starting at sprite 0,
//     following the link field, stopping at link==0, an already-visited
//     index, or an out-of-range link
//   def resolve_sprite_pixel(sprite, sx, sy): mirror the whole bounding box
//     per h/v flip before the column-major (col*height+row) tile-offset
//     split, then nibble-decode
//   def resolve_cram_index(sx, sy): apply the six-layer precedence order
//     (sprite-high, A-high, B-high, sprite-low, A-low, B-low, backdrop)
//     exactly as documented in vdp_render.h's C4 citations
//   (full frame): pixels[row][col] = resolve_cram_index(col, row) for every
//     one of the 320x224 coordinates; frame_digest =
//     SHA256(bytes(pixels) + bytes(cram)) via Python's hashlib, matching
//     this project's own documented digest field order
//
// A large hand-computed table of expected pixel values at representative
// coordinates (rather than a full independent 320x224 re-implementation
// embedded in this file) is used here for reviewability, per this
// checkpoint's own stated preference; the offline oracle above additionally
// cross-checked the exact 32-byte frame_digest, which -- unlike a handful of
// individual pixels -- cannot practically be hand-verified coordinate by
// coordinate, so it is included as an extra, independently-derived
// end-to-end check.

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

void write_word(uint8_t *buf, uint32_t addr, uint16_t value) {
  buf[addr] = (uint8_t)(value >> 8);
  buf[addr + 1] = (uint8_t)(value & 0xFF);
}

constexpr unsigned kPlaneWidthCells = 64U; // register #16 -> 64x32.

uint16_t nametable_word(uint8_t priority, uint8_t palette, uint8_t vflip, uint8_t hflip,
                         uint16_t tile_index) {
  return (uint16_t)(((unsigned)priority << 15) | ((unsigned)palette << 13) |
                     ((unsigned)vflip << 12) | ((unsigned)hflip << 11) |
                     ((unsigned)tile_index & GENESIS_VDP_TILE_INDEX_MASK));
}

void write_nametable_entry(uint8_t *vram, uint32_t plane_base, unsigned col, unsigned row,
                            uint8_t priority, uint8_t palette, uint16_t tile_index,
                            uint8_t vflip = 0, uint8_t hflip = 0) {
  uint32_t addr = plane_base + (row * kPlaneWidthCells + col) * 2U;
  write_word(vram, addr, nametable_word(priority, palette, vflip, hflip, tile_index));
}

void set_tile_pixel00(uint8_t *vram, unsigned tile_number, uint8_t pattern_index) {
  uint32_t addr = tile_number * GENESIS_VDP_TILE_BYTES;
  vram[addr] = (uint8_t)((vram[addr] & 0x0FU) | (uint8_t)(pattern_index << 4));
}

// Fills tile `tile_number`'s row 0 with ascending nibbles 1..8 across
// columns 0..7 (col0=1, col1=2, ..., col7=8) -- used by the flip tests below
// to make a mirrored sample independently distinguishable.
void set_tile_row0_ascending(uint8_t *vram, unsigned tile_number) {
  uint32_t addr = tile_number * GENESIS_VDP_TILE_BYTES;
  vram[addr + 0] = 0x12;
  vram[addr + 1] = 0x34;
  vram[addr + 2] = 0x56;
  vram[addr + 3] = 0x78;
}

uint32_t kSatBase = 0x5000;

void write_sprite(uint8_t *vram, unsigned sprite_index, int32_t x, int32_t y, unsigned width_cells,
                   unsigned height_cells, unsigned link, uint8_t priority, uint8_t palette,
                   uint16_t tile_index, uint8_t hflip = 0, uint8_t vflip = 0) {
  uint32_t addr = kSatBase + sprite_index * GENESIS_VDP_SPRITE_ENTRY_BYTES;
  uint16_t raw_y = (uint16_t)((y + GENESIS_VDP_SPRITE_COORD_BIAS) & 0x1FF);
  uint16_t raw_x = (uint16_t)((x + GENESIS_VDP_SPRITE_COORD_BIAS) & 0x1FF);
  uint8_t size_byte =
      (uint8_t)((((width_cells - 1U) & 0x03U) << 2) | ((height_cells - 1U) & 0x03U));
  uint16_t tile_word = (uint16_t)(((unsigned)priority << 15) | ((unsigned)palette << 13) |
                                   ((unsigned)vflip << 12) | ((unsigned)hflip << 11) |
                                   ((unsigned)tile_index & GENESIS_VDP_TILE_INDEX_MASK));
  write_word(vram, addr + 0, raw_y);
  vram[addr + 2] = size_byte;
  vram[addr + 3] = (uint8_t)(link & GENESIS_VDP_SPRITE_LINK_MASK);
  write_word(vram, addr + 4, tile_word);
  write_word(vram, addr + 6, raw_x);
}

void write_cram_color(uint8_t *cram, unsigned entry_index, uint8_t r3, uint8_t g3, uint8_t b3) {
  uint16_t word = (uint16_t)(((uint16_t)(b3 & 0x07U) << 9) | ((uint16_t)(g3 & 0x07U) << 5) |
                              ((uint16_t)(r3 & 0x07U) << 1));
  write_word(cram, entry_index * 2U, word);
}

struct Fixture {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
};

// Builds the one coherent fixture this whole file validates. See the file
// header comment above for the independent Python re-derivation this
// fixture's expected values were cross-checked against.
void build_fixture(Fixture *f) {
  std::memset(f->vram, 0, sizeof(f->vram));
  std::memset(f->vsram, 0, sizeof(f->vsram));
  std::memset(f->cram, 0, sizeof(f->cram));
  std::memset(f->regs, 0, sizeof(f->regs));

  f->regs[1] = 0x04;  // Mode 5 select.
  f->regs[2] = 0x30;  // Plane A base -> 0xC000.
  f->regs[4] = 0x04;  // Plane B base -> 0x8000.
  f->regs[5] = 0x28;  // SAT base -> 0x5000.
  f->regs[7] = 0x09;  // Backdrop: palette 0, index 9 -> CRAM index 9.
  f->regs[11] = 0x04; // full-screen H-scroll, 2-cell V-scroll.
  f->regs[12] = 0x81; // H40, non-interlaced.
  f->regs[13] = 0x00; // H-scroll table base = 0.
  f->regs[16] = 0x01; // 64x32 plane size (width field 1 -> 64, height field 0 -> 32).

  // Full-screen H-scroll: Plane A shifted right by 8px (plane_x = screen_x -
  // 8); Plane B not scrolled. Table base is register #13 == 0.
  write_word(f->vram, 0, 8); // Plane A hscroll = 8.
  write_word(f->vram, 2, 0); // Plane B hscroll = 0.

  // Per-2-cell-column V-scroll: only the column group covering screen tile
  // columns 6-7 (screen x 48-63, group index 3) gets a non-zero Plane B
  // vscroll (8px down); every other group (including all of Plane A) stays
  // at vscroll 0. VSRAM offset = group*4 + 2 (Plane B's word in the group).
  write_word(f->vsram, 3U * 4U + 2U, 8);

  // --- Plane A addressing (coordinate A: screen (24,24)) ---
  // plane_x = 24 - hscrollA(8) = 16 -> cell_col 2; plane_y = 24 + 0 = 24 ->
  // cell_row 3. Cell(2,3): tile 10, palette 0, priority 0, opaque index 5.
  write_nametable_entry(f->vram, 0xC000, 2, 3, /*priority=*/0, /*palette=*/0, /*tile_index=*/10);
  set_tile_pixel00(f->vram, 10, 5);

  // --- Plane B addressing (coordinate B: screen (104,48)) ---
  // plane_x = 104 - 0 = 104 -> cell_col 13; plane_y = 48 + 0 (group 6 has no
  // vscroll) = 48 -> cell_row 6. Cell(13,6): tile 20, palette 1, opaque
  // index 13 -> CRAM 1*16+13=29.
  write_nametable_entry(f->vram, 0x8000, 13, 6, /*priority=*/0, /*palette=*/1, /*tile_index=*/20);
  set_tile_pixel00(f->vram, 20, 13);

  // --- Per-2-cell-column V-scroll (coordinate C: screen (56,0)) ---
  // Plane B: plane_x = 56 - 0 = 56 -> cell_col 7; plane_y = 0 + vscrollB(8,
  // group 3) = 8 -> cell_row 1. Cell(7,1): tile 21, palette 2, opaque index
  // 11 -> CRAM 2*16+11=43. Without the vscroll shift this would instead read
  // cell(7,0), which is deliberately left untouched (transparent).
  write_nametable_entry(f->vram, 0x8000, 7, 1, /*priority=*/0, /*palette=*/2, /*tile_index=*/21);
  set_tile_pixel00(f->vram, 21, 11);

  // --- Plane tile H-flip (coordinate D: screen (208,104)) ---
  // Plane A: plane_x = 208 - 8 = 200 -> cell_col 25, tile_x 0; plane_y = 104
  // -> cell_row 13, tile_y 0. Cell(25,13): tile 30, palette 3, h_flip set.
  // tile 30's row 0 holds ascending nibbles 1..8 (col0=1..col7=8); h_flip
  // mirrors tile_x 0 to stored column 7 (value 8) -> CRAM 3*16+8=56.
  write_nametable_entry(f->vram, 0xC000, 25, 13, /*priority=*/0, /*palette=*/3, /*tile_index=*/30,
                         /*vflip=*/0, /*hflip=*/1);
  set_tile_row0_ascending(f->vram, 30);

  // --- Sprite/plane priority interaction fixtures (coordinates H, I) ---
  // Plane A cell(31,19) (screen (256,152) unscrolled-by-8): tile 80,
  // palette 0, HIGH priority, opaque index 5.
  write_nametable_entry(f->vram, 0xC000, 31, 19, /*priority=*/1, /*palette=*/0, /*tile_index=*/80);
  set_tile_pixel00(f->vram, 80, 5);
  // Plane A cell(32,19) (screen (264,152)): tile 81, palette 1, HIGH
  // priority, opaque index 9.
  write_nametable_entry(f->vram, 0xC000, 32, 19, /*priority=*/1, /*palette=*/1, /*tile_index=*/81);
  set_tile_pixel00(f->vram, 81, 9);

  // --- SAT chain: 6 sprites, indices 0-5, linked 0->1->2->3->4->5->end. ---

  // Sprites 0 and 1 both cover screen (150,150) (sprite-vs-sprite
  // traversal-order precedence): sprite 0 (traversed first) wins even
  // though sprite 1 has the higher priority bit.
  write_sprite(f->vram, 0, /*x=*/150, /*y=*/150, 1, 1, /*link=*/1, /*priority=*/0, /*palette=*/0,
               /*tile_index=*/40);
  set_tile_pixel00(f->vram, 40, 6);
  write_sprite(f->vram, 1, /*x=*/150, /*y=*/150, 1, 1, /*link=*/2, /*priority=*/1, /*palette=*/1,
               /*tile_index=*/41);
  set_tile_pixel00(f->vram, 41, 8);

  // Sprite 2: 2-cells-wide x 1-cell-tall, at (180,150) (multi-cell sprite
  // tile selection, coordinate F at its second cell, screen (188,150)).
  // Column-major tile order: cell(1,0) -> tile_index + 1*height_cells + 0 =
  // 50 + 1 = 51.
  write_sprite(f->vram, 2, /*x=*/180, /*y=*/150, 2, 1, /*link=*/3, /*priority=*/0, /*palette=*/2,
               /*tile_index=*/50);
  set_tile_pixel00(f->vram, 51, 12);

  // Sprite 3: 1x1 at (210,150), H-flipped (coordinate G). tile 60's row 0
  // holds ascending nibbles 1..8; h_flip mirrors local_x 0 to stored column
  // 7 (value 8) -> CRAM 1*16+8=24.
  write_sprite(f->vram, 3, /*x=*/210, /*y=*/150, 1, 1, /*link=*/4, /*priority=*/0, /*palette=*/1,
               /*tile_index=*/60, /*hflip=*/1);
  set_tile_row0_ascending(f->vram, 60);

  // Sprite 4: 1x1 at (256,152), HIGH priority (coordinate H: sprite priority
  // beats the also-opaque, also-HIGH-priority Plane A cell(31,19) above,
  // since sprite-high precedes planeA-high in the six-layer order).
  write_sprite(f->vram, 4, /*x=*/256, /*y=*/152, 1, 1, /*link=*/5, /*priority=*/1, /*palette=*/3,
               /*tile_index=*/70);
  set_tile_pixel00(f->vram, 70, 10);

  // Sprite 5: 1x1 at (264,152), LOW priority, end of chain (coordinate I:
  // Plane A cell(32,19) above is HIGH priority and beats this LOW-priority,
  // also-opaque sprite, since planeA-high precedes sprite-low).
  write_sprite(f->vram, 5, /*x=*/264, /*y=*/152, 1, 1, /*link=*/0, /*priority=*/0, /*palette=*/2,
               /*tile_index=*/71);
  set_tile_pixel00(f->vram, 71, 4);

  // CRAM: one distinguishable color per expected winning CRAM index below.
  write_cram_color(f->cram, 5, 1, 0, 0);
  write_cram_color(f->cram, 29, 0, 1, 0);
  write_cram_color(f->cram, 43, 0, 0, 1);
  write_cram_color(f->cram, 56, 1, 1, 0);
  write_cram_color(f->cram, 6, 0, 1, 1);
  write_cram_color(f->cram, 44, 1, 0, 1);
  write_cram_color(f->cram, 24, 1, 1, 1);
  write_cram_color(f->cram, 58, 2, 0, 0);
  write_cram_color(f->cram, 25, 0, 2, 0);
  write_cram_color(f->cram, 9, 0, 0, 2); // backdrop color.
}

void test_c6_complete_frame_matches_independent_oracle() {
  Fixture f;
  build_fixture(&f);

  GenesisFrameArtifact frame;
  check(genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, f.regs, &frame) == 0,
        "produce_frame succeeds on the C6 combined fixture");

  struct Coord {
    unsigned x, y;
    uint8_t expected;
    const char *what;
  };
  static const Coord kCoords[] = {
      {24, 24, 5, "A: Plane A addressing (scrolled) wins -> CRAM index 5"},
      {104, 48, 29, "B: Plane B addressing wins -> CRAM index 29"},
      {56, 0, 43, "C: per-2-cell-column V-scroll shifts Plane B content -> CRAM index 43"},
      {208, 104, 56, "D: H-flipped Plane A tile samples mirrored column -> CRAM index 56"},
      {150, 150, 6, "E: earlier-traversed sprite 0 wins over overlapping sprite 1 -> CRAM index 6"},
      {188, 150, 44, "F: multi-cell sprite's second (column-major) tile -> CRAM index 44"},
      {210, 150, 24, "G: H-flipped sprite samples mirrored column -> CRAM index 24"},
      {256, 152, 58, "H: HIGH-priority sprite beats HIGH-priority Plane A -> CRAM index 58"},
      {264, 152, 25, "I: HIGH-priority Plane A beats LOW-priority sprite -> CRAM index 25"},
      {25, 24, 9, "J: palette-index-0 transparency (adjacent to A) shows backdrop -> CRAM index 9"},
      {300, 200, 9, "K: pure backdrop (nothing opaque anywhere) -> CRAM index 9"},
  };
  for (const Coord &c : kCoords) {
    uint8_t got = frame.pixels[c.y * GENESIS_FRAME_WIDTH + c.x];
    check(got == c.expected, c.what);
  }

  check(std::memcmp(frame.palette_snapshot, f.cram, GENESIS_VDP_CRAM_BYTES) == 0,
        "palette_snapshot is a verbatim copy of the input cram buffer");

  // Independently offline-precomputed (Python re-implementation, see file
  // header comment) SHA-256 of this exact fixture's full pixels||cram byte
  // layout -- not produced by calling genesis_sha256_* here.
  static const uint8_t kExpectedDigest[32] = {
      0x59, 0x3e, 0x7f, 0xed, 0x94, 0x8f, 0xc4, 0xa0, 0x4b, 0xaa, 0x35, 0xbf,
      0x3e, 0x31, 0x8f, 0xb0, 0x30, 0x09, 0xa2, 0x46, 0xf9, 0xd8, 0xa4, 0x1a,
      0x78, 0x42, 0xf4, 0x1f, 0x65, 0x23, 0xf8, 0x44};
  check(std::memcmp(frame.frame_digest, kExpectedDigest, 32U) == 0,
        "frame_digest matches the independently offline-precomputed SHA-256 oracle");
}

// Fail-closed adversarial case: H32 (register #12 selects neither RS0 nor
// RS1) is outside this checkpoint's bound Mode-5/H40/non-interlaced surface
// (matches the established C2/C4/C5 fail-closed fixture convention).
// produce_frame must fail and must not produce a misleading/partial frame
// artifact.
void test_c6_fails_closed_outside_bound_surface_leaves_sentinel_untouched() {
  Fixture f;
  build_fixture(&f);
  f.regs[12] = 0x00; // Neither RS0 nor RS1 set -> H32, unsupported.

  GenesisFrameArtifact frame;
  std::memset(&frame, 0xAB, sizeof(frame)); // Sentinel-prefill.

  int result = genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, f.regs, &frame);
  check(result < 0, "produce_frame fails closed for an unsupported (H32) display mode");

  uint8_t sentinel_pixels[sizeof(frame.pixels)];
  uint8_t sentinel_palette[sizeof(frame.palette_snapshot)];
  uint8_t sentinel_digest[sizeof(frame.frame_digest)];
  std::memset(sentinel_pixels, 0xAB, sizeof(sentinel_pixels));
  std::memset(sentinel_palette, 0xAB, sizeof(sentinel_palette));
  std::memset(sentinel_digest, 0xAB, sizeof(sentinel_digest));

  check(std::memcmp(frame.pixels, sentinel_pixels, sizeof(sentinel_pixels)) == 0,
        "a fail-closed call leaves pixels completely unmodified (fully sentinel, not partially filled)");
  check(std::memcmp(frame.palette_snapshot, sentinel_palette, sizeof(sentinel_palette)) == 0,
        "a fail-closed call leaves palette_snapshot completely unmodified");
  check(std::memcmp(frame.frame_digest, sentinel_digest, sizeof(sentinel_digest)) == 0,
        "a fail-closed call leaves frame_digest completely unmodified");
}

} // namespace

int main() {
  test_c6_complete_frame_matches_independent_oracle();
  test_c6_fails_closed_outside_bound_surface_leaves_sentinel_untouched();

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
