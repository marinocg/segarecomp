// Test-only harness for SEG-007-T023's common MC68000 startup/data-movement
// batch (docs/references/m68k-common-startup-data-movement-batch-contract.md),
// modeled directly on tests/tools/tst_l_test_harness.cpp.
//
// This intentionally is NOT a production API and is not linked into the
// `segarecomp` CLI: it exists solely so tests/m68k_batch_a_static_slice_test.py
// and tests/m68k_batch_a_adversarial_test.py can black-box exercise the real,
// already-shared production pipeline -- decode_m68k_instruction,
// lift_m68k_instruction, m68k_resolve_absolute_test_operand,
// m68k_operation_effect, and emit_m68k_operation_c -- for the whitelisted
// TST/MOVE/MOVEA/CLR/LEA/JMP/JSR forms, without a second decode/executor/
// emitter implementation.
//
// decode_m68k_instruction is called with the same shared
// M68kDecodeProfile::general_startup every other batch-A consumer uses
// (discover_m68k_general_startup); the decode predicate itself lives exactly
// once, in src/m68k_pipeline.cpp. This harness never routes through
// FrontendAnalysis/analyze_m68k_frontend/discover_m68k_general_startup, so it
// cannot be confused with (and does not exercise) that separate bounded
// static-discovery route.
//
// Like tst_l_test_harness.cpp, this harness performs its OWN single
// resolution of a statically-foldable absolute/pc-relative SOURCE operand
// (via the shared m68k_resolve_absolute_test_operand), using the same image
// bytes the instruction itself was decoded from (identity mapping,
// image_offset == address, exactly as the TST.L harness's own comment
// documents). A statically-foldable absolute DESTINATION (MOVE/CLR store) is
// resolved with the same alignment/range policy discover_m68k_general_startup
// applies (reusing only the existing public alignment/range predicates,
// never a second policy): ROM writes are always rejected, and any address
// inside the supplied image is treated as ROM-resident for that prohibition,
// consistent with this harness's identity-mapped single-image convention.

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

void print_decode_rejection(const RejectedM68kDecode &rejected) {
  std::cerr << "{\"category\":\"" << decode_outcome_test_name(rejected.outcome)
             << "\",\"stage\":\"decode\",\"unsupported_instruction_form\":"
             << (rejected.unsupported_instruction_form ? "true" : "false") << ",\"provenance\":";
  if (rejected.has_provenance) {
    const auto &p = rejected.provenance;
    std::cerr << "{\"source_address\":\"" << hex(p.source.address.value, 6) << "\",\"image_offset\":\""
               << hex(p.source.image_offset.value, 2) << "\",\"length\":" << p.length.value << '}';
  } else {
    std::cerr << "null";
  }
  std::cerr << ",\"effective_address\":null}\n";
}

void print_ea_rejection(DirectFlowDiagnostic category, const M68kDecodedInstruction &decoded, std::uint32_t address) {
  std::cerr << "{\"category\":\"" << m68k_direct_flow_diagnostic_name(category)
             << "\",\"stage\":\"effective_address\",\"unsupported_instruction_form\":false,\"provenance\":"
             << "{\"source_address\":\"" << hex(decoded.provenance.source.address.value, 6) << "\",\"image_offset\":\""
             << hex(decoded.provenance.source.image_offset.value, 2)
             << "\",\"length\":" << decoded.provenance.length.value << "},\"effective_address\":\"" << hex(address, 8)
             << "\"}\n";
}

// Resolves a single statically-foldable absolute/pc-relative operand exactly
// as discover_m68k_general_startup's own m68k_resolve_static_general_operand
// does, reusing only the existing public predicates (never a second policy).
// Returns nullopt both when the mode isn't statically-foldable (nothing to
// resolve) and when resolution succeeds; `*category` is set only on failure.
[[nodiscard]] std::optional<M68kAbsoluteTestOperand> resolve_source_if_static(
    std::span<const std::uint8_t> image, const M68kEffectiveAddress &ea, M68kMemoryAccessWidth size,
    const InstructionProvenance &provenance, std::optional<DirectFlowDiagnostic> *failure) {
  if (ea.mode != M68kEaMode::absolute_word && ea.mode != M68kEaMode::absolute_long && ea.mode != M68kEaMode::pc_disp16)
    return std::nullopt;
  const auto resolution = m68k_resolve_absolute_test_operand(
      image, m68k_canonical_ea_address(ea), size, M68kMemoryAccessDirection::read, provenance);
  if (const auto *diagnostic = std::get_if<DirectFlowDiagnostic>(&resolution)) { *failure = *diagnostic; return std::nullopt; }
  if (const auto *controller_failure = std::get_if<M68kControllerIoFailure>(&resolution)) {
    *failure = controller_failure->category;
    return std::nullopt;
  }
  return std::get<M68kAbsoluteTestOperand>(resolution);
}

