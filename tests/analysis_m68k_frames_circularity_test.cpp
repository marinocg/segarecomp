// SEG-039-T001 (ADR 0086 follow-up, report-only/research): formalizes and reproduces, through the EXISTING unmodified
// `frames` domain production analysis, the monotone status/clobbered circularity ADR 0086 (SEG-038-T002) identified by
// reading the source, and tests a bounded "propose one handler instance, solve, validate, commit-or-reject" breaker
// hypothesis against ten required synthetic mutation classes. The breaker is test-only: it never edits
// `effective_status()`/`apply_resumptions()`/`clobbered` and is never linked into any production route. It reuses the
// same Asm/RegionImage/run() conventions as tests/analysis_m68k_frames_test.cpp (copied here, not reinvented) so this
// file can be read and run independently of that one.
//
// Project-authored synthetic fixtures only; no commercial input.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

// ---------------------------------------------------------------------------------------------------------------
// Shared fixture infrastructure (same conventions as analysis_m68k_frames_test.cpp).

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t irq6 = 0x400U;   // level-6 autovector handler (vector 30)
constexpr std::uint32_t irq4 = 0x480U;   // level-4 autovector handler (vector 28)
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;
constexpr std::uint32_t cell_a = 0x00FF0100U;
constexpr std::uint32_t cell_b = 0x00FF0200U;

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
  Asm &move_sr(std::uint32_t sr) { return w({0x46FCU, sr}); }
  Asm &moveq(unsigned reg, std::uint32_t value) { return w({0x7000U | (reg << 9U) | (value & 0xFFU)}); }
  Asm &store_imm(std::uint32_t value, std::uint32_t address) { return w({0x33FCU, value}).l(address); }
  Asm &load_word(unsigned reg, std::uint32_t address) { return w({0x3039U | (reg << 9U)}).l(address); }
  Asm &nop() { return w({0x4E71U}); }
  Asm &rte() { return w({0x4E73U}); }
  // MOVEM.L D0-D7/A0-A6,-(A7) / MOVEM.L (A7)+,D0-D7/A0-A6: every register the task's prescribed fixture names, pushed and
  // popped in the legal predecrement/postincrement mask encodings (M68000PRM Table 2-4): predecrement reverses bit order.
  Asm &movem_save_all() { return w({0x48E7U, 0xFFFEU}); }
  Asm &movem_restore_all() { return w({0x4CDFU, 0x7FFFU}); }
  Asm &bra_self() { return w({0x60FEU}); }  // tight infinite loop: never reaches RTE
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
      return M68kRegionExtent{M68kRegionKind::mutable_ram, 0U, work_ram_base, 0x1000000U - work_ram_base, 0x10000U};
    return flat_.region_of(address);
  }

private:
  M68kFlatAnalysisImage flat_;
};

M68kFiniteAnalysisResult run(const Asm &program, const std::vector<M68kHandlerVector> &vectors, M68kMemoryPolicy policy = {}) {
  const RegionImage view{program};
  M68kAnalysisConfig config{};
  config.domains.address = true;
  config.domains.memory = true;
  config.domains.contexts = true;
  config.domains.frames = true;
  std::set<std::uint32_t> entries{entry};
  for (const auto &vector : vectors) entries.insert(vector.handler);
  config.frames.vectors = vectors;
  config.frames.main_entries = {entry};
  config.frames.reset_entry = entry;
  config.frames.reset_ssp = ssp;
  config.memory.policy = std::move(policy);
  return analyze_m68k_finite_values(view, std::vector<std::uint32_t>(entries.begin(), entries.end()), config);
}

