// SEG-030-T004 (ADR 0079 decisions 7, 9, 11): the CPU-owned M68K abstract memory, object-field identity and stack/frame/object
// alias exclusion, with the asynchronous-writer (handler) model and the external-writer (Z80 release) policy.
//
// Project-authored synthetic fixtures only: real MC68000 encodings decoded by the unchanged decoder/lifter over a small flat
// cartridge image plus a work-RAM and an I/O region extent, so exact synthetic addresses may be asserted. No commercial input.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/abstract_memory.hpp"
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
constexpr std::uint32_t io_base = 0xA00000U;
constexpr std::uint32_t object = 0x00FF0100U;  // an object in work RAM; its routine field is at +$10

// A tiny assembler over the flat image: `pc` advances with every emitted word.
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
  // BEQ.S/BRA.S to `target` (from the current pc).
  Asm &branch(std::uint32_t opcode, std::uint32_t target) {
    const auto displacement = static_cast<std::uint32_t>(static_cast<std::int32_t>(target) - static_cast<std::int32_t>(pc + 2U));
    return w({opcode | (displacement & 0xFFU)});
  }
  Asm &lea_object(unsigned reg) { return w({0x41F9U | (reg << 9U)}).l(object); }  // LEA object,An
  Asm &moveq0() { return w({0x7000U}); }                                         // MOVEQ #0,D0
  Asm &store_field(std::uint32_t value) { return w({0x117CU, value, 0x0010U}); }  // MOVE.B #value,$10(A0)
  Asm &load_field() { return w({0x1028U, 0x0010U}); }                            // MOVE.B $10(A0),D0
  // JMP 2(PC,D0.W): the targets are the instruction's extension-word address + 2 + D0.
  std::uint32_t dispatch() {
    const auto site = pc;
    w({0x4EFBU, 0x0002U});
    return site;
  }
};

// The flat cartridge (one immutable image region) plus work RAM ($E00000-$FFFFFF, 64 KiB mirror) and an I/O window.
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
    if (address >= io_base && address < work_ram_base)
      return M68kRegionExtent{M68kRegionKind::io_device, 0U, io_base, work_ram_base - io_base};
    return flat_.region_of(address);
  }

private:
  M68kFlatAnalysisImage flat_;
};

struct Options {
  bool memory = true;
  bool interrupts = false;
  std::vector<std::uint32_t> handlers;
  bool release_range = false;
  bool assume_no_external = false;
};

M68kFiniteAnalysisResult run(const Asm &program, const Options &options = {}) {
  const RegionImage view{program};
  M68kAnalysisConfig config{};
  config.domains.address = true;
  config.domains.memory = options.memory;
  config.memory.interrupts = options.interrupts;
  config.memory.handler_roots = options.handlers;
  if (options.release_range) config.memory.release_ranges.emplace_back(0xA11000U, 0xA12000U);
  config.memory.assume_no_external_writer = options.assume_no_external;
  std::vector<std::uint32_t> entries{entry};
  entries.insert(entries.end(), options.handlers.begin(), options.handlers.end());
  return analyze_m68k_finite_values(view, entries, config);
}

const M68kPcIndexSiteReport *site_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto found = result.pc_index_sites.find(pc);
  return found == result.pc_index_sites.end() ? nullptr : &found->second;
}

bool resolved(const M68kFiniteAnalysisResult &result, std::uint32_t pc, const std::vector<std::uint32_t> &targets) {
  const auto *site = site_at(result, pc);
  return result.complete && site != nullptr && site->outcome == M68kPcIndexOutcome::resolved && site->targets == targets;
}

bool unresolved(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto *site = site_at(result, pc);
  return result.complete && site != nullptr && site->outcome != M68kPcIndexOutcome::resolved && site->targets.empty() &&
         !result.solution.computed_targets.contains(pc);
}

std::size_t unknown_reads(const M68kFiniteAnalysisResult &result, Sub sub) {
  std::size_t count = 0U;
  for (const auto &[key, n] : result.memory.unknown_reads)
    if (key.second == sub) count += n;
  return count;
}

std::string site_text(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto *site = site_at(result, pc);
  return site == nullptr ? "no site" : m68k_pc_index_outcome_name(site->outcome);
}

// ---------------------------------------------------------------------------------------------------------------
// Lattice and store semantics.

