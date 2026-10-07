// SEG-031 (ADR 0080): the report-only Genesis M68K hybrid admission planner on project-authored synthetic MC68000 fixtures (real
// encodings decoded by the unchanged decoder/lifter; no commercial input).
//
// Each fixture is one adversarial shape of the containment invariant `PossibleTargets(site) ⊆ island entries ⊆ H`:
//   exact finite target (no island); one bounded points-to region; a target in either of two executable images; a direct edge
//   leaving an island (closure); island code with a further uncovered transfer (second island); cyclic growth converging; overlapping
//   islands canonicalized; a RAM mirror of an alias; a materialized RAM image admitted whole; an Unknown image identity widening to
//   broad; the operand-width rule refused; a vector root kept; the round and island-entry bounds giving broad; invalid image
//   provenance giving broad. Every hybrid result also passes the independent validator, is a subset of broad U, and is
//   deterministic; the production artifact round-trips through the strict parser.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "segarecomp/genesis_analysis_report/hybrid_plan.hpp"

namespace {

using namespace segarecomp;

int failures = 0;
bool fail_fast = false;  // `--fail-fast`: exit at the first failed check (the mutation harness uses it so killed mutants stop early)
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
    if (fail_fast) std::exit(EXIT_FAILURE);
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t slots = 128U;        // code slots per table (above the 64 exact-offset bound: the pointer stays strided)
constexpr std::uint32_t ptrs = 0x800U;        // immutable code-pointer tables (128 longs each)
constexpr std::uint32_t ptrs_b = 0xA00U;
constexpr std::uint32_t ptrs_two = 0xC00U;    // 256 longs: 128 ROM slots then 128 alias slots
constexpr std::uint32_t table = 0x1000U;      // slot code, 16 bytes per slot
constexpr std::uint32_t table_b = 0x1800U;
constexpr std::uint32_t junk = 0x3000U;       // decodable but unreachable code (in U, never in H)
constexpr std::uint32_t alias_base = 0xFF0000U;
constexpr std::uint32_t alias_source = 0x2000U;  // alias source: 128 slots of 16 bytes (to $2800)

struct Image {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x4000U, 0U);
  Image() {
    put32(0x0U, 0x00FFFE00U);
    put32(0x4U, entry);
    for (std::uint32_t at = junk; at < junk + 0x400U; at += 2U) words(at, {0x4E71U});  // NOP
  }
  void put32(std::uint32_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
  }
  void words(std::uint32_t at, std::initializer_list<std::uint16_t> values) {
    for (const auto value : values) {
      bytes[at] = static_cast<std::uint8_t>(value >> 8U);
      bytes[at + 1U] = static_cast<std::uint8_t>(value);
      at += 2U;
    }
  }
};

struct Asm {
  Image &image;
  std::uint32_t pc;
  Asm &w(std::uint32_t value) {
    image.words(pc, {static_cast<std::uint16_t>(value)});
    pc += 2U;
    return *this;
  }
  Asm &l(std::uint32_t value) { return w(value >> 16U).w(value); }
  Asm &move_w_ram(unsigned d, std::uint16_t address) { return w(0x3038U | d << 9U).w(address); }      // MOVE.W (xxx).W,Dd
  Asm &movea_l_ram(unsigned a, std::uint16_t address) { return w(0x2078U | a << 9U).w(address); }    // MOVEA.L (xxx).W,Aa
  Asm &tst_w_ram(std::uint16_t address) { return w(0x4A78U).w(address); }                          // TST.W (xxx).W
  Asm &cmpi_w(unsigned d, std::uint16_t imm) { return w(0x0C40U | d).w(imm); }                     // CMPI.W #imm,Dd
  Asm &bcc_s(unsigned condition, std::uint32_t to) { return w(0x6000U | condition << 8U | ((to - pc - 2U) & 0xFFU)); }
  Asm &bra_w(std::uint32_t to) { return w(0x6000U).w((to - pc) & 0xFFFFU); }                       // BRA.W (disp from ext word)
  Asm &lsl_w4(unsigned d) { return w(0xE948U | d); }                                                // LSL.W #4,Dd
  Asm &lea_abs(unsigned a, std::uint32_t address) { return w(0x41F9U | a << 9U).l(address); }      // LEA (xxx).L,Aa
  Asm &adda_w(unsigned d, unsigned a) { return w(0xD0C0U | a << 9U | d); }                         // ADDA.W Dd,Aa
  Asm &jmp_an(unsigned a) { return w(0x4ED0U | a); }                                                // JMP (Aa)
  Asm &jsr_an(unsigned a) { return w(0x4E90U | a); }                                                // JSR (Aa)
  Asm &andi_w(unsigned d, std::uint16_t imm) { return w(0x0240U | d).w(imm); }                     // ANDI.W #imm,Dd
  Asm &movea_idx(unsigned destination, unsigned base, unsigned index) {                               // MOVEA.L 0(Ab,Di.W),Ad
    return w(0x2070U | destination << 9U | base).w(index << 12U);
  }
  Asm &jmp_abs(std::uint32_t to) { return w(0x4EF9U).l(to); }
  Asm &jmp_pcidx(unsigned index, std::uint32_t target_table) { return w(0x4EFBU).w(index << 12U | ((target_table - pc) & 0xFFU)); }
  Asm &move_b_ram(unsigned d, std::uint16_t address) { return w(0x1038U | d << 9U).w(address); }
  Asm &nop() { return w(0x4E71U); }
  Asm &rts() { return w(0x4E75U); }
  Asm &bra_self() { return w(0x60FEU); }
};

