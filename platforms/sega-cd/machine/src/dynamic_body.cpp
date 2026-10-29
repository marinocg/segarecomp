#include "segarecomp/machine/sega_cd/dynamic_body.hpp"

#include <array>

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

#include "emit_shared.hpp"

namespace segarecomp::machine::sega_cd {

const char *dynamic_body_status_name(DynamicBodyStatus status) noexcept {
  switch (status) {
    case DynamicBodyStatus::ok: return "ok";
    case DynamicBodyStatus::not_decoded: return "not_decoded";
    case DynamicBodyStatus::not_lifted: return "not_lifted";
    case DynamicBodyStatus::not_lowered: return "not_lowered";
    case DynamicBodyStatus::redecode: return "redecode";
    case DynamicBodyStatus::source_relative_refused: return "source_relative_refused";
  }
  return "?";
}

DynamicBody derive_dynamic_body(std::span<const std::uint8_t> image, std::uint32_t visible, std::uint32_t offset) {
  constexpr std::array<std::uint32_t, 3> kSentinel{0x00200000U, 0x00300000U, 0x00480000U};
  DynamicBody out;
  if ((offset & 1U) != 0U || visible > image.size() || offset >= visible) return out;
  const detail::SegaCdM68kRuntimeCEmitter emitter("scd_k", "scd_neutral_lookup");
  const std::string lookup = "scd_neutral_lookup";
  const auto window = image.subspan(0, visible);
  std::array<std::uint32_t, 3> pcs{};
  std::array<detail::LoweredInstruction, 3> lowered;
  std::vector<std::uint8_t> raw;
  M68kInstructionKind kind{};
  for (std::size_t i = 0; i < 3; ++i) {
    pcs[i] = kSentinel[i] + offset;
    const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pcs[i]}, MoveqImageOffset{offset}};
    const auto result = decode_m68k_instruction(window, source, M68kDecodeProfile::general_startup);
    const auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
    if (decoded == nullptr) {
      out.status = i == 0 ? DynamicBodyStatus::not_decoded : DynamicBodyStatus::redecode;
      return out;
    }
    if (i == 0) {
      raw = decoded->raw_bytes;
      kind = decoded->kind;
      out.prefetch_established = detail::prefetch_position(*decoded, pcs[i]) != 0U;
    } else if (decoded->kind != kind || decoded->raw_bytes != raw) {
      out.status = DynamicBodyStatus::redecode;
      return out;
    }
    const auto operation = lift_m68k_instruction(*decoded);
    if (decoded->kind != M68kInstructionKind::moveq && operation.kind == M68kIrKind::write_moveq) {
      out.status = DynamicBodyStatus::not_lifted;
      return out;
    }
    lowered[i] = detail::lower_instruction(operation, pcs[i], emitter, lookup);
    if (!lowered[i].compiled) {
      out.status = DynamicBodyStatus::not_lowered;
      return out;
    }
  }
  const auto derive = [&](auto member, std::string &target) {
    const auto derived = derive_source_relative_text({lowered[0].*member, lowered[1].*member, lowered[2].*member}, pcs, "scd_pc0");
    if (derived.status != SourceRelativeStatus::ok) {
      out.status = DynamicBodyStatus::source_relative_refused;
      out.derive = derived.status;
      return false;
    }
    target = derived.text;
    out.relative_literals += derived.relative_literals;
    return true;
  };
  if (!derive(&detail::LoweredInstruction::slots, out.slots) || !derive(&detail::LoweredInstruction::body, out.body) ||
      !derive(&detail::LoweredInstruction::timing, out.timing) || !derive(&detail::LoweredInstruction::refresh_skip, out.refresh_skip))
    return out;
  out.status = DynamicBodyStatus::ok;
  out.raw = std::move(raw);
  return out;
}

}  // namespace segarecomp::machine::sega_cd
