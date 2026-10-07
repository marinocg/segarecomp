// SEG-040-T003 (ADR 0087/0087-follow-up continuation, report-only): tests the premise behind a proposed
// priority-aware interrupt-preemption mechanism (a graph where an edge `candidate <- other` exists only when
// `other`'s vector level is eligible against `candidate`'s OWN governing/entry mask) against the exact engine
// SEG-039/SEG-040-T002 already diagnosed. Never edits effective_status()/apply_resumptions()/clobbered; never
// linked into any production route; the production diff of this task is empty. Project-authored synthetic
// fixtures only; no commercial input. Reuses the Asm/RegionImage/run() conventions of
// tests/analysis_m68k_frames_priority_order_test.cpp (copied here per that file's own stated convention).
//
// SEG-040-T002 showed the shared cross-candidate status collapse reproduces identically whether a self-nesting
// "bad" vector's level is above or below an unrelated "good" sibling's. That fixture's `bad` handler always
// executes `MOVE SR,#$2000` to set up its own self-nesting, which unconditionally re-opens every interrupt level
// (mask 0) while `bad` runs -- so even in the "bad cannot preempt good" ordering, `bad`'s OWN running mask (not
// its nominal vector level) genuinely admits a real nested-`good`-inside-`bad` edge, and `bad`'s self-nesting
// also makes `bad`'s OWN saved-SR frame provably unprovable (an unanalysed self-nested child is modelled as
// writing every cell, SEG-030-T004's documented policy for an undescribed/unanalysed writer). SEG-040-T002's
// canary therefore cannot, by itself, separate "the poisoning is priority-independent" from "a real edge exists
// anyway, just not the nominal one."
//
// This file removes that confound: `bad` here never writes its own SR at all (no self-nesting is even
// possible), and its ONLY defect is a single memory write through an address register that is never set (an
// ordinary, unrelated, undescribed-target write -- SEG-030-T004's existing async_all policy, nothing to do with
// interrupts). `bad`'s own nominal vector level is set LOWER than `good`'s, so under plain MC68000 interrupt-mask
// priority `bad` cannot legally preempt `good`'s body at all (its level is not greater than `good`'s own entry
// mask). If the shared-partition collapse were actually a pairwise preemption-eligibility fact (as a priority-
// graph design would assume), `good` should be unaffected here. The finding pinned by this test is that it is
// not: the unrelated, non-preempting, non-self-nesting `bad` handler's own frame-integrity failure (section 5 of
// `finite_adapter.cpp`'s frame derivation: `bad`'s own write target cannot be shown to miss `bad`'s own saved SR
// slot, since an unresolved-target write is conservatively `async_all`) still clobbers the SHARED PARENT
// partition (main flow, tag 0) that `good` is also taken from -- `good`'s own entry-frame derivation depends on
// main flow's OWN propagated status at the point `good` is taken, not on anything inside `good`'s own partition
// and not on any eligibility test against `good`'s own mask. A vector-level-gated preemption edge between `bad`
// and `good` could not intercept this: the hazard being modelled (main flow's own SR becoming unprovable after
// taking an unanalysable handler) is a fact about `bad` and `bad`'s OWN parent partition alone, independent of
// which other sibling later shares that parent and independent of that sibling's priority. Suppressing it by a
// `good`-relative level comparison would have no architectural justification and would be unsound (it would
// let `good`'s resumption be "proven" even though main flow's own processor status genuinely may have been
// corrupted by `bad`'s unanalysed write before `good` was ever taken).
//
// A companion control confirms the known-good baseline: once `bad`'s write target is made precise (no longer
// unresolved), both `bad` and `good` become analysed and `good`'s resumption is proven -- unchanged from today,
// confirming the defect isolated above is exactly, and only, the unresolved-write-target frame-integrity path,
// not some other difference between the two fixtures.

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

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t handler_a = 0x400U;  // "bad": an unrelated, non-self-nesting, undescribed-write handler
constexpr std::uint32_t handler_b = 0x480U;  // "good": the perfect, unrelated sibling
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;
constexpr std::uint32_t cell_b = 0x00FF0200U;
constexpr std::uint32_t known_target = 0x00FF0300U;

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
  Asm &store_imm(std::uint32_t value, std::uint32_t address) { return w({0x33FCU, value}).l(address); }
  Asm &nop() { return w({0x4E71U}); }
  Asm &rte() { return w({0x4E73U}); }
  Asm &movem_save_all() { return w({0x48E7U, 0xFFFEU}); }
  Asm &movem_restore_all() { return w({0x4CDFU, 0x7FFFU}); }
  Asm &bra_self() { return w({0x60FEU}); }
  // MOVE.L D0,(A0): a write through an address register this fixture never initialises, so the engine's
  // unresolved-target policy applies (SEG-030-T004: an undescribed/unresolved writer poisons every cell).
  Asm &store_unknown_target_via_a0() { return w({0x2080U}); }
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

