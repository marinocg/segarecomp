// SEG-034 (ADR 0081): the generic M68K precision refinements that remove whole-image fallback triggers of the SEG-031 hybrid planner.
//
// Each section names the trigger class it serves and carries its soundness negatives (the refinement must stay fail-closed where its
// premise does not hold). Project-authored synthetic fixtures only: real MC68000 encodings decoded by the unchanged decoder/lifter
// over a flat image at 0 plus a work-RAM region at $E00000 (64 KiB mirrored) and an I/O window at $A00000, reset entry $200 and reset
// SSP $FFFF00. No commercial input.

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
using Sub = M68kAnalysisSubReason;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
bool debug() { return std::getenv("PRECISION_DEBUG") != nullptr; }

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t callee = 0x300U;
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t io_base = 0xA00000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;

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
  Asm &lea_abs(unsigned reg, std::uint32_t address) { return w({0x41F9U | (reg << 9U)}).l(address); }  // LEA (xxx).L,An
  Asm &store_abs_long(std::uint32_t value, std::uint32_t address) { return w({0x23FCU}).l(value).l(address); }  // MOVE.L #v,(xxx).L
  Asm &load_abs_long(unsigned data_reg, std::uint32_t address) { return w({0x2039U | (data_reg << 9U)}).l(address); }  // MOVE.L (xxx).L,Dn
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
    if (address >= io_base && address < work_ram_base) return M68kRegionExtent{M68kRegionKind::io_device, 0U, io_base, work_ram_base - io_base};
    return flat_.region_of(address);
  }

private:
  M68kFlatAnalysisImage flat_;
};

enum class Domains { memory, all };

M68kFiniteAnalysisResult run(const Asm &program, Domains domains) {
  const RegionImage view{program};
  M68kAnalysisConfig config{};
  config.domains.address = true;
  config.domains.memory = true;
  config.domains.contexts = domains == Domains::all;
  config.domains.frames = domains == Domains::all;
  config.frames.main_entries = {entry};
  config.frames.reset_entry = entry;
  config.frames.reset_ssp = ssp;
  return analyze_m68k_finite_values(view, {entry}, config);
}

std::optional<std::vector<std::uint64_t>> data_values(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg) {
  const auto value = m68k_query_data_register(result, pc, reg, 32U);
  if (!value.is_precise()) return std::nullopt;
  return value.values();
}

const M68kReturnSiteReport *return_site(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto found = result.return_sites.find(pc);
  return found == result.return_sites.end() ? nullptr : &found->second;
}

std::size_t opaque(const M68kFiniteAnalysisResult &result, Sub sub) {
  const auto found = result.contexts.opaque_continuations.find(sub);
  return found == result.contexts.opaque_continuations.end() ? 0U : found->second;
}

// ---------------------------------------------------------------------------------------------------------------
// Class: store spill past a region end (return_slot_rewritten; whole-memory poison and asynchronous widening).
//
// A store of `span` bytes whose offsets reach the region's last bytes writes at most `span` bytes of the bus region that follows.
// The memory domain used to fail closed on any such store (every cell poisoned, every return slot rewritten); it now keeps the
// in-region part (clipped to the region's last `span` bytes) and adds the bounded landing store in the next region.

// A long store at $DFFFFE (the last two bytes of the I/O window) lands its upper half on work-RAM physical bytes 0 and 1.
void spill_into_next_region_keeps_other_cells() {
  Asm a;
  a.store_abs_long(0x1111U, 0x00FF0100U);  // survives: far from the landing bytes
  a.store_abs_long(0x2222U, 0x00FF0000U);  // physical bytes 0..3: the landing bytes are in this cell
  a.lea_abs(1, 0x00DFFFFEU).w({0x2281U});  // LEA $DFFFFE,A1; MOVE.L D1,(A1)
  a.load_abs_long(2, 0x00FF0100U);
  a.load_abs_long(3, 0x00FF0000U);
  const auto probe = a.pc;
  a.nop().stop();
  const auto result = run(a, Domains::memory);
  if (debug()) std::cerr << format_m68k_finite_analysis(result);
  const auto surviving = data_values(result, probe, 2);
  const auto landed = data_values(result, probe, 3);
  expect(result.complete && surviving && *surviving == std::vector<std::uint64_t>{0x1111U},
         "spill: a cell outside the landing bytes keeps its value");
  expect(result.complete && !landed, "spill: the cell the spill lands on is Unknown (not kept, not overwritten precisely)");
}

// The same store at $DFFFFC stays inside the I/O window: nothing is touched in work RAM at all.
void no_spill_touches_nothing() {
  Asm a;
  a.store_abs_long(0x2222U, 0x00FF0000U);
  a.lea_abs(1, 0x00DFFFFCU).w({0x2281U});
  a.load_abs_long(3, 0x00FF0000U);
  const auto probe = a.pc;
  a.nop().stop();
  const auto result = run(a, Domains::memory);
  const auto kept = data_values(result, probe, 3);
  expect(result.complete && kept && *kept == std::vector<std::uint64_t>{0x2222U}, "no spill: work RAM is untouched");
}

