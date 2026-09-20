// SEG-007-T049 checkpoint C5: focused unit coverage for
// platforms/genesis/runtime/vdp_render.c's deterministic frame-output producer
// (genesis_vdp_produce_frame). All fixtures are project-authored synthetic
// VRAM/VSRAM/CRAM/register byte patterns with independently hand-derived
// expected results -- no commercial ROM content is read or referenced.
// Which layer (backdrop, Plane A, Plane B, or a sprite) is expected to win
// at each hand-picked (x, y) is decided by hand from this project's own
// documented six-layer priority order (see vdp_render.h's C4 citations),
// never by calling genesis_vdp_produce_frame or genesis_vdp_compose_pixel
// itself to derive the expectation.

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

// SEG-007-T049 Part A3: fixture width, in cells, for this file's whole-frame
// plane (register #16 field value 1 -> 64 cells; see
// genesis_vdp_decode_plane_size). This is a representative
// wider-than-32-cell configuration -- the horizontal H40 (320px) frame no
// longer wraps within one 256px-wide plane the way a 32-cell plane would,
// matching T049's actual bound route more closely than a 32x32 plane does.
// Register #16's height field is left at 0 (32 cells), the other supported
// combination alongside width. This value is the ONLY thing establishing
// plane width for every helper below; genesis_vdp_decode_plane_size decodes
// it generically in production code, unchanged by this test file.
constexpr unsigned kFixturePlaneWidthCells = 64U;

// Mode 5, H40, non-interlaced; Plane A base 0xC000 (reg2=0x30), Plane B base
// 0x8000 (reg4=0x04); 64x32 plane size (reg16=0x01: width field 1 -> 64
// cells, height field 0 -> 32 cells; plane is 512x256px, so every screen
// pixel used below falls within one wrap of the plane, no scroll); full-
// screen H-scroll (table base 0, all-zero so hscroll=0) and 2-cell V-scroll
// (all-zero VSRAM so vscroll=0); SAT base 0x5000 (reg5=0x28, H40
// granularity). Matches genesis_vdp_render_composition_test.cpp's own
// fixture conventions except for the widened plane size (Part A3).
void make_default_registers(uint16_t regs[GENESIS_VDP_REGISTER_COUNT]) {
  std::memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[1] = 0x04;  // Mode 5 select.
  regs[2] = 0x30;  // Plane A base -> 0xC000.
  regs[4] = 0x04;  // Plane B base -> 0x8000.
  regs[5] = 0x28;  // SAT base -> 0x5000.
  regs[11] = 0x04; // full-screen H, 2-cell V.
  regs[12] = 0x81; // H40, non-interlaced.
  regs[13] = 0x00; // H-scroll table base = 0.
  regs[16] = 0x01; // 64x32 plane size (width field 1 -> 64, height field 0 -> 32).
}

void write_word(uint8_t *buf, uint32_t addr, uint16_t value) {
  buf[addr] = (uint8_t)(value >> 8);
  buf[addr + 1] = (uint8_t)(value & 0xFF);
}

// Writes a Mode-5 nametable entry word at plane_base + cell(cell_col,
// cell_row) * 2, for this file's kFixturePlaneWidthCells-wide plane (Part
// A3: hand-derived from the fixture's declared 64-cell width above, not the
// old 32-cell width).
void write_nametable_entry(uint8_t *vram, uint32_t plane_base, unsigned cell_col,
                            unsigned cell_row, uint8_t priority, uint8_t palette,
                            uint16_t tile_index) {
  uint16_t word = (uint16_t)(((unsigned)priority << 15) | ((unsigned)palette << 13) |
                              ((unsigned)tile_index & GENESIS_VDP_TILE_INDEX_MASK));
  uint32_t addr = plane_base + (cell_row * kFixturePlaneWidthCells + cell_col) * 2U;
  write_word(vram, addr, word);
}

