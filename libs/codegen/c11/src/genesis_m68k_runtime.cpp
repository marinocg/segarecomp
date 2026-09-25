// Genesis runtime-routed C11 protocol text used by the generic M68k lowering seam
// (M68kRuntimeCEmitter). The lowering owns MC68000 semantics; this wrapper owns the Genesis
// routed-access / stop / provenance ABI spelling. Output is byte-identical to the text the
// lowering emitted before SEG-018-T004 moved ownership.
#include "segarecomp/codegen/c11/genesis_frontend.hpp"

#include <iomanip>
#include <sstream>

namespace segarecomp {
namespace {
// SEG-020-T003: stack accesses route with their existing bus kind only when the
// generation-time diagnostics option is on; otherwise the emitted text is unchanged.
std::string stack_route_open(const M68kMemoryEmissionContext &ctx, const char *bus_kind) {
  if (!ctx.execution_history_hooks) return "genesis_route_access(" + std::string(ctx.runtime_object) + ", ";
  return "genesis_route_access_bus(" + std::string(ctx.runtime_object) + ", " + bus_kind + ", ";
}
}  // namespace

namespace {

std::string hex(std::uint32_t value, unsigned width) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
  return out.str();
}

std::string genesis_access_width(M68kMemoryAccessWidth size) {
  return size == M68kMemoryAccessWidth::byte ? "GENESIS_ACCESS_BYTE" :
         size == M68kMemoryAccessWidth::word ? "GENESIS_ACCESS_WORD" : "GENESIS_ACCESS_LONG";
}
// The lowering's own name for the Genesis width helper is preserved in the moved text below.
std::string m68k_genesis_access_width(M68kMemoryAccessWidth size) { return genesis_access_width(size); }

// SEG-022-T011: the routed-failure tail. Factored form: one call to the generated
// `genesis_routed_failure_stop` (defined once beside `genesis_static_stop`), which performs exactly the
// inline statements of the unfactored form in the same order and returns the same transfer. Returns ""
// when the caller did not opt in (or uses a different provenance helper), selecting the inline text.
std::string factored_route_failure(const M68kMemoryEmissionContext &ctx, std::string_view stop,
                                   std::string_view address, std::string_view width, std::string_view direction) {
  if (!ctx.factored_route_failure || ctx.runtime_provenance_helper != "genesis_attach_route_provenance") return {};
  std::ostringstream out;
  out << "return genesis_routed_failure_stop(&" << stop << ", " << ctx.runtime_source << ", " << address << ", "
      << width << ", " << direction << ");";
  return out.str();
}

class GenesisM68kRuntimeCEmitter final : public M68kRuntimeCEmitter {
 public:
  std::string instruction_source(const M68kIrOperation &operation) const override {
    const auto &p = operation.provenance;
    std::string routed_source = "&(const GenesisInstructionProvenance){GENESIS_CPU_MC68000, UINT32_C(0x" +
                    hex(p.source.address.value, 8) + "), UINT64_C(" + std::to_string(p.source.image_offset.value) +
                    "), {UINT8_C(0x" + hex(p.bytes[0], 2) + "), UINT8_C(0x" + hex(p.bytes[1], 2) +
                    ")}, UINT32_C(" + std::to_string(p.length.value) + ")}";
    return routed_source;
  }
  std::string_view default_provenance_helper() const override { return "genesis_attach_route_provenance"; }

  std::string routed_read(const M68kMemoryEmissionContext &ctx, std::string_view routed_addr, std::string_view value,
                          std::string_view stop, M68kMemoryAccessWidth size) const override {
    std::ostringstream out;
    out << "uint32_t " << value << " = UINT32_C(0); GenesisRuntimeStop " << stop << " = {0}; if "
        << "(genesis_route_access(" << ctx.runtime_object << ", " << routed_addr << ", "
        << m68k_genesis_access_width(size) << ", GENESIS_ACCESS_READ, &" << value << ", &" << stop
        << ") != GENESIS_ACCESS_OK) ";
    if (const auto factored = factored_route_failure(ctx, stop, routed_addr, m68k_genesis_access_width(size),
                                                     "GENESIS_ACCESS_READ"); !factored.empty()) {
      out << factored << "\n";
      return out.str();
    }
    out << "{ " << stop << ".provenance.has_instruction_provenance = 1U; "
        << stop << ".provenance.instruction = *" << ctx.runtime_source << "; "
         << stop << ".provenance.has_access = 1U; " << stop << ".provenance.access_address = " << routed_addr
        << "; " << stop << ".provenance.access_width = " << m68k_genesis_access_width(size)
         << "; " << stop << ".provenance.access_direction = GENESIS_ACCESS_READ; "
         << ctx.runtime_provenance_helper << "(&" << stop << ", " << ctx.runtime_source
         << "); { GenesisControlTransfer transfer = {0}; "
        << "transfer.kind = GENESIS_STOP; transfer.stop = " << stop << "; return transfer; } }\n";
    return out.str();
  }

