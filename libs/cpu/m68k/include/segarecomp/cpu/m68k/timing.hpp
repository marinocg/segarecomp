#pragma once

#include "segarecomp/cpu/m68k/ir.hpp"

#include <cstdint>
#include <optional>

namespace segarecomp {

// MC68000 instruction timing is a CPU contract.  Machine code consumes this
// typed result; it must not infer timing from generated dispatch structure.
[[nodiscard]] std::optional<std::uint32_t>
m68k_instruction_cycles(const M68kIrOperation &operation) noexcept;

// Table 8-1 effective-address cells.  Dynamic instruction timing expressions
// consume this typed CPU fact rather than duplicating an EA timing table in a
// machine/codegen owner.
[[nodiscard]] std::optional<std::uint32_t>
m68k_effective_address_cycles(const M68kEffectiveAddress &effective_address,
                              M68kMemoryAccessWidth size) noexcept;

}  // namespace segarecomp
