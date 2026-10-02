// SEG-029-T003 (ADR 0078): equivalence of the forward M68K finite-value adapter (generic solver) with the SEG-026-T002
// demand-driven backward PC-index recovery of the reachability challenger, over the same project-authored synthetic
// fixtures as reachability_pc_index_recovery_test (tiny assembler helpers copied; real MC68000 encodings decoded by
// the unchanged decoder/lifter, so exact synthetic addresses may be asserted). No commercial input.
//
// Per PC-indexed site both analyses must agree on the outcome class (resolved with the identical target set, or
// unresolved) and on the recovered target set. Genuine, sound differences are asserted explicitly and documented.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"
#include "segarecomp/machine/genesis/reachability_challenger.hpp"

namespace {

using namespace segarecomp;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr unsigned HI = 2U, LS = 3U, CS = 5U, EQ = 7U;  // Bcc condition fields

struct Image {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x1000U, 0U);
  Image() {
    put32(0x0U, 0x00FFFE00U);
    put32(0x4U, entry);
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
  Asm &moveq(unsigned d, std::uint8_t n) { return w(0x7000U | d << 9U | n); }
  Asm &move_b_ram(unsigned d, std::uint16_t address) { return w(0x1038U | d << 9U).w(address); }
  Asm &move_w_ram(unsigned d, std::uint16_t address) { return w(0x3038U | d << 9U).w(address); }
  Asm &andi_w(unsigned d, std::uint16_t imm) { return w(0x0240U | d).w(imm); }
  Asm &andi_b(unsigned d, std::uint8_t imm) { return w(0x0200U | d).w(imm); }
  Asm &cmpi_b(unsigned d, std::uint8_t imm) { return w(0x0C00U | d).w(imm); }
  Asm &subi_b(unsigned d, std::uint8_t imm) { return w(0x0400U | d).w(imm); }
  Asm &addq_w(unsigned d, unsigned n) { return w(0x5040U | (n & 7U) << 9U | d); }
  Asm &ext_w(unsigned d) { return w(0x4880U | d); }
  Asm &and_w_reg(unsigned source, unsigned d) { return w(0xC040U | d << 9U | source); }
  Asm &add_w_self(unsigned d) { return w(0xD040U | d << 9U | d); }
  Asm &lsl_w(unsigned d, unsigned count) { return w(0xE148U | (count & 7U) << 9U | d); }
  Asm &bcc_s(unsigned condition, std::uint32_t to) { return w(0x6000U | condition << 8U | ((to - pc - 2U) & 0xFFU)); }
  Asm &bra_w(std::uint32_t to) {
    w(0x6000U);
    return w((to - pc) & 0xFFFFU);
  }
  Asm &bra_self() { return w(0x60FEU); }
  Asm &rts() { return w(0x4E75U); }
  Asm &nop() { return w(0x4E71U); }
  Asm &index_ext(unsigned index, std::uint32_t table) { return w(index << 12U | ((table - pc) & 0xFFU)); }
  Asm &move_w_pcidx(unsigned index, unsigned destination, std::uint32_t table) {
    return w(0x303BU | destination << 9U).index_ext(index, table);
  }
  Asm &jmp_pcidx(unsigned index, std::uint32_t table) { return w(0x4EFBU).index_ext(index, table); }
  Asm &jsr_pcidx(unsigned index, std::uint32_t table) { return w(0x4EBBU).index_ext(index, table); }
  Asm &movea_l_imm(unsigned a, std::uint32_t value) { return w(0x207CU | a << 9U).w(value >> 16U).w(value); }
  Asm &jmp_an(unsigned a) { return w(0x4ED0U | a); }
};

std::uint32_t guarded_word_table_dispatch(Image &image, std::uint32_t at, std::uint8_t bound, std::uint32_t out,
                                          std::uint32_t table) {
  Asm a{image, at};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).cmpi_b(0, bound).bcc_s(HI, out).add_w_self(0).move_w_pcidx(0, 0, table);
  const auto jmp = a.pc;
  a.jmp_pcidx(0, table);
  return jmp;
}

// ------------------------------------------------------------------------------------------------------------------
// Side-by-side runner.

struct Pair {
  GenesisReachabilityChallengerResult challenger;
  M68kFiniteAnalysisResult adapter;
};

