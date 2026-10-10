#ifndef SEGARECOMP_MACHINE_GENESIS_CARTRIDGE_SRAM_HPP
#define SEGARECOMP_MACHINE_GENESIS_CARTRIDGE_SRAM_HPP

#include <cstdint>
#include <optional>
#include <span>

#include "segarecomp/machine/genesis/address_space_contract.h"

namespace segarecomp {

// ADR 0098: the header-declared cartridge SRAM of a Genesis ROM.  Derived only from the immutable ROM header
// ("RA", type byte, $20, start, end at $1B0; plutiedev "ROM header reference"); no title or checksum is consulted.
// `supported` is false for a declared-but-unsupported form (16-bit SRAM); the extent is then still recorded so the
// runtime fails closed with a typed diagnostic on any access to it instead of treating it as ROM.
struct GenesisCartridgeSramDescriptor {
  std::uint32_t start{};         // inclusive, on the lane's parity
  std::uint32_t end{};           // inclusive, on the lane's parity
  bool odd_lane{true};           // 8-bit SRAM on odd (true) or even (false) addresses
  bool battery_backed{false};    // informational: the type byte's backup bit; persistence is a later feature
  bool supported{true};
  // The loaded ROM ends at or below `start`: nothing lies beneath the SRAM, so it is visible regardless of $A130F1.
  bool always_mapped{false};
  // Dense storage size: one byte per lane address of the extent.
  [[nodiscard]] std::uint32_t storage_bytes() const noexcept { return (end - start) / 2U + 1U; }
};

enum class GenesisCartridgeSramHeaderStatus {
  absent,            // no "RA" marker: the cartridge declares no extra memory
  declared,          // a descriptor is present (check `supported`)
  unsupported_type,  // extra memory that is not SRAM (second byte != $20, e.g. EEPROM): no device, accesses fail closed as before
  malformed,         // "RA" present but the type byte or range is not a valid SRAM description: no device
};

struct GenesisCartridgeSramHeader {
  GenesisCartridgeSramHeaderStatus status{GenesisCartridgeSramHeaderStatus::absent};
  std::optional<GenesisCartridgeSramDescriptor> descriptor;
};

// Parses the extra-memory descriptor of a whole cartridge image (header at $1B0).
[[nodiscard]] GenesisCartridgeSramHeader parse_genesis_cartridge_sram_header(std::span<const std::uint8_t> image) noexcept;

// True when the access touches the descriptor's extent at all (the cartridge SRAM owner, not the ROM, then answers it).
[[nodiscard]] bool genesis_cartridge_sram_touches(const GenesisCartridgeSramDescriptor &descriptor, std::uint32_t address,
                                                   std::uint32_t width_bytes) noexcept;
// True when it is exactly the BYTE access on a lane address the model supports.
[[nodiscard]] bool genesis_cartridge_sram_access_supported(const GenesisCartridgeSramDescriptor &descriptor,
                                                            std::uint32_t address, std::uint32_t width_bytes) noexcept;

}  // namespace segarecomp

#endif
