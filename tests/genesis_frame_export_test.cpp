// SEG-007-T050 add-on: focused unit coverage for
// platforms/genesis/runtime/frame_export.c's operator-facing local frame-export
// module (genesis_frame_export_ppm_to_stream / genesis_frame_export_ppm_to_
// path). All fixtures are project-authored synthetic pixel/palette/register
// data with independently hand-derived expected results -- no commercial
// ROM content, pixel data, or palette is read or referenced anywhere in
// this file.
//
// This module is a pure PPM (P6) serializer over an already-produced
// GenesisFrameArtifact; it performs no rendering of its own. These tests
// therefore construct GenesisFrameArtifact values directly (never by
// calling genesis_vdp_produce_frame) and hand-derive expected RGB triplets
// from vdp_render.h's own documented, cited CRAM bit layout and 3-bit-to-
// 8-bit channel scaling (value * 255 / 7), the same formula
// genesis_vdp_decode_cram_entry implements -- reused here only to compute
// the *expected* value from first principles, never to derive the
// expectation by calling the function under test.
//
// `frame_digest` values used below to mark an artifact "populated" are
// arbitrary non-zero byte patterns, not real SHA-256 digests of the
// fixture's own pixel/palette content. genesis_frame_export_ppm_to_stream's
// only use of frame_digest is the existing "is this artifact the
// never-rendered {0} sentinel" presence check (genesis_frame_artifact_is_
// populated, runtime.h); it never recomputes or verifies a digest against
// content, so this is a correct, non-tautological way to exercise both the
// populated and not-populated paths.

#include "frame_export.h"
#include "vdp_render.h" // GENESIS_VDP_CRAM_ENTRY_COUNT (boundary/adversarial fixtures only)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "host_io.hpp"
#include <filesystem>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// Fills `frame` with all-zero pixels/palette and a zero digest -- exactly
// SEG-007-T131's original never-rendered sentinel value.
void make_unpopulated_artifact(GenesisFrameArtifact *frame) { std::memset(frame, 0, sizeof(*frame)); }

// Fills `frame` with all-zero pixels/palette but a non-zero, arbitrary
// digest pattern, marking it "populated" per genesis_frame_artifact_is_
// populated's own documented all-zero-digest sentinel rule.
void make_populated_artifact(GenesisFrameArtifact *frame) {
  std::memset(frame, 0, sizeof(*frame));
  for (size_t i = 0; i < sizeof(frame->frame_digest); ++i) {
    frame->frame_digest[i] = (uint8_t)(0xAB ^ i);
  }
}

// Writes a raw big-endian CRAM halfword for CRAM entry `entry_index` (0-63)
// into `cram` (GENESIS_VDP_CRAM_BYTES bytes), per GenesisVdpState.cram's own
// documented big-endian-halfword layout (vdp_render.h).
void write_cram_entry(uint8_t cram[GENESIS_VDP_CRAM_BYTES], unsigned entry_index, uint16_t raw_word) {
  unsigned offset = entry_index * 2U;
  cram[offset] = (uint8_t)((raw_word >> 8) & 0xFFU);
  cram[offset + 1U] = (uint8_t)(raw_word & 0xFFU);
}

// Hand-derives the expected GenesisRgb888 for a raw CRAM word, straight
// from vdp_render.h's own documented, cited bit layout
// (`----BBB-GGG-RRR-`) and channel-scaling policy (value * 255 / 7) -- an
// independent re-derivation, not a call into the function under test or
// into genesis_vdp_decode_cram_entry/genesis_vdp_decode_cram_word.
void expected_rgb_for_raw_word(uint16_t raw_word, uint8_t *r, uint8_t *g, uint8_t *b) {
  unsigned red3 = (raw_word >> 1) & 0x07U;
  unsigned green3 = (raw_word >> 5) & 0x07U;
  unsigned blue3 = (raw_word >> 9) & 0x07U;
  *r = (uint8_t)(red3 * 255U / 7U);
  *g = (uint8_t)(green3 * 255U / 7U);
  *b = (uint8_t)(blue3 * 255U / 7U);
}

std::vector<uint8_t> read_entire_stream(FILE *stream) {
  std::vector<uint8_t> data;
  std::rewind(stream);
  long size;
  if (std::fseek(stream, 0, SEEK_END) != 0) return data;
  size = std::ftell(stream);
  if (size < 0) return data;
  std::rewind(stream);
  data.resize((size_t)size);
  if (!data.empty()) {
    size_t got = std::fread(data.data(), 1, data.size(), stream);
    data.resize(got);
  }
  return data;
}

