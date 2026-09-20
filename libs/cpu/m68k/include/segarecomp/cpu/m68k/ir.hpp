#pragma once

// SEG-014-T002: MC68000-specific IR (docs/architecture/
// post-seg007-architecture-refactor-contract.md, section 8.2 "cpu/m68k/").
// Relocated from include/segarecomp/m68k_pipeline.hpp per
// docs/architecture/seg-014-t001-symbol-migration-map.md's `cpu/m68k/`
// section. No behavior changes.

#include "segarecomp/cpu/m68k/instruction.hpp"

namespace segarecomp {

enum class M68kIrKind {
  write_moveq, subtract_quick_long_d0, branch_ne_short, branch_always_short, return_from_subroutine, return_from_exception,
  test_operand, compare, compare_immediate, compare_address, add, add_address, add_immediate, add_quick, subtract, subtract_address, subtract_immediate, subtract_quick, logical_and, logical_and_immediate, logical_or, logical_or_immediate, exclusive_or, exclusive_or_immediate, write_move, write_movea, write_clr, load_effective_address, jump_general, call_general,
  // SEG-007-T025 (Batch C, C1).
  write_swap, sign_extend_word, sign_extend_long,
  // SEG-007-T025 (Batch C, C2). `push_effective_address` computes a control
  // EA's address (never reads its contents) and pushes that address as a
  // long onto the real A7 data stack -- unrelated to
  // M68kStackEffectKind::push_static_continuation, which is static CFG
  // call-provenance metadata, not arbitrary target-stack memory.
  push_effective_address, link_frame, unlink_frame,
  // SEG-007-T025 (Batch C, C3).
  bit_test, bit_change, bit_clear, bit_set,
  // SEG-007-T025 (Batch C, C4).
  general_branch, bsr_call, dbcc_loop,
  // SEG-007-T025 (Batch C, C5): see M68kInstructionKind::movem's doc comment.
  movem_transfer,
  // SEG-007-T025 (Batch C, C6): see M68kInstructionKind::shift_rotate's doc
  // comment.
  shift_rotate_register,
  // SEG-007-T025 (Batch C, C7a): the memory-WORD shift/rotate forms (`ss==11`)
  // C6 deliberately excluded. A genuine one-address read-modify-write (unlike
  // `shift_rotate_register`, which never touches memory) -- see
  // M68kInstructionKind::shift_rotate's doc comment for how `destination_ea`
  // (never `source_ea`, which is unused/default for this memory form: the
  // count is architecturally fixed to exactly 1, never a decoded operand)
  // distinguishes this from the register form at lift time.
  shift_rotate_memory,
  // MOVE An,USP writes persistent USP without memory or CCR effects.
  write_user_stack_pointer,
  // SEG-007-T088: MOVE <ea>,SR overwrites the entire existing generic 16-bit
  // `sr`/`status_register` runtime field from the decoded source operand
  // (see M68kInstructionKind::move_to_sr's own doc comment); no new
  // persistent-state field is introduced.
  write_status_register,
  // SEG-007-T114: NOP (see M68kInstructionKind::nop's doc comment). Per the
  // public Motorola M68000 Family Programmer's Reference Manual (1988) NOP
  // entry, "No operation occurs ... The processor state, other than the
  // program counter, is unaffected." This IR kind carries no operand, no
  // memory access, and no condition-code effect; its sole lowered effect is
  // the program-counter advance past the single instruction word.
  no_operation,
  // SEG-007-T116: MOVE from SR reads the existing generic 16-bit
  // `sr`/`status_register` runtime field (as a word) and writes it to the
  // decoded data-alterable `destination_ea` (see M68kInstructionKind::
  // move_from_sr's own doc comment). Contrast `write_status_register`: this
  // kind never modifies SR and affects NO condition codes (X/N/Z/V/C
  // unchanged). No new persistent-state field is introduced.
  read_status_register,
  // SEG-007-T118: MOVE <ea>,CCR reads the decoded data source operand (see
  // M68kInstructionKind::move_to_ccr's doc comment) as a word and writes only
  // its low-order byte into the CCR sub-field (low byte) of the existing
  // generic 16-bit `sr`/`status_register` runtime field. Contrast
  // `write_status_register`, which overwrites the entire 16-bit SR: this kind
  // leaves the upper (system) byte of SR untouched. It DOES affect all
  // condition codes (X/N/Z/V/C are replaced wholesale from the source). No new
  // persistent-state field is introduced.
  write_condition_codes,
  // SEG-007-T168: NOT <ea> (logical complement, byte/word/long), the
  // read-modify-write unary sibling of the AND/OR/EOR logical family (see
  // M68kInstructionKind::not_operand's own doc comment). `destination_ea`
  // (never `source_ea`, which is unused/default -- NOT has no second
  // operand) carries the single read-then-written EA, matching `write_clr`'s
  // convention for a sole destination-only slot. Reuses the SAME CCR
  // formula the logical family already established (N/Z set from the
  // result, V/C cleared) -- no new condition-code owner.
  logical_not,
  // NEG.W Dn: destination_ea is the sole read-then-written operand; zero is
  // the architecturally implied subtraction source.
  negate_word,
  // SEG-007-T220: MULS.W <ea>,Dn. See M68kInstructionKind::multiply_signed_
  // word's own doc comment for the full opcode/EA/CCR contract.
  multiply_signed_word,
  // SEG-007-T222: MULU.W/DIVS.W/DIVU.W. See the matching
  // M68kInstructionKind entries' own doc comments for the full contract
  // (ADR-0037 for the divide-by-zero synchronous exception).
  multiply_unsigned_word,
  divide_signed_word,
  divide_unsigned_word,
};

struct M68kIrOperation {
  InstructionProvenance provenance{};
  M68kIrKind kind{M68kIrKind::write_moveq};
  DataRegister destination{DataRegister::d0};
  std::int8_t operand{};
  // Carries the decoded record's verified span/extension into the typed
  // lifted operation; see M68kDecodedInstruction::raw_bytes/extension.
  std::vector<std::uint8_t> raw_bytes;
  std::uint32_t extension{};
  M68kMemoryAccessWidth size{M68kMemoryAccessWidth::long_word};
  M68kEffectiveAddress source_ea{};
  M68kEffectiveAddress destination_ea{};
  // SEG-007-T025 (Batch C, C4): see M68kDecodedInstruction::condition.
  M68kCondition condition{M68kCondition::always};
  // SEG-007-T025 (Batch C, C5): see M68kDecodedInstruction::movem_direction/
  // movem_register_mask.
  M68kMovemDirection movem_direction{M68kMovemDirection::registers_to_memory};
  std::uint16_t movem_register_mask{};
  // SEG-007-T025 (Batch C, C6): see M68kDecodedInstruction::shift_rotate_kind.
  M68kShiftRotateKind shift_rotate_kind{M68kShiftRotateKind::lsl};
};

[[nodiscard]] M68kIrOperation lift_m68k_instruction(const M68kDecodedInstruction &instruction);
[[nodiscard]] bool m68k_ir_is_transfer(const M68kIrOperation &operation) noexcept;

} // namespace segarecomp