void lattice() {
  const M68kRegion ram{M68kRegionKind::mutable_ram, 0U, work_ram_base, 0x200000U, 0x10000U};
  const M68kRegion high{M68kRegionKind::mutable_ram, 0U, 0xFF000000U | work_ram_base, 0x200000U, 0x10000U};
  const auto at = [&](const M68kRegion &region, std::vector<std::uint32_t> offsets) {
    return M68kPointsTo::of({{region, M68kOffsetSet::of(std::move(offsets))}});
  };
  const auto value = [](std::vector<std::uint64_t> values) {
    M68kCellValue out;
    out.data = FiniteValue::of(std::move(values));
    return out;
  };
  const M68kMemoryPolicy none{};
  M68kAbstractMemory memory;
  memory.absent = Sub::initial_memory;
  m68k_memory_store(memory, at(ram, {0x100U}), 1U, value({4U}), none);
  auto read = m68k_memory_read(memory, at(ram, {0x100U}), 1U, none);
  expect(read.known && read.value.data == FiniteValue::of({4U}), "memory: a singleton store is a strong update");
  // The 64 KiB mirror and the register upper byte name the same physical byte.
  read = m68k_memory_read(memory, at(high, {0x10100U}), 1U, none);
  expect(read.known && read.value.data == FiniteValue::of({4U}), "memory: mirrored and upper-byte aliases read the same cell");
  // A weak update (two exact targets) joins the matching cell; an absent cell stays Unknown.
  m68k_memory_store(memory, at(ram, {0x100U, 0x200U}), 1U, value({8U}), none);
  read = m68k_memory_read(memory, at(ram, {0x100U}), 1U, none);
  expect(read.known && read.value.data == FiniteValue::of({4U, 8U}), "memory: a weak update joins (never overwrites)");
  read = m68k_memory_read(memory, at(ram, {0x200U}), 1U, none);
  expect(!read.known && read.sub == Sub::initial_memory && read.reason == UnknownReason::unknown_input,
         "memory: a weak update over an absent cell stays Unknown(initial_memory)");
  // An overlapping store of another width removes the cell; a disjoint one does not.
  m68k_memory_store(memory, at(ram, {0x102U}), 2U, value({1U}), none);
  expect(m68k_memory_read(memory, at(ram, {0x100U}), 1U, none).known, "memory: a disjoint store leaves the cell");
  m68k_memory_store(memory, at(ram, {0x0FFU}), 2U, std::nullopt, none);
  expect(!m68k_memory_read(memory, at(ram, {0x100U}), 1U, none).known, "memory: an overlapping store removes the cell");
  // Join is the intersection of cells, pointwise.
  M68kAbstractMemory a, b;
  m68k_memory_store(a, at(ram, {0x10U}), 1U, value({1U}), none);
  m68k_memory_store(a, at(ram, {0x20U}), 1U, value({2U}), none);
  m68k_memory_store(b, at(ram, {0x10U}), 1U, value({3U}), none);
  const auto joined = join(a, b);
  expect(joined.cells.size() == 1U && leq(a, joined) && leq(b, joined) && !leq(joined, a),
         "memory: join keeps the common cells only, and is an upper bound");
  // An Unknown target poisons every cell.
  m68k_memory_store(a, M68kPointsTo::unknown(UnknownReason::unknown_input), 1U, value({0U}), none);
  expect(a.cells.empty() && a.absent == Sub::store_poison, "memory: an Unknown-target store poisons every cell");
  // Summary cell: a strided store is a weak update of every member; reading the stride over uninitialized RAM stays Unknown.
  M68kAbstractMemory summary;
  summary.absent = Sub::initial_memory;
  const auto stride = M68kPointsTo::of({{ram, M68kOffsetSet::strided(0x1000U, 0x40U, 0x1000U + 0x40U * 99U)}});
  m68k_memory_store(summary, stride, 1U, value({3U}), none);
  read = m68k_memory_read(summary, stride, 1U, none);
  expect(!read.known && read.sub == Sub::initial_memory, "summary cell: a strided store over uninitialized RAM reads Unknown");
  // Policy: asynchronous and external writers.
  M68kMemoryPolicy async{};
  async.add_async({M68kRegionKind::mutable_ram, 0U, 0x100U, 0x101U});
  M68kAbstractMemory guarded;
  m68k_memory_store(guarded, at(ram, {0x100U}), 1U, value({1U}), async);
  m68k_memory_store(guarded, at(ram, {0x110U}), 1U, value({1U}), async);
  expect(m68k_memory_read(guarded, at(ram, {0x100U}), 1U, async).sub == Sub::async_writer &&
             m68k_memory_read(guarded, at(ram, {0x110U}), 1U, async).known,
         "policy: an asynchronous cell is never stored and reads Unknown(async_writer); a disjoint cell survives");
  M68kMemoryPolicy external{};
  external.external_writer = true;
  expect(m68k_memory_read(guarded, at(ram, {0x110U}), 1U, external).sub == Sub::external_writer,
         "policy: an external writer makes every work-RAM read Unknown(external_writer)");
  expect(leq(async, join(async, external)) && !leq(external, async), "policy: the round configuration is a join-semilattice");
  // The cell bound.
  M68kAbstractMemory full;
  for (std::uint32_t i = 0; i <= m68k_memory_cell_bound; ++i) m68k_memory_store(full, at(ram, {2U * i}), 1U, value({1U}), none);
  expect(full.cells.empty() && full.absent == Sub::set_bound, "memory: more than the cell bound drops every cell (state_bound)");
}

