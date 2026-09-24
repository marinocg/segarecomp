#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/address_space.hpp"
#include "segarecomp/rom.hpp"

namespace segarecomp {
namespace {
std::string hex(std::uint32_t value, unsigned width) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
  return out.str();
}
}  // namespace
namespace {
std::uint32_t read_be32(const std::vector<std::uint8_t> &bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
}
std::vector<std::uint8_t> be32(std::uint32_t value) {
  return {static_cast<std::uint8_t>(value >> 24U), static_cast<std::uint8_t>(value >> 16U),
          static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value)};
}
bool even(std::uint32_t address) { return (address & 1U) == 0U; }
// The synthetic stack interval a JSR push or RTS pop of `width` bytes
// occupies relative to the current A7, consuming M68kOperationEffect's
// declared stack_width rather than an independently hardcoded constant.
// Only the startup profile has a stack model to apply this to; it does not
// redefine what width a selected operation requests, only computes the
// interval that width implies. Precondition: no unsigned wraparound (the
// caller's own alignment/range checks establish this before calling).
struct M68kStackInterval { std::uint32_t begin{}; std::uint32_t end{}; };
M68kStackInterval m68k_startup_stack_push_interval(std::uint32_t a7, std::uint32_t width) noexcept {
  return {a7 - width, a7};
}
M68kStackInterval m68k_startup_stack_pop_interval(std::uint32_t a7, std::uint32_t width) noexcept {
  return {a7, a7 + width};
}
// A malformed FrontendAnalysis (most plausibly hand-constructed by a test or
// a future caller) must fail closed rather than let execute_m68k_frontend_startup
// index analysis.ir[i] against an inconsistent analysis.decoded[i]. This is a
// small compatibility predicate independent of lift_m68k_instruction: it
// checks that a decoded/lifted pair could only have come from the same
// verified instruction, not that lift_m68k_instruction would reproduce it
// (calling lift_m68k_instruction again here would just be a second semantic
// path to trust, not a validation of the first one). It also requires the
// decoded/lifted operand and extension to agree, since several startup
// executor branches source their memory address/call target from the
// lifted side's extension while StartupInstruction::operand reports the
// decoded side's.
bool m68k_startup_analysis_pair_consistent(const M68kDecodedInstruction &decoded,
                                            const M68kIrOperation &lifted) noexcept {
  if (decoded.provenance.source.address.space != lifted.provenance.source.address.space) return false;
  if (decoded.provenance.source.address.value != lifted.provenance.source.address.value) return false;
  if (decoded.provenance.source.image_offset.value != lifted.provenance.source.image_offset.value) return false;
  if (decoded.provenance.length.value != lifted.provenance.length.value) return false;
  if (decoded.operand != lifted.operand) return false;
  if (decoded.extension != lifted.extension) return false;
  switch (decoded.kind) {
  case M68kInstructionKind::moveq: return lifted.kind == M68kIrKind::write_moveq;
  case M68kInstructionKind::subq_l_1_d0: return lifted.kind == M68kIrKind::subtract_quick_long_d0;
  case M68kInstructionKind::bne_short: return lifted.kind == M68kIrKind::branch_ne_short;
  case M68kInstructionKind::bra_short: return lifted.kind == M68kIrKind::branch_always_short;
  case M68kInstructionKind::rts: return lifted.kind == M68kIrKind::return_from_subroutine;
  // SEG-007-T023: execute_m68k_frontend_startup does not accept any of these
  // general whitelist kinds (its switch below has no case for them and
  // falls to the defensive `invalid_startup_analysis` default), so this
  // consistency check exists only to keep the exhaustive switch compiling
  // and to fail closed rather than silently accept a malformed pairing if a
  // future caller ever widens execute_m68k_frontend_startup itself.
  case M68kInstructionKind::tst: return lifted.kind == M68kIrKind::test_operand;
  case M68kInstructionKind::cmp: return lifted.kind == M68kIrKind::compare;
  case M68kInstructionKind::cmpi: return lifted.kind == M68kIrKind::compare_immediate;
  case M68kInstructionKind::cmpa: return lifted.kind == M68kIrKind::compare_address;
  case M68kInstructionKind::add: return lifted.kind == M68kIrKind::add;
  case M68kInstructionKind::adda: return lifted.kind == M68kIrKind::add_address;
  case M68kInstructionKind::addi: return lifted.kind == M68kIrKind::add_immediate;
  case M68kInstructionKind::addq: return lifted.kind == M68kIrKind::add_quick;
  case M68kInstructionKind::sub: return lifted.kind == M68kIrKind::subtract;
  case M68kInstructionKind::suba: return lifted.kind == M68kIrKind::subtract_address;
  case M68kInstructionKind::subi: return lifted.kind == M68kIrKind::subtract_immediate;
  case M68kInstructionKind::subq: return lifted.kind == M68kIrKind::subtract_quick;
  case M68kInstructionKind::logical_and: return lifted.kind == M68kIrKind::logical_and;
  case M68kInstructionKind::andi: return lifted.kind == M68kIrKind::logical_and_immediate;
  case M68kInstructionKind::logical_or: return lifted.kind == M68kIrKind::logical_or;
  case M68kInstructionKind::ori: return lifted.kind == M68kIrKind::logical_or_immediate;
  case M68kInstructionKind::eor: return lifted.kind == M68kIrKind::exclusive_or;
  case M68kInstructionKind::eori: return lifted.kind == M68kIrKind::exclusive_or_immediate;
  case M68kInstructionKind::move: return lifted.kind == M68kIrKind::write_move;
  case M68kInstructionKind::movea: return lifted.kind == M68kIrKind::write_movea;
  case M68kInstructionKind::clr: return lifted.kind == M68kIrKind::write_clr;
  case M68kInstructionKind::not_operand: return lifted.kind == M68kIrKind::logical_not;
  case M68kInstructionKind::negate_word: return lifted.kind == M68kIrKind::negate_word;
  case M68kInstructionKind::negate_extended: return lifted.kind == M68kIrKind::negate_extended;
  case M68kInstructionKind::add_extended: return lifted.kind == M68kIrKind::add_extended;
  case M68kInstructionKind::subtract_extended: return lifted.kind == M68kIrKind::subtract_extended;
  case M68kInstructionKind::compare_memory: return lifted.kind == M68kIrKind::compare_memory;
  case M68kInstructionKind::add_decimal: return lifted.kind == M68kIrKind::add_decimal;
  case M68kInstructionKind::subtract_decimal: return lifted.kind == M68kIrKind::subtract_decimal;
  case M68kInstructionKind::negate_decimal: return lifted.kind == M68kIrKind::negate_decimal;
  case M68kInstructionKind::exchange_registers: return lifted.kind == M68kIrKind::exchange_registers;
  case M68kInstructionKind::movep: return lifted.kind == M68kIrKind::movep_transfer;
  case M68kInstructionKind::set_conditional: return lifted.kind == M68kIrKind::set_conditional;
  case M68kInstructionKind::test_and_set: return lifted.kind == M68kIrKind::test_and_set;
  case M68kInstructionKind::lea: return lifted.kind == M68kIrKind::load_effective_address;
  case M68kInstructionKind::jmp: return lifted.kind == M68kIrKind::jump_general;
  case M68kInstructionKind::jsr: return lifted.kind == M68kIrKind::call_general;
  // SEG-007-T025 (Batch C, C1): execute_m68k_frontend_startup accepts none
  // of these either, for the same reason as the T023 kinds above.
  case M68kInstructionKind::swap: return lifted.kind == M68kIrKind::write_swap;
  case M68kInstructionKind::ext_w: return lifted.kind == M68kIrKind::sign_extend_word;
  case M68kInstructionKind::ext_l: return lifted.kind == M68kIrKind::sign_extend_long;
  case M68kInstructionKind::pea: return lifted.kind == M68kIrKind::push_effective_address;
  case M68kInstructionKind::link: return lifted.kind == M68kIrKind::link_frame;
  case M68kInstructionKind::unlk: return lifted.kind == M68kIrKind::unlink_frame;
  case M68kInstructionKind::btst: return lifted.kind == M68kIrKind::bit_test;
  case M68kInstructionKind::bchg: return lifted.kind == M68kIrKind::bit_change;
  case M68kInstructionKind::bclr: return lifted.kind == M68kIrKind::bit_clear;
  case M68kInstructionKind::bset: return lifted.kind == M68kIrKind::bit_set;
  case M68kInstructionKind::branch: return lifted.kind == M68kIrKind::general_branch;
  case M68kInstructionKind::bsr: return lifted.kind == M68kIrKind::bsr_call;
  case M68kInstructionKind::dbcc: return lifted.kind == M68kIrKind::dbcc_loop;
  // SEG-007-T025 (Batch C, C5): execute_m68k_frontend_startup accepts none
  // of these either, for the same reason as every other Batch-C kind above.
  case M68kInstructionKind::movem: return lifted.kind == M68kIrKind::movem_transfer;
  // SEG-007-T025 (Batch C, C6): execute_m68k_frontend_startup accepts none
  // of these either, for the same reason as every other Batch-C kind above.
  // SEG-007-T025 (Batch C, C7a): either lowering is a valid pairing for this
  // one decoded kind -- see the lift-time register/memory distinction above.
  case M68kInstructionKind::shift_rotate:
    return lifted.kind == M68kIrKind::shift_rotate_register || lifted.kind == M68kIrKind::shift_rotate_memory;
  case M68kInstructionKind::move_an_to_usp: return lifted.kind == M68kIrKind::write_user_stack_pointer;
  case M68kInstructionKind::move_to_sr: return lifted.kind == M68kIrKind::write_status_register;
  // SEG-007-T114: NOP. execute_m68k_frontend_startup (the genesis_rom_startup
  // fixed-profile executor) does not accept NOP either -- its switch below has
  // no case for it and falls to the defensive `invalid_startup_analysis`
  // default -- so this pairing check exists only to keep the exhaustive switch
  // compiling and to fail closed on a malformed pairing.
  case M68kInstructionKind::nop: return lifted.kind == M68kIrKind::no_operation;
  // SEG-007-T116: MOVE from SR. execute_m68k_frontend_startup (the
  // genesis_rom_startup fixed-profile executor) does not accept MOVE from SR
  // either -- its switch below has no case for it and falls to the defensive
  // `invalid_startup_analysis` default -- so this pairing check exists only to
  // keep the exhaustive switch compiling and to fail closed on a malformed
  // pairing.
  case M68kInstructionKind::move_from_sr: return lifted.kind == M68kIrKind::read_status_register;
  // SEG-007-T118: MOVE <ea>,CCR. execute_m68k_frontend_startup (the
  // genesis_rom_startup fixed-profile executor) does not accept MOVE to CCR
  // either -- its switch below has no case for it and falls to the defensive
  // `invalid_startup_analysis` default -- so this pairing check exists only to
  // keep the exhaustive switch compiling and to fail closed on a malformed
  // pairing.
  case M68kInstructionKind::move_to_ccr: return lifted.kind == M68kIrKind::write_condition_codes;
  // SEG-007-T047 / ADR-0020 §9: RTE is decoded only under the general_startup
  // policy, never the genesis_rom_startup fixed profile -- its executor switch
  // below has no case for it and falls to the defensive `invalid_startup_analysis`
  // default -- so this pairing check exists only to keep the exhaustive switch
  // compiling and to fail closed on a malformed pairing.
  case M68kInstructionKind::rte: return lifted.kind == M68kIrKind::return_from_exception;
  // SEG-007-T220: MULS.W. execute_m68k_frontend_startup (the
  // genesis_rom_startup fixed-profile executor) does not accept MULS.W
  // either -- its switch below has no case for it and falls to the
  // defensive `invalid_startup_analysis` default -- so this pairing check
  // exists only to keep the exhaustive switch compiling and to fail closed
  // on a malformed pairing.
  case M68kInstructionKind::multiply_signed_word: return lifted.kind == M68kIrKind::multiply_signed_word;
  // SEG-007-T222: MULU.W/DIVS.W/DIVU.W -- same defensive pairing-only
  // treatment as MULS.W above; this fixed-profile executor accepts none of
  // them.
  case M68kInstructionKind::multiply_unsigned_word: return lifted.kind == M68kIrKind::multiply_unsigned_word;
  case M68kInstructionKind::divide_signed_word: return lifted.kind == M68kIrKind::divide_signed_word;
  case M68kInstructionKind::divide_unsigned_word: return lifted.kind == M68kIrKind::divide_unsigned_word;
  }
  return false;
}
}

