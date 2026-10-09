// SEG-046 (ADR 0095): the REPORT-ONLY generic per-window feature export (platforms/genesis/machine hybrid_admission.hpp).
// Project-authored synthetic MC68000 image only. Properties: deterministic; window sizes restricted to 256/512; columns are the fixed
// generic schema; counts come from the MC68000-owned control-successor projection; the 512-byte aggregation equals the 256-byte one;
// the export never depends on any title, path or runtime data and never mutates its input.

#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "segarecomp/machine/genesis/hybrid_admission.hpp"

namespace {

using namespace segarecomp;
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

struct Report {
  std::vector<std::string> columns;
  std::map<std::uint32_t, std::vector<std::uint64_t>> rows;
};

Report parse(const std::string &text, std::uint32_t window_bytes) {
  Report report;
  std::istringstream in(text);
  std::string line;
  std::getline(in, line);
  expect(line == "segarecomp.m68k_window_features.v1 window_bytes " + std::to_string(window_bytes), "header");
  std::getline(in, line);
  std::istringstream columns(line);
  std::string token;
  columns >> token;
  expect(token == "columns", "columns line");
  while (columns >> token) report.columns.push_back(token);
  while (std::getline(in, line) && line != "end") {
    std::istringstream row(line);
    std::string start;
    row >> start;
    std::vector<std::uint64_t> values;
    std::uint64_t value;
    while (row >> value) values.push_back(value);
    expect(values.size() == report.columns.size(), "row width");
    report.rows[static_cast<std::uint32_t>(std::stoul(start, nullptr, 16)) / window_bytes] = values;
  }
  return report;
}

std::uint64_t at(const Report &report, std::uint32_t window, const std::string &column) {
  const auto row = report.rows.find(window);
  if (row == report.rows.end()) return 0U;
  for (std::size_t i = 0; i < report.columns.size(); ++i)
    if (report.columns[i] == column) return row->second[i];
  expect(false, "unknown column " + column);
  return 0U;
}

}  // namespace

int main() {
  std::vector<std::uint8_t> image(0x1000U, 0U);
  const auto words = [&image](std::uint32_t at_address, std::initializer_list<std::uint16_t> values) {
    for (const auto value : values) {
      image[at_address] = static_cast<std::uint8_t>(value >> 8U);
      image[at_address + 1U] = static_cast<std::uint8_t>(value);
      at_address += 2U;
    }
  };
  words(0x0U, {0x00FFU, 0xFE00U, 0x0000U, 0x0200U});
  words(0x78U, {0x0000U, 0x0300U});
  words(0x200U, {0x4EB9U, 0x0000U, 0x0400U});  // JSR $400 (call_direct; continuation $206)
  words(0x206U, {0x60FEU});                    // BRA self (uncond_direct, same window, terminator)
  words(0x300U, {0x4E71U, 0x60FEU});
  words(0x400U, {0x4E71U, 0x4E75U});           // NOP ; RTS (ret, terminator)
  auto program = make_genesis_bridge_startup_program(image, 0U, 0x200U, std::nullopt);
  expect(program.has_value(), "program");
  (void)apply_genesis_immutable_rom_aot(*program);
  const auto analysis = analyze_m68k_frontend(*program);
  const auto *accepted = std::get_if<FrontendAnalysis>(&analysis);
  const auto *partial = std::get_if<FrontendPartialProgram>(&analysis);
  const auto &entries = accepted != nullptr ? accepted->immutable_rom_aot_entries : partial->accepted_prefix.immutable_rom_aot_entries;
  expect(!entries.empty(), "entries");

  expect(!genesis_window_feature_report(entries, 128U).has_value(), "reject window size 128");
  expect(!genesis_window_feature_report(entries, 4096U).has_value(), "reject window size 4096");
  const auto text256 = genesis_window_feature_report(entries, 256U);
  expect(text256.has_value() && text256 == genesis_window_feature_report(entries, 256U), "deterministic");
  const auto r256 = parse(*text256, 256U);
  expect(at(r256, 2U, "call_direct") == 1U && at(r256, 2U, "call_any") == 1U, "JSR $400 is a direct call");
  expect(at(r256, 2U, "edge_far_ident") + at(r256, 2U, "edge_adjacent") == 1U, "call target in another window");
  expect(at(r256, 2U, "uncond_direct") == 1U && at(r256, 2U, "terminator") == 1U, "BRA self terminates");
  expect(at(r256, 2U, "edge_same") == 1U && at(r256, 2U, "edge_in_local") == 1U, "BRA self lands in its own window");
  expect(at(r256, 4U, "ret") == 1U && at(r256, 4U, "terminator") == 1U, "RTS is a return and a terminator");
  expect(at(r256, 4U, "edge_in_external") == 1U, "callee window sees one incoming external edge");
  expect(at(r256, 3U, "uncond_direct") == 1U, "handler BRA");
  const auto text512 = genesis_window_feature_report(entries, 512U);
  const auto r512 = parse(*text512, 512U);
  for (const char *column : {"ident", "ret", "terminator", "call_direct", "uncond_direct"}) {
    std::uint64_t sum256 = 0U, sum512 = 0U;
    for (std::uint32_t w = 0; w < 16U; ++w) sum256 += at(r256, w, column);
    for (std::uint32_t w = 0; w < 8U; ++w) sum512 += at(r512, w, column);
    expect(sum256 == sum512, std::string("512-byte aggregation equals 256-byte: ") + column);
  }
  // No address / title / ordinal columns exist in the export.
  for (const auto &column : r256.columns)
    for (const char *forbidden : {"addr", "offset", "ordinal", "title", "path", "name"})
      expect(column.find(forbidden) == std::string::npos, "forbidden column fragment in " + column);
  if (failures != 0) return 1;
  std::cout << "genesis_window_features_test: ok\n";
  return 0;
}
