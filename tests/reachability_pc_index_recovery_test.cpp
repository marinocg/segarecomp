// SEG-026-T002: exact PC-indexed immutable jump-table recovery in the reachability challenger (project-authored,
// ROM-independent fixtures). Every image is a small synthetic Genesis image (reset PC = 0x200) whose bytes are real
// MC68000 encodings decoded by the unchanged decoder/lifter, so exact synthetic addresses may be asserted.
// RAM operands use absolute-word addresses in the 0xF100 range (sign-extended into work RAM: mutable memory).

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/finite_register_values.hpp"
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

// A tiny sequential assembler for the handful of encodings the fixtures need.
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
  // (d8,PC,Dindex.W) operand: the extension word's own address is the PC base.
  Asm &index_ext(unsigned index, std::uint32_t table) { return w(index << 12U | ((table - pc) & 0xFFU)); }
  Asm &move_w_pcidx(unsigned index, unsigned destination, std::uint32_t table) {
    return w(0x303BU | destination << 9U).index_ext(index, table);
  }
  Asm &jmp_pcidx(unsigned index, std::uint32_t table) { return w(0x4EFBU).index_ext(index, table); }
  Asm &jsr_pcidx(unsigned index, std::uint32_t table) { return w(0x4EBBU).index_ext(index, table); }
  Asm &movea_l_imm(unsigned a, std::uint32_t value) { return w(0x207CU | a << 9U).w(value >> 16U).w(value); }
  Asm &jmp_an(unsigned a) { return w(0x4ED0U | a); }
};

GenesisReachabilityChallengerResult run(const Image &image, bool recovery = true, bool width_domains = false) {
  const auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  if (!program) {
    ++failures;
    std::cerr << "FAIL: fixture program\n";
    return {};
  }
  GenesisReachabilityChallengerConfig config{};
  config.pc_index_recovery = recovery;
  config.pc_index_width_domains = width_domains;
  return run_genesis_reachability_challenger(*program, config);
}

bool in_d(const GenesisReachabilityChallengerResult &r, std::uint32_t pc) { return r.discovered.contains(pc); }
const GenesisPcIndexSiteRecovery *site(const GenesisReachabilityChallengerResult &r, std::uint32_t pc) {
  const auto found = r.pc_index_sites.find(pc);
  return found == r.pc_index_sites.end() ? nullptr : &found->second;
}
bool outcome(const GenesisReachabilityChallengerResult &r, std::uint32_t pc, GenesisPcIndexOutcome expected) {
  const auto *s = site(r, pc);
  return s != nullptr && s->outcome == expected;
}
bool targets(const GenesisReachabilityChallengerResult &r, std::uint32_t pc, std::vector<std::uint32_t> expected) {
  const auto *s = site(r, pc);
  return s != nullptr && s->outcome == GenesisPcIndexOutcome::resolved && s->targets == expected;
}

// Word-offset table dispatch guarded by CMPI.B/BHI (index <= bound), with the MOVEQ #0 + MOVE.B zero extension:
//   MOVEQ #0,D0; MOVE.B (ram).W,D0; CMPI.B #bound,D0; BHI.S out; ADD.W D0,D0;
//   MOVE.W (tbl,PC,D0.W),D0; JMP (tbl,PC,D0.W)       -> the JMP is at `at + 0x12`.
std::uint32_t guarded_word_table_dispatch(Image &image, std::uint32_t at, std::uint8_t bound, std::uint32_t out,
                                          std::uint32_t table) {
  Asm a{image, at};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).cmpi_b(0, bound).bcc_s(HI, out).add_w_self(0).move_w_pcidx(0, 0, table);
  const auto jmp = a.pc;
  a.jmp_pcidx(0, table);
  return jmp;
}

