// SEG-045 (ADR 0094): REPORT-ONLY structural pruning of a proposed executable region (platforms/genesis/machine hybrid_admission.hpp).
// Project-authored synthetic MC68000 images only. Properties: closed subsets survive; false-positive decoded data whose mandatory edge
// leaves K is pruned (transitively, cyclic components included); K ⊆ K0 ⊆ U; the greatest fixed point is order independent; rejection
// is fail-closed (roots, materialized identities, empty K, resource cap); the unchanged production validator accepts every plan.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/machine/genesis/hybrid_admission.hpp"

namespace {

using namespace segarecomp;
using Entries = std::vector<FrontendAnalysis::ImmutableRomAotEntry>;
using Ranges = std::vector<FrontendProgram::ImmutableRomAotRange>;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

const std::string sha(64U, 'a');
constexpr std::size_t image_size = 0x1000U;

struct Image {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(image_size, 0U);
  void words(std::uint32_t at, std::initializer_list<std::uint16_t> values) {
    for (const auto value : values) {
      bytes[at] = static_cast<std::uint8_t>(value >> 8U);
      bytes[at + 1U] = static_cast<std::uint8_t>(value);
      at += 2U;
    }
  }
};

// The shared "real" executable skeleton: reset PC $200 -> JSR $400 (continuation $206); IRQ6 handler $300; callee $400.
Image skeleton() {
  Image image;
  image.words(0x0U, {0x00FFU, 0xFE00U, 0x0000U, 0x0200U});
  image.words(0x78U, {0x0000U, 0x0300U});
  image.words(0x200U, {0x4EB9U, 0x0000U, 0x0400U});
  image.words(0x206U, {0x60FEU});
  image.words(0x300U, {0x4E71U, 0x60FEU});
  image.words(0x400U, {0x4E71U, 0x4E75U});
  return image;
}

struct Program {
  FrontendProgram program;
  Entries entries;
};

Program build(const Image &image, bool alias = false) {
  auto program = make_genesis_bridge_startup_program(image.bytes, 0U, 0x200U, std::nullopt);
  Program out{*program, {}};
  (void)apply_genesis_immutable_rom_aot(out.program);
  if (alias) (void)apply_genesis_immutable_copy_alias(out.program, 0xFF0000U, 0x600U, 4U);
  const auto analysis = analyze_m68k_frontend(out.program);
  if (const auto *accepted = std::get_if<FrontendAnalysis>(&analysis)) out.entries = accepted->immutable_rom_aot_entries;
  else if (const auto *partial = std::get_if<FrontendPartialProgram>(&analysis)) out.entries = partial->accepted_prefix.immutable_rom_aot_entries;
  return out;
}

std::uint32_t address_of(const FrontendAnalysis::ImmutableRomAotEntry &entry) {
  return static_cast<std::uint32_t>(entry.decoded.provenance.source.address.value) & 0xFFFFFFU;
}

std::vector<std::uint32_t> addresses(const Entries &entries) {
  std::vector<std::uint32_t> out;
  for (const auto &entry : entries) out.push_back(address_of(entry));
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool has(const std::vector<std::uint32_t> &set, std::uint32_t pc) { return std::binary_search(set.begin(), set.end(), pc); }

Ranges r(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> values) {
  Ranges out;
  for (const auto &[begin, end] : values) out.push_back({begin, end});
  return out;
}

// The region rule the production owner applies (SEG-050-T002 O1): an alias identity is in R by its RAM address or its byte-identity source.
bool in_region_of(const Entries &entries, const Ranges &regions, std::uint32_t pc) {
  if (genesis_hybrid_admission_contains(regions, pc)) return true;
  for (const auto &entry : entries)
    if (address_of(entry) == pc) return entry.execution_alias && !entry.materialized_ram_thunk && genesis_hybrid_admission_contains(regions, entry.alias_source_address & 0xFFFFFFU);
  return false;
}

// Independent reference: the naive "repeat until no change" greatest fixed point, written without the kernel's reverse-edge worklist.
std::vector<std::uint32_t> reference(const Entries &entries, const Ranges &regions) {
  const auto universe = addresses(entries);
  std::set<std::uint32_t> keep;
  for (const auto pc : universe)
    if (in_region_of(entries, regions, pc)) keep.insert(pc);
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto &entry : entries) {
      const auto pc = address_of(entry);
      if (!keep.contains(pc)) continue;
      const auto control = m68k_control_successors(entry.operation);
      bool ok = true;
      for (const auto &successor : control.successors) {
        const auto target = successor.target & 0xFFFFFFU;
        if (has(universe, target) && !keep.contains(target)) ok = false;
      }
      if (control.stacked == M68kStackedContinuationKind::call_continuation) {
        const auto target = control.stacked_address & 0xFFFFFFU;
        if (has(universe, target) && !keep.contains(target)) ok = false;
      }
      if (!ok) {
        keep.erase(pc);
        changed = true;
      }
    }
  }
  return {keep.begin(), keep.end()};
}

