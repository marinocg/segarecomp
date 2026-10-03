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
// context-insensitive). No continuation is ever taken from the Gen-2 return set. This is deliberately coarse: the
// enablement is global (a discovered call whose callee never returns still contributes its continuation) and
// latched (an RTS later reclassified as push-then-RTS does not revoke it), so data following a non-returning
// call could in principle enter D; the comparison reports D - O so such growth stays visible.
//
// Exception returns: `strict` model -- RTE/RTR discover nothing; `normal_resumption` hypothesis -- an RTE/RTR
// resumes an already-discovered boundary, where the only boundaries it may add are the stacked exception
// continuations of discovered TRAP/TRAPV instructions and (RTR) the challenger's own call continuations. This
// hypothesis is measurement only, never production authority.
//
// Every other runtime-derived PC (JMP/JSR (An), d16(An), (d8,An,Xn), (d8,PC,Xn), push-then-RTS, unclassified)
// is recorded as an unresolved site and discovery stops through that edge.
//
// SEG-026-T002 (`pc_index_recovery`, off by default): a `JMP/JSR (d8,PC,Xn)` site is resolved only when its index
// register has an exact finite domain proven by a demand-driven backward evaluation over the challenger's OWN
// discovered predecessor graph (fixed-flow, direct-call and already-recovered edges), using the tiny
// `m68k_finite_register_after` domain: constants, masks, CMP/TST + Bcc guards on the edge actually taken,
// add/sub/shift/extend transforms, and entries read from uniquely owned immutable cartridge bytes. A block
// that is a root, a call/exception continuation or has no discovered predecessor is opaque (Unknown). An
// unproven domain, an entry read outside the immutable image, or a target outside the mapped image leaves the
// site unresolved (fail closed); there is no operand-width region, plausibility scan or table-size guess. The
// exact targets are enqueued as ordinary discovery roots and the closure iterates to a deterministic fixed
// point. A proof is relative to the predecessors the challenger knows: if a later edge invalidates an earlier
// proof, discovery restarts with that site pinned unresolved.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"

namespace segarecomp {

enum class GenesisReachabilityExceptionModel : std::uint8_t { strict, normal_resumption };

struct GenesisReachabilityChallengerConfig {
  GenesisReachabilityExceptionModel exception_model{GenesisReachabilityExceptionModel::strict};
  // Treat PEA of a statically foldable address as a possible manual-call continuation (variant; off by default).
  bool pea_continuations{};
  // SEG-026-T002: exact PC-indexed immutable jump-table recovery (report-only; off by default).
  bool pc_index_recovery{};
  // SEG-026-T002 measurement VARIANT only: also accept an index domain whose upper extent is only the width of a
  // byte loaded from mutable memory (`M68kFiniteValues::width_derived`). The primary policy leaves such a site
  // unresolved (`width_only_domain`): a width bound is not an explicit table-extent proof.
  bool pc_index_width_domains{};
};

// Site family index: m68k_dynamic_control_family values, plus the ADR 0048 push-then-RTS classification.
inline constexpr std::uint32_t genesis_challenger_family_push_window_rts = m68k_dynamic_control_family_count;
inline constexpr std::uint32_t genesis_challenger_family_count = m68k_dynamic_control_family_count + 1U;
[[nodiscard]] std::string genesis_challenger_family_name(std::uint32_t family);

// SEG-026-T002: the recovery outcome of one encountered `(d8,PC,Xn)` control site.
enum class GenesisPcIndexOutcome : std::uint8_t {
  resolved,
  index_unknown,           // no exact finite index domain on the known predecessor paths
  resource_limit,          // the bounded backward evaluation budget was exhausted
  address_register_index,  // An index register: not tracked
  entry_outside_immutable_image,  // a table entry read left the uniquely owned immutable image
  target_outside_image,    // a proven target is not a mapped image PC
  empty_domain,            // every proven target is odd (address error), no normal target
  invalidated,             // a later predecessor invalidated an earlier proof; pinned unresolved
  width_only_domain,       // the finite domain is only a mutable byte's width (no explicit mask/shift/guard bound)
};
inline constexpr std::uint32_t genesis_pc_index_outcome_count = 9U;
[[nodiscard]] const char *genesis_pc_index_outcome_name(GenesisPcIndexOutcome outcome) noexcept;

// For an `index_unknown` site: where the backward evaluation first met Unknown (a generic category).
enum class GenesisPcIndexUnknownOrigin : std::uint8_t {
  none,
  machine_root,              // a reset/vector root (entry register state unknown)
  return_continuation,       // a call/exception continuation (entered by a return)
  no_known_predecessor,      // no discovered predecessor (e.g. only reachable from an unresolved dynamic site)
  cycle,                     // a loop in the slice (no loop reasoning)
  untracked_load_or_source,  // MOVE from mutable word/long memory, An, or another untracked source
  arithmetic_on_unknown,     // ADD/SUB/AND/OR on an operand whose value is unknown
  unsupported_writer,        // another register writer outside the tiny domain
  limit_exceeded,            // a finite set exceeded the resource limit
};
inline constexpr std::uint32_t genesis_pc_index_unknown_origin_count = 9U;
[[nodiscard]] const char *genesis_pc_index_unknown_origin_name(GenesisPcIndexUnknownOrigin origin) noexcept;

struct GenesisPcIndexSiteRecovery {
  bool call{};  // JSR (else JMP)
  GenesisPcIndexOutcome outcome{GenesisPcIndexOutcome::index_unknown};
  std::vector<std::uint32_t> targets;  // exact 24-bit bus PCs (sorted) when resolved
  std::uint32_t proof{};               // m68k_finite_proof bits
  std::uint32_t table_reads{};
  std::uint32_t misaligned_reads_excluded{};
  std::uint32_t odd_targets_excluded{};
  std::uint32_t first_round{};         // recovery round in which the site was first discovered (0 = before any)
  GenesisPcIndexUnknownOrigin unknown_origin{GenesisPcIndexUnknownOrigin::none};
};

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
  // SEG-026-T002 (only with pc_index_recovery). Resolved sites are removed from `sites` (which stays the set of
  // unresolved dynamic sites); every encountered PC-indexed site is in `pc_index_sites`.
  std::map<std::uint32_t, GenesisPcIndexSiteRecovery> pc_index_sites;
  std::set<std::uint32_t> recovered_targets;  // union of resolved sites' exact targets
  std::uint32_t recovery_rounds{};            // recovery steps that changed a target set
  std::uint32_t recovery_restarts{};          // restarts after an invalidated proof
  std::size_t discovered_before_recovery{};   // |D| at the first ordinary fixed point
  std::array<std::size_t, genesis_challenger_family_count> sites_before_recovery{};
  std::set<std::uint32_t> table_entry_addresses;  // distinct immutable entry addresses read by resolved sites
  std::uint64_t table_entries_overlapping_code{};  // of those, entries whose bytes overlap a discovered instruction
  std::uint64_t exception_raising_instructions{};  // discovered ILLEGAL/line-A/line-F style words (decode indicator)
};

