#pragma once

// SEG-026-T001 (experiment, report-only): a reachability-first MC68000 discovery challenger for Genesis images.
//
// It does not alter admission, generated output or generated execution authority, and it never consumes runtime
// coverage, broad immutable-ROM AOT identities, external hints or disassembler output. Starting only from the
// architecture-owned roots the Genesis machine model delivers (the reset entry and the handler of every
// exception/interrupt vector the generated-native machine can raise: IRQ6 and the synchronous vectors), it
// decodes ONE instruction at each reached PC with the unchanged decoder/lifter and follows only the fixed
// successors `m68k_control_successors` (a projection of `m68k_operation_effect`) states: fallthrough, BRA,
// both Bcc/DBcc outcomes, BSR and direct JSR/JMP targets. It never linear-sweeps.
//
// Returns: every call (BSR/JSR, direct or indirect) DISCOVERED BY THIS CHALLENGER contributes its stacked
// continuation to a challenger-owned continuation set; once a reachable RTS that is not an ADR 0048
// push-then-RTS computed jump exists, every member of that set is discovered (deterministic fixed point,
// context-insensitive). No continuation is ever taken from the Gen-2 return set.
//
// Exception returns: `strict` model -- RTE/RTR discover nothing; `normal_resumption` hypothesis -- an RTE/RTR
// resumes an already-discovered boundary, where the only boundaries it may add are the stacked exception
// continuations of discovered TRAP/TRAPV instructions and (RTR) the challenger's own call continuations. This
// hypothesis is measurement only, never production authority.
//
// Every other runtime-derived PC (JMP/JSR (An), d16(An), (d8,An,Xn), (d8,PC,Xn), push-then-RTS, unclassified)
// is recorded as an unresolved site and discovery stops through that edge.

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"

namespace segarecomp {

enum class GenesisReachabilityExceptionModel : std::uint8_t { strict, normal_resumption };

struct GenesisReachabilityChallengerConfig {
  GenesisReachabilityExceptionModel exception_model{GenesisReachabilityExceptionModel::strict};
  // Treat PEA of a statically foldable address as a possible manual-call continuation (variant; off by default).
  bool pea_continuations{};
};

// Site family index: m68k_dynamic_control_family values, plus the ADR 0048 push-then-RTS classification.
inline constexpr std::uint32_t genesis_challenger_family_push_window_rts = m68k_dynamic_control_family_count;
inline constexpr std::uint32_t genesis_challenger_family_count = m68k_dynamic_control_family_count + 1U;
[[nodiscard]] std::string genesis_challenger_family_name(std::uint32_t family);

struct GenesisReachabilityChallengerResult {
  std::vector<std::uint32_t> roots;  // sorted distinct 24-bit bus PCs
  std::uint32_t vector_roots{};      // installed machine-delivered vectors among the roots' sources
  std::map<std::uint32_t, std::uint32_t> discovered;  // instruction-start PC -> instruction length
  std::set<std::uint32_t> call_continuations;         // stacked by discovered calls
  std::set<std::uint32_t> exception_continuations;    // stacked by discovered TRAP/TRAPV
  std::set<std::uint32_t> pushed_code_addresses;      // PEA of foldable addresses (variant only)
  std::array<std::set<std::uint32_t>, genesis_challenger_family_count> sites{};  // unresolved dynamic sites
  std::set<std::uint32_t> rejected_decode_targets;  // reached PCs the decoder rejected
  std::set<std::uint32_t> unmapped_targets;         // reached PCs outside a unique ROM mapping or alias
  std::set<std::uint32_t> odd_targets;
  std::uint64_t overlapping_starts{};  // discovered starts lying inside another discovered instruction
  std::uint32_t rounds{};              // continuation-resumption rounds of the fixed point
  bool continuations_enabled{};        // a reachable ordinary RTS (or RTR under the hypothesis) exists
};

[[nodiscard]] GenesisReachabilityChallengerResult run_genesis_reachability_challenger(
    const FrontendProgram &program, const GenesisReachabilityChallengerConfig &config);

// Classification only (never discovery): decodes the one instruction at `pc` with the challenger's decoder and
// projects its control successors. Used to label observed runtime transitions by generic mechanism; the result
// never enters D.
struct GenesisReachabilityPcClassification {
  bool decoded{};
  std::uint32_t length{};
  M68kControlSuccessors control{};
  bool push_window_rts{};  // RTS preceded (within this classification set) by an ADR 0048 push window
};
[[nodiscard]] std::map<std::uint32_t, GenesisReachabilityPcClassification> classify_genesis_reachability_pcs(
    const FrontendProgram &program, const std::vector<std::uint32_t> &pcs);
// PRIVATE JSON (exact PCs) of a classification.
[[nodiscard]] std::string format_genesis_reachability_classification_private(
    const std::map<std::uint32_t, GenesisReachabilityPcClassification> &classification);

// Aggregate-only JSON (counts per family etc.; never an address). Safe for durable evidence.
[[nodiscard]] std::string format_genesis_reachability_challenger_aggregate(const GenesisReachabilityChallengerResult &result,
                                                                           const GenesisReachabilityChallengerConfig &config);
// PRIVATE JSON with exact PCs for local comparison against runtime coverage. Commercial-derived when the input
// is commercial: keep it in an ignored location and never persist it.
[[nodiscard]] std::string format_genesis_reachability_challenger_private(const GenesisReachabilityChallengerResult &result,
                                                                         const GenesisReachabilityChallengerConfig &config);

}  // namespace segarecomp