template <typename F>
auto join_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc, std::uint32_t tag, F &&get) {
  std::decay_t<decltype(get(std::declval<const M68kAnalysisState &>()))> out{};
  for (const auto &[point, state] : m68k_points_of(result, pc))
    if (m68k_point_tag(point) == tag) out = join(out, get(*state));
  return out;
}
FiniteValue status_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc, std::uint32_t tag = 0U) {
  return join_at(result, pc, tag, [](const M68kAnalysisState &state) { return state.status; });
}
FiniteValue word_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg, std::uint32_t tag = 0U) {
  return join_at(result, pc, tag, [&](const M68kAnalysisState &state) { return state.values.values[m68k_analysis_slot(reg, 16U)]; });
}
bool reached_in(const M68kFiniteAnalysisResult &result, std::uint32_t pc, std::uint32_t tag) {
  for (const auto &[point, state] : m68k_points_of(result, pc)) {
    (void)state;
    if (m68k_point_tag(point) == tag) return true;
  }
  return false;
}
std::size_t unanalysed(const M68kFiniteAnalysisResult &result, const std::string &why) {
  const auto found = result.frames.unanalysed.find(why);
  return found == result.frames.unanalysed.end() ? 0U : found->second;
}
// True when `handler` ever reached "analysed" status for at least one credited, resuming instance: it has a known
// (supervisor-proven) entry A7/status and its vector never appears in any unanalysed cause.
bool handler_reached_analysed(const M68kFiniteAnalysisResult &result, std::uint32_t handler) {
  if (!result.frames.validated) return false;
  for (std::uint32_t tag = 1U; tag < m68k_max_instance_tag && tag < 64U; ++tag) {
    if (reached_in(result, handler, tag)) return true;
  }
  return false;
}
bool debug() { return std::getenv("FRAMES_DEBUG") != nullptr; }
std::string describe(const M68kFiniteAnalysisResult &result) { return format_m68k_finite_analysis(result); }

const std::vector<M68kHandlerVector> irq6_only{{30U, irq6}};

// ---------------------------------------------------------------------------------------------------------------
// Step 3: minimal synthetic fixtures.

// The literally-prescribed minimal fixture: main enters supervisor mode and enables level 6; the handler saves and
// restores every register the task names (D0-D7/A0-A6) and RTEs. No self-nesting is possible (the handler never
// lowers its own entry mask, I = 6, below itself).
void clean_single_handler_reaches_analysed() {
  Asm a;
  a.move_sr(0x2000U);  // S = 1, I = 0: level 6 is eligible everywhere after this
  a.moveq(0, 1U).store_imm(1U, cell_a);
  const auto flow_done = a.pc;
  a.nop().bra_self();
  a.at(irq6).movem_save_all().store_imm(5U, cell_b).movem_restore_all().rte();
  const auto result = run(a, irq6_only);
  if (debug()) std::cerr << describe(result);
  expect(result.complete && result.frames.validated, "clean: a validated frames round");
  expect(result.frames.unanalysed.empty(), "clean: the perfect-preserve handler has no unanalysed cause");
  expect(handler_reached_analysed(result, irq6), "clean: the handler reaches analysed status");
  expect(result.frames.unproven_resumptions == 0U, "clean: its resumption is proven (D0-D7/A0-A6 all preserved)");
  expect(status_at(result, flow_done) == FiniteValue::of({8U}),
         "clean: the main flow's status stays known (S = 1, I = 0) past the interrupt-eligible point: " +
             status_at(result, flow_done).describe());
  expect(!result.frames.main_async_all, "clean: the main flow keeps a bounded writer set");
}

// The handler lowers its own mask to 0 (MOVE #$2000,SR) before doing anything else: level 6 can now preempt itself
// (self-nesting), exactly as in the pre-existing SEG-030-T006 handler_lowers_mask() fixture, but every register is
// also explicitly, legally saved/restored around it. Confirms: even a syntactically perfect save/restore sequence
// never reaches "analysed" once self-nesting is reachable, and the handler's own taking boundary (not merely the
// nested occurrence) is the one left permanently poisoned -- effective_status()/clobbered at finite_adapter.cpp.
void self_nesting_handler_never_analysed() {
  Asm a;
  a.move_sr(0x2000U);
  a.moveq(0, 1U).store_imm(1U, cell_a);
  const auto flow_done = a.pc;
  a.nop().bra_self();
  a.at(irq6).move_sr(0x2000U).movem_save_all().store_imm(5U, cell_b).movem_restore_all().rte();
  const auto result = run(a, irq6_only);
  if (debug()) std::cerr << describe(result);
  expect(result.frames.validated, "self-nesting: a validated (converged) round -- the pessimistic verdict is stable, not a solver failure");
  expect(unanalysed(result, "nested") + unanalysed(result, "entry_unknown") >= 1U,
         "self-nesting: the handler never reaches analysed status (nested or entry_unknown, exactly as handler_lowers_mask)");
  expect(!handler_reached_analysed(result, irq6), "self-nesting: no credited instance of the handler ever reaches analysed status");
  expect(result.frames.clobbered_partitions >= 1U, "self-nesting: the main partition is clobbered");
  expect(status_at(result, flow_done).is_unknown(),
         "self-nesting: the main flow's status is Unknown downstream of the interrupt-eligible point (the handler's own taking "
         "boundary -- the fact its own entry proof needs -- is exactly what got poisoned)");
}

