#pragma once

// The public Kosinski LZ decoder (16-bit little-endian descriptor words consumed LSB first). Header-only, pure, bounded, allocation
// limited by the caller's `limit`. Shared by the Sega CD epoch recipe (ADR 0179) and the Genesis static Z80 producer (ADR 0277).
//
// Stream grammar (descriptor bit, then payload):
//   1                       literal: one byte
//   0 1  lo hi              "full" copy: displacement = ((hi & 0xF8) << 5 | lo) - 0x2000; count = hi & 7
//                           count != 0: copy count + 2 bytes; count == 0: one more byte `extra`: 0 = end of stream,
//                           1 = no copy (continue), otherwise copy extra + 1 bytes
//   0 0  b1 b0  disp        "inline" copy: displacement = disp - 256; count = (b1 << 1 | b0) + 2
// A copy may only reference bytes this stream already produced. A decode is not authority for anything by itself: callers prove the
// placement and the activation contract separately.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace segarecomp {

enum class KosinskiError : std::uint8_t {
  none,
  source_exhausted,         // the stream ended before its terminator
  reference_before_stream,  // a copy reaches before the first produced byte
  decoded_length_exceeded,  // the output would exceed the caller's limit
  token_bound_exceeded,     // more tokens than the bound
};

struct KosinskiResult {
  KosinskiError error = KosinskiError::none;
  std::vector<std::uint8_t> output;
  std::size_t end = 0;  // source position just after the terminator (valid when error == none)
};

inline constexpr std::uint64_t kKosinskiMaxTokens = 1U << 22;

[[nodiscard]] inline KosinskiResult kosinski_decode(std::span<const std::uint8_t> source, std::size_t position, std::size_t limit,
                                                    std::uint64_t max_tokens = kKosinskiMaxTokens) {
  KosinskiResult result;
  std::size_t pos = position;
  unsigned bits = 0, left = 0;
  const auto fail = [&](KosinskiError e) {
    result.error = e;
    return result;
  };
  const auto next_byte = [&](unsigned& out) {
    if (pos >= source.size()) return false;
    out = source[pos++];
    return true;
  };
  const auto refill = [&]() {
    unsigned lo = 0, hi = 0;
    if (!next_byte(lo) || !next_byte(hi)) return false;
    bits = lo | (hi << 8U);
    left = 16U;
    return true;
  };
  const auto bit = [&](unsigned& out) {
    out = bits & 1U;
    bits >>= 1U;
    if (--left == 0U) return refill();
    return true;
  };
  const auto copy = [&](int displacement, unsigned count) {
    const std::size_t back = static_cast<std::size_t>(-displacement);
    if (back > result.output.size()) return KosinskiError::reference_before_stream;
    if (result.output.size() + count > limit) return KosinskiError::decoded_length_exceeded;
    for (unsigned i = 0; i < count; ++i) result.output.push_back(result.output[result.output.size() - back]);
    return KosinskiError::none;
  };
  if (!refill()) return fail(KosinskiError::source_exhausted);
  for (std::uint64_t n = 0; n < max_tokens; ++n) {
    unsigned b = 0, byte = 0;
    if (!bit(b)) return fail(KosinskiError::source_exhausted);
    if (b != 0U) {
      if (result.output.size() >= limit) return fail(KosinskiError::decoded_length_exceeded);
      if (!next_byte(byte)) return fail(KosinskiError::source_exhausted);
      result.output.push_back(static_cast<std::uint8_t>(byte));
      continue;
    }
    if (!bit(b)) return fail(KosinskiError::source_exhausted);
    KosinskiError status = KosinskiError::none;
    if (b != 0U) {
      unsigned lo = 0, hi = 0;
      if (!next_byte(lo) || !next_byte(hi)) return fail(KosinskiError::source_exhausted);
      const int displacement = static_cast<int>(0xE000U | ((hi & 0xF8U) << 5U) | lo) - 0x10000;
      const unsigned count = hi & 7U;
      if (count != 0U) {
        status = copy(displacement, count + 2U);
      } else {
        unsigned extra = 0;
        if (!next_byte(extra)) return fail(KosinskiError::source_exhausted);
        if (extra == 0U) {
          result.end = pos;
          return result;
        }
        if (extra != 1U) status = copy(displacement, extra + 1U);
      }
    } else {
      unsigned hi_bit = 0, lo_bit = 0, disp = 0;
      if (!bit(hi_bit) || !bit(lo_bit)) return fail(KosinskiError::source_exhausted);
      if (!next_byte(disp)) return fail(KosinskiError::source_exhausted);
      status = copy(static_cast<int>(disp) - 256, ((hi_bit << 1U) | lo_bit) + 2U);
    }
    if (status != KosinskiError::none) return fail(status);
  }
  return fail(KosinskiError::token_bound_exceeded);
}

}  // namespace segarecomp
