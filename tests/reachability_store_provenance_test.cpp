// SEG-026-T003: exact static store provenance of width-only PC-indexed dispatch indices in the reachability challenger
// (project-authored, ROM-independent fixtures). Every image is a small synthetic Genesis image (reset PC = 0x200)
// whose bytes are real MC68000 encodings decoded by the unchanged decoder/lifter. State bytes are absolute-word
// work-RAM addresses in the 0xF100 range (sign-extended to 0xFFF1xx).
//
// Every fixture dispatches through the same shape, which SEG-026-T002 leaves `width_only_domain`:
//   MOVEQ #0,D0; MOVE.B (S).W,D0; MOVE.W (tbl,PC,D0.W),D0; JMP (tbl,PC,D0.W)

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include "segarecomp/machine/genesis/reachability_challenger.hpp"

namespace {

using namespace segarecomp;
using Mode = GenesisReachabilityChallengerConfig::StoreProvenance;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint16_t S = 0xF100U;   // the state byte
constexpr std::uint16_t T = 0xF102U;   // another state byte
constexpr std::uint16_t U = 0xF300U;   // untracked mutable memory
constexpr std::uint32_t table = 0x280U;  // within the d8 reach of every fixture dispatch
constexpr unsigned LS = 3U;

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
  Asm &move_b_imm_ram(std::uint8_t imm, std::uint16_t address) { return w(0x11FCU).w(imm).w(address); }
  Asm &move_b_reg_ram(unsigned d, std::uint16_t address) { return w(0x11C0U | d).w(address); }
  Asm &move_b_ram(unsigned d, std::uint16_t address) { return w(0x1038U | d << 9U).w(address); }
  Asm &move_w_ram(unsigned d, std::uint16_t address) { return w(0x3038U | d << 9U).w(address); }
  Asm &move_b_disp_a0(unsigned d, std::int16_t disp) { return w(0x1028U | d << 9U).w(static_cast<std::uint16_t>(disp)); }
  Asm &move_b_d1_to_a1() { return w(0x1281U); }            // MOVE.B D1,(A1)
  Asm &move_l_d1_push() { return w(0x2F01U); }             // MOVE.L D1,-(A7)
  Asm &movea_l_ram(unsigned a, std::uint16_t address) { return w(0x2078U | a << 9U).w(address); }
  Asm &lea_ram(unsigned a, std::uint16_t address) { return w(0x41F8U | a << 9U).w(address); }
  Asm &addq_b_ram(unsigned n, std::uint16_t address) { return w(0x5038U | (n & 7U) << 9U).w(address); }
  Asm &eori_b_ram(std::uint8_t imm, std::uint16_t address) { return w(0x0A38U).w(imm).w(address); }
  Asm &addq_b(unsigned n, unsigned d) { return w(0x5000U | (n & 7U) << 9U | d); }
  Asm &cmpi_b(unsigned d, std::uint8_t imm) { return w(0x0C00U | d).w(imm); }
  Asm &bcc_s(unsigned condition, std::uint32_t to) { return w(0x6000U | condition << 8U | ((to - pc - 2U) & 0xFFU)); }
  Asm &bra_self() { return w(0x60FEU); }
  Asm &nop() { return w(0x4E71U); }
  Asm &rte() { return w(0x4E73U); }
  Asm &index_ext(unsigned index, std::uint32_t at) { return w(index << 12U | ((at - pc) & 0xFFU)); }
  // The width-only dispatch on the byte at `source` (absolute) or at `disp(A0)`; returns the JMP's PC.
  std::uint32_t dispatch_abs(std::uint16_t source) {
    moveq(0, 0).move_b_ram(0, source);
    return tail();
  }
  std::uint32_t dispatch_a0(std::int16_t disp) {
    moveq(0, 0).move_b_disp_a0(0, disp);
    return tail();
  }
  std::uint32_t tail() {
    w(0x303BU).index_ext(0, table);  // MOVE.W (tbl,PC,D0.W),D0
    const auto jmp = pc;
    w(0x4EFBU).index_ext(0, table);  // JMP (tbl,PC,D0.W)
    return jmp;
  }
};

// A word-offset table whose entry at even byte index i names 0x400 + 0x10 * i (entries for even indices below
// size; an odd index is a misaligned word read, excluded). Every named target is an idle loop, so a target in D
// proves the dispatch resolved it.
void table_and_targets(Image &image, unsigned size) {
  for (unsigned i = 0; i < size; i += 2U) {
    image.words(table + i, {static_cast<std::uint16_t>(0x400U + 0x10U * i - table)});
    Asm{image, 0x400U + 0x10U * i}.bra_self();
  }
}
std::uint32_t target(unsigned value) {  // the target of index value `value` (an entry is read at table + value)
  return 0x400U + 0x10U * value;
}