// The sole owner of the startup profile's operation state/memory/control
// effects: MOVEQ register write, absolute-long RAM store/load, direct-call
// stack push/dispatch, and static-return stack pop/validate/dispatch. Every
// register-result CCR update it performs reuses m68k_move_result_ccr (shared
// with SEG-003's execute_op); every absolute-operand address check reuses
// m68k_startup_absolute_operand_alignment/m68k_startup_ram_operand_in_range/
// m68k_startup_ram_offset (shared with the static decode-time validation in
// analyze_startup_profile); and the call/return identity it defensively
// re-checks is read directly from the one record discover_m68k_static_call_return
// (T006) already established in `analysis.static_frames`, never recomputed
// by an independent formula. It consumes `analysis.decoded`/`analysis.ir`
// directly by index: there is no second, startup-only copy of a decoded or
// lifted operation for it to diverge from.
StartupResult execute_m68k_frontend_startup(const FrontendAnalysis &analysis, StartupState initial,
                                            const StartupExecutionTestContext &test_context) {
  StartupExecution out{};
  out.initial_state = initial;
  if (analysis.profile != M68kFrontendProfile::genesis_rom_startup || !analysis.startup_ingress) {
    StartupFailure failure{}; failure.category = "invalid_startup_analysis"; failure.attempted_source = initial.pc; return failure;
  }
  if (analysis.decoded.size() != analysis.ir.size()) {
    StartupFailure failure{}; failure.category = "invalid_startup_analysis"; failure.attempted_source = initial.pc; return failure;
  }
  for (std::size_t i = 0; i < analysis.decoded.size(); ++i) {
    if (!m68k_startup_analysis_pair_consistent(analysis.decoded[i], analysis.ir[i])) {
      StartupFailure failure{}; failure.category = "invalid_startup_analysis"; failure.attempted_source = initial.pc; return failure;
    }
  }
  std::vector<std::uint8_t> ram(0x10000U);
  for (const auto &[address, bytes] : test_context.ram_bytes) {
    if (address < m68k_startup_ram_begin || bytes.size() > ram.size() ||
        static_cast<std::uint64_t>(m68k_startup_ram_offset(address)) + bytes.size() > ram.size()) {
      StartupFailure failure{}; failure.category = "invalid_startup_analysis"; failure.attempted_source = initial.pc; return failure;
    }
    std::copy(bytes.begin(), bytes.end(), ram.begin() + static_cast<std::ptrdiff_t>(m68k_startup_ram_offset(address)));
  }
  std::vector<std::uint32_t> changed;
  std::vector<M68kStaticCall> frames = test_context.static_call_frames;
  const auto same_call = [](const M68kStaticCall &left, const M68kStaticCall &right) {
    return left.caller.source.address.space == right.caller.source.address.space &&
           left.caller.source.address.value == right.caller.source.address.value &&
           left.caller.source.image_offset.value == right.caller.source.image_offset.value &&
           left.continuation.space == right.continuation.space &&
           left.continuation.value == right.continuation.value && left.callee.space == right.callee.space &&
           left.callee.value == right.callee.value;
  };
  const auto fail = [&](std::string category, std::uint64_t ordinal, const M68kDecodedInstruction &decoded,
                        const std::vector<std::uint8_t> &raw_bytes) -> StartupFailure {
    StartupFailure failure{}; failure.ordinal = ordinal; failure.category = std::move(category);
    failure.attempted_source = decoded.provenance.source.address;
    failure.image_offset = decoded.provenance.source.image_offset;
    failure.complete_instruction = decoded.provenance;
    failure.complete_instruction_raw_bytes = raw_bytes;
    failure.accesses = out.accesses; failure.boundaries = out.boundaries;
    failure.retained_boundary_ordinal = out.boundaries.back().ordinal;
    if (!out.accesses.empty()) failure.bus_through_ordinal = out.accesses.back().ordinal;
    return failure;
  };
  out.boundaries.push_back({0U, initial, {}, std::nullopt, "continue"});
  for (std::size_t ordinal = 0; ordinal < analysis.decoded.size(); ++ordinal) {
    const auto &decoded = analysis.decoded[ordinal];
    const auto &ir = analysis.ir[ordinal];
    StartupInstruction instruction{};
    instruction.provenance = decoded.provenance; instruction.raw_bytes = decoded.raw_bytes;
    instruction.decoded = decoded; instruction.operation = ir; instruction.operand = decoded.extension;
    switch (decoded.kind) {
    case M68kInstructionKind::moveq: instruction.kind = StartupInstructionKind::moveq_d0; break;
    case M68kInstructionKind::move:
      if (decoded.source_ea.mode == M68kEaMode::data_register && decoded.source_ea.reg == 0U)
        instruction.kind = StartupInstructionKind::move_l_d0_absolute_long;
      else instruction.kind = StartupInstructionKind::move_l_absolute_long_d1;
      break;
    case M68kInstructionKind::jsr: instruction.kind = StartupInstructionKind::jsr_absolute_long; break;
    case M68kInstructionKind::rts: instruction.kind = StartupInstructionKind::rts; break;
    default: { StartupFailure failure{}; failure.category="invalid_startup_analysis"; failure.attempted_source=initial.pc; return failure; }
    }
    out.instructions.push_back(instruction);
    out.accesses.push_back({static_cast<std::uint64_t>(out.accesses.size()), StartupBusKind::instruction_read,
                            decoded.provenance.source.address, decoded.raw_bytes, "raw_cartridge_rom", decoded.provenance});
    if (ir.kind == M68kIrKind::write_moveq) {
      const auto effect = m68k_operation_effect(ir);
      initial.d[static_cast<unsigned>(effect.register_write->reg)] = effect.register_write->value;
      initial.sr = m68k_move_result_ccr(initial.sr, effect.register_write->value); initial.pc.value += effect.pc_delta;
    } else if (ir.kind == M68kIrKind::write_move && decoded.source_ea.mode == M68kEaMode::data_register &&
               decoded.source_ea.reg == 0U && decoded.destination_ea.mode == M68kEaMode::absolute_long) {
      const auto effect = m68k_operation_effect(ir);
      const auto address = decoded.destination_ea.absolute_address;
      if (const auto misaligned = m68k_startup_absolute_operand_alignment(address)) {
        auto f=fail(misaligned==DirectFlowDiagnostic::effective_address_not_24bit?"effective_address_not_24bit":"odd_effective_address",ordinal,decoded,decoded.raw_bytes);f.effective_address=address;return f;
      }
      if (!m68k_startup_ram_operand_in_range(address)) { auto f=fail("unmapped_data_access",ordinal,decoded,decoded.raw_bytes);f.effective_address=address;return f; }
      const auto offset = m68k_startup_ram_offset(address); const auto register_value = initial.d[0]; const auto bytes = be32(register_value); std::copy(bytes.begin(), bytes.end(), ram.begin() + static_cast<std::ptrdiff_t>(offset)); changed.push_back(address);
      out.accesses.push_back({static_cast<std::uint64_t>(out.accesses.size()), StartupBusKind::data_write, {{}, address}, bytes, "synthetic_work_ram", decoded.provenance});
      if (effect.affects_condition_codes) initial.sr = m68k_move_result_ccr(initial.sr, register_value);
      initial.pc.value += effect.pc_delta;
    } else if (ir.kind == M68kIrKind::call_general) {
      // The static frontend has already validated the direct target and
      // discover_m68k_static_call_return has already established the one
      // call identity in analysis.static_frames. Keep a defensive
      // fail-closed guard here so a forged analysis cannot dispatch, but
      // source the continuation from that single discovered identity
      // rather than recomputing its formula independently.
      const auto effect = m68k_operation_effect(ir);
      // Structural belt-and-suspenders check: the JSR IR kind must always
      // produce this exact shared effect shape. Not reachable via any
      // current fixture; guards only against a future inconsistency between
      // ir.kind and m68k_operation_effect.
      if (effect.stack != M68kStackEffectKind::push_static_continuation || effect.stack_width != 4U ||
          effect.pc != M68kPcEffectKind::direct_target)
        return fail("invalid_startup_analysis", ordinal, decoded, decoded.raw_bytes);
      if (m68k_startup_absolute_operand_alignment(effect.direct_target)) return fail("invalid_startup_analysis",ordinal,decoded,decoded.raw_bytes);
      if (analysis.static_frames.empty()) return fail("invalid_startup_analysis", ordinal, decoded, decoded.raw_bytes);
      const auto &discovered_call = analysis.static_frames.front().call;
      if (discovered_call.caller.source.address.space != decoded.provenance.source.address.space ||
          discovered_call.caller.source.address.value != decoded.provenance.source.address.value ||
          discovered_call.callee.value != effect.direct_target)
        return fail("invalid_startup_analysis", ordinal, decoded, decoded.raw_bytes);
      const auto continuation = discovered_call.continuation.value;
      // Alignment has precedence, but do not form a wrapping attempted range.
      // The subtraction is performed only after its unsigned precondition has
      // been established, so rejected underflow retains the pre-call state.
      if (!even(initial.a[7])) {
        auto f=fail("invalid_stack_alignment",ordinal,decoded,decoded.raw_bytes); f.a7=initial.a[7];
        if (initial.a[7] >= effect.stack_width) { const auto interval = m68k_startup_stack_push_interval(initial.a[7], effect.stack_width); f.attempted_range={{interval.begin,interval.end}}; f.effective_address=interval.begin; }
        f.expected_continuation=continuation; return f;
      }
      if (initial.a[7] < effect.stack_width) { auto f=fail("invalid_stack_range",ordinal,decoded,decoded.raw_bytes);f.a7=initial.a[7];f.expected_continuation=continuation;return f; }
      const auto push_interval = m68k_startup_stack_push_interval(initial.a[7], effect.stack_width);
      const auto stack = push_interval.begin;
      if (!m68k_startup_ram_operand_in_range(stack)) { auto f=fail("invalid_stack_range",ordinal,decoded,decoded.raw_bytes);f.a7=initial.a[7];f.attempted_range={{stack,push_interval.end}};f.effective_address=stack;f.expected_continuation=continuation;return f; }
      const auto bytes = be32(continuation); std::copy(bytes.begin(), bytes.end(), ram.begin() + static_cast<std::ptrdiff_t>(m68k_startup_ram_offset(stack))); changed.push_back(stack);
      out.accesses.push_back({static_cast<std::uint64_t>(out.accesses.size()), StartupBusKind::stack_write, {{}, stack}, bytes, "synthetic_work_ram", decoded.provenance});
      out.calls.push_back(discovered_call); frames.push_back(discovered_call); initial.a[7]=stack; initial.pc.value=effect.direct_target;
    } else if (ir.kind == M68kIrKind::return_from_subroutine) {
      // As with JSR, preserve alignment precedence, then prove the interval
      // endpoint before recording it.  No failed RTS may wrap A7 or create a
      // fabricated range record. The actual pop/validate/dispatch logic
      // below depends on runtime A7/RAM/frame state the effect layer cannot
      // see (the observed return value is a real RAM read, validated
      // against the discovered frame/edge below), so only the stack width
      // and structural shape are sourced from the shared effect.
      const auto effect = m68k_operation_effect(ir);
      // Structural belt-and-suspenders check: the RTS IR kind must always
      // produce this exact shared effect shape. Not reachable via any
      // current fixture; guards only against a future inconsistency between
      // ir.kind and m68k_operation_effect.
      if (effect.stack != M68kStackEffectKind::pop_static_return || effect.stack_width != 4U ||
          effect.pc != M68kPcEffectKind::observed_stack_return)
        return fail("invalid_startup_analysis", ordinal, decoded, decoded.raw_bytes);
      if (!even(initial.a[7])) {
        auto f=fail("invalid_stack_alignment",ordinal,decoded,decoded.raw_bytes); f.a7=initial.a[7];
        if (initial.a[7] <= std::numeric_limits<std::uint32_t>::max() - effect.stack_width)
          f.attempted_range={{initial.a[7],static_cast<std::uint32_t>(initial.a[7]+effect.stack_width)}};
        f.effective_address=initial.a[7]; return f;
      }
      if (initial.a[7] > std::numeric_limits<std::uint32_t>::max() - effect.stack_width) {
        auto f=fail("invalid_stack_range",ordinal,decoded,decoded.raw_bytes);f.a7=initial.a[7];f.effective_address=initial.a[7];return f;
      }
      const auto pop_interval = m68k_startup_stack_pop_interval(initial.a[7], effect.stack_width);
      const auto stack_end = pop_interval.end;
      if (!m68k_startup_ram_operand_in_range(initial.a[7])) { auto f=fail("invalid_stack_range",ordinal,decoded,decoded.raw_bytes);f.a7=initial.a[7];f.attempted_range={{initial.a[7],stack_end}};f.effective_address=initial.a[7];return f; }
      const std::vector<std::uint8_t> bytes(ram.begin() + static_cast<std::ptrdiff_t>(m68k_startup_ram_offset(initial.a[7])), ram.begin() + static_cast<std::ptrdiff_t>(m68k_startup_ram_offset(initial.a[7]) + effect.stack_width));
      out.accesses.push_back({static_cast<std::uint64_t>(out.accesses.size()), StartupBusKind::stack_read, {{}, initial.a[7]}, bytes, "synthetic_work_ram", decoded.provenance});
      const auto observed = read_be32(bytes);
      const auto expected = std::find_if(analysis.static_edges.begin(), analysis.static_edges.end(), [&](const M68kStaticEdge &edge) {
        return edge.kind == M68kStaticEdgeKind::return_to_continuation &&
               edge.source_instruction.source.address.space == decoded.provenance.source.address.space &&
               edge.source_instruction.source.address.value == decoded.provenance.source.address.value;
      });
      if (frames.empty() || expected == analysis.static_edges.end() || !expected->call ||
          !same_call(frames.back(), *expected->call)) { auto f=fail("return_context_missing",ordinal,decoded,decoded.raw_bytes);f.a7=initial.a[7];f.attempted_range={{initial.a[7],stack_end}};f.effective_address=initial.a[7];return f; }
      const auto frame = frames.back();
      if (observed != frame.continuation.value) { auto f=fail("return_target_mismatch",ordinal,decoded,decoded.raw_bytes);f.a7=initial.a[7];f.attempted_range={{initial.a[7],stack_end}};f.effective_address=initial.a[7];f.call=frame;f.expected_continuation=frame.continuation.value;f.observed_return_target=observed;return f; }
      out.returns.push_back({decoded.provenance, {{}, observed}, frame}); frames.pop_back(); initial.a[7] = stack_end; initial.pc.value=observed;
    } else if (ir.kind == M68kIrKind::write_move && decoded.source_ea.mode == M68kEaMode::absolute_long &&
               decoded.destination_ea.mode == M68kEaMode::data_register && decoded.destination_ea.reg == 1U) {
      const auto effect = m68k_operation_effect(ir);
      const auto address = decoded.source_ea.absolute_address;
      if (const auto misaligned = m68k_startup_absolute_operand_alignment(address)) {
        auto f=fail(misaligned==DirectFlowDiagnostic::effective_address_not_24bit?"effective_address_not_24bit":"odd_effective_address",ordinal,decoded,decoded.raw_bytes);f.effective_address=address;return f;
      }
      if (!m68k_startup_ram_operand_in_range(address)) { auto f=fail("unmapped_data_access",ordinal,decoded,decoded.raw_bytes);f.effective_address=address;return f; }
      const auto offset=m68k_startup_ram_offset(address); initial.d[1]=read_be32({ram.begin()+static_cast<std::ptrdiff_t>(offset),ram.begin()+static_cast<std::ptrdiff_t>(offset+4U)}); out.accesses.push_back({static_cast<std::uint64_t>(out.accesses.size()),StartupBusKind::data_read,{{},address},{ram.begin()+static_cast<std::ptrdiff_t>(offset),ram.begin()+static_cast<std::ptrdiff_t>(offset+4U)},"synthetic_work_ram",decoded.provenance}); if (effect.affects_condition_codes) initial.sr = m68k_move_result_ccr(initial.sr, initial.d[1]); initial.pc.value += effect.pc_delta;
    }
    StartupBoundary boundary{}; boundary.ordinal=ordinal+1U; boundary.state=initial; for(const auto address:changed) { const auto offset=m68k_startup_ram_offset(address); boundary.ram_bytes.push_back({address,{ram[offset],ram[offset+1U],ram[offset+2U],ram[offset+3U]}}); } boundary.bus_through_ordinal=out.accesses.back().ordinal; boundary.stop_reason=ordinal+1U==analysis.decoded.size()?"instruction_budget_exhausted":"continue"; out.boundaries.push_back(std::move(boundary));
  }
  out.final_state=initial; out.stop_reason="instruction_budget_exhausted"; return out;
}

