#pragma once

// SEG-014-T002: MC68000 decode (docs/architecture/
// post-seg007-architecture-refactor-contract.md, section 8.2 "cpu/m68k/").
// Relocated from include/segarecomp/m68k_pipeline.hpp per
// docs/architecture/seg-014-t001-symbol-migration-map.md's `cpu/m68k/`
// section. No behavior changes.
//
// `DecodeOutcome` physically moves here (rather than staying attached to the
// temporary include/segarecomp/moveq.hpp compatibility surface) because
// RejectedM68kDecode below -- explicit cpu/m68k content per the migration
// map -- has always embedded it as a field's type; leaving the definition in
// moveq.hpp would force this permanent cpu/m68k header to depend on the
// temporary MOVEQ facade it is supposed to outlive. moveq.hpp now reuses this
// one relocated definition (including its historical `decoded_moveq` value,
// unchanged) instead of redefining it, matching the map's own instruction
// that moveq.hpp "must now delegate to the moved cpu/m68k/core types...
// rather than duplicating them." See the map's `core/` section for why
// DecodeOutcome itself was never proposed as a `core/` type.

#include "segarecomp/cpu/m68k/instruction.hpp"

#include <span>
#include <variant>

namespace segarecomp {

enum class DecodeOutcome {
  decoded_moveq,
  unsupported_cpu_variant,
  unsupported_address_space,
  odd_instruction_address,
  truncated_instruction,
  illegal_instruction,
  valid_but_unsupported_instruction,
};

// Decoder-owned classification for the small set of complete CPU frontiers
// that C5 can report safely.  It derives only from an already-verified
// provenance span; discovery and emission must use this one helper.
enum class M68kCpuFrontierKind { none, nop, stop_immediate_word, reset, move_an_to_usp };
[[nodiscard]] M68kCpuFrontierKind classify_m68k_cpu_frontier(const InstructionProvenance &provenance);

struct RejectedM68kDecode {
  DecodeOutcome outcome{};
  DecodeSource source{};
  std::uint64_t available_bytes{};
  std::uint32_t requested_length{2};
  std::uint32_t instruction_length{};
  bool has_instruction_length{};
  InstructionProvenance provenance{};
  bool has_provenance{};
  bool unsupported_instruction_form{};
  M68kCpuFrontierKind cpu_frontier{M68kCpuFrontierKind::none};
};

using M68kDecodeResult = std::variant<M68kDecodedInstruction, RejectedM68kDecode>;

[[nodiscard]] M68kDecodeResult decode_m68k_instruction(
    std::span<const std::uint8_t> image, DecodeSource source, M68kDecodeProfile profile);

// Names are CPU classification, not CLI presentation ownership.  The probe
// compatibility spelling retains historical exact-form labels without making
// the CLI inspect instruction kinds or effective-address modes itself.
[[nodiscard]] const char *m68k_instruction_kind_name(M68kInstructionKind kind) noexcept;
[[nodiscard]] const char *m68k_probe_instruction_kind_name(const M68kDecodedInstruction &instruction) noexcept;

} // namespace segarecomp