std::string outcome_of(const GenesisReachabilityChallengerResult &r, std::uint32_t pc) {
  const auto found = r.pc_index_sites.find(pc);
  return found == r.pc_index_sites.end() ? "absent" : genesis_pc_index_outcome_name(found->second.outcome);
}
std::string outcome_of(const M68kFiniteAnalysisResult &r, std::uint32_t pc) {
  const auto found = r.pc_index_sites.find(pc);
  return found == r.pc_index_sites.end() ? "absent" : m68k_pc_index_outcome_name(found->second.outcome);
}

M68kFiniteAnalysisResult run_adapter(const Image &image, const std::vector<std::uint32_t> &roots, bool width_domains) {
  const M68kFlatAnalysisImage view{image.bytes, 0U};
  M68kAnalysisConfig config{};
  config.accept_width_domains = width_domains;
  return analyze_m68k_finite_values(view, roots, config);
}

// Runs both analyses. `documented` lists sites whose outcome CLASS is allowed to differ (asserted separately by the
// caller); every other site must agree on class and exact target set. Detailed outcome-name differences inside the
// same class are printed (report) but are not failures.
Pair compare(const std::string &label, const Image &image, bool width_domains = false,
             const std::set<std::uint32_t> &documented = {}) {
  Pair out;
  const auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  if (!program) {
    expect(false, label + ": fixture program");
    return out;
  }
  GenesisReachabilityChallengerConfig config{};
  config.pc_index_recovery = true;
  config.pc_index_width_domains = width_domains;
  out.challenger = run_genesis_reachability_challenger(*program, config);
  out.adapter = run_adapter(image, out.challenger.roots, width_domains);
  expect(out.adapter.complete, label + ": adapter fixed point completes");

  std::set<std::uint32_t> sites;
  for (const auto &[pc, s] : out.challenger.pc_index_sites) sites.insert(pc);
  for (const auto &[pc, s] : out.adapter.pc_index_sites) sites.insert(pc);
  std::set<std::uint32_t> challenger_targets, adapter_targets;
  for (const auto pc : sites) {
    const auto c = out.challenger.pc_index_sites.find(pc);
    const auto a = out.adapter.pc_index_sites.find(pc);
    const bool c_resolved = c != out.challenger.pc_index_sites.end() && c->second.outcome == GenesisPcIndexOutcome::resolved;
    const bool a_resolved = a != out.adapter.pc_index_sites.end() && a->second.outcome == M68kPcIndexOutcome::resolved;
    std::ostringstream where;
    where << label << ": site 0x" << std::hex << pc << std::dec << " (challenger " << outcome_of(out.challenger, pc)
          << ", adapter " << outcome_of(out.adapter, pc) << ")";
    if (c_resolved) challenger_targets.insert(c->second.targets.begin(), c->second.targets.end());
    if (a_resolved) adapter_targets.insert(a->second.targets.begin(), a->second.targets.end());
    if (documented.contains(pc)) {
      std::cout << "documented difference: " << where.str() << '\n';
      continue;
    }
    expect(c != out.challenger.pc_index_sites.end() && a != out.adapter.pc_index_sites.end(),
           where.str() + ": encountered by both");
    expect(c_resolved == a_resolved, where.str() + ": identical outcome class");
    if (c_resolved && a_resolved) expect(c->second.targets == a->second.targets, where.str() + ": identical target set");
    if (!c_resolved && !a_resolved && outcome_of(out.challenger, pc) != outcome_of(out.adapter, pc))
      std::cout << "same class, different detail: " << where.str() << '\n';
  }
  if (documented.empty()) {
    expect(challenger_targets == adapter_targets, label + ": identical recovered target sets");
    std::set<std::uint32_t> discovered, reached;
    for (const auto &[pc, length] : out.challenger.discovered) discovered.insert(pc);
    for (const auto &[pc, length] : out.adapter.reached) reached.insert(pc);
    expect(discovered == reached, label + ": identical reached instruction set");
  }
  return out;
}