std::string format_genesis_rom_startup_result(const StartupResult &result) {
  std::ostringstream out;
  const auto provenance = [](const InstructionProvenance &p) { std::ostringstream v; v << "{\"source_address\":\"" << hex(p.source.address.value,8) << "\",\"image_offset\":" << p.source.image_offset.value << ",\"raw_bytes\":\"" << hex(p.bytes[0],2) << hex(p.bytes[1],2) << "\",\"length\":" << p.length.value << '}'; return v.str(); };
  const auto state = [](const StartupState &s) { std::ostringstream v; v << "{\"d\":["; for(std::size_t i=0;i<s.d.size();++i) { if(i) v << ','; v << '"' << hex(s.d[i],8) << '"'; } v << "],\"a7\":\"" << hex(s.a[7],8) << "\",\"sr\":\"" << hex(s.sr,4) << "\",\"pc\":\"" << hex(s.pc.value,8) << "\"}"; return v.str(); };
  const auto access_list = [&](const std::vector<StartupBusRecord> &records) { std::ostringstream v; v << '['; for (std::size_t i=0;i<records.size();++i) { const auto &a=records[i]; if(i)v<<','; v << "{\"ordinal\":" << a.ordinal << ",\"kind\":" << static_cast<unsigned>(a.kind) << ",\"address\":\"" << hex(a.address.value,8) << "\",\"bytes\":\""; for(const auto byte:a.bytes)v<<hex(byte,2); v << "\",\"length\":" << a.bytes.size() << ",\"region\":\"" << a.region << "\",\"instruction\":" << provenance(a.instruction) << '}'; } return v << ']',v.str(); };
  const auto boundary_list = [&](const std::vector<StartupBoundary> &records) { std::ostringstream v; v << '['; for(std::size_t i=0;i<records.size();++i){const auto&b=records[i];if(i)v<<',';v<<"{\"ordinal\":"<<b.ordinal<<",\"state\":"<<state(b.state)<<",\"ram_bytes\":[";for(std::size_t j=0;j<b.ram_bytes.size();++j){if(j)v<<',';v<<"{\"address\":\""<<hex(b.ram_bytes[j].first,8)<<"\",\"bytes\":\"";for(const auto byte:b.ram_bytes[j].second)v<<hex(byte,2);v<<"\"}";}v<<"],\"bus_through_ordinal\":";if(b.bus_through_ordinal)v<<*b.bus_through_ordinal;else v<<"null";v<<",\"stop_reason\":\""<<b.stop_reason<<"\"}";}return v<<']',v.str(); };
  const auto call = [&](const M68kStaticCall &value) { std::ostringstream v; v << "{\"caller\":" << provenance(value.caller) << ",\"continuation\":\"" << hex(value.continuation.value,8) << "\",\"callee\":\"" << hex(value.callee.value,8) << "\"}"; return v.str(); };
  if (const auto *failed = std::get_if<StartupFailure>(&result)) {
    out << "{\"result\":\"rejected\",\"ordinal\":" << failed->ordinal << ",\"category\":\"" << failed->category << "\",\"attempted_source\":{\"cpu_variant\":\"mc68000\",\"source_address\":\"" << hex(failed->attempted_source.value, 8) << "\",\"image_offset\":";
    if (failed->image_offset) out << failed->image_offset->value; else out << "null";
    out << "},\"complete_instruction\":"; if(failed->complete_instruction) { out << provenance(*failed->complete_instruction); } else out << "null";
    out << ",\"complete_instruction_raw_bytes\":"; if (failed->complete_instruction_raw_bytes) { out << '\"'; for (const auto byte : *failed->complete_instruction_raw_bytes) out << hex(byte,2); out << '\"'; } else out << "null";
    out << ",\"primary_provenance\":"; if(failed->primary_provenance) out << provenance(*failed->primary_provenance); else out << "null";
    out << ",\"available_bytes\":" << failed->available_bytes << ",\"requested_length\":" << failed->requested_length << ",\"effective_address\":";
    if(failed->effective_address) out << '"' << hex(*failed->effective_address,8) << '"'; else out << "null";
    out << ",\"attempted_range\":"; if(failed->attempted_range) out << "{\"begin\":\"" << hex(failed->attempted_range->first,8) << "\",\"end\":\"" << hex(failed->attempted_range->second,8) << "\"}"; else out << "null";
    out << ",\"region\":"; if(failed->region) out << '"' << *failed->region << '"'; else out << "null";
    out << ",\"a7\":"; if(failed->a7) out << '"' << hex(*failed->a7,8) << '"'; else out << "null";
    out << ",\"call\":"; if(failed->call) out << call(*failed->call); else out << "null";
    out << ",\"expected_continuation\":"; if(failed->expected_continuation) out << '"' << hex(*failed->expected_continuation,8) << '"'; else out << "null";
    out << ",\"observed_return_target\":"; if(failed->observed_return_target) out << '"' << hex(*failed->observed_return_target,8) << '"'; else out << "null";
    out << ",\"retained_boundary_ordinal\":" << failed->retained_boundary_ordinal << ",\"bus_through_ordinal\":"; if(failed->bus_through_ordinal)out<<*failed->bus_through_ordinal;else out<<"null";
    out << ",\"accesses\":" << access_list(failed->accesses) << ",\"boundaries\":" << boundary_list(failed->boundaries);
    return out << "}", out.str();
  }
  const auto &ok = std::get<StartupExecution>(result);
  out << "{\"result\":\"accepted\",\"instructions\":["; for(std::size_t i=0;i<ok.instructions.size();++i){if(i)out<<',';out<<"{\"provenance\":"<<provenance(ok.instructions[i].provenance)<<",\"raw_bytes\":\"";for(const auto byte:ok.instructions[i].raw_bytes)out<<hex(byte,2);out<<"\",\"kind\":"<<static_cast<unsigned>(ok.instructions[i].kind)<<"}";} out << "],\"accesses\":" << access_list(ok.accesses);
  out << ",\"bus_records\":" << ok.accesses.size() << ",\"boundaries\":" << boundary_list(ok.boundaries) << ",\"calls\":[";for(std::size_t i=0;i<ok.calls.size();++i){if(i)out<<',';out<<call(ok.calls[i]);}out<<"],\"returns\":"<<ok.returns.size()<<",\"final_state\":" << state(ok.final_state) << ",\"stop_reason\":\"" << ok.stop_reason << "\",\"pc\":\"" << hex(ok.final_state.pc.value,8) << "\",\"a7\":\"" << hex(ok.final_state.a[7],8) << "\",\"d0\":\"" << hex(ok.final_state.d[0],8) << "\",\"d1\":\"" << hex(ok.final_state.d[1],8) << "\"}";
  return out.str();
}