GenesisReachabilityChallengerResult run(const Image &image, Mode mode = Mode::prove,
                                        GenesisStoreAliasPolicy policy = GenesisStoreAliasPolicy::strict) {
  const auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  if (!program) {
    ++failures;
    std::cerr << "FAIL: fixture program\n";
    return {};
  }
  GenesisReachabilityChallengerConfig config{};
  config.pc_index_recovery = true;
  config.store_provenance = mode;
  config.store_alias_policy = policy;
  return run_genesis_reachability_challenger(*program, config);
}

const GenesisPcIndexSiteRecovery *site(const GenesisReachabilityChallengerResult &r, std::uint32_t pc) {
  const auto found = r.pc_index_sites.find(pc);
  return found == r.pc_index_sites.end() ? nullptr : &found->second;
}
bool outcome(const GenesisReachabilityChallengerResult &r, std::uint32_t pc, GenesisPcIndexOutcome expected) {
  const auto *s = site(r, pc);
  return s != nullptr && s->outcome == expected;
}
bool source(const GenesisReachabilityChallengerResult &r, std::uint32_t pc, GenesisStateSourceOutcome expected) {
  const auto *s = site(r, pc);
  return s != nullptr && s->source_outcome == expected;
}
std::vector<std::uint32_t> targets_of(std::initializer_list<unsigned> values) {
  std::vector<std::uint32_t> out;
  for (const auto v : values) out.push_back(target(v));
  return out;
}
bool resolved_to(const GenesisReachabilityChallengerResult &r, std::uint32_t pc, std::initializer_list<unsigned> values) {
  const auto *s = site(r, pc);
  return s != nullptr && s->outcome == GenesisPcIndexOutcome::resolved && s->targets == targets_of(values);
}
const GenesisStateSourceRecord *location(const GenesisReachabilityChallengerResult &r, std::uint16_t address) {
  const auto found = r.state_sources.find(0x00FF0000U | address);
  return found == r.state_sources.end() ? nullptr : &found->second;
}

// F1: a state byte written only by constants has the exact domain {reset 0, constants}; T002 leaves it width-only.
void fixture1_constants_only() {
  Image image;
  Asm a{image, entry};
  a.move_b_imm_ram(2, S).move_b_imm_ram(4, S);
  const auto jmp = a.dispatch_abs(S);
  table_and_targets(image, 6U);
  const auto t002 = run(image, Mode::off);
  expect(outcome(t002, jmp, GenesisPcIndexOutcome::width_only_domain), "F1: T002 leaves the state-byte dispatch width-only");
  const auto classify = run(image, Mode::classify);
  expect(classify.discovered == t002.discovered && outcome(classify, jmp, GenesisPcIndexOutcome::width_only_domain) &&
             site(classify, jmp)->source_kinds == (1U << static_cast<unsigned>(GenesisStateSourceKind::absolute_ram)),
         "F1: classify mode records an absolute_ram source and changes nothing");
  const auto r = run(image);
  expect(resolved_to(r, jmp, {0, 2, 4}), "F1: constants-only state byte resolves to exactly {0,2,4}");
  const auto *loc = location(r, S);
  expect(loc != nullptr && loc->outcome == GenesisStateSourceOutcome::resolved && loc->values == std::vector<std::uint32_t>{0, 2, 4} &&
             loc->exact_writers == 2U && loc->zero_only_from_reset,
         "F1: the location record: two exact writers; 0 only from the reset RAM model");
  expect(!r.discovered.contains(target(1)) && !r.discovered.contains(target(3)) && !r.discovered.contains(target(5)),
         "F1: no other table entry becomes a target");
}