// F1 (explicit bound), F4 (adjacent immutable data never enters), F10 (unreachable table-like bytes), F12 (x2).
void fixture1_explicit_bound_and_adjacent_data() {
  Image image;
  const auto jmp = guarded_word_table_dispatch(image, 0x200U, 2U, 0x270U, 0x220U);
  image.words(0x220U, {0x0020U, 0x0024U, 0x0028U,  // exactly three entries (index 0..2, doubled)
                       0x0060U});                  // adjacent immutable data: would name 0x280
  for (std::uint32_t t : {0x240U, 0x244U, 0x248U}) Asm{image, t}.bra_self();
  Asm{image, 0x270U}.bra_self();
  Asm{image, 0x280U}.nop().rts();  // legal code only the adjacent word would name
  // F10: an identical, unreachable dispatch + table island.
  guarded_word_table_dispatch(image, 0x600U, 2U, 0x670U, 0x620U);
  image.words(0x620U, {0x0020U, 0x0024U, 0x0028U});
  for (std::uint32_t t : {0x640U, 0x644U, 0x648U}) Asm{image, t}.bra_self();

  const auto r = run(image);
  expect(jmp == 0x212U, "F1: fixture layout");
  expect(targets(r, jmp, {0x240U, 0x244U, 0x248U}), "F1: guard-bounded word table yields exactly three targets");
  const auto *s = site(r, jmp);
  expect(s != nullptr && (s->proof & m68k_finite_proof::guard) != 0U && (s->proof & m68k_finite_proof::immutable_load) != 0U &&
             (s->proof & m68k_finite_proof::add_sub) != 0U && s->table_reads == 3U,
         "F1/F12: proof uses the guard, x2 transform and exactly three immutable entry reads");
  expect(in_d(r, 0x240U) && in_d(r, 0x244U) && in_d(r, 0x248U), "F1: exact targets enter D");
  expect(!in_d(r, 0x280U) && !in_d(r, 0x282U), "F4: adjacent immutable data never becomes a target");
  expect(r.sites[static_cast<std::size_t>(M68kDynamicControlFamily::jump_pc_index)].empty(),
         "F1: a resolved site is no longer an unresolved dynamic site");
  for (std::uint32_t pc = 0x600U; pc < 0x700U; pc += 2U) expect(!in_d(r, pc), "F10: unreachable table-like island never enters D");
  expect(r.pc_index_sites.size() == 1U, "F10: the unreachable dispatch is never encountered");
  expect(r.table_entry_addresses.size() == 3U, "F4: only the three proven entries are read");
}

// F2: mask-bounded index into an inline BRA.W table (JSR), the upper word unknown before the mask.
void fixture2_mask_bounded() {
  Image image;
  Asm a{image, 0x200U};
  a.move_b_ram(0, 0xF100U).andi_w(0, 0x000CU).jsr_pcidx(0, 0x210U).bra_self();  // JSR at 0x208, continuation 0x20C
  Asm t{image, 0x210U};
  t.bra_w(0x240U).bra_w(0x250U).bra_w(0x260U).bra_w(0x270U);
  for (std::uint32_t leaf : {0x240U, 0x250U, 0x260U, 0x270U}) Asm{image, leaf}.rts();
  const auto r = run(image);
  expect(targets(r, 0x208U, {0x210U, 0x214U, 0x218U, 0x21CU}), "F2: ANDI.W #$C proves exactly four targets");
  const auto *s = site(r, 0x208U);
  expect(s != nullptr && s->call && (s->proof & m68k_finite_proof::mask) != 0U, "F2: mask proof on a JSR site");
  expect(in_d(r, 0x240U) && in_d(r, 0x270U) && in_d(r, 0x20CU), "F2: targets' leaves and the JSR continuation (via RTS)");
}

// F3: signed word entries (a negative offset) with the second-level index in D1; ANDI.B on a zero-extended byte.
void fixture3_signed_entries() {
  Image image;
  Asm a{image, 0x200U};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).andi_b(0, 2U).move_w_pcidx(0, 1, 0x280U);
  const auto jmp = a.pc;
  a.jmp_pcidx(1, 0x280U);
  image.words(0x280U, {0xFFC0U, 0x0020U});  // -0x40 -> 0x240, +0x20 -> 0x2A0
  Asm{image, 0x240U}.bra_self();
  Asm{image, 0x2A0U}.bra_self();
  const auto r = run(image);
  expect(targets(r, jmp, {0x240U, 0x2A0U}), "F3: sign-extended word entries give a backward and a forward target");
}

