// SEG-040-T002 (ADR 0087 follow-up, report-only): sharpens the SEG-039 collateral-poisoning finding along the one
// dimension T001's fixture did not vary: the RELATIVE PRIORITY of the self-nesting ("bad") vector and the otherwise
// perfect, unrelated ("good") sibling sharing its parent partition. Reuses the exact Asm/RegionImage/run() conventions
// from tests/analysis_m68k_frames_circularity_test.cpp (copied here, not reinvented, per that file's own stated
// convention), so this file is independently readable and runnable. Never edits
// effective_status()/apply_resumptions()/clobbered; never linked into any production route; the production diff of
// this task is empty. Project-authored synthetic fixtures only; no commercial input.
//
// Finding pinned here: the shared, cross-candidate `clobbered[0]` entry-status collapse ADR 0087 identified is
// PRIORITY-ORDER-INDEPENDENT. It reproduces identically whether the self-nesting vector's own interrupt level is
// numerically ABOVE the unrelated sibling's level (priority_order_bad_above_good: levels 6 and 4, the exact shape of
// the existing self_nesting_poisons_unrelated_handler() fixture) or BELOW it (priority_order_bad_below_good: levels 2
// and 4, where the M68000PRM interrupt-mask priority rule means the self-nesting vector cannot even legally preempt
// the sibling's own handler body). This rules out "just reorder the vectors" as a workaround and confirms the defect
// is a property of the shared fact itself, not of T001's specific level choice.
//
// A second, related finding (SEG-040-T002's report, not pinned as an automated assertion here) is that at the "bad
// ABOVE good" ordering specifically, the sibling ALSO has an independent, non-collateral, hardware-correct
// frame-integrity exposure of its own (a self-nesting handler that can legally nest inside it can also corrupt its
// own saved SR frame while it runs) -- so a fix that only repairs the shared fact exercised below would not, by
// itself, make such a sibling's resumption fully provable. That second finding is NOT independently observable
// through any existing counter without first unblocking the entry proof the shared fact currently blocks (every
// existing production entry point treats the two causes as one joined fact), so it could only be confirmed via a
// temporary, non-shipped instrumentation experiment (reverted before this task's completion; see the task report for
// the exact methodology) rather than a durable automated test -- itself a relevant data point, not merely an
// omission: the current instrumentation cannot observationally separate the two causes, only a sound mechanism that
// first separates them architecturally could make the distinction durably testable.

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

// ---------------------------------------------------------------------------------------------------------------
// Shared fixture infrastructure (same conventions as analysis_m68k_frames_circularity_test.cpp).

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t handler_a = 0x400U;  // the self-nesting ("bad") handler slot
constexpr std::uint32_t handler_b = 0x480U;  // the perfect, unrelated ("good") handler slot
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;
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
  Asm &store_imm(std::uint32_t value, std::uint32_t address) { return w({0x33FCU, value}).l(address); }
  Asm &nop() { return w({0x4E71U}); }
  Asm &rte() { return w({0x4E73U}); }
  Asm &movem_save_all() { return w({0x48E7U, 0xFFFEU}); }
  Asm &movem_restore_all() { return w({0x4CDFU, 0x7FFFU}); }
  Asm &bra_self() { return w({0x60FEU}); }
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
bool debug() { return std::getenv("FRAMES_DEBUG") != nullptr; }
std::string describe(const M68kFiniteAnalysisResult &result) { return format_m68k_finite_analysis(result); }

// ---------------------------------------------------------------------------------------------------------------
// The shared cross-candidate entry-status collapse reproduces identically whether the self-nesting vector's
// priority LEVEL is numerically above or below the unrelated sibling's own level.

struct CascadeResult {
  bool bad_analysed{};
  bool good_analysed{};
  bool good_analysed_control{};
  bool control_proven{};
};
CascadeResult run_cascade(unsigned bad_level, unsigned good_level) {
  Asm a;
  a.move_sr(0x2000U);
  a.nop().bra_self();
  a.at(handler_a).move_sr(0x2000U).store_imm(5U, cell_b).rte();  // bad: self-nesting, at `bad_level`
  a.at(handler_b).movem_save_all().store_imm(5U, cell_b).movem_restore_all().rte();  // good: perfect, never touches SR
  const std::vector<M68kHandlerVector> vectors{{24U + bad_level, handler_a}, {24U + good_level, handler_b}};
  const auto result = run(a, vectors);
  if (debug()) std::cerr << describe(result);

  Asm clean = a;
  clean.at(handler_a).store_imm(5U, cell_b).rte();  // no MOVE SR: cannot self-nest
  const auto control = run(clean, vectors);
  if (debug()) std::cerr << describe(control);

  return {handler_reached_analysed(result, handler_a), handler_reached_analysed(result, handler_b),
          handler_reached_analysed(control, handler_b), control.frames.unproven_resumptions == 0U};
}

// Priority order A: bad (self-nesting) at level 6 > good (perfect) at level 4 -- exactly
// self_nesting_poisons_unrelated_handler()'s own shape, reproduced here for direct comparison with order B.
void priority_order_bad_above_good() {
  const auto r = run_cascade(6U, 4U);
  expect(!r.bad_analysed, "order A (bad=6>good=4): the self-nesting handler never reaches analysed status");
  expect(!r.good_analysed, "order A (bad=6>good=4): the unrelated handler also never reaches analysed status (collateral)");
  expect(r.good_analysed_control, "order A control (bad clean): the same unrelated handler DOES reach analysed status");
  expect(r.control_proven, "order A control: the unrelated handler's resumption is proven once the bad handler is clean");
}

// Priority order B: bad (self-nesting) at level 2 < good (perfect) at level 4. At this ordering the bad handler
// CANNOT preempt the good handler's own body (2 is not > 4: M68000PRM interrupt-mask priority), so the good handler
// has no hardware-grounded internal exposure to the bad handler's defect at all -- yet the SAME collateral-blocking
// outcome reproduces identically in the unmodified production engine, confirming the shared `clobbered[0]` fact (not
// T001's specific level choice, and not any genuine nesting exposure) is what blocks it here.
void priority_order_bad_below_good() {
  const auto r = run_cascade(2U, 4U);
  expect(!r.bad_analysed, "order B (bad=2<good=4): the self-nesting handler never reaches analysed status");
  expect(!r.good_analysed,
         "order B (bad=2<good=4): the unrelated handler ALSO never reaches analysed status, even though it has no "
         "hardware-grounded internal exposure to the bad handler at this priority ordering -- confirming the blocking "
         "mechanism is the shared cross-candidate fact itself, not the specific priority choice of the original fixture");
  expect(r.good_analysed_control, "order B control (bad clean): the same unrelated handler DOES reach analysed status");
  expect(r.control_proven, "order B control: the unrelated handler's resumption is proven once the bad handler is clean");
}

}  // namespace

int main() {
  priority_order_bad_above_good();
  priority_order_bad_below_good();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_frames_priority_order_test: all checks passed\n";
  return EXIT_SUCCESS;
}
