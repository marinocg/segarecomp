#include "segarecomp/machine/sega_cd/program.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/cpu/m68k/timing.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::sega_cd {
namespace {

std::string hex(std::uint32_t value, int width = 8) {
  char text[16];
  std::snprintf(text, sizeof text, "%0*X", width, static_cast<unsigned>(value));
  return text;
}

std::string width_of(M68kMemoryAccessWidth size) {
  return size == M68kMemoryAccessWidth::byte ? "1U" : size == M68kMemoryAccessWidth::word ? "2U" : "4U";
}

// The Sega CD routed-access protocol text for one processor instance (ADR 0128). The CPU lowering decides when an access, guard,
// exception or return is needed and owns all MC68000 semantics; this text only names the Sega CD runtime ABI (scd_runtime.h).
// Every failure returns 1 after the runtime recorded the typed stop; nothing is committed by the failing statement.
class SegaCdM68kRuntimeCEmitter final : public M68kRuntimeCEmitter {
 public:
  explicit SegaCdM68kRuntimeCEmitter(unsigned cpu) : k_(std::to_string(cpu) + "U"), lookup_("scd_p" + std::to_string(cpu) + "_lookup") {}

  std::string instruction_source(const M68kIrOperation &op) const override {
    return "UINT32_C(0x" + hex(op.provenance.source.address.value) + ")";
  }
  std::string_view default_provenance_helper() const override { return "scd_unused_provenance"; }

  std::string routed_read(const M68kMemoryEmissionContext &ctx, std::string_view addr, std::string_view value, std::string_view,
                          M68kMemoryAccessWidth size) const override {
    return "uint32_t " + std::string(value) + " = UINT32_C(0); if (scd_read(m, " + k_ + ", " + std::string(addr) + ", " +
           width_of(size) + ", &" + std::string(value) + ", " + std::string(ctx.runtime_source) + ") != 0) return 1;\n";
  }
  std::string routed_write(const M68kMemoryEmissionContext &ctx, std::string_view addr, std::string_view, M68kMemoryAccessWidth size,
                           std::string_view value) const override {
    return "if (scd_write(m, " + k_ + ", " + std::string(addr) + ", " + width_of(size) + ", (uint32_t)(" + std::string(value) +
           "), " + std::string(ctx.runtime_source) + ") != 0) return 1;";
  }
  std::string return_pop_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7) const override {
    return "{ uint32_t m68k_observed_return = UINT32_C(0); if (scd_read(m, " + k_ + ", " + std::string(a7) +
           ", 4U, &m68k_observed_return, " + std::string(ctx.runtime_source) + ") != 0) return 1; if (" + lookup_ +
           "(m68k_observed_return) == NULL) return scd_fail(m, " + k_ + ", SCD_STOP_RETURN_TARGET_NOT_COMPILED, " +
           std::string(ctx.runtime_source) + ", m68k_observed_return); ";
  }
  std::string unresolved_indirect_stop(const M68kMemoryEmissionContext &ctx) const override {
    return "return scd_fail(m, " + k_ + ", SCD_STOP_ENTRY_NOT_COMPILED, " + std::string(ctx.runtime_source) + ", 0U);\n";
  }
  std::string call_push_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7, bool) const override {
    return "{ uint32_t m68k_continuation = UINT32_C(0x" + hex(ctx.continuation) + "); { const uint32_t m68k_new_a7 = " +
           std::string(a7) + " - UINT32_C(4); if (scd_write(m, " + k_ + ", m68k_new_a7, 4U, m68k_continuation, " +
           std::string(ctx.runtime_source) + ") != 0) return 1;\n";
  }
  std::string divide_by_zero(const M68kMemoryEmissionContext &ctx, std::uint32_t next_pc, std::uint32_t entry_cycles) const override {
    return "return scd_raise(m, " + k_ + ", 5U, UINT32_C(0x" + hex(next_pc) + "), UINT32_C(" + std::to_string(entry_cycles) + "), " +
           std::string(ctx.runtime_source) + "); } ";
  }
  std::string exception_return(const M68kMemoryEmissionContext &ctx) const override {
    return "{ uint32_t m68k_rte_pc = UINT32_C(0); if (scd_rte(m, " + k_ + ", &m68k_rte_pc, " + std::string(ctx.runtime_source) +
           ") != 0) return 1; ";
  }
  std::string privilege_violation(const M68kMemoryEmissionContext &ctx, std::uint32_t fault_pc, std::uint32_t entry_cycles) const override {
    return "{ return scd_raise(m, " + k_ + ", 8U, UINT32_C(0x" + hex(fault_pc) + "), UINT32_C(" + std::to_string(entry_cycles) + "), " +
           std::string(ctx.runtime_source) + "); }";
  }
  std::string trace_deferred_stop(const M68kMemoryEmissionContext &ctx) const override {
    return "{ return scd_fail(m, " + k_ + ", SCD_STOP_TRACE_UNSUPPORTED, " + std::string(ctx.runtime_source) + ", 0U); }";
  }
  std::string software_exception(const M68kMemoryEmissionContext &ctx, std::uint32_t vector, std::uint32_t stacked_pc,
                                 std::uint32_t entry_cycles) const override {
    return "{ return scd_raise(m, " + k_ + ", UINT32_C(" + std::to_string(vector) + "), UINT32_C(0x" + hex(stacked_pc) + "), UINT32_C(" +
           std::to_string(entry_cycles) + "), " + std::string(ctx.runtime_source) + "); }";
  }
  std::string condition_code_return(const M68kMemoryEmissionContext &ctx) const override {
    return "{ uint32_t m68k_rtr_pc = UINT32_C(0); if (scd_rtr(m, " + k_ + ", &m68k_rtr_pc, " + std::string(ctx.runtime_source) +
           ") != 0) return 1; ";
  }
  std::string stopped_state(const M68kMemoryEmissionContext &) const override { return "scd_enter_stop(m, " + k_ + ");\n"; }

 private:
  std::string k_;
  std::string lookup_;
};

