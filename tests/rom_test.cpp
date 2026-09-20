#include "segarecomp/c_emitter.hpp"
#include "segarecomp/rom.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) { if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }
void write(std::vector<std::uint8_t> &bytes, std::size_t offset, std::string_view value) {
  for (std::size_t index = 0; index < value.size(); ++index) bytes[offset + index] = static_cast<std::uint8_t>(value[index]);
}
void write_be32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 24U); bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8U); bytes[offset + 3] = static_cast<std::uint8_t>(value);
}
std::string canonical_manifest_report(std::string_view id) {
  const auto manifest_path = std::filesystem::path(__FILE__).parent_path() / "fixtures/genesis-reset-image-fixtures.json";
  std::ifstream file(manifest_path);
  const std::string manifest{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  const std::string fixture_marker = "\"id\": \"" + std::string(id) + "\"";
  const auto fixture = manifest.find(fixture_marker);
  const auto expected = manifest.find("\"expected_report\": ", fixture);
  if (fixture == std::string::npos || expected == std::string::npos) return {};
  const auto begin = manifest.find('{', expected);
  std::size_t depth = 0;
  bool quoted = false;
  bool escaped = false;
  std::string canonical;
  for (std::size_t index = begin; index < manifest.size(); ++index) {
    const char character = manifest[index];
    if (quoted) {
      canonical += character;
      if (escaped) escaped = false;
      else if (character == '\\') escaped = true;
      else if (character == '"') quoted = false;
      continue;
    }
    if (character == '"') { quoted = true; canonical += character; continue; }
    if (character == '{') ++depth;
    if (character == '}') --depth;
    if (character != ' ' && character != '\n') canonical += character;
    if (depth == 0) return canonical;
  }
  return {};
}
void test_provenance_and_display() {
  std::vector<std::uint8_t> bytes(0x150);
  write(bytes, 0x100, "SEGA GENESIS    ");
  for (std::size_t index = 0; index < 48; ++index) bytes[0x120 + index] = 0x20;
  write(bytes, 0x120, "A"); bytes[0x121] = 1;
  const auto rom = segarecomp::inspect_rom(bytes);
  expect(rom.outcome == segarecomp::ClassificationOutcome::recognized, "recognizes exact padded Genesis field");
  expect(rom.domestic_title_display == "A\\x01", "uses lossless title display");
  expect(rom.candidates.size() == 1 && rom.candidates[0].offset == 0x100 &&
             rom.candidates[0].domestic_title_bytes.size() == 48, "retains Genesis field provenance");
  expect(segarecomp::emit_c_manifest(rom).find("Metadata-only") != std::string::npos, "emits metadata-only manifest");
}
void test_precedence_and_bounds() {
  std::vector<std::uint8_t> bytes(0x2000);
  write(bytes, 0x100, "SEGA MEGA DRIVE "); write(bytes, 0x120, "TITLE");
  write(bytes, 0x1ff0, "TMR SEGA"); bytes[0x1fff] = 0x20;
  const auto rom = segarecomp::inspect_rom(bytes);
  expect(rom.diagnostic == segarecomp::Diagnostic::sms_gg_region_invalid, "invalid region precedes valid header");
  std::vector<std::uint8_t> oversized(segarecomp::image_size_limit + 1);
  expect(segarecomp::inspect_rom(oversized).diagnostic == segarecomp::Diagnostic::size_limit, "size limit precedes reads");
  const std::vector<std::uint8_t> short_input(0x104);
  expect(segarecomp::inspect_rom(short_input).diagnostic == segarecomp::Diagnostic::not_recognized, "unrelated partial input has no candidate");
}
void test_reset_contract_matrix() {
  struct Fixture { std::string_view id; std::size_t size; std::optional<std::uint32_t> ssp; std::optional<std::uint32_t> pc; segarecomp::ResetDiagnostic diagnostic; };
  const std::vector<Fixture> fixtures{
    {"empty", 0, {}, {}, segarecomp::ResetDiagnostic::reset_ssp_truncated},
    {"ssp-one-byte", 1, {}, {}, segarecomp::ResetDiagnostic::reset_ssp_truncated},
    {"ssp-two-bytes", 2, {}, {}, segarecomp::ResetDiagnostic::reset_ssp_truncated},
    {"ssp-three-bytes", 3, {}, {}, segarecomp::ResetDiagnostic::reset_ssp_truncated},
    {"ssp-only", 4, 0x12345678U, {}, segarecomp::ResetDiagnostic::reset_pc_truncated},
    {"pc-one-byte", 5, 0x12345678U, {}, segarecomp::ResetDiagnostic::reset_pc_truncated},
    {"pc-two-bytes", 6, 0x12345678U, {}, segarecomp::ResetDiagnostic::reset_pc_truncated},
    {"pc-three-bytes", 7, 0x12345678U, {}, segarecomp::ResetDiagnostic::reset_pc_truncated},
    {"valid-be32-entry", 8, 0x00ff0000U, 6U, segarecomp::ResetDiagnostic::reset_image_accepted},
    {"pc-high-byte", 8, 0U, 0x01000000U, segarecomp::ResetDiagnostic::reset_pc_not_24bit},
    {"pc-high-byte-and-odd", 8, 0U, 0x01000001U, segarecomp::ResetDiagnostic::reset_pc_not_24bit},
    {"pc-odd", 8, 0U, 7U, segarecomp::ResetDiagnostic::reset_pc_odd},
    {"pc-odd-unmapped", 8, 0U, 9U, segarecomp::ResetDiagnostic::reset_pc_odd},
    {"pc-at-image-end", 8, 0U, 8U, segarecomp::ResetDiagnostic::reset_pc_unmapped},
    {"pc-even-unmapped", 8, 0U, 10U, segarecomp::ResetDiagnostic::reset_pc_unmapped},
    {"four-mebibyte-last-even", 0x400000, 0U, 0x003ffffeU, segarecomp::ResetDiagnostic::reset_image_accepted},
    {"four-mebibyte-window-end", 0x400000, 0U, 0x00400000U, segarecomp::ResetDiagnostic::reset_pc_unmapped},
    {"over-limit", 0x400001, 0U, 0U, segarecomp::ResetDiagnostic::image_size_limit},
    {"over-limit-unexamined-vectors", 0x400001, {}, {}, segarecomp::ResetDiagnostic::image_size_limit},
  };
  for (const auto &fixture : fixtures) {
    std::vector<std::uint8_t> bytes(fixture.size);
    if (fixture.id == "ssp-one-byte") bytes[0] = 0x12;
    if (fixture.id == "ssp-two-bytes") { bytes[0] = 0x12; bytes[1] = 0x34; }
    if (fixture.id == "ssp-three-bytes") { bytes[0] = 0x12; bytes[1] = 0x34; bytes[2] = 0x56; }
    if (fixture.ssp) write_be32(bytes, 0, *fixture.ssp);
    if (fixture.pc && fixture.size >= 8) write_be32(bytes, 4, *fixture.pc);
    const auto report = segarecomp::analyze_genesis_reset_image(bytes);
    expect(report.diagnostic == fixture.diagnostic, fixture.id);
    const auto expected_report = canonical_manifest_report(fixture.id);
    expect(!expected_report.empty(), "fixture has complete expected report");
    expect(segarecomp::format_genesis_reset_image_report(report) == expected_report, "reset report matches manifest expectation");
    if (fixture.diagnostic == segarecomp::ResetDiagnostic::image_size_limit) {
      expect(!report.raw_region_constructed && report.ssp.status == segarecomp::ResetRangeStatus::unexamined, "size limit leaves vectors unexamined");
    }
  }
  std::vector<std::uint8_t> valid(8); write_be32(valid, 0, 0x00ff0000U); write_be32(valid, 4, 6U);
  const auto report = segarecomp::analyze_genesis_reset_image(valid);
  expect(report.initial_ssp && report.initial_ssp->value == 0x00ff0000U && report.entry_image_offset && report.entry_image_offset->value == 6, "BE32 and entry provenance are exact");
}
} // namespace
int main() { test_provenance_and_display(); test_precedence_and_bounds(); test_reset_contract_matrix(); return failures == 0 ? 0 : 1; }