bool adapter_targets(const Pair &p, std::uint32_t pc, std::vector<std::uint32_t> expected) {
  const auto found = p.adapter.pc_index_sites.find(pc);
  return found != p.adapter.pc_index_sites.end() && found->second.outcome == M68kPcIndexOutcome::resolved &&
         found->second.targets == expected;
}
bool adapter_outcome(const Pair &p, std::uint32_t pc, M68kPcIndexOutcome expected) {
  const auto found = p.adapter.pc_index_sites.find(pc);
  return found != p.adapter.pc_index_sites.end() && found->second.outcome == expected;
}

// ------------------------------------------------------------------------------------------------------------------
// Fixtures (same images as reachability_pc_index_recovery_test).

void fixture1() {
  Image image;
  const auto jmp = guarded_word_table_dispatch(image, 0x200U, 2U, 0x270U, 0x220U);
  image.words(0x220U, {0x0020U, 0x0024U, 0x0028U, 0x0060U});
  for (std::uint32_t t : {0x240U, 0x244U, 0x248U}) Asm{image, t}.bra_self();
  Asm{image, 0x270U}.bra_self();
  Asm{image, 0x280U}.nop().rts();
  guarded_word_table_dispatch(image, 0x600U, 2U, 0x670U, 0x620U);
  image.words(0x620U, {0x0020U, 0x0024U, 0x0028U});
  for (std::uint32_t t : {0x640U, 0x644U, 0x648U}) Asm{image, t}.bra_self();
  const auto p = compare("F1", image);
  expect(adapter_targets(p, jmp, {0x240U, 0x244U, 0x248U}), "F1: adapter proves exactly three targets");
  expect(!p.adapter.reached.contains(0x280U), "F4: adjacent immutable data never becomes a target");
  expect(p.adapter.pc_index_sites.size() == 1U && !p.adapter.reached.contains(0x612U),
         "F10: unreachable dispatch never encountered");
  expect(!p.adapter.unresolved_computed.contains(jmp), "F1: resolved site is not an unresolved computed site");
  expect(p.adapter.solution.computed_targets.at(jmp) == std::set<std::uint64_t>{0x240U, 0x244U, 0x248U},
         "F1: generic solution records the computed targets");
}

void fixture2() {
  Image image;
  Asm a{image, 0x200U};
  a.move_b_ram(0, 0xF100U).andi_w(0, 0x000CU).jsr_pcidx(0, 0x210U).bra_self();
  Asm t{image, 0x210U};
  t.bra_w(0x240U).bra_w(0x250U).bra_w(0x260U).bra_w(0x270U);
  for (std::uint32_t leaf : {0x240U, 0x250U, 0x260U, 0x270U}) Asm{image, leaf}.rts();
  const auto p = compare("F2", image);
  expect(adapter_targets(p, 0x208U, {0x210U, 0x214U, 0x218U, 0x21CU}), "F2: mask proves four targets");
  expect(p.adapter.pc_index_sites.at(0x208U).call && p.adapter.reached.contains(0x20CU), "F2: JSR site and continuation");
  expect(m68k_query_data_register(p.adapter, 0x20CU, 0U, 16U) == analysis::FiniteValue::unknown(analysis::UnknownReason::unknown_input),
         "F2: the call continuation is an opaque entry (no call summary): D0 is Unknown there, not the pre-call set");
}

void fixture3() {
  Image image;
  Asm a{image, 0x200U};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).andi_b(0, 2U).move_w_pcidx(0, 1, 0x280U);
  const auto jmp = a.pc;
  a.jmp_pcidx(1, 0x280U);
  image.words(0x280U, {0xFFC0U, 0x0020U});
  Asm{image, 0x240U}.bra_self();
  Asm{image, 0x2A0U}.bra_self();
  const auto p = compare("F3", image);
  expect(adapter_targets(p, jmp, {0x240U, 0x2A0U}), "F3: signed entries");
}