// Runs the kernel, checks the universal invariants, and returns the result.
GenesisRegionPruneResult run(const Program &p, const Ranges &regions, const std::string &label, std::size_t cap = genesis_region_prune_default_max_rounds) {
  const auto result = prune_genesis_region_admission(p.program, p.entries, regions, cap);
  const auto universe = addresses(p.entries);
  expect(result.universe_count == universe.size(), label + ": |U|");
  expect(std::is_sorted(result.admitted.begin(), result.admitted.end()), label + ": K sorted");
  for (const auto pc : result.admitted) {
    expect(has(universe, pc), label + ": K ⊆ U");
    expect(in_region_of(p.entries, regions, pc), label + ": K ⊆ K0 (no widening beyond R)");
  }
  expect(result.admitted.size() + result.pruned_count == result.k0_count || result.failure, label + ": |K0| = |K| + pruned");
  if (!result.failure) {
    expect(result.admitted == reference(p.entries, regions), label + ": equals the naive greatest fixed point");
    // The unchanged production validator accepts the emitted plan and keeps exactly K.
    GenesisRegionPruneResult again;
    const auto plan = plan_genesis_region_admission(p.program, sha, p.entries, GenesisExecutableRegionProposal{sha, regions}, again, cap);
    expect(plan.has_value() && again.admitted == result.admitted, label + ": plan produced");
    if (plan) {
      auto entries = p.entries;
      const auto failure = apply_genesis_hybrid_admission(p.program, sha, *plan, entries);
      expect(!failure, label + ": production validator accepts: " + failure.value_or(""));
      expect(addresses(entries) == result.admitted, label + ": validator keeps exactly K");
      expect(parse_genesis_hybrid_admission_plan(format_genesis_hybrid_admission_plan(*plan)).has_value(), label + ": plan round-trips");
    }
  } else {
    expect(result.admitted.empty(), label + ": a rejected proposal exposes no K");
  }
  return result;
}

