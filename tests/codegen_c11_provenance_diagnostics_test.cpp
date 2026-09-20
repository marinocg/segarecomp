// SEG-020-T002: provenance diagnostic projection (synthetic; no commercial data).
#include "segarecomp/codegen/c11/provenance_diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <type_traits>

using namespace segarecomp;

// Generic/core provenance must not gain an M68k-specific form/semantic field.
template <typename T> concept HasForm = requires(T t) { t.form; } || requires(T t) { t.form_id; } || requires(T t) { t.kind; };
static_assert(!HasForm<InstructionProvenance>);
static_assert(!HasForm<DecodeSource>);

#define CHECK(c) do { if (!(c)) { std::cerr << "FAILED: " #c " line " << __LINE__ << '\n'; std::exit(1); } } while (0)

static InstructionProvenance prov(std::uint32_t pc, std::uint8_t b0) {
  InstructionProvenance p;
  p.source.address.value = pc;
  p.source.image_offset.value = pc;
  p.bytes = {b0, 0x5A};  // must never appear in output
  return p;
}

int main() {
  std::vector<M68kDecodedInstruction> decoded(2);
  decoded[0].provenance = prov(0x200, 0xAB); decoded[0].kind = M68kInstructionKind::moveq;
  decoded[1].provenance = prov(0x202, 0xCD); decoded[1].kind = M68kInstructionKind::rts;
  std::vector<M68kStaticBlock> blocks(1);
  blocks[0].id.entry.value = 0x200;
  blocks[0].instructions = {prov(0x202, 0xCD), prov(0x200, 0xAB)};
  const std::string a(64U, 'a'), b(64U, 'b');
  const auto p1 = build_m68k_provenance_diagnostic_projection(a, decoded, blocks, {}, {});
  const auto p2 = build_m68k_provenance_diagnostic_projection(a, decoded, blocks, {}, {});
  const auto other = build_m68k_provenance_diagnostic_projection(b, decoded, blocks, {}, {});
  CHECK(p1.entries.size() == 2U && p1.entries[0].guest_pc == 0x200U && p1.entries[1].guest_pc == 0x202U);
  CHECK(p1.entries[0].form_id == static_cast<std::uint32_t>(M68kInstructionKind::moveq));
  CHECK(p1.entries[1].form_id == static_cast<std::uint32_t>(M68kInstructionKind::rts));
  CHECK(p1.entries[0].block_entry == 0x200U);
  CHECK(p1.closure.blocks == 1U && p1.closure.instructions == 2U && p1.closure.edges == 0U);
  const auto c1 = emit_m68k_provenance_diagnostic_c(p1);
  CHECK(c1 == emit_m68k_provenance_diagnostic_c(p2));
  CHECK(c1 != emit_m68k_provenance_diagnostic_c(other));  // image identity distinguishes images
  CHECK(c1.find(a) != std::string::npos);
  CHECK(c1.find("0xab") == std::string::npos && c1.find("0xcd") == std::string::npos && c1.find("0x5a") == std::string::npos);
  CHECK(c1.find("0xAB") == std::string::npos && c1.find("0x5A") == std::string::npos);
  // Image offsets above 32 bits must be preserved exactly.
  blocks[0].instructions[0].source.image_offset.value = UINT64_C(0x0000000100000123);
  const auto wide = emit_m68k_provenance_diagnostic_c(build_m68k_provenance_diagnostic_projection(a, decoded, blocks, {}, {}));
  CHECK(wide.find("uint64_t image_offset;") != std::string::npos);
  CHECK(wide.find("0x100000123ULL") != std::string::npos);
  return 0;
}
