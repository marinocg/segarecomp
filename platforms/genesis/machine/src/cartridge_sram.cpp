#include "segarecomp/machine/genesis/cartridge_sram.hpp"

namespace segarecomp {

namespace {
constexpr std::size_t header_offset = 0x1B0U;
constexpr std::size_t header_size = 12U;

std::uint32_t be32(std::span<const std::uint8_t> image, std::size_t offset) noexcept {
  return (static_cast<std::uint32_t>(image[offset]) << 24U) | (static_cast<std::uint32_t>(image[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(image[offset + 2U]) << 8U) | static_cast<std::uint32_t>(image[offset + 3U]);
}
}  // namespace

GenesisCartridgeSramHeader parse_genesis_cartridge_sram_header(std::span<const std::uint8_t> image) noexcept {
  GenesisCartridgeSramHeader result;
  if (image.size() < header_offset + header_size || image[header_offset] != UINT8_C('R') ||
      image[header_offset + 1U] != UINT8_C('A'))
    return result;
  const std::uint8_t type = image[header_offset + 2U];
  const std::uint8_t kind = image[header_offset + 3U];
  const auto malformed = [&result]() {
    result.status = GenesisCartridgeSramHeaderStatus::malformed;
    return result;
  };
  // The second byte is $20 for SRAM/FRAM; $40 marks EEPROM.  Anything else is not a form this model knows.
  if (kind == UINT8_C(0x40)) {
    result.status = GenesisCartridgeSramHeaderStatus::unsupported_type;
    return result;
  }
  if (kind != UINT8_C(0x20)) return malformed();
  // Documented type bytes: $A0/$E0 16-bit, $B0/$F0 8-bit even, $B8/$F8 8-bit odd; the $40 bit is "saves on power-off".
  if (type != 0xA0U && type != 0xB0U && type != 0xB8U && type != 0xE0U && type != 0xF0U && type != 0xF8U) return malformed();
  const std::uint32_t start = be32(image, header_offset + 4U);
  const std::uint32_t end = be32(image, header_offset + 8U);
  if (start > end || start < SEGARECOMP_GENESIS_CARTRIDGE_SRAM_WINDOW_BEGIN ||
      end >= SEGARECOMP_GENESIS_CARTRIDGE_SRAM_WINDOW_END || end - start >= SEGARECOMP_GENESIS_CARTRIDGE_SRAM_MAX_SPAN)
    return malformed();
  GenesisCartridgeSramDescriptor descriptor;
  descriptor.start = start;
  descriptor.end = end;
  descriptor.battery_backed = (type & 0x40U) != 0U;
  descriptor.always_mapped = image.size() <= start;
  const bool sixteen_bit = (type & 0x10U) == 0U;
  descriptor.odd_lane = (type & 0x08U) != 0U;
  if (sixteen_bit) {
    // 16-bit SRAM spans whole words: the range is declared as an even start and an odd end.  Recorded, not supported.
    if ((start & 1U) != 0U || (end & 1U) == 0U) return malformed();
    descriptor.supported = false;
    descriptor.odd_lane = false;
  } else {
    // An 8-bit lane SRAM begins and ends on its own lane's addresses.
    const std::uint32_t parity = descriptor.odd_lane ? 1U : 0U;
    if ((start & 1U) != parity || (end & 1U) != parity) return malformed();
  }
  result.status = GenesisCartridgeSramHeaderStatus::declared;
  result.descriptor = descriptor;
  return result;
}

bool genesis_cartridge_sram_touches(const GenesisCartridgeSramDescriptor &descriptor, std::uint32_t address,
                                     std::uint32_t width_bytes) noexcept {
  return segarecomp_genesis_cartridge_sram_classify(descriptor.start, descriptor.end, descriptor.odd_lane ? 1U : 0U, address,
                                                     width_bytes) != SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_OUTSIDE;
}

bool genesis_cartridge_sram_access_supported(const GenesisCartridgeSramDescriptor &descriptor, std::uint32_t address,
                                              std::uint32_t width_bytes) noexcept {
  return descriptor.supported &&
         segarecomp_genesis_cartridge_sram_classify(descriptor.start, descriptor.end, descriptor.odd_lane ? 1U : 0U, address,
                                                     width_bytes) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_BYTE;
}

}  // namespace segarecomp