void fixture12() {
  Image image;
  Asm{image, 0x200U}.bra_w(0x300U);
  Asm a{image, 0x300U};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).andi_b(0, 0x80U).ext_w(0);
  const auto jmp = a.pc;
  a.jmp_pcidx(0, 0x370U);
  Asm{image, 0x370U}.bra_self();
  Asm{image, 0x2F0U}.bra_self();
  const auto p = compare("F12b", image);
  expect(adapter_targets(p, jmp, {0x2F0U, 0x370U}), "F12b: EXT.W backward target");

  Image leak;
  Asm b{leak, 0x200U};
  b.moveq(1, 0).move_b_ram(1, 0xF100U).moveq(0, 0x7F).and_w_reg(1, 0).add_w_self(0).move_w_pcidx(0, 0, 0x280U);
  const auto jmp_leak = b.pc;
  b.jmp_pcidx(0, 0x280U);
  const auto pl = compare("F12b-leak", leak);
  expect(adapter_outcome(pl, jmp_leak, M68kPcIndexOutcome::width_only_domain), "F12b: register AND stays width-only");
}

void fixture5() {
  Image word_index;
  Asm a{word_index, 0x200U};
  a.move_w_ram(0, 0xF100U).add_w_self(0).move_w_pcidx(0, 0, 0x280U);
  const auto jmp_a = a.pc;
  a.jmp_pcidx(0, 0x280U);
  word_index.words(0x280U, {0x0400U});
  Asm{word_index, 0x680U}.nop().bra_self();
  const auto p = compare("F5-word", word_index);
  expect(adapter_outcome(p, jmp_a, M68kPcIndexOutcome::index_unknown) && !p.adapter.reached.contains(0x680U) &&
             p.adapter.unresolved_computed.contains(jmp_a),
         "F5: a RAM word is Unknown; nothing named; site reported unresolved");

  Image width_only;
  Asm b{width_only, 0x200U};
  b.moveq(0, 0).move_b_ram(0, 0xF100U).add_w_self(0).move_w_pcidx(0, 0, 0x280U);
  const auto jmp_b = b.pc;
  b.jmp_pcidx(0, 0x280U);
  width_only.words(0x280U, {0x0400U});
  Asm{width_only, 0x680U}.nop().bra_self();
  const auto strict = compare("F5-width-strict", width_only);
  expect(adapter_outcome(strict, jmp_b, M68kPcIndexOutcome::width_only_domain) && !strict.adapter.reached.contains(0x680U),
         "F5: width-only domain is unresolved under the strict policy");
  const auto variant = compare("F5-width-variant", width_only, true);
  expect(adapter_outcome(variant, jmp_b, M68kPcIndexOutcome::resolved) && variant.adapter.reached.contains(0x680U),
         "F5: the measurement variant admits the width-only domain");
}

void fixture6() {
  Image entry_outside;
  Asm{entry_outside, 0x200U}.bra_w(0xFE0U);
  Asm a{entry_outside, 0xFE0U};
  a.move_b_ram(0, 0xF100U).andi_w(0, 0x0006U).move_w_pcidx(0, 0, 0xFFCU);
  const auto jmp_a = a.pc;
  a.jmp_pcidx(0, 0xFFCU);
  const auto p = compare("F6-entry", entry_outside);
  expect(adapter_outcome(p, jmp_a, M68kPcIndexOutcome::entry_outside_immutable_image) &&
             p.adapter.pc_index_sites.at(jmp_a).reason == analysis::UnknownReason::non_immutable_read,
         "F6: entry read past the image fails closed (typed non_immutable_read)");

  Image target_outside;
  const auto jmp_b = guarded_word_table_dispatch(target_outside, 0x200U, 1U, 0x270U, 0x220U);
  target_outside.words(0x220U, {0x0020U, 0x7F00U});
  Asm{target_outside, 0x240U}.bra_self();
  Asm{target_outside, 0x270U}.bra_self();
  const auto t = compare("F6-target", target_outside);
  expect(adapter_outcome(t, jmp_b, M68kPcIndexOutcome::target_outside_image) && !t.adapter.reached.contains(0x240U),
         "F6: a target outside the image fails closed with no partial set");
}

void fixture7() {
  Image image;
  const auto jmp = guarded_word_table_dispatch(image, 0x200U, 2U, 0x270U, 0x220U);
  image.words(0x220U, {0x0020U, 0x0020U, 0x0024U});
  Asm{image, 0x240U}.bra_self();
  Asm{image, 0x244U}.bra_self();
  Asm{image, 0x270U}.bra_self();
  const auto p = compare("F7", image);
  expect(adapter_targets(p, jmp, {0x240U, 0x244U}), "F7: duplicates deduplicate");
}