struct Reached {
  std::uint32_t pc{};
  std::optional<M68kDecodedInstruction> decoded;
  std::optional<M68kIrOperation> operation;
};

struct Traversal {
  std::map<std::uint32_t, Reached> instructions;  // ordered: deterministic emission
  std::uint32_t out_of_image{};
  std::uint32_t vector_roots{};
};

Traversal traverse(const ProcessorImages &processor) {
  Traversal out;
  const auto &image = processor.images.images.front();
  const auto bytes = executable_image_bytes(processor.images, image);
  const std::uint32_t base = image.mappings.front().execution_base + image.mappings.front().first_offset;
  const std::uint32_t end = base + image.mappings.front().length;
  std::deque<std::uint32_t> work{processor.entry_pc};
  std::set<std::uint32_t> seen;
  std::set<std::uint32_t> outside;
  while (!work.empty()) {
    const std::uint32_t pc = work.front();
    work.pop_front();
    if (!seen.insert(pc).second) continue;
    if (pc < base || pc >= end || (pc & 1U) != 0U) {
      outside.insert(pc);
      continue;
    }
    Reached reached;
    reached.pc = pc;
    const std::uint32_t offset = pc - base + image.mappings.front().first_offset;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(bytes, source, M68kDecodeProfile::general_startup);
    if (const auto *decoded = std::get_if<M68kDecodedInstruction>(&result)) {
      auto operation = lift_m68k_instruction(*decoded);
      const bool lifted = !(decoded->kind != M68kInstructionKind::moveq && operation.kind == M68kIrKind::write_moveq);
      if (lifted) {
        const auto successors = m68k_control_successors(operation);
        for (const auto &successor : successors.successors) work.push_back(successor.target);
        if (successors.stacked == M68kStackedContinuationKind::call_continuation ||
            successors.stacked == M68kStackedContinuationKind::exception_continuation)
          work.push_back(successors.stacked_address);
        // Static proof of an installed handler: MOVE.L #handler,(vector).W/L into the sub processor's vector table (PRG RAM
        // $000008-$0003FF is ordinary RAM holding the sub's vectors, contract section 4). The handler becomes a root.
        if (processor.processor == Processor::sub && operation.kind == M68kIrKind::write_move &&
            operation.size == M68kMemoryAccessWidth::long_word && operation.source_ea.mode == M68kEaMode::immediate &&
            (operation.destination_ea.mode == M68kEaMode::absolute_long || operation.destination_ea.mode == M68kEaMode::absolute_word)) {
          const std::uint32_t slot = operation.destination_ea.absolute_address & 0x00FFFFFFU;
          if (slot >= 8U && slot < 0x400U && (slot & 3U) == 0U) {
            work.push_back(operation.source_ea.immediate_value);
            ++out.vector_roots;
          }
        }
        reached.decoded = *decoded;
        reached.operation = std::move(operation);
      }
    }
    out.instructions.emplace(pc, std::move(reached));
  }
  out.out_of_image = static_cast<std::uint32_t>(outside.size());
  return out;
}

