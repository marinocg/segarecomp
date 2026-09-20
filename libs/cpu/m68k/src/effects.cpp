#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/effective_address.hpp"

namespace segarecomp {

std::uint16_t m68k_move_result_ccr(std::uint16_t status_register, std::uint32_t result) noexcept {
  return M68kMoveResultCcrSpecification::apply(status_register, result);
}

M68kSubtractionResult m68k_evaluate_subtraction(std::uint32_t source, std::uint32_t destination,
                                                 M68kMemoryAccessWidth width) noexcept {
  return M68kSubtractionResultSpecification::evaluate(source, destination, width);
}

M68kAdditionResult m68k_evaluate_addition(std::uint32_t source, std::uint32_t destination,
                                          M68kMemoryAccessWidth width) noexcept {
  return M68kAdditionResultSpecification::evaluate(source, destination, width);
}

std::uint16_t m68k_addition_ccr(std::uint16_t status_register, std::uint32_t source,
                                std::uint32_t destination, M68kMemoryAccessWidth width) noexcept {
  return M68kAdditionResultSpecification::apply(status_register, source, destination, width);
}

M68kLogicalResult m68k_evaluate_logical(std::uint32_t result, M68kMemoryAccessWidth width) noexcept {
  return M68kLogicalResultSpecification::evaluate(result, width);
}

std::uint16_t m68k_logical_ccr(std::uint16_t status_register, std::uint32_t result,
                               M68kMemoryAccessWidth width) noexcept {
  return M68kLogicalResultSpecification::apply(status_register, result, width);
}

M68kBitOperationResult m68k_evaluate_bit_operation(M68kBitOperationKind kind, std::uint32_t original,
                                                    std::uint32_t bit_number,
                                                    M68kMemoryAccessWidth destination_width) noexcept {
  return M68kBitOperationSpecification::evaluate(kind, original, bit_number, destination_width);
}

std::uint16_t m68k_bit_test_ccr(std::uint16_t status_register, bool original_bit_set) noexcept {
  return M68kBitTestCcrSpecification::apply(status_register, original_bit_set);
}

bool m68k_evaluate_condition(M68kCondition condition, std::uint16_t status_register) noexcept {
  return M68kConditionSpecification::evaluate(condition, status_register);
}

std::string m68k_condition_c_expr(M68kCondition condition, std::string_view status_register) {
  return M68kConditionSpecification::c_expr(condition, status_register);
}

// SEG-007-T025 (Batch C, C4): the one shared branch-displacement target
// formula (contract: "branch target formula"). Verified against pinned
// Musashi's m68ki_branch_8/16 (each applied to REG_PC already advanced past
// only the primary word, i.e. source+2, regardless of byte/word width).
std::uint32_t m68k_branch_target(std::uint32_t source_address, std::int32_t signed_displacement) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::int64_t>(source_address) + 2 + signed_displacement);
}

// SEG-007-T025 (Batch C, C4c review correction): thin delegate onto the one
// shared M68kDbccDecrementSpecification owner above -- no arithmetic formula
// is duplicated here (contract: "host wrapper must delegate").
M68kDbccDecrementResult m68k_evaluate_dbcc_decrement(std::uint32_t original) noexcept {
  return M68kDbccDecrementSpecification::evaluate(original);
}

// SEG-007-T025 (Batch C, C5a/C5b): see the header declaration's doc comment.
// The one shared owner of MOVEM's transfer order, for both shapes. Never
// duplicated independently anywhere else (host tests, generated-C emitter,
// static resolver, or Musashi harness all call this one function).
std::vector<std::uint8_t> m68k_movem_transfer_order(std::uint16_t register_mask, M68kMovemTransferOrder order) {
  std::vector<std::uint8_t> sequence;
  for (std::uint8_t index = 0U; index < 16U; ++index) {
    if ((register_mask & static_cast<std::uint16_t>(1U << index)) != 0U)
      sequence.push_back(order == M68kMovemTransferOrder::predecrement
                              ? static_cast<std::uint8_t>(15U - index)
                              : index);
  }
  return sequence;
}

M68kShiftRotateResult m68k_evaluate_shift_rotate(M68kShiftRotateKind kind, std::uint32_t original,
                                                  std::uint32_t count, M68kMemoryAccessWidth width,
                                                  bool original_extend) noexcept {
  return M68kShiftRotateSpecification::evaluate(kind, original, count, width, original_extend);
}