void closed_subset_and_false_positives() {
  auto image = skeleton();
  // 0x420: BRA.S -> 0x430 -> BRA.S -> 0x440 -> BRA.W $0842 (outside R). A decoded-data chain whose tail escapes.
  image.words(0x420U, {0x600EU});
  image.words(0x430U, {0x600EU});
  image.words(0x440U, {0x6000U, 0x0400U});
  // Closed cyclic component: 0x4A0 -> 0x4A4 -> 0x4A0.
  image.words(0x4A0U, {0x6002U});
  image.words(0x4A4U, {0x60FAU});
  // Escaping cyclic component: 0x4C0 -> 0x4C4; 0x4C4: BEQ.S -> 0x4C0 (fallthrough 0x4C6); 0x4C6: BRA.W out of R.
  image.words(0x4C0U, {0x6002U});
  image.words(0x4C4U, {0x67FAU});
  image.words(0x4C6U, {0x6000U, 0x0500U});
  // Ordinary arithmetic false positive: ADDQ.W #1,D0.
  image.words(0x4D0U, {0x5240U, 0x4E75U});  // ADDQ.W #1,D0; RTS (sequential flow is itself an obligation)
  // Odd branch target (not an aligned identity): no obligation.
  image.words(0x4E0U, {0x6001U});
  const auto p = build(image);
  const auto universe = addresses(p.entries);
  for (const auto pc : {0x200U, 0x206U, 0x300U, 0x302U, 0x400U, 0x402U, 0x420U, 0x430U, 0x440U, 0x4A0U, 0x4A4U, 0x4C0U, 0x4C4U, 0x4C6U, 0x4D0U, 0x4E0U})
    expect(has(universe, pc), "fixture: broad identity " + std::to_string(pc));

  const auto result = run(p, r({{0x200U, 0x500U}}), "closed-subset");
  expect(!result.failure, "closed-subset: accepted");
  for (const auto pc : {0x200U, 0x206U, 0x300U, 0x302U, 0x400U, 0x402U}) expect(has(result.admitted, pc), "real code survives unchanged");
  expect(has(result.admitted, 0x4D0U), "ordinary arithmetic false positive survives harmlessly");
  expect(has(result.admitted, 0x4E0U), "an odd-target branch has no obligation (retains broad behaviour)");
  expect(!has(result.admitted, 0x4E3U), "an odd address is never an identity");
  expect(!has(result.admitted, 0x4FEU), "zero padding whose sequential flow runs off R is pruned");
  for (const auto pc : {0x440U, 0x430U, 0x420U}) expect(!has(result.admitted, pc), "fake branch outside R and its predecessors are pruned transitively");
  expect(has(result.admitted, 0x4A0U) && has(result.admitted, 0x4A4U), "a closed cyclic component remains");
  for (const auto pc : {0x4C0U, 0x4C4U, 0x4C6U}) expect(!has(result.admitted, pc), "an escaping cyclic component is eliminated");
  expect(result.rounds >= 3U, "removal waves chain through the transitive predecessors");

  // Exact-universe property: C ⊆ K0 and C closed => C ⊆ K.
  const std::vector<std::uint32_t> c{0x200U, 0x206U, 0x300U, 0x302U, 0x400U, 0x402U};
  Entries only_c;
  for (const auto &entry : p.entries)
    if (has(c, address_of(entry))) only_c.push_back(entry);
  GenesisHybridAdmissionPlan exact;
  exact.rom_sha256 = sha;
  exact.universe_sha256 = genesis_hybrid_admission_universe_digest(universe);
  exact.strategy = GenesisAdmissionStrategy::hybrid;
  exact.ranges = genesis_hybrid_admission_ranges(universe, c);
  auto scratch = p.entries;
  expect(!apply_genesis_hybrid_admission(p.program, sha, exact, scratch), "exact C is accepted by the production validator");
  for (const auto pc : c) expect(has(result.admitted, pc), "C ⊆ K");

  // The whole image as R prunes nothing that is closed: K = the naive fixed point and the validator accepts.
  run(p, r({{0x0U, image_size}}), "whole-image");
  // A region that is not a superset of C may legitimately drop real code; the call target is then required.
  const auto missing_callee = run(p, r({{0x200U, 0x400U}}), "callee-outside");
  expect(missing_callee.failure == std::optional<std::string>("machine_root_not_admitted"), "a call target outside R prunes the caller; the reset root is lost");
  const auto no_reset = run(p, r({{0x300U, 0x500U}}), "reset-outside");
  expect(no_reset.failure == std::optional<std::string>("machine_root_not_admitted"), "region without the reset root rejects");
  const auto empty = run(p, r({{0x440U, 0x442U}}), "empty-after-prune");
  expect(empty.failure == std::optional<std::string>("empty_admission") && empty.k0_count == 1U, "a K0 pruned to nothing fails closed");
  const auto outside = run(p, r({{0x800U, 0x900U}}), "no-identity");
  expect(outside.failure == std::optional<std::string>("empty_admission"), "a region with no broad identity fails closed");
  const auto malformed = prune_genesis_region_admission(p.program, p.entries, r({{0x201U, 0x300U}}), 8U);
  expect(malformed.failure == std::optional<std::string>("malformed_region"), "odd region rejected");

  // Resource cap: the three-link chain needs >= 3 waves; cap exhaustion is a rejection, never a broadening.
  const auto capped = run(p, r({{0x200U, 0x500U}}), "cap", 2U);
  expect(capped.failure == std::optional<std::string>("region_prune_cap_exhausted") && capped.admitted.empty(), "cap exhaustion rejects");
}