const char kExpectedHeader[] = "P6\n320 224\n255\n";
const size_t kExpectedHeaderLength = sizeof(kExpectedHeader) - 1U; // exclude trailing NUL
const size_t kExpectedPixelBytes =
    (size_t)GENESIS_FRAME_WIDTH * (size_t)GENESIS_FRAME_HEIGHT * 3U;

// 1. NULL frame rejected without touching the stream.
void test_null_frame_rejected() {
  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_null_frame_rejected: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(NULL, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT,
        "test_null_frame_rejected: status");
  std::vector<uint8_t> data = read_entire_stream(stream);
  check(data.empty(), "test_null_frame_rejected: no bytes written");
  std::fclose(stream);
}

// 2. NULL stream rejected.
void test_null_stream_rejected() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, NULL);
  check(status == GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT,
        "test_null_stream_rejected: status");
}

// 3. Never-rendered ({0}) artifact is rejected as not-populated, distinct
// from an invalid argument, and no bytes are written.
void test_unpopulated_artifact_rejected() {
  GenesisFrameArtifact frame;
  make_unpopulated_artifact(&frame);
  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_unpopulated_artifact_rejected: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_ARTIFACT_NOT_POPULATED,
        "test_unpopulated_artifact_rejected: status");
  std::vector<uint8_t> data = read_entire_stream(stream);
  check(data.empty(), "test_unpopulated_artifact_rejected: no bytes written");
  std::fclose(stream);
}

// 4. A valid populated artifact (all pixels index 0, CRAM entry 0 left at
// raw 0x0000) exports OK with exactly the expected P6 header and total
// byte count (header + width*height*3 pixel bytes), the exact deterministic
// 320x224 P6 PPM schema this module documents.
void test_valid_export_header_and_size() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  // pixels already all zero (CRAM entry 0); palette_snapshot already all
  // zero (entry 0 raw word 0x0000 -> black).
  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_valid_export_header_and_size: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_OK, "test_valid_export_header_and_size: status");
  std::vector<uint8_t> data = read_entire_stream(stream);
  check(data.size() == kExpectedHeaderLength + kExpectedPixelBytes,
        "test_valid_export_header_and_size: total size");
  check(data.size() >= kExpectedHeaderLength &&
            std::memcmp(data.data(), kExpectedHeader, kExpectedHeaderLength) == 0,
        "test_valid_export_header_and_size: header bytes");
  // All-black pixel payload: every triplet must be (0, 0, 0).
  bool all_black = true;
  for (size_t i = kExpectedHeaderLength; i < data.size(); ++i) {
    if (data[i] != 0) {
      all_black = false;
      break;
    }
  }
  check(all_black, "test_valid_export_header_and_size: all-black payload");
  std::fclose(stream);
}

// 5. A single hand-derived palette entry (pure red, CRAM entry 5) decodes
// to the exact hand-computed RGB triplet at the one pixel using it.
void test_pixel_color_mapping_hand_derived() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  const unsigned kEntry = 5U;
  const uint16_t kRawWord = 0x000EU; // red3=7, green3=0, blue3=0.
  write_cram_entry(frame.palette_snapshot, kEntry, kRawWord);
  frame.pixels[0] = (uint8_t)kEntry; // top-left pixel only.

  uint8_t expected_r, expected_g, expected_b;
  expected_rgb_for_raw_word(kRawWord, &expected_r, &expected_g, &expected_b);
  check(expected_r == 255 && expected_g == 0 && expected_b == 0,
        "test_pixel_color_mapping_hand_derived: hand-derived expectation sanity");

  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_pixel_color_mapping_hand_derived: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_OK, "test_pixel_color_mapping_hand_derived: status");
  std::vector<uint8_t> data = read_entire_stream(stream);
  check(data.size() == kExpectedHeaderLength + kExpectedPixelBytes,
        "test_pixel_color_mapping_hand_derived: total size");
  size_t first_pixel_offset = kExpectedHeaderLength;
  check(data.size() >= first_pixel_offset + 3 && data[first_pixel_offset] == expected_r &&
            data[first_pixel_offset + 1] == expected_g && data[first_pixel_offset + 2] == expected_b,
        "test_pixel_color_mapping_hand_derived: first pixel RGB");
  std::fclose(stream);
}

