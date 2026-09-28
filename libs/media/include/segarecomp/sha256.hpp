#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace segarecomp {

// Lowercase hexadecimal SHA-256 (FIPS 180-4) of `bytes`.
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);

} // namespace segarecomp
