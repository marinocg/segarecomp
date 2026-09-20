// Test-only harness for SEG-007-T008's TST.L (xxx).L contract validation
// (docs/references/tst-l-absolute-long-contract.md).
//
// This intentionally is NOT a production API and is not linked into the
// `segarecomp` CLI: it exists solely so tests/tst_l_static_slice_test.py can
// black-box exercise the complete, already-shared production pipeline --
// decode_m68k_instruction, lift_m68k_instruction,
// m68k_resolve_absolute_test_operand, m68k_operation_effect, and
// emit_m68k_operation_c -- for exactly TST.L (xxx).L, without segarecomp
// exposing a second instruction-specific public header/model
// (include/segarecomp/tst_l.hpp and src/tst_l.cpp were removed; TST's only
// production home is the shared M68k pipeline in m68k_pipeline.hpp/.cpp).
//
// decode_m68k_instruction is called with the same shared
// M68kDecodeProfile::genesis_startup every other TST.L consumer uses
// (including the segarecomp CLI's probe-genesis-startup-decode); TST's
// decode predicate lives exactly once, in src/m68k_pipeline.cpp. This
// harness never routes through FrontendAnalysis/analyze_m68k_frontend/
// execute_m68k_frontend_startup, so it cannot be confused with (and does not
// exercise) the separate, unchanged fixed five-operation
// M68kFrontendProfile::genesis_rom_startup graph.

#include "segarecomp/codegen/c11/genesis_frontend.hpp"
#include "segarecomp/rom.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace segarecomp;

[[nodiscard]] std::string hex(std::uint64_t value, unsigned width) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
  return out.str();
}

[[nodiscard]] std::optional<std::uint64_t> parse_hex(std::string_view text, std::size_t width) {
  if (text.size() != width) return std::nullopt;
  std::uint64_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

[[nodiscard]] const char *decode_outcome_test_name(DecodeOutcome outcome) noexcept {
  switch (outcome) {
  case DecodeOutcome::decoded_moveq: return "decoded_moveq";
  case DecodeOutcome::unsupported_cpu_variant: return "unsupported_cpu_variant";
  case DecodeOutcome::unsupported_address_space: return "unsupported_address_space";
  case DecodeOutcome::odd_instruction_address: return "odd_instruction_address";
  case DecodeOutcome::truncated_instruction: return "truncated_instruction";
  case DecodeOutcome::illegal_instruction: return "illegal_instruction";
  case DecodeOutcome::valid_but_unsupported_instruction: return "valid_but_unsupported_instruction";
  }
  return "unknown";
}

} // namespace