constexpr unsigned EQ = 7U;

// A1 = one of `count` code pointers read from the immutable table at `pointers` through a masked RAM index: a proven strided
// points-to set (above the 64 exact-offset bound, so the address domain never enumerates it and the site stays unresolved).
Asm &strided_pointer(Asm &a, std::uint32_t pointers, std::uint32_t count = slots) {
  return a.move_w_ram(0, 0xF000U).andi_w(0, static_cast<std::uint16_t>(4U * count - 4U)).lea_abs(0, pointers).movea_idx(1, 0, 0);
}

void pointer_table(Image &image, std::uint32_t pointers, std::uint32_t code, std::uint32_t count = slots) {
  for (std::uint32_t k = 0; k < count; ++k) image.put32(pointers + 4U * k, code + 16U * k);
}

std::optional<FrontendProgram> program_of(const Image &image, bool with_alias = false) {
  auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  if (!program || !apply_genesis_immutable_rom_aot(*program)) return std::nullopt;
  if (with_alias && !apply_genesis_immutable_copy_alias(*program, alias_base, alias_source, slots * 16U)) return std::nullopt;
  return program;
}

GenesisHybridPlan plan_of(const FrontendProgram &program, GenesisHybridPlanConfig config = {}) {
  return plan_genesis_hybrid_admission(program, config);
}

bool admitted(const GenesisHybridPlan &plan, std::uint32_t pc) {
  return std::binary_search(plan.admitted.begin(), plan.admitted.end(), pc);
}
bool in_universe(const GenesisHybridPlan &plan, std::uint32_t pc) {
  return std::binary_search(plan.universe.begin(), plan.universe.end(), pc);
}

// Common invariants of a hybrid outcome: hybrid ⊆ broad, junk excluded, deterministic, artifact round-trip, and a strict reduction.
void common(const std::string &name, const FrontendProgram &program, const GenesisHybridPlan &plan, GenesisHybridPlanConfig config = {}) {
  expect(plan.outcome == GenesisHybridOutcome::hybrid, name + ": hybrid outcome (got " + genesis_hybrid_outcome_name(plan.outcome) + ")");
  expect(std::includes(plan.universe.begin(), plan.universe.end(), plan.admitted.begin(), plan.admitted.end()), name + ": hybrid ⊆ broad");
  expect(plan.admitted.size() < plan.universe.size(), name + ": the hybrid is smaller than broad");
  expect(in_universe(plan, junk) && !admitted(plan, junk), name + ": unreachable decodable code stays outside the hybrid");
  const auto again = plan_of(program, config);
  expect(again.admitted == plan.admitted && again.island_entries == plan.island_entries && again.rounds == plan.rounds &&
             format_genesis_hybrid_plan_aggregate(again) == format_genesis_hybrid_plan_aggregate(plan),
         name + ": deterministic");
  const auto artifact = genesis_hybrid_admission_plan(plan, program, std::string(64U, 'a'));
  expect(artifact.strategy == GenesisAdmissionStrategy::hybrid, name + ": hybrid artifact");
  const auto parsed = parse_genesis_hybrid_admission_plan(format_genesis_hybrid_admission_plan(artifact));
  expect(parsed && *parsed == artifact, name + ": the artifact round-trips through the strict parser");
  for (const auto pc : plan.universe)
    expect(genesis_hybrid_admission_contains(artifact.ranges, pc) == admitted(plan, pc), name + ": artifact ranges admit exactly H ∩ U");
  const auto aggregate = format_genesis_hybrid_plan_aggregate(plan);
  expect(aggregate.find("\"pc\"") == std::string::npos && aggregate.find("0010") == std::string::npos, name + ": aggregate is sanitized");
}

