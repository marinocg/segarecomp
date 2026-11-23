// SEG-152-T001 (ADR 0309/0313/0314): the ABI 2 scenery window contract as pure code, shared by the scheduled-mod adapter (mod_capture.cpp) and the ABI 1 / ABI 2 parity harness
// (tests/abi_parity_scenery_test.cpp) so the comparison is made against the production limits and lookup, never a copy of them. No state, no host dependency, deterministic.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace segarecomp {
namespace scenery {

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

// SEG-152-T007 (ADR 0320 decision 8 addendum): the per-row generic mask of one plane. Bit `r & 7` of byte `r >> 3` (LSB first, the producer's `authored_rows` convention) marks produced SCREEN row `r` as answered by the VDP's own
// plane as a provider word (the ABI 1 `own_plane` semantics). At most 448 rows (GENESIS_FRAME_MAX_HEIGHT), 56 bytes per plane.
constexpr std::uint32_t kMaskRowsMax = 448U, kMaskBytesMax = 56U;

struct RowMask {
  bool any = false;   // at least one bit set: only such a mask counts toward the frame-served rule
  std::uint8_t bits[kMaskBytesMax] = {};
};

// The submission is in range (the adapter faults the callback with `range` otherwise): plane 0/1, non-NULL mask, 1..448 rows, `mask_bytes` exactly (rows + 7) / 8 and zero padding bits beyond `rows`.
inline bool mask_in_range(std::uint32_t plane, const std::uint8_t *mask, std::uint32_t rows, std::uint32_t mask_bytes) {
  if (plane > 1U || mask == nullptr || rows == 0U || rows > kMaskRowsMax || mask_bytes != (rows + 7U) / 8U) return false;
  if ((rows & 7U) != 0U && (mask[mask_bytes - 1U] >> (rows & 7U)) != 0U) return false;   // padding bits of the last byte must be zero
  return true;
}

// Copies a validated submission (the caller checked mask_in_range) into fixed storage; bytes beyond `mask_bytes` stay zero, so rows >= `rows` are not marked.
inline void mask_store(RowMask &out, const std::uint8_t *mask, std::uint32_t mask_bytes) {
  out = RowMask{};
  std::memcpy(out.bits, mask, mask_bytes);
  for (std::uint32_t i = 0; i < mask_bytes; ++i) out.any = out.any || mask[i] != 0U;
}

// The row test: is produced screen row `row` marked generic? One bit test; rows beyond the stored mask are not marked.
inline bool row_generic(const RowMask &m, unsigned row) { return row < kMaskRowsMax && ((m.bits[row >> 3U] >> (row & 7U)) & 1U) != 0U; }

// The whole host answer for one off-picture cell of one plane (the adapter's `scenery_word` after its frame checks, shared with the parity harness): a marked screen row answers `vdp_code` (the host-private
// GENESIS_COMPAT_WORD_VDP_START, before any window of the plane is consulted); a plane without a window answers `generic_code` (ADR 0314); otherwise the first containing window answers, or the cell fails (0).
inline int answer(const RowMask &mask, const Window (&windows)[kWindowsPerPlane], int row, int x, int hscroll, int vscroll, std::uint16_t *word, int vdp_code, int generic_code) {
  if (row >= 0 && row_generic(mask, static_cast<unsigned>(row))) return vdp_code;
  if (!windows[0].present && !windows[1].present) return generic_code;
  return lookup(windows, row, x, hscroll, vscroll, word);
}

}  // namespace scenery
}  // namespace segarecomp