// F12b: EXT.W sign-extends an exactly known byte (a negative index names a target before the table base); an AND
// with a width-only register source stays width-only.
void fixture12_ext_and_register_and() {
  Image image;
  Asm{image, 0x200U}.bra_w(0x300U);
  Asm a{image, 0x300U};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).andi_b(0, 0x80U).ext_w(0);  // {0, 0x80} -> {0, 0xFF80}
  const auto jmp = a.pc;
  a.jmp_pcidx(0, 0x370U);
  Asm{image, 0x370U}.bra_self();
  Asm{image, 0x2F0U}.bra_self();
  const auto r = run(image);
  expect(targets(r, jmp, {0x2F0U, 0x370U}), "F12b: EXT.W gives the sign-extended backward target");
  const auto *s = site(r, jmp);
  expect(s != nullptr && (s->proof & m68k_finite_proof::sign_extend) != 0U, "F12b: EXT in the proof");

  Image leak;
  Asm b{leak, 0x200U};
  b.moveq(1, 0).move_b_ram(1, 0xF100U).moveq(0, 0x7F).and_w_reg(1, 0).add_w_self(0).move_w_pcidx(0, 0, 0x280U);
  const auto jmp_leak = b.pc;
  b.jmp_pcidx(0, 0x280U);
  const auto rl = run(leak);
  expect(outcome(rl, jmp_leak, GenesisPcIndexOutcome::width_only_domain),
         "F12b: AND with a width-only register source is not an explicit bound");
}

// F5: unprovable extents stay unresolved; nothing they would name enters D. The width-only variant is measured.
void fixture5_unprovable() {
  Image word_index;
  Asm a{word_index, 0x200U};
  a.move_w_ram(0, 0xF100U).add_w_self(0).move_w_pcidx(0, 0, 0x280U);
  const auto jmp_a = a.pc;
  a.jmp_pcidx(0, 0x280U);
  word_index.words(0x280U, {0x0400U});
  Asm{word_index, 0x680U}.nop().bra_self();
  const auto r = run(word_index);
  expect(outcome(r, jmp_a, GenesisPcIndexOutcome::index_unknown), "F5: a word from RAM is not a finite domain");
  expect(!in_d(r, 0x680U), "F5: an unproven table names nothing");
  expect(r.sites[static_cast<std::size_t>(M68kDynamicControlFamily::jump_pc_index)].contains(jmp_a),
         "F5: the unresolved site stays reported");

  Image width_only;
  Asm b{width_only, 0x200U};
  b.moveq(0, 0).move_b_ram(0, 0xF100U).add_w_self(0).move_w_pcidx(0, 0, 0x280U);
  const auto jmp_b = b.pc;
  b.jmp_pcidx(0, 0x280U);
  width_only.words(0x280U, {0x0400U});  // entry 0 names 0x680; the over-read (zero) entries name the table base
  Asm{width_only, 0x680U}.nop().bra_self();
  const auto strict = run(width_only);
  expect(outcome(strict, jmp_b, GenesisPcIndexOutcome::width_only_domain),
         "F5: a zero-extended RAM byte without an explicit bound is width-only (strict: unresolved)");
  expect(!in_d(strict, 0x680U), "F5: width-only domain names nothing under the strict policy");
  const auto variant = run(width_only, true, true);
  expect(outcome(variant, jmp_b, GenesisPcIndexOutcome::resolved) && in_d(variant, 0x680U),
         "F5: the measurement variant admits the width-only domain (and its over-read entries)");
}

