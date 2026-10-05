#pragma once

// SEG-031 (ADR 0080, report-only): the Genesis M68K hybrid admission planner.
//
// Question: given sound precise discovery D (the SEG-030 `all` instantiation), how much of the broad immutable-ROM AOT universe U must
// be retained as conservative fallback to obtain a complete static generated-native program?
//
//   hybrid admission H = D ∪ fallback islands ∪ transitive static closure,   admitted identities = H ∩ U.
//
// Safety invariant, for every uncovered dynamic control site of H (an unresolved computed site, or a return the continuation model
// does not cover):   PossibleTargets(site) ⊆ island entries of the site ⊆ H.
//
// Containment authorities (the widening ladder, uncertainty only ever enlarges H):
//   1. exact SEG-030 target sets: followed by the analysis itself (no island);
//   2. a `JMP/JSR (An)` / `d16(An)` site whose address register holds a proven points-to set (joined over every context and
//      partition of the site) that is not width-derived: the island entries are exactly the set's members plus the displacement
//      (strided members enumerated), each mapped even PC (`points_to_region`, `executable_image` when the set strides over a whole
//      image, `materialized_image` when it names `static_proof` alias PCs);
//   3. anything else (Unknown values, width-only domains, indexed forms, returns, unclassified PC effects): no bound narrower than the
//      whole program, so H degenerates to broad AOT (`whole_image`).
// Every `static_proof` (ADR 0049) executable image is admitted whole as a mandatory root set. Forbidden as authorities: byte
// appearance, external disassemblers, runtime coverage (in either direction), operand width, nearest functions, title identity.
//
// Closure (a real monotone fixed point): an island's members enter the SAME SEG-030 instantiation as computed edges of their site
// carrying the site's own state (`M68kAnalysisConfig::island_entries`: exactly as a resolved site's targets, a call entering its
// callees in the call-site context), so island code is analysed (its returns, stores and further transfers) and the SEG-030 closure
// premise (ADR 0079 decision 8) holds over H instead of D. The materialized roots have no source transfer and enter as opaque
// all-Unknown entries from the startup entry (`opaque_entries`). Each round's uncovered sites widen the islands (union per
// site); the planner stops at the first round whose islands do not grow, then an independent validator re-checks the containment.
// An invalid image set, an incomplete solve, a non-validated frames round (`historical_assumption`), an unbounded container, the
// island-entry bound, the round bound or a validation failure all yield whole broad AOT: a partial hybrid program is never produced,
// and a resource ceiling is never evidence that omitted code is unreachable.
//
// Inherited premise: SEG-030's named return-slot integrity premise (ADR 0079 decision 8) for an ordinary RTS; the planner counts
// those sites. Nothing observed at run time is an input.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/genesis_analysis_report/report.hpp"
#include "segarecomp/machine/genesis/hybrid_admission.hpp"