// 6. Multiple distinct palette entries at distinct pixel positions decode
// to distinct, correctly-ordered (row-major) RGB triplets -- proves
// positional correctness, not just single-pixel correctness.
void test_positional_correctness_multiple_entries() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  const uint16_t kRedWord = 0x000EU;   // entry 5: pure red.
  const uint16_t kGreenWord = 0x00E0U; // entry 10: pure green.
  const uint16_t kBlueWord = 0x0E00U;  // entry 20: pure blue.
  write_cram_entry(frame.palette_snapshot, 5U, kRedWord);
  write_cram_entry(frame.palette_snapshot, 10U, kGreenWord);
  write_cram_entry(frame.palette_snapshot, 20U, kBlueWord);
  frame.pixels[0] = 5U;                          // (row 0, col 0)
  frame.pixels[1] = 10U;                         // (row 0, col 1)
  frame.pixels[GENESIS_FRAME_WIDTH] = 20U;       // (row 1, col 0)

  uint8_t red_r, red_g, red_b, green_r, green_g, green_b, blue_r, blue_g, blue_b;
  expected_rgb_for_raw_word(kRedWord, &red_r, &red_g, &red_b);
  expected_rgb_for_raw_word(kGreenWord, &green_r, &green_g, &green_b);
  expected_rgb_for_raw_word(kBlueWord, &blue_r, &blue_g, &blue_b);

  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_positional_correctness_multiple_entries: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_OK,
        "test_positional_correctness_multiple_entries: status");
  std::vector<uint8_t> data = read_entire_stream(stream);
  size_t base = kExpectedHeaderLength;
  size_t pixel0 = base + 0U * 3U;
  size_t pixel1 = base + 1U * 3U;
  size_t pixel_row1_col0 = base + (size_t)GENESIS_FRAME_WIDTH * 3U;
  check(data.size() == kExpectedHeaderLength + kExpectedPixelBytes,
        "test_positional_correctness_multiple_entries: total size");
  check(data.size() > pixel_row1_col0 + 2 && data[pixel0] == red_r && data[pixel0 + 1] == red_g &&
            data[pixel0 + 2] == red_b,
        "test_positional_correctness_multiple_entries: (0,0) red");
  check(data[pixel1] == green_r && data[pixel1 + 1] == green_g && data[pixel1 + 2] == green_b,
        "test_positional_correctness_multiple_entries: (0,1) green");
  check(data[pixel_row1_col0] == blue_r && data[pixel_row1_col0 + 1] == blue_g &&
            data[pixel_row1_col0 + 2] == blue_b,
        "test_positional_correctness_multiple_entries: (1,0) blue");
  std::fclose(stream);
}

// 7. Boundary: the maximum valid palette index (GENESIS_VDP_CRAM_ENTRY_COUNT
// - 1 == 63) exports successfully.
void test_boundary_max_valid_palette_index() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  frame.pixels[0] = (uint8_t)(GENESIS_VDP_CRAM_ENTRY_COUNT - 1U);
  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_boundary_max_valid_palette_index: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_OK, "test_boundary_max_valid_palette_index: status");
  std::fclose(stream);
}

// 8. Adversarial boundary: exactly one-past-the-maximum valid palette index
// (GENESIS_VDP_CRAM_ENTRY_COUNT == 64) is rejected as an invalid palette
// index, distinct from an I/O failure.
void test_invalid_palette_index_just_over_boundary() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  frame.pixels[0] = (uint8_t)GENESIS_VDP_CRAM_ENTRY_COUNT;
  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_invalid_palette_index_just_over_boundary: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX,
        "test_invalid_palette_index_just_over_boundary: status");
  std::fclose(stream);
}

// 9. Adversarial: the maximum possible byte value (255), far out of range,
// is also rejected as an invalid palette index (not a crash / OOB read).
void test_invalid_palette_index_max_byte_value() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  frame.pixels[GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT - 1U] = 255U; // last pixel.
  FILE *stream = std::tmpfile();
  check(stream != NULL, "test_invalid_palette_index_max_byte_value: tmpfile");
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_stream(&frame, stream);
  check(status == GENESIS_FRAME_EXPORT_STATUS_INVALID_PALETTE_INDEX,
        "test_invalid_palette_index_max_byte_value: status");
  std::fclose(stream);
}

