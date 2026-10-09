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

// Independent reference: the naive "repeat until no change" greatest fixed point, written without the kernel's reverse-edge worklist.
std::vector<std::uint32_t> reference(const Entries &entries, const Ranges &regions) {
  const auto universe = addresses(entries);
  std::set<std::uint32_t> keep;
  for (const auto pc : universe)
    if (genesis_hybrid_admission_contains(regions, pc)) keep.insert(pc);
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
    expect(genesis_hybrid_admission_contains(regions, pc), label + ": K ⊆ K0 (no widening beyond R)");
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

}  // namespace

int main() {
  parser();
  closed_subset_and_false_positives();
  call_continuation();
  materialized_identity();
  order_independence_and_properties();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "genesis_region_prune_test: all checks passed\n";
  return EXIT_SUCCESS;
}
