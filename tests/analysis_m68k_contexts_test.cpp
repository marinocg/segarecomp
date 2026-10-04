// SEG-030-T005 (ADR 0079 decisions 6, 9, 10, 11): bounded call contexts (call string k = 1, K contexts per callee entry) and callee
// summaries with register AND memory effects, applied to call continuations only when proven (balanced stack, every return an RTS at
// the entry stack delta, no unknown effect) and validated against the round's own solution.
//
// Project-authored synthetic fixtures only: real MC68000 encodings decoded by the unchanged decoder/lifter over a small flat cartridge
// image plus a work-RAM region extent, so exact synthetic addresses may be asserted. No commercial input.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"

namespace {

using namespace segarecomp;
using analysis::FiniteValue;
using analysis::UnknownReason;
using Sub = M68kAnalysisSubReason;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t object_a = 0x00FF0100U;
constexpr std::uint32_t object_b = 0x00FF0200U;

struct Asm {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(image_size, 0U);
  std::uint32_t pc = entry;
  Asm &w(std::initializer_list<std::uint32_t> values) {
    for (const auto value : values) {
      bytes[pc] = static_cast<std::uint8_t>(value >> 8U);
      bytes[pc + 1U] = static_cast<std::uint8_t>(value);
      pc += 2U;
    }
    return *this;
  }
  Asm &at(std::uint32_t address) {
    pc = address;
    return *this;
  }
  Asm &l(std::uint32_t value) { return w({value >> 16U, value & 0xFFFFU}); }
  // BSR.W target; returns the call site.
  std::uint32_t bsr(std::uint32_t target) {
    const auto site = pc;
    w({0x6100U, (target - (pc + 2U)) & 0xFFFFU});
    return site;
  }
  Asm &branch(std::uint32_t opcode, std::uint32_t target) {  // Bcc.S
    const auto displacement = static_cast<std::uint32_t>(static_cast<std::int32_t>(target) - static_cast<std::int32_t>(pc + 2U));
    return w({opcode | (displacement & 0xFFU)});
  }
  Asm &rts() { return w({0x4E75U}); }
  Asm &moveq(unsigned reg, std::uint32_t value) { return w({0x7000U | (reg << 9U) | (value & 0xFFU)}); }
  Asm &lea(unsigned reg, std::uint32_t address) { return w({0x41F9U | (reg << 9U)}).l(address); }  // LEA (xxx).L,An
  // JMP 2(PC,Dn.W): the targets are the site + 4 + Dn.
  std::uint32_t dispatch(unsigned reg = 0U) {
    const auto site = pc;
    w({0x4EFBU, (reg << 12U) | 0x0002U});
    return site;
  }
  Asm &stop() { return w({0x60FEU}); }  // BRA.S *
  Asm &stack() { return w({0x2E7CU}).l(0x00FFFF00U); }  // MOVEA.L #$00FFFF00,A7 (a known stack: pushes never alias an object)
};

class RegionImage final : public M68kAnalysisImage {
public:
  explicit RegionImage(const Asm &program) : flat_(program.bytes, 0U) {}
  [[nodiscard]] std::optional<Instruction> decode(std::uint32_t pc) const override { return flat_.decode(pc); }
  [[nodiscard]] bool mapped(std::uint32_t pc) const override { return flat_.mapped(pc); }
  [[nodiscard]] std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) const override {
    return flat_.immutable_read(address, bytes);
  }
  [[nodiscard]] std::optional<M68kRegionExtent> region_of(std::uint32_t address) const override {
    if (address >= work_ram_base && address < 0x1000000U)
      return M68kRegionExtent{M68kRegionKind::work_ram, 0U, work_ram_base, 0x1000000U - work_ram_base, 0x10000U};
    return flat_.region_of(address);
  }

private:
  M68kFlatAnalysisImage flat_;
};

M68kFiniteAnalysisResult run(const Asm &program, bool contexts = true, std::uint32_t round_bound = m68k_memory_round_bound) {
  const RegionImage view{program};
  M68kAnalysisConfig config{};
  config.domains.address = true;
  config.domains.memory = true;
  config.domains.contexts = contexts;
  config.contexts.round_bound = round_bound;
  return analyze_m68k_finite_values(view, {entry}, config);
}

