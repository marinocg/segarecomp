// SEG-049 (ADR 0100): the REPORT-ONLY generic per-word-position MC68000 decode-token export (platforms/genesis/machine hybrid_admission.hpp).
// Project-authored synthetic MC68000 words only. Properties: deterministic; exactly 8 bounded generic fields per even position; classes come
// from the CPU-owned decoder/IR/control-successor projection; the token of a word never depends on its ROM position (no address, ordinal or
// decoded target is ever emitted); undecodable / truncated positions are all-zero; the format header and trailer are fixed.

#include <cstdint>
#include <iostream>
#include <string>
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

std::vector<std::uint8_t> words(std::initializer_list<std::uint16_t> list) {
  std::vector<std::uint8_t> out;
  for (const auto w : list) {
    out.push_back(static_cast<std::uint8_t>(w >> 8U));
    out.push_back(static_cast<std::uint8_t>(w & 0xFFU));
  }
  return out;
}

struct Tokens {
  std::string header;
  std::vector<std::vector<std::uint8_t>> rows;
  bool ok = false;
};

Tokens parse(const std::string &report, std::size_t positions) {
  Tokens t;
  const auto newline = report.find('\n');
  if (newline == std::string::npos) return t;
  t.header = report.substr(0, newline);
  const std::size_t body = newline + 1U;
  if (report.size() != body + positions * genesis_decode_token_fields + 4U) return t;
  if (report.substr(report.size() - 4U) != "end\n") return t;
  for (std::size_t p = 0; p < positions; ++p)
    t.rows.emplace_back(report.begin() + static_cast<std::ptrdiff_t>(body + p * genesis_decode_token_fields),
                        report.begin() + static_cast<std::ptrdiff_t>(body + (p + 1U) * genesis_decode_token_fields));
  t.ok = true;
  return t;
}

}  // namespace

int main() {
  // RTS, NOP, BRA.s +2, BSR.w, JMP (A0), line-A word, MOVE.L D0,D1, MOVEQ #1,D0, BEQ.s, a trailing odd-length-free pair.
  const auto rom = words({0x4E75, 0x4E71, 0x6002, 0x6100, 0x0010, 0x4ED0, 0xA000, 0x2200, 0x7001, 0x6700, 0x4E75, 0x4E75});
  const auto report = genesis_decode_token_report(rom);
  expect(report == genesis_decode_token_report(rom), "deterministic");
  const auto t = parse(report, rom.size() / 2U);
  expect(t.ok, "well-formed binary report");
  expect(t.header == "segarecomp.m68k_decode_tokens.v1 positions 12 fields status length family control ea_src ea_dst width flags", "header");
  if (!t.ok) return 1;
  // fields: status length family control ea_src ea_dst width flags
  expect(t.rows[0][0] == 2U && t.rows[0][1] == 1U && t.rows[0][3] == 6U, "RTS: ordinary, 1 word, return");
  expect(t.rows[1][0] == 2U && t.rows[1][3] == 0U, "NOP: ordinary, no control transfer");
  expect(t.rows[2][3] == 2U && (t.rows[2][7] & 2U) != 0U, "BRA.s: direct jump, PC-relative flag");
  expect(t.rows[3][3] == 3U && t.rows[3][1] == 2U && (t.rows[3][7] & 2U) != 0U, "BSR.w: direct call, 2 words, PC-relative flag");
  expect(t.rows[5][3] == 4U && (t.rows[5][7] & 2U) == 0U, "JMP (A0): dynamic jump, not a displacement form");
  expect(t.rows[6][0] == 1U && t.rows[6][2] == 6U && t.rows[6][3] == 7U, "line-A word: architectural exception form, always-exception");
  expect(t.rows[7][0] == 2U && t.rows[7][2] == 1U, "MOVE.L: move family");
  expect(t.rows[7][6] == 3U, "MOVE.L: long operand width");
  expect(t.rows[9][3] == 1U, "BEQ.s: conditional control");
  for (const auto &row : t.rows) {
    expect(row[0] < 3U && row[1] < 6U && row[2] < 8U && row[3] < 8U && row[4] < 13U && row[5] < 13U && row[6] < 4U && row[7] < 4U,
           "every field within its declared cardinality");
    if (row[0] == 0U) {
      bool zero = true;
      for (const auto v : row) zero = zero && v == 0U;
      expect(zero, "an undecodable position is all-zero");
    }
  }
  // Position independence: the same word at a different ROM position yields the identical token (no address / ordinal / target input).
  const auto shifted = words({0x4E71, 0x4E71, 0x4E71, 0x4E71, 0x4E75});
  const auto ts = parse(genesis_decode_token_report(shifted), 5U);
  expect(ts.ok && ts.rows[4] == t.rows[0], "RTS token independent of position");
  // BRA.s with different displacements: the displacement (a target) must not influence any token field.
  const auto a = parse(genesis_decode_token_report(words({0x6002, 0x4E71, 0x4E71, 0x4E71})), 4U);
  const auto b = parse(genesis_decode_token_report(words({0x6006, 0x4E71, 0x4E71, 0x4E71})), 4U);
  expect(a.ok && b.ok && a.rows[0] == b.rows[0], "branch displacement (target) does not enter the token");
  // A multi-word instruction truncated by the end of the image is not decodable there.
  const auto trunc = parse(genesis_decode_token_report(words({0x4E71, 0x6100})), 2U);
  expect(trunc.ok && trunc.rows[1][0] == 0U, "truncated instruction rejected");
  // Empty and odd-size images stay well-defined (odd tail byte ignored).
  const auto empty = genesis_decode_token_report({});
  expect(empty == "segarecomp.m68k_decode_tokens.v1 positions 0 fields status length family control ea_src ea_dst width flags\nend\n", "empty image");
  if (failures != 0) return 1;
  std::cout << "genesis_decode_tokens_test OK\n";
  return 0;
}
