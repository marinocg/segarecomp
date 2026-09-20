#pragma once

// SEG-014-T002: the shared typed-provenance contract (docs/architecture/
// post-seg007-architecture-refactor-contract.md, section 3.2 and 8.1
// "provenance"). Relocated from include/segarecomp/moveq.hpp per
// docs/architecture/seg-014-t001-symbol-migration-map.md's `core/` section;
// no field or behavior changes.

#include "segarecomp/core/address.hpp"

#include <array>
#include <cstdint>

namespace segarecomp {

struct DecodeSource {
  CpuVariant cpu_variant{CpuVariant::mc68000};
  M68kProgramAddress address{};
  MoveqImageOffset image_offset{};
};

struct InstructionProvenance {
  DecodeSource source{};
  std::array<std::uint8_t, 2> bytes{};
  ByteLength length{2};
};

} // namespace segarecomp