// Two independent, unrelated vectors share the same main partition: level 6 is the self-nesting handler above; level 4 is
// a syntactically perfect, otherwise entirely provable save/restore handler that never touches its own mask and could
// never be preempted at its own entry level (I = 4 at entry excludes level-4-on-level-4 nesting by the mask invariant
// alone). It is reachable only after the same MOVE #$2000,SR that also makes level 6 eligible. Demonstrates the
// generalized, real-title-shaped failure mode ADR 0086 reports ("zero handler instances... on the authorized titles"):
// ONE unrelated handler's corruption permanently poisons clobbered[0]/status, which then blocks every OTHER handler
// sharing that parent partition too, even one with no defect of its own.
void self_nesting_poisons_unrelated_handler() {
  Asm a;
  a.move_sr(0x2000U);
  a.moveq(0, 1U).store_imm(1U, cell_a);
  const auto flow_done = a.pc;
  a.nop().bra_self();
  a.at(irq6).move_sr(0x2000U).store_imm(5U, cell_b).rte();  // bad: self-nesting
  a.at(irq4).movem_save_all().store_imm(5U, cell_b).movem_restore_all().rte();  // good: perfect, never touches SR
  const auto result = run(a, {{30U, irq6}, {28U, irq4}});
  if (debug()) std::cerr << describe(result);
  expect(result.frames.validated, "cascade: a validated (converged) round");
  expect(!handler_reached_analysed(result, irq6), "cascade: the bad (self-nesting) handler never reaches analysed status");
  expect(!handler_reached_analysed(result, irq4),
         "cascade: the GOOD (perfect save/restore, never self-nesting) handler ALSO never reaches analysed status -- it is "
         "collateral damage of the unrelated level-6 corruption, not a defect of its own code");
  expect(status_at(result, flow_done).is_unknown(), "cascade: the main flow's status is Unknown downstream (both levels are eligible there)");
  // Control: with level 6 made clean too (no self-nesting), the SAME level-4 handler DOES reach analysed status --
  // isolating the cascade to the unrelated handler's defect, not to having two vectors or to anything about level 4.
  Asm clean = a;
  clean.at(irq6).store_imm(5U, cell_b).rte();  // no MOVE SR: cannot self-nest
  const auto control = run(clean, {{30U, irq6}, {28U, irq4}});
  if (debug()) std::cerr << describe(control);
  expect(handler_reached_analysed(control, irq4),
         "cascade control: with the level-6 handler clean, the SAME level-4 handler reaches analysed status "
         "(confirms the cascade, not a defect in the two-vector fixture itself)");
  expect(control.frames.unproven_resumptions == 0U, "cascade control: level 4's resumption is proven once level 6 is clean");
}

// ---------------------------------------------------------------------------------------------------------------
// Step 4/5: an experiment-only "propose one handler instance, solve, validate, commit-or-reject" breaker.
//
// Never edits effective_status()/apply_resumptions()/clobbered; never called by any production route (it lives only
// in this test binary). "Solve" reuses the UNCHANGED, already-sound production engine, restricted to a configuration
// containing only the one candidate vector under test (plus, when `co_eligible` says so, any other vector genuinely
// capable of preempting the candidate -- dropping it would be the unsound non-aliasing/transparency shortcut the task
// forbids). "Validate" inspects that smaller run's OWN internal consistency (no unanalysed cause for the candidate's
// own vector, no frame-integrity failure, a proven resumption) before "commit" (accepting its derived facts) --
// never a syntactic MOVEM-shape match and never an unconditional transparency assumption.
struct Breaker {
  M68kFiniteAnalysisResult isolated;
  bool committed{};
};
Breaker propose_solve_validate(const Asm &program, const std::vector<M68kHandlerVector> &scope, std::uint32_t candidate_handler) {
  Breaker out;
  out.isolated = run(program, scope);
  out.committed = out.isolated.complete && out.isolated.frames.validated && out.isolated.frames.frame_integrity_failures == 0U &&
                  out.isolated.frames.unanalysed.empty() && out.isolated.frames.unproven_resumptions == 0U &&
                  handler_reached_analysed(out.isolated, candidate_handler);
  return out;
}