void exact_target() {
  Image image;
  Asm{image, entry}.lea_abs(0, 0x300U).jmp_an(0);
  Asm{image, 0x300U}.bra_self();
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("exact", *program, plan);
  expect(plan.island_entries.empty() && plan.rounds == 1U, "exact: no island, one round");
  expect(admitted(plan, 0x300U) && plan.resolved_sites == 1U, "exact: the exact target is admitted");
}

// Slot k of the table at `base`: `code(k)` written at base + 16k.
template <typename Code>
void fill_slots(Image &image, std::uint32_t base, Code code) {
  for (std::uint32_t k = 0; k < slots; ++k) {
    Asm a{image, base + 16U * k};
    code(a, k);
  }
}

void bounded_region() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs).jmp_an(1);
  pointer_table(image, ptrs, table);
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.bra_w(entry); });
  for (std::uint32_t k = 0; k < slots; ++k) Asm{image, table + 16U * k + 8U}.nop();  // in U, between slots: never admitted
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("bounded", *program, plan);
  expect(plan.island_entries.size() == 1U && plan.island_entries.begin()->second.size() == slots, "bounded: one island of 128 entries");
  expect(plan.rounds == 2U, "bounded: the closure needs a second round");
  bool all = true;
  for (std::uint32_t k = 0; k < slots; ++k) all = all && admitted(plan, table + 16U * k) && !admitted(plan, table + 16U * k + 8U);
  expect(all, "bounded: every slot entry admitted, no interleaved code");
  const auto site = plan.sites.begin();
  expect(site != plan.sites.end() && site->second.container == GenesisHybridContainer::points_to_region, "bounded: points_to_region");
}

void called_island() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs).jsr_an(1).bra_self();
  pointer_table(image, ptrs, table);
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.nop().rts(); });  // callees return through the call's own slot
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("called", *program, plan);
  const auto last_rts = table + 16U * (slots - 1U) + 2U;
  expect(std::binary_search(plan.hybrid.begin(), plan.hybrid.end(), last_rts) && (admitted(plan, last_rts) || !in_universe(plan, last_rts)),
         "called: island callees and their returns are analysed (in H) and admitted when broad has them");
  expect(admitted(plan, entry + 20U), "called: the call's continuation (after the 18-byte prologue and the JSR) is admitted");
}

void two_images() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs_two, 2U * slots).jmp_an(1);
  pointer_table(image, ptrs_two, table);
  pointer_table(image, ptrs_two + 4U * slots, alias_base);
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.bra_self(); });
  fill_slots(image, alias_source, [](Asm &s, std::uint32_t) { s.bra_self(); });
  const auto program = program_of(image, true);
  const auto plan = plan_of(*program);
  common("two_images", *program, plan);
  const auto &entries = plan.sites.begin()->second.entries;
  expect(entries.size() == 2U * slots, "two_images: both images' members are island entries");
  expect(admitted(plan, table) && admitted(plan, alias_base + 16U * (slots - 1U)), "two_images: ROM and alias members admitted");
  expect(plan.sites.begin()->second.container == GenesisHybridContainer::materialized_image, "two_images: materialized_image container");
}

void direct_edge_leaves_island() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs).jmp_an(1);
  pointer_table(image, ptrs, table);
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.jmp_abs(0x2E00U); });
  Asm{image, 0x2E00U}.nop().nop().bra_self();
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("direct_edge", *program, plan);
  expect(admitted(plan, 0x2E00U) && admitted(plan, 0x2E02U) && admitted(plan, 0x2E04U), "direct_edge: code outside the island is closed over");
}

