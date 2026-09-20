#include "segarecomp/moveq.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
segarecomp::DecodeSource source(
    std::uint32_t address, std::uint64_t offset = 0,
    segarecomp::CpuVariant cpu_variant = segarecomp::CpuVariant::mc68000,
    segarecomp::TargetAddressSpace address_space = segarecomp::TargetAddressSpace::m68k_program) {
  return {cpu_variant, {address_space, address}, {offset}};
}
void test_decode_and_lift_provenance() {
  const std::vector<std::uint8_t> image{0x7EU, 0xFFU};
  const auto decoded = segarecomp::decode_moveq(image, source(0x00123400U));
  const auto *result = std::get_if<segarecomp::DecodedMoveq>(&decoded);
  expect(result != nullptr, "MOVEQ is decoded");
  if (result == nullptr) return;
  const auto &instruction = result->instruction;
  expect(instruction.destination == segarecomp::DataRegister::d7 && instruction.immediate == -1, "typed operands decode");
  expect(instruction.provenance.source.cpu_variant == segarecomp::CpuVariant::mc68000 &&
             instruction.provenance.source.address.space == segarecomp::TargetAddressSpace::m68k_program &&
             instruction.provenance.source.address.value == 0x00123400U && instruction.provenance.source.image_offset.value == 0U &&
             instruction.provenance.bytes == std::array<std::uint8_t, 2>{0x7EU, 0xFFU} && instruction.provenance.length.value == 2U,
         "decoder retains complete provenance");
  const auto ir = segarecomp::lift_moveq(instruction);
  expect(ir.provenance.source.address.value == instruction.provenance.source.address.value &&
             ir.provenance.source.image_offset.value == instruction.provenance.source.image_offset.value &&
             ir.provenance.bytes == instruction.provenance.bytes && ir.provenance.length.value == 2U &&
             ir.decode_outcome == segarecomp::DecodeOutcome::decoded_moveq && ir.destination == instruction.destination && ir.immediate == instruction.immediate,
         "lifter copies provenance and typed semantics");
  const auto emitted = segarecomp::emit_moveq(ir, {});
  expect(emitted.instruction.provenance.bytes == instruction.provenance.bytes &&
             emitted.instruction.provenance.source.address.value == 0x00123400U &&
             emitted.instruction.decode_outcome == segarecomp::DecodeOutcome::decoded_moveq &&
             emitted.c_source.find("7EFF") == std::string::npos,
         "emission retains provenance without embedding source bytes in C");
}
void test_rejection_precedence() {
  const std::vector<std::uint8_t> word{0x4AU, 0xFCU};
  const auto odd = std::get<segarecomp::RejectedMoveq>(segarecomp::decode_moveq(word, source(0x101U)));
  expect(odd.outcome == segarecomp::DecodeOutcome::odd_instruction_address && odd.available_bytes == 2U && !odd.has_instruction_length,
         "odd address wins before word classification");
  const std::vector<std::uint8_t> short_image{0x70U};
  const auto truncated = std::get<segarecomp::RejectedMoveq>(segarecomp::decode_moveq(short_image, source(0x100U)));
  expect(truncated.outcome == segarecomp::DecodeOutcome::truncated_instruction && truncated.available_bytes == 1U && !truncated.has_instruction_length,
         "truncation does not manufacture a word");
  const auto illegal = std::get<segarecomp::RejectedMoveq>(segarecomp::decode_moveq(word, source(0x100U)));
  expect(illegal.outcome == segarecomp::DecodeOutcome::illegal_instruction && illegal.has_instruction_length,
         "source-defined illegal word is distinct");
}
void test_source_contract_precedes_byte_classification() {
  const std::vector<std::uint8_t> image{0x4AU, 0xFCU};
  const auto invalid_cpu = static_cast<segarecomp::CpuVariant>(1);
  const auto invalid_address_space = static_cast<segarecomp::TargetAddressSpace>(1);
  const auto wrong_cpu = std::get<segarecomp::RejectedMoveq>(segarecomp::decode_moveq(
      image, source(0x101U, 0U, invalid_cpu, invalid_address_space)));
  expect(wrong_cpu.outcome == segarecomp::DecodeOutcome::unsupported_cpu_variant &&
             wrong_cpu.available_bytes == 2U && !wrong_cpu.has_instruction_length &&
             wrong_cpu.source.cpu_variant == invalid_cpu && wrong_cpu.source.address.space == invalid_address_space,
         "unsupported CPU wins before address and byte classification");
  expect(segarecomp::format_moveq_rejection(wrong_cpu) ==
             "{\"category\":\"unsupported_cpu_variant\",\"cpu_variant\":\"unknown\",\"source_address\":{\"space\":\"unknown\",\"value\":\"0x000101\"},\"image_offset\":\"0x00\",\"available_bytes\":2,\"requested_length\":2,\"instruction_length\":null}",
         "unsupported CPU diagnostic is stable and source-provenanced");
  const auto wrong_space = std::get<segarecomp::RejectedMoveq>(segarecomp::decode_moveq(
      image, source(0x100U, 0U, segarecomp::CpuVariant::mc68000, invalid_address_space)));
  expect(wrong_space.outcome == segarecomp::DecodeOutcome::unsupported_address_space &&
             wrong_space.available_bytes == 2U && !wrong_space.has_instruction_length &&
             wrong_space.source.cpu_variant == segarecomp::CpuVariant::mc68000 &&
             wrong_space.source.address.space == invalid_address_space,
         "unsupported address space wins before byte classification");
  expect(segarecomp::format_moveq_rejection(wrong_space) ==
             "{\"category\":\"unsupported_address_space\",\"cpu_variant\":\"mc68000\",\"source_address\":{\"space\":\"unknown\",\"value\":\"0x000100\"},\"image_offset\":\"0x00\",\"available_bytes\":2,\"requested_length\":2,\"instruction_length\":null}",
         "unsupported address-space diagnostic is stable and source-provenanced");
}
} // namespace

int main() {
  test_decode_and_lift_provenance();
  test_rejection_precedence();
  test_source_contract_precedes_byte_classification();
  return failures == 0 ? 0 : 1;
}
