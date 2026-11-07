// The shared public Kosinski decoder (libs/core kosinski.hpp): streams produced by the project-authored encoder of
// tests/sega_cd_epoch_common.py, and every typed failure.
#include <array>
#include <cstdio>
#include <vector>

#include "segarecomp/core/kosinski.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label);
  if (!ok) ++failures;
}

constexpr std::array<std::uint8_t, 46> kData1 = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
                                                  0x12, 0x13, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07};
constexpr std::array<std::uint8_t, 32> kStream1 = {0xFF, 0xFE, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xF8, 0xF8, 0x0F, 0x0A, 0x0B, 0x0C,
                                                   0x0D, 0x0E, 0x5F, 0x01, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x07, 0xFF, 0xF8, 0x0A, 0xF0, 0xF0, 0x00};
constexpr std::array<std::uint8_t, 7> kStream2 = {0x0B, 0x00, 0x61, 0x62, 0xF0, 0xF0, 0x00};
}  // namespace

int main() {
  using segarecomp::KosinskiError;
  using segarecomp::kosinski_decode;

  const auto a = kosinski_decode(kStream1, 0, 4096);
  check(a.error == KosinskiError::none && a.output.size() == kData1.size() && std::equal(a.output.begin(), a.output.end(), kData1.begin()),
        "a stream with literals, inline and long copies decodes to the original bytes");
  check(a.end == kStream1.size(), "the end position is just past the terminator");

  const auto b = kosinski_decode(kStream2, 0, 4096);
  check(b.error == KosinskiError::none && b.output == std::vector<std::uint8_t>({0x61, 0x62}) && b.end == kStream2.size(), "a two-literal stream");

  // decoding from a position inside a larger buffer (the Genesis loader's stream address)
  std::vector<std::uint8_t> padded(5, 0xEE);
  padded.insert(padded.end(), kStream2.begin(), kStream2.end());
  padded.push_back(0x99);
  const auto c = kosinski_decode(padded, 5, 16);
  check(c.error == KosinskiError::none && c.output.size() == 2 && c.end == 5 + kStream2.size(), "a stream at an offset; the trailing byte is not consumed");

  // typed failures
  check(kosinski_decode(std::span<const std::uint8_t>(kStream1.data(), 20), 0, 4096).error == KosinskiError::source_exhausted, "a truncated stream is source_exhausted");
  check(kosinski_decode(kStream1, 100, 4096).error == KosinskiError::source_exhausted, "a position past the buffer is source_exhausted");
  check(kosinski_decode(kStream1, 0, 20).error == KosinskiError::decoded_length_exceeded, "the output limit is enforced");
  check(kosinski_decode(kStream1, 0, 1).error == KosinskiError::decoded_length_exceeded, "a one-byte limit stops at the second byte");
  const std::array<std::uint8_t, 4> before = {0x00, 0x00, 0xFF, 0xF8};  // first token: inline copy of 2 bytes from 256 back, nothing produced yet
  check(kosinski_decode(before, 0, 4096).error == KosinskiError::reference_before_stream, "a copy before the first produced byte is refused");
  check(kosinski_decode(kStream1, 0, 4096, 3).error == KosinskiError::token_bound_exceeded, "the token bound is enforced");
  check(kosinski_decode(std::span<const std::uint8_t>{}, 0, 16).error == KosinskiError::source_exhausted, "an empty buffer is source_exhausted");

  return failures == 0 ? 0 : 1;
}