M68kFiniteAnalysisResult run(const Asm &program, const std::vector<M68kHandlerVector> &vectors) {
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
  return analyze_m68k_finite_values(view, std::vector<std::uint32_t>(entries.begin(), entries.end()), config);
}

bool reached_in(const M68kFiniteAnalysisResult &result, std::uint32_t pc, std::uint32_t tag) {
  for (const auto &[point, state] : m68k_points_of(result, pc)) {
    (void)state;
    if (m68k_point_tag(point) == tag) return true;
  }
  return false;
}
bool handler_reached_analysed(const M68kFiniteAnalysisResult &result, std::uint32_t handler) {
  if (!result.frames.validated) return false;
  for (std::uint32_t tag = 1U; tag < m68k_max_instance_tag && tag < 64U; ++tag)
    if (reached_in(result, handler, tag)) return true;
  return false;
}

// `bad` (level 2, no self-nesting, no SR writes at all -- cannot legally preempt `good` at level 6, 2 is not >
// 6) has a single undescribed-target write; `good` (level 6) is otherwise perfect. If the shared-partition
// collapse were a pairwise preemption-eligibility fact, `good` should be unaffected by `bad`'s defect here.
void unrelated_non_preempting_bad_still_collaterally_blocks_good() {
  Asm a;
  a.move_sr(0x2000U);
  a.nop().bra_self();
  a.at(handler_a).store_unknown_target_via_a0().rte();              // bad: unrelated undescribed write, level 2
  a.at(handler_b).movem_save_all().store_imm(5U, cell_b).movem_restore_all().rte();  // good: perfect, level 6
  const std::vector<M68kHandlerVector> vectors{{24U + 2U, handler_a}, {24U + 6U, handler_b}};
  const auto result = run(a, vectors);

  expect(!handler_reached_analysed(result, handler_b),
         "an unrelated, non-self-nesting, strictly-lower-level `bad` handler's own undescribed-target write "
         "still collaterally blocks the higher-level, perfect `good` sibling from reaching analysed status -- "
         "confirming the shared cross-candidate status collapse is not a pairwise preemption-eligibility fact "
         "between `bad` and `good` (a vector-level-gated edge from `bad` to `good` could not have intercepted "
         "this, since `bad` cannot preempt `good` at these levels at all)");
}

// Control: once `bad`'s write target is made precise (no longer unresolved), both handlers are unaffected by
// each other and both reach analysed status with a proven resumption -- the known-good baseline, confirming the
// isolated defect above is exactly the unresolved-write-target frame-integrity path.
void control_precise_write_target_leaves_both_handlers_analysed() {
  Asm a;
  a.move_sr(0x2000U);
  a.nop().bra_self();
  a.at(handler_a).store_imm(7U, known_target).rte();  // bad: now a precise, unrelated write; level 2
  a.at(handler_b).movem_save_all().store_imm(5U, cell_b).movem_restore_all().rte();  // good: perfect, level 6
  const std::vector<M68kHandlerVector> vectors{{24U + 2U, handler_a}, {24U + 6U, handler_b}};
  const auto result = run(a, vectors);

  expect(handler_reached_analysed(result, handler_a), "control: the now-precise `bad` handler reaches analysed status");
  expect(handler_reached_analysed(result, handler_b), "control: `good` reaches analysed status once `bad`'s write is precise");
  expect(result.frames.unproven_resumptions == 0U, "control: no unproven resumption remains once `bad`'s write is precise");
}

}  // namespace

int main() {
  unrelated_non_preempting_bad_still_collaterally_blocks_good();
  control_precise_write_target_leaves_both_handlers_analysed();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_frames_preemption_scope_test: all checks passed\n";
  return EXIT_SUCCESS;
}
