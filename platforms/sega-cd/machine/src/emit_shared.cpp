#include "emit_shared.hpp"

#include <cstdio>

#include "segarecomp/cpu/m68k/timing.hpp"

namespace segarecomp::machine::sega_cd::detail {
namespace {

std::string width_of(M68kMemoryAccessWidth size) {
  return size == M68kMemoryAccessWidth::byte ? "1U" : size == M68kMemoryAccessWidth::word ? "2U" : "4U";
}

}  // namespace

std::string hex(std::uint32_t value, int width) {
  char text[16];
  std::snprintf(text, sizeof text, "%0*X", width, static_cast<unsigned>(value));
  return text;
}

// The Sega CD routed-access protocol text for one processor instance (ADR 0128). The CPU lowering decides when an access, guard,
// exception or return is needed and owns all MC68000 semantics; this text only names the Sega CD runtime ABI (scd_runtime.h).
// Every failure returns 1 after the runtime recorded the typed stop; nothing is committed by the failing statement.
SegaCdM68kRuntimeCEmitter::SegaCdM68kRuntimeCEmitter(unsigned cpu)
    : k_(std::to_string(cpu) + "U"), lookup_("scd_p" + std::to_string(cpu) + "_lookup") {}

std::string SegaCdM68kRuntimeCEmitter::instruction_source(const M68kIrOperation &op) const {
  return "UINT32_C(0x" + hex(op.provenance.source.address.value) + ")";
}
std::string_view SegaCdM68kRuntimeCEmitter::default_provenance_helper() const { return "scd_unused_provenance"; }

std::string SegaCdM68kRuntimeCEmitter::routed_read(const M68kMemoryEmissionContext &ctx, std::string_view addr, std::string_view value,
                                                   std::string_view, M68kMemoryAccessWidth size) const {
  return "uint32_t " + std::string(value) + " = UINT32_C(0); if (scd_read(m, " + k_ + ", " + std::string(addr) + ", " + width_of(size) +
         ", &" + std::string(value) + ", " + std::string(ctx.runtime_source) + ") != 0) return 1;\n";
}
std::string SegaCdM68kRuntimeCEmitter::routed_write(const M68kMemoryEmissionContext &ctx, std::string_view addr, std::string_view,
                                                    M68kMemoryAccessWidth size, std::string_view value) const {
  return "if (scd_write(m, " + k_ + ", " + std::string(addr) + ", " + width_of(size) + ", (uint32_t)(" + std::string(value) + "), " +
         std::string(ctx.runtime_source) + ") != 0) return 1;";
}
std::string SegaCdM68kRuntimeCEmitter::return_pop_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7) const {
  return "{ uint32_t m68k_observed_return = UINT32_C(0); if (scd_read(m, " + k_ + ", " + std::string(a7) + ", 4U, &m68k_observed_return, " +
         std::string(ctx.runtime_source) + ") != 0) return 1; if (" + lookup_ + "(m68k_observed_return) == NULL) return scd_fail(m, " + k_ +
         ", SCD_STOP_RETURN_TARGET_NOT_COMPILED, " + std::string(ctx.runtime_source) + ", m68k_observed_return); ";
}
std::string SegaCdM68kRuntimeCEmitter::unresolved_indirect_stop(const M68kMemoryEmissionContext &ctx) const {
  return "return scd_fail(m, " + k_ + ", SCD_STOP_ENTRY_NOT_COMPILED, " + std::string(ctx.runtime_source) + ", m68k_indirect_ea);\n";   // the detail is the computed destination (SEG-099-T005)
}
std::string SegaCdM68kRuntimeCEmitter::call_push_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7, bool) const {
  return "{ uint32_t m68k_continuation = UINT32_C(0x" + hex(ctx.continuation) + "); { const uint32_t m68k_new_a7 = " + std::string(a7) +
         " - UINT32_C(4); if (scd_write(m, " + k_ + ", m68k_new_a7, 4U, m68k_continuation, " + std::string(ctx.runtime_source) +
         ") != 0) return 1;\n";
}
std::string SegaCdM68kRuntimeCEmitter::divide_by_zero(const M68kMemoryEmissionContext &ctx, std::uint32_t next_pc,
                                                      std::uint32_t entry_cycles) const {
  return "return scd_raise(m, " + k_ + ", 5U, UINT32_C(0x" + hex(next_pc) + "), UINT32_C(" + std::to_string(entry_cycles) + "), " +
         std::string(ctx.runtime_source) + "); } ";
}
std::string SegaCdM68kRuntimeCEmitter::exception_return(const M68kMemoryEmissionContext &ctx) const {
  return "{ uint32_t m68k_rte_pc = UINT32_C(0); if (scd_rte(m, " + k_ + ", &m68k_rte_pc, " + std::string(ctx.runtime_source) +
         ") != 0) return 1; ";
}
std::string SegaCdM68kRuntimeCEmitter::privilege_violation(const M68kMemoryEmissionContext &ctx, std::uint32_t fault_pc,
                                                           std::uint32_t entry_cycles) const {
  return "{ return scd_raise(m, " + k_ + ", 8U, UINT32_C(0x" + hex(fault_pc) + "), UINT32_C(" + std::to_string(entry_cycles) + "), " +
         std::string(ctx.runtime_source) + "); }";
}
std::string SegaCdM68kRuntimeCEmitter::trace_deferred_stop(const M68kMemoryEmissionContext &ctx) const {
  return "{ return scd_fail(m, " + k_ + ", SCD_STOP_TRACE_UNSUPPORTED, " + std::string(ctx.runtime_source) + ", 0U); }";
}
std::string SegaCdM68kRuntimeCEmitter::software_exception(const M68kMemoryEmissionContext &ctx, std::uint32_t vector,
                                                          std::uint32_t stacked_pc, std::uint32_t entry_cycles) const {
  return "{ return scd_raise(m, " + k_ + ", UINT32_C(" + std::to_string(vector) + "), UINT32_C(0x" + hex(stacked_pc) + "), UINT32_C(" +
         std::to_string(entry_cycles) + "), " + std::string(ctx.runtime_source) + "); }";
}
std::string SegaCdM68kRuntimeCEmitter::condition_code_return(const M68kMemoryEmissionContext &ctx) const {
  return "{ uint32_t m68k_rtr_pc = UINT32_C(0); if (scd_rtr(m, " + k_ + ", &m68k_rtr_pc, " + std::string(ctx.runtime_source) +
         ") != 0) return 1; ";
}
std::string SegaCdM68kRuntimeCEmitter::stopped_state(const M68kMemoryEmissionContext &) const { return "scd_enter_stop(m, " + k_ + ");\n"; }

