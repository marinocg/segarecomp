// SEG-152-T001 (ADR 0309/0313/0314): the ABI 2 scenery window contract as pure code, shared by the scheduled-mod adapter (mod_capture.cpp) and the ABI 1 / ABI 2 parity harness
// (tests/abi_parity_scenery_test.cpp) so the comparison is made against the production limits and lookup, never a copy of them. No state, no host dependency, deterministic.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace segarecomp::scenery {

constexpr std::uint32_t kColsMax = 128U, kRowsMax = 64U, kWordsMax = 4096U, kWindowsPerPlane = 2U;   // ADR 0309 / ADR 0313
constexpr std::int32_t kOriginMax = 1 << 20;

struct Window {
  bool present = false;
  std::int32_t x0 = 0, y0 = 0;
  std::uint32_t cols = 0, rows = 0;
  std::vector<std::uint16_t> words;
};

// A submission is in range (the adapter faults the callback with `range` otherwise). `plane_full`: the plane already holds two windows (a third is refused).
inline bool submission_in_range(std::uint32_t plane, std::int32_t x0, std::int32_t y0, std::uint32_t cols, std::uint32_t rows, const std::uint16_t *words, bool plane_full) {
  return !(plane > 1U || words == nullptr || cols == 0U || rows == 0U || cols > kColsMax || rows > kRowsMax || cols * rows > kWordsMax || plane_full || x0 < -kOriginMax || x0 > kOriginMax ||
           y0 < -kOriginMax || y0 > kOriginMax);
}

// The host's answer for one off-picture cell: cell = floor((x - hscroll) / 8), floor((row + vscroll) / 8); the FIRST window of the plane that contains it answers (ADR 0313). Returns 1 with
// the word, or 0 when no window contains the cell (a failed answer, never a guess).
inline int lookup(const Window (&windows)[kWindowsPerPlane], int row, int x, int hscroll, int vscroll, std::uint16_t *word) {
  const auto cell = [](long long pixel) { return static_cast<long long>(pixel >= 0 ? pixel / 8 : -((-pixel + 7) / 8)); };   // floor division by 8
  const long long wx = cell(static_cast<long long>(x) - hscroll), wy = cell(static_cast<long long>(row) + vscroll);
  for (const Window &w : windows) {
    if (!w.present) continue;
    const long long cx = wx - w.x0, cy = wy - w.y0;
    if (cx < 0 || cy < 0 || cx >= static_cast<long long>(w.cols) || cy >= static_cast<long long>(w.rows)) continue;
    *word = w.words[static_cast<std::size_t>(cy) * w.cols + static_cast<std::size_t>(cx)];
    return 1;
  }
  return 0;
}

}  // namespace segarecomp::scenery