// An odd table entry raises an address error at the JMP: it is excluded (counted), never a target and never a reason to fail
// the whole site.
void fixture_odd_target() {
  Image image;
  const auto jmp = guarded_word_table_dispatch(image, 0x200U, 2U, 0x270U, 0x220U);
  image.words(0x220U, {0x0020U, 0x0021U, 0x0024U});
  Asm{image, 0x240U}.bra_self();
  Asm{image, 0x244U}.bra_self();
  Asm{image, 0x270U}.bra_self();
  const auto p = compare("ODD", image);
  expect(adapter_targets(p, jmp, {0x240U, 0x244U}) && p.adapter.pc_index_sites.at(jmp).odd_targets_excluded == 1U,
         "ODD: the odd entry is excluded and counted; the even targets stay proven");
}

// Flag provenance needs the setter to be the SOLE predecessor. The guarded branch is first reached from its physically preceding
// CMPI (filter applies), then from a later BRA with the same register values but flags from a different instruction: the second
// arrival must remove the provenance (the states are not leq) and the fallthrough loses the filter.
void fixture_second_predecessor_removes_filter() {
  Image image;
  Asm a{image, 0x200U};
  a.move_b_ram(2, 0xF104U).bcc_s(EQ, 0x260U).moveq(0, 0).move_b_ram(0, 0xF100U).cmpi_b(0, 2U);
  const auto bhi = a.pc;
  a.bcc_s(HI, 0x280U);
  const auto fallthrough = a.pc;
  a.nop().bra_self();
  Asm{image, 0x280U}.bra_self();
  Asm{image, 0x260U}.moveq(0, 0).move_b_ram(0, 0xF100U).bra_w(bhi);
  const auto p = compare("FLAGS-2PRED", image);
  const auto guarded = m68k_query_data_register(p.adapter, fallthrough, 0U, 16U);
  expect(guarded.is_precise() && guarded.values().size() == 256U,
         "FLAGS-2PRED: a second predecessor removes the CMPI filter (D0.w is every byte, not {0,1,2}): " + guarded.describe());
  const auto at_branch = p.adapter.solution.in_states.find(bhi);
  expect(at_branch != p.adapter.solution.in_states.end() && !at_branch->second.flag_setter,
         "FLAGS-2PRED: the joined branch state carries no flag setter");
}

void fixture8_9() {
  Image image;
  Asm a{image, 0x200U};
  a.move_b_ram(0, 0xF100U).andi_w(0, 0x0004U).jmp_pcidx(0, 0x210U);
  Asm t{image, 0x210U};
  t.bra_w(0x300U).bra_w(0x400U);
  Asm b{image, 0x300U};
  b.move_b_ram(1, 0xF102U).andi_w(1, 0x0002U);
  const auto nested = b.pc;
  b.jmp_pcidx(1, 0x320U);
  image.words(0x320U, {0x60FEU, 0x60FEU});
  Asm{image, 0x400U}.movea_l_imm(0, 0x500U).jmp_an(0);
  Asm{image, 0x500U}.nop().bra_self();
  const auto p = compare("F8/F9", image);
  expect(adapter_targets(p, 0x208U, {0x210U, 0x214U}) && adapter_targets(p, nested, {0x320U, 0x322U}),
         "F8: first-level and nested sites resolved");
  expect(p.adapter.unresolved_computed.contains(0x406U) && !p.adapter.reached.contains(0x500U),
         "F9: JMP (A0) reported unresolved and never followed");
}