const M68kPcIndexSiteReport *pc_site(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto found = result.pc_index_sites.find(pc);
  return found == result.pc_index_sites.end() ? nullptr : &found->second;
}

bool resolved(const M68kFiniteAnalysisResult &result, std::uint32_t pc, const std::vector<std::uint32_t> &targets) {
  const auto *site = pc_site(result, pc);
  return result.complete && site != nullptr && site->outcome == M68kPcIndexOutcome::resolved && site->targets == targets;
}

bool unresolved(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto *site = pc_site(result, pc);
  return result.complete && site != nullptr && site->outcome != M68kPcIndexOutcome::resolved && site->targets.empty();
}

std::string text(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto *site = pc_site(result, pc);
  if (site == nullptr) return "no site";
  std::string out = m68k_pc_index_outcome_name(site->outcome);
  if (site->outcome != M68kPcIndexOutcome::resolved) out += std::string("/") + analysis::unknown_reason_name(site->reason);
  for (const auto target : site->targets) out += " " + std::to_string(target);
  return out;
}

// The index D`reg`.W immediately before `pc`, joined over every context reaching it.
FiniteValue data_before(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg) {
  FiniteValue out;
  for (const auto &[point, state] : m68k_points_of(result, pc)) {
    (void)point;
    out = join(out, state->values.values[m68k_analysis_slot(reg, 16U)]);
  }
  return out;
}

std::size_t opaque(const M68kFiniteAnalysisResult &result, Sub sub) {
  const auto found = result.contexts.opaque_continuations.find(sub);
  return found == result.contexts.opaque_continuations.end() ? 0U : found->second;
}

bool within_bounds(const M68kFiniteAnalysisResult &result) {
  return result.contexts.max_contexts_per_callee <= m68k_context_bound && result.solution.in_states.size() <= analysis::default_max_points;
}

// ---------------------------------------------------------------------------------------------------------------

void two_callers() {
  // Two callers store a different handler into their own object's field, then call the same routine with A0 = the object. The
  // routine loads the field and calls it. Context-free, the routine's entry joins both objects (no common field cell): Unknown.
  Asm a;
  constexpr std::uint32_t routine = 0x400U, handler_a = 0x480U, handler_b = 0x490U;
  a.stack().lea(0, object_a).w({0x217CU}).l(handler_a).w({0x0010U});  // MOVE.L #handler_a,$10(A0)
  const auto first = a.bsr(routine);
  a.lea(0, object_b).w({0x217CU}).l(handler_b).w({0x0010U});
  const auto second = a.bsr(routine);
  a.stop();
  a.at(routine).w({0x2268U, 0x0010U});  // MOVEA.L $10(A0),A1
  const auto site = a.pc;
  a.w({0x4E91U}).rts();  // JSR (A1); RTS
  a.at(handler_a).rts();
  a.at(handler_b).w({0x4E71U}).rts();
  const auto result = run(a);
  if (std::getenv("CONTEXTS_DEBUG") != nullptr) std::cerr << format_m68k_finite_analysis(result);
  const auto found = result.address_sites.find(site);
  expect(result.complete && found != result.address_sites.end() && found->second.resolved &&
             found->second.targets == std::vector<std::uint32_t>{handler_a, handler_b},
         "two callers: the handler site resolves per context (union of the per-context targets)");
  bool per_context = true;
  std::size_t contexts = 0U;
  for (const auto &[point, state] : m68k_points_of(result, site)) {
    ++contexts;
    const auto expected = m68k_point_context(point) == m68k_call_context(first) ? handler_a : handler_b;
    per_context = per_context && m68k_point_context(point) != 0U && state->address[1].values() == std::vector<std::uint32_t>{expected};
  }
  expect(per_context && contexts == 2U, "two callers: each context keeps its own object's handler in A1");
  expect(m68k_points_of(result, routine).size() == 2U && m68k_point_context(m68k_points_of(result, routine)[1].first) ==
                                                              m68k_call_context(second),
         "two callers: the same callee is analysed once per call-site context");
  const auto free = run(a, false);
  const auto unresolved_site = free.address_sites.find(site);
  expect(free.complete && unresolved_site != free.address_sites.end() && !unresolved_site->second.resolved,
         "two callers: without contexts the joined objects leave the handler Unknown");
  expect(result.contexts.sites_resolved_only_with_contexts >= 1U && within_bounds(result),
         "two callers: the comparator counts the context-only resolution");
}

