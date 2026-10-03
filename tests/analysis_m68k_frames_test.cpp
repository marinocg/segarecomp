// SEG-030-T006 (ADR 0079 decisions 5, 7, 9 and 10): the CPU-owned interrupt-mask, handler-stack, exception-frame and
// RTE/RTR/computed-RTS provenance of the `frames` domain.
//
// Every result is a proof or the expected typed Unknown: per-point interrupt eligibility from the SR mask (reset: S = 1, I = 7), the
// frame address from a proven supervisor stack, handler instances entered with the accepted mask and the frame address, nesting and
// preemption per the mask, per-partition asynchronous writers, frame integrity, and returns resolved only from code-built frames.
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
#include <tuple>
#include <type_traits>
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

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t irq6 = 0x400U;   // level-6 autovector handler (vector 30)
constexpr std::uint32_t sync_a = 0x500U; // a synchronous handler
constexpr std::uint32_t sync_b = 0x580U;
constexpr std::uint32_t target = 0x700U;
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;   // the reset supervisor stack
constexpr std::uint32_t cell_a = 0x00FF0100U;
constexpr std::uint32_t cell_b = 0x00FF0200U;
constexpr std::uint32_t cell_c = 0x00FF0300U;  // never written: initial memory

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
  Asm &move_sr(std::uint32_t sr) { return w({0x46FCU, sr}); }                         // MOVE #sr,SR
  Asm &moveq(unsigned reg, std::uint32_t value) { return w({0x7000U | (reg << 9U) | (value & 0xFFU)}); }
  Asm &store_word(unsigned reg, std::uint32_t address) { return w({0x33C0U | reg}).l(address); }  // MOVE.W Dn,(xxx).L
  Asm &store_imm(std::uint32_t value, std::uint32_t address) { return w({0x33FCU, value}).l(address); }  // MOVE.W #v,(xxx).L
  Asm &load_word(unsigned reg, std::uint32_t address) { return w({0x3039U | (reg << 9U)}).l(address); }  // MOVE.W (xxx).L,Dn
  Asm &push_long(std::uint32_t value) { return w({0x2F3CU}).l(value); }              // MOVE.L #v,-(A7)
  Asm &push_word(std::uint32_t value) { return w({0x3F3CU, value}); }                // MOVE.W #v,-(A7)
  Asm &nop() { return w({0x4E71U}); }
  Asm &rte() { return w({0x4E73U}); }
  Asm &rtr() { return w({0x4E77U}); }
  Asm &rts() { return w({0x4E75U}); }
  Asm &stop() { return w({0x60FEU}); }  // BRA.S *
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

M68kFiniteAnalysisResult run(const Asm &program, const std::vector<M68kHandlerVector> &vectors, bool reset = true, bool frames = true,
                             const std::vector<M68kHandlerVector> &potential = {}) {
  const RegionImage view{program};
  M68kAnalysisConfig config{};
  config.domains.address = true;
  config.domains.memory = true;
  config.domains.contexts = true;
  config.domains.frames = frames;
  config.memory.interrupts = !frames;
  std::set<std::uint32_t> entries{entry};
  for (const auto &vector : vectors) {
    entries.insert(vector.handler);
    if (!frames) config.memory.handler_roots.push_back(vector.handler);
  }
  config.frames.vectors = vectors;
  config.frames.potential_interrupts = potential;
  config.frames.main_entries = {entry};
  if (reset) {
    config.frames.reset_entry = entry;
    config.frames.reset_ssp = ssp;
  }
  return analyze_m68k_finite_values(view, std::vector<std::uint32_t>(entries.begin(), entries.end()), config);
}