// ---------------------------------------------------------------------------------------------------------------
// Fixtures.

// TST.B D1; BEQ a; MOVE.B #0,$10(A0); BRA join; a: MOVE.B #4,$10(A0); join: [interference] MOVEQ #0,D0; MOVE.B $10(A0),D0;
// JMP 2(PC,D0.W). The routine field's store set is complete on every path.
struct Dispatch {
  Asm program;
  std::uint32_t site{};
  std::uint32_t base{};  // the target of D0 = 0
};

Dispatch field_dispatch_program(std::initializer_list<std::uint32_t> interference = {}, std::uint32_t stack = 0U) {
  Dispatch out;
  auto &a = out.program;
  if (stack != 0U) a.w({0x2E7CU}).l(stack);  // MOVEA.L #stack,A7
  a.lea_object(0);
  a.w({0x4A01U});  // TST.B D1
  const auto beq = a.pc;
  a.w({0x6700U});  // patched below
  a.store_field(0U);
  const auto bra = a.pc;
  a.w({0x6000U});
  const auto other = a.pc;
  a.store_field(4U);
  const auto joined = a.pc;
  a.w(interference);
  a.moveq0().load_field();
  out.site = a.dispatch();
  out.base = out.site + 4U;
  a.w({0x4E71U, 0x4E71U, 0x4E71U, 0x60FEU});  // the targets (base, base + 4) and a stop
  a.at(beq).branch(0x6700U, other);
  a.at(bra).branch(0x6000U, joined);
  return out;
}

void field_dispatch() {
  const auto program = field_dispatch_program();
  const auto result = run(program.program);
  expect(resolved(result, program.site, {program.base, program.base + 4U}),
         "field dispatch: a complete store set resolves the width-only field index exactly: " + site_text(result, program.site));
  expect(result.memory.converged && result.memory.precise_reads >= 1U, "field dispatch: converged with a precise field read");
  Options off{};
  off.memory = false;
  expect(unresolved(run(program.program, off), program.site), "field dispatch: without the memory domain the field is width-only");
}

void interfering_unknown_base_store() {
  // MOVE.B D2,(A1): A1 is Unknown, so the store may alias the field.
  const auto program = field_dispatch_program({0x1282U});
  const auto result = run(program.program);
  expect(unresolved(result, program.site) && unknown_reads(result, Sub::store_poison) >= 1U,
         "interference: an Unknown-base store poisons the field (store_poison): " + site_text(result, program.site));
  expect(result.memory.unknown_target_stores >= 1U, "interference: the Unknown-target store is counted");
}