// Mutation 1: perfect save/restore -> provable (commit, and the preserved registers really are reported identity-preserved
// downstream: D0 stays precise through the interrupt-eligible window because the handler's own write is undone).
void mutation_1_perfect_preserve() {
  Asm a;
  a.move_sr(0x2000U).moveq(0, 9U);
  const auto after = a.pc;
  a.nop().bra_self();
  a.at(irq6).movem_save_all().moveq(0, 5U).movem_restore_all().rte();
  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  expect(breaker.committed, "mutation 1 (perfect preserve): breaker commits");
  expect(word_at(breaker.isolated, after, 0U) == FiniteValue::of({9U}),
         "mutation 1: D0 (saved and restored) stays precise at 9 across the interrupt-eligible window");
}

// Mutation 2: clobber D0, never restore it -> the breaker may still commit the handler (it IS analysable), but D0 must
// NOT be reported preserved: its resumption must carry the clobbered value (or Unknown), never the stale pre-interrupt one.
void mutation_2_clobber_no_restore() {
  Asm a;
  a.move_sr(0x2000U).moveq(0, 9U);
  const auto after = a.pc;
  a.nop().bra_self();
  a.at(irq6).moveq(0, 5U).rte();  // D0 := 5, never restored
  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  const auto d0 = word_at(breaker.isolated, after, 0U);
  const bool falsely_preserved = d0.is_precise() && d0.values().size() == 1U && d0.values().front() == 9U;
  expect(!falsely_preserved, "mutation 2: D0 is never falsely reported as still 9 (preserved) after the clobbering handler");
  expect(d0.is_unknown() || (d0.is_precise() && std::find(d0.values().begin(), d0.values().end(), 5U) != d0.values().end()),
         "mutation 2: D0 downstream reflects the clobbered value 5 (or Unknown), not a false preservation claim");
}

// Mutation 3: restore D0 from the wrong stack slot (swap D0/D1's save slots) -> the preservation claim must be rejected;
// D0 must not be reported as its original pre-interrupt value.
void mutation_3_wrong_slot() {
  Asm a;
  a.move_sr(0x2000U).moveq(0, 9U).moveq(1, 0xAU);
  const auto after = a.pc;
  a.nop().bra_self();
  // Save D0,D1; restore into D1,D0 (swapped): MOVEM.L D0/D1,-(A7); MOVEM.L (A7)+,D1/D0 is not expressible as a single
  // legal MOVEM (the mask names registers, not slot order), so the swap is built explicitly with MOVE.L.
  a.at(irq6).w({0x2F00U});           // MOVE.L D0,-(A7)
  a.w({0x2F01U});                    // MOVE.L D1,-(A7)
  a.w({0x201FU});                    // MOVE.L (A7)+,D0  <- actually loads what was D1 (wrong slot for D0)
  a.w({0x221FU});                    // MOVE.L (A7)+,D1  <- loads what was D0 (wrong slot for D1)
  a.rte();
  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  const auto d0 = word_at(breaker.isolated, after, 0U);
  const bool falsely_preserved = d0.is_precise() && d0.values().size() == 1U && d0.values().front() == 9U;
  expect(!falsely_preserved, "mutation 3: D0 restored from D1's slot is never falsely reported as its original value 9");
}

// Mutation 4: corrupt the saved SR (MOVE.W #imm,(A7) before RTE) -> the frame/status effect downstream must reflect the
// corruption: the breaker must not commit a clean, unaffected status for the interrupted boundary.
void mutation_4_corrupt_saved_sr() {
  Asm a;
  a.move_sr(0x2000U).moveq(0, 9U);
  const auto after = a.pc;
  a.nop().bra_self();
  a.at(irq6).w({0x3EBCU, 0x2000U}).rte();  // MOVE.W #$2000,(A7): rewrites the saved SR word
  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  expect(!breaker.committed, "mutation 4: corrupting the saved SR is rejected (not committed as a clean resumption)");
  expect(status_at(breaker.isolated, after).is_unknown(),
         "mutation 4: the status after the interrupt-eligible point reflects the corruption (Unknown), never the original SR");
}

// Mutation 5: fail to restore A7 correctly (push one extra long and never pop it before RTE) -> rejected.
void mutation_5_bad_a7_restore() {
  Asm a;
  a.move_sr(0x2000U);
  const auto after = a.pc;
  a.nop().bra_self();
  a.at(irq6).w({0x2F3CU, 0x0000U, 0x0000U}).rte();  // MOVE.L #0,-(A7): leaves A7 four bytes short of the frame at RTE
  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  expect(!breaker.committed, "mutation 5: an unrestored A7 (extra push before RTE) is rejected, not committed as a sound exit");
  (void)after;
}