StartupState make_genesis_reset_startup_state(const GenesisResetImageReport &reset) {
  StartupState initial{};
  initial.d = {0x11111111U, 0x22222222U, 0x33333333U, 0x44444444U,
               0x55555555U, 0x66666666U, 0x77777777U, 0x88888888U};
  initial.a[7] = reset.initial_ssp->value;
  initial.sr = 0x2700U;
  initial.pc = {TargetAddressSpace::m68k_program, reset.entry_address->value};
  return initial;
}

FrontendProgram make_genesis_reset_startup_program(std::vector<std::uint8_t> bytes,
                                                    const StartupState &initial,
                                                    bool general_startup) {
  FrontendProgram program{};
  program.profile = general_startup ? M68kFrontendProfile::general_startup
                                    : M68kFrontendProfile::genesis_rom_startup;
  program.image = {"genesis-reset-image", std::move(bytes), 0U};
  program.image.byte_length = program.image.bytes.size();
  program.mapping_claims.push_back(
      {"raw_cartridge_rom", {TargetAddressSpace::m68k_program, 0U},
       {TargetAddressSpace::m68k_program, static_cast<std::uint32_t>(program.image.bytes.size())},
       {0U}, {program.image.bytes.size()}});
  program.startup_ingress = M68kStartupIngress{initial.pc, initial.a[7]};
  return program;
}