int main(int argc, char **argv) {
  // <image> <pc-hex8> <offset-hex16> <sr-hex4> <d0..d7-hex8> [<ram-address-hex8> <ram-value-hex8>]...
  if (argc < 13 || (argc - 13) % 2 != 0) {
    std::cerr << "usage: tst_l_test_harness <image> <pc> <offset> <sr> <d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7> "
                 "[<ram-address> <ram-value>]...\n";
    return 2;
  }
  const auto pc = parse_hex(argv[2], 8);
  const auto offset = parse_hex(argv[3], 16);
  const auto sr = parse_hex(argv[4], 4);
  if (!pc || !offset || !sr) { std::cerr << "tst_l_test_harness: invalid state\n"; return 2; }
  std::array<std::uint32_t, 8> d{};
  for (std::size_t index = 0; index < d.size(); ++index) {
    const auto value = parse_hex(argv[5 + static_cast<int>(index)], 8);
    if (!value) { std::cerr << "tst_l_test_harness: invalid state\n"; return 2; }
    d[index] = static_cast<std::uint32_t>(*value);
  }
  std::vector<std::pair<std::uint32_t, std::array<std::uint8_t, 4>>> ram_seed;
  for (int index = 13; index < argc; index += 2) {
    const auto address = parse_hex(argv[index], 8);
    const auto value = parse_hex(argv[index + 1], 8);
    if (!address || !value) { std::cerr << "tst_l_test_harness: invalid ram seed\n"; return 2; }
    ram_seed.push_back({static_cast<std::uint32_t>(*address),
                        {static_cast<std::uint8_t>(*value >> 24U), static_cast<std::uint8_t>(*value >> 16U),
                         static_cast<std::uint8_t>(*value >> 8U), static_cast<std::uint8_t>(*value)}});
  }
  const auto bytes = read_binary(argv[1]);
  const M68kProgramAddress pc_address{TargetAddressSpace::m68k_program, static_cast<std::uint32_t>(*pc)};
  const DecodeSource source{CpuVariant::mc68000, pc_address, MoveqImageOffset{*offset}};

  // Steps 1-7 of the contract's precedence: the same shared decoder every
  // other genesis_startup-profile consumer uses.
  const auto decoded = decode_m68k_instruction(bytes, source, M68kDecodeProfile::genesis_startup);
  if (const auto *rejected = std::get_if<RejectedM68kDecode>(&decoded)) {
    std::cerr << "{\"category\":\"" << decode_outcome_test_name(rejected->outcome)
               << "\",\"stage\":\"decode\",\"unsupported_instruction_form\":"
               << (rejected->unsupported_instruction_form ? "true" : "false") << ",\"provenance\":";
    if (rejected->has_provenance) {
      const auto &p = rejected->provenance;
      std::cerr << "{\"source_address\":\"" << hex(p.source.address.value, 6) << "\",\"image_offset\":\""
                << hex(p.source.image_offset.value, 2) << "\",\"length\":" << p.length.value << '}';
    } else {
      std::cerr << "null";
    }
    std::cerr << ",\"complete_raw_bytes\":null,\"effective_address\":null}\n";
    return 1;
  }
  const auto &decoded_instruction = std::get<M68kDecodedInstruction>(decoded);
  if (decoded_instruction.kind != M68kInstructionKind::tst ||
      decoded_instruction.size != M68kMemoryAccessWidth::long_word ||
      decoded_instruction.source_ea.mode != M68kEaMode::absolute_long) {
    // This test-only harness's fixture set only ever exercises TST.L
    // (xxx).L and its fail-closed neighbors; any other successfully
    // decoded kind means the fixture set changed and this harness needs
    // updating, not a silent fallback.
    std::cerr << "tst_l_test_harness: decoded an unexpected instruction kind for this fixture set\n";
    return 3;
  }
  const auto lifted = lift_m68k_instruction(decoded_instruction);

  // Step 8 ("Data-read region resolution"): reuses the shared Batch-A EA
  // fact and the same `bytes` the instruction itself was decoded from.
  // instruction itself was decoded from, matching the contract's identity
  // mapping (image_offset == address) for a raw_cartridge_rom operand.
  const auto resolution = m68k_resolve_absolute_test_operand(
      bytes, decoded_instruction.source_ea.absolute_address, M68kMemoryAccessWidth::long_word,
      M68kMemoryAccessDirection::read, decoded_instruction.provenance);
  const auto resolution_diagnostic = [&]() -> std::optional<DirectFlowDiagnostic> {
    if (const auto *diagnostic = std::get_if<DirectFlowDiagnostic>(&resolution)) return *diagnostic;
    if (const auto *failure = std::get_if<M68kControllerIoFailure>(&resolution)) return failure->category;
    return std::nullopt;
  }();
  if (resolution_diagnostic) {
    std::cerr << "{\"category\":\"" << m68k_direct_flow_diagnostic_name(*resolution_diagnostic)
               << "\",\"stage\":\"effective_address\",\"unsupported_instruction_form\":false,\"provenance\":"
               << "{\"source_address\":\"" << hex(decoded_instruction.provenance.source.address.value, 6)
               << "\",\"image_offset\":\"" << hex(decoded_instruction.provenance.source.image_offset.value, 2)
               << "\",\"length\":" << decoded_instruction.provenance.length.value << "},\"complete_raw_bytes\":\"";
    for (const auto byte : decoded_instruction.raw_bytes) std::cerr << hex(byte, 2).substr(2);
    std::cerr << "\",\"effective_address\":\"" << hex(decoded_instruction.source_ea.absolute_address, 8)
              << "\"}\n";
    return 1;
  }
  const auto &operand = std::get<M68kAbsoluteTestOperand>(resolution);

  // The real shared m68k_operation_effect/emit_m68k_operation_c production
  // path -- exercised the same way probe-genesis-startup-decode exercises
  // it, and never a second semantic implementation.
  (void)m68k_operation_effect(lifted);

  std::ostringstream out;
  out << "/* Static MC68000 TST.L (xxx).L translation (test harness); target image is not present. */\n"
      << "#include <inttypes.h>\n#include <stdint.h>\n#include <stdio.h>\n\n"
      << "const uint32_t segarecomp_tst_l_source_address = UINT32_C(0x" << std::uppercase << std::hex
      << std::setw(8) << std::setfill('0') << decoded_instruction.provenance.source.address.value << ");\n"
      << "const uint64_t segarecomp_tst_l_image_offset = UINT64_C(0x" << std::setw(16)
      << decoded_instruction.provenance.source.image_offset.value << ");\n"
      << "const uint32_t segarecomp_tst_l_byte_length = UINT32_C(6);\n"
      << "const char segarecomp_tst_l_cpu_variant[] = \"mc68000\";\n\n"
      << "int main(void) {\n  uint32_t d[8] = {";
  for (std::size_t index = 0; index < d.size(); ++index) {
    if (index != 0U) out << ", ";
    out << "UINT32_C(0x" << std::setw(8) << d[index] << ")";
  }
  out << "};\n  uint16_t sr = UINT16_C(0x" << std::setw(4) << static_cast<unsigned>(*sr) << ");\n"
      << "  uint32_t pc = UINT32_C(0x" << std::setw(8) << static_cast<std::uint32_t>(*pc) << ");\n"
      << "  uint8_t ram[65536] = {0};\n  (void)ram;\n";
  if (operand.region == M68kAbsoluteOperandRegion::synthetic_work_ram) {
    for (const auto &[address, ram_bytes] : ram_seed) {
      const auto ram_offset = m68k_startup_ram_offset(address);
      for (std::size_t i = 0; i < ram_bytes.size(); ++i) {
        out << "  ram[UINT32_C(" << std::dec << ram_offset + i << ")] = UINT8_C(0x" << std::uppercase << std::hex
            << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ram_bytes[i]) << ");\n";
      }
    }
  }
  GenesisM68kEmissionContext memory{};
  memory.ram_array = "ram";
  memory.test_operand_access = genesis_lowering_access(operand.region);
  memory.test_operand_value = operand.value;
  out << emit_m68k_operation_c(lifted, "d", "sr", "  ", &memory);
  out << "  return printf(\"{\\\"schema\\\":1,\\\"d\\\":[\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 "
         "\"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 "
         "\"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\"],\\\"pc\\\":\\\"%08\" PRIX32 "
         "\"\\\",\\\"sr\\\":\\\"%04\" PRIX16 \"\\\",\\\"stop_reason\\\":\\\"instruction_budget_exhausted\\\"}\\n\", "
         "d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], pc, sr) < 0;\n}\n";
  std::cout << out.str();
  return 0;
}