// Writes tile `tile_number`'s pixel(0,0) (top-left) to `pattern_index`
// (0-15); all other pixels of the tile are left at whatever `vram` was
// already initialized to (callers memset vram to 0 first).
void write_tile_pixel00(uint8_t *vram, unsigned tile_number, uint8_t pattern_index) {
  uint32_t tile_addr = tile_number * GENESIS_VDP_TILE_BYTES;
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

void write_cram_color(uint8_t *cram, unsigned entry_index, uint8_t r3, uint8_t g3, uint8_t b3) {
  uint16_t word = (uint16_t)(((uint16_t)(b3 & 0x07U) << 9) | ((uint16_t)(g3 & 0x07U) << 5) |
                              ((uint16_t)(r3 & 0x07U) << 1));
  write_word(cram, entry_index * 2U, word);
}

// Hand-picked coordinates used throughout this file:
//   backdrop pixel: (200, 200) -- cell(25,25), never written, transparent on
//     both planes; no sprite placed there.
//   Plane-A-winning pixel: (0, 0) -- cell(0,0).
//   Plane-B-winning pixel: (8, 0) -- cell(1,0).
//   sprite-winning pixel: (16, 16) -- covered by a placed sprite, no plane
//     opaque there.
constexpr unsigned kBackdropX = 200, kBackdropY = 200;
constexpr unsigned kPlaneAX = 0, kPlaneAY = 0;
constexpr unsigned kPlaneBX = 8, kPlaneBY = 0;
constexpr unsigned kSpriteX = 16, kSpriteY = 16;

struct Fixture {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];

  // Hand-derived expected CRAM index bytes, filled in by build().
  uint8_t expected_backdrop_index = 0;
  uint8_t expected_plane_a_index = 0;
  uint8_t expected_plane_b_index = 0;
  uint8_t expected_sprite_index = 0;

  Fixture() {
    std::memset(vram, 0, sizeof(vram));
    std::memset(vsram, 0, sizeof(vsram));
    std::memset(cram, 0, sizeof(cram));
    make_default_registers(regs);
    build();
  }

  void build() {
    // Backdrop: register #7 palette 1, index 5 -> reg7=(1<<4)|5=0x15,
    // CRAM index = 1*16+5 = 21.
    regs[7] = 0x15;
    expected_backdrop_index = 21;

    // Plane A cell(0,0): tile 1, palette 0, priority 0, opaque index 4 ->
    // CRAM index = 0*16+4 = 4.
    write_nametable_entry(vram, 0xC000, 0, 0, /*priority=*/0, /*palette=*/0, /*tile_index=*/1);
    write_tile_pixel00(vram, 1, 4);
    expected_plane_a_index = 4;

    // Plane B cell(1,0) (screen x=8..15): tile 2, palette 1, priority 0,
    // opaque index 6 -> CRAM index = 1*16+6 = 22. Plane A cell(1,0) and
    // Plane B cell(0,0) are left transparent (tile 0, never touched) so they
    // do not interfere with either the Plane-A or Plane-B expectation pixel.
    write_nametable_entry(vram, 0x8000, 1, 0, /*priority=*/0, /*palette=*/1, /*tile_index=*/2);
    write_tile_pixel00(vram, 2, 6);
    expected_plane_b_index = 22;

    // Sprite at (16,16): opaque, palette 2, index 9, high priority -> CRAM
    // index = 2*16+9 = 41. Placed as sprite 0 (SAT traversal always starts
    // there); nothing else covers (16,16).
    write_sprite_entry(vram, 0x5000, (uint16_t)(kSpriteY + GENESIS_VDP_SPRITE_COORD_BIAS), 0x00,
                        /*link=*/0, sprite_tile_word(/*priority=*/1, /*palette=*/2, /*tile_index=*/9),
                        (uint16_t)(kSpriteX + GENESIS_VDP_SPRITE_COORD_BIAS));
    write_tile_pixel00(vram, 9, 9);
    expected_sprite_index = 41;

    write_cram_color(cram, expected_backdrop_index, 1, 1, 1);
    write_cram_color(cram, expected_plane_a_index, 2, 3, 4);
    write_cram_color(cram, expected_plane_b_index, 5, 6, 7);
    write_cram_color(cram, expected_sprite_index, 7, 0, 2);
  }
};

void test_deterministic_across_two_calls() {
  Fixture f;
  GenesisFrameArtifact frame1;
  GenesisFrameArtifact frame2;
  check(genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, f.regs, &frame1) == 0,
        "produce_frame succeeds (first call)");
  check(genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, f.regs, &frame2) == 0,
        "produce_frame succeeds (second call)");
  check(std::memcmp(frame1.pixels, frame2.pixels, sizeof(frame1.pixels)) == 0,
        "two calls from identical input state produce byte-identical pixels");
  check(std::memcmp(frame1.palette_snapshot, frame2.palette_snapshot,
                     sizeof(frame1.palette_snapshot)) == 0,
        "two calls from identical input state produce byte-identical palette_snapshot");
  check(std::memcmp(frame1.frame_digest, frame2.frame_digest, sizeof(frame1.frame_digest)) == 0,
        "two calls from identical input state produce byte-identical frame_digest");
}

