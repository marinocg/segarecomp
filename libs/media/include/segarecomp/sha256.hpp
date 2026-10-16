#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace segarecomp {

// Lowercase hexadecimal SHA-256 (FIPS 180-4) of `bytes`.
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);

// SEG-062-T002: the same FIPS 180-4 digest over data supplied in pieces (disc files are hashed while streaming).
class Sha256Stream {
 public:
  void update(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::string finish_hex();  // the stream is spent afterwards

 private:
  std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_used_{};
  std::uint64_t total_bytes_{};
};

} // namespace segarecomp
