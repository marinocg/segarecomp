// SEG-021-T003: emitter driver of the table-driven generated-native differential conformance harness.
// Reads one hexadecimal instruction encoding per stdin line, decodes/lifts/emits each through the public
// pipeline entry points (same direct linear-memory route and conformance window as the T002 capability
// probe) and writes ONE translation unit containing a function per distinct encoding plus a lookup table.
// stdout: one "<CODEHEX> <status>" line per input encoding; status is `ok` or the first failing stage
// (`undecodable`, `not_lifted`, `no_emission`, `length_mismatch`). It holds no instruction knowledge and
// never invents an emission: an unsupported encoding is reported, not approximated.
//
//   m68k_conformance_emitter --out FILE.c < encodings.txt
#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace segarecomp;

namespace {
constexpr std::uint32_t kBase = 0x00002000U;

bool parse_hex(const std::string &text, std::vector<std::uint8_t> &out) {
  if (text.empty() || text.size() % 4U != 0U) return false;  // whole 16-bit words only
  for (std::size_t i = 0; i < text.size(); i += 2U) {
    unsigned value = 0;
    for (std::size_t k = 0; k < 2U; ++k) {
      const char c = text[i + k];
      unsigned nibble;
      if (c >= '0' && c <= '9') nibble = static_cast<unsigned>(c - '0');
      else if (c >= 'A' && c <= 'F') nibble = static_cast<unsigned>(c - 'A' + 10);
      else return false;
      value = value * 16U + nibble;
    }
    out.push_back(static_cast<std::uint8_t>(value));
  }
  return true;
}
}  // namespace

int main(int argc, char **argv) {
  std::string out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    else { std::cerr << "usage: m68k_conformance_emitter --out FILE.c < encodings\n"; return 2; }
  }
  if (out_path.empty()) { std::cerr << "--out is required\n"; return 2; }

  std::ostringstream functions, table;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    std::vector<std::uint8_t> image;
    if (!parse_hex(line, image)) { std::cout << line << " undecodable\n"; continue; }
    const M68kProgramAddress pc{TargetAddressSpace::m68k_program, kBase};
    const DecodeSource source{CpuVariant::mc68000, pc, {0U}};
    const auto decoded_result =
        decode_m68k_instruction(std::span<const std::uint8_t>(image), source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&decoded_result);
    if (decoded == nullptr) { std::cout << line << " undecodable\n"; continue; }
    const auto operation = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && operation.kind == M68kIrKind::write_moveq) {
      std::cout << line << " not_lifted\n"; continue;
    }
    const std::size_t length = decoded->raw_bytes.empty() ? static_cast<std::size_t>(operation.provenance.length.value)
                                                          : decoded->raw_bytes.size();
    if (length != image.size()) { std::cout << line << " length_mismatch\n"; continue; }
    if (!m68k_operation_has_complete_c_emission(operation)) { std::cout << line << " no_emission\n"; continue; }
    M68kMemoryEmissionContext memory{"s->ram", "s->a", "frame_ids", "frame_continuations", "frame_depth", 0U,
                                     M68kOperandAccess::linear_memory, 0U, {}, {}};
    memory.linear_memory_begin = 0U;
    memory.linear_memory_end = 0x100000U;
    memory.user_stack_pointer = "s->usp";
    const auto body = emit_m68k_operation_c(operation, "s->d", "s->sr", "  ", &memory);
    if (body.empty()) { std::cout << line << " no_emission\n"; continue; }
    functions << "static int cf_" << line << "(cap_state *s) {\n  uint32_t pc = s->pc;\n" << body
              << "  s->pc = pc;\n  return 0;\n}\n";
    table << "  {\"" << line << "\", cf_" << line << "},\n";
    std::cout << line << " ok\n";
  }
  std::ofstream out(out_path);
  out << "#include <stdint.h>\n#include <stddef.h>\n"
         "typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;\n"
         "extern uint32_t frame_ids[64]; extern uint32_t frame_continuations[64]; extern uint32_t frame_depth; /* owned and reset per vector by the runner */\n"
      << functions.str()
      << "typedef struct { const char *code; int (*fn)(cap_state *); } cf_entry;\n"
         "const cf_entry cf_table[] = {\n" << table.str() << "  {NULL, NULL}\n};\n";
  return out ? 0 : 1;
}