void test_schema_dimensions() {
  GenesisFrameArtifact frame;
  check(sizeof(frame.pixels) == (size_t)GENESIS_FRAME_WIDTH * (size_t)GENESIS_FRAME_HEIGHT,
        "GenesisFrameArtifact.pixels is exactly GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT bytes");
  check(sizeof(frame.palette_snapshot) == (size_t)GENESIS_VDP_CRAM_BYTES,
        "GenesisFrameArtifact.palette_snapshot is exactly GENESIS_VDP_CRAM_BYTES bytes");
  check(sizeof(frame.frame_digest) == 32U, "GenesisFrameArtifact.frame_digest is exactly 32 bytes");
}

void test_known_pixels() {
  Fixture f;
  GenesisFrameArtifact frame;
  check(genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, f.regs, &frame) == 0,
        "produce_frame succeeds");

  check(frame.pixels[kBackdropY * GENESIS_FRAME_WIDTH + kBackdropX] == f.expected_backdrop_index,
        "backdrop pixel (200,200) decodes to the hand-derived register #7 CRAM index (21)");
  check(frame.pixels[kPlaneAY * GENESIS_FRAME_WIDTH + kPlaneAX] == f.expected_plane_a_index,
        "Plane-A-winning pixel (0,0) decodes to the hand-derived CRAM index (4)");
  check(frame.pixels[kPlaneBY * GENESIS_FRAME_WIDTH + kPlaneBX] == f.expected_plane_b_index,
        "Plane-B-winning pixel (8,0) decodes to the hand-derived CRAM index (22)");
  check(frame.pixels[kSpriteY * GENESIS_FRAME_WIDTH + kSpriteX] == f.expected_sprite_index,
        "sprite-winning pixel (16,16) decodes to the hand-derived CRAM index (41)");

  check(std::memcmp(frame.palette_snapshot, f.cram, GENESIS_VDP_CRAM_BYTES) == 0,
        "palette_snapshot is a verbatim copy of the input cram buffer");
}