  std::string routed_write(const M68kMemoryEmissionContext &ctx, std::string_view routed_addr, std::string_view stop,
                           M68kMemoryAccessWidth size, std::string_view value) const override {
    std::ostringstream out;
    out << "uint32_t m68k_routed_value = (" << value << "); GenesisRuntimeStop " << stop << " = {0}; if "
        << "(genesis_route_access(" << ctx.runtime_object << ", " << routed_addr << ", "
        << m68k_genesis_access_width(size) << ", GENESIS_ACCESS_WRITE, &m68k_routed_value, &" << stop
        << ") != GENESIS_ACCESS_OK) ";
    if (const auto factored = factored_route_failure(ctx, stop, routed_addr, m68k_genesis_access_width(size),
                                                     "GENESIS_ACCESS_WRITE"); !factored.empty()) {
      out << factored;
      return out.str();
    }
    out << "{ " << stop << ".provenance.has_instruction_provenance = 1U; "
        << stop << ".provenance.instruction = *" << ctx.runtime_source << "; "
        << stop << ".provenance.has_access = 1U; " << stop << ".provenance.access_address = " << routed_addr
        << "; " << stop << ".provenance.access_width = " << m68k_genesis_access_width(size)
         << "; " << stop << ".provenance.access_direction = GENESIS_ACCESS_WRITE; "
         << ctx.runtime_provenance_helper << "(&" << stop << ", " << ctx.runtime_source
         << "); { GenesisControlTransfer transfer = {0}; "
        << "transfer.kind = GENESIS_STOP; transfer.stop = " << stop << "; return transfer; } }";
    return out.str();
  }

  std::string return_pop_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7) const override {
    std::ostringstream out;
          out << "{ uint32_t m68k_observed_return = UINT32_C(0); GenesisRuntimeStop m68k_route_stop = {0}; "
                 << "if ((" << a7 << " & 1U) != 0U) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_ALIGNMENT, "
                 << ctx.runtime_source << ", 1U, " << a7 << ", GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ); "
                 << "if (" << a7 << " < UINT32_C(0x" << hex(ctx.linear_memory_begin, 8) << ") || " << a7
                 << " > UINT32_C(0x" << hex(ctx.linear_memory_end - 4U, 8) << ")) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_RANGE, "
                 << ctx.runtime_source << ", 1U, " << a7 << ", GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ); "
                 << "if (" << stack_route_open(ctx, "GENESIS_BUS_STACK_READ") << a7
                 << ", GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ, &m68k_observed_return, &m68k_route_stop) != GENESIS_ACCESS_OK) ";
          if (const auto factored = factored_route_failure(ctx, "m68k_route_stop", a7, "GENESIS_ACCESS_LONG",
                                                           "GENESIS_ACCESS_READ"); !factored.empty())
            out << factored << " if (";
          else
            out << "{ "
                 << "m68k_route_stop.provenance.has_instruction_provenance = 1U; m68k_route_stop.provenance.instruction = *"
                 << ctx.runtime_source << "; m68k_route_stop.provenance.has_access = 1U; m68k_route_stop.provenance.access_address = "
                 << a7 << "; m68k_route_stop.provenance.access_width = GENESIS_ACCESS_LONG; m68k_route_stop.provenance.access_direction = GENESIS_ACCESS_READ; "
                 << ctx.runtime_provenance_helper << "(&m68k_route_stop, " << ctx.runtime_source
                 << "); { GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = m68k_route_stop; return transfer; } } if (";
          for (std::size_t index = 0; index < ctx.runtime_return_targets.size(); ++index) {
            if (index != 0U) out << " && ";
            out << "m68k_observed_return != UINT32_C(0x" << hex(ctx.runtime_return_targets[index], 8) << ")";
          }
          if (ctx.runtime_return_targets.empty()) out << "1";
          out << ") return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_RETURN_TARGET_MISMATCH, "
                 << ctx.runtime_source << ", 1U, " << a7 << ", GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ); ";
    return out.str();
  }