[[nodiscard]] std::optional<DirectFlowDiagnostic> validate_destination_if_static(
    std::span<const std::uint8_t> image, const M68kEffectiveAddress &ea, M68kMemoryAccessWidth width) {
  if (ea.mode != M68kEaMode::absolute_word && ea.mode != M68kEaMode::absolute_long && ea.mode != M68kEaMode::pc_disp16)
    return std::nullopt;
  const auto address = m68k_canonical_ea_address(ea);
  if (const auto misaligned = m68k_startup_absolute_operand_alignment(address, width)) return *misaligned;
  if (static_cast<std::uint64_t>(address) < image.size()) return DirectFlowDiagnostic::rom_write_prohibited;
  if (!m68k_startup_ram_range_in_range(address, static_cast<std::uint32_t>(width)))
    return DirectFlowDiagnostic::unmapped_data_access;
  return std::nullopt;
}

} // namespace

int main(int argc, char **argv) {
  // <image> <pc-hex8> <offset-hex16> <sr-hex4> <d0..d7-hex8> <a0..a7-hex8> [<ram-address-hex8> <ram-value-hex8>]...
  if (argc < 21 || (argc - 21) % 2 != 0) {
    std::cerr << "usage: m68k_batch_a_test_harness <image> <pc> <offset> <sr> "
                 "<d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7> <a0> <a1> <a2> <a3> <a4> <a5> <a6> <a7> "
                 "[<ram-address> <ram-value>]...\n";
    return 2;
  }
  const auto pc = parse_hex(argv[2], 8);
  const auto offset = parse_hex(argv[3], 16);
  const auto sr = parse_hex(argv[4], 4);
  if (!pc || !offset || !sr) { std::cerr << "m68k_batch_a_test_harness: invalid state\n"; return 2; }
  std::array<std::uint32_t, 8> d{};
  for (std::size_t index = 0; index < d.size(); ++index) {
    const auto value = parse_hex(argv[5 + static_cast<int>(index)], 8);
    if (!value) { std::cerr << "m68k_batch_a_test_harness: invalid state\n"; return 2; }
    d[index] = static_cast<std::uint32_t>(*value);
  }
  std::array<std::uint32_t, 8> a{};
  for (std::size_t index = 0; index < a.size(); ++index) {
    const auto value = parse_hex(argv[13 + static_cast<int>(index)], 8);
    if (!value) { std::cerr << "m68k_batch_a_test_harness: invalid state\n"; return 2; }
    a[index] = static_cast<std::uint32_t>(*value);
  }
  std::vector<std::pair<std::uint32_t, std::array<std::uint8_t, 4>>> ram_seed;
  for (int index = 21; index < argc; index += 2) {
    const auto address = parse_hex(argv[index], 8);
    const auto value = parse_hex(argv[index + 1], 8);
    if (!address || !value) { std::cerr << "m68k_batch_a_test_harness: invalid ram seed\n"; return 2; }
    ram_seed.push_back({static_cast<std::uint32_t>(*address),
                        {static_cast<std::uint8_t>(*value >> 24U), static_cast<std::uint8_t>(*value >> 16U),
                         static_cast<std::uint8_t>(*value >> 8U), static_cast<std::uint8_t>(*value)}});
  }
  const auto bytes = read_binary(argv[1]);
  const M68kProgramAddress pc_address{TargetAddressSpace::m68k_program, static_cast<std::uint32_t>(*pc)};
  const DecodeSource source{CpuVariant::mc68000, pc_address, MoveqImageOffset{*offset}};

  const auto decoded = decode_m68k_instruction(bytes, source, M68kDecodeProfile::general_startup);
  if (const auto *rejected = std::get_if<RejectedM68kDecode>(&decoded)) {
    print_decode_rejection(*rejected);
    return 1;
  }
  const auto &decoded_instruction = std::get<M68kDecodedInstruction>(decoded);
  const auto lifted = lift_m68k_instruction(decoded_instruction);

  GenesisM68kEmissionContext memory{};
  memory.ram_array = "ram";
  memory.address_registers = "a";
  memory.frame_ids_array = "frame_ids";
  memory.frame_continuations_array = "frame_continuations";
  memory.frame_depth = "frame_depth";
  memory.continuation = 0U;

  if (decoded_instruction.kind == M68kInstructionKind::lea ||
            decoded_instruction.kind == M68kInstructionKind::jmp ||
            decoded_instruction.kind == M68kInstructionKind::jsr) {
    // LEA never accesses memory at all (it only computes an address value;
    // no bounds check applies, matching real MC68000 LEA and
    // emit_m68k_operation_c's own unconditional-fold case). JMP/JSR's
    // target-mapping validation belongs to discover_m68k_general_startup's
    // own walk() (which this test-only harness deliberately never calls,
    // exactly like the TST.L precedent never calls
    // execute_m68k_frontend_startup); emit_m68k_operation_c's jump_general/
    // call_general case likewise folds a statically-foldable target
    // unconditionally. Nothing to resolve here for any of the three.
  } else {
    const auto source_ea = decoded_instruction.source_ea;
    std::optional<DirectFlowDiagnostic> failure;
    const auto source_resolution = resolve_source_if_static(
        bytes, source_ea, decoded_instruction.size, decoded_instruction.provenance, &failure);
    if (failure) {
      print_ea_rejection(*failure, decoded_instruction, source_ea.absolute_address);
      return 1;
    }
    if (source_resolution) {
      memory.test_operand_access = genesis_lowering_access(source_resolution->region);
      memory.test_operand_value = source_resolution->value;
    }
    // MOVEA's destination is always the fixed An register (never memory);
    // MOVE is the only kind with a genuinely independent, potentially-static
    // destination position.
    if (decoded_instruction.kind == M68kInstructionKind::move ||
        decoded_instruction.kind == M68kInstructionKind::clr) {
      if (const auto dest_failure =
              validate_destination_if_static(bytes, decoded_instruction.destination_ea, decoded_instruction.size)) {
        print_ea_rejection(*dest_failure, decoded_instruction, decoded_instruction.destination_ea.absolute_address);
        return 1;
      }
    }
  }

  std::ostringstream out;
  out << "/* Static MC68000 batch-A translation (test harness); target image is not present. */\n"
      << "#include <inttypes.h>\n#include <stdint.h>\n#include <stdio.h>\n\nint main(void) {\n  uint32_t d[8] = {";
  for (std::size_t index = 0; index < d.size(); ++index) {
    if (index != 0U) out << ", ";
    out << "UINT32_C(0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << d[index] << ")";
  }
  out << "};\n  uint32_t a[8] = {";
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (index != 0U) out << ", ";
    out << "UINT32_C(0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << a[index] << ")";
  }
  out << "};\n  uint16_t sr = UINT16_C(0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
      << static_cast<unsigned>(*sr) << ");\n"
      << "  uint32_t pc = UINT32_C(0x" << std::setw(8) << static_cast<std::uint32_t>(*pc) << ");\n"
      << "  uint8_t ram[65536] = {0}; (void)ram;\n"
      << "  uint32_t frame_ids[1] = {0U}; uint32_t frame_continuations[1] = {0U}; uint32_t frame_depth = 0U;\n"
      << "  (void)frame_ids; (void)frame_continuations; (void)frame_depth;\n";
  for (const auto &[address, ram_bytes] : ram_seed) {
    const auto ram_offset = m68k_startup_ram_offset(address);
    for (std::size_t i = 0; i < ram_bytes.size(); ++i) {
      out << "  ram[UINT32_C(" << std::dec << ram_offset + i << ")] = UINT8_C(0x" << std::uppercase << std::hex
          << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ram_bytes[i]) << ");\n";
    }
  }
  out << emit_m68k_operation_c(lifted, "d", "sr", "  ", &memory);
  out << "  return printf(\"{\\\"schema\\\":1,\\\"d\\\":[\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" "
         "PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 "
         "\"\\\",\\\"%08\" PRIX32 \"\\\"],\\\"a\\\":[\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" "
         "PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 "
         "\"\\\",\\\"%08\" PRIX32 \"\\\"],\\\"pc\\\":\\\"%08\" PRIX32 \"\\\",\\\"sr\\\":\\\"%04\" PRIX16 "
         "\"\\\",\\\"stop_reason\\\":\\\"instruction_budget_exhausted\\\"}\\n\", "
         "d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], pc, sr) < 0;\n"
         "}\n";
  std::cout << out.str();
  return 0;
}
