// SEG-031 (ADR 0080): the production side of the explicit hybrid admission candidate (platforms/genesis/machine
// hybrid_admission.hpp): the strict plan parser, the exact range builder and the fail-closed validate-and-filter seam. Production
// code only (no analysis linkage); project-authored synthetic MC68000 bytes.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "segarecomp/machine/genesis/hybrid_admission.hpp"

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
  words(0x0U, {0x00FFU, 0xFE00U, 0x0000U, 0x0200U});  // SSP, reset PC
  words(0x78U, {0x0000U, 0x0300U});                  // IRQ6 -> $300
  words(0x200U, {0x4EB9U, 0x0000U, 0x0400U});        // JSR $400 (continuation $206)
  words(0x206U, {0x60FEU});                          // BRA.S *
  words(0x300U, {0x4E71U, 0x60FEU});                 // handler: NOP; BRA.S *
  words(0x400U, {0x4E71U, 0x4E75U});                 // callee: NOP; RTS
  words(0x600U, {0x4E71U, 0x4E71U});                 // unreachable
  return bytes;
}

struct Program {
  FrontendProgram program;
  std::vector<FrontendAnalysis::ImmutableRomAotEntry> entries;
};

Program program_with(bool alias) {
  const auto bytes = image_bytes();
  auto program = make_genesis_bridge_startup_program(bytes, 0U, 0x200U, std::nullopt);
  Program out{*program, {}};
  (void)apply_genesis_immutable_rom_aot(out.program);
  if (alias) (void)apply_genesis_immutable_copy_alias(out.program, 0xFF0000U, 0x600U, 4U);
  const auto analysis = analyze_m68k_frontend(out.program);
  if (const auto *accepted = std::get_if<FrontendAnalysis>(&analysis)) out.entries = accepted->immutable_rom_aot_entries;
  else if (const auto *partial = std::get_if<FrontendPartialProgram>(&analysis)) out.entries = partial->accepted_prefix.immutable_rom_aot_entries;
  return out;
}

std::vector<std::uint32_t> addresses(const std::vector<FrontendAnalysis::ImmutableRomAotEntry> &entries) {
  std::vector<std::uint32_t> out;
  for (const auto &entry : entries) out.push_back(static_cast<std::uint32_t>(entry.decoded.provenance.source.address.value) & 0xFFFFFFU);
  std::sort(out.begin(), out.end());
  return out;
}

GenesisHybridAdmissionPlan plan_of(const Program &program, const std::vector<std::uint32_t> &admitted) {
  GenesisHybridAdmissionPlan plan;
  plan.rom_sha256 = sha;
  plan.universe_sha256 = genesis_hybrid_admission_universe_digest(addresses(program.entries));
  plan.aliases = program.program.immutable_copy_aliases;
  plan.strategy = GenesisAdmissionStrategy::hybrid;
  plan.ranges = genesis_hybrid_admission_ranges(addresses(program.entries), admitted);
  return plan;
}

void parser() {
  const std::string good = "segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 " + sha + "\nuniverse_sha256 " + sha +
                           "\nalias 00ff0000:00000600:00000004\nstrategy hybrid\nrange 00000200 00000206\nrange 00000300 00000304\nend\n";
  const auto parsed = parse_genesis_hybrid_admission_plan(good);
  expect(parsed && parsed->ranges.size() == 2U && parsed->aliases.size() == 1U && format_genesis_hybrid_admission_plan(*parsed) == good,
         "parser: a well-formed plan round-trips byte-identically");
  const auto bad = [&](const std::string &text, const std::string &reason) {
    std::string error;
    expect(!parse_genesis_hybrid_admission_plan(text, &error) && error == reason, "parser rejects (" + reason + "): got " + error);
  };
  const std::string head = "segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 " + sha + "\nuniverse_sha256 " + sha + "\n";
  bad("segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 " + sha + "\nstrategy broad\nend\n", "plan_universe_sha256");
  bad("segarecomp.m68k_hybrid_admission_plan.v0\n", "plan_schema");
  bad("segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 " + sha.substr(1) + "\n", "plan_rom_sha256");
  bad(head + "strategy maybe\nend\n", "plan_strategy");
  bad(head + "strategy hybrid\nend\n", "plan_empty");
  bad(head + "strategy broad\nrange 00000200 00000202\nend\n", "plan_range_in_broad");
  bad(head + "strategy hybrid\nrange 00000201 00000204\nend\n", "plan_range");
  bad(head + "strategy hybrid\nrange 00000204 00000204\nend\n", "plan_range");
  bad(head + "strategy hybrid\nrange 00000200 00000204\nrange 00000202 00000208\nend\n", "plan_range_order");
  bad(head + "strategy hybrid\nrange 00000200 00000204\nrange 00000204 00000208\nend\n", "plan_range_order");
  bad(head + "strategy hybrid\nrange 00000200 0000020A\nend\n", "plan_range");  // uppercase hex is not canonical
  bad(head + "alias 00ff0100:00000600:00000004\nalias 00ff0000:00000600:00000004\nstrategy broad\nend\n", "plan_alias_order");
  bad(head + "strategy broad\nend", "plan_unterminated_line");
  bad(head + "strategy broad\nend\nextra\n", "plan_trailer");
  bad(std::string(genesis_hybrid_admission_plan_max_bytes + 1U, 'x'), "plan_too_large");
}

