#include "segarecomp/codegen/c11/m68k.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace segarecomp {
namespace {
using Address = std::uint32_t;

std::string hex(std::uint64_t value, unsigned width) {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(width))
         << std::setfill('0') << value;
  return output.str();
}

std::string address_json(const M68kProgramAddress &address) {
  return "{\"space\":\"m68k_program\",\"value\":\"" + hex(address.value, 8) + "\"}";
}

std::string provenance_json(const InstructionProvenance &provenance) {
  std::ostringstream output;
  output << "{\"cpu_variant\":\"mc68000\",\"source_address\":"
         << address_json(provenance.source.address) << ",\"image_offset\":"
         << provenance.source.image_offset.value << ",\"raw_bytes\":\"";
  for (const auto byte : provenance.bytes) {
    output << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned>(byte);
  }
  output << "\",\"length\":" << std::dec << provenance.length.value << '}';
  return output.str();
}

std::string block_json(const BlockProvenance &block) {
  std::ostringstream output;
  output << "{\"id\":" << address_json(block.id.entry)
         << ",\"entry_instruction\":" << provenance_json(block.entry_instruction)
         << ",\"instructions\":[";
  for (std::size_t index = 0; index < block.instructions.size(); ++index) {
    if (index != 0U) output << ',';
    output << provenance_json(block.instructions[index]);
  }
  return output << "]}", output.str();
}

const char *edge_kind_name(DirectEdgeKind kind) {
  switch (kind) {
  case DirectEdgeKind::fallthrough: return "fallthrough";
  case DirectEdgeKind::bne_taken: return "bne_taken";
  case DirectEdgeKind::bne_fallthrough: return "bne_fallthrough";
  case DirectEdgeKind::bra_taken: return "bra_taken";
  }
  return "unknown";
}

const char *condition_name(DirectCondition condition) {
  switch (condition) {
  case DirectCondition::always: return "always";
  case DirectCondition::z_clear: return "z_clear";
  case DirectCondition::z_set: return "z_set";
  }
  return "unknown";
}

std::string edge_json(const DirectEdge &edge) {
  std::ostringstream output;
  output << "{\"source_block\":" << address_json(edge.source_block.entry)
         << ",\"source_instruction\":" << provenance_json(edge.source_instruction)
         << ",\"kind\":\"" << edge_kind_name(edge.kind) << "\",\"condition\":\""
         << condition_name(edge.condition) << "\",\"target\":" << address_json(edge.target)
         << '}';
  return output.str();
}

std::string c_string(const std::string &text) {
  std::string escaped;
  escaped.reserve(text.size() + 2U);
  for (const char character : text) {
    if (character == '\\' || character == '"') escaped.push_back('\\');
    escaped.push_back(character);
  }
  return escaped;
}

M68kIrOperation shared_operation(const DirectFlowIrOperation &operation) {
  M68kIrKind kind{M68kIrKind::write_moveq};
  switch (operation.kind) {
  case DirectFlowIrKind::write_moveq_d0: break;
  case DirectFlowIrKind::subtract_quick_long_d0: kind = M68kIrKind::subtract_quick_long_d0; break;
  case DirectFlowIrKind::branch_ne_short: kind = M68kIrKind::branch_ne_short; break;
  case DirectFlowIrKind::branch_always_short: kind = M68kIrKind::branch_always_short; break;
  }
  return {operation.provenance, kind, DataRegister::d0, operation.operand, {}, {}};
}

const DirectBlock *block_at(const DirectFlowAnalysis &analysis, const M68kProgramAddress &pc) {
  const auto found = std::find_if(analysis.blocks.begin(), analysis.blocks.end(), [&](const auto &block) {
    return block.provenance.id.entry.space == pc.space && block.provenance.id.entry.value == pc.value;
  });
  return found == analysis.blocks.end() ? nullptr : &*found;
}
} // namespace