void second_island() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs).jmp_an(1);
  pointer_table(image, ptrs, table);
  pointer_table(image, ptrs_b, table_b);
  // Each first-island slot dispatches again through the same proven D0 (carried by the site's state) into a second table.
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.lea_abs(2, ptrs_b).movea_idx(3, 2, 0).jmp_an(3); });
  fill_slots(image, table_b, [](Asm &s, std::uint32_t) { s.bra_self(); });
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("second_island", *program, plan);
  expect(plan.rounds == 3U && plan.island_entries.size() == 1U + slots, "second_island: island code adds islands in a later round");
  expect(admitted(plan, table_b + 16U * (slots - 1U)), "second_island: second-island members admitted");
  GenesisHybridPlanConfig bounded{};
  bounded.max_rounds = 2U;
  const auto cut = plan_of(*program, bounded);
  expect(cut.outcome == GenesisHybridOutcome::broad_closure_bound && cut.admitted == cut.universe,
         "closure bound: exhaustion is whole broad, never a partial hybrid");
  GenesisHybridPlanConfig tiny{};
  tiny.max_island_entries = slots;  // the second island needs more
  const auto capped = plan_of(*program, tiny);
  expect(capped.outcome == GenesisHybridOutcome::broad_island_bound && capped.admitted == capped.universe,
         "island bound: exhaustion is whole broad");
}

void cyclic_growth() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs).jmp_an(1);
  pointer_table(image, ptrs, table);
  pointer_table(image, ptrs_b, table_b);
  // Island A slots dispatch into island B; island B slots dispatch back into island A: the islands only grow and converge.
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.lea_abs(2, ptrs_b).movea_idx(3, 2, 0).jmp_an(3); });
  fill_slots(image, table_b, [](Asm &s, std::uint32_t) { s.lea_abs(2, ptrs).movea_idx(3, 2, 0).jmp_an(3); });
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("cyclic", *program, plan);
  expect(plan.rounds <= genesis_hybrid_max_rounds, "cyclic: converges within the round bound");
}

void overlapping_islands() {
  Image image;
  Asm a{image, entry};
  // Two sites over overlapping proven sets: table + 16k and table + 0x80 + 16k (k < 128).
  strided_pointer(a, ptrs).tst_w_ram(0xF002U).bcc_s(EQ, 0x240U).jmp_an(1);
  Asm b{image, 0x240U};
  strided_pointer(b, ptrs_b).jmp_an(1);
  pointer_table(image, ptrs, table);
  pointer_table(image, ptrs_b, table + 0x80U);
  for (std::uint32_t k = 0; k < slots + 8U; ++k) Asm{image, table + 16U * k}.bra_self();
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("overlap", *program, plan);
  expect(plan.island_entries.size() == 2U, "overlap: two sites");
  std::set<std::uint32_t> distinct;
  for (const auto &[pc, entries] : plan.island_entries) {
    expect(std::is_sorted(entries.begin(), entries.end()) && std::adjacent_find(entries.begin(), entries.end()) == entries.end(),
           "overlap: canonical (sorted, distinct) entries");
    distinct.insert(entries.begin(), entries.end());
  }
  expect(distinct.size() == slots + 8U, "overlap: the union is canonical");
}

void ram_mirror() {
  Image image;
  Asm a{image, entry};
  // Pointers into the work-RAM mirror at $E00000 congruent to the alias: not executable addresses (dispatch is exact, so broad cannot
  // execute them either); the exact proven addresses are enumerated, none is mapped.
  strided_pointer(a, ptrs).jmp_an(1);
  pointer_table(image, ptrs, 0xE00000U + (alias_base & 0xFFFFU));
  fill_slots(image, alias_source, [](Asm &s, std::uint32_t) { s.bra_self(); });
  const auto program = program_of(image, true);
  const auto plan = plan_of(*program);
  common("mirror", *program, plan);
  const auto site = plan.sites.begin();
  expect(site != plan.sites.end() && site->second.entries.empty(), "mirror: no mapped member at the mirror addresses");
  for (std::uint32_t k = 0; k < slots; ++k)
    expect(!in_universe(plan, 0xE00000U + (alias_base & 0xFFFFU) + 16U * k), "mirror: broad has no identity there either");
  expect(admitted(plan, alias_base), "mirror: the alias image itself stays admitted (materialized root)");
}

void materialized_image() {
  Image image;
  Asm{image, entry}.bra_self();
  fill_slots(image, alias_source, [](Asm &s, std::uint32_t) { s.nop().bra_self(); });
  const auto program = program_of(image, true);
  const auto plan = plan_of(*program);
  common("materialized", *program, plan);
  bool all = true;
  for (const auto pc : plan.universe)
    if (pc >= alias_base && pc < alias_base + slots * 16U) all = all && admitted(plan, pc);
  expect(all && plan.materialized_entries > 0U, "materialized: every alias identity is admitted (mandatory root)");
}

