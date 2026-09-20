// Test-only Batch C (SEG-007-T025) harness: shared decode/lift/emission
// only, modeled on tests/tools/m68k_batch_b_test_harness.cpp. This is the
// one Batch-C differential harness; C1 (SWAP/EXT.W/EXT.L) is the only
// accepted kind set below. Later Batch-C checkpoints (C2-C7) extend the
// accepted-kind list and, where they need memory, reuse the same
// resolve_static/memory-emission-context route already exercised by the
// shared Batch-A/B harnesses -- this file is not rewritten per checkpoint.
#include "segarecomp/codegen/c11/genesis_frontend.hpp"
#include "segarecomp/rom.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

using namespace segarecomp;
namespace {
std::optional<std::uint32_t> hex(std::string_view text) {
  if (text.empty() || text.size() > 8U) return std::nullopt;
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? std::optional{value} : std::nullopt;
}
}

int main(int argc, char **argv) {
  // IMAGE PC SR D0..D7 A0..A7 RESERVED [RAM_ADDRESS RAM_LONG]...
  // Keep the established Batch-B harness argument contract so every
  // Batch-C checkpoint's Musashi runner/differential driver can reuse this
  // one harness unmodified.
  if (argc < 21 || (argc - 21) % 2 != 0) return 2;
  const auto pc = hex(argv[2]);
  const auto sr = hex(argv[3]);
  if (!pc || !sr || *sr > UINT16_MAX) return 2;
  std::array<std::uint32_t, 8> d{};
  std::array<std::uint32_t, 8> a{};
  for (std::size_t index = 0; index < d.size(); ++index) {
    const auto value = hex(argv[4 + index]);
    if (!value) return 2;
    d[index] = *value;
  }
  for (std::size_t index = 0; index < a.size(); ++index) {
    const auto value = hex(argv[12 + index]);
    if (!value) return 2;
    a[index] = *value;
  }
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ram_seeds;
  for (int index = 21; index < argc; index += 2) {
    const auto address = hex(argv[index]);
    const auto value = hex(argv[index + 1]);
    if (!address || !value || !m68k_startup_ram_range_in_range(*address, 4U)) return 2;
    ram_seeds.emplace_back(*address, *value);
  }

  const auto image = read_binary(argv[1]);
  const DecodeSource source{CpuVariant::mc68000, {TargetAddressSpace::m68k_program, *pc}, {0}};
  const auto decoded = decode_m68k_instruction(image, source, M68kDecodeProfile::general_startup);
  if (!std::holds_alternative<M68kDecodedInstruction>(decoded)) return 1;
  const auto operation = lift_m68k_instruction(std::get<M68kDecodedInstruction>(decoded));
  // C1 (SWAP/EXT.W/EXT.L) + C2 (PEA/LINK/UNLK) + C3 (BTST/BCHG/BCLR/BSET) +
  // C4a (general BRA/Bcc branch) + C4b (BSR) + C4c (DBcc) + C5 (MOVEM) +
  // C6a (LSL/LSR register forms) + SEG-007-T114 NOP (no_operation) accepted
  // kinds. Later checkpoints add their own kinds here.
  if (operation.kind != M68kIrKind::write_swap && operation.kind != M68kIrKind::sign_extend_word &&
      operation.kind != M68kIrKind::sign_extend_long && operation.kind != M68kIrKind::push_effective_address &&
      operation.kind != M68kIrKind::link_frame && operation.kind != M68kIrKind::unlink_frame &&
      operation.kind != M68kIrKind::bit_test && operation.kind != M68kIrKind::bit_change &&
      operation.kind != M68kIrKind::bit_clear && operation.kind != M68kIrKind::bit_set &&
      operation.kind != M68kIrKind::general_branch && operation.kind != M68kIrKind::bsr_call &&
      operation.kind != M68kIrKind::dbcc_loop && operation.kind != M68kIrKind::movem_transfer &&
       operation.kind != M68kIrKind::shift_rotate_register && operation.kind != M68kIrKind::shift_rotate_memory &&
       operation.kind != M68kIrKind::no_operation && operation.kind != M68kIrKind::read_status_register &&
       operation.kind != M68kIrKind::write_condition_codes && operation.kind != M68kIrKind::negate_word)
    return 1;

  GenesisM68kEmissionContext memory{};
  memory.ram_array = "ram";
  memory.address_registers = "a";
  memory.frame_ids_array = "frame_ids";
  memory.frame_continuations_array = "frame_continuations";
  memory.frame_depth = "frame_depth";
  // BSR's real production emission (matching JSR's own established pattern:
  // the emission context's `continuation` is externally supplied by the
  // caller who already discovered/verified the call's continuation, not
  // recomputed inside emit_m68k_operation_c) needs the ACTUAL continuation
  // for THIS single instruction to make a meaningful single-instruction
  // execution test possible; every other accepted kind ignores this field,
  // so 0 remains a harmless default for them.
  memory.continuation = operation.kind == M68kIrKind::bsr_call
                             ? *pc + operation.provenance.length.value
                             : 0U;
  // Reuse the shared resolver for every static access, even though no C1
  // kind ever resolves anything (their sole operand is always Dn): this
  // keeps the harness identical to the Batch-B shape for later checkpoints
  // that do have statically-foldable operands.
  const auto resolve_static = [&](const M68kEffectiveAddress &ea,
                                  M68kMemoryAccessDirection direction) -> bool {
    if (ea.mode != M68kEaMode::absolute_word && ea.mode != M68kEaMode::absolute_long &&
        ea.mode != M68kEaMode::pc_disp16) return true;
    const auto resolution = m68k_resolve_absolute_test_operand(
        image, m68k_canonical_ea_address(ea), operation.size,
        direction, operation.provenance);
    const auto *operand = std::get_if<M68kAbsoluteTestOperand>(&resolution);
    if (operand == nullptr) return false;
    memory.test_operand_access = genesis_lowering_access(operand->region);
    memory.test_operand_value = operand->value;
    return true;
  };
  // PEA never performs a memory READ at its source EA at all -- it computes
  // an address and pushes that address (contract: "PEA is address
  // calculation, not operand read"). Its production emission never consumes
  // memory.test_operand_region/value, so resolving it here as though it were
  // an ordinary TST-style memory read would spuriously reject a legal PEA
  // whose statically-foldable address does not happen to land in any
  // resolvable region.
  if (operation.kind != M68kIrKind::push_effective_address) {
    if (!resolve_static(operation.source_ea, M68kMemoryAccessDirection::read)) return 1;
    if (!resolve_static(operation.destination_ea, M68kMemoryAccessDirection::read)) return 1;
    // BTST is read-only (mirrors production: only a READ resolution is ever
    // requested, even for a statically-foldable destination). BCHG/BCLR/
    // BSET modify their destination, so they additionally need a WRITE
    // resolution -- mirroring the exact production role split established
    // by discover_m68k_general_startup's own static resolver above (see
    // m68k_pipeline_frontend.cpp). The bit-number source_ea is never memory
    // (always Dn or immediate), so it is never resolved against a bus
    // region here, matching `resolve_static`'s own no-op for a
    // non-statically-foldable EA.
    const bool bit_operation_mutates = operation.kind == M68kIrKind::bit_change ||
        operation.kind == M68kIrKind::bit_clear || operation.kind == M68kIrKind::bit_set;
    if (bit_operation_mutates && !resolve_static(operation.destination_ea, M68kMemoryAccessDirection::write))
      return 1;
    // SEG-007-T025 (Batch C, C7a): a memory-form shift/rotate always
    // mutates its destination (unlike BTST above), so it needs the same
    // additional WRITE resolution BCHG/BCLR/BSET already require -- the
    // READ resolution already happened via the general destination_ea call
    // above, matching production's own read-then-write precedence.
    if (operation.kind == M68kIrKind::shift_rotate_memory &&
        !resolve_static(operation.destination_ea, M68kMemoryAccessDirection::write))
      return 1;
    // SEG-007-T025 (Batch C, C5a): MOVEM's own N-transfer generalization of
    // the single-value resolution above. A statically-foldable
    // memory_to_registers source needs one resolution PER SELECTED
    // TRANSFER (never just the base address), through the exact same shared
    // resolver, so a ROM-resident source can be lowered as a genuinely
    // per-transfer already-verified constant (matching production's own
    // per-transfer static routing in discover_m68k_general_startup) --
    // register_to_memory never needs this (an absolute destination is
    // always treated as synthetic_work_ram at emission time, matching every
    // other selected mutating memory form's established convention).
    if (operation.kind == M68kIrKind::movem_transfer &&
        operation.movem_direction == M68kMovemDirection::memory_to_registers &&
        (operation.source_ea.mode == M68kEaMode::absolute_word ||
         operation.source_ea.mode == M68kEaMode::absolute_long ||
         operation.source_ea.mode == M68kEaMode::pc_disp16)) {
      const auto order = m68k_movem_transfer_order(operation.movem_register_mask, M68kMovemTransferOrder::ascending);
      const auto base = m68k_canonical_ea_address(operation.source_ea);
      const auto width = static_cast<std::uint32_t>(operation.size);
      for (std::size_t slot = 0; slot < order.size(); ++slot) {
        const auto address = base + static_cast<std::uint32_t>(slot) * width;
        const auto resolution = m68k_resolve_absolute_test_operand(
            image, address, operation.size, M68kMemoryAccessDirection::read, operation.provenance);
        const auto *operand = std::get_if<M68kAbsoluteTestOperand>(&resolution);
        if (operand == nullptr) return 1;
        memory.movem_transfer_access.push_back(genesis_lowering_access(operand->region));
        memory.movem_transfer_values.push_back(operand->value);
      }
    }
  }
  std::ostringstream out;
  out << "#include <stdint.h>\n#include <stdio.h>\nint main(void) {\n  uint32_t d[8]={";
  for (std::size_t index = 0; index < d.size(); ++index) out << (index ? "," : "") << "UINT32_C(0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << d[index] << ")";
  out << "};\n  uint32_t a[8]={";
  for (std::size_t index = 0; index < a.size(); ++index) out << (index ? "," : "") << "UINT32_C(0x" << std::setw(8) << a[index] << ")";
  out << "};\n  uint16_t sr=UINT16_C(0x" << std::setw(4) << *sr << "); uint32_t pc=UINT32_C(0x" << std::setw(8) << *pc << ");\n"
      << "  uint8_t ram[65536]={[0]=UINT8_C(0x12),[1]=UINT8_C(0x34),[2]=UINT8_C(0x56),[3]=UINT8_C(0x78)}; uint32_t frame_ids[1]={0},frame_continuations[1]={0},frame_depth=0; (void)ram; (void)frame_ids; (void)frame_continuations; (void)frame_depth;\n";
  for (const auto &[address, value] : ram_seeds) {
    const auto offset = m68k_startup_ram_offset(address);
    for (unsigned byte = 0; byte < 4U; ++byte) out << "  ram[" << std::dec << offset + byte << "]=UINT8_C(0x" << std::hex << std::setw(2) << std::setfill('0') << ((value >> (24U - 8U * byte)) & 0xffU) << ");\n";
  }
  out << emit_m68k_operation_c(operation, "d", "sr", "  ", &memory)
      << "  printf(\"{\\\"d\\\":[\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\"],\\\"a\\\":[\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\",\\\"%08X\\\"],\\\"pc\\\":\\\"%08X\\\",\\\"sr\\\":\\\"%04X\\\",\\\"ram\\\":[\",(unsigned)d[0],(unsigned)d[1],(unsigned)d[2],(unsigned)d[3],(unsigned)d[4],(unsigned)d[5],(unsigned)d[6],(unsigned)d[7],(unsigned)a[0],(unsigned)a[1],(unsigned)a[2],(unsigned)a[3],(unsigned)a[4],(unsigned)a[5],(unsigned)a[6],(unsigned)a[7],(unsigned)pc,(unsigned)sr);\n";
  for (std::size_t index = 0; index < ram_seeds.size(); ++index) {
    const auto offset = m68k_startup_ram_offset(ram_seeds[index].first);
    // Offsets are literal C source-code array indices here, not printed
    // numerals: they must be emitted in decimal regardless of any earlier
    // std::hex left active by the seeding loop above (a two/three-hex-digit
    // offset, e.g. a real stack address near A7, would otherwise emit an
    // invalid C token such as `ram[FC]`).
    out << "  printf(\"" << (index == 0U ? "\\\"%08X\\\"" : ",\\\"%08X\\\"") << "\""
        << ",(unsigned)(((uint32_t)ram[" << std::dec << offset << "]<<24U)|((uint32_t)ram[" << offset + 1U
        << "]<<16U)|((uint32_t)ram[" << offset + 2U << "]<<8U)|ram[" << offset + 3U << "]));\n";
  }
  out << "  return printf(\"]}\\n\") < 0;\n}\n";
  std::cout << out.str();
}