// F6: malformed tables fail closed: an entry read outside the image, and a target outside the image.
void fixture6_malformed() {
  Image entry_outside;
  Asm{entry_outside, 0x200U}.bra_w(0xFE0U);
  Asm a{entry_outside, 0xFE0U};
  a.move_b_ram(0, 0xF100U).andi_w(0, 0x0006U).move_w_pcidx(0, 0, 0xFFCU);  // entries at 0xFFC..0x1002
  const auto jmp_a = a.pc;
  a.jmp_pcidx(0, 0xFFCU);
  const auto r = run(entry_outside);
  expect(outcome(r, jmp_a, GenesisPcIndexOutcome::entry_outside_immutable_image),
         "F6: an entry read past the immutable image fails closed");
  expect(r.recovered_targets.empty(), "F6: no target from a malformed table");

  Image target_outside;
  const auto jmp_b = guarded_word_table_dispatch(target_outside, 0x200U, 1U, 0x270U, 0x220U);
  target_outside.words(0x220U, {0x0020U, 0x7F00U});  // second entry names 0x7F20, beyond the 4 KiB image
  Asm{target_outside, 0x240U}.bra_self();
  Asm{target_outside, 0x270U}.bra_self();
  const auto t = run(target_outside);
  expect(outcome(t, jmp_b, GenesisPcIndexOutcome::target_outside_image), "F6: a target outside the image fails closed");
  expect(!in_d(t, 0x240U), "F6: fail closed means no target of that site, not a partial set");
}

// F7: duplicate entries deduplicate deterministically.
void fixture7_duplicates() {
  Image image;
  const auto jmp = guarded_word_table_dispatch(image, 0x200U, 2U, 0x270U, 0x220U);
  image.words(0x220U, {0x0020U, 0x0020U, 0x0024U});
  Asm{image, 0x240U}.bra_self();
  Asm{image, 0x244U}.bra_self();
  Asm{image, 0x270U}.bra_self();
  const auto r = run(image);
  expect(targets(r, jmp, {0x240U, 0x244U}), "F7: two distinct targets from three entries");
  expect(r.recovered_targets.size() == 2U, "F7: recovered target set deduplicated");
}

// F8: a recovered target exposes a second, independently proven PC-indexed site (nested round).
// F9: another recovered target exposes a JMP (An) site: reported, never followed.
void fixture8_9_nested_and_an() {
  Image image;
  Asm a{image, 0x200U};
  a.move_b_ram(0, 0xF100U).andi_w(0, 0x0004U).jmp_pcidx(0, 0x210U);  // JMP at 0x208, targets 0x210 / 0x214
  Asm t{image, 0x210U};
  t.bra_w(0x300U).bra_w(0x400U);
  Asm b{image, 0x300U};
  b.move_b_ram(1, 0xF102U).andi_w(1, 0x0002U);
  const auto nested = b.pc;
  b.jmp_pcidx(1, 0x320U);
  image.words(0x320U, {0x60FEU, 0x60FEU});  // two BRA.S self entries (index 0, 2)
  Asm{image, 0x400U}.movea_l_imm(0, 0x500U).jmp_an(0);
  Asm{image, 0x500U}.nop().bra_self();
  const auto r = run(image);
  expect(targets(r, 0x208U, {0x210U, 0x214U}), "F8: first-level table");
  const auto *s = site(r, nested);
  expect(s != nullptr && s->first_round == 1U && s->outcome == GenesisPcIndexOutcome::resolved &&
             s->targets == std::vector<std::uint32_t>{0x320U, 0x322U},
         "F8: the nested site is found in round 1 and proven independently");
  expect(r.recovery_rounds == 2U, "F8: two recovery rounds changed a target set");
  expect(r.sites[static_cast<std::size_t>(M68kDynamicControlFamily::jump_address_indirect)].contains(0x406U),
         "F9: the JMP (A0) exposed by a recovered target is reported");
  expect(!in_d(r, 0x500U), "F9: the (An) target is never followed");
}