void unknown_identity() {
  Image image;
  Asm{image, entry}.movea_l_ram(0, 0xF000U).jmp_an(0);
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  expect(plan.outcome == GenesisHybridOutcome::broad_whole_image && plan.admitted == plan.universe,
         "unknown identity: an Unknown pointer widens to whole broad");
  const auto artifact = genesis_hybrid_admission_plan(plan, *program, std::string(64U, 'a'));
  expect(artifact.strategy == GenesisAdmissionStrategy::broad && artifact.ranges.empty(), "unknown identity: the artifact is broad");
  expect(plan.sites.size() == 1U && plan.sites.begin()->second.container == GenesisHybridContainer::whole_image, "unknown identity: whole_image");
}

// SEG-041-T008: the same "Unknown pointer" shape as `unknown_identity()` above, now resolved by a structurally-re-verified
// external fact (modelling a qualified external backend's exact-target proof -- SEG-041-T004). `genesis_hybrid_container()`/
// `validate_genesis_hybrid_round()` must consult the SAME fact set identically; external analysis remains an additional input
// to the unchanged planner, never a second admission policy.
void external_fact_exact_resolves_unknown_site() {
  Image image;
  Asm{image, entry}.movea_l_ram(0, 0xF000U).jmp_an(0);  // the jmp (A0) site sits at entry+4
  Asm{image, 0x300U}.bra_self();
  const auto program = program_of(image);
  GenesisHybridPlanConfig config{};
  GenesisExternalM68kFacts facts;
  facts.rom_sha256 = std::string(64U, 'a');
  facts.producer = "test-producer-exact";
  facts.facts.push_back(GenesisExternalM68kFact{entry + 4U, {0x300U}, true});
  config.external_m68k_facts = facts;
  const auto plan = plan_of(*program, config);
  expect(plan.outcome == GenesisHybridOutcome::hybrid,
         "external exact: resolves to hybrid (got " + std::string(genesis_hybrid_outcome_name(plan.outcome)) + ")");
  expect(plan.external_facts_applied == 1U, "external exact: exactly one site used the external fact");
  expect(admitted(plan, 0x300U), "external exact: the externally-proven target is admitted");
  common("external exact", *program, plan, config);
  const auto found = plan.sites.find(entry + 4U);
  expect(found != plan.sites.end() && found->second.container == GenesisHybridContainer::exact && found->second.external,
         "external exact: the site is classified exact and marked external");
}

// A "contained" (non-exact) external fact uses the identical mechanism -- entries become island members subject to the SAME
// unmodified round-based closure, exactly like an internally-derived points-to region.
void external_fact_contained_resolves_unknown_site() {
  Image image;
  Asm{image, entry}.movea_l_ram(0, 0xF000U).jmp_an(0);
  Asm{image, 0x300U}.bra_self();
  const auto program = program_of(image);
  GenesisHybridPlanConfig config{};
  GenesisExternalM68kFacts facts;
  facts.rom_sha256 = std::string(64U, 'a');
  facts.producer = "test-producer-contained";
  facts.facts.push_back(GenesisExternalM68kFact{entry + 4U, {0x300U}, false});
  config.external_m68k_facts = facts;
  const auto plan = plan_of(*program, config);
  expect(plan.outcome == GenesisHybridOutcome::hybrid, "external contained: resolves to hybrid");
  const auto found = plan.sites.find(entry + 4U);
  expect(found != plan.sites.end() && found->second.container == GenesisHybridContainer::points_to_region && found->second.external,
         "external contained: the site is classified points_to_region (the unchanged island mechanism) and marked external");
}