void call_continuation() {
  auto image = skeleton();
  image.words(0x460U, {0x4EB9U, 0x0000U, 0x0300U, 0x4E75U});  // JSR $300 (in R); continuation $466 (RTS) is outside R in the first case
  const auto p = build(image);
  expect(has(addresses(p.entries), 0x466U), "fixture: continuation identity");
  const auto result = run(p, r({{0x200U, 0x466U}}), "call-continuation");
  expect(!result.failure && !has(result.admitted, 0x460U), "a call whose continuation leaves K is pruned although its target is inside");
  const auto widened = run(p, r({{0x200U, 0x468U}}), "call-continuation-in");
  expect(has(widened.admitted, 0x460U), "the same call survives when the continuation is inside");
}

void materialized_identity() {
  auto image = skeleton();
  image.words(0x600U, {0x4E71U, 0x4E75U});
  const auto p = build(image, true);
  const auto universe = addresses(p.entries);
  const bool alias_present = std::any_of(universe.begin(), universe.end(), [](std::uint32_t pc) { return pc >= 0xFF0000U; });
  expect(alias_present, "fixture: materialized identities exist");
  const auto rom_only = run(p, r({{0x200U, 0x500U}}), "alias-absent");
  expect(rom_only.failure == std::optional<std::string>("materialized_image_not_admitted"), "mandatory materialized identity outside R rejects");
  const auto with = run(p, r({{0x200U, 0x500U}, {0xFF0000U, 0xFF0004U}}), "alias-present");
  expect(!with.failure, "materialized identities inside R are retained and the validator accepts");
}

void order_independence_and_properties() {
  auto image = skeleton();
  image.words(0x420U, {0x600EU});
  image.words(0x430U, {0x600EU});
  image.words(0x440U, {0x6000U, 0x0400U});
  image.words(0x4C0U, {0x6002U});
  image.words(0x4C4U, {0x67FAU});
  image.words(0x4C6U, {0x6000U, 0x0500U});
  auto p = build(image);
  const auto regions = r({{0x200U, 0x500U}});
  const auto base = run(p, regions, "order-base");
  auto reversed = p;
  std::reverse(reversed.entries.begin(), reversed.entries.end());
  expect(prune_genesis_region_admission(p.program, reversed.entries, regions).admitted == base.admitted, "reversed identity order => same K");
  auto rotated = p;
  std::rotate(rotated.entries.begin(), rotated.entries.begin() + static_cast<std::ptrdiff_t>(rotated.entries.size() / 3U), rotated.entries.end());
  expect(prune_genesis_region_admission(p.program, rotated.entries, regions).admitted == base.admitted, "rotated identity order => same K");
  // Monotonicity: a larger R never shrinks the surviving *closed* real code (K(R) ⊆ K(R') is not guaranteed for fake data, but C is).
  run(p, r({{0x0U, 0xC00U}}), "large-region");
  // Randomised images: kernel == naive greatest fixed point; validator accepts; K ⊆ K0.
  std::uint32_t state = 0x12345678U;
  const auto next = [&]() {
    state = state * 1664525U + 1013904223U;
    return state >> 8U;
  };
  const std::uint16_t palette[] = {0x6002U, 0x6004U, 0x60FAU, 0x60FEU, 0x6702U, 0x66F8U, 0x4E71U, 0x4E75U, 0x0000U, 0x5240U, 0x6100U, 0x4EB9U, 0x6000U};
  for (int round = 0; round < 24; ++round) {
    auto random = skeleton();
    for (std::uint32_t at = 0x208U; at + 6U < 0x700U; at += 2U) {
      if (at >= 0x300U && at < 0x304U) continue;
      if (at >= 0x400U && at < 0x404U) continue;
      const auto pick = next();
      if (pick % 3U == 0U) random.words(at, {palette[pick % 13U]});
      if (palette[pick % 13U] == 0x6000U || palette[pick % 13U] == 0x6100U || palette[pick % 13U] == 0x4EB9U) random.words(at + 2U, {static_cast<std::uint16_t>((next() % 0x700U) & ~1U)});
    }
    const auto rp = build(random);
    const auto begin = 0x200U;
    const auto end = 0x300U + ((next() % 0x20U) * 0x20U);
    run(rp, r({{begin, end}}), "random-" + std::to_string(round));
  }
}

