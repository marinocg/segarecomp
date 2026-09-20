#include "segarecomp/moveq.hpp"

#include "segarecomp/codegen/c11/genesis_frontend.hpp"

#include <iomanip>
#include <sstream>

namespace segarecomp {
namespace {
[[nodiscard]] std::string hex(std::uint64_t value, unsigned width) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
  return out.str();
}
[[nodiscard]] const char *cpu_variant_name(CpuVariant cpu_variant) noexcept {
  switch (cpu_variant) {
  case CpuVariant::mc68000: return "mc68000";
  }
  return "unknown";
}
[[nodiscard]] const char *address_space_name(TargetAddressSpace address_space) noexcept {
  switch (address_space) {
  case TargetAddressSpace::m68k_program: return "m68k_program";
  }
  return "unknown";
}
} // namespace

MoveqDecodeResult decode_moveq(std::span<const std::uint8_t> image, DecodeSource source) {
  const auto decoded = decode_m68k_instruction(image, source, M68kDecodeProfile::moveq);
  if (const auto *rejected = std::get_if<RejectedM68kDecode>(&decoded)) {
    return RejectedMoveq{rejected->outcome, rejected->source, rejected->available_bytes,
                         rejected->requested_length, rejected->instruction_length,
                         rejected->has_instruction_length};
  }
  const auto &shared = std::get<M68kDecodedInstruction>(decoded);
  MoveqInstruction instruction{};
  instruction.provenance = shared.provenance;
  instruction.decode_outcome = DecodeOutcome::decoded_moveq;
  instruction.destination = shared.destination;
  instruction.immediate = shared.operand;
  return DecodedMoveq{instruction};
}

IrMoveq32 lift_moveq(const MoveqInstruction &instruction) {
  const M68kDecodedInstruction shared{instruction.provenance, M68kInstructionKind::moveq,
                                      instruction.destination, instruction.immediate, {}, {}};
  const auto lifted = lift_m68k_instruction(shared);
  return {lifted.provenance, instruction.decode_outcome, lifted.destination, lifted.operand};
}

const char *decode_outcome_name(DecodeOutcome outcome) noexcept {
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

std::string format_moveq_rejection(const RejectedMoveq &rejection) {
  std::ostringstream out;
  out << "{\"category\":\"" << decode_outcome_name(rejection.outcome)
      << "\",\"cpu_variant\":\"" << cpu_variant_name(rejection.source.cpu_variant)
      << "\",\"source_address\":{\"space\":\"" << address_space_name(rejection.source.address.space) << "\",\"value\":\""
      << hex(rejection.source.address.value, 6) << "\"},\"image_offset\":\""
      << hex(rejection.source.image_offset.value, 2) << "\",\"available_bytes\":" << rejection.available_bytes
      << ",\"requested_length\":2,\"instruction_length\":";
  if (rejection.has_instruction_length) out << 2;
  else out << "null";
  return out.str() + "}";
}

EmittedMoveq emit_moveq(const IrMoveq32 &instruction, const MoveqCpuState &initial_state) {
  std::ostringstream out;
  out << "/* Static MC68000 MOVEQ translation; target image is not present. */\n"
      << "#include <inttypes.h>\n#include <stdint.h>\n#include <stdio.h>\n\n"
      << "const uint32_t segarecomp_moveq_source_address = UINT32_C(0x" << std::uppercase << std::hex
      << std::setw(8) << std::setfill('0') << instruction.provenance.source.address.value << ");\n"
      << "const uint64_t segarecomp_moveq_image_offset = UINT64_C(0x" << std::setw(16)
      << instruction.provenance.source.image_offset.value << ");\n"
      << "const uint32_t segarecomp_moveq_byte_length = UINT32_C(2);\n"
      << "const char segarecomp_moveq_cpu_variant[] = \"mc68000\";\n"
      << "const char segarecomp_moveq_decode_outcome[] = \"decoded_moveq\";\n\n"
      << "int main(void) {\n  uint32_t d[8] = {";
  for (std::size_t index = 0; index < initial_state.d.size(); ++index) {
    if (index != 0U) out << ", ";
    out << "UINT32_C(0x" << std::setw(8) << initial_state.d[index] << ")";
  }
  out << "};\n  uint16_t sr = UINT16_C(0x" << std::setw(4) << static_cast<unsigned>(initial_state.sr) << ");\n"
      << "  const uint32_t pc = UINT32_C(0x" << std::setw(8) << initial_state.pc.value << ") + UINT32_C(2);\n"
       << emit_m68k_operation_c({instruction.provenance, M68kIrKind::write_moveq,
                                  instruction.destination, instruction.immediate, {}, {}}, "d", "sr", "  ")
       << "  return printf(\"{\\\"schema\\\":1,\\\"d\\\":[\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\",\\\"%08\" PRIX32 \"\\\"],\\\"pc\\\":\\\"%08\" PRIX32 \"\\\",\\\"sr\\\":\\\"%04\" PRIX16 \"\\\",\\\"stop_reason\\\":\\\"instruction_budget_exhausted\\\"}\\n\", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], pc, sr) < 0;\n}\n";
  return {instruction, out.str()};
}

std::string emit_moveq_c(const IrMoveq32 &instruction, const MoveqCpuState &initial_state) {
  return emit_moveq(instruction, initial_state).c_source;
}

} // namespace segarecomp