// SEG-007-T049 Part A2: a genuinely isolated one-VRAM-byte mutation.
//
// The baseline and mutated fixtures are byte-identical everywhere except one
// VRAM tile-pattern byte: tile 1's pixel(0,0) nibble, which the baseline's
// build() sets to opaque palette index 4 (see write_tile_pixel00 above) and
// the mutated fixture changes to opaque palette index 7 instead. CRAM is left
// completely untouched between the two fixtures (no new CRAM color is added):
// the CRAM-palette-index byte written into frame.pixels[] by the mutation is
// itself the relevant, sufficient evidence of an indexed-color-artifact
// change -- this test does not need the two indices to also resolve to
// visually distinct RGB colors.
void test_one_vram_byte_mutation_changes_output_deterministically() {
  Fixture baseline;
  GenesisFrameArtifact baseline_frame;
  check(genesis_vdp_produce_frame(baseline.vram, baseline.vsram, baseline.cram, baseline.regs,
                                   &baseline_frame) == 0,
        "produce_frame succeeds (baseline)");

  Fixture mutated;
  // Mutate exactly one VRAM byte: tile 1's pixel(0,0) nibble, changing its
  // decoded pattern index from 4 (baseline) to 7. Nothing else in `mutated`
  // (including all of `cram`) differs from `baseline` at this point --
  // `mutated` is a fresh Fixture whose build() reproduces byte-identical
  // vram/vsram/cram/regs to `baseline`'s build() except for this one write.
  write_tile_pixel00(mutated.vram, /*tile_number=*/1, /*pattern_index=*/7);
  uint8_t expected_mutated_plane_a_index = 7; // 0*16+7, same palette (0) as baseline.

  // Confirm the two fixtures' VRAM buffers differ in exactly the one target
  // byte (tile 1's row-0 byte, offset GENESIS_VDP_TILE_BYTES*1 + 0), and that
  // every other VRAM byte -- and all of VSRAM/CRAM/registers -- is untouched.
  uint32_t mutated_byte_addr = 1U * GENESIS_VDP_TILE_BYTES + 0U;
  unsigned vram_diff_count = 0;
  for (uint32_t i = 0; i < GENESIS_VDP_VRAM_BYTES; ++i) {
    if (baseline.vram[i] != mutated.vram[i]) {
      ++vram_diff_count;
      check(i == mutated_byte_addr, "the only differing VRAM byte is the targeted tile-pattern byte");
    }
  }
  check(vram_diff_count == 1U, "exactly one VRAM byte differs between baseline and mutated fixtures");
  check(std::memcmp(baseline.vsram, mutated.vsram, sizeof(baseline.vsram)) == 0,
        "VSRAM is byte-identical between baseline and mutated fixtures");
  check(std::memcmp(baseline.cram, mutated.cram, sizeof(baseline.cram)) == 0,
        "CRAM is byte-identical between baseline and mutated fixtures (no new color added)");
  check(std::memcmp(baseline.regs, mutated.regs, sizeof(baseline.regs)) == 0,
        "registers are identical between baseline and mutated fixtures");

  GenesisFrameArtifact mutated_frame;
  check(genesis_vdp_produce_frame(mutated.vram, mutated.vsram, mutated.cram, mutated.regs,
                                   &mutated_frame) == 0,
        "produce_frame succeeds (mutated)");
  GenesisFrameArtifact mutated_frame_again;
  check(genesis_vdp_produce_frame(mutated.vram, mutated.vsram, mutated.cram, mutated.regs,
                                   &mutated_frame_again) == 0,
        "produce_frame succeeds (mutated, second call)");

  // (1) the specific target pixel byte changes from 4 to 7.
  check(baseline_frame.pixels[kPlaneAY * GENESIS_FRAME_WIDTH + kPlaneAX] == 4U,
        "baseline Plane-A-winning pixel(0,0) decodes to CRAM index 4");
  check(mutated_frame.pixels[kPlaneAY * GENESIS_FRAME_WIDTH + kPlaneAX] ==
            expected_mutated_plane_a_index,
        "mutated Plane-A-winning pixel(0,0) decodes to CRAM index 7");

  // (2) the complete palette_snapshot array is byte-identical between
  //     baseline and mutated frames.
  check(std::memcmp(baseline_frame.palette_snapshot, mutated_frame.palette_snapshot,
                     sizeof(baseline_frame.palette_snapshot)) == 0,
        "palette_snapshot is byte-identical between baseline and mutated frames");

  // (3) the overall pixels array differs (at least the one byte).
  check(std::memcmp(mutated_frame.pixels, baseline_frame.pixels, sizeof(mutated_frame.pixels)) != 0,
        "the one-VRAM-byte mutation changes the overall pixels array");

  // (4) frame_digest differs between baseline and mutated.
  check(std::memcmp(mutated_frame.frame_digest, baseline_frame.frame_digest, 32U) != 0,
        "the one-VRAM-byte mutation changes the frame_digest");

  // (5) producing the mutated fixture twice yields the same new digest.
  check(std::memcmp(mutated_frame.frame_digest, mutated_frame_again.frame_digest, 32U) == 0,
        "determinism holds for the mutated fixture: two calls yield the same frame_digest");
}