void balanced_return() {
  // MOVEQ #4,D0; BSR leaf; JMP 2(PC,D0.W) - the ADR 0054 index reached through a return continuation. The leaf (MOVEQ #1,D1; RTS)
  // is balanced and leaves D0: its summary keeps the caller's index.
  Asm a;
  constexpr std::uint32_t leaf = 0x400U;
  a.moveq(0, 4U);
  a.bsr(leaf);
  const auto site = a.dispatch();
  a.w({0x4E71U, 0x4E71U}).stop();
  a.at(leaf).moveq(1, 1U).rts();
  const auto result = run(a);
  expect(resolved(result, site, {site + 8U}), "balanced return: the summary keeps the index across the call: " + text(result, site));
  expect(result.contexts.validated && result.contexts.converged && result.contexts.summary_continuations >= 1U,
         "balanced return: a validated, converged round applied the summary");
  const auto free = run(a, false);
  const auto *before = pc_site(free, site);
  expect(unresolved(free, site) && before->outcome == M68kPcIndexOutcome::index_unknown &&
             before->reason == UnknownReason::unknown_input,
         "balanced return: without contexts the opaque continuation leaves index_unknown/unknown_input: " + text(free, site));
}

void callee_field_clobber() {
  // LEA obj,A0; MOVE.B #0,$10(A0); BSR clobber; MOVEQ #0,D0; MOVE.B $10(A0),D0; JMP 2(PC,D0.W). The callee may store 8 into the
  // field (TST.B D1; BEQ skip; MOVE.B #8,$10(A0); skip: RTS): the summary carries the callee's memory effect, never the pre-call cell.
  Asm a;
  constexpr std::uint32_t clobber = 0x400U;
  a.stack().lea(0, object_a).w({0x117CU, 0x0000U, 0x0010U});
  a.bsr(clobber);
  a.moveq(0, 0U).w({0x1028U, 0x0010U});
  const auto site = a.dispatch();
  a.at(clobber).w({0x4A01U});
  const auto beq = a.pc;
  a.w({0x6700U}).w({0x117CU, 0x0008U, 0x0010U});
  const auto skip = a.pc;
  a.rts();
  a.at(beq).branch(0x6700U, skip);
  auto result = run(a);
  expect(resolved(result, site, {site + 4U, site + 12U}),
         "field clobber: the summary joins the callee's paths (field 0 or 8): " + text(result, site));
  // An unconditional clobber: only the callee's value.
  a.at(beq).w({0x4E71U});
  result = run(a);
  expect(resolved(result, site, {site + 12U}), "field clobber: a strong callee store replaces the caller's cell: " + text(result, site));
}

void unbalanced_stack() {
  // The callee drops its return address (ADDQ.L #4,A7; RTS returns to the caller's caller): never a summary. Its caller is unproven
  // too (an unbalanced nested activation).
  Asm a;
  constexpr std::uint32_t outer = 0x400U, inner = 0x480U;
  a.moveq(0, 4U);
  a.bsr(inner);
  const auto site = a.dispatch();
  a.at(0x300U);
  const auto via_outer = a.pc;
  a.moveq(0, 4U);
  a.bsr(outer);
  const auto outer_site = a.dispatch();
  a.at(outer).bsr(inner);
  a.rts();
  a.at(inner).w({0x588FU}).rts();  // ADDQ.L #4,A7; RTS
  M68kAnalysisConfig config{};
  config.domains.address = config.domains.memory = config.domains.contexts = true;
  const RegionImage view{a};
  const auto result = analyze_m68k_finite_values(view, {entry, via_outer}, config);
  expect(unresolved(result, site) && data_before(result, site, 0U) == FiniteValue::unknown(UnknownReason::unsupported_transfer),
         "unbalanced: the continuation is opaque Unknown(stack_unbalanced): " + text(result, site));
  expect(unresolved(result, outer_site), "unbalanced: a caller of an unbalanced callee is unproven: " + text(result, outer_site));
  const auto found = result.contexts.unproven.find(Sub::stack_unbalanced);
  expect(found != result.contexts.unproven.end() && found->second == 3U && opaque(result, Sub::stack_unbalanced) == 3U,
         "unbalanced: both callee contexts and their caller are reported stack_unbalanced");
}