void parser() {
  const std::string good = "segarecomp.m68k_executable_regions.v1\nrom_sha256 " + sha + "\nrange 00000200 00000300\nrange 00000400 00000500\nend\n";
  const auto parsed = parse_genesis_executable_regions(good);
  expect(parsed && parsed->ranges.size() == 2U && format_genesis_executable_regions(*parsed) == good, "regions: round trip");
  const auto bad = [&](const std::string &text, const std::string &reason) {
    std::string error;
    expect(!parse_genesis_executable_regions(text, &error) && error == reason, "regions reject (" + reason + "): got " + error);
  };
  const std::string head = "segarecomp.m68k_executable_regions.v1\nrom_sha256 " + sha + "\n";
  bad("segarecomp.m68k_executable_regions.v0\nrom_sha256 " + sha + "\nrange 00000200 00000300\nend\n", "regions_schema");
  bad(head + "end\n", "regions_schema");
  bad(head + "range 00000201 00000300\nend\n", "regions_range");
  bad(head + "range 00000300 00000300\nend\n", "regions_range");
  bad(head + "range 00000200 00000300\nrange 00000300 00000400\nend\n", "regions_range_order");
  bad(head + "range 00000200 00000300\nend", "regions_unterminated_line");
  bad(head + "range 00000200 00000300\nextra\n", "regions_trailer");
  bad(head + "range 0000020A 00000300\nend\n", "regions_range");
}


// ---------------------------------------------------------------------------------------------------------------------------------
// SEG-050-T002 (ADR 0111, option O1): an ADR 0049 copy-alias identity belongs to R when its byte-identity SOURCE is in R (or, as before, when its
// RAM execution address is). Adversarial properties: no range is widened; a RAM thunk (no cartridge source), a partially covered copy, an alias whose
// mandatory edge leaves K and every unproven shape stay fail-closed; the UNCHANGED production validator accepts every K; no alias => identical.
struct AliasSpec { std::uint32_t execution, source, length; };

Program build_aliases(const Image &image, std::initializer_list<AliasSpec> aliases, bool thunk = false) {
  auto program = make_genesis_bridge_startup_program(image.bytes, 0U, 0x200U, std::nullopt);
  Program out{*program, {}};
  (void)apply_genesis_immutable_rom_aot(out.program);
  for (const auto &a : aliases) expect(apply_genesis_immutable_copy_alias(out.program, a.execution, a.source, a.length), "fixture: alias descriptor accepted");
  if (thunk) {
    const std::uint8_t jmp[] = {0x4EU, 0xF9U, 0x00U, 0x00U, 0x03U, 0x00U};  // JMP $000300.L
    expect(apply_genesis_materialized_ram_thunk(out.program, 0xFF0100U, jmp), "fixture: RAM thunk accepted");
  }
  const auto analysis = analyze_m68k_frontend(out.program);
  if (const auto *accepted = std::get_if<FrontendAnalysis>(&analysis)) out.entries = accepted->immutable_rom_aot_entries;
  else if (const auto *partial = std::get_if<FrontendPartialProgram>(&analysis)) out.entries = partial->accepted_prefix.immutable_rom_aot_entries;
  return out;
}