std::string emit_processor(unsigned cpu, const ProcessorImages &processor, const Traversal &traversal, ProcessorEmissionStats &stats) {
  const SegaCdM68kRuntimeCEmitter emitter(cpu);
  const std::string p = "scd_p" + std::to_string(cpu);
  const std::string k = std::to_string(cpu) + "U";
  std::ostringstream out;
  out << "/* ---- processor " << cpu << " (" << processor_name(processor.processor) << "): "
      << traversal.instructions.size() << " reached instructions ---- */\n";
  out << "static const uint32_t " << p << "_entries[] = {";
  bool first = true;
  for (const auto &[pc, reached] : traversal.instructions) {
    out << (first ? "" : ",") << (first ? "" : " ") << "UINT32_C(0x" << hex(pc) << ")";
    first = false;
  }
  out << "};\n";
  out << "static const uint32_t *" << p << "_lookup(uint32_t pc) {\n"
      << "  size_t lo = 0U, hi = sizeof " << p << "_entries / sizeof " << p << "_entries[0];\n"
      << "  while (lo < hi) { const size_t mid = lo + (hi - lo) / 2U; if (" << p << "_entries[mid] == pc) return &" << p
      << "_entries[mid]; if (" << p << "_entries[mid] < pc) lo = mid + 1U; else hi = mid; }\n"
      << "  return NULL;\n}\n";
  for (const auto &[pc, reached] : traversal.instructions) {
    const std::string fn = p + "_i_" + hex(pc);
    std::optional<std::string> timing;
    std::string body;
    std::string slots;
    if (reached.operation) {
      const auto &op = *reached.operation;
      timing = m68k_timing_c_expression(op);
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
      ctx.compiled_entry_lookup_symbol = p + "_lookup";
      if (op.kind == M68kIrKind::dbcc_loop) {
        slots += "  uint8_t m68k_dbcc_took_branch = 0U;\n";
        ctx.timing_dbcc_taken = "m68k_dbcc_took_branch";
      }
      if (op.kind == M68kIrKind::set_conditional && op.destination_ea.mode == M68kEaMode::data_register) {
        slots += "  uint8_t m68k_scc_true = 0U;\n";
        ctx.timing_scc_true = "m68k_scc_true";
      }
      if (op.kind == M68kIrKind::multiply_signed_word || op.kind == M68kIrKind::multiply_unsigned_word) {
        slots += "  uint16_t m68k_timing_mul_source = UINT16_C(0);\n";
        ctx.timing_mul_source = "m68k_timing_mul_source";
      }
      if (op.kind == M68kIrKind::divide_signed_word || op.kind == M68kIrKind::divide_unsigned_word) {
        slots += "  uint32_t m68k_timing_div_dividend = UINT32_C(0); uint16_t m68k_timing_div_divisor = UINT16_C(0);\n";
        ctx.timing_div_dividend = "m68k_timing_div_dividend";
        ctx.timing_div_divisor = "m68k_timing_div_divisor";
      }
      body = emit_m68k_operation_c(op, "cpu->d", "cpu->sr", "  ", &ctx);
    }
    const bool compiled = reached.operation && timing && body.find_first_not_of(" \n") != std::string::npos;
    if (!compiled) {
      ++stats.unsupported;
      out << "static int " << fn << "(ScdMachine *m) { return scd_fail(m, " << k << ", SCD_STOP_INSTRUCTION_UNSUPPORTED, UINT32_C(0x"
          << hex(pc) << "), UINT32_C(0x" << hex(pc) << ")); }\n";
      continue;
    }
    ++stats.compiled;
    const auto &raw = reached.decoded->raw_bytes;
    out << "static const uint8_t " << fn << "_bytes[] = {";
    for (std::size_t i = 0; i < raw.size(); ++i) out << (i ? ", " : "") << "0x" << hex(raw[i], 2);
    out << "};\n";
    out << "static int " << fn << "(ScdMachine *m) {\n"
        << "  ScdCpu *const cpu = &m->cpu[" << cpu << "];\n"
        << "  uint32_t pc = cpu->pc;\n"
        << "  if (scd_code_guard(m, " << k << ", UINT32_C(0x" << hex(pc) << "), " << fn << "_bytes, " << raw.size()
        << "U) != 0) return 1;\n"
        << slots << body << "  cpu->pc = pc;\n"
        << "  return scd_retire(m, " << k << ", (uint32_t)(" << *timing << "));\n}\n";
  }
  out << "static int " << p << "_dispatch(ScdMachine *m) {\n  (void)" << p << "_lookup; /* referenced by computed control */\n"
      << "  switch (m->cpu[" << cpu << "].pc) {\n";
  for (const auto &[pc, reached] : traversal.instructions)
    out << "  case UINT32_C(0x" << hex(pc) << "): return " << p << "_i_" << hex(pc) << "(m);\n";
  out << "  default: return scd_fail(m, " << k << ", SCD_STOP_ENTRY_NOT_COMPILED, m->cpu[" << cpu << "].pc, m->cpu[" << cpu
      << "].pc);\n  }\n}\n";
  stats.out_of_image_targets = traversal.out_of_image;
  stats.vector_roots = traversal.vector_roots;
  return out.str();
}

