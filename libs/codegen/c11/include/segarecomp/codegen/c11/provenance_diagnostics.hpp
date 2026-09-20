#pragma once

// SEG-020-T002 / ADR-0042 sections 1, 7, 8: a diagnostic projection composed at the codegen
// reporting boundary. It maps guest instructions to (cpu, platform-owned image identity, guest PC,
// image offset, static block identity, M68k-owned form id). Raw instruction bytes are never
// projected, and the neutral core provenance type is not widened: the M68k form id lives only here.
// Emission is opt-in; callers that do not request it produce no additional output.

#include "segarecomp/core/provenance.hpp"
#include "segarecomp/cpu/m68k/instruction.hpp"
#include "segarecomp/cpu/m68k/static_program.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp {

// M68k-owned stable form identity: the numeric M68kInstructionKind ordinal, or unknown when the
// instruction has no decoded form record.
inline constexpr std::uint32_t m68k_diagnostic_unknown_form_id = 0xFFFFFFFFU;

struct M68kProvenanceDiagnosticEntry {
  CpuVariant cpu{CpuVariant::mc68000};
  std::uint32_t guest_pc{};
  std::uint64_t image_offset{};
  std::uint32_t block_entry{};
  std::uint32_t form_id{m68k_diagnostic_unknown_form_id};
  friend bool operator==(const M68kProvenanceDiagnosticEntry &, const M68kProvenanceDiagnosticEntry &) = default;
};

// Only directly derivable static structure counts (ADR-0042 section 8).
struct M68kStaticClosureCounts { std::uint64_t blocks{}, instructions{}, edges{}, calls{}; };

struct M68kProvenanceDiagnosticProjection {
  std::string image_sha256;  // platform-owned image identity, supplied by the caller
  std::vector<M68kProvenanceDiagnosticEntry> entries;  // sorted by (guest_pc, block_entry), unique
  M68kStaticClosureCounts closure{};
};

[[nodiscard]] M68kProvenanceDiagnosticProjection build_m68k_provenance_diagnostic_projection(
    std::string_view image_sha256, const std::vector<M68kDecodedInstruction> &decoded,
    const std::vector<M68kStaticBlock> &blocks, const std::vector<M68kStaticEdge> &edges,
    const std::vector<M68kStaticCall> &calls);

// Deterministic C text: a static const lookup table plus closure counts. Contains no raw bytes.
[[nodiscard]] std::string emit_m68k_provenance_diagnostic_c(const M68kProvenanceDiagnosticProjection &projection);

} // namespace segarecomp