// A fact citing even one structurally-unverifiable entry (odd target, or a target outside the mapped image) is discarded in
// its ENTIRETY -- never partially trusted -- and the site falls back to exactly the unmodified `unknown_identity()` outcome.
void external_fact_discarded_when_unverifiable() {
  Image image;
  Asm{image, entry}.movea_l_ram(0, 0xF000U).jmp_an(0);
  Asm{image, 0x300U}.bra_self();
  const auto program = program_of(image);
  for (const auto bad_target : {UINT32_C(0x301), UINT32_C(0x8000)}) {  // odd; and past the end of the mapped image (0x4000 bytes)
    GenesisHybridPlanConfig config{};
    GenesisExternalM68kFacts facts;
    facts.rom_sha256 = std::string(64U, 'a');
    facts.producer = "test-producer-bad";
    facts.facts.push_back(GenesisExternalM68kFact{entry + 4U, {0x300U, bad_target}, true});  // one good, one bad entry
    config.external_m68k_facts = facts;
    const auto plan = plan_of(*program, config);
    expect(plan.outcome == GenesisHybridOutcome::broad_whole_image && plan.admitted == plan.universe,
           "external discarded: an unverifiable fact (target " + std::to_string(bad_target) +
               ") never partially trusted -- falls back to broad");
    expect(plan.external_facts_applied == 0U, "external discarded: zero sites credited to the discarded fact");
  }
}

// An otherwise-well-formed fact whose entry count exceeds the configured island bound is discarded in
// its entirety, exactly like an internally-derived points-to set over the same bound (`count > max_entries`).
void external_fact_discarded_over_entry_bound() {
  Image image;
  Asm{image, entry}.movea_l_ram(0, 0xF000U).jmp_an(0);
  Asm{image, 0x300U}.bra_self();
  Asm{image, 0x400U}.bra_self();
  const auto program = program_of(image);
  GenesisHybridPlanConfig config{};
  config.max_island_entries = 1U;  // one legal entry alone would fit; two cannot
  GenesisExternalM68kFacts facts;
  facts.rom_sha256 = std::string(64U, 'a');
  facts.producer = "test-producer-bound";
  facts.facts.push_back(GenesisExternalM68kFact{entry + 4U, {0x300U, 0x400U}, true});
  config.external_m68k_facts = facts;
  const auto plan = plan_of(*program, config);
  expect(plan.outcome == GenesisHybridOutcome::broad_whole_image && plan.admitted == plan.universe,
         "external discarded (bound): a fact over the configured island-entry bound is never partially trusted");
  expect(plan.external_facts_applied == 0U, "external discarded (bound): zero sites credited");
}

// The validator must consult the SAME external facts the plan was built with; omitting them on a re-check correctly rejects a
// configuration the validator can no longer independently re-derive (the existing "freshly recompute and compare" property,
// now also covering the external input -- it is never blindly trusted just because `island_entries` already names it).
void external_fact_validator_requires_matching_facts() {
  Image image;
  Asm{image, entry}.movea_l_ram(0, 0xF000U).jmp_an(0);
  Asm{image, 0x300U}.bra_self();
  const auto program = program_of(image);
  GenesisHybridPlanConfig config{};
  GenesisExternalM68kFacts facts;
  facts.rom_sha256 = std::string(64U, 'a');
  facts.producer = "test-producer-validator";
  facts.facts.push_back(GenesisExternalM68kFact{entry + 4U, {0x300U}, true});
  config.external_m68k_facts = facts;
  const auto plan = plan_of(*program, config);
  expect(plan.outcome == GenesisHybridOutcome::hybrid, "external validator: setup plan is hybrid");
  GenesisAnalysisReportConfig analysis_config{};
  analysis_config.domains = GenesisAnalysisDomains{true, true, true, true};
  analysis_config.island_entries = plan.island_entries;
  const auto report = run_genesis_analysis_report(*program, analysis_config);
  const auto view = GenesisM68kAnalysisImage::create(*program);
  expect(!validate_genesis_hybrid_round(report, *view, plan.island_entries, plan.universe, genesis_hybrid_max_island_entries, facts),
         "external validator: validates when given the matching facts");
  expect(validate_genesis_hybrid_round(report, *view, plan.island_entries, plan.universe) ==
             std::optional<std::string>("unbounded_site"),
         "external validator: without the matching facts it can no longer re-derive the island, and fails closed");
}