std::string c_string(const std::string &text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) >= 0x7FU) {
      char buffer[8];
      std::snprintf(buffer, sizeof buffer, "\\%03o", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buffer;
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

}  // namespace

ProgramEmission emit_sega_cd_program(const DiscImage &disc, const BootImages &images) {
  ProgramEmission result;
  std::ostringstream out;
  const std::string identity_text = std::string(kSegaCdGeneratorVersion) + "\n" + disc.identity() + "\n";
  result.program_identity =
      sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(identity_text.data()), identity_text.size()));
  out << "/* Generated by segarecomp (" << kSegaCdGeneratorVersion << ", SEG-062). Sega CD synthetic post-BIOS profile.\n"
      << " * Disc identity " << disc.identity() << ". Do not edit. */\n"
      << "#include <stddef.h>\n#include <stdint.h>\n#include \"scd_runtime.h\"\n\n";
  const ProcessorImages *processors[2] = {&images.main, &images.sub};
  for (unsigned cpu = 0; cpu < 2; ++cpu) {
    ProcessorEmissionStats stats;
    stats.processor = processors[cpu]->processor;
    const auto traversal = traverse(*processors[cpu]);
    out << emit_processor(cpu, *processors[cpu], traversal, stats);
    result.stats.push_back(stats);
  }
  out << "static const ScdDiscFile scd_disc_files[] = {\n";
  for (const auto &file : disc.files())
    out << "  {" << c_string(file.name) << ", UINT64_C(" << file.size << "), \"" << file.sha256 << "\"},\n";
  out << "};\nstatic const ScdDiscTrack scd_disc_tracks[] = {\n";
  for (const auto &track : disc.tracks())
    out << "  {" << track.number << "U, " << static_cast<unsigned>(track.type) << "U, " << track.file_index << "U, " << track.sector_size
        << "U, UINT64_C(" << track.file_offset << "), " << track.first_lba << "U, " << track.sector_count << "U},\n";
  out << "};\n";
  const auto image_fields = [&](const ProcessorImages &p, const SegaCdBootDescriptor &d, unsigned cpu) {
    const auto &mapping = p.images.images.front().mappings.front();
    std::ostringstream text;
    text << "{scd_p" << cpu << "_entries, (uint32_t)(sizeof scd_p" << cpu << "_entries / sizeof scd_p" << cpu << "_entries[0]), scd_p"
         << cpu << "_dispatch, UINT32_C(0x" << hex(p.entry_pc) << "), UINT32_C(0x" << hex(mapping.execution_base) << "), UINT32_C(0x"
         << hex(d.disc_address) << "), UINT32_C(0x" << hex(mapping.length) << ")}";
    return text.str();
  };
  out << "static const ScdProgram scd_program = {\n  {" << image_fields(images.main, images.header.ip, 0) << ",\n   "
      << image_fields(images.sub, images.header.sp, 1) << "},\n"
      << "  {scd_disc_files, " << disc.files().size() << "U, scd_disc_tracks, " << disc.tracks().size() << "U, \"" << disc.identity()
      << "\", {0}},\n"
      << "  UINT32_C(0x" << hex(kMainInitialSsp) << "),\n  \"" << result.program_identity << "\"\n};\n\n"
      << "int main(int argc, char **argv) { return scd_program_main(&scd_program, argc, argv); }\n";
  result.c_source = out.str();
  return result;
}

}  // namespace segarecomp::machine::sega_cd