void stack_store_excluded() {
  // MOVE.L D3,-(A7) with a known A7 ($FFFF00) cannot alias the field at $FF0110; PEA and the A7-relative frame keep it too.
  const auto program = field_dispatch_program({0x2F03U, 0x4879U, 0x0000U, 0x0300U}, 0x00FFFF00U);  // MOVE.L D3,-(A7); PEA ($300).L
  const auto result = run(program.program);
  expect(resolved(result, program.site, {program.base, program.base + 4U}),
         "stack exclusion: pushes at a known A7 offset leave the field: " + site_text(result, program.site));
  // The same pushes with an Unknown A7 may hit any cell.
  const auto unknown_stack = field_dispatch_program({0x2F03U});
  expect(unresolved(run(unknown_stack.program), unknown_stack.site), "stack exclusion: a push through an Unknown A7 poisons");
  // SEG-030-T008: a stack writer that actually aliases is never excluded. With A7 = field + 4, PEA writes its long at A7 - 4, i.e.
  // over the field byte: the byte cell is removed and the field read is Unknown (a push placed at A7 would miss it).
  const auto aliasing = field_dispatch_program({0x4879U, 0x0000U, 0x0300U}, object + 0x14U);  // PEA ($300).L
  expect(unresolved(run(aliasing.program), aliasing.site),
         "stack aliasing: a push below a known A7 that overlaps the field removes it: " + site_text(run(aliasing.program), aliasing.site));
}

// A handler root: MOVE.B #8,(target).L; RTE.
std::uint32_t handler(Asm &program, std::uint32_t at, std::uint32_t target) {
  program.at(at).w({0x13FCU, 0x0008U}).l(target).w({0x4E73U});
  return at;
}

void interrupt_handler_writer() {
  auto program = field_dispatch_program();
  const auto irq = handler(program.program, 0x600U, object + 0x10U);
  Options options{};
  options.handlers = {irq};
  options.interrupts = true;
  auto result = run(program.program, options);
  expect(unresolved(result, program.site) && result.memory.policy.async_all && unknown_reads(result, Sub::async_writer) >= 1U,
         "IRQ writer: with interrupts every cell is asynchronous (Unknown async_writer): " + site_text(result, program.site));
  // Synchronous-only handlers (no interrupt can preempt): the asynchronous set is the handler's own store cells.
  options.interrupts = false;
  result = run(program.program, options);
  expect(unresolved(result, program.site) && !result.memory.policy.async_all && result.memory.policy.async.size() == 1U,
         "handler writer: the handler's field store makes the field asynchronous: " + site_text(result, program.site));
  auto disjoint = field_dispatch_program();
  const auto other = handler(disjoint.program, 0x600U, object + 0x20U);
  options.handlers = {other};
  result = run(disjoint.program, options);
  expect(resolved(result, disjoint.site, {disjoint.base, disjoint.base + 4U}) && result.memory.rounds == 2U,
         "handler writer: a disjoint handler store leaves the field (round 2 validates round 1's policy): " + site_text(result, disjoint.site));
}

void nested_trap_and_interrupt() {
  // A TRAP handler writes and reads the field, an IRQ6 handler writes it too. With interrupts the IRQ can preempt the TRAP handler;
  // even when it cannot, the field has another (asynchronous) writer: the TRAP handler's read is Unknown either way.
  Asm program;
  program.w({0x4E40U});  // TRAP #0 (the startup entry)
  const auto trap = program.at(0x400U).pc;
  program.lea_object(0).store_field(4U).moveq0().load_field();
  const auto site = program.dispatch();
  program.w({0x4E71U, 0x4E71U, 0x4E71U, 0x4E73U});
  const auto irq = handler(program, 0x600U, object + 0x10U);
  for (const bool interrupts : {true, false}) {
    Options options{};
    options.handlers = {trap, irq};
    options.interrupts = interrupts;
    const auto result = run(program, options);
    expect(unresolved(result, site) && unknown_reads(result, Sub::async_writer) >= 1U,
           std::string("nested: the TRAP handler's read of an IRQ-written field is Unknown(async_writer), interrupts=") +
               (interrupts ? "on" : "off"));
  }
  Options alone{};
  alone.handlers = {trap};
  // The writing-handler exemption ADR 0079 decision 7 permits is not taken: a handler's own store cells are asynchronous in every
  // context, the writing handler included (conservative; never false certainty).
  const auto own = run(program, alone);
  expect(unresolved(own, site) && unknown_reads(own, Sub::async_writer) >= 1U,
         "nested: even without the IRQ writer a handler's own store cell is asynchronous (no writer exemption)");
}