void unknown_callee_effect() {
  // An unresolved JSR (A1) (A1 from an Unknown D7) and a callee whose body leaves through an unresolved JMP (A1): opaque (T004).
  Asm a;
  constexpr std::uint32_t escaping = 0x400U;
  a.moveq(0, 4U).w({0x2247U, 0x4E91U});  // MOVEA.L D7,A1; JSR (A1)
  const auto site = a.dispatch();
  a.at(0x300U);
  const auto second_root = a.pc;
  a.moveq(0, 4U);
  a.bsr(escaping);
  const auto escaping_site = a.dispatch();
  a.at(escaping).w({0x4A01U});
  const auto beq = a.pc;
  a.w({0x6700U, 0x2247U, 0x4ED1U});  // BEQ done; MOVEA.L D7,A1; JMP (A1)
  const auto done = a.pc;
  a.rts();
  a.at(beq).branch(0x6700U, done);
  M68kAnalysisConfig config{};
  config.domains.address = config.domains.memory = config.domains.contexts = true;
  const RegionImage view{a};
  const auto result = analyze_m68k_finite_values(view, {entry, second_root}, config);
  expect(unresolved(result, site) && pc_site(result, site)->reason == UnknownReason::unknown_input,
         "unknown effect: an unresolved callee keeps the opaque continuation: " + text(result, site));
  expect(unresolved(result, escaping_site) && pc_site(result, escaping_site)->reason == UnknownReason::unknown_input,
         "unknown effect: a callee with an unresolved exit has no summary: " + text(result, escaping_site));
  expect(opaque(result, Sub::none) >= 2U && result.contexts.summary_continuations == 0U,
         "unknown effect: both continuations stay opaque");
}

void recursion() {
  // rec: TST.B D1; BEQ done; BSR rec; done: RTS - the recursive activation is never proven (context_bound, generic state_bound).
  Asm a;
  constexpr std::uint32_t rec = 0x400U;
  a.moveq(0, 4U);
  a.bsr(rec);
  const auto site = a.dispatch();
  a.at(rec).w({0x4A01U});
  const auto beq = a.pc;
  a.w({0x6700U});
  a.bsr(rec);
  const auto done = a.pc;
  a.rts();
  a.at(beq).branch(0x6700U, done);
  const auto result = run(a);
  expect(unresolved(result, site) && pc_site(result, site)->reason == UnknownReason::state_bound,
         "recursion: the caller's continuation is Unknown(state_bound): " + text(result, site));
  expect(result.contexts.recursive_activations >= 1U && opaque(result, Sub::context_bound) >= 1U && within_bounds(result),
         "recursion: the recursive activation is reported context_bound");
}

void context_exhaustion() {
  // MOVEQ #4,D0; n x BSR g; JMP 2(PC,D0.W) with g: RTS. With n = K the summaries carry D0; with n = K + 1 the callee is merged into
  // context 0 and its continuations are Unknown(context_bound).
  // ADR 0079 decision 11 fixes K = 8: the fixture uses the literal so a raised bound is observable (SEG-030-T008).
  static constexpr std::size_t adr_k = 8U;
  for (const std::size_t n : {adr_k, adr_k + 1U}) {
    Asm a;
    constexpr std::uint32_t g = 0x400U;
    a.moveq(0, 4U);
    for (std::size_t i = 0; i < n; ++i) a.bsr(g);
    const auto site = a.dispatch();
    a.w({0x4E71U, 0x4E71U}).stop();
    a.at(g).rts();
    const auto result = run(a);
    if (n == adr_k) {
      expect(resolved(result, site, {site + 8U}) && result.contexts.max_contexts_per_callee == n && result.contexts.merged_callees == 0U,
             "context bound: K contexts keep their summaries: " + text(result, site));
    } else {
      expect(unresolved(result, site) && pc_site(result, site)->reason == UnknownReason::state_bound &&
                 result.contexts.merged_callees == 1U && opaque(result, Sub::context_bound) == n && within_bounds(result),
             "context bound: K + 1 call sites merge the callee (Unknown(state_bound), context_bound): " + text(result, site));
      for (const auto &[point, state] : m68k_points_of(result, g)) {
        (void)state;
        expect(m68k_point_context(point) == 0U, "context bound: the merged callee is analysed in context 0 only");
      }
    }
  }
}

