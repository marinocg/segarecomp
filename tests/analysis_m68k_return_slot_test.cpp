// SEG-030-T008 (ADR 0079 decision 8): the return slot of an RTS at its activation's entry stack delta.
//
// Precise proof OR typed Unknown, plus one named premise: an RTS at the entry delta is a normal return only when its return cell
// (A7).L holds the address its call pushed, or the recorded slot cannot have been written; a precise cell with another value is a
// computed return resolved to that set; a slot a known-target store may have rewritten is Unknown(return_slot_rewritten); what an
// Unknown-target store, an opaque callee, an asynchronous or external writer or an untracked A7 may do to the slot is the
// return-slot integrity premise, counted per site and never applied against precise or weak knowledge of a contradicting write.
//
// Project-authored synthetic fixtures only: real MC68000 encodings over a flat image at 0 plus a work-RAM region at $E00000 (64 KiB
// mirrored), reset entry $200 and reset SSP $FFFF00. No commercial input.

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
using analysis::UnknownReason;
using Sub = M68kAnalysisSubReason;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
bool debug() { return std::getenv("RETURN_SLOT_DEBUG") != nullptr; }

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t callee = 0x300U;
constexpr std::uint32_t target = 0x400U;
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;
constexpr std::uint32_t other_cell = 0x00FF0100U;

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
  Asm &jsr(std::uint32_t address) { return w({0x4EB9U}).l(address); }  // JSR (xxx).L
  Asm &nop() { return w({0x4E71U}); }
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

enum class Domains { baseline, memory, contexts, all };

M68kFiniteAnalysisResult run(const Asm &program, Domains domains) {
  const RegionImage view{program};
  M68kAnalysisConfig config{};
  config.domains.address = domains != Domains::baseline;
  config.domains.memory = domains != Domains::baseline;
  config.domains.contexts = domains == Domains::contexts || domains == Domains::all;
  config.domains.frames = domains == Domains::all;
  config.frames.main_entries = {entry};
  config.frames.reset_entry = entry;
  config.frames.reset_ssp = ssp;
  return analyze_m68k_finite_values(view, {entry}, config);
}

const M68kReturnSiteReport *return_site(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto found = result.return_sites.find(pc);
  return found == result.return_sites.end() ? nullptr : &found->second;
}
bool reached(const M68kFiniteAnalysisResult &result, std::uint32_t pc) { return result.reached.contains(pc); }
std::size_t premise(const M68kFiniteAnalysisResult &result, M68kReturnSlotPremise cause) {
  const auto found = result.return_slots.premise_by_cause.find(cause);
  return found == result.return_slots.premise_by_cause.end() ? 0U : found->second;
}
std::size_t opaque(const M68kFiniteAnalysisResult &result, Sub sub) {
  const auto found = result.contexts.opaque_continuations.find(sub);
  return found == result.contexts.opaque_continuations.end() ? 0U : found->second;
}

// $200: JSR ($300).L; $206: NOP; BRA *; $300: <body>; RTS. Returns the RTS PC.
std::uint32_t call_shape(Asm &a, const std::vector<std::uint32_t> &body) {
  a.jsr(callee).nop().stop();
  a.at(callee);
  for (const auto word : body) a.w({word});
  const auto rts = a.pc;
  a.rts();
  a.at(target).nop().stop();
  return rts;
}

// ---------------------------------------------------------------------------------------------------------------

void reproducer_resolved() {
  // MOVE.L #$400,(A7) overwrites the return slot strongly: the RTS is a computed return resolved to {$400}.
  Asm a;
  const auto rts = call_shape(a, {0x2EBCU, 0x0000U, target});  // MOVE.L #$400,(A7)
  const auto result = run(a, Domains::all);
  if (debug()) std::cerr << format_m68k_finite_analysis(result);
  const auto *site = return_site(result, rts);
  expect(result.complete && site != nullptr && site->resolved && site->targets == std::vector<std::uint32_t>{target},
         "reproducer: the RTS is a computed return resolved to {$400}");
  expect(reached(result, target), "reproducer: $400 is in D");
  expect(result.return_slots.computed_sites == 1U && result.return_slots.premise_sites == 0U,
         "reproducer: one computed return-slot site, no premise");
  expect(result.contexts.summaries == 0U && opaque(result, Sub::return_slot_rewritten) == 1U,
         "reproducer: the callee is not a proven balanced activation (opaque return_slot_rewritten continuation)");
}

void reproducer_baseline_premise() {
  // The same shape with the baseline domains keeps the inherited behaviour (no memory: the premise, unreported).
  Asm a;
  const auto rts = call_shape(a, {0x2EBCU, 0x0000U, target});
  const auto baseline = run(a, Domains::baseline);
  expect(baseline.complete && !reached(baseline, target) && return_site(baseline, rts) == nullptr && !baseline.return_slots.enabled,
         "baseline: unchanged (the RTS is an ordinary return; $400 is not in D)");
  // Memory without frames: A7 is never located (no reset SSP), so the store is an Unknown-target store and the slot is untracked:
  // the premise, counted.
  const auto memory = run(a, Domains::memory);
  expect(memory.complete && !reached(memory, target) && memory.return_slots.premise_sites == 1U &&
             premise(memory, M68kReturnSlotPremise::slot_untracked) == 1U && memory.return_slots.premise_pcs.contains(rts),
         "memory only: premise (slot_untracked), counted");
  const auto contexts = run(a, Domains::contexts);
  expect(contexts.complete && !reached(contexts, target) && premise(contexts, M68kReturnSlotPremise::slot_untracked) == 1U,
         "contexts without frames: premise (slot_untracked), counted");
}