std::optional<FrontendProgram> make_genesis_bridge_startup_program(
    std::vector<std::uint8_t> bytes, std::uint32_t mapping_base, std::uint32_t entry,
    std::optional<std::pair<std::uint32_t, std::uint32_t>> synthetic_completion) {
  const auto mapping_end = static_cast<std::uint64_t>(mapping_base) + bytes.size();
  // A mapping endpoint is exclusive, so the 24-bit cartridge address space
  // may end exactly one byte beyond its largest address.
  if (mapping_base > UINT32_C(0x00FFFFFF) || mapping_end > UINT64_C(0x01000000) ||
      entry < mapping_base || static_cast<std::uint64_t>(entry) >= mapping_end) {
    return std::nullopt;
  }
  FrontendProgram program{};
  program.profile = M68kFrontendProfile::general_startup;
  program.image = {"bridge-generation-input", std::move(bytes), 0U};
  program.image.byte_length = program.image.bytes.size();
  program.mapping_claims.push_back(
      {"raw_cartridge_rom", {TargetAddressSpace::m68k_program, mapping_base},
       {TargetAddressSpace::m68k_program,
         static_cast<std::uint32_t>(mapping_end)},
       {0U}, {program.image.bytes.size()}});
  // Reserve the first RAM longword for a nested JSR predecrement; a declared
  // synthetic-completion sentinel occupies the initial SSP slot above it.
  program.startup_ingress = M68kStartupIngress{
      {TargetAddressSpace::m68k_program, entry}, UINT32_C(0x00FF0004)};
  if (synthetic_completion) {
    program.synthetic_completion = FrontendProgram::CompletionContract{
        {TargetAddressSpace::m68k_program, synthetic_completion->first},
        {TargetAddressSpace::m68k_program, synthetic_completion->second}};
  }
  return program;
}

std::optional<FrontendProgram> make_genesis_reset_bridge_startup_program(
    std::vector<std::uint8_t> bytes, const GenesisResetImageReport &reset,
    std::optional<std::pair<std::uint32_t, std::uint32_t>> synthetic_completion) {
  if (reset.outcome != ResetOutcome::accepted || !reset.entry_address || !reset.initial_ssp) {
    return std::nullopt;
  }
  auto program = make_genesis_bridge_startup_program(
      std::move(bytes), 0U, reset.entry_address->value, synthetic_completion);
  if (!program) return std::nullopt;
  program->startup_ingress->initial_ssp = reset.initial_ssp->value;
  return program;
}

} // namespace segarecomp