// F2: constant + register-guarded bounded increment (S = S + 4, reset to 0 above 8) -> exactly {0,4,8}.
// F12: the same update without the guard wraps through the byte -> unbounded_update (unresolved).
void fixture2_bounded_and_unbounded_update() {
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(4, S).move_b_ram(0, S).addq_b(4, 0).cmpi_b(0, 8);
    const auto ok = a.pc + 4U;
    a.bcc_s(LS, ok).moveq(0, 0);
    a.move_b_reg_ram(0, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 13U);
    const auto r = run(image);
    expect(resolved_to(r, jmp, {0, 4, 8}), "F2: guarded increment resolves to exactly {0,4,8}");
    const auto *loc = location(r, S);
    expect(loc != nullptr && loc->update_iterations >= 3U && !loc->zero_only_from_reset,
           "F2: a self-update iterates to its fixed point; 0 is also stored");
  }
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(4, S).addq_b_ram(4, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 13U);
    const auto r = run(image);
    expect(outcome(r, jmp, GenesisPcIndexOutcome::width_only_domain) &&
               source(r, jmp, GenesisStateSourceOutcome::unbounded_update),
           "F12: an unguarded ADDQ.B update exceeds the bound: unresolved");
  }
  {  // a bounded read-modify-write toggle converges: {0,4} closed under EORI #4
    Image image;
    Asm a{image, entry};
    a.eori_b_ram(4, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    expect(resolved_to(run(image), jmp, {0, 4}), "F2b: EORI toggle on the state byte resolves to exactly {0,4}");
  }
}

// F3: an additional store through an unknown address register with an unknown value may alias S: unresolved.
// Its measurement variants attribute the poison (unknown base) without changing the strict result.
void fixture3_alias_poison() {
  Image image;
  Asm a{image, entry};
  a.move_b_imm_ram(2, S).movea_l_ram(1, U).move_b_ram(1, U).move_b_d1_to_a1();
  const auto jmp = a.dispatch_abs(S);
  table_and_targets(image, 6U);
  const auto r = run(image);
  expect(outcome(r, jmp, GenesisPcIndexOutcome::width_only_domain) && source(r, jmp, GenesisStateSourceOutcome::alias_poison),
         "F3: a possibly aliasing unresolved store keeps the site unresolved");
  const auto *loc = location(r, S);
  expect(loc != nullptr && loc->poison_classes == (1U << static_cast<unsigned>(GenesisStoreClass::register_relative)),
         "F3: the poison is attributed to an unknown-base register-relative store");
  expect(r.stores_by_class[static_cast<std::size_t>(GenesisStoreClass::register_relative)] == 1U,
         "F3: the store table counts it");
  const auto ceiling = run(image, Mode::prove, GenesisStoreAliasPolicy::exclude_unresolved);
  expect(resolved_to(ceiling, jmp, {0, 2}), "F3: only the unsound exclude-unresolved variant ignores it");
}

// F4: an unresolved-looking store whose base is exactly proven locally (LEA of another address) is disjoint.
// F11: an object-relative field with an exactly proven base resolves like an absolute location.
void fixture4_11_exact_bases() {
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, S).lea_ram(1, 0xF180U).move_b_ram(1, U).move_b_d1_to_a1();
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    const auto r = run(image);
    expect(resolved_to(r, jmp, {0, 2}), "F4: a store through an exactly based An to another byte does not poison");
    expect(r.stores_by_class[static_cast<std::size_t>(GenesisStoreClass::exact_address)] == 2U,
           "F4: that store is an exact-address store");
  }
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, 0xF104U).lea_ram(0, S);
    const auto jmp = a.dispatch_a0(4);
    table_and_targets(image, 6U);
    const auto r = run(image);
    expect(resolved_to(r, jmp, {0, 2}), "F11: 4(A0) with A0 = LEA S resolves through location S+4");
    expect(site(r, jmp)->source_kinds == (1U << static_cast<unsigned>(GenesisStateSourceKind::register_relative)),
           "F11: classified register_relative");
  }
}

// F5: a store in unreachable bytes never participates; F10: an unknown object base is unresolved.
void fixture5_10_unreachable_and_unknown_base() {
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    Asm island{image, 0x800U};
    island.movea_l_ram(1, U).move_b_ram(1, U).move_b_d1_to_a1().move_b_imm_ram(5, S).bra_self();
    const auto r = run(image);
    expect(resolved_to(r, jmp, {0, 2}), "F5: unreachable poisoning and constant stores do not participate");
  }
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, 0xF104U).movea_l_ram(0, U);
    const auto jmp = a.dispatch_a0(4);
    table_and_targets(image, 6U);
    const auto r = run(image);
    expect(outcome(r, jmp, GenesisPcIndexOutcome::width_only_domain) && source(r, jmp, GenesisStateSourceOutcome::base_unknown),
           "F10: an object-relative field with an unknown base stays unresolved");
  }
}