LoweredInstruction lower_instruction(const M68kIrOperation &op, std::uint32_t pc, const SegaCdM68kRuntimeCEmitter &emitter,
                                     const std::string &lookup_symbol) {
  LoweredInstruction out;
  const auto timing = m68k_timing_c_expression(op);
  M68kMemoryEmissionContext ctx;
  ctx.runtime_routing = true;
  ctx.runtime_object = "m";
  ctx.runtime_emitter = &emitter;
  ctx.ram_array = "scd_no_linear_memory";  // any non-routed access would fail to compile: fail closed at build time
  ctx.address_registers = "cpu->a";
  ctx.user_stack_pointer = "cpu->usp";
  ctx.frame_ids_array = "scd_no_frames";
  ctx.frame_continuations_array = "scd_no_frames";
  ctx.frame_depth = "scd_no_frames";
  ctx.program_counter = "pc";
  ctx.linear_memory_begin = 0U;
  ctx.linear_memory_end = 0x01000000U;
  ctx.continuation = pc + op.provenance.length.value;
  ctx.test_operand_access = M68kOperandAccess::runtime_routed;
  ctx.compiled_entry_lookup_symbol = lookup_symbol;
  if (op.kind == M68kIrKind::dbcc_loop) {
    out.slots += "  uint8_t m68k_dbcc_took_branch = 0U;\n";
    ctx.timing_dbcc_taken = "m68k_dbcc_took_branch";
  }
  if (op.kind == M68kIrKind::set_conditional && op.destination_ea.mode == M68kEaMode::data_register) {
    out.slots += "  uint8_t m68k_scc_true = 0U;\n";
    ctx.timing_scc_true = "m68k_scc_true";
  }
  if (op.kind == M68kIrKind::multiply_signed_word || op.kind == M68kIrKind::multiply_unsigned_word) {
    out.slots += "  uint16_t m68k_timing_mul_source = UINT16_C(0);\n";
    ctx.timing_mul_source = "m68k_timing_mul_source";
  }
  if (op.kind == M68kIrKind::divide_signed_word || op.kind == M68kIrKind::divide_unsigned_word) {
    out.slots += "  uint32_t m68k_timing_div_dividend = UINT32_C(0); uint16_t m68k_timing_div_divisor = UINT16_C(0);\n";
    ctx.timing_div_dividend = "m68k_timing_div_dividend";
    ctx.timing_div_divisor = "m68k_timing_div_divisor";
  }
  out.body = emit_m68k_operation_c(op, "cpu->d", "cpu->sr", "  ", &ctx);
  out.compiled = timing.has_value() && out.body.find_first_not_of(" \n") != std::string::npos;
  if (timing) out.timing = *timing;
  const auto long_class = m68k_long_instruction_class(op);
  if (long_class.kind == M68kLongInstructionKind::register_count_shift)
    out.refresh_skip = "  m->refresh_skip_cycles = 2U * (cpu->d[" + std::to_string(long_class.count_register) + "] & UINT32_C(63));\n";
  else if (long_class.kind == M68kLongInstructionKind::movem_long)
    out.refresh_skip = "  m->refresh_skip_cycles = " + std::to_string(8U * long_class.registers) + "U;\n";
  return out;
}

}  // namespace segarecomp::machine::sega_cd::detail