// F11: a bound branch where only the taken path reaches the dispatch (resolved), vs. a join with an unguarded path.
void fixture11_one_path() {
  // Guarded ladder: index in [0xE0, 0xE2] only on the path that reaches the dispatch; SUBI.B + LSL.W #2 (F12).
  Image image;
  Asm a{image, 0x200U};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).cmpi_b(0, 0xE0U).bcc_s(CS, 0x280U).cmpi_b(0, 0xE2U);
  const auto bls = a.pc;
  a.bcc_s(LS, bls + 4U).bra_self();  // the not-taken path loops forever: never reaches the dispatch
  a.subi_b(0, 0xE0U).lsl_w(0, 2U);
  const auto jmp = a.pc;
  a.jmp_pcidx(0, 0x240U);
  Asm t{image, 0x240U};
  t.bra_w(0x2A0U).bra_w(0x2A4U).bra_w(0x2A8U).bra_w(0x2B0U);  // fourth entry is never proven
  for (std::uint32_t leaf : {0x2A0U, 0x2A4U, 0x2A8U, 0x2B0U}) Asm{image, leaf}.bra_self();
  Asm{image, 0x280U}.bra_self();
  const auto r = run(image);
  expect(targets(r, jmp, {0x240U, 0x244U, 0x248U}), "F11: only the guarded path reaches the dispatch: three targets");
  const auto *s = site(r, jmp);
  expect(s != nullptr && (s->proof & m68k_finite_proof::guard) != 0U && (s->proof & m68k_finite_proof::shift) != 0U,
         "F11/F12: guard and LSL transform in the proof");
  expect(!in_d(r, 0x24CU) && !in_d(r, 0x2B0U), "F11: the unproven fourth entry is not a target");

  // Join: an unguarded BEQ (flags from MOVE.B: no exact filter) also reaches the dispatch.
  Image join;
  Asm j{join, 0x200U};
  j.moveq(0, 0).move_b_ram(0, 0xF100U);
  const auto beq = j.pc;
  j.bcc_s(EQ, 0x230U).cmpi_b(0, 2U).bcc_s(HI, 0x270U);
  j.pc = 0x230U;
  j.add_w_self(0).move_w_pcidx(0, 0, 0x260U);
  const auto jmp_join = j.pc;
  j.jmp_pcidx(0, 0x260U);
  (void)beq;
  // The guarded fallthrough (0x20E) reaches 0x230 by branch.
  Asm{join, 0x20EU}.bra_w(0x230U);
  join.words(0x260U, {0x0040U, 0x0044U, 0x0048U});
  Asm{join, 0x270U}.bra_self();
  const auto rj = run(join);
  expect(outcome(rj, jmp_join, GenesisPcIndexOutcome::width_only_domain),
         "F11: a join with an unbounded path is not a proof (strict: unresolved)");
}

// Proof invalidation: a site proven in round 1 loses its proof when a later-recovered target joins its slice with
// an unknown index; discovery restarts with it pinned unresolved.
void fixture_invalidation() {
  Image image;
  Asm root{image, 0x200U};
  root.move_b_ram(2, 0xF104U).bcc_s(EQ, 0x280U).bra_w(0x300U);
  // A: mask-proven JMP (d8,PC,D1.W) into a BRA.W table whose second entry branches into B's slice.
  Asm a{image, 0x280U};
  a.move_b_ram(1, 0xF102U).andi_w(1, 0x0004U).jmp_pcidx(1, 0x290U);  // JMP at 0x288
  Asm ta{image, 0x290U};
  ta.bra_w(0x2E0U).bra_w(0x30CU);  // entry 1 lands on B's ADD.W, after B's guard
  Asm{image, 0x2E0U}.bra_self();
  // B: guarded word-table dispatch at 0x300 (ADD.W at 0x30C, JMP at 0x312).
  const auto jmp_b = guarded_word_table_dispatch(image, 0x300U, 1U, 0x370U, 0x320U);
  image.words(0x320U, {0x0040U, 0x0044U});
  Asm{image, 0x360U}.bra_self();
  Asm{image, 0x364U}.bra_self();
  Asm{image, 0x370U}.bra_self();
  const auto r = run(image);
  expect(jmp_b == 0x312U, "INV: fixture layout");
  expect(targets(r, 0x288U, {0x290U, 0x294U}), "INV: A stays proven");
  expect(outcome(r, jmp_b, GenesisPcIndexOutcome::invalidated) && r.recovery_restarts == 1U,
         "INV: B's proof is invalidated by the later edge and pinned unresolved after one restart");
  expect(!in_d(r, 0x360U) && !in_d(r, 0x364U), "INV: an invalidated site contributes no target");
}

