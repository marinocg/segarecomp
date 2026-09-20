#include "segarecomp/machine/genesis/address_space.hpp"
#include "segarecomp/cpu/m68k/effective_address.hpp"

namespace segarecomp {

bool m68k_startup_ram_range_in_range(std::uint32_t address, std::uint32_t width) noexcept {
  return segarecomp_genesis_work_ram_contains(address, width) != 0;
}
bool m68k_startup_ram_operand_in_range(std::uint32_t address) noexcept { return m68k_startup_ram_range_in_range(address, 4U); }
std::uint32_t m68k_startup_ram_offset(std::uint32_t address) noexcept { return address - m68k_startup_ram_begin; }

GenesisStartupMappingClass classify_genesis_startup_mapping(
    std::uint64_t address, std::uint32_t width, std::uint64_t image_length) noexcept {
  if (address < image_length && static_cast<std::uint64_t>(width) <= image_length - address)
    return GenesisStartupMappingClass::raw_cartridge_rom;
  if (address <= UINT32_MAX && m68k_startup_ram_range_in_range(static_cast<std::uint32_t>(address), width))
    return GenesisStartupMappingClass::synthetic_work_ram;
  return GenesisStartupMappingClass::hardware_frontier;
}

const char *genesis_startup_mapping_class_name(GenesisStartupMappingClass classification) noexcept {
  switch (classification) {
  case GenesisStartupMappingClass::raw_cartridge_rom: return "raw_cartridge_rom";
  case GenesisStartupMappingClass::synthetic_work_ram: return "synthetic_work_ram";
  case GenesisStartupMappingClass::hardware_frontier: return "hardware_frontier";
  }
  return "hardware_frontier";
}

M68kGenesisDeviceRoutingResult m68k_route_genesis_device_access(const M68kMemoryAccessRequest &request) noexcept {
  const auto end = static_cast<std::uint64_t>(request.address.value) + static_cast<std::uint32_t>(request.width);
  // SEG-007-T115: the co-located PSG (SN76489) audio port at the odd byte
  // $C00011 is recognized BEFORE the VDP window below -- identical ordering to
  // the runtime's genesis_route_access -- because the address is inside the
  // VDP interval. The runtime owner (genesis_route_access ->
  // genesis_psg_access, SEG-007-T109) supports exactly a BYTE WRITE; every
  // other shape stays an unmapped-data frontier.
  if (segarecomp_genesis_psg_port_contains(request.address.value) != 0 &&
      request.direction == M68kMemoryAccessDirection::write &&
      request.width == M68kMemoryAccessWidth::byte)
    return M68kDeviceRoutedAccess{request.direction};
  // VDP register window: the runtime already fully owns a WORD READ of this
  // window (genesis_route_access -> genesis_vdp_access). The translation-time
  // seam only needs to recognize that exact shape and defer the value to the
  // runtime; every other width/direction stays an unmapped-data frontier,
  // identical to the controller-I/O precedent below.
  if (segarecomp_genesis_vdp_region_contains(request.address.value,
                                             static_cast<std::uint32_t>(request.width)) != 0 &&
      request.direction == M68kMemoryAccessDirection::read &&
      request.width == M68kMemoryAccessWidth::word)
    return M68kVdpRoutedRead{};
  // SEG-007-T113: write-direction mirror. A WORD or LONG store fully inside
  // the VDP register window is deferred to the runtime device gate
  // (genesis_route_access already lowers a LONG write as two WORD-write
  // transactions via genesis_vdp_control_port_write_word). Every other
  // width/direction -- notably a BYTE store -- stays an unmapped-data
  // frontier, identical to the read arm above and the controller-I/O
  // precedent below.
  if (segarecomp_genesis_vdp_region_contains(request.address.value,
                                             static_cast<std::uint32_t>(request.width)) != 0 &&
      request.direction == M68kMemoryAccessDirection::write &&
      (request.width == M68kMemoryAccessWidth::word ||
       request.width == M68kMemoryAccessWidth::long_word))
    return M68kVdpRoutedWrite{};
  // SEG-007-T115: the two 68k-side Z80 bus-arbitration control registers.
  // The SEG-007-T102 Z80 bus-arbitration policy owner supports: BUSREQ
  // ($A11100) WORD/BYTE read or write; RESET ($A11200) WORD/BYTE write only.
  // Any LONG access, a RESET read, and every other in-region sub-address stay
  // an unmapped-data frontier for the caller to classify.
  if (segarecomp_genesis_z80_arbitration_region_contains(request.address.value) != 0 &&
      (request.width == M68kMemoryAccessWidth::word || request.width == M68kMemoryAccessWidth::byte)) {
    if (request.address.value == SEGARECOMP_GENESIS_Z80_ARBITRATION_BUSREQ_REGISTER)
      return M68kDeviceRoutedAccess{request.direction};
    if (request.address.value == SEGARECOMP_GENESIS_Z80_ARBITRATION_RESET_REGISTER &&
        request.direction == M68kMemoryAccessDirection::write)
      return M68kDeviceRoutedAccess{request.direction};
  }
  // SEG-007-T115: the flat 68000-visible Z80 program-RAM window ($A00000,
  // GENESIS_Z80_RAM_BYTES). Runtime owner genesis_z80_ram_window_access
  // (SEG-007-T103) supports a BYTE read or write, gated at runtime on the
  // live bus-grant latch -- which the static seam neither models nor
  // pre-decides; it only defers the shape to the runtime owner. WORD/LONG
  // stay an unmapped-data frontier.
  if (segarecomp_genesis_z80_ram_window_contains(request.address.value) != 0 &&
      request.width == M68kMemoryAccessWidth::byte)
    return M68kDeviceRoutedAccess{request.direction};
  // SEG-007-T171: the YM2612 FM-synthesis register window ($A04000-$A04003).
  // Five runtime-confirmed shapes are accepted here, all BYTE width: a READ
  // of the PART-I status port ($A04000); a WRITE of the PART-I address port
  // ($A04000, register-select latch); a WRITE of the PART-I data port
  // ($A04001, register-data write); a WRITE of the PART-II address port
  // ($A04002, register-select latch for the PART-II register bank); and a
  // WRITE of the PART-II data port ($A04003, register-data write for the
  // PART-II register bank -- this task's own fifth frontier pass). The
  // runtime device owner (genesis_route_access -> genesis_ym2612_access)
  // owns all five; the static seam only recognizes the shape and defers to
  // that owner, exactly like the PSG / Z80 precedents above. Every
  // remaining shape (a READ of any of these four write-only ports, or the
  // still-unconfirmed PART-II status-port READ) stays an unmapped-data
  // frontier until it is itself runtime-confirmed.
  if (segarecomp_genesis_ym2612_region_contains(request.address.value) != 0 &&
      request.width == M68kMemoryAccessWidth::byte &&
      request.address.value == SEGARECOMP_GENESIS_YM2612_PART1_ADDRESS_PORT)
    return M68kDeviceRoutedAccess{request.direction};
  if (segarecomp_genesis_ym2612_region_contains(request.address.value) != 0 &&
      request.direction == M68kMemoryAccessDirection::write &&
      request.width == M68kMemoryAccessWidth::byte &&
      (request.address.value == SEGARECOMP_GENESIS_YM2612_PART1_DATA_PORT ||
       request.address.value == SEGARECOMP_GENESIS_YM2612_PART2_ADDRESS_PORT ||
       request.address.value == SEGARECOMP_GENESIS_YM2612_PART2_DATA_PORT))
    return M68kDeviceRoutedAccess{request.direction};
  // SEG-007-T121: the controller-I/O GPIO-register WRITE-direction selector
  // family (DATA1..DATA3 / CTRL1..CTRL3, BYTE width only; the runtime-reached
  // form is a CTRL3 $A1000D BYTE write). The runtime device
  // owner (genesis_route_access -> genesis_controller_io_access) performs the
  // deterministic latched-register store; the static seam only recognises the
  // shape and defers to that owner, exactly like the PSG / Z80 precedents
  // above. Every other controller-I/O write shape stays fail-closed via the
  // read-only selector table below.
  if (request.direction == M68kMemoryAccessDirection::write &&
      segarecomp_genesis_controller_io_gpio_register_index(
          request.address.value, static_cast<std::uint32_t>(request.width)) >= 0)
    return M68kDeviceRoutedAccess{request.direction};
  // SEG-007-T166: the controller-I/O DATA1/DATA2 three-button-pad BYTE-READ
  // selector family. Unlike the four fixed-constant selectors the fallback
  // table below folds statically, a DATA1/DATA2 BYTE read is a deterministic
  // function of live CTRL/DATA latch state the CPU sets dynamically -- it
  // cannot be folded to one static constant. This recognizes exactly the
  // (DATA1 $A10003 | DATA2 $A10005, BYTE, read) shape -- reusing the same
  // GPIO-register recognizer the write case above already uses, filtered to
  // slot 0/1 -- and defers the value to the runtime device owner
  // (genesis_route_access -> genesis_controller_io_access), identical in
  // shape to the VDP status-register read precedent above. DATA3 and every
  // other width/direction stay unmapped/fail-closed via the read-only
  // selector table below.
  if (request.direction == M68kMemoryAccessDirection::read &&
      request.width == M68kMemoryAccessWidth::byte) {
    const int data_port_slot = segarecomp_genesis_controller_io_gpio_register_index(
        request.address.value, static_cast<std::uint32_t>(request.width));
    if (data_port_slot == 0 || data_port_slot == 1) return M68kDeviceRoutedAccess{request.direction};
  }
  if (request.address.value >= m68k_controller_io_region_end || end <= m68k_controller_io_region_begin)
    return DirectFlowDiagnostic::unmapped_data_access;
  const auto result = m68k_controller_io_access(request);
  if (const auto *success = std::get_if<M68kControllerIoResult>(&result)) return *success;
  return std::get<M68kControllerIoFailure>(result);
}

M68kAbsoluteTestOperandResolution m68k_resolve_absolute_test_operand(
    std::span<const std::uint8_t> image, std::uint32_t address, M68kMemoryAccessWidth width,
    M68kMemoryAccessDirection direction, std::optional<InstructionProvenance> provenance) noexcept {
  if (const auto misaligned = m68k_startup_absolute_operand_alignment(address, width)) return *misaligned;
  const auto size = static_cast<std::uint32_t>(width);
  if (static_cast<std::uint64_t>(address) + size <= image.size()) {
    if (direction == M68kMemoryAccessDirection::write) return DirectFlowDiagnostic::rom_write_prohibited;
    M68kAbsoluteTestOperand operand{}; operand.region = M68kAbsoluteOperandRegion::raw_cartridge_rom;
    for (std::uint32_t i = 0; i < size; ++i) { operand.bytes[i] = image[static_cast<std::size_t>(address + i)]; operand.value = (operand.value << 8U) | operand.bytes[i]; }
    return operand;
  }
  if (m68k_startup_ram_range_in_range(address, size)) { M68kAbsoluteTestOperand operand{}; operand.region = M68kAbsoluteOperandRegion::synthetic_work_ram; return operand; }
  const auto routed = m68k_route_genesis_device_access({{TargetAddressSpace::m68k_program, address}, width, direction, std::move(provenance)});
  if (const auto *success = std::get_if<M68kControllerIoResult>(&routed)) {
    M68kAbsoluteTestOperand operand{}; operand.region = M68kAbsoluteOperandRegion::controller_io; operand.value = success->value;
    operand.controller_word_observations = success->word_observations; operand.controller_policy_provenance = success->policy_provenance; operand.access_request = success->request;
    return operand;
  }
  if (std::holds_alternative<M68kVdpRoutedRead>(routed) ||
      std::holds_alternative<M68kVdpRoutedWrite>(routed)) {
    // SEG-007-T113: both the read and the write marker reduce to the same
    // value-free vdp-region operand -- the runtime device gate owns the
    // access. The image-bounds write check above already returned
    // rom_write_prohibited for any store inside the loaded image, so a VDP
    // window address (above the image) reaches this routing branch.
    M68kAbsoluteTestOperand operand{};
    operand.region = M68kAbsoluteOperandRegion::vdp;
    return operand;
  }
  if (std::holds_alternative<M68kDeviceRoutedAccess>(routed)) {
    // SEG-007-T115: a Z80 bus-arbitration / Z80 program-RAM / PSG absolute
    // operand whose shape the runtime device owner supports reduces to the
    // same value-free routed operand -- genesis_route_access owns the access
    // and every device's state. The image-bounds write check above already
    // returned rom_write_prohibited for any store inside the loaded image, so
    // a device-window address (above the image) reaches this routing branch.
    M68kAbsoluteTestOperand operand{};
    operand.region = M68kAbsoluteOperandRegion::routed_device;
    return operand;
  }
  if (const auto *failure = std::get_if<M68kControllerIoFailure>(&routed)) return *failure;
  return std::get<DirectFlowDiagnostic>(routed);
}

M68kAbsoluteTestOperandResolution m68k_resolve_absolute_test_operand(std::span<const std::uint8_t> image, std::uint32_t address) noexcept {
  return m68k_resolve_absolute_test_operand(image, address, M68kMemoryAccessWidth::long_word, M68kMemoryAccessDirection::read);
}

} // namespace segarecomp
