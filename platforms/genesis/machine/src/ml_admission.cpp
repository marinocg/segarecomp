// SEG-049-T004 (ADR 0103): shared ML-proposed selective admission owner. See ml_admission.hpp.

#include "segarecomp/machine/genesis/ml_admission.hpp"

#include "segarecomp/machine/genesis/ml_region_gen2_producer.hpp"
#include "segarecomp/machine/genesis/ml_region_producer.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp {

namespace {
// v1 identity components (ADR 0096 section 2): pinned in ml_region_producer.cpp; repeated here only as report labels.
constexpr std::string_view v1_schema_sha256 = "d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570";

GenesisMlProposal propose_v1(std::span<const std::uint8_t> rom, std::string_view rom_sha256,
                             const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries, std::span<const std::uint32_t> seeds) {
  auto result = propose_genesis_ml_executable_regions(rom, rom_sha256, entries, seeds);
  GenesisMlProposal out;
  out.proposal = std::move(result.proposal);
  out.failure = std::move(result.failure);
  out.units = result.stats.windows;
  out.selected = result.stats.ml_selected;
  out.seed_units = result.stats.seed_windows;
  return out;
}

GenesisMlProposal propose_gen2(std::span<const std::uint8_t> rom, std::string_view rom_sha256,
                               const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &, std::span<const std::uint32_t> seeds) {
  auto result = propose_genesis_ml2_executable_regions(rom, rom_sha256, seeds);
  GenesisMlProposal out;
  out.proposal = std::move(result.proposal);
  out.failure = std::move(result.failure);
  out.units = result.stats.cells;
  out.selected = result.stats.ml_selected;
  out.seed_units = result.stats.seed_cells;
  return out;
}

std::string hash_text(const std::string &text) {
  return sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

std::string safe(std::string_view value) {
  std::string out;
  for (const char c : value) out.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_');
  return out;
}
}  // namespace

std::string_view genesis_ml_model_family_name(GenesisMlModelFamily family) noexcept { return family == GenesisMlModelFamily::gen2 ? "gen2" : "v1"; }

const GenesisMlProducer &genesis_ml_producer(GenesisMlModelFamily family) noexcept {
  static const GenesisMlProducer v1{GenesisMlModelFamily::v1, "ml_region", genesis_ml_feature_version, v1_schema_sha256, "", "", "", "", "windows",
                                    &genesis_ml_model_identity_failure, &propose_v1};
  static const GenesisMlProducer gen2{GenesisMlModelFamily::gen2, "ml_region_gen2", genesis_ml2_identity().model_sha256, genesis_ml2_identity().representation_sha256,
                                      genesis_ml2_identity().token_schema_sha256, genesis_ml2_identity().policy_id, genesis_ml2_identity().rank_plan_sha256,
                                      genesis_ml2_identity().weight_table_sha256, "cells", &genesis_ml2_model_identity_failure, &propose_gen2};
  return family == GenesisMlModelFamily::gen2 ? gen2 : v1;
}

std::string_view genesis_ml_fallback_class(std::string_view reason) noexcept {
  if (reason == "none") return "none";
  if (reason == "model_identity") return "producer_incompatible";
  if (reason == "prune_rejected") return "proposal_rejected";
  if (reason == "validator_rejected") return "validator_rejected";
  if (reason == "platform_not_applicable") return "not_applicable";
  if (reason == "exact_plan_precedence" || reason == "plan_alias_set_mismatch") return "exact_map";
  return "producer_input";  // rom_size, token_range, non_finite_score, empty_proposal, no_analysis and anything unclassified
}

GenesisMlAdmissionOutcome genesis_ml_admission(const GenesisMlProducer &producer, const FrontendProgram &program, std::string_view rom_sha256,
                                               std::span<const std::uint8_t> rom, std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries,
                                               std::span<const std::uint32_t> seed_addresses) {
  GenesisMlAdmissionOutcome out;
  out.broad = entries.size();
  if (producer.identity_failure == nullptr || producer.propose == nullptr || !producer.identity_failure().empty()) {
    out.reason = "model_identity";
    return out;
  }
  const GenesisMlProposal proposal = producer.propose(rom, rom_sha256, entries, seed_addresses);
  out.units = proposal.units;
  out.selected = proposal.selected;
  out.seed_units = proposal.seed_units;
  if (!proposal.proposal) {
    out.reason = proposal.failure.empty() ? "empty_proposal" : proposal.failure;
    return out;
  }
  GenesisRegionPruneResult pruned;
  const auto plan = plan_genesis_region_admission(program, rom_sha256, entries, *proposal.proposal, pruned);
  out.universe = pruned.universe_count;
  out.k0 = pruned.k0_count;
  out.pruned = pruned.pruned_count;
  out.rounds = pruned.rounds;
  if (!plan) {
    out.reason = "prune_rejected";
    out.detail = pruned.failure_class.empty() ? "unclassified" : pruned.failure_class;
    return out;
  }
  auto scratch = entries;
  if (apply_genesis_hybrid_admission(program, rom_sha256, *plan, scratch).has_value()) {
    out.reason = "validator_rejected";
    out.validator = "rejected";
    return out;
  }
  out.k = pruned.admitted.size();
  out.k_sha256 = genesis_hybrid_admission_universe_digest(pruned.admitted);
  out.admitted_count = scratch.size();
  out.validator = "accepted";
  entries = std::move(scratch);  // exactly what an explicit plan would admit
  out.admitted = true;
  return out;
}

std::string genesis_ml_admission_report_line(const GenesisMlProducer &producer, const GenesisMlAdmissionOutcome &o) {
  const std::string family(genesis_ml_model_family_name(producer.family));
  if (!o.admitted)
    return "segarecomp: m68k admission: requested=optimized producer=broad fallback=1 reason=" + o.reason +
           (o.detail.empty() ? "" : " detail=" + o.detail) + " model_family=" + family + " fallback_class=" + std::string(genesis_ml_fallback_class(o.reason)) +
           " validator=" + o.validator + '\n';
  const bool v1 = producer.family == GenesisMlModelFamily::v1;
  std::string line = "segarecomp: m68k admission: requested=optimized producer=" + std::string(producer.producer_id) + " fallback=0 reason=none model=" +
                     std::string(producer.model_id) + " schema=" + std::string(producer.schema_sha256);
  if (!v1)
    line += " token_contract=" + std::string(producer.token_contract_sha256) + " rank_policy=" + std::string(producer.rank_policy) +
            " rank_plan=" + std::string(producer.rank_plan_sha256) + " weights=" + std::string(producer.weight_table_sha256);
  line += std::string(" ") + std::string(producer.unit_name) + "=" + std::to_string(o.units) + " ml_selected=" + std::to_string(o.selected) +
          (v1 ? " seed_windows=" : " seed_cells=") + std::to_string(o.seed_units) + " universe=" + std::to_string(o.universe) + " k0=" + std::to_string(o.k0) +
          " k=" + std::to_string(o.k) + " pruned=" + std::to_string(o.pruned) + " rounds=" + std::to_string(o.rounds) + " broad=" + std::to_string(o.broad) +
          " admitted=" + std::to_string(o.admitted_count) + " k_sha256=" + o.k_sha256 + " validator=accepted model_family=" + family +
          " fallback_class=none contract=" + std::string(genesis_admission_analysis_contract) + '\n';
  return line;
}

std::string genesis_ml_admission_fallback_line(const GenesisMlProducer &producer, std::string_view reason) {
  GenesisMlAdmissionOutcome o;
  o.reason = std::string(reason);
  return genesis_ml_admission_report_line(producer, o);
}

std::string genesis_aot_policy_identity(std::string_view requested_policy, GenesisMlModelFamily requested_model, const GenesisMlProducer *producer) {
  const GenesisMlProducer &p = producer != nullptr ? *producer : genesis_ml_producer(requested_model);
  std::string text = "segarecomp.aot_policy_identity.v1|contract=" + std::string(genesis_admission_analysis_contract) + "|requested=" + safe(requested_policy);
  if (requested_policy == "optimized")
    text += "|family=" + std::string(genesis_ml_model_family_name(requested_model)) + "|producer=" + std::string(p.producer_id) + "|model=" + std::string(p.model_id) +
            "|schema=" + std::string(p.schema_sha256) + "|tokens=" + std::string(p.token_contract_sha256) + "|rank=" + std::string(p.rank_policy) + ":" +
            std::string(p.rank_plan_sha256) + "|weights=" + std::string(p.weight_table_sha256);
  return hash_text(text);
}

std::string genesis_aot_build_identity(const GenesisAotBuildIdentityInput &in, const GenesisMlProducer *producer) {
  const bool ml = in.effective == "ml_region" || in.effective == "ml_region_gen2";
  const bool gen2_request = in.requested_policy == "optimized" && in.requested_model == GenesisMlModelFamily::gen2;
  if (!gen2_request) {
    // Frozen SEG-047 text (v1 and Compatibility builds): unchanged so existing cache/report identities stay valid.
    const GenesisMlProducer &p = producer != nullptr ? *producer : genesis_ml_producer(GenesisMlModelFamily::v1);
    const std::string text = "segarecomp.aot_policy.v1|rom=" + std::string(in.rom_sha256) + "|requested=" + safe(in.requested_policy) + "|effective=" +
                             safe(in.effective) + "|model=" + (ml ? safe(p.model_id) : std::string("-")) + "|schema=" + (ml ? safe(p.schema_sha256) : std::string("-")) +
                             "|k=" + (ml ? safe(in.k_sha256) : std::string("-"));
    return hash_text(text);
  }
  const GenesisMlProducer &p = producer != nullptr ? *producer : genesis_ml_producer(GenesisMlModelFamily::gen2);
  const std::string text = "segarecomp.aot_policy.v2|rom=" + std::string(in.rom_sha256) + "|requested=optimized|requested_model=gen2|effective=" + safe(in.effective) +
                           "|contract=" + std::string(genesis_admission_analysis_contract) + "|model=" + (ml ? safe(p.model_id) : std::string("-")) +
                           "|schema=" + (ml ? safe(p.schema_sha256) : std::string("-")) + "|tokens=" + (ml ? safe(p.token_contract_sha256) : std::string("-")) +
                           "|rank=" + (ml ? safe(p.rank_policy) + ":" + safe(p.rank_plan_sha256) : std::string("-")) +
                           "|weights=" + (ml ? safe(p.weight_table_sha256) : std::string("-")) + "|k=" + (ml ? safe(in.k_sha256) : std::string("-"));
  return hash_text(text);
}

}  // namespace segarecomp