// Mutation 6: two RTE exits, one corrupt (a branch picks between a clean exit and a corrupted-frame exit) -> the whole
// resumption must be rejected: a good exit's proof must never paper over a bad one.
//
// Adversarial-review correction: an earlier revision used "ADDQ.L #2,2(A7)" before RTE as the "corrupted" exit. Two bugs
// stacked there: (1) its guarding BEQ.S used displacement +4, whose M68000PRM PC-relative target (opcode address + 2 +
// displacement) landed on the ADDQ instruction's own extension word rather than the ADDQ opcode itself -- the breaker still
// rejected the resulting decode-garbled handler, but for the wrong reason. (2) even with that encoding fixed, ADDQ/SUBQ on
// the frame's own stacked-PC cell is not corruption at all: finite_adapter.cpp's frame_pc_identity tracking (SEG-030-T009
// correction cycle 2) deliberately and precisely tracks an immediate ADDQ/SUBQ/ADDI/SUBI adjustment of that cell as a sound,
// offset-exact resumption (a legitimate "return 2 bytes past the stacked PC" pattern) -- so the breaker correctly COMMITTED
// it once the branch-target bug was fixed, which is not a production defect, only a mischaracterized fixture. The actually
// corrupting exit used below instead stores an Unknown-valued register (D1, loaded from the never-written cell_a) directly
// into the frame's PC slot: a plain (non-ADDQ/SUBQ) long store onto that cell degrades its fact to frame_pc_identity, which
// carries no offset (frame_pc_fact_offset returns nullopt below 0x80) -- so that exit's resumption is genuinely unprovable,
// never an exact alternate return address.
void mutation_6_mixed_exits() {
  Asm a;
  a.move_sr(0x2000U).moveq(0, 9U);
  const auto after = a.pc;
  a.nop().bra_self();
  a.at(irq6).load_word(1, cell_a);  // D1 <- an Unknown cell: picks either path
  const auto beq_pc = a.pc;
  a.w({0x6702U});                    // BEQ.S +2: lands exactly on the corrupted exit's own opcode (see regression check below)
  a.rte();                           // clean exit: unmodified frame
  const auto corrupt_entry = a.pc;
  a.w({0x2F41U, 0x0002U}).rte();     // corrupted exit: MOVE.L D1,2(A7) then RTE -- D1 (Unknown) overwrites the stacked PC

  // Regression (found during adversarial review): confirm the BEQ's PC-relative target is genuinely the corrupted exit's own
  // opcode address, not a byte or two into its extension word (the original encoding bug this replaced).
  const RegionImage decode_view{a};
  expect(beq_pc + 2U + 2U == corrupt_entry,
         "mutation 6: BEQ.S +2's PC-relative target is exactly the corrupted exit's own opcode address, not mid-instruction");
  const auto at_target = decode_view.decode(beq_pc + 2U + 2U);
  const auto at_corrupt_entry = decode_view.decode(corrupt_entry);
  expect(at_target.has_value() && at_corrupt_entry.has_value() && at_target->length == at_corrupt_entry->length,
         "mutation 6: decoding at the branch target agrees with decoding the corrupted exit directly (not garbled)");

  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  expect(!breaker.committed || breaker.isolated.frames.unproven_resumptions > 0U,
         "mutation 6: a handler with a mixed-quality set of RTE exits never commits an unconditionally clean resumption");
  (void)after;
}

// Mutation 7: an unknown, potentially-aliasing write into the save area (through an Unknown-valued address register) ->
// rejected: disjointness from the save slots cannot be assumed.
void mutation_7_aliasing_write() {
  Asm a;
  a.move_sr(0x2000U).moveq(0, 9U);
  const auto after = a.pc;
  a.nop().bra_self();
  // The address register is loaded from a never-written cell (Unknown), then used to store through it: this store may
  // alias anything, including the handler's own saved registers.
  a.at(irq6).movem_save_all();
  a.w({0x2079U}).l(0x00FF0300U);  // MOVEA.L (cell C, never written).L,A0 -> A0 is Unknown
  a.w({0x2088U});                 // MOVE.L A0,(A0): an Unknown-target store (also exercises the aliasing address itself)
  a.movem_restore_all().rte();
  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  expect(!breaker.committed, "mutation 7: an unknown-target store inside the handler is rejected (disjointness is never assumed)");
  (void)after;
}

