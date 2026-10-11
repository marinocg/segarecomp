// SEG-049-T004 (ADR 0103): the shared ML admission owner and the model/policy lifecycle (platforms/genesis/machine ml_admission.hpp).
// Project-authored synthetic MC68000 bytes and synthetic producers only. Gates: the validator remains the authority (a producer can only
// propose plain ranges); every non-success is a classified fallback that leaves the broad entries untouched; an incompatible producer fails
// closed; no ML-specific type is visible to the prune/validator headers; compatibility identities are bound to every producer component and the
// frozen SEG-047 v1/Compatibility identities are unchanged.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "segarecomp/machine/genesis/ml_admission.hpp"
#include "segarecomp/sha256.hpp"

namespace {

using namespace segarecomp;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

const std::string sha(64U, 'a');

std::vector<std::uint8_t> image_bytes() {
  std::vector<std::uint8_t> bytes(0x800U, 0U);
  const auto words = [&](std::uint32_t at, std::initializer_list<std::uint16_t> values) {
    for (const auto value : values) {
      bytes[at] = static_cast<std::uint8_t>(value >> 8U);
      bytes[at + 1U] = static_cast<std::uint8_t>(value);
      at += 2U;
    }
  };
  words(0x0U, {0x00FFU, 0xFE00U, 0x0000U, 0x0200U});
  words(0x78U, {0x0000U, 0x0300U});
  words(0x200U, {0x4EB9U, 0x0000U, 0x0400U});
  words(0x206U, {0x60FEU});
  words(0x300U, {0x4E71U, 0x60FEU});
  words(0x400U, {0x4E71U, 0x4E75U});
  words(0x600U, {0x4E71U, 0x4E71U});  // unreachable
  return bytes;
}

struct Fixture {
  std::vector<std::uint8_t> bytes;
  FrontendProgram program;
  std::vector<FrontendAnalysis::ImmutableRomAotEntry> entries;
  std::vector<std::uint32_t> seeds;
};

Fixture fixture() {
  Fixture f;
  f.bytes = image_bytes();
  auto program = make_genesis_bridge_startup_program(f.bytes, 0U, 0x200U, std::nullopt);
  f.program = *program;
  (void)apply_genesis_immutable_rom_aot(f.program);
  const auto analysis = analyze_m68k_frontend(f.program);
  const FrontendAnalysis *direct = nullptr;
  if (const auto *accepted = std::get_if<FrontendAnalysis>(&analysis)) direct = accepted;
  else if (const auto *partial = std::get_if<FrontendPartialProgram>(&analysis)) direct = &partial->accepted_prefix;
  if (direct != nullptr) {
    f.entries = direct->immutable_rom_aot_entries;
    for (const auto &decoded : direct->decoded) f.seeds.push_back(static_cast<std::uint32_t>(decoded.provenance.source.address.value) & 0xFFFFFFU);
  }
  return f;
}

std::vector<std::uint32_t> addresses(const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries) {
  std::vector<std::uint32_t> out;
  for (const auto &entry : entries) out.push_back(static_cast<std::uint32_t>(entry.decoded.provenance.source.address.value) & 0xFFFFFFU);
  return out;
}

std::string no_failure() { return {}; }
std::string bad_identity() { return "model_identity"; }

GenesisMlProposal propose_fail(std::span<const std::uint8_t>, std::string_view, const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &,
                               std::span<const std::uint32_t>) {
  GenesisMlProposal p;
  p.failure = "rom_size";
  return p;
}
GenesisMlProposal propose_empty(std::span<const std::uint8_t>, std::string_view, const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &,
                                std::span<const std::uint32_t>) {
  return {};  // no proposal and no stated reason
}
GenesisMlProposal propose_wrong_rom(std::span<const std::uint8_t>, std::string_view, const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &,
                                    std::span<const std::uint32_t>) {
  GenesisMlProposal p;
  p.proposal = GenesisExecutableRegionProposal{std::string(64U, 'b'), {FrontendProgram::ImmutableRomAotRange{0U, 0x800U}}};
  p.units = 16U;
  p.selected = 16U;
  return p;
}
GenesisMlProposal propose_unreachable_only(std::span<const std::uint8_t>, std::string_view rom_sha, const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &,
                                           std::span<const std::uint32_t>) {
  GenesisMlProposal p;  // misses every machine root: the structural stage must reject it
  p.proposal = GenesisExecutableRegionProposal{std::string(rom_sha), {FrontendProgram::ImmutableRomAotRange{0x600U, 0x680U}}};
  p.units = 16U;
  p.selected = 1U;
  return p;
}
GenesisMlProposal propose_everything(std::span<const std::uint8_t>, std::string_view rom_sha, const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &,
                                     std::span<const std::uint32_t>) {
  GenesisMlProposal p;
  p.proposal = GenesisExecutableRegionProposal{std::string(rom_sha), {FrontendProgram::ImmutableRomAotRange{0U, 0x800U}}};
  p.units = 16U;
  p.selected = 16U;
  return p;
}
GenesisMlProposal propose_reachable_only(std::span<const std::uint8_t>, std::string_view rom_sha, const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &,
                                         std::span<const std::uint32_t>) {
  GenesisMlProposal p;  // the reachable code, not the unreachable block at $600
  p.proposal = GenesisExecutableRegionProposal{std::string(rom_sha), {FrontendProgram::ImmutableRomAotRange{0U, 0x500U}}};
  p.units = 16U;
  p.selected = 10U;
  return p;
}

GenesisMlProducer synthetic(std::string (*identity)(), GenesisMlProposer propose) {
  GenesisMlProducer p = genesis_ml_producer(GenesisMlModelFamily::gen2);
  p.identity_failure = identity;
  p.propose = propose;
  return p;
}

void test_fallbacks_leave_entries_untouched() {
  const Fixture f = fixture();
  expect(!f.entries.empty(), "fixture has broad identities");
  struct Case {
    GenesisMlProducer producer;
    const char *reason;
    const char *klass;
  };
  const Case cases[] = {
      {synthetic(&bad_identity, &propose_everything), "model_identity", "producer_incompatible"},
      {synthetic(&no_failure, &propose_fail), "rom_size", "producer_input"},
      {synthetic(&no_failure, &propose_empty), "empty_proposal", "producer_input"},
      {synthetic(&no_failure, &propose_wrong_rom), "prune_rejected", "proposal_rejected"},
      {synthetic(&no_failure, &propose_unreachable_only), "prune_rejected", "proposal_rejected"},
  };
  for (const Case &c : cases) {
    auto entries = f.entries;
    const auto outcome = genesis_ml_admission(c.producer, f.program, sha, f.bytes, entries, f.seeds);
    expect(!outcome.admitted && outcome.reason == c.reason, std::string("fallback reason ") + c.reason + " got " + outcome.reason);
    expect(genesis_ml_fallback_class(outcome.reason) == c.klass, std::string("fallback class of ") + c.reason);
    expect(addresses(entries) == addresses(f.entries), std::string("a fallback leaves the broad entries untouched: ") + c.reason);
    expect(outcome.validator != "accepted", "a fallback is never reported as validator-accepted");
    const std::string line = genesis_ml_admission_report_line(c.producer, outcome);
    expect(line.find("producer=broad fallback=1 reason=" + std::string(c.reason)) != std::string::npos && line.find("fallback_class=" + std::string(c.klass)) != std::string::npos &&
               line.find("model_family=gen2") != std::string::npos && line.find("ml_region_gen2 fallback=0") == std::string::npos,
           "a fallback line never claims selective execution: " + line);
  }
  const auto no_analysis = genesis_ml_admission_fallback_line(genesis_ml_producer(GenesisMlModelFamily::v1), "no_analysis");
  expect(no_analysis.find("reason=no_analysis") != std::string::npos && no_analysis.find("model_family=v1") != std::string::npos &&
             no_analysis.find("fallback_class=producer_input") != std::string::npos,
         "no_analysis line");
}

void test_validator_remains_authority() {
  const Fixture f = fixture();
  {
    auto entries = f.entries;
    const auto outcome = genesis_ml_admission(synthetic(&no_failure, &propose_everything), f.program, sha, f.bytes, entries, f.seeds);
    expect(outcome.admitted && outcome.validator == "accepted" && outcome.reason == "none", "a whole-image region is accepted by the unchanged validator");
    expect(addresses(entries) == addresses(f.entries) && outcome.k == f.entries.size(), "admitting everything equals the broad set");
    expect(outcome.k_sha256.size() == 64U, "admitted-set digest");
  }
  {
    auto entries = f.entries;
    const auto outcome = genesis_ml_admission(synthetic(&no_failure, &propose_reachable_only), f.program, sha, f.bytes, entries, f.seeds);
    expect(outcome.admitted && outcome.validator == "accepted", "the reachable region is accepted");
    expect(entries.size() < f.entries.size() && outcome.admitted_count == entries.size(), "unselected identities are removed only through the validator-accepted plan");
    const auto kept = addresses(entries);
    expect(std::none_of(kept.begin(), kept.end(), [](std::uint32_t a) { return a >= 0x600U; }), "the unproposed block is not admitted");
    const std::string line = genesis_ml_admission_report_line(synthetic(&no_failure, &propose_reachable_only), outcome);
    expect(line.find("producer=ml_region_gen2 fallback=0 reason=none") != std::string::npos && line.find("validator=accepted") != std::string::npos &&
               line.find("cells=16") != std::string::npos && line.find("contract=" + std::string(genesis_admission_analysis_contract)) != std::string::npos,
           "accepted line: " + line);
  }
  // Exact map precedence is structural (CLI option exclusion; tested in the CLI test): the owner has no plan input at all.
  // The real gen2 producer on this tiny image: whatever it decides, the invariants hold.
  {
    auto entries = f.entries;
    const auto outcome = genesis_ml_admission(genesis_ml_producer(GenesisMlModelFamily::gen2), f.program, sha, f.bytes, entries, f.seeds);
    if (outcome.admitted) expect(outcome.validator == "accepted" && entries.size() <= f.entries.size(), "gen2 admitted invariants");
    else expect(addresses(entries) == addresses(f.entries) && !genesis_ml_fallback_class(outcome.reason).empty(), "gen2 fallback invariants: " + outcome.reason);
  }
  {
    auto entries = f.entries;
    const auto outcome = genesis_ml_admission(genesis_ml_producer(GenesisMlModelFamily::v1), f.program, sha, f.bytes, entries, f.seeds);
    if (outcome.admitted) expect(outcome.validator == "accepted", "v1 admitted invariants");
    else expect(addresses(entries) == addresses(f.entries), "v1 fallback invariants: " + outcome.reason);
  }
}

void test_classification_table() {
  for (const char *reason : {"rom_size", "token_range", "non_finite_score", "empty_proposal", "no_analysis", "no_report", "whatever"})
    expect(genesis_ml_fallback_class(reason) == "producer_input", std::string("class of ") + reason);
  expect(genesis_ml_fallback_class("none") == "none" && genesis_ml_fallback_class("model_identity") == "producer_incompatible" &&
             genesis_ml_fallback_class("prune_rejected") == "proposal_rejected" && genesis_ml_fallback_class("validator_rejected") == "validator_rejected" &&
             genesis_ml_fallback_class("platform_not_applicable") == "not_applicable" && genesis_ml_fallback_class("plan_alias_set_mismatch") == "exact_map" &&
             genesis_ml_fallback_class("exact_plan_precedence") == "exact_map",
         "classification table");
}

std::string h(const std::string &text) { return sha256_hex({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}); }

void test_identities() {
  const auto &v1 = genesis_ml_producer(GenesisMlModelFamily::v1);
  const auto &g2 = genesis_ml_producer(GenesisMlModelFamily::gen2);
  expect(v1.producer_id == "ml_region" && g2.producer_id == "ml_region_gen2" && v1.model_id != g2.model_id, "producer ids");
  expect(g2.model_id == "11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1" && g2.rank_policy == "B35" && g2.token_contract_sha256.size() == 64U &&
             g2.rank_plan_sha256.size() == 64U && g2.weight_table_sha256.size() == 64U && g2.schema_sha256.size() == 64U,
         "gen2 identity components are embedded");
  expect(genesis_ml_producer(GenesisMlModelFamily::v1).identity_failure().empty() && genesis_ml_producer(GenesisMlModelFamily::gen2).identity_failure().empty(),
         "both embedded producers are compatible");

  // Policy identity (ROM-independent): every component and the request are bound.
  const std::string compat = genesis_aot_policy_identity("compatibility", GenesisMlModelFamily::v1);
  const std::string opt_v1 = genesis_aot_policy_identity("optimized", GenesisMlModelFamily::v1);
  const std::string opt_g2 = genesis_aot_policy_identity("optimized", GenesisMlModelFamily::gen2);
  expect(compat != opt_v1 && opt_v1 != opt_g2 && compat != opt_g2, "policy identities of the three requests differ");
  expect(compat == genesis_aot_policy_identity("compatibility", GenesisMlModelFamily::gen2), "an unused model family does not change the Compatibility identity");
  expect(opt_g2 == genesis_aot_policy_identity("optimized", GenesisMlModelFamily::gen2) && opt_g2.size() == 64U, "deterministic");
  const auto mutated_differs = [&](auto mutate, const char *what) {
    GenesisMlProducer m = g2;
    mutate(m);
    expect(genesis_aot_policy_identity("optimized", GenesisMlModelFamily::gen2, &m) != opt_g2, std::string("policy identity binds ") + what);
    const std::string admitted_digest(64U, 'c');  // the input holds views: keep the storage alive
    GenesisAotBuildIdentityInput in{sha, "optimized", GenesisMlModelFamily::gen2, "ml_region_gen2", admitted_digest};
    expect(genesis_aot_build_identity(in, &m) != genesis_aot_build_identity(in), std::string("build identity binds ") + what);
  };
  mutated_differs([](GenesisMlProducer &m) { m.model_id = "x"; }, "the model id (cache invalidation after a model change)");
  mutated_differs([](GenesisMlProducer &m) { m.schema_sha256 = "x"; }, "the representation/feature schema");
  mutated_differs([](GenesisMlProducer &m) { m.token_contract_sha256 = "x"; }, "the CPU token contract");
  mutated_differs([](GenesisMlProducer &m) { m.rank_policy = "B40"; }, "the rank policy (cache invalidation after a rank-policy change)");
  mutated_differs([](GenesisMlProducer &m) { m.rank_plan_sha256 = "x"; }, "the rank plan");
  mutated_differs([](GenesisMlProducer &m) { m.weight_table_sha256 = "x"; }, "the weight table");

  // Build identity: frozen v1 / Compatibility text of SEG-047, distinct gen2 text, no aliasing across policy, model, ROM, effective producer or admitted set.
  const std::string k(64U, 'd');
  const std::string v1_text = "segarecomp.aot_policy.v1|rom=" + sha + "|requested=optimized|effective=ml_region|model=seg046-features-v1|schema=" + std::string(v1.schema_sha256) + "|k=" + k;
  expect(genesis_aot_build_identity({sha, "optimized", GenesisMlModelFamily::v1, "ml_region", k}) == h(v1_text), "v1 build identity is frozen at its SEG-047 value");
  const std::string compat_text = "segarecomp.aot_policy.v1|rom=" + sha + "|requested=compatibility|effective=broad|model=-|schema=-|k=-";
  expect(genesis_aot_build_identity({sha, "compatibility", GenesisMlModelFamily::v1, "broad", ""}) == h(compat_text), "Compatibility build identity is frozen");
  const std::string fb_text = "segarecomp.aot_policy.v1|rom=" + sha + "|requested=optimized|effective=broad|model=-|schema=-|k=-";
  expect(genesis_aot_build_identity({sha, "optimized", GenesisMlModelFamily::v1, "broad", ""}) == h(fb_text), "v1 fallback build identity is frozen");
  const auto id = [&](const char *policy, GenesisMlModelFamily fam, const char *effective, const std::string &kk, const std::string &rom = sha) {
    return genesis_aot_build_identity({rom, policy, fam, effective, kk});
  };
  const std::string ids[] = {
      id("compatibility", GenesisMlModelFamily::v1, "broad", ""),        id("optimized", GenesisMlModelFamily::v1, "broad", ""),
      id("optimized", GenesisMlModelFamily::gen2, "broad", ""),          id("optimized", GenesisMlModelFamily::v1, "ml_region", k),
      id("optimized", GenesisMlModelFamily::gen2, "ml_region_gen2", k),  id("optimized", GenesisMlModelFamily::gen2, "ml_region_gen2", std::string(64U, 'e')),
      id("optimized", GenesisMlModelFamily::gen2, "ml_region_gen2", k, std::string(64U, 'f')), id("optimized", GenesisMlModelFamily::gen2, "admission_plan", ""),
  };
  for (std::size_t i = 0; i < std::size(ids); ++i)
    for (std::size_t j = i + 1U; j < std::size(ids); ++j) expect(ids[i] != ids[j], "build identities " + std::to_string(i) + " and " + std::to_string(j) + " never alias");
  expect(id("compatibility", GenesisMlModelFamily::gen2, "broad", "") == ids[0], "a model family never changes a Compatibility build identity");
}

}  // namespace

int main() {
  test_classification_table();
  test_fallbacks_leave_entries_untouched();
  test_validator_remains_authority();
  test_identities();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "genesis_ml_admission_lifecycle_test: ok\n";
  return 0;
}
