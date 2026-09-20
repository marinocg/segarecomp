#pragma once

#include "segarecomp/core/address.hpp"
#include "segarecomp/core/provenance.hpp"
#include "segarecomp/cpu/m68k/instruction.hpp"

#include <optional>

namespace segarecomp {

// A CPU request is routed by the Genesis machine; it is not a Genesis device
// protocol in its own right.
struct M68kMemoryAccessRequest {
  M68kProgramAddress address{};
  M68kMemoryAccessWidth width{M68kMemoryAccessWidth::long_word};
  M68kMemoryAccessDirection direction{M68kMemoryAccessDirection::read};
  std::optional<InstructionProvenance> source_provenance;
};

// SEG-007-T115: `routed_device` is the generalized routed-device region --
// a direct absolute operand whose (address, width, direction) shape an
// existing genesis_route_access runtime device owner already fully owns for a
// non-VDP device window (68k-side Z80 bus-arbitration control registers,
// SEG-007-T102; flat Z80 program-RAM window, SEG-007-T103; co-located PSG
// audio port, SEG-007-T109). It extends -- never collapses -- the enumerated
// per-region model: the translation seam carries only the routing decision
// and the runtime stays the sole holder of every device's state.
enum class M68kAbsoluteOperandRegion { raw_cartridge_rom, synthetic_work_ram, controller_io, vdp, routed_device };

} // namespace segarecomp