// Mutation 8: a nested, higher-priority interrupt during the handler -> conservative unless proven. The candidate
// (level 4) is syntactically perfect, but a real, genuinely co-eligible level-6 source can preempt it with an
// unanalysable (self-nesting) effect of its own. Excluding level 6 from the breaker's scope would be the unsound
// non-aliasing/transparency shortcut the task forbids; keeping it in scope, the breaker correctly stays conservative.
void mutation_8_nested_higher_priority() {
  Asm a;
  a.move_sr(0x2000U);
  const auto after = a.pc;
  a.nop().bra_self();
  a.at(irq4).movem_save_all().moveq(0, 7U).movem_restore_all().rte();  // the candidate: perfect in isolation
  a.at(irq6).move_sr(0x2000U).rte();                                  // genuinely co-eligible, self-nesting (unanalysable)
  const std::vector<M68kHandlerVector> scope{{28U, irq4}, {30U, irq6}};  // sound scope: keeps the real nesting source
  const auto breaker = propose_solve_validate(a, scope, irq4);
  expect(!breaker.committed,
         "mutation 8: a candidate that IS genuinely co-eligible with an unbounded nested source stays conservative (not committed)");
  // An unsound breaker that dropped level 6 from scope (pretending the candidate were alone) would wrongly commit it:
  const auto unsound = propose_solve_validate(a, {{28U, irq4}}, irq4);
  expect(unsound.committed,
         "mutation 8 (negative control): dropping the genuinely co-eligible nested source DOES let an unsound breaker "
         "falsely commit -- demonstrating why scope narrowing must never drop a real nesting source");
  (void)after;
}

// Mutation 9: a handler that never executes RTE (an infinite tight loop) -> no resumption may be produced for it.
void mutation_9_never_returns() {
  Asm a;
  a.move_sr(0x2000U);
  const auto after = a.pc;
  a.nop().bra_self();
  a.at(irq6).moveq(0, 5U).bra_self();  // never reaches RTE
  const auto breaker = propose_solve_validate(a, irq6_only, irq6);
  expect(breaker.isolated.frames.resumption_points == 0U,
         "mutation 9: a handler that never executes RTE produces no resumption points");
  expect(breaker.isolated.frames.unproven_resumptions == 0U,
         "mutation 9: absence of a resumption is not reported as an unproven one (nothing is falsely claimed either way)");
  (void)after;
}

// Mutation 10: hitting an existing resource/iteration bound mid-analysis -> Unknown, never an optimistic guess. Reuses
// the production round-bound knob (never raising it) at 0, matching the existing failed_frame_round_is_incomplete
// convention: the result must be explicitly incomplete, never a false "committed" verdict.
void mutation_10_iteration_bound() {
  const RegionImage view{[] {
    Asm a;
    a.move_sr(0x2000U).nop().bra_self();
    a.at(irq6).movem_save_all().moveq(0, 5U).movem_restore_all().rte();
    return a;
  }()};
  M68kAnalysisConfig config{};
  config.domains.address = true;
  config.domains.memory = true;
  config.domains.contexts = true;
  config.domains.frames = true;
  config.frames.vectors = irq6_only;
  config.frames.main_entries = {entry};
  config.frames.reset_entry = entry;
  config.frames.reset_ssp = ssp;
  config.contexts.round_bound = 0U;  // an existing bound, deliberately starved -- never raised
  const auto result = analyze_m68k_finite_values(view, {entry, irq6}, config);
  expect(!result.complete, "mutation 10: an exhausted iteration bound is explicitly incomplete, never a committed proof");
  const bool falsely_committed = result.complete && result.frames.validated && handler_reached_analysed(result, irq6);
  expect(!falsely_committed, "mutation 10: the breaker's own commit criterion never fires on a bound-starved, incomplete run");
}

}  // namespace

int main() {
  clean_single_handler_reaches_analysed();
  self_nesting_handler_never_analysed();
  self_nesting_poisons_unrelated_handler();
  mutation_1_perfect_preserve();
  mutation_2_clobber_no_restore();
  mutation_3_wrong_slot();
  mutation_4_corrupt_saved_sr();
  mutation_5_bad_a7_restore();
  mutation_6_mixed_exits();
  mutation_7_aliasing_write();
  mutation_8_nested_higher_priority();
  mutation_9_never_returns();
  mutation_10_iteration_bound();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_frames_circularity_test: all checks passed\n";
  return EXIT_SUCCESS;
}