// F6/F7: a proven domain {0,2} recovers targets; target(2)'s code contains a new possibly aliasing store with an
// unknown value. The re-evaluated domain is poisoned, the site is invalidated, discovery restarts with it pinned,
// and the stale targets leave D.
void fixture6_7_invalidation() {
  Image image;
  Asm a{image, entry};
  a.move_b_imm_ram(2, S);
  const auto jmp = a.dispatch_abs(S);
  table_and_targets(image, 6U);
  Asm poison{image, target(2)};
  poison.movea_l_ram(1, U).move_b_ram(1, U).move_b_d1_to_a1().bra_self();
  const auto r = run(image);
  expect(outcome(r, jmp, GenesisPcIndexOutcome::invalidated), "F6: the newly discovered writer invalidates the proof");
  expect(r.recovery_restarts == 1U && r.store_domain_invalidations == 1U, "F7: exactly one deterministic restart");
  expect(!r.discovered.contains(target(0)) && !r.discovered.contains(target(2)), "F7: stale recovered targets leave D");
  const auto again = run(image);
  expect(again.discovered == r.discovered && again.recovery_restarts == 1U, "F7: the restart is deterministic");
}

// F8: an exact value copied through a register; F9: a copy from another exactly proven state source.
// F13: a writer storing an Unknown value leaves the whole domain unresolved.
void fixture8_9_13_copies() {
  {
    Image image;
    Asm a{image, entry};
    a.moveq(3, 4).move_b_reg_ram(3, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    expect(resolved_to(run(image), jmp, {0, 4}), "F8: MOVEQ -> Dn -> S resolves");
  }
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, T).move_b_ram(3, T).move_b_reg_ram(3, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    const auto r = run(image);
    expect(resolved_to(r, jmp, {0, 2}), "F9: S copied from T (constants only) resolves");
    expect(location(r, T) != nullptr && location(r, T)->outcome == GenesisStateSourceOutcome::resolved,
           "F9: the source location T is itself resolved");
  }
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, S).move_w_ram(3, U).move_b_reg_ram(3, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    const auto r = run(image);
    expect(outcome(r, jmp, GenesisPcIndexOutcome::width_only_domain) &&
               source(r, jmp, GenesisStateSourceOutcome::writer_value_unknown),
           "F13: one Unknown-valued writer leaves the domain unresolved");
  }
}

// Stack and interrupt frames: a push of an Unknown value through A7, and an installed interrupt handler (its entry
// stacks a frame), each poison under the strict policy; the exclude-stack variant measures that class alone.
void fixture_stack_and_frames() {
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, S).move_w_ram(1, U).move_l_d1_push();
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    const auto r = run(image);
    expect(source(r, jmp, GenesisStateSourceOutcome::alias_poison) &&
               location(r, S)->poison_classes == (1U << static_cast<unsigned>(GenesisStoreClass::stack)),
           "STACK: an Unknown push through A7 poisons under strict");
    expect(resolved_to(run(image, Mode::prove, GenesisStoreAliasPolicy::exclude_stack), jmp, {0, 2}),
           "STACK: the exclude-stack variant attributes it");
  }
  {
    Image image;
    image.put32(0x78U, 0x900U);  // IRQ6 (vector 30) handler
    Asm{image, 0x900U}.rte();
    Asm a{image, entry};
    a.move_b_imm_ram(2, S);
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 6U);
    const auto r = run(image);
    expect(source(r, jmp, GenesisStateSourceOutcome::alias_poison) &&
               (location(r, S)->poison_classes & (1U << static_cast<unsigned>(GenesisStoreClass::exception_frame))) != 0U,
           "FRAME: an interrupt entry's stacked frame poisons under strict");
  }
}

// F14: the engine's only inputs are the image and the configuration (no coverage, hints or identities); outputs are
// deterministic, the default is off (T002 output unchanged) and the aggregate carries no address.
void fixture14_inputs_default_privacy() {
  Image image;
  Asm a{image, entry};
  a.move_b_imm_ram(2, S);
  const auto jmp = a.dispatch_abs(S);
  table_and_targets(image, 6U);
  const auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  GenesisReachabilityChallengerConfig off{};
  off.pc_index_recovery = true;
  expect(off.store_provenance == Mode::off, "F14: store provenance is off by default");
  const auto base = run_genesis_reachability_challenger(*program, off);
  expect(outcome(base, jmp, GenesisPcIndexOutcome::width_only_domain) && base.state_sources.empty(),
         "F14: with it off, the T002 result stands");
  const auto aggregate_off = format_genesis_reachability_challenger_aggregate(base, off);
  expect(aggregate_off.find("store_provenance") == std::string::npos && aggregate_off.find("store_domain") == std::string::npos,
         "F14: the T002 aggregate is unchanged when off");
  GenesisReachabilityChallengerConfig on = off;
  on.store_provenance = Mode::prove;
  const auto x = run_genesis_reachability_challenger(*program, on);
  const auto y = run_genesis_reachability_challenger(*program, on);
  expect(format_genesis_reachability_challenger_private(x, on) == format_genesis_reachability_challenger_private(y, on),
         "F14: deterministic private output");
  const auto aggregate = format_genesis_reachability_challenger_aggregate(x, on);
  expect(aggregate.find("\"store_provenance\":{\"mode\":\"prove\"") != std::string::npos &&
             aggregate.find("\"state_location_outcomes\":{\"resolved\":1") != std::string::npos,
         "F14: aggregate store-provenance counts");
  expect(aggregate.find("f100") == std::string::npos && aggregate.find("F100") == std::string::npos &&
             aggregate.find("000400") == std::string::npos && aggregate.find("0x") == std::string::npos,
         "F14: the aggregate carries no address");
}