void fixture11() {
  Image image;
  Asm a{image, 0x200U};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).cmpi_b(0, 0xE0U).bcc_s(CS, 0x280U).cmpi_b(0, 0xE2U);
  const auto bls = a.pc;
  a.bcc_s(LS, bls + 4U).bra_self();
  a.subi_b(0, 0xE0U).lsl_w(0, 2U);
  const auto jmp = a.pc;
  a.jmp_pcidx(0, 0x240U);
  Asm t{image, 0x240U};
  t.bra_w(0x2A0U).bra_w(0x2A4U).bra_w(0x2A8U).bra_w(0x2B0U);
  for (std::uint32_t leaf : {0x2A0U, 0x2A4U, 0x2A8U, 0x2B0U}) Asm{image, leaf}.bra_self();
  Asm{image, 0x280U}.bra_self();
  const auto p = compare("F11", image);
  expect(adapter_targets(p, jmp, {0x240U, 0x244U, 0x248U}), "F11: guarded path gives three targets");

  Image join;
  Asm j{join, 0x200U};
  j.moveq(0, 0).move_b_ram(0, 0xF100U);
  j.bcc_s(EQ, 0x230U).cmpi_b(0, 2U).bcc_s(HI, 0x270U);
  j.pc = 0x230U;
  j.add_w_self(0).move_w_pcidx(0, 0, 0x260U);
  const auto jmp_join = j.pc;
  j.jmp_pcidx(0, 0x260U);
  Asm{join, 0x20EU}.bra_w(0x230U);
  join.words(0x260U, {0x0040U, 0x0044U, 0x0048U});
  Asm{join, 0x270U}.bra_self();
  const auto pj = compare("F11-join", join);
  expect(adapter_outcome(pj, jmp_join, M68kPcIndexOutcome::width_only_domain), "F11: unguarded join is not a proof");
}

// INV: the challenger resolves B in recovery round 0, loses the proof in round 1 and restarts with B pinned
// ("invalidated"). The forward fixed point already sees B's widened slice before B's transfer (ordered worklist), so B is
// unresolved as index_unknown. Same class, same targets: no restart is needed in this order.
void fixture_invalidation() {
  Image image;
  Asm root{image, 0x200U};
  root.move_b_ram(2, 0xF104U).bcc_s(EQ, 0x280U).bra_w(0x300U);
  Asm a{image, 0x280U};
  a.move_b_ram(1, 0xF102U).andi_w(1, 0x0004U).jmp_pcidx(1, 0x290U);
  Asm ta{image, 0x290U};
  ta.bra_w(0x2E0U).bra_w(0x30CU);
  Asm{image, 0x2E0U}.bra_self();
  const auto jmp_b = guarded_word_table_dispatch(image, 0x300U, 1U, 0x370U, 0x320U);
  image.words(0x320U, {0x0040U, 0x0044U});
  Asm{image, 0x360U}.bra_self();
  Asm{image, 0x364U}.bra_self();
  Asm{image, 0x370U}.bra_self();
  const auto p = compare("INV", image);
  expect(adapter_targets(p, 0x288U, {0x290U, 0x294U}), "INV: A stays proven");
  expect(!adapter_targets(p, jmp_b, {0x360U, 0x364U}) && !p.adapter.reached.contains(0x360U) &&
             !p.adapter.reached.contains(0x364U),
         "INV: B contributes no target");
  expect(outcome_of(p.challenger, jmp_b) == "invalidated", "INV: challenger detail is invalidated");
}

// The invalidation path: a site whose emitted targets are lost later is pinned (by the generic solver, then reported invalidated by
// the driver) and the solve restarts.
// Ordering forces it: B (low addresses) is transferred before the later edge from A (high addresses) widens its slice.
void fixture_adapter_invalidation() {
  Image image;
  Asm root{image, 0x200U};
  root.move_b_ram(2, 0xF104U).bcc_s(EQ, 0x220U).bra_w(0x300U);
  Asm{image, 0x220U}.bra_w(0x600U);
  const auto jmp_b = guarded_word_table_dispatch(image, 0x300U, 1U, 0x370U, 0x320U);  // ADD.W at 0x30C
  image.words(0x320U, {0x0040U, 0x0044U});
  Asm{image, 0x360U}.bra_self();
  Asm{image, 0x364U}.bra_self();
  Asm{image, 0x370U}.bra_self();
  // A at 0x600, its second entry branches into B's slice after B's guard.
  Asm a{image, 0x600U};
  a.move_b_ram(1, 0xF102U).andi_w(1, 0x0004U).jmp_pcidx(1, 0x610U);
  Asm ta{image, 0x610U};
  ta.bra_w(0x660U).bra_w(0x30CU);
  Asm{image, 0x660U}.bra_self();
  const auto p = compare("INV-forward", image);
  expect(adapter_outcome(p, jmp_b, M68kPcIndexOutcome::invalidated) && p.adapter.restarts == 1U,
         "INV-forward: lost targets pin the site invalidated after one restart");
  expect(!p.adapter.reached.contains(0x360U) && !p.adapter.reached.contains(0x364U),
         "INV-forward: an invalidated site contributes no target");
  expect(outcome_of(p.challenger, jmp_b) != "resolved", "INV-forward: the challenger also leaves B unresolved");
  expect(adapter_targets(p, 0x608U, {0x610U, 0x614U}), "INV-forward: A stays proven");
}