  std::string unresolved_indirect_stop(const M68kMemoryEmissionContext &ctx) const override {
    std::ostringstream out;
    out << "return genesis_static_stop(GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET, GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE, "
        << ctx.runtime_source << ", 0U, UINT32_C(0), GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);\n";
    return out.str();
  }

  std::string call_push_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7, bool expanded) const override {
    std::ostringstream out;
    if (expanded) {
          out << "  { uint32_t m68k_continuation = UINT32_C(0x" << hex(ctx.continuation, 8)
                 << "); GenesisRuntimeStop m68k_route_stop = {0};\n"
                 << "    if ((" << a7 << " & 1U) != 0U) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_ALIGNMENT, "
                 << ctx.runtime_source << ", 0U, UINT32_C(0), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);\n"
                 << "    if (" << a7 << " < UINT32_C(0x" << hex(ctx.linear_memory_begin + 4U, 8) << ") || " << a7
                 << " > UINT32_C(0x" << hex(ctx.linear_memory_end, 8)
                 << ")) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_RANGE, "
                 << ctx.runtime_source << ", 0U, UINT32_C(0), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);\n"
                 << "    { const uint32_t m68k_new_a7 = " << a7 << " - UINT32_C(4);\n"
                 << "      if (" << stack_route_open(ctx, "GENESIS_BUS_STACK_WRITE")
                 << "m68k_new_a7, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, &m68k_continuation, &m68k_route_stop) != GENESIS_ACCESS_OK) ";
      if (const auto factored = factored_route_failure(ctx, "m68k_route_stop", "m68k_new_a7", "GENESIS_ACCESS_LONG",
                                                       "GENESIS_ACCESS_WRITE"); !factored.empty()) {
        out << factored << "\n";
        return out.str();
      }
      out << "{\n"
                 << "        m68k_route_stop.provenance.has_instruction_provenance = 1U; m68k_route_stop.provenance.instruction = *"
                 << ctx.runtime_source
                 << "; m68k_route_stop.provenance.has_access = 1U; m68k_route_stop.provenance.access_address = m68k_new_a7; m68k_route_stop.provenance.access_width = GENESIS_ACCESS_LONG; m68k_route_stop.provenance.access_direction = GENESIS_ACCESS_WRITE;\n"
                 << "        " << ctx.runtime_provenance_helper << "(&m68k_route_stop, " << ctx.runtime_source << ");\n"
                 << "        { GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = m68k_route_stop; return transfer; }\n"
                 << "      }\n";
    } else {
                out << "{ uint32_t m68k_continuation = UINT32_C(0x"
                       << hex(ctx.continuation, 8) << "); GenesisRuntimeStop m68k_route_stop = {0}; "
                       << "if ((" << a7 << " & 1U) != 0U) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_ALIGNMENT, "
                       << ctx.runtime_source << ", 0U, UINT32_C(0), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE); "
                       << "if (" << a7 << " < UINT32_C(0x" << hex(ctx.linear_memory_begin + 4U, 8) << ") || " << a7
                       << " > UINT32_C(0x" << hex(ctx.linear_memory_end, 8) << ")) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_RANGE, "
                       << ctx.runtime_source << ", 0U, UINT32_C(0), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE); "
                       << "{ const uint32_t m68k_new_a7 = " << a7 << " - UINT32_C(4); "
                       << "if (" << stack_route_open(ctx, "GENESIS_BUS_STACK_WRITE") << "m68k_new_a7, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, &m68k_continuation, &m68k_route_stop) != GENESIS_ACCESS_OK) ";
      if (const auto factored = factored_route_failure(ctx, "m68k_route_stop", "m68k_new_a7", "GENESIS_ACCESS_LONG",
                                                       "GENESIS_ACCESS_WRITE"); !factored.empty()) {
        out << factored << " ";
        return out.str();
      }
      out << "{ "
                       << "m68k_route_stop.provenance.has_instruction_provenance = 1U; m68k_route_stop.provenance.instruction = *"
                       << ctx.runtime_source << "; m68k_route_stop.provenance.has_access = 1U; m68k_route_stop.provenance.access_address = m68k_new_a7; m68k_route_stop.provenance.access_width = GENESIS_ACCESS_LONG; m68k_route_stop.provenance.access_direction = GENESIS_ACCESS_WRITE; "
                       << ctx.runtime_provenance_helper << "(&m68k_route_stop, " << ctx.runtime_source
                       << "); { GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = m68k_route_stop; return transfer; } } ";
    }
    return out.str();
  }

  std::string divide_by_zero(const M68kMemoryEmissionContext &ctx, std::uint32_t next_pc) const override {
    std::ostringstream out;
                 out << "uint32_t divide_handler_pc = UINT32_C(0); GenesisRuntimeStop divide_stop = {0}; "
                  << "if (genesis_raise_divide_by_zero("
                 << ctx.runtime_object << ", UINT32_C(" << next_pc
                 // Literal bare "pc" spelling (never `ctx.program_counter`,
                 // which holds the fully-qualified "runtime->pc" text): this
                 // body is always reached through the C4 caller's
                 // `#define pc runtime->pc` bridge (frontend.cpp), and the
                 // fully-qualified spelling's own trailing "pc" token would be
                 // re-expanded into "runtime->runtime->pc" by that still-active
                 // macro (the exact hazard return_from_exception's own comment
                 // documents for `subtract_address`).
                  << "), &divide_handler_pc, &divide_stop) == 1) { "
                  << "GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_CONTINUE_AT_PC; "
                  << "transfer.next_pc = divide_handler_pc; return transfer; } else { "
                 << "divide_stop.provenance.has_instruction_provenance = 1U; divide_stop.provenance.instruction = *"
                 << ctx.runtime_source << "; " << ctx.runtime_provenance_helper
                 << "(&divide_stop, " << ctx.runtime_source << "); "
                 << "{ GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = divide_stop; return transfer; } } } ";
    return out.str();
  }

  std::string exception_return(const M68kMemoryEmissionContext &ctx) const override {
    std::ostringstream out;
        out << "{ uint32_t m68k_rte_pc = UINT32_C(0); GenesisRuntimeStop m68k_rte_stop = {0}; "
               << "if (genesis_exception_return(" << ctx.runtime_object
               << ", &m68k_rte_pc, &m68k_rte_stop) != 1) { "
               << "m68k_rte_stop.provenance.has_instruction_provenance = 1U; m68k_rte_stop.provenance.instruction = *"
               << ctx.runtime_source << "; " << ctx.runtime_provenance_helper
               << "(&m68k_rte_stop, " << ctx.runtime_source
               << "); { GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = m68k_rte_stop; return transfer; } } ";
    return out.str();
  }

  std::string privilege_violation(const M68kMemoryEmissionContext &ctx, std::uint32_t fault_pc) const override {
    std::ostringstream out;
    out << "{ uint32_t m68k_privilege_handler_pc = UINT32_C(0); GenesisRuntimeStop m68k_privilege_stop = {0}; "
        << "if (genesis_raise_privilege_violation(" << ctx.runtime_object << ", UINT32_C(0x" << hex(fault_pc, 8)
        << "), &m68k_privilege_handler_pc, &m68k_privilege_stop) == 1) { "
        << "GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_CONTINUE_AT_PC; "
        << "transfer.next_pc = m68k_privilege_handler_pc; return transfer; } "
        << "m68k_privilege_stop.provenance.has_instruction_provenance = 1U; m68k_privilege_stop.provenance.instruction = *"
        << ctx.runtime_source << "; " << ctx.runtime_provenance_helper << "(&m68k_privilege_stop, "
        << ctx.runtime_source << "); "
        << "{ GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = m68k_privilege_stop; "
           "return transfer; } }";
    return out.str();
  }

  std::string trace_deferred_stop(const M68kMemoryEmissionContext &ctx) const override {
    std::ostringstream out;
    out << "{ GenesisRuntimeStop m68k_trace_stop = {0}; m68k_trace_stop.stop_class = GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION; "
        << "m68k_trace_stop.diagnostic_category = GENESIS_DIAG_UNSUPPORTED_TRACE_EXCEPTION; "
        << "m68k_trace_stop.provenance.has_instruction_provenance = 1U; m68k_trace_stop.provenance.instruction = *"
        << ctx.runtime_source << "; " << ctx.runtime_provenance_helper << "(&m68k_trace_stop, " << ctx.runtime_source
        << "); { GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = m68k_trace_stop; "
           "return transfer; } }";
    return out.str();
  }
};

}  // namespace

const M68kRuntimeCEmitter &genesis_m68k_runtime_c_emitter() {
  static const GenesisM68kRuntimeCEmitter emitter;
  return emitter;
}

}  // namespace segarecomp
