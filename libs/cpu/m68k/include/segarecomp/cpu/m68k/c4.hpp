#pragma once

// MC68000-owned C4 translation-readiness preflight vocabulary (moved from the
// generic recompiler package by SEG-018-T003; no behavior change).

#include "segarecomp/cpu/m68k/decode.hpp"

#include <string_view>
#include <vector>

namespace segarecomp {

// C4 describes translation readiness over an already-understood CPU program;
// it does not own machine routing or controller/device taxonomy.
enum class M68kC4OperandRole { source, destination, operation };
enum class M68kC4AutoUpdateClass { none, predecrement, postincrement };
enum class M68kC4GapClass { missing_dispatcher, missing_fact, missing_routing, requires_architecture_decision };
struct M68kC4PreflightRow {
  std::string_view family;
  M68kIrKind ir_kind{};
  M68kC4OperandRole operand_role{M68kC4OperandRole::operation};
  M68kMemoryAccessWidth width{M68kMemoryAccessWidth::word};
  M68kEaMode ea_class{M68kEaMode::data_register};
  M68kC4AutoUpdateClass auto_update{M68kC4AutoUpdateClass::none};
  M68kC4GapClass gap{M68kC4GapClass::missing_dispatcher};
  std::string_view predecessor;
};
struct M68kC4Preflight { bool valid{}; std::vector<M68kC4PreflightRow> rows; };

} // namespace segarecomp
