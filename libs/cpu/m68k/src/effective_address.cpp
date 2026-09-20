#include "segarecomp/cpu/m68k/effective_address.hpp"

namespace segarecomp {

bool m68k_is_statically_foldable_control_ea(const M68kEffectiveAddress &ea) noexcept {
  return ea.mode == M68kEaMode::absolute_word || ea.mode == M68kEaMode::absolute_long ||
         ea.mode == M68kEaMode::pc_disp16;
}

bool m68k_is_supported_computed_control_ea(const M68kEffectiveAddress &ea) noexcept {
  if (ea.mode == M68kEaMode::pc_index8) return true;
  return ea.mode == M68kEaMode::address_indirect && ea.displacement == 0 && ea.extension_words == 0U;
}

std::optional<DirectFlowDiagnostic> m68k_startup_absolute_operand_alignment(std::uint32_t address,
                                                                             M68kMemoryAccessWidth width) noexcept {
  if ((address & UINT32_C(0xFF000000)) != 0U) return DirectFlowDiagnostic::effective_address_not_24bit;
  // SEG-007-T023 bugfix: a byte-size access carries no alignment restriction
  // at all on the base MC68000 (only word/long-word operands and
  // instruction fetches require an even address; see the batch contract's
  // "Addressing-mode mechanics").
  if (width != M68kMemoryAccessWidth::byte && (address & 1U) != 0U) return DirectFlowDiagnostic::odd_effective_address;
  return std::nullopt;
}

std::uint32_t m68k_canonical_ea_address(const M68kEffectiveAddress &ea) noexcept {
  if (ea.mode == M68kEaMode::absolute_word) return ea.absolute_address & UINT32_C(0x00FFFFFF);
  return ea.absolute_address;
}

} // namespace segarecomp