// SEG-029-T006 (correction cycle 2): a site whose own target re-enters its slice with an unbounded index. The generic solver pins
// the site; the driver must report it invalidated (never resolved from the narrower input of the restarted solve), like the challenger.
void fixture_self_widening_site() {
  Image image;
  const auto jmp = guarded_word_table_dispatch(image, 0x200U, 1U, 0x280U, 0x220U);  // ADD.W at 0x20C, JMP at 0x212
  image.words(0x220U, {0x0040U, 0x0050U});                                           // targets 0x260, 0x270
  Asm{image, 0x260U}.move_b_ram(0, 0xF100U).bra_w(0x20CU);                         // re-enters with a RAM index
  Asm{image, 0x270U}.bra_self();
  Asm{image, 0x280U}.bra_self();
  const auto p = compare("SELF", image);
  expect(adapter_outcome(p, jmp, M68kPcIndexOutcome::invalidated), "SELF: a solver-pinned site is reported invalidated");
  expect(p.adapter.pc_index_sites.at(jmp).targets.empty() && !p.adapter.reached.contains(0x260U) &&
             !p.adapter.reached.contains(0x270U),
         "SELF: the pinned site names no target and reaches none");
  expect(outcome_of(p.challenger, jmp) == "invalidated", "SELF: the challenger also invalidates the site");
}

void fixture_narrow_query() {
  for (const bool absolute : {false, true}) {
    Image image;
    constexpr std::uint32_t table = 0x240U;
    Asm a{image, 0x200U};
    if (absolute) {
      a.w(0x2038U).w(table + 4U);
    } else {
      a.moveq(1, 4);
      a.w(0x203BU).index_ext(1, table);
    }
    const auto jmp = a.pc;
    a.jmp_pcidx(0, table);
    image.words(table + 4U, {0x0001U, 0x0020U});
    Asm{image, 0x260U}.bra_self();
    const auto p = compare(absolute ? "WIDE-abs" : "WIDE-pcidx", image);
    expect(adapter_targets(p, jmp, {0x260U}), "WIDE: the .W index uses the low word of the long load");
    const auto low = m68k_query_data_register(p.adapter, jmp, 0U, 16U);
    const auto full = m68k_query_data_register(p.adapter, jmp, 0U, 32U);
    expect(low == analysis::FiniteValue::constant(0x20U) && full == analysis::FiniteValue::constant(0x00010020U),
           "WIDE: independent .W and .L slots");
  }
}

// A sound difference: D0 is invariant on a loop the backward evaluator meets as a cycle (Unknown, no loop reasoning);
// the forward fixed point carries D0 = {4} around the loop and resolves the site exactly.
void fixture_loop_invariant_index() {
  Image image;
  Asm a{image, 0x200U};
  a.moveq(0, 4);
  const auto loop = a.pc;
  a.move_b_ram(1, 0xF100U).bcc_s(EQ, loop);  // the loop writes only D1; flags from MOVE.B: no filter
  const auto jmp = a.pc;
  a.jmp_pcidx(0, 0x210U);
  Asm{image, 0x214U}.bra_self();
  const auto p = compare("LOOP", image, false, {jmp});
  expect(outcome_of(p.challenger, jmp) == "index_unknown", "LOOP: backward evaluator reports the cycle as Unknown");
  expect(adapter_targets(p, jmp, {0x214U}) && p.adapter.reached.contains(0x214U),
         "LOOP: forward fixed point resolves the loop-invariant index (sound: the only D0 writer is MOVEQ #4)");
  expect(m68k_query_data_register(p.adapter, loop, 0U, 16U) == analysis::FiniteValue::constant(4U),
         "LOOP: D0 at the loop head is exactly {4}");
}

