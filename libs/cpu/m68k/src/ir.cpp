#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

M68kIrOperation lift_m68k_instruction(const M68kDecodedInstruction &instruction) {
  M68kIrOperation operation{instruction.provenance, M68kIrKind::write_moveq,
                             instruction.destination, instruction.operand,
                             instruction.raw_bytes, instruction.extension};
  // SEG-007-T023: carried through unconditionally for every kind (harmless
  // for the pre-existing kinds, which never read these fields); this is the
  // sole place a decoded instruction's typed EA facts cross into the lifted
  // IR operation.
  operation.size = instruction.size;
  operation.source_ea = instruction.source_ea;
  operation.destination_ea = instruction.destination_ea;
  // SEG-007-T025 (Batch C, C4): carried through unconditionally, harmless
  // (`always`) default for every kind that does not use it.
  operation.condition = instruction.condition;
  // SEG-007-T025 (Batch C, C5): carried through unconditionally, harmless
  // defaults for every kind that does not use them.
  operation.movem_direction = instruction.movem_direction;
  operation.movem_register_mask = instruction.movem_register_mask;
  // SEG-007-T025 (Batch C, C6): carried through unconditionally, harmless
  // default (`lsl`) for every kind that does not use it.
  operation.shift_rotate_kind = instruction.shift_rotate_kind;
  switch (instruction.kind) {
  case M68kInstructionKind::moveq: break;
  case M68kInstructionKind::subq_l_1_d0: operation.kind = M68kIrKind::subtract_quick_long_d0; break;
  case M68kInstructionKind::bne_short: operation.kind = M68kIrKind::branch_ne_short; break;
  case M68kInstructionKind::bra_short: operation.kind = M68kIrKind::branch_always_short; break;
  case M68kInstructionKind::rts: operation.kind = M68kIrKind::return_from_subroutine; break;
  case M68kInstructionKind::rte: operation.kind = M68kIrKind::return_from_exception; break;
  case M68kInstructionKind::tst: operation.kind = M68kIrKind::test_operand; break;
  case M68kInstructionKind::cmp: operation.kind = M68kIrKind::compare; break;
  case M68kInstructionKind::cmpi: operation.kind = M68kIrKind::compare_immediate; break;
  case M68kInstructionKind::cmpa: operation.kind = M68kIrKind::compare_address; break;
  case M68kInstructionKind::add: operation.kind = M68kIrKind::add; break;
  case M68kInstructionKind::adda: operation.kind = M68kIrKind::add_address; break;
  case M68kInstructionKind::addi: operation.kind = M68kIrKind::add_immediate; break;
  case M68kInstructionKind::addq: operation.kind = M68kIrKind::add_quick; break;
  case M68kInstructionKind::sub: operation.kind = M68kIrKind::subtract; break;
  case M68kInstructionKind::suba: operation.kind = M68kIrKind::subtract_address; break;
  case M68kInstructionKind::subi: operation.kind = M68kIrKind::subtract_immediate; break;
  case M68kInstructionKind::subq: operation.kind = M68kIrKind::subtract_quick; break;
  case M68kInstructionKind::logical_and: operation.kind = M68kIrKind::logical_and; break;
  case M68kInstructionKind::andi: operation.kind = M68kIrKind::logical_and_immediate; break;
  case M68kInstructionKind::logical_or: operation.kind = M68kIrKind::logical_or; break;
  case M68kInstructionKind::ori: operation.kind = M68kIrKind::logical_or_immediate; break;
  case M68kInstructionKind::eor: operation.kind = M68kIrKind::exclusive_or; break;
  case M68kInstructionKind::eori: operation.kind = M68kIrKind::exclusive_or_immediate; break;
  case M68kInstructionKind::move: operation.kind = M68kIrKind::write_move; break;
  case M68kInstructionKind::movea: operation.kind = M68kIrKind::write_movea; break;
  case M68kInstructionKind::clr: operation.kind = M68kIrKind::write_clr; break;
  case M68kInstructionKind::not_operand: operation.kind = M68kIrKind::logical_not; break;
  case M68kInstructionKind::negate_word: operation.kind = M68kIrKind::negate_word; break;
  case M68kInstructionKind::negate_extended: operation.kind = M68kIrKind::negate_extended; break;
  case M68kInstructionKind::add_extended: operation.kind = M68kIrKind::add_extended; break;
  case M68kInstructionKind::subtract_extended: operation.kind = M68kIrKind::subtract_extended; break;
  case M68kInstructionKind::compare_memory: operation.kind = M68kIrKind::compare_memory; break;
  case M68kInstructionKind::lea: operation.kind = M68kIrKind::load_effective_address; break;
  case M68kInstructionKind::jmp: operation.kind = M68kIrKind::jump_general; break;
  case M68kInstructionKind::jsr: operation.kind = M68kIrKind::call_general; break;
  case M68kInstructionKind::swap: operation.kind = M68kIrKind::write_swap; break;
  case M68kInstructionKind::ext_w: operation.kind = M68kIrKind::sign_extend_word; break;
  case M68kInstructionKind::ext_l: operation.kind = M68kIrKind::sign_extend_long; break;
  case M68kInstructionKind::pea: operation.kind = M68kIrKind::push_effective_address; break;
  case M68kInstructionKind::link: operation.kind = M68kIrKind::link_frame; break;
  case M68kInstructionKind::unlk: operation.kind = M68kIrKind::unlink_frame; break;
  case M68kInstructionKind::btst: operation.kind = M68kIrKind::bit_test; break;
  case M68kInstructionKind::bchg: operation.kind = M68kIrKind::bit_change; break;
  case M68kInstructionKind::bclr: operation.kind = M68kIrKind::bit_clear; break;
  case M68kInstructionKind::bset: operation.kind = M68kIrKind::bit_set; break;
  case M68kInstructionKind::branch: operation.kind = M68kIrKind::general_branch; break;
  case M68kInstructionKind::bsr: operation.kind = M68kIrKind::bsr_call; break;
  case M68kInstructionKind::dbcc: operation.kind = M68kIrKind::dbcc_loop; break;
  case M68kInstructionKind::movem: operation.kind = M68kIrKind::movem_transfer; break;
  case M68kInstructionKind::shift_rotate:
    // SEG-007-T025 (Batch C, C7a): the register/memory distinction lives
    // purely in `destination_ea.mode`, never a second decoded fact -- a
    // register-form destination is always `data_register` (C6); every other
    // legal destination mode (the six `m68k_ea_memory_alterable` modes) is a
    // C7 memory read-modify-write.
    operation.kind = instruction.destination_ea.mode == M68kEaMode::data_register
                          ? M68kIrKind::shift_rotate_register
                          : M68kIrKind::shift_rotate_memory;
    break;
  case M68kInstructionKind::move_an_to_usp:
    operation.kind = M68kIrKind::write_user_stack_pointer;
    break;
  case M68kInstructionKind::move_to_sr:
    operation.kind = M68kIrKind::write_status_register;
    break;
  case M68kInstructionKind::nop:
    operation.kind = M68kIrKind::no_operation;
    break;
  case M68kInstructionKind::move_from_sr:
    operation.kind = M68kIrKind::read_status_register;
    break;
  case M68kInstructionKind::move_to_ccr:
    operation.kind = M68kIrKind::write_condition_codes;
    break;
  case M68kInstructionKind::multiply_signed_word:
    operation.kind = M68kIrKind::multiply_signed_word;
    break;
  case M68kInstructionKind::multiply_unsigned_word:
    operation.kind = M68kIrKind::multiply_unsigned_word;
    break;
  case M68kInstructionKind::divide_signed_word:
    operation.kind = M68kIrKind::divide_signed_word;
    break;
  case M68kInstructionKind::divide_unsigned_word:
    operation.kind = M68kIrKind::divide_unsigned_word;
    break;
  }
  return operation;
}

bool m68k_ir_is_transfer(const M68kIrOperation &operation) noexcept {
  return operation.kind == M68kIrKind::branch_ne_short || operation.kind == M68kIrKind::branch_always_short ||
          operation.kind == M68kIrKind::return_from_subroutine ||
         operation.kind == M68kIrKind::return_from_exception ||
         operation.kind == M68kIrKind::jump_general || operation.kind == M68kIrKind::call_general;
}

} // namespace segarecomp
