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

Fixture fixture(bool ram_thunk = false, bool copy_alias = false) {
  Fixture f;
  f.bytes = image_bytes();
  auto program = make_genesis_bridge_startup_program(f.bytes, 0U, 0x200U, std::nullopt);
  f.program = *program;
  (void)apply_genesis_immutable_rom_aot(f.program);
  if (copy_alias) expect(apply_genesis_immutable_copy_alias(f.program, 0xFF0200U, 0x400U, 4U), "fixture: copy alias accepted");
  if (ram_thunk) {
    const std::uint8_t jmp[] = {0x4EU, 0xF9U, 0x00U, 0x00U, 0x06U, 0x00U};  // JMP $000600.L: a target the proposal below leaves out
    expect(apply_genesis_materialized_ram_thunk(f.program, 0xFF0100U, jmp), "fixture: RAM thunk accepted");
  }
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

bool holds(const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries, std::uint32_t pc) {
  const auto a = addresses(entries);
  return std::find(a.begin(), a.end(), pc) != a.end();
}

// SEG-122-T010 (ADR 0270): closure of a thunk-bearing gen2 emission after a prune rejection, and every case that must not use it.
void test_ram_thunk_closure() {
  const auto gen2 = synthetic(&no_failure, &propose_reachable_only);
  const Fixture t = fixture(true);
  expect(holds(t.entries, 0xFF0100U) && holds(t.entries, 0x600U), "fixture: thunk and its target are broad identities");
  GenesisRegionPruneResult direct;
  expect(!plan_genesis_region_admission(t.program, sha, t.entries, *propose_reachable_only(t.bytes, sha, t.entries, t.seeds).proposal, direct) &&
             direct.failure_class == "outside_region", "the unchanged prune still rejects the thunk emission");
  {
    auto entries = t.entries;
    const auto o = genesis_ml_admission(gen2, t.program, sha, t.bytes, entries, t.seeds);
    expect(o.admitted && o.closure_used && o.validator == "accepted" && o.reason == "none", "gen2: a thunk emission stays selective: " + o.reason);
    expect(holds(entries, 0xFF0100U) && holds(entries, 0x600U) && holds(entries, 0x602U), "the thunk and the fixed flow of its target are admitted");
    expect(entries.size() < t.entries.size() && o.pruned == 0U && o.thunks == 1U && o.k == o.k0 + o.added && o.roots_added >= 1U && o.roots_added <= o.added, "closure counts");
    auto again = t.entries;
    const auto second = genesis_ml_admission(gen2, t.program, sha, t.bytes, again, t.seeds);
    expect(second.k_sha256 == o.k_sha256 && addresses(again) == addresses(entries), "closure is deterministic across runs");
    const auto line = genesis_ml_admission_report_line(gen2, o);
    expect(line.find(" closure=close added=" + std::to_string(o.added) + " roots_added=" + std::to_string(o.roots_added) + " thunks=1") != std::string::npos &&
               line.find("fallback=0") != std::string::npos && line.find("0xff") == std::string::npos, "closure report line: " + line);
    GenesisMlAdmissionOutcome plain = o;
    plain.closure_used = false;
    expect(genesis_ml_admission_report_line(gen2, plain).find("closure=") == std::string::npos, "a non-closed line has no closure fields");
  }
  {  // v1 stays prune-only.
    GenesisMlProducer v1 = gen2;
    v1.family = GenesisMlModelFamily::v1;
    auto entries = t.entries;
    const auto o = genesis_ml_admission(v1, t.program, sha, t.bytes, entries, t.seeds);
    expect(!o.admitted && !o.closure_used && o.reason == "prune_rejected" && o.detail == "outside_region" && addresses(entries) == addresses(t.entries), "v1: broad fallback, unchanged");
  }
  {  // An exhausted cap is the broad fallback, never a partial program.
    auto entries = t.entries;
    const auto o = genesis_ml_admission(gen2, t.program, sha, t.bytes, entries, t.seeds, 1U);
    expect(!o.admitted && !o.closure_used && o.reason == "prune_rejected" && o.detail == "closure_rejected" && o.validator == "not_run" && addresses(entries) == addresses(t.entries), "cap: broad fallback");
  }
  {  // A proposal for another image is not closed.
    auto entries = t.entries;
    const auto o = genesis_ml_admission(synthetic(&no_failure, &propose_wrong_rom), t.program, sha, t.bytes, entries, t.seeds);
    expect(!o.admitted && !o.closure_used && addresses(entries) == addresses(t.entries), "wrong ROM: broad fallback");
  }
  {  // No thunk: a rejected prune stays the broad fallback, also with a copy alias whose source is outside the proposal.
    const Fixture plain = fixture();
    auto entries = plain.entries;
    const auto o = genesis_ml_admission(synthetic(&no_failure, &propose_unreachable_only), plain.program, sha, plain.bytes, entries, plain.seeds);
    expect(!o.admitted && !o.closure_used && o.reason == "prune_rejected" && addresses(entries) == addresses(plain.entries), "no thunk: broad fallback");
    const Fixture alias = fixture(false, true);
    auto alias_entries = alias.entries;
    const auto a = genesis_ml_admission(synthetic(&no_failure, &propose_unreachable_only), alias.program, sha, alias.bytes, alias_entries, alias.seeds);
    expect(!a.admitted && !a.closure_used && a.reason == "prune_rejected" && addresses(alias_entries) == addresses(alias.entries), "alias without thunk: broad fallback");
  }
  {  // The trigger is any prune rejection once a thunk exists (frozen in ADR 0269), not only the thunk's own: a proposal missing every machine root is completed too.
    auto entries = t.entries;
    const auto o = genesis_ml_admission(synthetic(&no_failure, &propose_unreachable_only), t.program, sha, t.bytes, entries, t.seeds);
    expect(o.admitted && o.closure_used && o.validator == "accepted" && holds(entries, 0x200U) && holds(entries, 0x600U) && o.roots_added >= 2U, "a thunk emission closes a rootless proposal");
    const Fixture alias_thunk = fixture(true, true);  // copy alias + thunk: both are mandatory, and the alias source need not be proposed
    auto both = alias_thunk.entries;
    const auto b = genesis_ml_admission(synthetic(&no_failure, &propose_unreachable_only), alias_thunk.program, sha, alias_thunk.bytes, both, alias_thunk.seeds);
    expect(b.admitted && b.closure_used && holds(both, 0xFF0100U) && holds(both, 0xFF0200U) && b.k == b.k0 + b.added, "alias and thunk are both admitted by closure");
  }
  {  // A successful prune does not change: same K as the plain prune, no closure fields.
    const Fixture plain = fixture();
    auto entries = plain.entries;
    const auto o = genesis_ml_admission(gen2, plain.program, sha, plain.bytes, entries, plain.seeds);
    GenesisRegionPruneResult prune;
    expect(plan_genesis_region_admission(plain.program, sha, plain.entries, *propose_reachable_only(plain.bytes, sha, plain.entries, plain.seeds).proposal, prune).has_value(), "plain prune accepts");
    expect(o.admitted && !o.closure_used && o.added == 0U && o.k == prune.admitted.size() && o.k_sha256 == genesis_hybrid_admission_universe_digest(prune.admitted), "a successful prune is unchanged");
  }
  {  // A closed plan is an ordinary plan: wrong ROM, alias set or universe is refused by the unchanged validator.
    GenesisRegionPruneResult closed;
    const auto proposal = *propose_reachable_only(t.bytes, sha, t.entries, t.seeds).proposal;
    const auto plan = plan_genesis_region_admission(t.program, sha, t.entries, proposal, closed, genesis_region_prune_default_max_rounds, GenesisRegionAdmissionClosure::close);
    expect(plan.has_value(), "closed plan produced");
    if (plan) {
      auto wrong_rom = *plan; wrong_rom.rom_sha256 = std::string(64U, 'c');
      auto wrong_universe = *plan; wrong_universe.universe_sha256 = std::string(64U, 'c');
      auto wrong_alias = *plan; wrong_alias.aliases.push_back({0xFF0300U, 0x400U, 4U});
      auto missing_thunk = *plan;
      for (auto &range : missing_thunk.ranges)
        if (range.end_address > 0xFF0100U) range.end_address = 0xFF0100U;  // the run holding the thunk now ends before it
      for (const auto *bad : {&wrong_rom, &wrong_universe, &wrong_alias, &missing_thunk}) {
        auto entries = t.entries;
        expect(apply_genesis_hybrid_admission(t.program, sha, *bad, entries).has_value() && addresses(entries) == addresses(t.entries), "the validator refuses a tampered closed plan");
      }
    }
  }
  // The byte-identity proof gate is before admission: a descriptor that is not a verbatim copy never becomes an identity.
  {
    Fixture bad = fixture();
    expect(!apply_genesis_immutable_copy_alias(bad.program, 0xFF0200U, 0x7FEU, 4U), "an alias whose source leaves the image is refused");
    const std::uint8_t not_jmp[] = {0x4EU, 0x71U, 0x4EU, 0x71U, 0x4EU, 0x71U};
    expect(!apply_genesis_materialized_ram_thunk(bad.program, 0xFF0100U, not_jmp), "bytes that are not exactly one JMP are refused as a thunk");
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
  test_ram_thunk_closure();
  test_identities();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "genesis_ml_admission_lifecycle_test: ok\n";
  return 0;
}