std::uint16_t m68k_shift_rotate_ccr(std::uint16_t status_register, const M68kShiftRotateResult &result) noexcept {
  return M68kShiftRotateSpecification::ccr(status_register, result);
}

std::uint16_t m68k_compare_ccr(std::uint16_t status_register, std::uint32_t source,
                               std::uint32_t destination, M68kMemoryAccessWidth width) noexcept {
  const M68kIrOperation compare_operation{.kind = M68kIrKind::compare, .raw_bytes = {}};
  return m68k_subtraction_ccr(status_register, source, destination, width,
                              m68k_operation_effect(compare_operation).extend_flag_policy);
}

std::uint16_t m68k_subtraction_ccr(std::uint16_t status_register, std::uint32_t source,
                                   std::uint32_t destination, M68kMemoryAccessWidth width,
                                   M68kExtendFlagPolicy extend_flag_policy) noexcept {
  return M68kSubtractionResultSpecification::apply(status_register, source, destination, width,
                                                    extend_flag_policy);
}


M68kOperationEffect m68k_operation_effect(const M68kIrOperation &operation) noexcept {
  M68kOperationEffect effect{};
  switch (operation.kind) {
  case M68kIrKind::write_moveq:
    // Both existing callers write MOVEQ's result unconditionally to D0
    // regardless of the decoded destination-register field; preserve that
    // exact existing behavior rather than deriving the register from
    // operation.destination, which would be a silent behavior change.
    effect.register_write = M68kRegisterWrite{DataRegister::d0,
        static_cast<std::uint32_t>(static_cast<std::int32_t>(operation.operand))};
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = 2U;
    break;
  case M68kIrKind::return_from_subroutine:
    effect.stack = M68kStackEffectKind::pop_static_return;
    effect.stack_width = 4U;
    effect.pc = M68kPcEffectKind::observed_stack_return;
    break;
  case M68kIrKind::return_from_exception:
    // SEG-007-T047 / ADR-0020 §9: pops the basic MC68000 exception stack frame
    // (SR at SP, PC at SP+2, then A7 += 6). Compared-only metadata; the runtime
    // routine genesis_exception_return performs the validated atomic restore.
    effect.stack = M68kStackEffectKind::pop_exception_frame;
    effect.stack_width = 6U;
    effect.pc = M68kPcEffectKind::observed_exception_return;
    break;
  case M68kIrKind::write_user_stack_pointer:
    effect.resolved_source_ea = operation.source_ea;
    if (operation.source_ea.mode == M68kEaMode::address_register && operation.source_ea.reg < 8U)
      effect.user_stack_pointer_source = static_cast<AddressRegister>(operation.source_ea.reg);
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::write_status_register:
    // MOVE to SR overwrites the entire SR (CCR bits included) from the
    // decoded source operand; this layer has no register/memory model of
    // its own to resolve that source (per its documented contract, matching
    // every other selected general-whitelist kind above), so it states only
    // that CCR/SR is affected and copies the already-decoded source fact
    // through unresolved.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::read_status_register:
    // SEG-007-T116: MOVE from SR copies the current SR (word) into the decoded
    // data-alterable destination. Per the public Motorola M68000 Family PRM
    // System Control Group entry, MOVE from SR affects NO condition codes
    // (X/N/Z/V/C unchanged) -- contrast write_status_register above, which
    // sets affects_condition_codes. This layer has no register/memory model
    // to resolve the SR source itself, so it states only the destination EA,
    // size, and PC shape; the caller performs the actual read/write.
    effect.operand_size = operation.size;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = false;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::write_condition_codes:
    // SEG-007-T118: MOVE <ea>,CCR reads the decoded data source operand as a
    // word and replaces the CCR sub-field (low byte of SR) with its low-order
    // byte; the upper (system) byte of SR is untouched. Per the public Motorola
    // M68000 Family PRM System Control Group entry it DOES affect all condition
    // codes (X/N/Z/V/C are taken wholesale from the source) -- like
    // write_status_register above and unlike read_status_register. This layer
    // has no register/memory model to resolve the source itself, so it states
    // only the source EA, size, CCR effect, and PC shape; the caller performs
    // the actual read/write.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::no_operation:
    // NOP (Motorola M68000 Family PRM NOP entry): "No operation occurs ...
    // The processor state, other than the program counter, is unaffected."
    // No operand, no CCR/SR effect (affects_condition_codes stays false), no
    // source/destination EA, no memory access. The sole effect is the
    // two-byte program-counter advance past the single instruction word.
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::subtract_quick_long_d0:
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::branch_always_short:
    break;
  // SEG-007-T023: the general whitelist kinds. Each copies through its own
  // already-decoded, unresolved EA fact(s) (this layer has no memory/
  // register-file model to reuse, per its documented contract) and states
  // only the size/CCR/PC shape the contract's "Condition-code effects" table
  // requires; the caller resolves/validates/executes the EA against actual
  // register/memory state.
  case M68kIrKind::test_operand:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::compare:
  case M68kIrKind::compare_immediate:
  case M68kIrKind::compare_address:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    // CMP/CMPI/CMPA preserve X while their shared subtraction arithmetic sets
    // N/Z/V/C. A future selected subtraction form may choose from_carry.
    effect.extend_flag_policy = M68kExtendFlagPolicy::preserve;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::add:
  case M68kIrKind::add_immediate:
  case M68kIrKind::add_quick:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = operation.destination_ea.mode != M68kEaMode::address_register;
    if (operation.destination_ea.mode == M68kEaMode::address_register)
      effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::add_address:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::subtract_quick:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    // SUB/SUBI/SUBQ to Dn or memory update X from borrow.  SUBQ to An is
    // addressed below because address arithmetic leaves SR untouched.
    effect.affects_condition_codes = operation.destination_ea.mode != M68kEaMode::address_register;
    if (operation.destination_ea.mode == M68kEaMode::address_register)
      effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.extend_flag_policy = M68kExtendFlagPolicy::from_carry;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::subtract_address:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::write_move:
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::write_movea:
    // MOVEA's entire CCR/SR is "Not affected" (contract, MOVEA "Condition
    // Codes"): affects_condition_codes stays false, unlike plain MOVE.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::write_clr:
    // CLR's CCR result (N=0,Z=1,V=0,C=0, X unaffected) never depends on the
    // cleared value, so affects_condition_codes is a fixed pattern, not
    // m68k_move_result_ccr applied to a value (contract: "no observable
    // consequence" of the real-hardware read-before-write bus note).
    effect.operand_size = operation.size;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::logical_not:
    // SEG-007-T168: NOT is a genuine one-address read-modify-write (unlike
    // write_clr's write-only shape) -- both resolved_source_ea and
    // resolved_destination_ea are the SAME single destination_ea, exactly
    // matching shift_rotate_memory's one-address RMW footprint. Its CCR
    // result reuses the logical family's own formula (N/Z set from the
    // complemented result, V/C cleared), so affects_condition_codes follows
    // the same fixed pattern every logical-family kind above already uses.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.destination_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::negate_word:
    // NEG.W is the sized subtraction 0 - destination; X and C follow borrow.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.destination_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.extend_flag_policy = M68kExtendFlagPolicy::from_carry;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::multiply_signed_word:
    // SEG-007-T220: MULS.W reads the word-size source_ea and the low word of
    // destination_ea (always a Dn), and writes the full 32-bit product back
    // to that same Dn -- an asymmetric read/write width, matching ADDA's own
    // established "read narrower than the register it widens into" shape
    // (M68kIrKind::add_address above) rather than the uniform-width AND/OR/
    // EOR family. `operand_size` states the SOURCE read width (word); the
    // caller's own MULS lowering (not this typed-fact layer, which has no
    // register/memory model to compute the product with) knows the
    // destination write is always the full Dn. CCR reuses the logical
    // family's own formula (N/Z from the 32-bit product, V/C cleared, X
    // unaffected) -- affects_condition_codes follows the same fixed pattern.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::multiply_unsigned_word:
    // SEG-007-T222: MULU.W shares MULS.W's exact typed-fact shape above
    // (unsigned rather than signed evaluation is a caller/emission-layer
    // concern, not a typed-fact difference).
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word:
    // SEG-007-T222 / ADR-0037: DIVS.W/DIVU.W share the same conservative
    // "Dn may be written" typed-fact shape as every other analyzed
    // instruction (ADR-0037 Decision D: no consumer needs a conditional-
    // write effect kind). The two additive fields record only that this
    // operation may instead raise the synchronous vector-5 exception;
    // `resolved_destination_ea`/`affects_condition_codes` are unchanged from
    // the unconditional-write shape.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.may_raise_synchronous_exception = true;
    effect.exception_vector = 5U;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::load_effective_address:
    // LEA is entirely CCR-unaffected and never performs a memory access at
    // all (it only computes an address into An); affects_condition_codes
    // stays false.
    effect.resolved_source_ea = operation.source_ea;
    effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::write_swap:
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long:
    // SEG-007-T025 (Batch C, C1): SWAP/EXT.W/EXT.L read and write the same
    // Dn slot (destination_ea only; there is no separate source_ea -- see
    // M68kInstructionKind::swap doc). Their CCR result is N/Z from the
    // (correctly sized/sign-extended) written value with V/C cleared and X
    // preserved -- exactly m68k_move_result_ccr's existing contract, applied
    // by the caller/emitter to that value; no new CCR formula is introduced.
    effect.operand_size = operation.size;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::push_effective_address:
    // PEA computes source_ea's ADDRESS (never reads its contents) and pushes
    // that 32-bit address as a real A7 data-stack long. No destination_ea
    // (mirrors the existing jump_general/call_general convention); entirely
    // CCR-unaffected.
    effect.operand_size = M68kMemoryAccessWidth::long_word;
    effect.resolved_source_ea = operation.source_ea;
    effect.stack = M68kStackEffectKind::push_data_long;
    effect.stack_width = 4U;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::link_frame:
    // LINK pushes the OLD An value (source_ea carries the raw, not yet
    // sign-extended, word-displacement immediate fact), sets An to the new
    // A7, then adds the sign-extended displacement to A7. Entirely
    // CCR-unaffected.
    effect.operand_size = M68kMemoryAccessWidth::long_word;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.stack = M68kStackEffectKind::push_data_long;
    effect.stack_width = 4U;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::unlink_frame:
    // UNLK sets A7 to An, then pops the saved long back into An. Entirely
    // CCR-unaffected.
    effect.operand_size = M68kMemoryAccessWidth::long_word;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.address_register_write = static_cast<AddressRegister>(operation.destination_ea.reg);
    effect.stack = M68kStackEffectKind::pop_data_long;
    effect.stack_width = 4U;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::bit_test:
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set:
    // SEG-007-T025 (Batch C, C3): source_ea carries the bit-number fact
    // (Dn or immediate, never memory); destination_ea carries the Dn-or-
    // memory destination whose mode determines operand_size (already
    // recorded in operation.size at decode time: long_word for Dn, byte for
    // memory). CCR is affected (Z only, from the ORIGINAL tested bit -- see
    // m68k_bit_test_ccr) for all four; only bit_test never writes
    // destination_ea (read-only), which the emitter/caller distinguishes by
    // operation.kind, exactly like compare vs. add/subtract already do
    // through this same effect shape.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::general_branch: {
    // SEG-007-T025 (Batch C, C4a): BRA (operation.condition == always) and
    // every selected Bcc condition. Entirely CCR-unaffected (Bcc READS SR,
    // never modifies it). `direct_target` always carries the verified
    // static target -- true information regardless of condition, computed
    // through the one shared m68k_branch_target formula -- but this effect
    // layer cannot itself decide whether that target or the fallthrough is
    // actually taken (that depends on the runtime SR, which this layer has
    // no model of); the caller (generated-C emission, and separately static
    // discovery's own conservative both-edges policy for a genuine Bcc)
    // owns that decision.
    effect.resolved_source_ea = operation.source_ea;
    const auto raw = operation.source_ea.immediate_value;
    const auto signed_disp = operation.size == M68kMemoryAccessWidth::byte
                                  ? static_cast<std::int32_t>(static_cast<std::int8_t>(raw))
                                  : static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
    effect.pc = M68kPcEffectKind::direct_target;
    effect.direct_target = m68k_branch_target(operation.provenance.source.address.value, signed_disp);
    break;
  }
  case M68kIrKind::bsr_call: {
    // SEG-007-T025 (Batch C, C4b): BSR is a direct call sharing the
    // project's one call/continuation/callee identity owner with JSR
    // (contract: "reuse the existing JSR static call identity"; see
    // build_m68k_static_call and friends in m68k_pipeline_frontend.cpp),
    // never a second BSR-specific return-identity formula. Its target is
    // PC-relative -- computed via the exact same shared m68k_branch_target
    // formula BRA/Bcc use above, NOT m68k_is_statically_foldable_control_ea
    // (that check is for JMP/JSR's absolute/pc-relative EA target, which
    // BSR's displacement immediate is not). Entirely CCR-unaffected.
    effect.resolved_source_ea = operation.source_ea;
    const auto raw = operation.source_ea.immediate_value;
    const auto signed_disp = operation.size == M68kMemoryAccessWidth::byte
                                  ? static_cast<std::int32_t>(static_cast<std::int8_t>(raw))
                                  : static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
    effect.pc = M68kPcEffectKind::direct_target;
    effect.direct_target = m68k_branch_target(operation.provenance.source.address.value, signed_disp);
    effect.stack = M68kStackEffectKind::push_static_continuation;
    effect.stack_width = 4U;
    break;
  }
  case M68kIrKind::dbcc_loop: {
    // SEG-007-T025 (Batch C, C4c): DBcc reads and never modifies SR
    // (contract: "DBcc shares condition owner" -- "DBcc itself changes no
    // SR bits"), so affects_condition_codes stays false (default). The
    // taken target uses the same shared m68k_branch_target formula every
    // other selected branch form uses -- descriptive here only, exactly
    // like general_branch above: this effect layer cannot itself decide
    // condition-vs-decrement-vs-expiration (data-dependent, no register/SR
    // model to reuse), which the caller (emission) owns.
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    const auto raw = operation.source_ea.immediate_value;
    const auto signed_disp = static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
    effect.pc = M68kPcEffectKind::direct_target;
    effect.direct_target = m68k_branch_target(operation.provenance.source.address.value, signed_disp);
    break;
  }
  case M68kIrKind::movem_transfer:
    // SEG-007-T025 (Batch C, C5a): entirely CCR/SR-unaffected (contract:
    // "MOVEM does not alter condition codes"). This layer has no memory/
    // register-file model to reuse (per its documented contract) and
    // therefore cannot itself enumerate the individual register transfers a
    // selected mask requests -- that is the emitter's job, via the one
    // shared m68k_movem_transfer_order owner. `resolved_source_ea`/
    // `resolved_destination_ea` copy through whichever position carries the
    // memory operand (memory_to_registers: source_ea; registers_to_memory:
    // destination_ea), matching every other selected memory-affecting kind's
    // convention above; the other position stays `unused` and is copied
    // through unread.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::shift_rotate_register:
    // SEG-007-T025 (Batch C, C6): entirely register-only (Dn destination,
    // Dn-or-immediate count source) -- no memory access at all, so unlike
    // MOVEM this kind never populates a resolved memory EA. CCR IS affected
    // (contract: "one shared register shift/rotate semantic owner"), but
    // this layer has no register-file model of its own to compute the
    // actual N/Z/V/C/X facts (that is m68k_evaluate_shift_rotate's job, via
    // the caller-supplied original destination/count/current-X); it states
    // only that CCR is affected and copies the already-decoded count/
    // destination facts through unresolved.
    effect.operand_size = operation.size;
    effect.resolved_source_ea = operation.source_ea;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::shift_rotate_memory:
    // SEG-007-T025 (Batch C, C7a): a genuine one-address memory
    // read-modify-write (`destination_ea` is one of the six
    // `m68k_ea_memory_alterable` modes, never Dn) -- `source_ea` is unused
    // (the count is architecturally fixed to 1, never a decoded operand).
    // CCR is affected exactly like the register form, through the SAME
    // shared semantic owner (`M68kShiftRotateSpecification`); this layer
    // still has no register/memory model of its own, so it states only that
    // CCR is affected and copies the already-decoded destination fact
    // through unresolved.
    effect.operand_size = operation.size;
    effect.resolved_destination_ea = operation.destination_ea;
    effect.affects_condition_codes = true;
    effect.pc = M68kPcEffectKind::advance;
    effect.pc_delta = operation.provenance.length.value;
    break;
  case M68kIrKind::jump_general:
  case M68kIrKind::call_general: {
    // JMP/JSR's target is statically foldable exactly when its EA mode is
    // one of the three compile-time-constant control-addressing forms
    // (absolute.w/absolute.l/d16(PC)); (An)/d16(An) are genuinely
    // runtime-only and have no direct_target here (decision: the closest-
    // fitting existing DirectFlowDiagnostic category is the caller's job to
    // apply, per the batch contract's "JSR scope decision"). Both kinds are
    // entirely CCR-unaffected.
    effect.resolved_source_ea = operation.source_ea;
    const auto &ea = operation.source_ea;
    if (operation.kind == M68kIrKind::call_general) {
      effect.stack = M68kStackEffectKind::push_static_continuation;
      effect.stack_width = 4U;
    }
    if (m68k_is_statically_foldable_control_ea(ea)) {
      effect.pc = M68kPcEffectKind::direct_target;
      // This is the decoded-EA-to-Genesis-program-target handoff.  Keep the
      // decoded sign-extended absolute-word value intact on `ea`; only the
      // target consumed by static control flow uses Genesis canonicalization.
      effect.direct_target = m68k_canonical_ea_address(ea);
    }
    break;
  }
  }
  // This is deliberately derived from the lifted operation, not from an
  // analysis-local opcode table.  Operations outside the selected lifted set
  // stay incomplete and are consequently unusable by retained proofs.
  const auto note_ea_auto_update = [&](const M68kEffectiveAddress &ea) {
    if ((ea.mode == M68kEaMode::address_postinc || ea.mode == M68kEaMode::address_predec) && ea.reg < 8U)
      effect.address_register_write_mask |= static_cast<std::uint8_t>(1U << ea.reg);
  };
  note_ea_auto_update(operation.source_ea);
  note_ea_auto_update(operation.destination_ea);
  if (effect.register_write && static_cast<unsigned>(effect.register_write->reg) < 8U)
    effect.data_register_write_mask |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(effect.register_write->reg));
  if (operation.kind == M68kIrKind::subtract_quick_long_d0)
    effect.data_register_write_mask |= UINT8_C(0x01);
  if (effect.address_register_write && static_cast<unsigned>(*effect.address_register_write) < 8U)
    effect.address_register_write_mask |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(*effect.address_register_write));
  if ((operation.destination_ea.mode == M68kEaMode::data_register) && operation.destination_ea.reg < 8U &&
      operation.kind != M68kIrKind::compare && operation.kind != M68kIrKind::compare_immediate &&
      operation.kind != M68kIrKind::compare_address && operation.kind != M68kIrKind::test_operand &&
      operation.kind != M68kIrKind::bit_test)
    effect.data_register_write_mask |= static_cast<std::uint8_t>(1U << operation.destination_ea.reg);
  // Only the narrow operation subset whose D/A effects are exhaustively
  // represented above advertises completeness.  An absent claim is a reject,
  // never an assertion that the operation writes no registers.
  // SEG-007-T150 (ADR-0016): ordinary `add` joins this whitelist because its
  // complete D/A write footprint is already exhaustively represented above
  // -- an explicit Dn destination write (data_register_write_mask, general
  // rule just above), an address-register direct destination write
  // (address_register_write/-mask, the `add`/`add_quick`/`add_immediate`
  // case above), and any decoded EA postincrement/predecrement auto-update
  // on either operand (note_ea_auto_update, unconditional for every kind).
  // No second, ADD-specific effect model is introduced; this only lets an
  // already-truthful footprint advertise itself as complete.
  //
  // SEG-007-T155 (ADR-0017, Scope item 1a): ordinary `write_move` joins this
  // whitelist for the same reason -- its complete D/A write footprint is
  // already exhaustively represented above and no `write_move`-specific effect
  // model is added. MOVE's only register writes are (a) an explicit Dn
  // destination write when `destination_ea` is `data_register`
  // (data_register_write_mask, the general rule just above -- `write_move` is
  // deliberately not in the compare/test exclusion list there), and (b) a
  // decoded EA postincrement/predecrement auto-update on either operand
  // (note_ea_auto_update, unconditional for every kind). MOVE never writes an
  // address register directly: `MOVE <ea>,An` decodes as `write_movea`, a
  // separate kind that already carries its own `address_register_write`. Plain
  // `write_move` therefore has no unrepresented register effect, so this only
  // lets an already-truthful footprint advertise itself as complete (required
  // by ADR-0017 so an auto-updating output-cursor MOVE writer can be an
  // admitted advancing operation of a data-transform progress proof).
  effect.register_write_footprint_complete = operation.kind == M68kIrKind::no_operation ||
      operation.kind == M68kIrKind::add_quick || operation.kind == M68kIrKind::add_immediate ||
      operation.kind == M68kIrKind::add ||
      operation.kind == M68kIrKind::write_move ||
      operation.kind == M68kIrKind::compare || operation.kind == M68kIrKind::compare_address ||
      operation.kind == M68kIrKind::general_branch;
  if (operation.kind == M68kIrKind::push_effective_address || operation.kind == M68kIrKind::return_from_subroutine ||
      operation.kind == M68kIrKind::link_frame || operation.kind == M68kIrKind::unlink_frame ||
      operation.kind == M68kIrKind::bsr_call || operation.kind == M68kIrKind::call_general)
    effect.address_register_write_mask |= UINT8_C(0x80);
  return effect;
}


} // namespace segarecomp
