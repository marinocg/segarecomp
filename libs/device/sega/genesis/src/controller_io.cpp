#include "segarecomp/device/sega/genesis/controller_io.hpp"

namespace segarecomp {

M68kControllerIoAccessResult m68k_controller_io_access(const M68kMemoryAccessRequest &request) noexcept {
  for (std::size_t index = 0; index < SEGARECOMP_GENESIS_CONTROLLER_IO_SELECTOR_COUNT; ++index) {
    const auto &selector = segarecomp_genesis_controller_io_selectors[index];
    if (request.address.space != TargetAddressSpace::m68k_program || request.address.value != selector.address ||
        static_cast<std::uint32_t>(request.width) != selector.width ||
        static_cast<std::uint32_t>(request.direction) != selector.direction) continue;
    const auto identity = static_cast<M68kControllerIoPolicyProvenance>(selector.identity - 1);
    std::array<M68kControllerIoWordObservation, 2> observations{};
    observations[0] = {static_cast<std::uint16_t>(selector.policy_value), 0U};
    if (selector.width > 1U) observations[1] = {0U, 1U};
    return M68kControllerIoResult{request, selector.policy_value, observations, identity};
  }
  return M68kControllerIoFailure{DirectFlowDiagnostic::unsupported_device_region_controller_io, request};
}

ControllerIoTargetRegisterClass m68k_classify_controller_io_target_register_class(const M68kMemoryAccessRequest &request) noexcept {
  const auto begin = static_cast<std::uint64_t>(request.address.value);
  const auto end = begin + static_cast<std::uint32_t>(request.width);
  const auto matches = [&](std::uint32_t address) { return begin == static_cast<std::uint64_t>(address) - 1U && end == begin + 2U; };
  if (matches(m68k_controller_io_version_register_address)) return ControllerIoTargetRegisterClass::version;
  if (matches(m68k_controller_io_data1_register_address) || matches(m68k_controller_io_data2_register_address) || matches(m68k_controller_io_data3_register_address)) return ControllerIoTargetRegisterClass::data;
  if (matches(m68k_controller_io_ctrl3_register_address)) return ControllerIoTargetRegisterClass::ctrl;
  if (matches(m68k_controller_io_s_ctrl1_register_address) || matches(m68k_controller_io_s_ctrl2_register_address) || matches(m68k_controller_io_s_ctrl3_register_address)) return ControllerIoTargetRegisterClass::s_ctrl;
  if (matches(m68k_controller_io_tx_data1_register_address) || matches(m68k_controller_io_tx_data2_register_address) || matches(m68k_controller_io_tx_data3_register_address)) return ControllerIoTargetRegisterClass::tx_data;
  if (matches(m68k_controller_io_rx_data1_register_address) || matches(m68k_controller_io_rx_data2_register_address) || matches(m68k_controller_io_rx_data3_register_address)) return ControllerIoTargetRegisterClass::rx_data;
  return ControllerIoTargetRegisterClass::unclassified;
}

ControllerIoAccessShape m68k_classify_controller_io_access_shape(const M68kMemoryAccessRequest &request) noexcept {
  ControllerIoAccessShape shape{}; shape.width = request.width; shape.direction = request.direction;
  if (request.width != M68kMemoryAccessWidth::long_word) shape.mismatches.push_back(ControllerIoAccessShapeMismatch::width);
  if (request.direction != M68kMemoryAccessDirection::read) shape.mismatches.push_back(ControllerIoAccessShapeMismatch::direction);
  const auto begin = static_cast<std::uint64_t>(request.address.value);
  const auto end = begin + static_cast<std::uint32_t>(request.width);
  const auto selector_end = static_cast<std::uint64_t>(m68k_controller_io_selector_begin) + m68k_controller_io_selector_width;
  if (begin != m68k_controller_io_selector_begin || end != selector_end) {
    if (end <= m68k_controller_io_selector_begin || begin >= selector_end) {
      shape.mismatches.push_back(ControllerIoAccessShapeMismatch::different_controller_io_register_shape);
      shape.target_register_class = m68k_classify_controller_io_target_register_class(request);
    } else shape.mismatches.push_back(ControllerIoAccessShapeMismatch::partial_or_crossing_shape);
  }
  return shape;
}

} // namespace segarecomp