// SEG-030-T002: the architecture-owned discovery roots the Genesis machine delivers: the reset entry and the handler of every
// installed machine-delivered exception/interrupt vector (IRQ6 and the synchronous vectors of ADR 0021 / ADR 0043). `roots` is
// sorted and distinct; `vector_roots` counts installed delivered vectors (a handler shared by several vectors counts once per
// vector). The challenger and the report-only analysis driver share this single owner.
struct GenesisReachabilityRoots {
  std::vector<std::uint32_t> roots;
  std::uint32_t vector_roots{};
  // SEG-030-T006: every installed delivered vector as (vector number, handler), in delivery-list order (report-only consumers).
  std::vector<std::pair<std::uint32_t, std::uint32_t>> vectors;
  // SEG-030-T006: every installed interrupt vector the machine model does not deliver (the spurious vector 24 and the autovectors
  // 25-31 other than IRQ6), as (vector number, handler). Never roots: report-only consumers treat them as potential asynchronous
  // sources (real hardware delivers them once the program enables their source).
  std::vector<std::pair<std::uint32_t, std::uint32_t>> potential_interrupts;
};
[[nodiscard]] GenesisReachabilityRoots genesis_reachability_roots(const FrontendProgram &program);

// SEG-030-T002: the challenger's experiment-local mirror of ADR 0048's push-window scoping rule, over a caller-supplied
// decoded instruction set: the RTS PCs reached within 8 stack-neutral instructions from a MOVE.L <ea>,-(A7). Classification
// only (a computed jump rather than an ordinary return); never discovery.
struct GenesisReachabilityInstruction {
  M68kIrOperation operation;
  std::uint32_t length{};
};
[[nodiscard]] std::set<std::uint32_t> genesis_push_window_rts(
    const std::map<std::uint32_t, GenesisReachabilityInstruction> &instructions);

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
  // SEG-026-T002 (classification only): for a `(d8,PC,Xn)` control site, the strict index-domain outcome of the
  // same backward proof, run over a predecessor graph built ONLY from the classified instructions' fixed
  // successors (their stacked continuations opaque). A label for measuring the remaining PC-indexed graph; it
  // never enters D and never authorizes a target.
  std::optional<GenesisPcIndexOutcome> pc_index_domain;
  GenesisPcIndexUnknownOrigin pc_index_unknown_origin{GenesisPcIndexUnknownOrigin::none};
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