void auto_update_self_alias() {
  // LEA object,A0; MOVE.L #1,(A0); MOVE.L #2,4(A0); MOVE.L (A0)+,(A0); LEA object,A1; MOVE.L 4(A1),D0; NOP; BRA *.
  // The destination address is evaluated after the source's post-increment: object+4 receives 1.
  Asm program;
  program.lea_object(0).w({0x20BCU}).l(1U).w({0x217CU}).l(2U).w({0x0004U, 0x2098U}).lea_object(1).w({0x2029U, 0x0004U});
  const auto after = program.pc;
  program.w({0x4E71U, 0x60FEU});
  const auto result = run(program);
  const auto d0 = m68k_query_data_register(result, after, 0, 32U);
  expect(d0 == FiniteValue::of({1U}), "self-alias: MOVE.L (A0)+,(A0) writes the post-incremented address: " + d0.describe());
}

void invalidated_store_proof() {
  // LEA object,A0; MOVE.B #0,$10(A0); loop: MOVEQ #0,D0; MOVE.B $10(A0),D0; JMP 2(PC,D0.W); target: MOVE.B D5,$10(A0); BRA loop.
  // The site resolves from the stored 0, but the code it exposes stores an Unknown value into the field and loops back: the proof
  // loses its targets, so the solver pins the site (invalidated) and restarts.
  Asm program;
  program.lea_object(0).store_field(0U);
  const auto loop = program.pc;
  program.moveq0().load_field();
  const auto site = program.dispatch();
  program.w({0x1145U, 0x0010U});
  program.branch(0x6000U, loop);
  const auto result = run(program);
  const auto *report = site_at(result, site);
  expect(result.complete && report != nullptr && report->outcome == M68kPcIndexOutcome::invalidated && report->targets.empty(),
         "invalidation: a store-derived proof undone by the code it exposed is pinned: " + site_text(result, site));
  expect(!result.reached.contains(site + 4U), "invalidation: the pinned site's target is not discovered");
}

void z80_release() {
  // MOVE.W #$0100,($A11200).L (release the Z80 reset) before the field dispatch: the Z80 may then write work RAM at any time.
  const auto releasing = field_dispatch_program({0x33FCU, 0x0100U, 0x00A1U, 0x1200U});
  Options options{};
  options.release_range = true;
  auto result = run(releasing.program, options);
  expect(unresolved(result, releasing.site) && result.memory.policy.external_writer && result.memory.release_store &&
             unknown_reads(result, Sub::external_writer) >= 1U,
         "Z80 release: a release store makes every work-RAM read Unknown(external_writer): " + site_text(result, releasing.site));
  // The labelled diagnostic premise ablation (never credited) measures the sensitivity.
  options.assume_no_external = true;
  result = run(releasing.program, options);
  expect(resolved(result, releasing.site, {releasing.base, releasing.base + 4U}) && result.memory.assumed_no_external_writer &&
             result.memory.release_store && !result.memory.policy.external_writer,
         "Z80 ablation: assuming no Z80 RAM writes the field resolves (diagnostic only)");
  // A program that never releases the Z80 keeps its memory facts.
  const auto quiet = field_dispatch_program();
  options.assume_no_external = false;
  result = run(quiet.program, options);
  expect(resolved(result, quiet.site, {quiet.base, quiet.base + 4U}) && !result.memory.release_store &&
             !result.memory.policy.external_writer,
         "Z80: a program that never releases the Z80 keeps its memory facts");
}

void deterministic_and_inert() {
  const auto program = field_dispatch_program();
  expect(format_m68k_finite_analysis(run(program.program)) == format_m68k_finite_analysis(run(program.program)),
         "determinism: two memory runs format identically");
  Options off{};
  off.memory = false;
  const auto baseline = run(program.program, off);
  bool empty = true;
  for (const auto &[point, state] : baseline.solution.in_states) {
    (void)point;
    empty = empty && state.memory.cells.empty() && state.memory.absent == Sub::none;
  }
  expect(empty && !baseline.memory.enabled, "inert: without the memory domain no state carries memory");
}

}  // namespace

int main() {
  lattice();
  field_dispatch();
  interfering_unknown_base_store();
  stack_store_excluded();
  interrupt_handler_writer();
  nested_trap_and_interrupt();
  auto_update_self_alias();
  invalidated_store_proof();
  z80_release();
  deterministic_and_inert();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_memory_test: all checks passed\n";
  return EXIT_SUCCESS;
}