namespace segarecomp {

// Resource constants (defence in depth; exceeding one gives broad AOT).
inline constexpr std::uint32_t genesis_hybrid_max_rounds = 8U;
inline constexpr std::size_t genesis_hybrid_max_island_entries = 65536U;

struct GenesisHybridPlanConfig {
  // The SEG-030 driver configuration. The planner forces `--domains all` (the only interrupt-register model proven sound).
  GenesisAnalysisReportConfig analysis;
  std::uint32_t max_rounds{genesis_hybrid_max_rounds};
  std::size_t max_island_entries{genesis_hybrid_max_island_entries};
};

enum class GenesisHybridOutcome : std::uint8_t {
  hybrid,
  broad_images_invalid,       // the executable-image set does not validate
  broad_universe_rejected,    // the broad analysis rejects the program (no U)
  broad_analysis_incomplete,  // a solve exhausted a bound: every query Unknown
  broad_historical_model,     // the frames round did not validate (`historical_assumption`): never credited
  broad_whole_image,          // an uncovered site has no bound narrower than the whole program
  broad_island_bound,         // the island entries exceed `max_island_entries`
  broad_closure_bound,        // no fixed point within `max_rounds`
  broad_validation_failed,    // the independent containment validator rejected the fixed point
};
[[nodiscard]] const char *genesis_hybrid_outcome_name(GenesisHybridOutcome outcome) noexcept;

enum class GenesisHybridContainer : std::uint8_t { exact, points_to_region, executable_image, materialized_image, whole_image };
inline constexpr std::size_t genesis_hybrid_container_count = 5U;
[[nodiscard]] const char *genesis_hybrid_container_name(GenesisHybridContainer container) noexcept;

struct GenesisHybridSite {
  GenesisAnalysisFamily family{GenesisAnalysisFamily::unclassified};
  analysis::UnknownReason reason{analysis::UnknownReason::unsupported_transfer};
  GenesisAnalysisSubReason sub{GenesisAnalysisSubReason::none};
  GenesisHybridContainer container{GenesisHybridContainer::whole_image};
  std::vector<std::uint32_t> entries;  // the site's island entries this round (sorted; empty for whole_image)
};

struct GenesisHybridPlan {
  GenesisHybridOutcome outcome{GenesisHybridOutcome::broad_images_invalid};
  std::uint32_t rounds{};
  std::vector<std::uint32_t> universe;  // U: sorted broad identity execution addresses
  std::size_t precise_d{};              // |D| of round 1 (the unmodified SEG-030 `all` result)
  std::size_t precise_d_in_universe{};  // |D ∩ U|
  std::vector<std::uint32_t> hybrid;    // H (sorted), hybrid outcome only
  std::vector<std::uint32_t> admitted;  // H ∩ U (hybrid) or U (broad)
  std::size_t resolved_sites{};         // exact computed sites of the last analysed round
  std::size_t premise_returns{};        // ordinary RTS sites covered under the inherited return-slot premise (last round)
  std::map<std::uint32_t, GenesisHybridSite> sites;                      // uncovered sites of the last analysed round
  std::map<std::uint32_t, std::vector<std::uint32_t>> island_entries;    // the final island configuration
  std::size_t materialized_entries{};   // mandatory `static_proof` image entries
  std::string validation_failure;       // broad_validation_failed only
};

// Plans the hybrid admission of `program` (which may carry ADR 0049 aliases). Deterministic.
[[nodiscard]] GenesisHybridPlan plan_genesis_hybrid_admission(const FrontendProgram &program, const GenesisHybridPlanConfig &config);

// Independent containment validator over one analysis round: nullopt when every root, fixed successor, stacked call continuation,
// resolved target and island entry that decodes is in the round's discovered set, every uncovered site is configured with its whole
// container, and no reached undecodable PC is a broad identity; else the first failure class. `universe` is U (sorted).
[[nodiscard]] std::optional<std::string> validate_genesis_hybrid_round(
    const GenesisAnalysisReport &report, const GenesisM68kAnalysisImage &image,
    const std::map<std::uint32_t, std::vector<std::uint32_t>> &island_entries, const std::vector<std::uint32_t> &universe,
    std::size_t max_island_entries = genesis_hybrid_max_island_entries);

// The container of one site of `report` under the ladder above (entries sorted; whole_image when unbounded or over `max_entries`).
[[nodiscard]] GenesisHybridSite genesis_hybrid_container(const GenesisAnalysisReport &report, const GenesisM68kAnalysisImage &image,
                                                         std::uint32_t pc, std::size_t max_entries = genesis_hybrid_max_island_entries);

// The production artifact of a plan (ROM digest and alias set of `program`; `broad` unless the outcome is hybrid).
[[nodiscard]] GenesisHybridAdmissionPlan genesis_hybrid_admission_plan(const GenesisHybridPlan &plan, const FrontendProgram &program,
                                                                       const std::string &rom_sha256);

// Sanitized aggregate JSON (counts, ratios and generic classes only; never an address).
[[nodiscard]] std::string format_genesis_hybrid_plan_aggregate(const GenesisHybridPlan &plan);
// PRIVATE JSON with exact PCs (sites, island entries): ignored locations only for a commercial input.
[[nodiscard]] std::string format_genesis_hybrid_plan_private(const GenesisHybridPlan &plan);

}  // namespace segarecomp