// SEG-041-T008 text-format parser: valid round-trip plus every malformed-input class fails closed (identical in effect to no
// file at all -- never partially parsed/trusted).
void external_facts_parser() {
  const std::string rom = std::string(64U, 'b');
  const std::string valid = "segarecomp.m68k_external_facts.v1\nrom_sha256 " + rom +
                            "\nproducer segarecomp-angr-m68k-v1\nfact 00001004 exact 00000300\nfact 00002000 contained "
                            "00002100,00002200\nend\n";
  const auto parsed = parse_genesis_external_m68k_facts(valid, rom);
  expect(parsed.has_value(), "parser: a well-formed file parses");
  if (parsed) {
    expect(parsed->rom_sha256 == rom && parsed->producer == "segarecomp-angr-m68k-v1", "parser: rom_sha256/producer round-trip");
    expect(parsed->facts.size() == 2U, "parser: both facts parsed");
    expect(parsed->facts[0].pc == 0x1004U && parsed->facts[0].exact && parsed->facts[0].entries == std::vector<std::uint32_t>{0x300U},
           "parser: first fact (exact)");
    expect(parsed->facts[1].pc == 0x2000U && !parsed->facts[1].exact &&
               parsed->facts[1].entries == std::vector<std::uint32_t>{0x2100U, 0x2200U},
           "parser: second fact (contained, two entries)");
  }
  expect(!parse_genesis_external_m68k_facts(valid, std::string(64U, 'c')).has_value(), "parser: wrong rom_sha256 is rejected");
  expect(!parse_genesis_external_m68k_facts("wrong.schema.v1\nrom_sha256 " + rom + "\nproducer x\nend\n", rom).has_value(),
         "parser: wrong schema line is rejected");
  expect(!parse_genesis_external_m68k_facts("segarecomp.m68k_external_facts.v1\nrom_sha256 " + rom + "\nproducer x\n"
                                            "fact 00001004 exact 00000300\nfact 00001000 exact 00000400\nend\n",
                                            rom)
             .has_value(),
         "parser: non-ascending fact PCs are rejected");
  expect(!parse_genesis_external_m68k_facts("segarecomp.m68k_external_facts.v1\nrom_sha256 " + rom + "\nproducer x\n"
                                            "fact 00001004 exact 00000400,00000300\nend\n",
                                            rom)
             .has_value(),
         "parser: non-ascending entries within one fact are rejected");
  expect(!parse_genesis_external_m68k_facts("segarecomp.m68k_external_facts.v1\nrom_sha256 " + rom + "\nproducer x\n"
                                            "fact 00001004 exact 00000300\nfact 00001004 exact 00000400\nend\n",
                                            rom)
             .has_value(),
         "parser: a duplicate PC is rejected");
  expect(!parse_genesis_external_m68k_facts("segarecomp.m68k_external_facts.v1\nrom_sha256 " + rom + "\nproducer x\n"
                                            "fact 00001004 exact 00000300\n",
                                            rom)
             .has_value(),
         "parser: a missing 'end' trailer is rejected");
  expect(!parse_genesis_external_m68k_facts("segarecomp.m68k_external_facts.v1\nrom_sha256 " + rom + "\nproducer x\nend\ntrailing\n",
                                            rom)
             .has_value(),
         "parser: trailing content after 'end' is rejected");
  expect(!parse_genesis_external_m68k_facts(std::string(genesis_external_m68k_facts_max_bytes + 1U, 'x'), rom).has_value(),
         "parser: an oversized file is rejected");
}

void width_rule() {
  Image image;
  Asm a{image, entry};
  a.move_b_ram(0, 0xF000U);
  a.jmp_pcidx(0, a.pc + 2U);  // index bounded only by the operand width of a mutable byte: the forbidden SEG-024 rule
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  expect(plan.outcome == GenesisHybridOutcome::broad_whole_image, "width rule: an operand-width-only index never bounds an island");
}

void width_rule_address() {
  Image image;
  Asm a{image, entry};
  // A code pointer read through an index bounded only by the operand width of a mutable byte (D0 in [0, 255] times 4): the
  // pointer set is width-derived, so it never bounds an island (the forbidden SEG-024 rule), even though it is finite.
  a.w(0x7000U).move_b_ram(0, 0xF000U).w(0xD040U).w(0xD040U).lea_abs(0, ptrs).movea_idx(1, 0, 0).jmp_an(1);  // MOVEQ #0; ADD.W D0,D0 x2
  for (std::uint32_t k = 0; k < 256U; ++k) image.put32(ptrs + 4U * k, table + 16U * (k % slots));
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.bra_self(); });
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  expect(plan.outcome == GenesisHybridOutcome::broad_whole_image && plan.admitted == plan.universe,
         std::string("width rule (address): a width-derived pointer set never bounds an island: ") + genesis_hybrid_outcome_name(plan.outcome));
}