// 10. Determinism: exporting the exact same artifact twice (to two separate
// streams) produces byte-for-byte identical output.
void test_deterministic_repeated_export() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  write_cram_entry(frame.palette_snapshot, 5U, 0x000EU);
  for (size_t i = 0; i < sizeof(frame.pixels); ++i) {
    frame.pixels[i] = (uint8_t)(i % GENESIS_VDP_CRAM_ENTRY_COUNT);
  }
  for (unsigned e = 0; e < GENESIS_VDP_CRAM_ENTRY_COUNT; ++e) {
    write_cram_entry(frame.palette_snapshot, e, (uint16_t)(e * 7U));
  }

  FILE *stream_a = std::tmpfile();
  FILE *stream_b = std::tmpfile();
  check(stream_a != NULL && stream_b != NULL, "test_deterministic_repeated_export: tmpfile");
  GenesisFrameExportStatus status_a = genesis_frame_export_ppm_to_stream(&frame, stream_a);
  GenesisFrameExportStatus status_b = genesis_frame_export_ppm_to_stream(&frame, stream_b);
  check(status_a == GENESIS_FRAME_EXPORT_STATUS_OK && status_b == GENESIS_FRAME_EXPORT_STATUS_OK,
        "test_deterministic_repeated_export: status");
  std::vector<uint8_t> data_a = read_entire_stream(stream_a);
  std::vector<uint8_t> data_b = read_entire_stream(stream_b);
  check(data_a.size() == data_b.size() && data_a == data_b,
        "test_deterministic_repeated_export: byte-identical output");
  std::fclose(stream_a);
  std::fclose(stream_b);
}

// 11. Path-based wrapper round-trip: a real temporary file on disk receives
// byte-for-byte the same content the stream-based entry point produces.
void test_path_export_round_trip() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  write_cram_entry(frame.palette_snapshot, 5U, 0x000EU);
  frame.pixels[0] = 5U;

  // genesis_frame_export_ppm_to_path reopens/truncates the path itself.
  const std::string path_storage = host_io::make_temp_file("segarecomp_frame_export_test");
  check(!path_storage.empty(), "test_path_export_round_trip: temp file");
  const char *path_template = path_storage.c_str();

  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_path(&frame, path_template);
  check(status == GENESIS_FRAME_EXPORT_STATUS_OK, "test_path_export_round_trip: status");

  FILE *readback = std::fopen(path_template, "rb");
  check(readback != NULL, "test_path_export_round_trip: reopen for readback");
  if (readback != NULL) {
    std::vector<uint8_t> data = read_entire_stream(readback);
    check(data.size() == kExpectedHeaderLength + kExpectedPixelBytes,
          "test_path_export_round_trip: total size");
    check(data.size() >= kExpectedHeaderLength &&
              std::memcmp(data.data(), kExpectedHeader, kExpectedHeaderLength) == 0,
          "test_path_export_round_trip: header bytes");
    std::fclose(readback);
  }
  std::remove(path_template);
}

// 12. Path-based wrapper classifies an unwritable destination (a directory
// path unusable as `fopen(..., "wb")`'s target) as an I/O error, distinct
// from an artifact-validity failure -- the artifact itself remains valid.
void test_path_export_invalid_path_io_error() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  // A directory can never be fopen'd for writing; this is a portable,
  // deterministic way to force an I/O failure without depending on
  // filesystem permission semantics.
  GenesisFrameExportStatus status = genesis_frame_export_ppm_to_path(&frame, std::filesystem::temp_directory_path().string().c_str());
  check(status == GENESIS_FRAME_EXPORT_STATUS_IO_ERROR,
        "test_path_export_invalid_path_io_error: status");
}

// 13. Path-based wrapper rejects NULL frame, NULL path, and an empty path
// string as invalid arguments (not I/O errors).
void test_path_export_null_and_empty_argument_rejected() {
  GenesisFrameArtifact frame;
  make_populated_artifact(&frame);
  check(genesis_frame_export_ppm_to_path(NULL, "/tmp/unused-path") ==
            GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT,
        "test_path_export_null_and_empty_argument_rejected: NULL frame");
  check(genesis_frame_export_ppm_to_path(&frame, NULL) == GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT,
        "test_path_export_null_and_empty_argument_rejected: NULL path");
  check(genesis_frame_export_ppm_to_path(&frame, "") == GENESIS_FRAME_EXPORT_STATUS_INVALID_ARGUMENT,
        "test_path_export_null_and_empty_argument_rejected: empty path");
}

} // namespace

int main() {
  test_null_frame_rejected();
  test_null_stream_rejected();
  test_unpopulated_artifact_rejected();
  test_valid_export_header_and_size();
  test_pixel_color_mapping_hand_derived();
  test_positional_correctness_multiple_entries();
  test_boundary_max_valid_palette_index();
  test_invalid_palette_index_just_over_boundary();
  test_invalid_palette_index_max_byte_value();
  test_deterministic_repeated_export();
  test_path_export_round_trip();
  test_path_export_invalid_path_io_error();
  test_path_export_null_and_empty_argument_rejected();

  if (failures == 0) {
    std::printf("OK: all genesis_frame_export tests passed\n");
    return 0;
  }
  std::printf("FAILED: %d genesis_frame_export test(s)\n", failures);
  return 1;
}