// The summary-invalidation program: a used summary becomes stale when a later round discovers a new writer in the callee.
//   entry: BSR getsel (MOVEQ #0,D2; RTS); MOVEQ #4,D3; LEA normal,A1; TST.B D1; BEQ via
//   call:  BSR dispatch_routine; X: JMP 2(PC,D3.W)
//   via:   Y: JMP 2(PC,D2.W) -> LEA writer,A1; BRA call     (Y resolves only once getsel's summary is applied)
//   dispatch_routine: JSR (A1); RTS     normal: RTS     writer: MOVEQ #8,D3; RTS (the newly discovered writer of D3)
struct Invalidation {
  Asm program;
  std::uint32_t x{};
  std::uint32_t y{};
};

Invalidation invalidation_program() {
  Invalidation out;
  auto &a = out.program;
  constexpr std::uint32_t getsel = 0x400U, routine = 0x440U, normal = 0x480U, writer = 0x4C0U;
  a.bsr(getsel);
  a.moveq(3, 4U).lea(1, normal).w({0x4A01U});
  const auto beq = a.pc;
  a.w({0x6700U});
  const auto call = a.pc;
  a.bsr(routine);
  out.x = a.dispatch(3U);
  a.w({0x4E71U, 0x4E71U}).stop().w({0x4E71U}).stop();  // X targets: x + 8 (D3 = 4), x + 12 (D3 = 8)
  const auto via = a.pc;
  out.y = a.dispatch(2U);
  a.lea(1, writer);  // Y's target (D2 = 0)
  a.branch(0x6000U, call);
  a.at(beq).branch(0x6700U, via);
  a.at(getsel).moveq(2, 0U).rts();
  a.at(routine).w({0x4E91U}).rts();
  a.at(normal).rts();
  a.at(writer).moveq(3, 8U).rts();
  return out;
}

void summary_invalidation() {
  const auto program = invalidation_program();
  const auto result = run(program.program);
  expect(resolved(result, program.y, {program.y + 4U}), "invalidation: Y resolves from getsel's summary: " + text(result, program.y));
  expect(resolved(result, program.x, {program.x + 8U, program.x + 12U}) &&
             data_before(result, program.x, 3U) == FiniteValue::of({4U, 8U}),
         "invalidation: the summary is regrown with the newly discovered writer (no stale D3 = 4): " + text(result, program.x));
  expect(result.contexts.validated && result.contexts.converged && result.contexts.returned_round == result.contexts.rounds &&
             result.contexts.rounds >= 3U,
         "invalidation: the stale rounds fail validation and the run converges later");
  // With fewer rounds than the invalidation needs, only a validated round is returned: never the stale summary's precise D3 = 4.
  for (std::uint32_t bound = 1U; bound < result.contexts.rounds; ++bound) {
    const auto bounded = run(program.program, true, bound);
    const auto d3 = data_before(bounded, program.x, 3U);
    const bool stale = d3.is_precise() && d3 != FiniteValue::of({4U, 8U});
    expect(bounded.complete && bounded.contexts.validated && !bounded.contexts.converged && !stale &&
               !resolved(bounded, program.x, {program.x + 8U}),
           "invalidation: a round bound of " + std::to_string(bound) + " returns a validated round, never the stale summary: " +
               text(bounded, program.x) + " D3=" + d3.describe());
  }
}