// SEG-007-T049 Part A1: independently precomputed expected frame_digest.
//
// Fixture: all-zero VRAM/VSRAM (every nametable entry decodes to tile 0,
// whose pattern data is all-zero, i.e. palette index 0 -- transparent on
// both planes; the lone SAT entry 0 decodes to x=y=-128 (raw 0 - 128 bias),
// width/height 1 cell, link 0, so it never covers any on-screen pixel and
// SAT traversal stops immediately). With no plane or sprite opaque anywhere,
// every one of the 320*224 pixels resolves to the register #7 backdrop CRAM
// index, which is 0 here (register #7 left at its memset-zero value).
// CRAM entry 0 is set to a single non-zero color (raw word 0x02A6: R3=3,
// G3=5, B3=1) so palette_snapshot is not itself all-zero; every other CRAM
// byte (entries 1-63) stays zero.
//
// Expected digest derivation (reviewable/reproducible, NOT produced by
// calling genesis_sha256_*): the exact byte layout hashed by
// genesis_vdp_produce_frame is [full `pixels` array, row-major, 320*224 =
// 71680 bytes, every byte 0x00] followed immediately by [full
// `palette_snapshot` array, 128 bytes: byte 0 = 0x02, byte 1 = 0xA6, bytes
// 2-127 = 0x00] -- 71808 bytes total, with frame_digest itself never
// included in its own hash input. That exact 71808-byte buffer was hashed
// once, offline, outside this test process, with:
//   python3 -c "
//   import hashlib
//   data = bytes(320*224) + bytes([0x02, 0xA6]) + bytes(126)
//   print(', '.join('0x%02x' % b for b in hashlib.sha256(data).digest()))"
// which prints the 32-byte constant hard-coded below. (Cross-checked with
// `sha256sum`/`shasum -a 256` on a file containing the same 71808 bytes,
// which must print the same digest re-encoded as one hex string.)
void test_frame_digest_matches_independently_precomputed_sha256() {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  std::memset(vram, 0, sizeof(vram));
  std::memset(vsram, 0, sizeof(vsram));
  std::memset(cram, 0, sizeof(cram));
  make_default_registers(regs);
  // regs[7] left at 0 (backdrop CRAM index 0) by make_default_registers.

  write_cram_color(cram, /*entry_index=*/0, /*r3=*/3, /*g3=*/5, /*b3=*/1);

  GenesisFrameArtifact frame;
  check(genesis_vdp_produce_frame(vram, vsram, cram, regs, &frame) == 0,
        "produce_frame succeeds (all-transparent backdrop-only fixture)");

  uint8_t all_zero_pixels[GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT];
  std::memset(all_zero_pixels, 0, sizeof(all_zero_pixels));
  check(std::memcmp(frame.pixels, all_zero_pixels, sizeof(all_zero_pixels)) == 0,
        "every pixel resolves to backdrop CRAM index 0 (no plane/sprite is opaque anywhere)");

  static const uint8_t kExpectedDigest[32] = {
      0x40, 0xd3, 0xcd, 0xf2, 0xb5, 0xf9, 0x8c, 0xbc, 0xb7, 0x4e, 0xf3, 0xc0,
      0x21, 0xca, 0x7a, 0x09, 0x3d, 0xc6, 0x5e, 0x0b, 0x84, 0xed, 0x70, 0x92,
      0x0b, 0x75, 0x14, 0x70, 0x54, 0x24, 0x36, 0xd4};
  check(std::memcmp(frame.frame_digest, kExpectedDigest, 32U) == 0,
        "frame_digest matches the independently offline-precomputed SHA-256 of "
        "pixels||palette_snapshot");
}

void test_fails_closed_outside_bound_display_mode_leaves_sentinel_untouched() {
  Fixture f;
  f.regs[12] = 0x00; // Neither RS0 nor RS1: H32, outside this checkpoint's bound surface
                      // (matches the existing C2/C4 fail-closed fixture convention).

  GenesisFrameArtifact frame;
  std::memset(&frame, 0xAB, sizeof(frame)); // Sentinel-prefill.

  int result = genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, f.regs, &frame);
  check(result < 0, "produce_frame fails closed when display mode is outside the bound surface");

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

void test_rejects_null_arguments() {
  Fixture f;
  GenesisFrameArtifact frame;
  check(genesis_vdp_produce_frame(nullptr, f.vsram, f.cram, f.regs, &frame) < 0,
        "produce_frame rejects NULL vram");
  check(genesis_vdp_produce_frame(f.vram, nullptr, f.cram, f.regs, &frame) < 0,
        "produce_frame rejects NULL vsram");
  check(genesis_vdp_produce_frame(f.vram, f.vsram, nullptr, f.regs, &frame) < 0,
        "produce_frame rejects NULL cram");
  check(genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, nullptr, &frame) < 0,
        "produce_frame rejects NULL registers");
  check(genesis_vdp_produce_frame(f.vram, f.vsram, f.cram, f.regs, nullptr) < 0,
        "produce_frame rejects NULL frame_out");
}

} // namespace

int main() {
  test_deterministic_across_two_calls();
  test_schema_dimensions();
  test_known_pixels();
  test_frame_digest_matches_independently_precomputed_sha256();
  test_one_vram_byte_mutation_changes_output_deterministically();
  test_fails_closed_outside_bound_display_mode_leaves_sentinel_untouched();
  test_rejects_null_arguments();

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