void incomplete_solve() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs).jmp_an(1);
  pointer_table(image, ptrs, table);
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.bra_self(); });
  const auto program = program_of(image);
  GenesisHybridPlanConfig config{};
  config.analysis.bounds.max_iterations = 2U;
  const auto plan = plan_of(*program, config);
  expect(plan.outcome == GenesisHybridOutcome::broad_analysis_incomplete && plan.admitted == plan.universe,
         "incomplete: an exhausted solve is whole broad, never credited");
}

void vector_root() {
  Image image;
  Asm{image, entry}.bra_self();
  image.put32(30U * 4U, 0x600U);  // IRQ6 handler (never returns, so no resumption is involved)
  Asm{image, 0x600U}.nop().bra_self();
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  common("vector_root", *program, plan);
  expect(admitted(plan, 0x600U) && admitted(plan, 0x602U), "vector root: the interrupt handler is admitted");
}

void invalid_provenance() {
  Image image;
  Asm{image, entry}.bra_self();
  auto program = program_of(image);
  // An alias descriptor that was never validated (it bypasses the fail-closed recorder): the image set does not validate.
  program->immutable_copy_aliases.push_back({alias_base, 0x3FF0U, 0x100U});
  const auto plan = plan_of(*program);
  expect(plan.outcome == GenesisHybridOutcome::broad_images_invalid && plan.admitted.empty(),
         "invalid provenance: an unverified image set never justifies an island");
}

void validator_rejects_missing_configuration() {
  Image image;
  Asm a{image, entry};
  strided_pointer(a, ptrs).jmp_an(1);
  pointer_table(image, ptrs, table);
  fill_slots(image, table, [](Asm &s, std::uint32_t) { s.bra_self(); });
  const auto program = program_of(image);
  const auto plan = plan_of(*program);
  GenesisAnalysisReportConfig config{};
  config.domains = GenesisAnalysisDomains{true, true, true, true};
  config.island_entries = plan.island_entries;
  const auto report = run_genesis_analysis_report(*program, config);
  const auto view = GenesisM68kAnalysisImage::create(*program);
  expect(!validate_genesis_hybrid_round(report, *view, plan.island_entries, plan.universe), "validator: the fixed point validates");
  auto missing = plan.island_entries;
  missing.begin()->second.pop_back();  // omit one member of the proven container
  expect(validate_genesis_hybrid_round(report, *view, missing, plan.universe) == std::optional<std::string>("site_container_not_configured"),
         "validator: an island missing one proven member is rejected");
  expect(validate_genesis_hybrid_round(report, *view, {}, plan.universe) == std::optional<std::string>("site_not_configured"),
         "validator: an unconfigured uncovered site is rejected");
  // A reached dynamic site the round neither reports uncovered nor resolved (a site missing from every site map: an inconsistent
  // report) is rejected, never passed silently.
  auto inconsistent = report;
  const auto site_pc = plan.island_entries.begin()->first;
  inconsistent.analysis.unresolved_computed.erase(site_pc);
  inconsistent.analysis.address_sites.erase(site_pc);
  inconsistent.analysis.pc_index_sites.erase(site_pc);
  inconsistent.analysis.return_sites.erase(site_pc);
  inconsistent.computed_sites.erase(site_pc);
  expect(validate_genesis_hybrid_round(inconsistent, *view, plan.island_entries, plan.universe) ==
             std::optional<std::string>("unclassified_dynamic_site"),
         "validator: an unexpected dynamic site is rejected");
}

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--fail-fast") {
      fail_fast = true;
    } else {
      std::cerr << "usage: analysis_hybrid_plan_test [--fail-fast]\n";
      return EXIT_FAILURE;
    }
  }
  exact_target();
  bounded_region();
  called_island();
  two_images();
  direct_edge_leaves_island();
  second_island();
  cyclic_growth();
  overlapping_islands();
  ram_mirror();
  materialized_image();
  unknown_identity();
  external_fact_exact_resolves_unknown_site();
  external_fact_contained_resolves_unknown_site();
  external_fact_discarded_when_unverifiable();
  external_fact_discarded_over_entry_bound();
  external_fact_validator_requires_matching_facts();
  external_facts_parser();
  width_rule();
  width_rule_address();
  incomplete_solve();
  vector_root();
  invalid_provenance();
  validator_rejects_missing_configuration();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_hybrid_plan_test: all checks passed\n";
  return EXIT_SUCCESS;
}