// Off by default (T001 behaviour unchanged) and deterministic.
void fixture_default_and_determinism() {
  Image image;
  const auto jmp = guarded_word_table_dispatch(image, 0x200U, 2U, 0x270U, 0x220U);
  image.words(0x220U, {0x0020U, 0x0024U, 0x0028U});
  for (std::uint32_t t : {0x240U, 0x244U, 0x248U}) Asm{image, t}.bra_self();
  Asm{image, 0x270U}.bra_self();
  const auto off = run(image, false);
  expect(off.pc_index_sites.empty() && !in_d(off, 0x240U) &&
             off.sites[static_cast<std::size_t>(M68kDynamicControlFamily::jump_pc_index)].contains(jmp),
         "DEFAULT: recovery is off by default; the site stays unresolved");
  const auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  GenesisReachabilityChallengerConfig config{};
  config.pc_index_recovery = true;
  const auto a = run_genesis_reachability_challenger(*program, config);
  const auto b = run_genesis_reachability_challenger(*program, config);
  expect(format_genesis_reachability_challenger_private(a, config) == format_genesis_reachability_challenger_private(b, config),
         "DET: identical private output across runs");
  const auto aggregate = format_genesis_reachability_challenger_aggregate(a, config);
  expect(aggregate.find("\"pc_index_recovery\":{\"sites_encountered\":1,\"resolved\":1") != std::string::npos,
         "AGG: aggregate recovery counts");
  expect(aggregate.find("000240") == std::string::npos && aggregate.find("0x") == std::string::npos,
         "AGG: aggregate carries no target address");
}

// A `.W` index register loaded by MOVE.L from immutable bytes is the operand's LOW word, which big-endian memory
// holds at the HIGHER address. The operand is read at its architectural access width (long) and only then
// restricted (SEG-026-T003 correction; the narrow query previously read the high word).
void fixture_narrow_query_of_wider_load() {
  for (const bool absolute : {false, true}) {
    Image image;
    constexpr std::uint32_t table = 0x240U;
    Asm a{image, 0x200U};
    if (absolute) {
      a.w(0x2038U).w(table + 4U);  // MOVE.L (table+4).W,D0
    } else {
      a.moveq(1, 4);
      a.w(0x203BU).index_ext(1, table);  // MOVE.L (table,PC,D1.W),D0
    }
    const auto jmp = a.pc;
    a.jmp_pcidx(0, table);  // JMP (table,PC,D0.W)
    // High word 0x0001 would name an odd PC (no target); the low word names 0x260.
    image.words(table + 4U, {0x0001U, 0x0020U});
    Asm{image, 0x260U}.bra_self();
    const auto r = run(image);
    expect(targets(r, jmp, {0x260U}) && in_d(r, 0x260U),
           absolute ? "WIDE: MOVE.L (abs).W -> .W index uses the low word"
                    : "WIDE: MOVE.L (d8,PC,Xn) -> .W index uses the low word");
  }
}

}  // namespace

int main() {
  fixture_narrow_query_of_wider_load();
  fixture1_explicit_bound_and_adjacent_data();
  fixture2_mask_bounded();
  fixture3_signed_entries();
  fixture5_unprovable();
  fixture12_ext_and_register_and();
  fixture6_malformed();
  fixture7_duplicates();
  fixture8_9_nested_and_an();
  fixture11_one_path();
  fixture_invalidation();
  fixture_default_and_determinism();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "reachability_pc_index_recovery_tests: OK\n";
  return EXIT_SUCCESS;
}
