#pragma once

// SEG-049-T004 (ADR 0103): the single owner of ML-proposed selective admission for Genesis M68K, shared by the SEG-047 v1 producer and the
// SEG-049 gen2 producer. Normative contract: docs/architecture/admission-authority-and-model-lifecycle-contract.md.
//
// A producer only PROPOSES an executable region `R` (plain ranges + ROM digest). This owner then applies the UNCHANGED structural prune and
// the UNCHANGED production validator; only a validator-accepted plan replaces the broad identities. Any failure is a classified fallback to
// the broad universe and leaves the entries untouched. Producers cannot read or write validator state, and no ML type crosses this boundary.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/hybrid_admission.hpp"

namespace segarecomp {

// The semantics of the structural prune + admission validator that every producer feeds. Bump on any change that can alter an admitted set.
inline constexpr std::string_view genesis_admission_analysis_contract = "segarecomp.admission_contract.v1";

enum class GenesisMlModelFamily { v1, gen2 };
[[nodiscard]] std::string_view genesis_ml_model_family_name(GenesisMlModelFamily family) noexcept;  // "v1" | "gen2"

// What a producer returns: a plain proposal or a stable sanitized failure. Unit = the producer's decision unit (512 B window / 128 B cell).
struct GenesisMlProposal {
  std::optional<GenesisExecutableRegionProposal> proposal;
  std::string failure;
  std::size_t units{};        // decision units in the image
  std::size_t selected{};     // units selected by the model/policy before the seed union
  std::size_t seed_units{};   // units holding a precise direct-control-discovery identity
};
using GenesisMlProposer = GenesisMlProposal (*)(std::span<const std::uint8_t> rom, std::string_view rom_sha256,
                                                const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries,
                                                std::span<const std::uint32_t> seed_addresses);

// The identity components of one producer (section 3 of the contract). Empty string = not applicable to the family.
struct GenesisMlProducer {
  GenesisMlModelFamily family{};
  std::string_view producer_id;       // "ml_region" | "ml_region_gen2"
  std::string_view model_id;          // v1: feature version; gen2: model SHA-256
  std::string_view schema_sha256;     // v1: feature-schema SHA; gen2: representation SHA
  std::string_view token_contract_sha256;
  std::string_view rank_policy;       // gen2: "B35"
  std::string_view rank_plan_sha256;
  std::string_view weight_table_sha256;
  std::string_view unit_name;         // "windows" | "cells" (report key vocabulary)
  std::string (*identity_failure)();  // empty => compatible; otherwise the stable reason "model_identity"
  GenesisMlProposer propose;
};
// The two embedded producers. There is no registry: a different model is a different binary.
[[nodiscard]] const GenesisMlProducer &genesis_ml_producer(GenesisMlModelFamily family) noexcept;

// Classification of a stable fallback reason into its fallback class (contract section 5). Unknown reasons classify as "producer_input".
[[nodiscard]] std::string_view genesis_ml_fallback_class(std::string_view reason) noexcept;

struct GenesisMlAdmissionOutcome {
  bool admitted{};                 // the entries were replaced by the validator-accepted subset
  std::string reason{"none"};      // stable code
  std::string detail;              // sanitized prune failure class (prune_rejected only)
  std::string validator{"not_run"};  // accepted | rejected | not_run
  std::size_t units{}, selected{}, seed_units{};
  std::size_t universe{}, k0{}, k{}, pruned{}, rounds{}, broad{}, admitted_count{};
  std::string k_sha256;
};

// Runs the producer and, on a proposal, the prune and the validator against `program`/`entries`. `entries` is replaced ONLY when
// `admitted`. Pure otherwise; never throws for a bad proposal.
[[nodiscard]] GenesisMlAdmissionOutcome genesis_ml_admission(const GenesisMlProducer &producer, const FrontendProgram &program,
                                                             std::string_view rom_sha256, std::span<const std::uint8_t> rom,
                                                             std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries,
                                                             std::span<const std::uint32_t> seed_addresses);

// The sanitized one-line machine-readable report (stderr `segarecomp: m68k admission: requested=optimized ...`); no address, byte or per-unit datum.
[[nodiscard]] std::string genesis_ml_admission_report_line(const GenesisMlProducer &producer, const GenesisMlAdmissionOutcome &outcome);
// The same report for a request that never reached a producer (no analysis).
[[nodiscard]] std::string genesis_ml_admission_fallback_line(const GenesisMlProducer &producer, std::string_view reason);

// ---- identities -------------------------------------------------------------------------------------------------------------------
// ROM-independent policy identity (what `segarecomp aot-policy-identity` prints): a pure function of the analysis contract, the requested
// policy, the requested model family and the producer components. Any component change changes it.
// `producer == nullptr` selects the embedded producer of `requested_model`; tests pass a modified copy to prove every component is bound.
[[nodiscard]] std::string genesis_aot_policy_identity(std::string_view requested_policy, GenesisMlModelFamily requested_model,
                                                      const GenesisMlProducer *producer = nullptr);

struct GenesisAotBuildIdentityInput {
  std::string_view rom_sha256;
  std::string_view requested_policy;  // compatibility | optimized
  GenesisMlModelFamily requested_model{GenesisMlModelFamily::v1};
  std::string_view effective;         // broad | admission_plan | ml_region | ml_region_gen2
  std::string_view k_sha256;          // admitted-set digest when effective is an ML producer
};
// Per-build compatibility identity (`status.json` aot_policy.identity). Identities of Compatibility and v1 builds are frozen at their
// SEG-047 values (`segarecomp.aot_policy.v1|...`); a gen2 request has its own `v2` text. Fallback builds never alias an ML build.
[[nodiscard]] std::string genesis_aot_build_identity(const GenesisAotBuildIdentityInput &input, const GenesisMlProducer *producer = nullptr);

}  // namespace segarecomp