struct Outcome { bool admitted = false; std::string failure, failure_class, plan_text; std::vector<std::uint32_t> k; };

Outcome plan_of(const Program &p, const Ranges &regions, const std::string &label) {
  GenesisRegionPruneResult result;
  const auto plan = plan_genesis_region_admission(p.program, sha, p.entries, GenesisExecutableRegionProposal{sha, regions}, result);
  Outcome out;
  out.failure = result.failure.value_or("");
  out.failure_class = result.failure_class;
  if (!plan) {
    expect(result.admitted.empty(), label + ": a rejected proposal exposes no K");
    return out;
  }
  auto entries = p.entries;
  const auto verdict = apply_genesis_hybrid_admission(p.program, sha, *plan, entries);
  expect(!verdict, label + ": the unchanged production validator accepts: " + verdict.value_or(""));
  expect(addresses(entries) == result.admitted, label + ": the validator keeps exactly K");
  out.admitted = true;
  out.k = result.admitted;
  out.plan_text = format_genesis_hybrid_admission_plan(*plan);
  return out;
}

void alias_source_mapping() {
  auto image = skeleton();
  image.words(0x600U, {0x4E71U, 0x4E75U, 0x4E71U, 0x4E75U});        // NOP; RTS; NOP; RTS: closed, verbatim-copyable routines
  image.words(0x700U, {0x4E71U, 0x4EF9U, 0x0000U, 0x0420U});        // NOP; JMP $000420.L (an absolute target stays absolute in a copy)
  image.words(0x420U, {0x4E71U, 0x4E75U});                          // the jump target: a ROM identity
  const Ranges rom_code = r({{0x200U, 0x500U}});
  const Ranges with_source = r({{0x200U, 0x500U}, {0x600U, 0x604U}});

  // (1) a correct proven copy: admitted exactly when its source is selected; nothing is widened.
  const auto p = build_aliases(image, {{0xFF0000U, 0x600U, 4U}});
  expect(has(addresses(p.entries), 0xFF0000U) && has(addresses(p.entries), 0xFF0002U), "fixture: both alias identities exist");
  const auto missing = plan_of(p, rom_code, "source outside R");
  expect(!missing.admitted && missing.failure == "materialized_image_not_admitted" && missing.failure_class == "outside_region",
         "a copy whose source the proposal does not contain is rejected (no widening, ADR 0104 class outside_region)");
  const auto mapped = plan_of(p, with_source, "source inside R");
  expect(mapped.admitted && has(mapped.k, 0xFF0000U) && has(mapped.k, 0xFF0002U), "a copy whose source is selected is admitted through its byte-identity source");
  expect(has(mapped.k, 0x600U) && has(mapped.k, 0x602U), "the ROM identities of the selected source stay admitted (they are in R)");
  const auto direct = plan_of(p, r({{0x200U, 0x500U}, {0xFF0000U, 0xFF0004U}}), "RAM range");
  expect(direct.admitted, "an explicitly proposed RAM range keeps its previous meaning");
  const auto wrong = plan_of(p, r({{0x200U, 0x500U}, {0x604U, 0x608U}}), "unrelated range");
  expect(!wrong.admitted && wrong.failure_class == "outside_region", "an unrelated selected span does not admit the copy (the exact source decides)");

  // (2) an unproven RAM target: a RAM thunk has no cartridge source; its source-free alias key never maps into R.
  const auto t = build_aliases(image, {{0xFF0000U, 0x600U, 4U}}, true);
  expect(has(addresses(t.entries), 0xFF0100U), "fixture: thunk identity exists");
  const auto thunk = plan_of(t, with_source, "thunk");
  expect(!thunk.admitted && thunk.failure == "materialized_image_not_admitted" && thunk.failure_class == "outside_region", "an unproven RAM thunk stays fail-closed");
  const auto thunk_ok = plan_of(t, r({{0x200U, 0x500U}, {0x600U, 0x604U}, {0xFF0100U, 0xFF0106U}}), "thunk explicit");
  expect(thunk_ok.admitted && has(thunk_ok.k, 0xFF0100U), "an explicitly proposed RAM range for the thunk keeps its previous meaning");

  // (3) a wrong source span or overlapping descriptors fail closed at the descriptor validator; a partially covered copy is rejected.
  {
    auto program = make_genesis_bridge_startup_program(image.bytes, 0U, 0x200U, std::nullopt);
    (void)apply_genesis_immutable_rom_aot(*program);
    expect(!apply_genesis_immutable_copy_alias(*program, 0xFF0000U, 0x0FFEU, 8U), "a source span leaving the image is rejected by the descriptor validator");
    expect(apply_genesis_immutable_copy_alias(*program, 0xFF0000U, 0x600U, 4U), "first descriptor accepted");
    expect(!apply_genesis_immutable_copy_alias(*program, 0xFF0002U, 0x600U, 4U), "overlapping execution spans are rejected");
  }
  const auto wide = build_aliases(image, {{0xFF0000U, 0x600U, 8U}});
  const auto partial = plan_of(wide, r({{0x200U, 0x500U}, {0x600U, 0x604U}}), "partial");
  expect(!partial.admitted && partial.failure == "materialized_image_not_admitted" && partial.failure_class == "outside_region",
         "a copy only partly inside R is rejected (every alias identity is a mandatory root; the span is never widened)");
  const auto complete = plan_of(wide, r({{0x200U, 0x500U}, {0x600U, 0x608U}}), "complete");
  expect(complete.admitted && has(complete.k, 0xFF0006U), "the same copy with its whole source selected is admitted");

  // (4) an alias whose mandatory edge leaves K is eliminated and the proposal is rejected: fail closed, classified pruned, no reachability repair.
  const auto escaping = build_aliases(image, {{0xFF0200U, 0x700U, 8U}});
  const auto esc = plan_of(escaping, r({{0x200U, 0x410U}, {0x700U, 0x708U}}), "escaping");
  expect(!esc.admitted && esc.failure == "materialized_image_not_admitted" && esc.failure_class == "pruned",
         "an alias whose fixed edge escapes K is pruned: classified pruned, not outside_region");
  const auto closed = plan_of(escaping, r({{0x200U, 0x500U}, {0x700U, 0x708U}}), "closed");
  expect(closed.admitted && has(closed.k, 0xFF0200U), "the same alias is admitted once its absolute target is also selected");

  // (5) no aliases: the result is the naive greatest fixed point of the plain region rule and the plan is deterministic.
  const auto plain = build(skeleton());
  const auto base = run(plain, rom_code, "no-alias");
  const auto again = plan_of(plain, rom_code, "no-alias repeat");
  expect(!base.failure && again.admitted && again.k == base.admitted, "without aliases the result is the plain region rule");

  // (6) determinism and a later-round alias: identical input gives identical K and plan text; the plan of the alias-less emission is refused for the
  // alias emission by the unchanged validator (the alias set must match).
  const auto repeat = plan_of(p, with_source, "repeat");
  expect(repeat.admitted && repeat.k == mapped.k && repeat.plan_text == mapped.plan_text, "repeated runs are identical");
  GenesisRegionPruneResult first;
  const auto first_plan = plan_genesis_region_admission(plain.program, sha, plain.entries, GenesisExecutableRegionProposal{sha, rom_code}, first);
  auto stale_entries = p.entries;
  expect(first_plan && apply_genesis_hybrid_admission(p.program, sha, *first_plan, stale_entries) == std::optional<std::string>("alias_set_mismatch"),
         "a plan from the alias-less emission is refused for the alias emission");
}

}  // namespace

int main() {
  parser();
  closed_subset_and_false_positives();
  call_continuation();
  materialized_identity();
  alias_source_mapping();
  order_independence_and_properties();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "genesis_region_prune_test: all checks passed\n";
  return EXIT_SUCCESS;
}