std::string emit_m68k_structured_direct_flow_c(const DirectFlowAnalysis &analysis,
                                                std::span<const M68kDirectFlowUnit> units,
                                                const DirectFlowState &initial,
                                                std::uint64_t budget) {
  const auto validation = execute_m68k_direct_flow(analysis, initial, budget);
  if (const auto *rejection = std::get_if<RejectedDirectFlow>(&validation)) {
    return "/* translation rejected: " + format_m68k_direct_flow_rejection(*rejection) + " */\n";
  }
  std::ostringstream output;
  output << "/* Static lifted MC68000 direct-flow translation. No target image or runtime instruction interpretation is present. */\n"
         << "#include <inttypes.h>\n#include <stdint.h>\n#include <stdio.h>\n"
         << "static void print_state(const uint32_t d[8], uint32_t pc, uint16_t sr) {\n"
         << "  printf(\"{\\\"d\\\":[\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\"],\\\"pc\\\":\\\"%08\" PRIX32 \"\\\",\\\"sr\\\":\\\"%04\" PRIX32 \"\\\"}\", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], pc, (uint32_t)sr);\n}\n";
  for (const auto &unit : units) {
    const bool legacy_block = unit.id.rfind("m68k_block_", 0U) == 0U;
    output << "static " << (legacy_block ? "void " : "int ") << unit.id << "(uint32_t d[8], uint16_t *sr, uint32_t *pc, const char **block_json, const char **edge_json) {\n  (void)d; (void)sr;\n";
    for (const auto &member : unit.members) {
      const auto *block = block_at(analysis, member.entry);
      if (block == nullptr) return "/* translation rejected: missing static unit block */\n";
      const auto &last = block->operations.back();
      output << "  if (*pc == UINT32_C(" << hex(member.entry.value, 8) << ")) {\n"
             << "    *block_json = \"" << c_string(block_json(block->provenance)) << "\";\n";
      for (const auto &operation : block->operations)
        output << emit_m68k_operation_c(shared_operation(operation), "d", "*sr", "    ");
      const auto emit_edge = [&](DirectEdgeKind kind) {
        const auto found = std::find_if(analysis.edges.begin(), analysis.edges.end(), [&](const auto &edge) {
          return edge.source_block.entry.value == block->provenance.id.entry.value && edge.kind == kind;
        });
        return found == analysis.edges.end() ? std::string{} : edge_json(*found);
      };
      if (last.kind == DirectFlowIrKind::branch_ne_short) {
        output << "    if ((*sr & UINT16_C(4)) == 0U) { *edge_json = \"" << c_string(emit_edge(DirectEdgeKind::bne_taken)) << "\"; *pc = UINT32_C(" << hex(static_cast<Address>(static_cast<std::int64_t>(last.provenance.source.address.value) + 2 + last.operand), 8) << "); } else { *edge_json = \"" << c_string(emit_edge(DirectEdgeKind::bne_fallthrough)) << "\"; *pc = UINT32_C(" << hex(last.provenance.source.address.value + 2U, 8) << "); }\n";
      } else {
        const auto kind = last.kind == DirectFlowIrKind::branch_always_short ? DirectEdgeKind::bra_taken : DirectEdgeKind::fallthrough;
        const auto target = last.kind == DirectFlowIrKind::branch_always_short ? static_cast<Address>(static_cast<std::int64_t>(last.provenance.source.address.value) + 2 + last.operand) : last.provenance.source.address.value + last.provenance.length.value;
        output << "    *edge_json = \"" << c_string(emit_edge(kind)) << "\"; *pc = UINT32_C(" << hex(target, 8) << ");\n";
      }
      output << (legacy_block ? "    return;\n  }\n" : "    return 1;\n  }\n");
    }
    output << (legacy_block ? "}\n" : "  return 0;\n}\n");
  }
  output << "static int dispatch(uint32_t pc, uint32_t d[8], uint16_t *sr, uint32_t *next_pc, const char **block_json, const char **edge_json) {\n";
  if (std::none_of(units.begin(), units.end(), [](const auto &unit) { return unit.id.rfind("m68k_block_", 0U) == 0U; }))
    output << "  (void)pc;\n";
  for (const auto &unit : units) {
    if (unit.id.rfind("m68k_block_", 0U) == 0U) {
      const auto &entry = unit.members.front().entry;
      output << "  if (pc == UINT32_C(" << hex(entry.value, 8) << ")) { " << unit.id << "(d, sr, next_pc, block_json, edge_json); return 1; }\n";
    } else {
      output << "  if (" << unit.id << "(d, sr, next_pc, block_json, edge_json)) return 1;\n";
    }
  }
  output << "  return -1;\n}\nint main(void) {\n  uint32_t d[8] = {";
  for (std::size_t index = 0; index < initial.d.size(); ++index) {
    if (index != 0U) output << ',';
    output << "UINT32_C(" << hex(initial.d[index], 8) << ')';
  }
  const auto *initial_block = block_at(analysis, initial.pc);
  output << "};\n  uint32_t pc = UINT32_C(" << hex(initial.pc.value, 8) << ");\n  uint16_t sr = UINT16_C("
         << hex(initial.sr, 4) << ");\n  uint64_t executed = 0U;\n  const char *block_json = \""
         << c_string(block_json(initial_block->provenance)) << "\";\n  const char *incoming_edge = \"\\\"reset_seed\\\"\";\n"
         << "  printf(\"{\\\"boundaries\\\":[{\\\"ordinal\\\":0,\\\"block\\\":%s,\\\"incoming_edge\\\":%s,\\\"outgoing_edge\\\":null,\\\"edge\\\":null,\\\"state\\\":\", block_json, incoming_edge);\n"
          << "  print_state(d, pc, sr);\n  printf(\",\\\"executed_blocks\\\":0,\\\"budget\\\":%\" PRIu64 \",\\\"stop_reason\\\":\\\"continue\\\"}\", (uint64_t)UINT64_C(" << std::dec << budget << "));\n"
         << "  while (executed < UINT64_C(" << budget << ")) {\n    const char *outgoing_edge;\n    if (!dispatch(pc, d, &sr, &pc, &block_json, &outgoing_edge)) return 1;\n    ++executed;\n"
          << "    printf(\",{\\\"ordinal\\\":%\" PRIu64 \",\\\"block\\\":%s,\\\"incoming_edge\\\":%s,\\\"outgoing_edge\\\":%s,\\\"edge\\\":%s,\\\"state\\\":\", (uint64_t)executed, block_json, incoming_edge, outgoing_edge, outgoing_edge);\n"
          << "    print_state(d, pc, sr);\n    printf(\",\\\"executed_blocks\\\":%\" PRIu64 \",\\\"budget\\\":%\" PRIu64 \",\\\"stop_reason\\\":\\\"%s\\\"}\", (uint64_t)executed, (uint64_t)UINT64_C(" << budget << "), executed == UINT64_C(" << budget << ") ? \"instruction_budget_exhausted\" : \"continue\");\n    incoming_edge = outgoing_edge;\n"
          << "  }\n  printf(\"],\\\"final_state\\\":\"); print_state(d, pc, sr);\n  printf(\",\\\"executed_blocks\\\":%\" PRIu64 \",\\\"stop_reason\\\":\\\"instruction_budget_exhausted\\\"}\\n\", (uint64_t)executed);\n  return 0;\n}\n";
  return output.str();
}

std::string emit_m68k_direct_flow_c(const DirectFlowAnalysis &analysis, const DirectFlowState &initial,
                                     std::uint64_t budget) {
  std::vector<M68kDirectFlowUnit> units;
  for (std::size_t i = 0; i < analysis.blocks.size(); ++i) {
    M68kDirectFlowUnit unit{};
    unit.ordinal = static_cast<std::uint32_t>(i);
    unit.id = "m68k_block_" + hex(analysis.blocks[i].provenance.id.entry.value, 8).substr(2);
    unit.members.push_back(analysis.blocks[i].provenance.id);
    units.push_back(std::move(unit));
  }
  return emit_m68k_structured_direct_flow_c(analysis, units, initial, budget);
}

} // namespace segarecomp