// A word store at the very last byte ($DFFFFF) lands three bytes in work RAM: still the first cell, still bounded.
void spill_one_byte_in_region() {
  Asm a;
  a.store_abs_long(0x2222U, 0x00FF0000U);
  a.store_abs_long(0x3333U, 0x00FF0008U);
  a.lea_abs(1, 0x00DFFFFFU).w({0x2281U});
  a.load_abs_long(3, 0x00FF0000U);
  a.load_abs_long(4, 0x00FF0008U);
  const auto probe = a.pc;
  a.nop().stop();
  const auto result = run(a, Domains::memory);
  const auto landed = data_values(result, probe, 3);
  const auto far = data_values(result, probe, 4);
  expect(result.complete && !landed && far && *far == std::vector<std::uint64_t>{0x3333U},
         "spill from the last byte: the landing is bounded to the first span bytes of the next region");
}

// A store at the end of work RAM wraps on the 24-bit bus to the cartridge (untracked): only the region's last bytes are affected.
void spill_off_the_end_of_ram() {
  Asm a;
  a.store_abs_long(0x4444U, 0x00FFFFF0U);  // away from the last four bytes
  a.store_abs_long(0x5555U, 0x00FFFFFCU);  // the last cell
  a.lea_abs(1, 0x00FFFFFEU).w({0x2281U});  // MOVE.L D1,($FFFFFE): two bytes in RAM, two on the wrapped bus
  a.load_abs_long(3, 0x00FFFFF0U);
  a.load_abs_long(4, 0x00FFFFFCU);
  const auto probe = a.pc;
  a.nop().stop();
  const auto result = run(a, Domains::memory);
  const auto far = data_values(result, probe, 3);
  const auto last = data_values(result, probe, 4);
  expect(result.complete && far && *far == std::vector<std::uint64_t>{0x4444U} && !last,
         "spill off the end of RAM: the wrapped bytes are untracked, the in-region part still poisons the last cell");
}

// A pointer set of two exact targets, one of them spilling off the end of RAM, must not widen to the hull between them: the cell in
// the middle keeps its value.
void exact_set_with_one_spilling_member_stays_exact() {
  Asm a;
  a.store_abs_long(0x7777U, 0x00FF8000U);  // between the two members: must survive
  a.store_abs_long(0x1111U, 0x00FF0100U);  // the first member
  a.store_abs_long(0x5555U, 0x00FFFFFCU);  // the last cell of RAM (the spilling member's in-region bytes)
  a.lea_abs(1, 0x00FF0100U);
  a.w({0x4A41U, 0x6706U});          // TST.W D1; BEQ.S +6 (skip the second LEA)
  a.lea_abs(1, 0x00FFFFFEU);
  a.w({0x2281U});                   // MOVE.L D1,(A1): A1 is {$FF0100, $FFFFFE}
  a.load_abs_long(2, 0x00FF8000U);
  a.load_abs_long(3, 0x00FF0100U);
  a.load_abs_long(4, 0x00FFFFFCU);
  const auto probe = a.pc;
  a.nop().stop();
  const auto result = run(a, Domains::memory);
  const auto middle = data_values(result, probe, 2);
  const auto first = data_values(result, probe, 3);
  const auto last = data_values(result, probe, 4);
  expect(result.complete && middle && *middle == std::vector<std::uint64_t>{0x7777U},
         "spill in a pointer set: the cell between two exact members keeps its value (no hull)");
  expect(result.complete && !first && !last, "spill in a pointer set: both members' cells are Unknown (weak update)");
}

// The return slot: a called routine storing across the I/O/RAM boundary far from the stack does not rewrite its own slot
// (previously: every slot rewritten, Unknown(return_slot_rewritten), an unproven activation).
void spill_does_not_rewrite_the_return_slot() {
  Asm a;
  a.jsr(callee).nop().stop();
  a.at(callee);
  a.lea_abs(1, 0x00DFFFFEU).w({0x2281U});
  const auto rts = a.pc;
  a.rts();
  const auto result = run(a, Domains::all);
  if (debug()) std::cerr << format_m68k_finite_analysis(result);
  expect(result.complete && return_site(result, rts) == nullptr && opaque(result, Sub::return_slot_rewritten) == 0U &&
             result.contexts.summaries == 1U,
         "spill: the RTS stays an ordinary return of a proven activation");
}

// Negative: a store whose landing bytes ARE the stack slot still rewrites it (RAM end wrapped to physical 0 is not the stack, but
// a RAM-wide store with the stack inside is). The stack of this fixture is $FFFEFC: a long store through a pointer set that includes
// it must stay Unknown(return_slot_rewritten).
void store_over_the_stack_still_rewrites() {
  Asm a;
  a.jsr(callee).nop().stop();
  a.at(callee);
  // MOVEA.L A7,A0 ; MOVE.L D1,(A0): the store hits exactly the return slot.
  a.w({0x204FU, 0x2081U});
  const auto rts = a.pc;
  a.rts();
  const auto result = run(a, Domains::all);
  const auto *site = return_site(result, rts);
  expect(result.complete && site != nullptr && !site->resolved && site->sub == Sub::return_slot_rewritten,
         "negative: a store that lands on the slot still makes it Unknown(return_slot_rewritten)");
}

}  // namespace

int main() {
  spill_into_next_region_keeps_other_cells();
  no_spill_touches_nothing();
  spill_one_byte_in_region();
  spill_off_the_end_of_ram();
  exact_set_with_one_spilling_member_stays_exact();
  spill_does_not_rewrite_the_return_slot();
  store_over_the_stack_still_rewrites();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "analysis_m68k_precision_test: ok\n";
  return 0;
}