// CORRECTION: a `.W` index loaded by MOVE.L from an immutable table is the entry's LOW word (big-endian: the higher
// address). Before SEG-026-T003 the narrow query read the high word.
void fixture_narrow_query_of_long_load() {
  Image image;
  Asm a{image, entry};
  a.moveq(1, 4);
  a.w(0x203BU).index_ext(1, table);  // MOVE.L (tbl,PC,D1.W),D0
  const auto jmp = a.pc;
  a.w(0x4EFBU).index_ext(0, table);  // JMP (tbl,PC,D0.W)
  image.words(table + 4U, {0x0001U, static_cast<std::uint16_t>(target(0) - table)});
  Asm{image, target(0)}.bra_self();
  const auto r = run(image, Mode::off);
  expect(resolved_to(r, jmp, {0}), "CORR: the low word of a long table entry names the target");
}

// CORRECTION 1 (validator): a destination based on the An that the source operand steps is not at An's prior value.
// LEA S-1,A1; MOVE.B (A1)+,(A1) really stores into S: it must stay unresolved, never exclude the writer.
// Also pinned: a predecrement store's span and the big-endian byte offsets of a word store covering the byte.
void fixture_store_addresses() {
  {
    Image image;
    Asm a{image, entry};
    a.move_b_imm_ram(2, S).move_b_imm_ram(8, S - 1U).lea_ram(1, S - 1U).w(0x1299U);  // MOVE.B (A1)+,(A1)
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 10U);
    const auto r = run(image);
    expect(outcome(r, jmp, GenesisPcIndexOutcome::width_only_domain) && source(r, jmp, GenesisStateSourceOutcome::alias_poison),
           "SAME-AN: a stepped-source destination is unresolved, so it poisons");
  }
  for (const auto &[base, values] : {std::pair<std::uint16_t, std::vector<unsigned>>{S + 1U, {0, 6}},
                                    std::pair<std::uint16_t, std::vector<unsigned>>{S + 2U, {0}}}) {
    Image image;
    Asm a{image, entry};
    a.lea_ram(1, base).w(0x133CU).w(6U);  // MOVE.B #6,-(A1)
    const auto jmp = a.dispatch_abs(S);
    table_and_targets(image, 10U);
    const auto r = run(image);
    const auto *s0 = site(r, jmp);
    std::vector<std::uint32_t> expected;
    for (const auto v : values) expected.push_back(target(v));
    expect(s0 != nullptr && s0->outcome == GenesisPcIndexOutcome::resolved && s0->targets == expected,
           "PREDEC: -(A1) writes the byte below A1 exactly");
  }
  {
    constexpr std::uint16_t odd = 0xF105U;  // the low byte of the word at 0xF104
    Image image;
    Asm a{image, entry};
    a.w(0x31FCU).w(0x0206U).w(0xF104U);  // MOVE.W #$0206,($F104).W -> byte 0xF105 = 6 (big-endian)
    a.w(0x31FCU).w(0x0408U).w(0xF106U);  // MOVE.W #$0408,($F106).W -> does not cover 0xF105
    const auto jmp = a.dispatch_abs(odd);
    table_and_targets(image, 10U);
    expect(resolved_to(run(image), jmp, {0, 6}), "ENDIAN: a word store covers its low byte at the higher address");
  }
}

}  // namespace

int main() {
  fixture_narrow_query_of_long_load();
  fixture_store_addresses();
  fixture1_constants_only();
  fixture2_bounded_and_unbounded_update();
  fixture3_alias_poison();
  fixture4_11_exact_bases();
  fixture5_10_unreachable_and_unknown_base();
  fixture6_7_invalidation();
  fixture8_9_13_copies();
  fixture_stack_and_frames();
  fixture14_inputs_default_privacy();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "reachability_store_provenance_tests: OK\n";
  return EXIT_SUCCESS;
}