// A loop that genuinely grows the index stays sound: the set exceeds its bound and the site is unresolved. The two
// back edges ({2x} and {2x+1}) double the head set each round, so the bound is reached in a few iterations.
void fixture_loop_growing_index() {
  Image image;
  Asm a{image, 0x200U};
  a.moveq(0, 1);
  const auto loop = a.pc;
  a.add_w_self(0).move_b_ram(1, 0xF100U).bcc_s(EQ, loop).addq_w(0, 1).move_b_ram(1, 0xF102U).bcc_s(EQ, loop);
  const auto jmp = a.pc;
  a.jmp_pcidx(0, 0x218U);
  const auto p = compare("LOOP-grow", image);
  expect(adapter_outcome(p, jmp, M68kPcIndexOutcome::index_unknown) &&
             p.adapter.pc_index_sites.at(jmp).reason == analysis::UnknownReason::set_bound,
         "LOOP-grow: a growing loop index becomes Unknown(set_bound), never a partial set");
}

// Width / flag-provenance lattice laws.
void state_lattice() {
  auto a = M68kAnalysisState::all_unknown();
  auto b = a;
  a.values.values[0] = analysis::FiniteValue::of({1U, 2U});
  b.values.values[0] = analysis::FiniteValue::of({3U});
  a.width_derived[0] = true;
  a.flag_setter = 0x200U;
  b.flag_setter = 0x200U;
  const auto j = join(a, b);
  expect(j.values.values[0] == analysis::FiniteValue::of({1U, 2U, 3U}) && j.width_derived[0] && j.flag_setter == 0x200U,
         "LAT: join unions values, ORs width_derived, keeps an agreeing flag setter");
  expect(leq(a, j) && leq(b, j) && !leq(j, b), "LAT: join is an upper bound");
  b.flag_setter = 0x204U;
  expect(!join(a, b).flag_setter, "LAT: disagreeing flag setters join to none");
  b.flag_setter.reset();
  expect(!join(a, b).flag_setter && leq(a, join(a, b)), "LAT: an edge with no setter removes the provenance");
  auto u = a;
  u.values.values[0] = analysis::FiniteValue::unknown(analysis::UnknownReason::unsupported_transfer);
  u.width_derived[0] = false;
  const auto ju = join(a, u);
  expect(ju.values.values[0].is_unknown() && !ju.width_derived[0], "LAT: Unknown carries no width annotation");
  expect(leq(M68kAnalysisState::unreachable(), a) && join(M68kAnalysisState::unreachable(), a) == a, "LAT: bottom");
}

void determinism() {
  Image image;
  guarded_word_table_dispatch(image, 0x200U, 2U, 0x270U, 0x220U);
  image.words(0x220U, {0x0020U, 0x0024U, 0x0028U});
  for (std::uint32_t t : {0x240U, 0x244U, 0x248U}) Asm{image, t}.bra_self();
  Asm{image, 0x270U}.bra_self();
  const auto x = format_m68k_finite_analysis(run_adapter(image, {entry}, false));
  const auto y = format_m68k_finite_analysis(run_adapter(image, {entry}, false));
  expect(!x.empty() && x == y, "DET: byte-identical serialized result across runs");
  const M68kFlatAnalysisImage view{image.bytes, 0U};
  const auto bounded = analyze_m68k_finite_values(view, {entry}, {}, analysis::Bounds{3U, analysis::default_max_points});
  expect(!bounded.complete && bounded.reason == analysis::UnknownReason::iteration_bound &&
             m68k_query_data_register(bounded, entry, 0U, 16U).is_unknown(),
         "BOUND: an exhausted iteration bound gives Unknown(iteration_bound) for every query");
}

}  // namespace

int main() {
  fixture1();
  fixture2();
  fixture3();
  fixture5();
  fixture6();
  fixture7();
  fixture_odd_target();
  fixture_second_predecessor_removes_filter();
  fixture8_9();
  fixture11();
  fixture12();
  fixture_invalidation();
  fixture_adapter_invalidation();
  fixture_self_widening_site();
  fixture_narrow_query();
  fixture_loop_invariant_index();
  fixture_loop_growing_index();
  state_lattice();
  determinism();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_equivalence_test: OK\n";
  return EXIT_SUCCESS;
}
