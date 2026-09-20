#include "segarecomp/codegen/c11/provenance_diagnostics.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace segarecomp {

M68kProvenanceDiagnosticProjection build_m68k_provenance_diagnostic_projection(
    std::string_view image_sha256, const std::vector<M68kDecodedInstruction> &decoded,
    const std::vector<M68kStaticBlock> &blocks, const std::vector<M68kStaticEdge> &edges,
    const std::vector<M68kStaticCall> &calls) {
  M68kProvenanceDiagnosticProjection projection;
  projection.image_sha256 = std::string(image_sha256);
  std::map<std::uint32_t, std::uint32_t> forms;
  for (const auto &instruction : decoded)
    forms.emplace(instruction.provenance.source.address.value, static_cast<std::uint32_t>(instruction.kind));
  projection.closure.blocks = blocks.size();
  projection.closure.edges = edges.size();
  projection.closure.calls = calls.size();
  for (const auto &block : blocks) {
    projection.closure.instructions += block.instructions.size();
    for (const auto &instruction : block.instructions) {
      M68kProvenanceDiagnosticEntry entry;
      entry.cpu = instruction.source.cpu_variant;
      entry.guest_pc = instruction.source.address.value;
      entry.image_offset = instruction.source.image_offset.value;
      entry.block_entry = block.id.entry.value;
      if (const auto found = forms.find(entry.guest_pc); found != forms.end()) entry.form_id = found->second;
      projection.entries.push_back(entry);
    }
  }
  std::sort(projection.entries.begin(), projection.entries.end(), [](const auto &a, const auto &b) {
    return a.guest_pc != b.guest_pc ? a.guest_pc < b.guest_pc : a.block_entry < b.block_entry;
  });
  projection.entries.erase(std::unique(projection.entries.begin(), projection.entries.end()), projection.entries.end());
  return projection;
}

std::string emit_m68k_provenance_diagnostic_c(const M68kProvenanceDiagnosticProjection &projection) {
  std::ostringstream out;
  out << "\n/* SEG-020-T002 provenance diagnostics (opt-in; no raw bytes) */\n"
      << "#include <stddef.h>\n#include <stdint.h>\n"
      << "typedef struct segarecomp_provenance_diag_entry { uint32_t guest_pc; uint32_t image_offset; uint32_t block_entry; uint32_t form_id; } segarecomp_provenance_diag_entry;\n"
      << "const char segarecomp_provenance_diag_cpu[] = \"mc68000\";\n"
      << "const char segarecomp_provenance_diag_image_sha256[] = \"" << projection.image_sha256 << "\";\n"
      << "const uint64_t segarecomp_static_closure_blocks = " << projection.closure.blocks << "ULL;\n"
      << "const uint64_t segarecomp_static_closure_instructions = " << projection.closure.instructions << "ULL;\n"
      << "const uint64_t segarecomp_static_closure_edges = " << projection.closure.edges << "ULL;\n"
      << "const uint64_t segarecomp_static_closure_calls = " << projection.closure.calls << "ULL;\n"
      << "const size_t segarecomp_provenance_diag_count = " << projection.entries.size() << "U;\n"
      << "const segarecomp_provenance_diag_entry segarecomp_provenance_diag_table[" << (projection.entries.empty() ? 1U : projection.entries.size()) << "] = {\n";
  if (projection.entries.empty()) out << "  {0U, 0U, 0U, 0xFFFFFFFFU}\n";
  for (const auto &entry : projection.entries)
    out << "  {0x" << std::hex << entry.guest_pc << "U, 0x" << static_cast<std::uint32_t>(entry.image_offset) << "U, 0x"
        << entry.block_entry << "U, 0x" << entry.form_id << "U},\n" << std::dec;
  out << "};\n";
  return out.str();
}

} // namespace segarecomp