void normal_return() {
  Asm a;
  const auto rts = call_shape(a, {0x4E71U});  // NOP
  const auto result = run(a, Domains::all);
  expect(result.complete && return_site(result, rts) == nullptr && result.return_slots.normal_sites == 1U &&
             result.return_slots.premise_sites == 0U && result.contexts.summaries == 1U && !reached(result, target),
         "untouched slot: normal return with a summary");
  // Rewriting the slot with the pushed address itself is still a normal return.
  Asm b;
  const auto same = call_shape(b, {0x2EBCU, 0x0000U, 0x0206U});  // MOVE.L #$206,(A7)
  const auto same_result = run(b, Domains::all);
  expect(same_result.complete && return_site(same_result, same) == nullptr && same_result.return_slots.normal_sites == 1U &&
             same_result.contexts.summaries == 1U,
         "slot rewritten with its own return address: normal");
}

// MOVEA.L A7,A0; TST.W D0; BEQ.S +6; LEA ($FF0100).L,A0; <store through A0>: A0 is {slot, other cell}, a weak update.
std::vector<std::uint32_t> weak_prefix() {
  return {0x204FU, 0x4A40U, 0x6706U, 0x41F9U, other_cell >> 16U, other_cell & 0xFFFFU};
}

void weak_rewrite_unknown_value() {
  Asm a;
  auto body = weak_prefix();
  body.push_back(0x2082U);  // MOVE.L D2,(A0)  (D2 Unknown)
  const auto rts = call_shape(a, body);
  const auto result = run(a, Domains::all);
  if (debug()) std::cerr << format_m68k_finite_analysis(result);
  const auto *site = return_site(result, rts);
  expect(result.complete && site != nullptr && !site->resolved && site->sub == Sub::return_slot_rewritten,
         "weak rewrite with an Unknown value: Unknown(return_slot_rewritten)");
  expect(result.return_slots.unknown_sites == 1U && result.return_slots.premise_sites == 0U && result.contexts.summaries == 0U,
         "weak rewrite: never the premise, never a proven activation");
}

void weak_rewrite_precise_value() {
  Asm a;
  auto body = weak_prefix();
  body.insert(body.end(), {0x20BCU, 0x0000U, target});  // MOVE.L #$400,(A0)
  const auto rts = call_shape(a, body);
  const auto result = run(a, Domains::all);
  const auto *site = return_site(result, rts);
  expect(result.complete && site != nullptr && site->resolved && site->targets == std::vector<std::uint32_t>{0x206U, target} &&
             reached(result, target),
         "weak rewrite with a precise value: computed return resolved to {$206, $400}");
}

void unknown_base_store_premise() {
  Asm a;
  const auto rts = call_shape(a, {0x2043U, 0x20BCU, 0x0000U, target});  // MOVEA.L D3,A0; MOVE.L #$400,(A0)
  const auto result = run(a, Domains::all);
  expect(result.complete && return_site(result, rts) == nullptr && !reached(result, target) &&
             result.return_slots.premise_sites == 1U && premise(result, M68kReturnSlotPremise::unknown_target_store) == 1U &&
             result.return_slots.premise_pcs.contains(rts),
         "Unknown-target store: the return-slot integrity premise, counted (return_slot_premise_sites)");
  // A later precise rewrite is knowledge of a contradicting write: never the premise.
  Asm b;
  const auto later = call_shape(b, {0x2043U, 0x20BCU, 0x0000U, target, 0x2EBCU, 0x0000U, target});
  const auto rewritten = run(b, Domains::all);
  const auto *site = return_site(rewritten, later);
  expect(rewritten.complete && site != nullptr && site->resolved && site->targets == std::vector<std::uint32_t>{target} &&
             rewritten.return_slots.premise_sites == 0U,
         "Unknown-target store then a precise rewrite: computed, not the premise");
}

void root_return_unknown() {
  // An RTS at the reset root: no call pushed its slot, the cell is initial memory: a typed Unknown site.
  Asm a;
  const auto rts = a.pc;
  a.rts();
  const auto result = run(a, Domains::all);
  const auto *site = return_site(result, rts);
  expect(result.complete && site != nullptr && !site->resolved && site->sub == Sub::initial_memory,
         "root RTS: Unknown(initial_memory)");
}

void deterministic() {
  Asm a;
  (void)call_shape(a, {0x2EBCU, 0x0000U, target});
  expect(format_m68k_finite_analysis(run(a, Domains::all)) == format_m68k_finite_analysis(run(a, Domains::all)),
         "deterministic output");
}

}  // namespace

int main() {
  reproducer_resolved();
  reproducer_baseline_premise();
  normal_return();
  weak_rewrite_unknown_value();
  weak_rewrite_precise_value();
  unknown_base_store_premise();
  root_return_unknown();
  deterministic();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "analysis_m68k_return_slot_test: ok\n";
  return 0;
}
