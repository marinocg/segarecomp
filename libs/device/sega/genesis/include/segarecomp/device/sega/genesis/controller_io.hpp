#pragma once

#include "segarecomp/device/sega/genesis/controller_io_contract.h"
#include "segarecomp/machine/genesis/address_types.hpp"
#include "segarecomp/cpu/m68k/direct_flow.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace segarecomp {

enum class ControllerIoAccessShapeMismatch { width, direction, partial_or_crossing_shape, different_controller_io_register_shape };
enum class ControllerIoTargetRegisterClass { version, data, ctrl, s_ctrl, tx_data, rx_data, unclassified };
struct ControllerIoAccessShape { M68kMemoryAccessWidth width{M68kMemoryAccessWidth::long_word}; M68kMemoryAccessDirection direction{M68kMemoryAccessDirection::read}; std::vector<ControllerIoAccessShapeMismatch> mismatches; std::optional<ControllerIoTargetRegisterClass> target_register_class; };

inline constexpr std::uint32_t m68k_controller_io_selector_begin = SEGARECOMP_GENESIS_CONTROLLER_CTRL1_CTRL2_LONG_BEGIN;
inline constexpr std::uint32_t m68k_controller_io_selector_width = 4U;
inline constexpr std::uint32_t m68k_controller_io_region_begin = SEGARECOMP_GENESIS_CONTROLLER_IO_BEGIN;
inline constexpr std::uint32_t m68k_controller_io_region_end = SEGARECOMP_GENESIS_CONTROLLER_IO_END;
inline constexpr std::uint32_t m68k_controller_io_version_register_address = SEGARECOMP_GENESIS_CONTROLLER_VERSION_BYTE_ADDRESS;
inline constexpr std::uint32_t m68k_controller_io_data1_register_address = UINT32_C(0x00A10003);
inline constexpr std::uint32_t m68k_controller_io_data2_register_address = UINT32_C(0x00A10005);
inline constexpr std::uint32_t m68k_controller_io_data3_register_address = UINT32_C(0x00A10007);
inline constexpr std::uint32_t m68k_controller_io_ctrl3_register_address = SEGARECOMP_GENESIS_CONTROLLER_CTRL3_WORD_BEGIN + 1U;
inline constexpr std::uint32_t m68k_controller_io_tx_data1_register_address = UINT32_C(0x00A1000F);
inline constexpr std::uint32_t m68k_controller_io_rx_data1_register_address = UINT32_C(0x00A10011);
inline constexpr std::uint32_t m68k_controller_io_s_ctrl1_register_address = UINT32_C(0x00A10013);
inline constexpr std::uint32_t m68k_controller_io_tx_data2_register_address = UINT32_C(0x00A10015);
inline constexpr std::uint32_t m68k_controller_io_rx_data2_register_address = UINT32_C(0x00A10017);
inline constexpr std::uint32_t m68k_controller_io_s_ctrl2_register_address = UINT32_C(0x00A10019);
inline constexpr std::uint32_t m68k_controller_io_tx_data3_register_address = UINT32_C(0x00A1001B);
inline constexpr std::uint32_t m68k_controller_io_rx_data3_register_address = UINT32_C(0x00A1001D);
inline constexpr std::uint32_t m68k_controller_io_s_ctrl3_register_address = UINT32_C(0x00A1001F);

enum class M68kControllerIoPolicyProvenance { seg_007_t020, seg_007_t038, seg_007_t079, seg_007_t111 };
struct M68kControllerIoWordObservation { std::uint16_t value{}; std::uint32_t transfer_ordinal{}; };
struct M68kControllerIoResult { M68kMemoryAccessRequest request{}; std::uint32_t value{}; std::array<M68kControllerIoWordObservation, 2> word_observations{}; M68kControllerIoPolicyProvenance policy_provenance{M68kControllerIoPolicyProvenance::seg_007_t020}; };
struct M68kControllerIoFailure { DirectFlowDiagnostic category{DirectFlowDiagnostic::unsupported_device_region_controller_io}; M68kMemoryAccessRequest request{}; };
using M68kControllerIoAccessResult = std::variant<M68kControllerIoResult, M68kControllerIoFailure>;

[[nodiscard]] M68kControllerIoAccessResult m68k_controller_io_access(const M68kMemoryAccessRequest &request) noexcept;
[[nodiscard]] ControllerIoAccessShape m68k_classify_controller_io_access_shape(const M68kMemoryAccessRequest &request) noexcept;
[[nodiscard]] ControllerIoTargetRegisterClass m68k_classify_controller_io_target_register_class(const M68kMemoryAccessRequest &request) noexcept;

} // namespace segarecomp