void ranges() {
  const std::vector<std::uint32_t> universe{0x200U, 0x202U, 0x204U, 0x208U, 0x300U};
  const auto built = genesis_hybrid_admission_ranges(universe, {0x200U, 0x204U, 0x208U, 0x300U});
  expect(built.size() == 2U && built[0].begin_address == 0x200U && built[0].end_address == 0x202U && built[1].begin_address == 0x204U &&
             built[1].end_address == 0x302U,
         "ranges: maximal runs over U admit exactly the admitted identities");
  for (const auto pc : universe)
    expect(genesis_hybrid_admission_contains(built, pc) == (pc != 0x202U), "ranges: membership is exact");
}

void apply() {
  auto base = program_with(false);
  const auto universe = addresses(base.entries);
  const std::vector<std::uint32_t> needed{0x200U, 0x206U, 0x300U, 0x302U, 0x400U, 0x402U};
  for (const auto pc : needed) expect(std::binary_search(universe.begin(), universe.end(), pc), "fixture: broad identity " + std::to_string(pc));
  {
    auto entries = base.entries;
    expect(!apply_genesis_hybrid_admission(base.program, sha, plan_of(base, needed), entries), "apply: a closed plan is accepted");
    expect(addresses(entries) == needed, "apply: exactly the admitted identities remain");
  }
  const auto rejects = [&](const Program &program, const GenesisHybridAdmissionPlan &plan, const std::string &digest, const std::string &reason) {
    auto entries = program.entries;
    const auto failure = apply_genesis_hybrid_admission(program.program, digest, plan, entries);
    expect(failure == std::optional<std::string>(reason), "apply rejects (" + reason + "): got " + failure.value_or("accepted"));
    expect(entries.size() == program.entries.size(), "apply: a rejected plan filters nothing (" + reason + ")");
  };
  rejects(base, plan_of(base, needed), std::string(64U, 'b'), "rom_sha256_mismatch");
  auto stale = plan_of(base, needed);
  stale.universe_sha256 = std::string(64U, 'c');
  rejects(base, stale, sha, "universe_mismatch");  // a plan computed for another broad universe (frontend, entry mode, image set)
  auto without = [&](std::uint32_t pc) {
    auto admitted = needed;
    admitted.erase(std::remove(admitted.begin(), admitted.end(), pc), admitted.end());
    return admitted;
  };
  rejects(base, plan_of(base, without(0x206U)), sha, "call_continuation_not_admitted");
  rejects(base, plan_of(base, without(0x400U)), sha, "fixed_successor_not_admitted");
  rejects(base, plan_of(base, without(0x302U)), sha, "fixed_successor_not_admitted");
  rejects(base, plan_of(base, {0x206U, 0x300U, 0x302U, 0x400U, 0x402U}), sha, "machine_root_not_admitted");    // the reset entry
  rejects(base, plan_of(base, {0x200U, 0x206U, 0x400U, 0x402U}), sha, "machine_root_not_admitted");             // the IRQ6 handler
  auto empty = plan_of(base, needed);
  empty.ranges.clear();
  rejects(base, empty, sha, "empty_admission");
  auto overlapping = plan_of(base, needed);
  overlapping.ranges.push_back({0x200U, 0x204U});
  rejects(base, overlapping, sha, "malformed_range");
  {
    GenesisHybridAdmissionPlan broad;
    broad.rom_sha256 = sha;
    broad.universe_sha256 = genesis_hybrid_admission_universe_digest(addresses(base.entries));
    auto entries = base.entries;
    expect(!apply_genesis_hybrid_admission(base.program, sha, broad, entries) && entries.size() == base.entries.size(),
           "apply: a broad plan filters nothing");
  }
  auto aliased = program_with(true);
  rejects(aliased, plan_of(base, needed), sha, "alias_set_mismatch");
  rejects(aliased, plan_of(aliased, needed), sha, "materialized_image_not_admitted");
  auto with_alias = needed;
  for (const auto pc : addresses(aliased.entries))
    if (pc >= 0xFF0000U) with_alias.push_back(pc);
  std::sort(with_alias.begin(), with_alias.end());
  auto entries = aliased.entries;
  expect(!apply_genesis_hybrid_admission(aliased.program, sha, plan_of(aliased, with_alias), entries), "apply: materialized identities admitted");
  FrontendProgram no_aot = base.program;
  no_aot.immutable_rom_aot_enabled = false;
  rejects(Program{no_aot, base.entries}, plan_of(base, needed), sha, "broad_aot_not_enabled");
}

}  // namespace

int main() {
  parser();
  ranges();
  apply();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "genesis_hybrid_admission_test: all checks passed\n";
  return EXIT_SUCCESS;
}