void handler_writer_in_callee() {
  // A field dispatch whose field is also written by a subroutine of a (synchronous) handler root: the store executes in a callee
  // context only, and it still makes the field asynchronous (the writer set covers every context).
  Asm a;
  a.lea(0, object_a).w({0x4A01U});  // TST.B D1
  const auto beq = a.pc;
  a.w({0x6700U, 0x117CU, 0x0000U, 0x0010U});  // BEQ other; MOVE.B #0,$10(A0)
  const auto bra = a.pc;
  a.w({0x6000U});
  const auto other = a.pc;
  a.w({0x117CU, 0x0004U, 0x0010U});
  const auto joined = a.pc;
  a.moveq(0, 0U).w({0x1028U, 0x0010U});
  const auto site = a.dispatch();
  a.w({0x4E71U, 0x4E71U, 0x4E71U}).stop();
  a.at(beq).branch(0x6700U, other);
  a.at(bra).branch(0x6000U, joined);
  // The handler reaches its writer only through a dispatch that resolves from a callee summary (D2 = 0 from `select`): code that
  // only the contexts domain discovers.
  constexpr std::uint32_t handler = 0x600U, writer = 0x680U, select = 0x6C0U;
  a.at(handler).stack().bsr(select);  // a known handler stack: the return-address push is not an Unknown-target store
  a.dispatch(2U);
  a.bsr(writer);
  a.w({0x4E73U});  // RTE
  a.at(select).moveq(2, 0U).rts();
  a.at(writer).w({0x13FCU, 0x0008U}).l(object_a + 0x10U).rts();  // MOVE.B #8,(field).L; RTS
  const RegionImage view{a};
  M68kAnalysisConfig config{};
  config.domains.address = config.domains.memory = config.domains.contexts = true;
  config.memory.handler_roots = {handler};
  const auto result = analyze_m68k_finite_values(view, {entry, handler}, config);
  // The field's physical cell: object_a + $10 folded into the 64 KiB work-RAM mirror.
  const M68kCell field{M68kRegionKind::work_ram, 0U, (object_a + 0x10U - work_ram_base) % 0x10000U, 1U};
  expect(result.reached.contains(writer) && unresolved(result, site) && !result.memory.policy.async_all &&
             result.memory.policy.asynchronous(field),
         "handler writer in a callee: the store of a callee context makes the field asynchronous: " + text(result, site));
  config.domains.contexts = false;
  expect(!analyze_m68k_finite_values(view, {entry, handler}, config).reached.contains(writer),
         "handler writer in a callee: only the contexts domain discovers the writer");
}

void stack_delta_and_inert() {
  // PEA/push/pop and MOVEM keep the stack delta exact; a balanced callee that saves D0 on a known stack, clobbers the data registers
  // (MOVEM's data-owner footprint) and restores D0 from its stack cell is summarized with the restored index.
  Asm a;
  constexpr std::uint32_t saver = 0x400U;
  a.stack().moveq(0, 4U);
  a.bsr(saver);
  const auto site = a.dispatch();
  a.w({0x4E71U, 0x4E71U}).stop();
  // MOVE.L D0,-(A7); MOVEM.L D1-D2,-(A7); PEA ($300).L; ADDQ.L #4,A7; MOVEM.L (A7)+,D1-D2; MOVE.L (A7)+,D0; RTS
  a.at(saver).w({0x2F00U, 0x48E7U, 0x6000U, 0x4879U, 0x0000U, 0x0300U, 0x588FU, 0x4CDFU, 0x0006U, 0x201FU}).rts();
  const auto result = run(a);
  if (std::getenv("CONTEXTS_DEBUG") != nullptr) std::cerr << format_m68k_finite_analysis(result);
  expect(resolved(result, site, {site + 8U}), "stack delta: pushes and pops at exact deltas keep the callee balanced: " + text(result, site));
  bool exit_zero = true;
  for (const auto &[point, state] : m68k_points_of(result, saver + 20U)) {
    (void)point;
    exit_zero = exit_zero && state->stack_delta == FiniteValue::of({0U});
  }
  expect(exit_zero, "stack delta: the RTS is reached at delta 0");
  expect(format_m68k_finite_analysis(result) == format_m68k_finite_analysis(run(a)), "determinism: two contexts runs format identically");
  const auto off = run(a, false);
  bool inert = !off.contexts.enabled;
  for (const auto &[point, state] : off.solution.in_states) inert = inert && m68k_point_context(point) == 0U && state.stack_delta.is_bottom();
  expect(inert, "inert: without the contexts domain every point is its PC and no state carries a stack delta");
}


