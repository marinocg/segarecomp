// Test-only B1 compare harness: shared decode/lift/emission only.
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
  // Keep the established B1 harness reserved argument for its static test.
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
   if (operation.kind != M68kIrKind::compare && operation.kind != M68kIrKind::compare_immediate &&
        operation.kind != M68kIrKind::compare_address && operation.kind != M68kIrKind::subtract &&
        operation.kind != M68kIrKind::subtract_address && operation.kind != M68kIrKind::subtract_immediate &&
        operation.kind != M68kIrKind::subtract_quick && operation.kind != M68kIrKind::add &&
        operation.kind != M68kIrKind::add_address && operation.kind != M68kIrKind::add_immediate &&
        operation.kind != M68kIrKind::add_quick && operation.kind != M68kIrKind::logical_and &&
        operation.kind != M68kIrKind::logical_and_immediate && operation.kind != M68kIrKind::logical_or &&
        operation.kind != M68kIrKind::logical_or_immediate && operation.kind != M68kIrKind::exclusive_or &&
        operation.kind != M68kIrKind::exclusive_or_immediate && operation.kind != M68kIrKind::write_move &&
       operation.kind != M68kIrKind::write_movea) return 1;

  GenesisM68kEmissionContext memory{};
  memory.ram_array = "ram";
  memory.address_registers = "a";
  memory.frame_ids_array = "frame_ids";
  memory.frame_continuations_array = "frame_continuations";
  memory.frame_depth = "frame_depth";
  memory.continuation = 0U;
  // Reuse the shared resolver for every static access. Subtraction memory
  // destinations are RMW: validate their read first, then their write, so a
  // ROM/device/unmapped destination cannot become generated RAM indexing.
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
  if (!resolve_static(operation.source_ea, M68kMemoryAccessDirection::read)) return 1;
   const bool subtraction_write = (operation.kind == M68kIrKind::subtract ||
       operation.kind == M68kIrKind::subtract_immediate || operation.kind == M68kIrKind::subtract_quick) &&
       operation.destination_ea.mode != M68kEaMode::address_register;
   const bool addition_write = (operation.kind == M68kIrKind::add ||
       operation.kind == M68kIrKind::add_immediate || operation.kind == M68kIrKind::add_quick) &&
       operation.destination_ea.mode != M68kEaMode::address_register;
   const bool move_write = operation.kind == M68kIrKind::write_move;
   const bool logical_write = operation.kind == M68kIrKind::logical_and ||
       operation.kind == M68kIrKind::logical_and_immediate || operation.kind == M68kIrKind::logical_or ||
       operation.kind == M68kIrKind::logical_or_immediate || operation.kind == M68kIrKind::exclusive_or ||
       operation.kind == M68kIrKind::exclusive_or_immediate;
   if (!resolve_static(operation.destination_ea, M68kMemoryAccessDirection::read) ||
         ((subtraction_write || addition_write || logical_write || move_write) && !resolve_static(operation.destination_ea, M68kMemoryAccessDirection::write))) return 1;
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
    out << "  printf(\"" << (index == 0U ? "\\\"%08X\\\"" : ",\\\"%08X\\\"") << "\""
        << ",(unsigned)(((uint32_t)ram[" << offset << "]<<24U)|((uint32_t)ram[" << offset + 1U
        << "]<<16U)|((uint32_t)ram[" << offset + 2U << "]<<8U)|ram[" << offset + 3U << "]));\n";
  }
  out << "  return printf(\"]}\\n\") < 0;\n}\n";
  std::cout << out.str();
}
