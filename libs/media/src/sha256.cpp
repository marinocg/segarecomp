#include "segarecomp/sha256.hpp"

#include <array>
#include <vector>

namespace segarecomp {
namespace {
constexpr std::uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// Plain arrays and a macro rotate: this hashes whole generated-source trees on every Z80 materialization iteration, and the unoptimized
// (Debug) builds the test suite uses pay a call per std::array::operator[] and per rotate. The arithmetic is the FIPS 180-4 one.
#define SEGARECOMP_ROTR(value, amount) (((value) >> (amount)) | ((value) << (32U - (amount))))

void compress(std::array<std::uint32_t, 8> &state, const std::uint8_t *block) {
  std::uint32_t w[64];
  std::uint32_t v[8];
  for (std::size_t i = 0; i < 16; ++i)
    w[i] = (std::uint32_t{block[i * 4]} << 24U) | (std::uint32_t{block[i * 4 + 1]} << 16U) |
           (std::uint32_t{block[i * 4 + 2]} << 8U) | std::uint32_t{block[i * 4 + 3]};
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = SEGARECOMP_ROTR(w[i - 15], 7U) ^ SEGARECOMP_ROTR(w[i - 15], 18U) ^ (w[i - 15] >> 3U);
    const std::uint32_t s1 = SEGARECOMP_ROTR(w[i - 2], 17U) ^ SEGARECOMP_ROTR(w[i - 2], 19U) ^ (w[i - 2] >> 10U);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  for (std::size_t i = 0; i < 8; ++i) v[i] = state[i];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = SEGARECOMP_ROTR(v[4], 6U) ^ SEGARECOMP_ROTR(v[4], 11U) ^ SEGARECOMP_ROTR(v[4], 25U);
    const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
    const std::uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
    const std::uint32_t s0 = SEGARECOMP_ROTR(v[0], 2U) ^ SEGARECOMP_ROTR(v[0], 13U) ^ SEGARECOMP_ROTR(v[0], 22U);
    const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    const std::uint32_t t2 = s0 + maj;
    v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
    v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
  }
  for (std::size_t i = 0; i < 8; ++i) state[i] += v[i];
}
#undef SEGARECOMP_ROTR
} // namespace

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const std::size_t full = bytes.size() / 64U;
  for (std::size_t i = 0; i < full; ++i) compress(state, bytes.data() + i * 64U);
  std::vector<std::uint8_t> tail(bytes.begin() + static_cast<std::ptrdiff_t>(full * 64U), bytes.end());
  tail.push_back(0x80U);
  while (tail.size() % 64U != 56U) tail.push_back(0U);
  const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8U;
  for (int shift = 56; shift >= 0; shift -= 8) tail.push_back(static_cast<std::uint8_t>(bits >> shift));
  for (std::size_t offset = 0; offset < tail.size(); offset += 64U) compress(state, tail.data() + offset);
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const auto word : state)
    for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(word >> shift) & 0xFU]);
  return out;
}

} // namespace segarecomp