// B1 (soundness): a resuming exception's continuation is reached only after a handler whose effect on A7 is never proven. Its stack
// delta must be Unknown (never bottom, which would join away): a leaf whose TRAP path pops its caller's return slot (ADDQ.L #4,A7)
// before its RTS is unbalanced, so neither the leaf nor its caller has a proven summary and the caller's dispatch stays Unknown.
void exception_continuation_delta() {
  const auto build = [](bool trap) {
    Asm a;
    constexpr std::uint32_t mid = 0x400U, leaf = 0x500U;
    a.stack().moveq(0, 4U);
    a.bsr(mid);
    const auto site = a.dispatch();
    a.w({0x4E71U, 0x4E71U, 0x4E71U, 0x4E71U, 0x4E71U, 0x4E71U}).stop();
    a.at(mid);
    a.bsr(leaf);
    a.moveq(0, 8U).rts();
    // TST.B D1; BEQ.S L; TRAP #0 (or NOP); ADDQ.L #4,A7 (or NOP); L: RTS
    a.at(leaf).w({0x4A01U, 0x6704U, trap ? 0x4E40U : 0x4E71U, trap ? 0x588FU : 0x4E71U}).rts();
    return std::make_pair(a, site);
  };
  const auto run_exc = [](const Asm &program) {
    const RegionImage view{program};
    M68kAnalysisConfig config{};
    config.domains.address = true;
    config.domains.memory = true;
    config.domains.contexts = true;
    config.exception_continuations = true;
    return analyze_m68k_finite_values(view, {entry}, config);
  };
  const auto [trapping, site] = build(true);
  const auto result = run_exc(trapping);
  expect(unresolved(result, site) && !data_before(result, site, 0U).is_precise(),
         "exception continuation: the TRAP path's untracked A7 leaves the dispatch Unknown: " + text(result, site) +
             " D0=" + data_before(result, site, 0U).describe());
  bool untracked = false;
  for (const auto &[point, state] : result.solution.in_states)
    if (m68k_point_pc(point) == 0x506U) untracked = untracked || state.stack_delta.is_unknown();
  expect(untracked, "exception continuation: the continuation's stack delta is Unknown, never bottom");
  // Control: the same program without the TRAP path is balanced and proven.
  const auto [plain, plain_site] = build(false);
  const auto control = run_exc(plain);
  expect(resolved(control, plain_site, {plain_site + 12U}), "exception continuation control: a balanced leaf is proven: " +
                                                                 text(control, plain_site));
}

// M1: the typed per-PC queries join every context's point (a context-0 point alone is a context-restricted fact).
void queries_join_contexts() {
  Asm a;
  constexpr std::uint32_t f = 0x400U;
  a.stack().moveq(0, 1U).lea(1, object_a);
  a.bsr(f);
  a.moveq(0, 2U).lea(1, object_b).w({0x6000U, (f - (a.pc + 2U)) & 0xFFFFU});  // BRA.W f (context 0)
  a.at(f).w({0x4E71U}).rts();
  const auto result = run(a);
  expect(result.complete && m68k_points_of(result, f).size() >= 2U, "queries: the callee is reached in two contexts");
  const auto d0 = m68k_query_data_register(result, f, 0U, 16U);
  expect(d0 == FiniteValue::of({1U, 2U}), "queries: D0 joins every context: " + d0.describe());
  const auto a1 = m68k_query_address_register(result, f, 1U);
  expect(a1.is_known() && a1.values() == std::vector<std::uint32_t>{object_a, object_b},
         "queries: A1 joins every context: " + a1.describe());
  expect(m68k_query_data_register(result, 0x800U, 0U, 16U).is_bottom() && m68k_query_address_register(result, 0x800U, 1U).is_bottom(),
         "queries: an unreached PC is bottom");
}

}  // namespace

int main() {
  two_callers();
  balanced_return();
  callee_field_clobber();
  unbalanced_stack();
  unknown_callee_effect();
  recursion();
  context_exhaustion();
  summary_invalidation();
  handler_writer_in_callee();
  stack_delta_and_inert();
  exception_continuation_delta();
  queries_join_contexts();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_contexts_test: all checks passed\n";
  return EXIT_SUCCESS;
}
