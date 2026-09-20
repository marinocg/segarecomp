#pragma once

#include "segarecomp/device/sega/genesis/controller_io.hpp"
#include "segarecomp/machine/genesis/address_space_contract.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace segarecomp {

inline constexpr std::uint32_t m68k_startup_ram_begin = SEGARECOMP_GENESIS_WORK_RAM_BEGIN;
inline constexpr std::uint32_t m68k_startup_ram_end = SEGARECOMP_GENESIS_WORK_RAM_END;
[[nodiscard]] bool m68k_startup_ram_range_in_range(std::uint32_t address, std::uint32_t width) noexcept;
[[nodiscard]] bool m68k_startup_ram_operand_in_range(std::uint32_t address) noexcept;
[[nodiscard]] std::uint32_t m68k_startup_ram_offset(std::uint32_t address) noexcept;

// ROM-free probe classification belongs to the Genesis address-space owner;
// it deliberately describes only the established cartridge/RAM/hardware
// buckets and is not a memory-map expansion.
enum class GenesisStartupMappingClass { raw_cartridge_rom, synthetic_work_ram, hardware_frontier };
[[nodiscard]] GenesisStartupMappingClass classify_genesis_startup_mapping(
    std::uint64_t address, std::uint32_t width, std::uint64_t image_length) noexcept;
[[nodiscard]] const char *genesis_startup_mapping_class_name(GenesisStartupMappingClass classification) noexcept;

// Marker result for a routed VDP-window access: the runtime owns the value,
// so the translation-time seam carries only the routing decision.
struct M68kVdpRoutedRead {};

// SEG-007-T113: the write-direction counterpart. A direct absolute-operand
// WORD or LONG store into the VDP register window is deferred to the runtime
// device gate (genesis_route_access -> genesis_vdp_access), exactly like the
// read marker above; the translation-time seam carries only the routing
// decision and never a static VDP register-semantics model.
struct M68kVdpRoutedWrite {};

// SEG-007-T115: the generalized routed-device marker for the remaining
// reachable non-VDP device windows whose genesis_route_access runtime owner
// already exists -- the 68k-side Z80 bus-arbitration control registers
// (SEG-007-T102), the flat Z80 program-RAM window (SEG-007-T103), and the
// co-located PSG audio port (SEG-007-T109). A direct absolute-operand access
// whose exact (address, width, direction) shape one of those runtime owners
// supports is deferred to genesis_route_access as a value-free routed access,
// exactly like the VDP markers above; `direction` is retained only so the
// coupled C4 re-verifiers can confirm the retained fact against the routing
// gate. The translation-time seam never models any device register.
struct M68kDeviceRoutedAccess {
  M68kMemoryAccessDirection direction{M68kMemoryAccessDirection::read};
};

using M68kGenesisDeviceRoutingResult =
    std::variant<M68kControllerIoResult, M68kControllerIoFailure, M68kVdpRoutedRead, M68kVdpRoutedWrite,
                 M68kDeviceRoutedAccess, DirectFlowDiagnostic>;
[[nodiscard]] M68kGenesisDeviceRoutingResult m68k_route_genesis_device_access(const M68kMemoryAccessRequest &request) noexcept;

struct M68kAbsoluteTestOperand {
  M68kAbsoluteOperandRegion region{M68kAbsoluteOperandRegion::raw_cartridge_rom};
  std::array<std::uint8_t, 4> bytes{};
  std::uint32_t value{};
  std::array<M68kControllerIoWordObservation, 2> controller_word_observations{};
  std::optional<M68kControllerIoPolicyProvenance> controller_policy_provenance;
  std::optional<M68kMemoryAccessRequest> access_request;
};
using M68kAbsoluteTestOperandResolution = std::variant<M68kAbsoluteTestOperand, DirectFlowDiagnostic, M68kControllerIoFailure>;
[[nodiscard]] M68kAbsoluteTestOperandResolution m68k_resolve_absolute_test_operand(std::span<const std::uint8_t> rom_image, std::uint32_t address) noexcept;
[[nodiscard]] M68kAbsoluteTestOperandResolution m68k_resolve_absolute_test_operand(
    std::span<const std::uint8_t> rom_image, std::uint32_t address,
    M68kMemoryAccessWidth width, M68kMemoryAccessDirection direction,
    std::optional<InstructionProvenance> source_provenance = std::nullopt) noexcept;

} // namespace segarecomp
