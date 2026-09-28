#pragma once

// SEG-024-T001 (experiment, report-only): Genesis composition of the CPU-neutral executable-support
// fixed point (segarecomp/recompiler/executable_support.hpp) over the existing Gen-2 immutable-ROM AOT
// universe. Nothing here changes admission, discovery, emission or runtime behavior.
//
//   U      = FrontendAnalysis::immutable_rom_aot_entries (identity = 24-bit bus execution address)
//   roots  = reset entry + every installed MC68000 vector 2..63 of the immutable vector table (Genesis
//            interrupt sources are autovectored, so user vectors 64..255 are not selectable)
//   edges  = m68k_control_support(entry.operation) (a projection of m68k_operation_effect)
//   domains, per dynamic site, chosen by explicit models (see GenesisExecutableSupportConfig).

#include <cstdint>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/control_support.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/recompiler/executable_support.hpp"

namespace segarecomp {

// RTS target authority.
enum class GenesisReturnDomainModel : std::uint8_t {
  // What Gen-2 generated code enforces today: an RTS validates the popped value against the whole-program
  // return-target set (ADR 0011: static-frame continuations, AOT call continuations and foldable PEA
  // addresses that are themselves compiled), except an ADR 0048 push-window RTS, which accepts any compiled
  // identity (AnyImmutableRom).
  gen2_runtime_authority,
  // Conditional refinement: a non-push-window RTS returns only to continuations pushed by LIVE calls/PEAs,
  // which already enter L as return-continuation edges. Valid only under ADR 0011's premise; not proven.
  live_call_continuations,
};

// RTE/RTR target authority.
enum class GenesisExceptionReturnModel : std::uint8_t {
  any_candidate,  // what Gen-2 enforces: the restored PC dispatches to any compiled identity
  // Conditional refinement: the restored PC is the interrupted/next PC of a live identity, already in L.
  // Requires an exception-frame integrity proof (no handler writes the stacked PC, no fabricated frame),
  // which does not exist today.
  resumed_live_boundary,
};

// Runtime-owned JMP/JSR target authority.
enum class GenesisIndirectDomainModel : std::uint8_t {
  any_candidate,  // what Gen-2 enforces: the final compiled-entry lookup (Tier-1 exact sets aside)
  // Sound refinement: (d8,PC,Xn.W) targets lie within base +/- 32 KiB by index width alone.
  architectural_operand_width,
};

struct GenesisExecutableSupportConfig {
  GenesisReturnDomainModel returns{GenesisReturnDomainModel::gen2_runtime_authority};
  GenesisExceptionReturnModel exception_returns{GenesisExceptionReturnModel::any_candidate};
  GenesisIndirectDomainModel indirects{GenesisIndirectDomainModel::any_candidate};
  bool honor_tier1_exact_sets{true};
};

// Machine site families: the CPU families, then machine-scoped ones.
inline constexpr std::uint32_t genesis_support_family_push_window_rts = m68k_dynamic_control_family_count;
inline constexpr std::uint32_t genesis_support_family_tier1_exact = m68k_dynamic_control_family_count + 1U;
inline constexpr std::uint32_t genesis_support_family_count = m68k_dynamic_control_family_count + 2U;
[[nodiscard]] std::string genesis_support_family_name(std::uint32_t family);

struct GenesisExecutableSupportRoots {
  std::vector<ExecutableIdentity> identities;  // sorted, unique
  std::uint32_t reset{};
  std::uint32_t vectors_installed{};
  std::uint32_t vectors_uninstalled{};
  std::uint32_t vectors_outside_rom{};  // installed handler not in a raw_cartridge_rom claim (e.g. work RAM)
  bool vector_table_present{};
};

struct GenesisExecutableSupportModel {
  ExecutableSupportInput input;
  GenesisExecutableSupportRoots roots;
  std::uint64_t push_window_rts{};        // RTS identities classified by the ADR 0048 window mirror
  std::uint64_t gen2_return_targets{};    // size of the reconstructed Gen-2 return-target set
};

[[nodiscard]] GenesisExecutableSupportRoots genesis_executable_support_roots(const FrontendProgram &program);
[[nodiscard]] GenesisExecutableSupportModel build_genesis_executable_support_model(
    const FrontendProgram &program, const FrontendAnalysis &analysis, const GenesisExecutableSupportConfig &config);

// Runs the fixed experiment matrix and renders deterministic, sanitized JSON: counts, digests, timings are
// NOT included (the caller measures wall time/RSS). No address, byte or instruction word is emitted.
[[nodiscard]] std::string format_genesis_executable_support_report(const FrontendProgram &program,
                                                                   const FrontendAnalysis &analysis);

}  // namespace segarecomp