// The join over every point of `pc` in partition `tag` of `get(state)`.
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
M68kPointsTo a7_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc, std::uint32_t tag = 0U) {
  return join_at(result, pc, tag, [](const M68kAnalysisState &state) { return state.address[7]; });
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
const M68kReturnSiteReport *return_site(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto found = result.return_sites.find(pc);
  return found == result.return_sites.end() ? nullptr : &found->second;
}
std::string describe(const M68kFiniteAnalysisResult &result) { return format_m68k_finite_analysis(result); }
bool debug() { return std::getenv("FRAMES_DEBUG") != nullptr; }

const std::vector<M68kHandlerVector> irq_only{{30U, irq6}};

// A main flow that stores 1 into cell A and cell B, reads both back into D1 and D2, then stops. `sr` is loaded into SR first.
struct MainFlow {
  std::uint32_t read_done{};
};
MainFlow main_flow(Asm &a, std::optional<std::uint32_t> sr) {
  if (sr) a.move_sr(*sr);
  a.moveq(0, 1U).store_word(0, cell_a).store_word(0, cell_b).nop();
  a.load_word(1, cell_a).load_word(2, cell_b);
  MainFlow out{a.pc};
  a.stop();
  return out;
}

// ---------------------------------------------------------------------------------------------------------------

void interrupt_masked() {
  // Reset leaves I = 7: the level-6 handler can never be taken, its store never reaches the main flow, both reads stay precise.
  Asm a;
  const auto flow = main_flow(a, std::nullopt);
  a.at(irq6).store_imm(5U, cell_a).rte();
  const auto result = run(a, irq_only);
  if (debug()) std::cerr << describe(result);
  expect(result.complete && result.frames.validated, "masked: a validated frames round");
  expect(status_at(result, flow.read_done) == FiniteValue::of({m68k_reset_status}), "masked: the reset status (S = 1, I = 7) holds");
  expect(result.frames.interrupt_eligible == 0U && result.frames.instances == 0U && result.frames.dead_handlers == 1U,
         "masked: no boundary admits level 6; the handler has no live instance");
  expect(word_at(result, flow.read_done, 1U) == FiniteValue::of({1U}) && word_at(result, flow.read_done, 2U) == FiniteValue::of({1U}),
         "masked: both cells are read precisely (no asynchronous writer)");
  expect(!result.frames.main_async_all && result.frames.main_async_ranges == 0U, "masked: the main flow has no asynchronous writer");
  // Inert when off: the same program without the frames domain is the T004/T005 model (every cell asynchronous under interrupts).
  const auto off = run(a, irq_only, true, false);
  expect(!off.frames.enabled && off.return_sites.empty() && describe(off).find("status=") == std::string::npos &&
             word_at(off, flow.read_done, 2U).is_unknown(),
         "masked: without the frames domain the T005 model is unchanged (every cell asynchronous)");
}

void interrupt_enabled() {
  // MOVE #$2000,SR: level 6 is taken at every later boundary. The handler instance is entered at SSP - 6 with I = 6; the main
  // flow's asynchronous writers are exactly its store (cell A) and its frames below SSP: cell B stays precise.
  Asm a;
  const auto flow = main_flow(a, 0x2000U);
  a.at(irq6).store_imm(5U, cell_a);
  const auto rte = a.pc;
  a.rte();
  const auto result = run(a, irq_only);
  if (debug()) std::cerr << describe(result);
  expect(result.complete && result.frames.validated && result.frames.instances == 1U && result.frames.interrupt_instances == 1U,
         "enabled: one validated interrupt instance");
  expect(status_at(result, flow.read_done) == FiniteValue::of({8U}), "enabled: the main flow runs with S = 1, I = 0");
  std::uint32_t tag = 0U;
  for (std::uint32_t t = 1U; t < 8U; ++t)
    if (reached_in(result, irq6, t)) tag = t;
  expect(tag != 0U && status_at(result, irq6, tag) == FiniteValue::of({0xEU}), "enabled: the instance is entered with S = 1, I = 6");
  const auto entry_a7 = a7_at(result, irq6, tag);
  expect(entry_a7.is_known() && entry_a7.values() == std::vector<std::uint32_t>{ssp - m68k_exception_frame_bytes},
         "enabled: the instance's entry A7 is the frame address SSP - 6: " + entry_a7.describe());
  expect(word_at(result, flow.read_done, 1U).is_unknown() && word_at(result, flow.read_done, 2U) == FiniteValue::of({1U}),
         "enabled: cell A (handler store) is asynchronous, cell B stays precise");
  expect(!result.frames.main_async_all && result.frames.main_async_ranges == 2U &&
             result.frames.main_async_bytes == 2U + m68k_exception_frame_bytes,
         "enabled: the main flow's asynchronous writers are the handler's cell and its frame (" +
             std::to_string(result.frames.main_async_ranges) + " ranges, " + std::to_string(result.frames.main_async_bytes) + " bytes)");
  const auto *site = return_site(result, rte);
  expect(site != nullptr && !site->resolved && site->sub == Sub::interrupt_resumption,
         "enabled: the handler's RTE resumes interrupted code: Unknown(interrupt_resumption)");
  expect(describe(result) == describe(run(a, irq_only)), "enabled: deterministic");
}

void handler_raises_mask() {
  // The handler raises the mask to 7 at once: no boundary of the instance admits level 6 again (no nesting).
  Asm a;
  const auto flow = main_flow(a, 0x2000U);
  a.at(irq6).move_sr(0x2700U);
  const auto store = a.pc;
  a.store_imm(5U, cell_a).rte();
  const auto result = run(a, irq_only);
  if (debug()) std::cerr << describe(result);
  std::uint32_t tag = 0U;
  for (std::uint32_t t = 1U; t < 8U; ++t)
    if (reached_in(result, store, t)) tag = t;
  expect(result.frames.validated && tag != 0U && status_at(result, store, tag) == FiniteValue::of({0xFU}),
         "raises: S = 1, I = 7 after MOVE #$2700,SR in the handler");
  expect(result.frames.unanalysed.empty() && !result.frames.main_async_all &&
             word_at(result, flow.read_done, 2U) == FiniteValue::of({1U}),
         "raises: no nested instance; the main flow keeps a bounded writer set");
}

void handler_lowers_mask() {
  // The handler lowers the mask to 0: level 6 can preempt the handler itself (nesting, unbounded): that nested instance is not
  // analysed, the handler instance's writers are every cell, and so are the main flow's.
  Asm a;
  const auto flow = main_flow(a, 0x2000U);
  a.at(irq6).move_sr(0x2000U).store_imm(5U, cell_a).rte();
  const auto result = run(a, irq_only);
  if (debug()) std::cerr << describe(result);
  // The nested instance makes the handler instance's writers every cell and its saved SR unproven, so the main flow's status is
  // Unknown after every eligible boundary and the handler's own entry is no longer a proven frame (both are unanalysed causes).
  expect(result.frames.validated && unanalysed(result, "nested") + unanalysed(result, "entry_unknown") >= 1U &&
             result.frames.main_async_all,
         "lowers: the nested instance is unanalysed and the main flow's writers are every cell");
  expect(word_at(result, flow.read_done, 2U).is_unknown(), "lowers: cell B is no longer precise");
}

void nested_synchronous() {
  // TRAP #0 from the main flow (I = 7). Its handler raises TRAP #1: a non-resuming handler inside a non-resuming instance is not
  // analysed and has no consequence on the main flow (neither resumes into an analysed boundary).
  Asm a;
  a.moveq(0, 1U).store_word(0, cell_b).load_word(2, cell_b);
  const auto trap = a.pc;
  a.w({0x4E40U});  // TRAP #0
  a.at(sync_a).w({0x4E41U}).rte();
  a.at(sync_b).rte();
  const auto result = run(a, {{32U, sync_a}, {33U, sync_b}});
  if (debug()) std::cerr << describe(result);
  expect(result.frames.validated && result.frames.synchronous_instances == 1U && unanalysed(result, "non_resuming_parent") == 1U,
         "nested sync: one TRAP #0 instance; the TRAP #1 inside it is unanalysed");
  expect(!result.frames.main_async_all && word_at(result, trap, 2U) == FiniteValue::of({1U}),
         "nested sync: the main flow has no asynchronous writer");
  // A resuming handler (divide by zero) that divides again: nested in itself, so its writers, and the main flow's, are every cell.
  Asm b;
  b.moveq(3, 1U).store_word(3, cell_b).w({0x80C1U});  // DIVU.W D1,D0
  const auto after = b.pc;
  b.load_word(2, cell_b);
  const auto done = b.pc;
  b.stop();
  b.at(sync_a).w({0x80C1U}).rte();
  const auto nested = run(b, {{5U, sync_a}});
  if (debug()) std::cerr << describe(nested);
  expect(nested.frames.validated && unanalysed(nested, "nested") >= 1U && nested.frames.main_async_all &&
             word_at(nested, done, 2U).is_unknown(),
         "nested sync: a resuming handler nested in itself makes the main flow's writers every cell");
  // The same handler without the nested divide: its frame is pushed in the main flow at A7 - 6, cell B stays precise.
  Asm c = b;
  c.at(sync_a).store_imm(5U, cell_a).rte();
  const auto single = run(c, {{5U, sync_a}});
  if (debug()) std::cerr << describe(single);
  expect(single.frames.validated && single.frames.resuming_instances == 1U && !single.frames.main_async_all &&
             word_at(single, done, 2U) == FiniteValue::of({1U}) && a7_at(single, after).values() == std::vector<std::uint32_t>{ssp},
         "nested sync: one resuming instance; its writer set is bounded and A7 is kept across the divide");
}

void interrupt_preempts_synchronous() {
  // The divide handler lowers the mask: level 6 preempts it. The level-6 instance's parent is the divide instance; its store and
  // frame reach the divide instance and, because that one resumes into the main flow, the main flow too.
  Asm a;
  a.moveq(3, 1U).store_word(3, cell_b).w({0x80C1U});  // DIVU.W D1,D0
  a.load_word(4, cell_a).load_word(2, cell_b);
  const auto done = a.pc;
  a.stop();
  a.at(sync_a).move_sr(0x2000U).nop().move_sr(0x2700U).rte();
  a.at(irq6).store_imm(5U, cell_a).rte();
  const auto result = run(a, {{5U, sync_a}, {30U, irq6}});
  if (debug()) std::cerr << describe(result);
  std::uint32_t divide_tag = 0U, irq_tag = 0U;
  for (std::uint32_t t = 1U; t < 8U; ++t) {
    if (reached_in(result, sync_a, t)) divide_tag = t;
    if (reached_in(result, irq6, t)) irq_tag = t;
  }
  expect(result.frames.validated && divide_tag != 0U && irq_tag != 0U && result.frames.instances == 2U,
         "preempts sync: a divide instance and a level-6 instance below it");
  const auto entry_a7 = a7_at(result, irq6, irq_tag);
  expect(entry_a7.values() == std::vector<std::uint32_t>{ssp - 2U * m68k_exception_frame_bytes},
         "preempts sync: the level-6 frame is pushed below the divide frame: " + entry_a7.describe());
  expect(!result.frames.main_async_all && word_at(result, done, 4U).is_unknown() && word_at(result, done, 2U) == FiniteValue::of({1U}),
         "preempts sync: cell A is asynchronous for the main flow through the resuming divide instance; cell B stays precise");
}

void unknown_status() {
  // No reset state (a bridge entry): the SR is Unknown, every boundary may be interrupted and no frame address is proven.
  Asm a;
  const auto flow = main_flow(a, std::nullopt);
  a.at(irq6).store_imm(5U, cell_a).load_word(3, cell_a);
  const auto rte = a.pc;
  a.rte();
  const auto result = run(a, irq_only, false);
  if (debug()) std::cerr << describe(result);
  expect(result.frames.validated && status_at(result, flow.read_done).is_unknown() && unanalysed(result, "entry_unknown") >= 1U,
         "unknown SR: every boundary is eligible and the frame address is not proven");
  expect(result.frames.main_async_all && word_at(result, flow.read_done, 2U).is_unknown(),
         "unknown SR: the main flow's writers are every cell (the T004 consequence)");
  expect(reached_in(result, rte, m68k_dead_handler_tag) && word_at(result, rte, 3U, m68k_dead_handler_tag).is_unknown(),
         "unknown SR: the unanalysed handler's own code keeps every cell asynchronous (no unchecked precision)");
}

void unknown_supervisor_stack() {
  // S = 1 is proven but A7 is loaded from never-written memory: the frame address is Unknown.
  Asm a;
  a.w({0x2E79U}).l(cell_c);  // MOVEA.L (cell C).L,A7
  const auto flow = main_flow(a, 0x2000U);
  a.at(irq6).store_imm(5U, cell_a).rte();
  const auto result = run(a, irq_only);
  if (debug()) std::cerr << describe(result);
  expect(result.frames.validated && result.frames.first_round_frame_unknown_a7 >= 1U && unanalysed(result, "entry_unknown") >= 1U &&
             result.frames.main_async_all && word_at(result, flow.read_done, 2U).is_unknown(),
         "unknown SSP: S proven, A7 Unknown: the frame is not proven and every cell is asynchronous");
}

void rte_from_code_built_frame() {
  // MOVE.L #target,-(A7); MOVE.W #$2300,-(A7); RTE: a frame built by analysed code restores PC = target and I = 3.
  Asm a;
  a.push_long(target).push_word(0x2300U);
  const auto rte = a.pc;
  a.rte();
  a.at(target).nop().stop();
  const auto result = run(a, {});
  if (debug()) std::cerr << describe(result);
  const auto *site = return_site(result, rte);
  expect(site != nullptr && site->resolved && site->targets == std::vector<std::uint32_t>{target},
         "RTE: the code-built frame resolves the target");
  expect(status_at(result, target) == FiniteValue::of({0xBU}) && a7_at(result, target).values() == std::vector<std::uint32_t>{ssp},
         "RTE: the restored status is S = 1, I = 3 and A7 pops the 6-byte frame");
  // RTR pops CCR and PC: the target is resolved and the status is unchanged.
  Asm b;
  b.push_long(target).push_word(0U);
  const auto rtr = b.pc;
  b.rtr();
  b.at(target).nop().stop();
  const auto restored = run(b, {});
  const auto *rtr_site = return_site(restored, rtr);
  expect(rtr_site != nullptr && rtr_site->resolved && rtr_site->targets == std::vector<std::uint32_t>{target} &&
             status_at(restored, target) == FiniteValue::of({m68k_reset_status}),
         "RTR: resolved from the code-built frame; the system byte is unchanged");
}

void rte_unproven() {
  // The SR word comes from an Unknown register: the frame is not proven (both SR and PC stay Unknown).
  Asm a;
  a.push_long(target).w({0x3F05U});  // MOVE.W D5,-(A7)
  const auto rte = a.pc;
  a.rte();
  a.at(target).stop();
  const auto result = run(a, {});
  const auto *site = return_site(result, rte);
  expect(site != nullptr && !site->resolved && site->sub == Sub::frame_unproven && !reached_in(result, target, 0U),
         "RTE unproven SR: Unknown(frame_unproven), no edge");
  // A modified frame: the PC slot is overwritten with an Unknown register.
  Asm b;
  b.push_long(target).push_word(0x2300U).w({0x2F45U, 0x0002U});  // MOVE.L D5,2(A7)
  const auto modified_rte = b.pc;
  b.rte();
  b.at(target).stop();
  const auto modified = run(b, {});
  const auto *modified_site = return_site(modified, modified_rte);
  expect(modified_site != nullptr && !modified_site->resolved && modified_site->sub == Sub::frame_unproven,
         "RTE modified frame: Unknown(frame_unproven)");
  // The frame overwritten through an Unknown base: every cell is poisoned.
  Asm c;
  c.push_long(target).push_word(0x2300U).w({0x2079U}).l(cell_c).w({0x2085U});  // MOVEA.L (cell C).L,A0; MOVE.L D5,(A0)
  const auto poisoned_rte = c.pc;
  c.rte();
  c.at(target).stop();
  const auto poisoned = run(c, {});
  const auto *poisoned_site = return_site(poisoned, poisoned_rte);
  expect(poisoned_site != nullptr && !poisoned_site->resolved && poisoned_site->sub == Sub::store_poison,
         "RTE frame overwritten through an Unknown base: Unknown(store_poison)");
}

void computed_rts() {
  // PEA target; RTS and MOVE.L #target,-(A7); RTS: an RTS away from the activation's entry delta returns to the pushed address.
  Asm a;
  a.w({0x4879U}).l(target);  // PEA (target).L
  const auto rts = a.pc;
  a.rts();
  a.at(target).nop().stop();
  const auto result = run(a, {});
  if (debug()) std::cerr << describe(result);
  const auto *site = return_site(result, rts);
  expect(site != nullptr && site->resolved && site->targets == std::vector<std::uint32_t>{target} && reached_in(result, target, 0U) &&
             a7_at(result, target).values() == std::vector<std::uint32_t>{ssp},
         "push + RTS: resolved to the pushed address, A7 popped");
  Asm b;
  b.push_long(target);
  const auto move_rts = b.pc;
  b.rts();
  b.at(target).stop();
  const auto moved = run(b, {});
  const auto *move_site = return_site(moved, move_rts);
  expect(move_site != nullptr && move_site->resolved && move_site->targets == std::vector<std::uint32_t>{target},
         "MOVE.L #target,-(A7) + RTS: resolved");
  // An Unknown pushed value stays Unknown.
  Asm c;
  c.w({0x2F05U});  // MOVE.L D5,-(A7)
  const auto unknown_rts = c.pc;
  c.rts();
  const auto unknown = run(c, {});
  const auto *unknown_site = return_site(unknown, unknown_rts);
  expect(unknown_site != nullptr && !unknown_site->resolved, "push of an Unknown value + RTS: Unknown");
}

void frame_integrity() {
  // The handler rewrites its saved SR (MOVE.W #$2000,(A7)): the status after any boundary where it can be taken is Unknown.
  Asm a;
  const auto flow = main_flow(a, 0x2000U);
  a.at(irq6).w({0x3EBCU, 0x2000U}).rte();  // MOVE.W #$2000,(A7)
  const auto result = run(a, irq_only);
  if (debug()) std::cerr << describe(result);
  // Once the main flow's status is Unknown after its boundaries, the handler's entry is no longer a proven frame either: the returned
  // round reports that cause, with the main partition clobbered.
  expect(result.frames.validated && result.frames.frame_integrity_failures + unanalysed(result, "entry_unknown") >= 1U &&
             result.frames.clobbered_partitions >= 1U && status_at(result, flow.read_done).is_unknown(),
         "integrity: a handler rewriting its saved SR makes the interrupted status Unknown");
  // Writing only the saved PC (2(A7)) leaves the SR intact.
  Asm b;
  const auto pc_flow = main_flow(b, 0x2000U);
  b.at(irq6).w({0x2F7CU}).l(target).w({0x0002U}).rte();  // MOVE.L #target,2(A7)
  b.at(target).stop();
  const auto intact = run(b, irq_only);
  expect(intact.frames.validated && intact.frames.frame_integrity_failures == 0U && status_at(intact, pc_flow.read_done) == FiniteValue::of({8U}),
         "integrity: a write to the saved PC only keeps the interrupted status");
  // Through a subroutine of the handler: its A7 is 4 below the handler's, so 4(A7) is the saved SR and (A7) its own return address.
  constexpr std::uint32_t subroutine = 0x480U;
  const auto through = [&](std::uint32_t displacement) {
    Asm c;
    const auto flow = main_flow(c, 0x2000U);
    c.at(irq6).w({0x6100U, (subroutine - (irq6 + 2U)) & 0xFFFFU}).rte();  // BSR.W subroutine; RTE
    c.at(subroutine).w({0x3F7CU, 0x2000U, displacement}).rts();         // MOVE.W #$2000,d16(A7); RTS
    const auto result = run(c, irq_only);
    if (debug()) std::cerr << describe(result);
    return std::make_pair(result.frames.validated && result.frames.clobbered_partitions == 0U &&
                              status_at(result, flow.read_done) == FiniteValue::of({8U}),
                          result.frames.validated && result.frames.clobbered_partitions >= 1U &&
                              status_at(result, flow.read_done).is_unknown());
  };
  expect(through(4U).second, "integrity: a subroutine of the handler rewriting 4(A7) (the saved SR) clobbers the interrupted status");
  expect(through(0U).first, "integrity: a subroutine of the handler rewriting its own return address keeps the interrupted status");
}


// An installed level-4 autovector the machine model does not deliver is still a potential asynchronous source (real hardware delivers
// it once the program enables its source). SEG-030-T006 iteration 3: wherever level 4 is eligible it enters a handler instance
// exactly like the delivered level 6, analysed for asynchronous writers only (writer-only: never seeded, never in D or the site
// reports), so the main flow's writers are its bounded store set and frame. Where the mask excludes level 4 it has no effect; where
// its entry A7 is Unknown it is an unanalysed resuming interrupt and every cell is asynchronous, as before.
void undelivered_interrupt_source() {
  constexpr std::uint32_t irq4 = 0x600U;
  const std::vector<M68kHandlerVector> potential{{28U, irq4}};
  std::uint32_t irq4_rte = 0U;
  const auto build = [&](std::uint32_t sr) {
    Asm a;
    const auto flow = main_flow(a, sr);
    a.at(irq6).rte();                  // the delivered level-6 handler writes nothing
    a.at(irq4).store_imm(5U, cell_b);  // the installed level-4 handler writes cell B
    irq4_rte = a.pc;
    a.rte();
    return std::make_pair(a, flow);
  };
  const auto [open, flow] = build(0x2300U);  // I = 3: levels 4 and 6 are eligible
  const auto result = run(open, irq_only, true, true, potential);
  if (debug()) std::cerr << describe(result);
  expect(result.complete && result.frames.validated && result.frames.potential_interrupt_vectors == 1U &&
             result.frames.potential_eligible >= 1U && result.frames.writer_only_instances >= 1U &&
             result.frames.writer_only_points >= 2U && result.frames.unanalysed.empty(),
         "undelivered: the installed level-4 handler is analysed as a writer-only instance");
  std::uint32_t tag = 0U;
  for (std::uint32_t t = 1U; t < 8U; ++t)
    if (reached_in(result, irq4, t) && status_at(result, irq4, t) == FiniteValue::of({0xCU})) tag = t;
  expect(tag != 0U && result.writer_only_tags.contains(tag), "undelivered: the instance is entered with S = 1, I = 4, writer-only");
  expect(!result.frames.main_async_all && word_at(result, flow.read_done, 2U).is_unknown() &&
             word_at(result, flow.read_done, 1U) == FiniteValue::of({1U}),
         "undelivered: the main flow's writers are bounded: cell B (its store) is asynchronous, cell A stays precise");
  expect(!result.reached.contains(irq4) && !result.reached.contains(irq4_rte) && !result.return_sites.contains(irq4_rte),
         "undelivered: its handler is never credited (D and the site reports unchanged)");
  const auto [masked, masked_flow] = build(0x2500U);  // I = 5: level 4 masked, level 6 eligible
  const auto closed = run(masked, irq_only, true, true, potential);
  expect(closed.complete && closed.frames.validated && closed.frames.potential_eligible == 0U &&
             closed.frames.writer_only_instances == 0U && closed.writer_only_tags.empty() && !closed.frames.main_async_all &&
             word_at(closed, masked_flow.read_done, 2U) == FiniteValue::of({1U}),
         "undelivered: with level 4 masked (I = 5) there is no instance and cell B stays precise");
  const auto [at_four, four_flow] = build(0x2400U);  // I = 4: level 4 is not eligible at an equal mask
  const auto equal = run(at_four, irq_only, true, true, potential);
  expect(equal.complete && equal.frames.potential_eligible == 0U && word_at(equal, four_flow.read_done, 2U) == FiniteValue::of({1U}),
         "undelivered: level 4 is not eligible at I = 4");
  const auto without = run(open, irq_only);
  expect(!without.frames.main_async_all && word_at(without, flow.read_done, 2U) == FiniteValue::of({1U}),
         "undelivered: the delivered-only premise (the previous, unsound model) would keep cell B precise");
  // An Unknown status and supervisor stack (no reset state): the level-4 entry A7 is Unknown, so it stays an unanalysed resuming
  // interrupt and every cell of the main flow is asynchronous.
  const auto unknown = run(open, irq_only, false, true, potential);
  expect(unknown.complete && unknown.frames.validated && unknown.frames.writer_only_instances == 0U &&
             unanalysed(unknown, "entry_unknown/writer_only") >= 1U && unknown.frames.main_async_all &&
             word_at(unknown, flow.read_done, 1U).is_unknown(),
         "undelivered: an Unknown entry A7 still clobbers (every cell asynchronous)");
}

// The A7 of the points of `pc` in the call-site context of `site` (main partition).
M68kPointsTo a7_in_context(const M68kFiniteAnalysisResult &result, std::uint32_t pc, std::uint32_t site) {
  M68kPointsTo out{};
  for (const auto &[point, state] : m68k_points_of(result, pc))
    if (m68k_point_tag(point) == 0U && m68k_context_site(m68k_point_context(point)) == m68k_call_context(site))
      out = join(out, state->address[7]);
  return out;
}

// A balanced summary returns at its caller's own A7. The nested callee's context is shared by every invocation of its outer callee,
// one from a known A7 and one from an Unknown A7, so the summary's joined exit A7 is Unknown; the continuation in the known
// invocation still keeps that invocation's A7 (the callee's entry A7 + 4). An unbalanced callee has no summary: its continuation's
// A7 stays Unknown.
void balanced_summary_relative_a7() {
  constexpr std::uint32_t outer = 0x300U, inner = 0x340U, unbalanced = 0x380U;
  const auto bsr = [](Asm &a, std::uint32_t callee) { return a.w({0x6100U, (callee - (a.pc + 2U)) & 0xFFFFU}); };
  Asm a;
  const auto known_site = a.pc;
  bsr(a, outer);
  const auto after_known = a.pc;
  a.w({0x2E79U}).l(cell_c);  // MOVEA.L (cell C).L,A7: an Unknown A7 from here on
  bsr(a, outer);
  bsr(a, unbalanced);
  const auto after_unbalanced = a.pc;
  a.nop().stop();
  a.at(outer);
  bsr(a, inner);
  const auto after_nested = a.pc;
  a.nop().rts();
  a.at(inner).nop().rts();
  a.at(unbalanced).w({0x548FU}).rts();  // ADDQ.L #2,A7; RTS (an RTS at delta {2})
  const auto result = run(a, {});
  if (debug()) std::cerr << describe(result);
  expect(result.complete && result.frames.validated, "relative A7: a validated frames round");
  const auto nested = a7_in_context(result, after_nested, known_site);
  expect(nested.is_known() && nested.values() == std::vector<std::uint32_t>{ssp - 4U},
         "relative A7: after the shared nested callee the known invocation keeps its own A7 (SSP - 4): " + nested.describe());
  const auto main_a7 = a7_at(result, after_known);
  expect(main_a7.is_known() && main_a7.values() == std::vector<std::uint32_t>{ssp},
         "relative A7: the outer callee returns at the caller's A7 (SSP): " + main_a7.describe());
  expect(a7_at(result, after_unbalanced).is_unknown(), "relative A7: an unbalanced callee's continuation A7 stays Unknown");
  // A known caller of the unbalanced callee: its continuation is still Unknown (no summary, never rebased).
  Asm b;
  bsr(b, unbalanced);
  const auto after = b.pc;
  b.nop().stop();
  b.at(unbalanced).w({0x548FU}).rts();
  const auto unbalanced_result = run(b, {});
  expect(unbalanced_result.frames.validated && a7_at(unbalanced_result, after).is_unknown(),
         "relative A7: a known caller of an unbalanced callee keeps an Unknown continuation A7");
}

// An instruction that always raises a non-resuming synchronous vector (ILLEGAL) ends its path: its callee stays balanced, so the
// caller's continuation applies the summary (A7 back at SSP). A TRAP whose continuation is not modelled and an unresolved computed
// jump stay unknown effects: their callees are unproven (`none`) and the continuation is opaque.
void non_resuming_raise_ends_path() {
  constexpr std::uint32_t callee = 0x300U;
  const auto build = [&](std::initializer_list<std::uint32_t> escape) {
    Asm a;
    a.w({0x6100U, (callee - (a.pc + 2U)) & 0xFFFFU});  // BSR.W callee
    const auto after = a.pc;
    a.nop().stop();
    a.at(callee).w({0x4A40U, static_cast<std::uint32_t>(0x6700U | (2U * escape.size()))});  // TST.W D0; BEQ.S over the escape
    a.w(escape).rts();
    return std::make_pair(a, after);
  };
  const auto unproven_none = [](const M68kFiniteAnalysisResult &result) {
    const auto found = result.contexts.unproven.find(Sub::none);
    return found == result.contexts.unproven.end() ? std::size_t{0} : found->second;
  };
  const auto [illegal, illegal_after] = build({0x4AFCU});  // ILLEGAL
  const auto proven = run(illegal, {});
  if (debug()) std::cerr << describe(proven);
  const auto a7 = a7_at(proven, illegal_after);
  expect(proven.frames.validated && proven.contexts.summaries == 1U && unproven_none(proven) == 0U && a7.is_known() &&
             a7.values() == std::vector<std::uint32_t>{ssp},
         "non-resuming raise: an ILLEGAL path does not make its callee unproven (summary applied, A7 = SSP): " + a7.describe());
  const auto [trap, trap_after] = build({0x4E40U});  // TRAP #0 (continuation not modelled)
  const auto trapped = run(trap, {});
  expect(trapped.frames.validated && trapped.contexts.summaries == 0U && unproven_none(trapped) == 1U &&
             a7_at(trapped, trap_after).is_unknown(),
         "non-resuming raise: a TRAP whose continuation is not modelled stays an unknown effect");
  const auto [jump, jump_after] = build({0x4ED0U});  // JMP (A0), A0 Unknown
  const auto escaped = run(jump, {});
  expect(escaped.frames.validated && escaped.contexts.summaries == 0U && unproven_none(escaped) == 1U &&
             a7_at(escaped, jump_after).is_unknown(),
         "non-resuming raise: an unresolved computed jump stays an unknown effect");
}

// The status of an opaque continuation is its own partition's bound. The main flow runs at I = 3; the level-6 handler calls an
// unresolved callee (JSR (A0)): its opaque continuation runs with the handler partition's statuses (I = 6), so level 6 is still
// masked there and the handler is not nested at its own level. The whole-program bound would admit the main flow's I = 3 and
// nest it (every cell asynchronous). A handler that itself lowers its mask before the call is still nested.
void per_partition_status_bound() {
  const auto build = [](bool lowers) {
    Asm a;
    const auto flow = main_flow(a, 0x2300U);
    a.at(irq6);
    if (lowers) a.move_sr(0x2300U);
    a.w({0x4E90U});  // JSR (A0), A0 Unknown
    const auto after = a.pc;
    a.nop().rte();
    return std::make_tuple(a, flow, after);
  };
  const auto [a, flow, after] = build(false);
  const auto result = run(a, irq_only);
  if (debug()) std::cerr << describe(result);
  std::uint32_t tag = 0U;
  for (std::uint32_t t = 1U; t < 8U; ++t)
    if (reached_in(result, after, t)) tag = t;
  expect(result.complete && result.frames.validated && result.frames.instances == 1U && tag != 0U,
         "partition bound: one validated level-6 instance");
  expect(status_at(result, after, tag) == FiniteValue::of({0xEU}),
         "partition bound: the handler's opaque continuation runs with the handler's own status (S = 1, I = 6): " +
             status_at(result, after, tag).describe());
  expect(unanalysed(result, "nested") == 0U && !result.frames.main_async_all &&
             word_at(result, flow.read_done, 2U) == FiniteValue::of({1U}),
         "partition bound: the handler is not nested at its own level; cell B stays precise in the main flow");
  const auto [lowered, lowered_flow, lowered_after] = build(true);
  const auto nested = run(lowered, irq_only);
  if (debug()) std::cerr << describe(nested);
  (void)lowered_after;
  expect(nested.frames.validated && unanalysed(nested, "nested") + unanalysed(nested, "entry_unknown") >= 1U &&
             nested.frames.main_async_all &&
             word_at(nested, lowered_flow.read_done, 2U).is_unknown(),
         "partition bound: a handler lowering its own mask is still nested (every cell asynchronous)");
}

}  // namespace

int main() {
  interrupt_masked();
  interrupt_enabled();
  handler_raises_mask();
  handler_lowers_mask();
  nested_synchronous();
  interrupt_preempts_synchronous();
  unknown_status();
  unknown_supervisor_stack();
  rte_from_code_built_frame();
  rte_unproven();
  computed_rts();
  frame_integrity();
  undelivered_interrupt_source();
  balanced_summary_relative_a7();
  non_resuming_raise_ends_path();
  per_partition_status_bound();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_frames_test: all checks passed\n";
  return EXIT_SUCCESS;
}
