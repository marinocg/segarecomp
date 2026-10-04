// SEG-030-T008 part 2 (ADR 0079 decision 8): seeded randomized differential of the M68K analysis against a bounded concrete
// executor. TEST-ONLY: never linked into a production target (analysis_build_graph_test).
//
// A deterministic generator builds small synthetic MC68000 images (project-authored encodings only; no commercial input) that cover
// field dispatch (an object pointer, a field load and `JSR (An)`, or a masked index and `JMP (d8,PC,Dn.W)`), object loops, calls and
// returns (nested, unbalanced, push-window, and return-slot rewrites: strong, weak, Unknown-base), TRAP/RTE exception frames, SR
// mask changes, stores through known and Unknown bases, and a level-6 interrupt injected at boundaries its mask permits. A small
// interpreter of exactly that subset (an explicit step budget; anything else ends the run) executes each image from the 68000 reset
// state with seeded register and work-RAM inputs, and every run is checked against three analysis configurations (baseline,
// address+memory+contexts, every domain):
//   * a transfer from a site reported resolved lands in its target set;
//   * a value the analysis reports precise before an instruction (D0-D7 at 16 and 32 bits, A0-A7) holds the concrete value;
//   * every reached PC is in D, or the run left D through a typed-Unknown site (`explained`: checking stops there);
//   * an RTS whose slot was rewritten concretely is resolved, typed Unknown, or a site where the return-slot integrity premise was
//     applied (`premise_violation`, counted apart, never sound and never dropped); a normal classification there is unsound.
// SEG-030-T009 correction cycle: interrupt and TRAP handlers write registers (clobbering D0/A1/A2/Dn that feed later dispatches, and
// preserving through MOVEM or MOVE.L save/restore). In `all` (the corrected frames model) every divergence is unsound. In the
// historical configurations (baseline, contexts: they keep the inherited assumption that a handler preserves the registers it
// interrupts) a divergence after a concrete resumption with a changed register is `historical_interrupt_register_assumption` when
// the same image run under the preserving reference semantics (an interrupt's RTE restores D0-D7/A0-A6) is not unsound; it is
// counted apart, never as sound and never dropped. Any other divergence is unsound.
// Determinism: each analysis runs twice and its serialization must be byte-identical; so must the concrete transcript.
// Any unsound result prints the image seed, the configuration, the step and the generated listing (a minimizable reproducer).

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"

namespace {

using namespace segarecomp;

// ---------------------------------------------------------------------------------------------------------------
// Layout.

constexpr std::uint32_t image_size = 0x2000U;
constexpr std::uint32_t main_entry = 0x200U;
constexpr std::uint32_t sub_base = 0x600U;
constexpr std::uint32_t sub_stride = 0x100U;
constexpr std::uint32_t sub_count = 8U;
constexpr std::uint32_t irq_handler = 0x1800U;
constexpr std::uint32_t trap_handler = 0x1900U;
constexpr std::uint32_t pad_base = 0x1A00U;
constexpr std::uint32_t pad_stride = 0x10U;
constexpr std::uint32_t pad_count = 8U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;
constexpr std::uint32_t object_base = 0x00FF2000U;
constexpr std::uint32_t object_stride = 0x40U;
constexpr std::uint32_t object_count = 4U;
constexpr std::uint32_t global_base = 0x00FF3000U;
constexpr std::uint32_t global_count = 8U;
constexpr std::uint32_t step_budget = 3000U;

struct Rng {
  std::uint64_t state;
  std::uint64_t next() {
    std::uint64_t z = (state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31U);
  }
  std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
  bool chance(std::uint32_t one_in) { return below(one_in) == 0U; }
};

// ---------------------------------------------------------------------------------------------------------------
// Assembler with labels (16-bit displacement fixups) and a listing.

struct Asm {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(image_size, 0U);
  std::uint32_t pc = 0U;
  std::uint32_t limit = 0U;  // exclusive end of the current routine slot
  bool overflow = false;
  std::vector<std::optional<std::uint32_t>> labels;
  std::vector<std::pair<std::uint32_t, std::size_t>> fixups;  // (displacement word address, label); disp = label - word address
  std::ostringstream listing;

  void word_at(std::uint32_t address, std::uint32_t value) {
    bytes[address] = static_cast<std::uint8_t>(value >> 8U);
    bytes[address + 1U] = static_cast<std::uint8_t>(value);
  }
  Asm &w(std::uint32_t value) {
    if (pc + 2U > limit) {
      overflow = true;
      return *this;
    }
    word_at(pc, value);
    pc += 2U;
    return *this;
  }
  Asm &l(std::uint32_t value) { return w(value >> 16U).w(value & 0xFFFFU); }
  Asm &note(const std::string &text) {
    listing << std::hex << pc << ": " << text << '\n';
    return *this;
  }
  void begin(std::uint32_t at, std::uint32_t end) {
    pc = at;
    limit = end;
  }
  std::size_t label() {
    labels.emplace_back();
    return labels.size() - 1U;
  }
  void bind(std::size_t id) { labels[id] = pc; }
  Asm &disp_to(std::size_t id) {
    fixups.emplace_back(pc, id);
    return w(0U);
  }
  bool resolve() {
    for (const auto &[at, id] : fixups) {
      if (!labels[id]) return false;
      const auto disp = static_cast<std::int32_t>(*labels[id]) - static_cast<std::int32_t>(at);
      if (disp < -32768 || disp > 32767) return false;
      word_at(at, static_cast<std::uint32_t>(disp) & 0xFFFFU);
    }
    return true;
  }
};

struct Image {
  std::uint64_t seed{};
  std::vector<std::uint8_t> bytes;
  bool irq{};
  bool trap{};
  std::string listing;
  std::array<std::uint32_t, 8> d{};
  std::array<std::uint32_t, 7> a{};  // A0-A6 (A7 is the reset SSP)
  std::uint64_t ram_seed{};
};

class Generator {
public:
  explicit Generator(std::uint64_t seed) : rng_{seed}, seed_(seed) {}

  Image build() {
    Image image;
    image.seed = seed_;
    image.irq = rng_.below(10U) < 6U;
    image.trap = rng_.below(10U) < 4U;
    irq_ = image.irq;
    trap_ = image.trap;
    // Vector table: SSP, reset PC, level-6 autovector (30), TRAP #0 (32).
    a_.begin(0U, 0x100U);
    a_.l(ssp).l(main_entry);
    if (irq_) {
      a_.pc = 30U * 4U;
      a_.l(irq_handler);
    }
    if (trap_) {
      a_.pc = 32U * 4U;
      a_.l(trap_handler);
    }
    // Main flow.
    a_.begin(main_entry, sub_base);
    a_.note("main");
    current_sub_ = -1;
    if (rng_.chance(2U)) sr_change();
    const auto blocks = 3U + rng_.below(6U);
    for (std::uint32_t i = 0; i < blocks; ++i) block(true);
    a_.note("halt").w(0x60FEU);
    // Subroutines (sub i calls only sub j > i).
    for (std::uint32_t i = 0; i < sub_count; ++i) {
      a_.begin(sub_base + i * sub_stride, sub_base + (i + 1U) * sub_stride - 16U);
      current_sub_ = static_cast<int>(i);
      a_.note("sub " + std::to_string(i));
      const auto body = 1U + rng_.below(3U);
      for (std::uint32_t k = 0; k < body; ++k) block(false);
      epilogue();
    }
    // Landing pads (computed-return targets): NOP; RTS or NOP; halt.
    for (std::uint32_t i = 0; i < pad_count; ++i) {
      a_.begin(pad_base + i * pad_stride, pad_base + (i + 1U) * pad_stride);
      a_.note("pad").w(0x4E71U);
      if (rng_.chance(2U)) a_.note("rts").w(0x4E75U);
      else a_.note("halt").w(0x60FEU);
    }
    if (irq_) handler(irq_handler);
    if (trap_) handler(trap_handler);
    image.bytes = a_.resolve() && !a_.overflow ? a_.bytes : std::vector<std::uint8_t>{};
    image.listing = a_.listing.str();
    // Concrete inputs (Unknown to the analysis): registers from a pool that includes return-slot and object addresses.
    const std::array<std::uint32_t, 8> pool{0x00FFFEFCU, 0x00FFFEF8U, 0x00FFFEF4U, global_base, object_base, global_base + 4U, 0U, 8U};
    for (auto &value : image.d) value = rng_.chance(2U) ? pool[rng_.below(8U)] : static_cast<std::uint32_t>(rng_.next());
    for (auto &value : image.a) value = rng_.chance(2U) ? pool[rng_.below(8U)] : static_cast<std::uint32_t>(rng_.next());
    image.ram_seed = rng_.next();
    return image;
  }

private:
  std::uint32_t sub_address(std::uint32_t i) const { return sub_base + i * sub_stride; }
  std::uint32_t pad_address() { return pad_base + rng_.below(pad_count) * pad_stride; }
  std::uint32_t global() { return global_base + 4U * rng_.below(global_count); }
  std::uint32_t object(std::uint32_t k) const { return object_base + k * object_stride; }
  // A callee strictly after the current routine (none from the last one).
  std::optional<std::uint32_t> callee() {
    const auto first = static_cast<std::uint32_t>(current_sub_ + 1);
    if (first >= sub_count) return std::nullopt;
    return first + rng_.below(sub_count - first);
  }
  std::uint32_t code_pointer() {
    if (const auto j = callee(); j && !rng_.chance(5U)) return sub_address(*j);
    return pad_address();
  }
  unsigned data_reg() { return rng_.below(7U); }  // D7 is the loop counter

  void sr_change() {
    static constexpr std::array<std::uint32_t, 4> values{0x2700U, 0x2500U, 0x2300U, 0x2000U};
    const auto sr = values[rng_.below(4U)];
    a_.note("move #sr,sr").w(0x46FCU).w(sr);
  }

  void block(bool main) {
    switch (rng_.below(main ? 15U : 14U)) {
    case 0:
      a_.note("moveq").w(0x7000U | (data_reg() << 9U) | rng_.below(256U));
      break;
    case 1:
      a_.note("move.l #imm,dn").w(0x203CU | (data_reg() << 9U)).l(rng_.chance(2U) ? global() : static_cast<std::uint32_t>(rng_.next()));
      break;
    case 2:
      if (rng_.chance(2U)) a_.note("move.l #imm,abs").w(0x23FCU).l(rng_.below(4U) * 4U).l(global());
      else a_.note("move.l abs,dn").w(0x2039U | (data_reg() << 9U)).l(global());
      break;
    case 3: {  // object init
      const auto k = rng_.below(object_count);
      a_.note("lea obj,a1").w(0x43F9U).l(object(k));
      a_.note("move.l #code,(0,a1)").w(0x237CU).l(code_pointer()).w(0U);
      a_.note("move.w #idx,(4,a1)").w(0x337CU).w(rng_.below(4U) * 4U).w(4U);
      break;
    }
    case 4: {  // object loop init
      const auto count = 1U + rng_.below(object_count);
      a_.note("lea obj0,a1").w(0x43F9U).l(object_base);
      a_.note("moveq #n,d7").w(0x7E00U | (count - 1U));
      const auto loop = a_.label();
      a_.bind(loop);
      a_.note("move.l #code,(0,a1)").w(0x237CU).l(code_pointer()).w(0U);
      a_.note("move.w #idx,(4,a1)").w(0x337CU).w(rng_.below(4U) * 4U).w(4U);
      a_.note("lea ($40,a1),a1").w(0x43E9U).w(object_stride);
      a_.note("dbf d7,loop").w(0x51CFU).disp_to(loop);
      break;
    }
    case 5: {  // field dispatch: object pointer, field load, JSR (A2)
      a_.note("lea obj,a1").w(0x43F9U).l(object(rng_.below(object_count)));
      a_.note("movea.l (0,a1),a2").w(0x2469U).w(0U);
      a_.note("jsr (a2)").w(0x4E92U);
      break;
    }
    case 6: {  // object loop dispatch
      const auto count = 1U + rng_.below(object_count);
      a_.note("lea obj0,a1").w(0x43F9U).l(object_base);
      a_.note("moveq #n,d7").w(0x7E00U | (count - 1U));
      const auto loop = a_.label();
      a_.bind(loop);
      a_.note("movea.l (0,a1),a2").w(0x2469U).w(0U);
      a_.note("jsr (a2)").w(0x4E92U);
      a_.note("lea ($40,a1),a1").w(0x43E9U).w(object_stride);
      a_.note("dbf d7,loop").w(0x51CFU).disp_to(loop);
      break;
    }
    case 7: {  // PC-indexed dispatch from an immediate, a field or a global, masked to the 4-entry table
      switch (rng_.below(3U)) {
      case 0: a_.note("moveq #i*4,d0").w(0x7000U | (rng_.below(4U) * 4U)); break;
      case 1:
        a_.note("lea obj,a1").w(0x43F9U).l(object(rng_.below(object_count)));
        a_.note("move.w (4,a1),d0").w(0x3029U).w(4U);
        break;
      default: a_.note("move.w abs,d0").w(0x3039U).l(global()); break;
      }
      a_.note("andi.w #$c,d0").w(0x0240U).w(0x000CU);
      a_.note("jmp (2,pc,d0.w)").w(0x4EFBU).w(0x0002U);
      std::array<std::size_t, 4> cases{};
      const auto join = a_.label();
      for (auto &label : cases) {
        label = a_.label();
        a_.note("bra.w case").w(0x6000U).disp_to(label);
      }
      for (const auto label : cases) {
        a_.bind(label);
        a_.note("moveq").w(0x7000U | (data_reg() << 9U) | rng_.below(256U));
        a_.note("bra.w join").w(0x6000U).disp_to(join);
      }
      a_.bind(join);
      break;
    }
    case 8: {  // a direct call
      const auto j = callee();
      if (!j) break;
      if (rng_.chance(2U)) a_.note("jsr sub").w(0x4EB9U).l(sub_address(*j));
      else {
        a_.note("bsr.w sub").w(0x6100U);
        const auto at = a_.pc;
        a_.w((sub_address(*j) - at) & 0xFFFFU);
      }
      break;
    }
    case 9: {  // a store through an Unknown base (a register input)
      const auto reg = data_reg();
      a_.note("movea.l dn,a3").w(0x2640U | reg);
      if (rng_.chance(2U)) a_.note("move.l #imm,(a3)").w(0x26BCU).l(rng_.chance(2U) ? pad_address() : rng_.below(16U));
      else a_.note("move.l dn,(a3)").w(0x2680U | data_reg());
      break;
    }
    case 10: {  // a store through a known base
      a_.note("lea glob,a4").w(0x49F9U).l(global());
      if (rng_.chance(2U)) a_.note("move.l dn,(a4)").w(0x2880U | data_reg());
      else a_.note("move.l dn,(4,a4)").w(0x2940U | data_reg()).w(4U);
      break;
    }
    case 11:  // a balanced push/pop
      a_.note("move.l #imm,-(a7)").w(0x2F3CU).l(static_cast<std::uint32_t>(rng_.next()));
      a_.note("addq.l #4,a7").w(0x588FU);
      break;
    case 12:  // a conditional skip
    {
      const auto skip = a_.label();
      a_.note("tst.w dn").w(0x4A40U | data_reg());
      a_.note("beq.w skip").w(0x6700U).disp_to(skip);
      a_.note("addq.w #1,dn").w(0x5240U | data_reg());
      a_.bind(skip);
      break;
    }
    case 13:
      if (trap_) a_.note("trap #0").w(0x4E40U);
      else a_.note("nop").w(0x4E71U);
      break;
    default:
      sr_change();
      break;
    }
  }

  void epilogue() {
    switch (rng_.below(12U)) {
    case 0:  // strong rewrite of the return slot
      a_.note("move.l #pad,(a7)").w(0x2EBCU).l(pad_address());
      break;
    case 1:  // weak rewrite: A0 is the slot or a global
    case 2: {
      a_.note("movea.l a7,a0").w(0x204FU);
      a_.note("tst.w d0").w(0x4A40U);
      a_.note("beq.s +6").w(0x6706U);
      a_.note("lea glob,a0").w(0x41F9U).l(global());
      if (rng_.chance(2U)) a_.note("move.l #pad,(a0)").w(0x20BCU).l(pad_address());
      else a_.note("move.l dn,(a0)").w(0x2080U | data_reg());
      break;
    }
    case 3:  // Unknown-base rewrite (the premise)
      a_.note("movea.l d3,a0").w(0x2043U);
      a_.note("move.l #pad,(a0)").w(0x20BCU).l(pad_address());
      break;
    case 4:  // unbalanced: pop the caller's slot
      a_.note("addq.l #4,a7").w(0x588FU);
      break;
    case 5:  // push window
      a_.note("pea pad").w(0x4879U).l(pad_address());
      break;
    case 6:
      a_.note("move.l #pad,-(a7)").w(0x2F3CU).l(pad_address());
      break;
    default: break;
    }
    a_.note("rts").w(0x4E75U);
  }

  void handler(std::uint32_t at) {
    a_.begin(at, at + 0x100U);
    a_.note(at == irq_handler ? "irq handler" : "trap handler");
    // SEG-030-T009 correction cycle: handlers write registers, both clobbering (D0 with a table index, A2 with a code pointer, A1
    // with an object pointer, any data register) and preserving (MOVEM save/restore of D0/A1/A2, or a MOVE.L push/pop of D0). The
    // clobbered registers feed the main flow's dispatches (D0: JMP (2,PC,D0.W); A2: JSR (A2); A1: the field load).
    const bool save = rng_.chance(3U);
    if (save) a_.note("movem.l d0/a1/a2,-(a7)").w(0x48E7U).w(0x8060U);
    const auto stores = 1U + rng_.below(3U);
    for (std::uint32_t i = 0; i < stores; ++i) {
      if (rng_.chance(4U)) a_.note("move.l #imm,(a5)").w(0x2ABCU).l(rng_.below(16U));
      else if (rng_.chance(3U)) a_.note("move.l #imm,(0,a1)").w(0x237CU).l(code_pointer()).w(0U);
      else a_.note("move.w #imm,abs").w(0x33FCU).w(rng_.below(4U) * 4U).l(global());
    }
    const auto writes = rng_.below(4U);
    for (std::uint32_t i = 0; i < writes; ++i) {
      switch (rng_.below(6U)) {
      case 0: a_.note("moveq #i*4,d0").w(0x7000U | (rng_.below(4U) * 4U)); break;
      case 1: a_.note("movea.l #code,a2").w(0x247CU).l(code_pointer()); break;
      case 2: a_.note("lea obj,a1").w(0x43F9U).l(object(rng_.below(object_count))); break;
      case 3: a_.note("moveq").w(0x7000U | (data_reg() << 9U) | rng_.below(256U)); break;
      case 4:
        a_.note("move.l d0,-(a7)").w(0x2F00U);
        a_.note("moveq #i*4,d0").w(0x7000U | (rng_.below(4U) * 4U));
        a_.note("move.l (a7)+,d0").w(0x201FU);
        break;
      default: a_.note("move.l #imm,dn").w(0x203CU | (data_reg() << 9U)).l(rng_.below(4U) * 4U); break;
      }
    }
    if (save) a_.note("movem.l (a7)+,d0/a1/a2").w(0x4CDFU).w(0x0601U);
    a_.note("rte").w(0x4E73U);
  }

  Rng rng_;
  std::uint64_t seed_;
  Asm a_;
  bool irq_{};
  bool trap_{};
  int current_sub_{-1};
};

// ---------------------------------------------------------------------------------------------------------------
// Concrete executor of exactly the generated subset.

enum class Entry : std::uint8_t { start, flow, dynamic, rts, rte, trap, interrupt };

struct Step {
  std::uint32_t pc{};
  std::array<std::uint32_t, 8> d{};
  std::array<std::uint32_t, 8> a{};
  Entry entry{Entry::start};
  std::uint32_t from{};  // the PC of the transferring instruction (dynamic, rts, rte)
  bool intact{};         // rts: the slot still holds its call's pushed address; rte: an intact interrupt frame
  bool modified{};       // rte from an intact interrupt frame: some D0-D7/A0-A6 differs from its value when the interrupt was taken
};

struct Trace {
  std::vector<Step> steps;
  std::string end;
  std::uint32_t interrupts{};
};

class Machine {
public:
  // `preserve`: the historical reference semantics (SEG-030-T009 correction cycle): an RTE from an intact interrupt frame restores
  // D0-D7/A0-A6 to their values when the interrupt was taken (the inherited assumption of the baseline and non-frames models).
  Machine(const Image &image, std::uint64_t interrupt_seed, bool preserve = false)
      : image_(image), rng_{interrupt_seed}, preserve_(preserve) {
    Rng fill{image.ram_seed};
    for (auto &byte : ram_) byte = static_cast<std::uint8_t>(fill.next());
    for (unsigned i = 0; i < 8U; ++i) d_[i] = image.d[i];
    for (unsigned i = 0; i < 7U; ++i) a_[i] = image.a[i];
    a_[7] = ssp;
    pc_ = main_entry;
  }

  Trace run() {
    Trace trace;
    Entry entry = Entry::start;
    std::uint32_t from = 0U;
    bool intact = false;
    bool modified = false;
    for (std::uint32_t step = 0; step < step_budget; ++step) {
      // A level-6 interrupt at a boundary its mask permits (a handler preserves every register).
      if (image_.irq && ((sr_ >> 8U) & 7U) < 6U && entry == Entry::flow && rng_.chance(6U)) {
        frames_.push_back({a_[7] - 6U, pc_, sr_, true, d_, a_});
        if (!push32(pc_) || !push16(sr_)) return finish(trace, "bus");
        sr_ = static_cast<std::uint16_t>((sr_ & 0xF8FFU) | 0x0600U);
        pc_ = irq_handler;
        entry = Entry::interrupt;
        ++trace.interrupts;
      }
      trace.steps.push_back({pc_, d_, a_, entry, from, intact, modified});
      entry = Entry::flow;
      from = pc_;
      intact = false;
      modified = false;
      const auto result = execute(entry, intact, modified);
      if (result) return finish(trace, *result);
    }
    return finish(trace, "budget");
  }

private:
  struct Frame {
    std::uint32_t address;
    std::uint32_t pc;
    std::uint16_t sr;
    bool interrupt;
    std::array<std::uint32_t, 8> d;  // the registers when the exception was taken
    std::array<std::uint32_t, 8> a;
  };

  static Trace finish(Trace &trace, const std::string &why) {
    trace.end = why;
    return std::move(trace);
  }

  // Bus: the image at 0 (writes ignored), work RAM at $E00000-$FFFFFF mirrored every 64 KiB; anything else ends the run.
  std::optional<std::uint32_t> read(std::uint32_t address, unsigned bytes) const {
    address &= 0xFFFFFFU;
    if (bytes > 1U && (address & 1U) != 0U) return std::nullopt;
    std::uint32_t value = 0U;
    for (unsigned i = 0; i < bytes; ++i) {
      const auto at = (address + i) & 0xFFFFFFU;
      std::uint8_t byte = 0U;
      if (at < image_size) byte = image_.bytes[at];
      else if (at >= work_ram_base) byte = ram_[at & 0xFFFFU];
      else return std::nullopt;
      value = (value << 8U) | byte;
    }
    return value;
  }
  bool write(std::uint32_t address, std::uint32_t value, unsigned bytes, bool call_push = false) {
    address &= 0xFFFFFFU;
    if (bytes > 1U && (address & 1U) != 0U) return false;
    for (unsigned i = 0; i < bytes; ++i) {
      const auto at = (address + i) & 0xFFFFFFU;
      if (at < image_size) continue;  // ROM: ignored
      if (at < work_ram_base) return false;
      const auto physical = at & 0xFFFFU;
      ram_[physical] = static_cast<std::uint8_t>(value >> (8U * (bytes - 1U - i)));
      for (std::uint32_t k = 0; k < 4U; ++k) call_slots_.erase((physical - k) & 0xFFFFU);
    }
    if (call_push) call_slots_[address & 0xFFFFU] = value;
    return true;
  }
  bool push32(std::uint32_t value, bool call = false) {
    a_[7] -= 4U;
    return write(a_[7], value, 4U, call);
  }
  bool push16(std::uint32_t value) {
    a_[7] -= 2U;
    return write(a_[7], value, 2U);
  }
  std::optional<std::uint32_t> fetch() {
    if (pc_ + 2U > image_size || (pc_ & 1U) != 0U) return std::nullopt;
    const auto value = static_cast<std::uint32_t>(image_.bytes[pc_] << 8U | image_.bytes[pc_ + 1U]);
    pc_ += 2U;
    return value;
  }
  std::optional<std::uint32_t> fetch32() {
    const auto high = fetch();
    const auto low = fetch();
    if (!high || !low) return std::nullopt;
    return *high << 16U | *low;
  }
  static std::uint32_t sext16(std::uint32_t v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v))); }

  // Executes one instruction; returns the reason the run ends, or nullopt.
  std::optional<std::string> execute(Entry &entry, bool &intact, bool &modified) {
    const auto at = pc_;
    const auto op = fetch();
    if (!op) return std::string("fetch");
    const auto w = *op;
    const unsigned high = (w >> 9U) & 7U, low = w & 7U;
    const auto ext = [&]() { return fetch(); };
    const auto lit = [&]() { return fetch32(); };
    if (w == 0x4E71U) return std::nullopt;
    if (w == 0x60FEU) return std::string("halt");
    if ((w & 0xF100U) == 0x7000U) {
      d_[high] = sext16(static_cast<std::uint32_t>(static_cast<std::int8_t>(w & 0xFFU)) & 0xFFFFU);
      return std::nullopt;
    }
    if ((w & 0xF1FFU) == 0x203CU) {
      const auto v = lit();
      if (!v) return std::string("fetch");
      d_[high] = *v;
      return std::nullopt;
    }
    if ((w & 0xF1FFU) == 0x41F9U) {  // LEA abs.L,An
      const auto v = lit();
      if (!v) return std::string("fetch");
      a_[high] = *v;
      return std::nullopt;
    }
    if ((w & 0xF1F8U) == 0x41E8U) {  // LEA d16(An),Am
      const auto v = ext();
      if (!v) return std::string("fetch");
      a_[high] = a_[low] + sext16(*v);
      return std::nullopt;
    }
    if ((w & 0xF1F8U) == 0x2040U) {
      a_[high] = d_[low];
      return std::nullopt;
    }
    if ((w & 0xF1FFU) == 0x207CU) {  // MOVEA.L #imm,An
      const auto v = lit();
      if (!v) return std::string("fetch");
      a_[high] = *v;
      return std::nullopt;
    }
    if ((w & 0xFFF8U) == 0x2F00U) {  // MOVE.L Dn,-(A7)
      return push32(d_[low]) ? std::nullopt : std::optional<std::string>("bus");
    }
    if ((w & 0xF1FFU) == 0x201FU) {  // MOVE.L (A7)+,Dn
      const auto value = read(a_[7], 4U);
      if (!value) return std::string("bus");
      a_[7] += 4U;
      d_[high] = *value;
      return std::nullopt;
    }
    if (w == 0x48E7U || w == 0x4CDFU) {  // MOVEM.L list,-(A7) / MOVEM.L (A7)+,list (A7 never in a generated list)
      const auto mask = ext();
      if (!mask) return std::string("fetch");
      const auto reg = [&](unsigned index) -> std::uint32_t & { return index < 8U ? d_[index] : a_[index - 8U]; };
      if (w == 0x48E7U) {
        for (unsigned index = 16U; index-- > 0U;)
          if (((*mask >> (15U - index)) & 1U) != 0U && !push32(reg(index))) return std::string("bus");
      } else {
        for (unsigned index = 0; index < 16U; ++index) {
          if (((*mask >> index) & 1U) == 0U) continue;
          const auto value = read(a_[7], 4U);
          if (!value) return std::string("bus");
          a_[7] += 4U;
          reg(index) = *value;
        }
      }
      return std::nullopt;
    }
    if ((w & 0xF1F8U) == 0x2048U) {
      a_[high] = a_[low];
      return std::nullopt;
    }
    if ((w & 0xF1F8U) == 0x2068U) {  // MOVEA.L (d16,An),Am
      const auto v = ext();
      if (!v) return std::string("fetch");
      const auto value = read(a_[low] + sext16(*v), 4U);
      if (!value) return std::string("bus");
      a_[high] = *value;
      return std::nullopt;
    }
    if ((w & 0xF1F8U) == 0x2140U) {  // MOVE.L Dn,(d16,An)
      const auto v = ext();
      if (!v) return std::string("fetch");
      return write(a_[high] + sext16(*v), d_[low], 4U) ? std::nullopt : std::optional<std::string>("bus");
    }
    if ((w & 0xF1FFU) == 0x217CU || (w & 0xF1FFU) == 0x317CU) {  // MOVE.L/W #imm,(d16,An)
      const bool is_long = (w & 0xF000U) == 0x2000U;
      const auto value = is_long ? lit() : ext();
      const auto disp = ext();
      if (!value || !disp) return std::string("fetch");
      return write(a_[high] + sext16(*disp), *value, is_long ? 4U : 2U) ? std::nullopt : std::optional<std::string>("bus");
    }
    if ((w & 0xF1F8U) == 0x3028U) {  // MOVE.W (d16,An),Dn
      const auto v = ext();
      if (!v) return std::string("fetch");
      const auto value = read(a_[low] + sext16(*v), 2U);
      if (!value) return std::string("bus");
      d_[high] = (d_[high] & 0xFFFF0000U) | *value;
      return std::nullopt;
    }
    if (w == 0x23FCU || w == 0x33FCU) {  // MOVE.L/W #imm,abs.L
      const bool is_long = w == 0x23FCU;
      const auto value = is_long ? lit() : ext();
      const auto address = lit();
      if (!value || !address) return std::string("fetch");
      return write(*address, *value, is_long ? 4U : 2U) ? std::nullopt : std::optional<std::string>("bus");
    }
    if ((w & 0xF1FFU) == 0x2039U || (w & 0xF1FFU) == 0x3039U) {  // MOVE.L/W abs.L,Dn
      const bool is_long = (w & 0xF000U) == 0x2000U;
      const auto address = lit();
      if (!address) return std::string("fetch");
      const auto value = read(*address, is_long ? 4U : 2U);
      if (!value) return std::string("bus");
      d_[high] = is_long ? *value : (d_[high] & 0xFFFF0000U) | *value;
      return std::nullopt;
    }
    if ((w & 0xF1F8U) == 0x2080U) {  // MOVE.L Dn,(An)
      return write(a_[high], d_[low], 4U) ? std::nullopt : std::optional<std::string>("bus");
    }
    if ((w & 0xF1FFU) == 0x20BCU) {  // MOVE.L #imm,(An)
      const auto value = lit();
      if (!value) return std::string("fetch");
      return write(a_[high], *value, 4U) ? std::nullopt : std::optional<std::string>("bus");
    }
    if (w == 0x2F3CU || w == 0x4879U) {  // MOVE.L #imm,-(A7) / PEA abs.L
      const auto value = lit();
      if (!value) return std::string("fetch");
      return push32(*value) ? std::nullopt : std::optional<std::string>("bus");
    }
    if (w == 0x588FU) {
      a_[7] += 4U;
      return std::nullopt;
    }
    if ((w & 0xFFF8U) == 0x5240U) {
      d_[low] = (d_[low] & 0xFFFF0000U) | ((d_[low] + 1U) & 0xFFFFU);
      return std::nullopt;
    }
    if ((w & 0xFFF8U) == 0x4A40U) {
      zero_ = (d_[low] & 0xFFFFU) == 0U;
      return std::nullopt;
    }
    if ((w & 0xFFF8U) == 0x0240U) {  // ANDI.W #imm,Dn
      const auto v = ext();
      if (!v) return std::string("fetch");
      d_[low] = (d_[low] & 0xFFFF0000U) | (d_[low] & *v & 0xFFFFU);
      return std::nullopt;
    }
    if ((w & 0xFF00U) == 0x6700U || (w & 0xFF00U) == 0x6000U || (w & 0xFF00U) == 0x6100U) {
      const auto base = pc_;
      std::uint32_t disp = sext16(static_cast<std::uint32_t>(static_cast<std::int8_t>(w & 0xFFU)) & 0xFFFFU);
      if ((w & 0xFFU) == 0U) {
        const auto v = ext();
        if (!v) return std::string("fetch");
        disp = sext16(*v);
      }
      const auto target = base + disp;
      if ((w & 0xFF00U) == 0x6100U) {
        if (!push32(pc_, true)) return std::string("bus");
        pc_ = target;
      } else if ((w & 0xFF00U) == 0x6000U || zero_) {
        pc_ = target;
      }
      return std::nullopt;
    }
    if ((w & 0xFFF8U) == 0x51C8U) {  // DBF Dn
      const auto base = pc_;
      const auto v = ext();
      if (!v) return std::string("fetch");
      const auto counter = (d_[low] - 1U) & 0xFFFFU;
      d_[low] = (d_[low] & 0xFFFF0000U) | counter;
      if (counter != 0xFFFFU) pc_ = base + sext16(*v);
      return std::nullopt;
    }
    if (w == 0x4EB9U) {
      const auto target = lit();
      if (!target) return std::string("fetch");
      if (!push32(pc_, true)) return std::string("bus");
      pc_ = *target & 0xFFFFFFU;
      return std::nullopt;
    }
    if ((w & 0xFFF8U) == 0x4E90U || (w & 0xFFF8U) == 0x4ED0U) {  // JSR/JMP (An)
      if ((w & 0xFFF8U) == 0x4E90U && !push32(pc_, true)) return std::string("bus");
      pc_ = a_[low] & 0xFFFFFFU;
      entry = Entry::dynamic;
      return std::nullopt;
    }
    if (w == 0x4EFBU) {  // JMP (d8,PC,D0.W)
      const auto base = pc_;
      const auto v = ext();
      if (!v) return std::string("fetch");
      const auto d8 = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(*v & 0xFFU)));
      pc_ = (base + d8 + sext16(d_[(*v >> 12U) & 7U] & 0xFFFFU)) & 0xFFFFFFU;
      entry = Entry::dynamic;
      return std::nullopt;
    }
    if (w == 0x4E75U) {  // RTS
      const auto slot = a_[7] & 0xFFFFU;
      const auto value = read(a_[7], 4U);
      if (!value) return std::string("bus");
      const auto found = call_slots_.find(slot);
      intact = found != call_slots_.end() && found->second == *value && (a_[7] & 0xFFFFFFU) >= work_ram_base;
      a_[7] += 4U;
      pc_ = *value & 0xFFFFFFU;
      entry = Entry::rts;
      return std::nullopt;
    }
    if (w == 0x4E73U) {  // RTE
      const auto sr = read(a_[7], 2U);
      const auto pc = read(a_[7] + 2U, 4U);
      if (!sr || !pc) return std::string("bus");
      intact = !frames_.empty() && frames_.back().address == a_[7] && frames_.back().pc == *pc && frames_.back().sr == *sr &&
               frames_.back().interrupt;
      if (intact) {
        const auto &frame = frames_.back();
        for (unsigned i = 0; i < 8U; ++i) modified = modified || d_[i] != frame.d[i] || (i < 7U && a_[i] != frame.a[i]);
        if (preserve_) {
          d_ = frame.d;
          for (unsigned i = 0; i < 7U; ++i) a_[i] = frame.a[i];
        }
      }
      if (!frames_.empty() && frames_.back().address == a_[7]) frames_.pop_back();
      sr_ = static_cast<std::uint16_t>(*sr);
      a_[7] += 6U;
      pc_ = *pc & 0xFFFFFFU;
      entry = Entry::rte;
      return std::nullopt;
    }
    if (w == 0x4E40U) {  // TRAP #0
      frames_.push_back({a_[7] - 6U, pc_, sr_, false, d_, a_});
      if (!push32(pc_) || !push16(sr_)) return std::string("bus");
      sr_ = static_cast<std::uint16_t>(sr_ | 0x2000U);
      const auto vector = read(32U * 4U, 4U);
      if (!vector || *vector == 0U) return std::string("vector");
      pc_ = *vector;
      entry = Entry::trap;
      return std::nullopt;
    }
    if (w == 0x46FCU) {
      const auto v = ext();
      if (!v) return std::string("fetch");
      sr_ = static_cast<std::uint16_t>(*v);
      return std::nullopt;
    }
    (void)at;
    return std::string("unsupported");
  }

  const Image &image_;
  Rng rng_;
  bool preserve_{};
  std::array<std::uint8_t, 0x10000> ram_{};
  std::array<std::uint32_t, 8> d_{};
  std::array<std::uint32_t, 8> a_{};
  std::uint32_t pc_{};
  std::uint16_t sr_{0x2700U};
  bool zero_{};
  std::map<std::uint32_t, std::uint32_t> call_slots_;  // physical work-RAM offset -> the return address a call pushed there
  std::vector<Frame> frames_;
};

std::string transcript(const Trace &trace) {
  std::ostringstream out;
  for (const auto &step : trace.steps) {
    out << step.pc << ':' << static_cast<int>(step.entry) << ':' << step.intact;
    for (const auto v : step.d) out << ',' << v;
    for (const auto v : step.a) out << ',' << v;
    out << '\n';
  }
  return out.str() + trace.end;
}

// ---------------------------------------------------------------------------------------------------------------
// Analysis configurations and checks.

enum class Mode : std::uint8_t { baseline, contexts, all };
const char *mode_name(Mode mode) {
  switch (mode) {
  case Mode::baseline: return "baseline";
  case Mode::contexts: return "contexts";
  case Mode::all: return "all";
  }
  return "?";
}

class RegionImage final : public M68kAnalysisImage {
public:
  explicit RegionImage(const std::vector<std::uint8_t> &bytes) : flat_(bytes, 0U) {}
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

M68kFiniteAnalysisResult analyze(const Image &image, Mode mode) {
  const RegionImage view{image.bytes};
  M68kAnalysisConfig config{};
  config.call_continuations = true;
  config.exception_continuations = false;
  config.pushed_code_continuations = false;
  std::vector<std::uint32_t> entries{main_entry};
  std::vector<M68kHandlerVector> vectors;
  if (image.irq) vectors.push_back({30U, irq_handler});
  if (image.trap) vectors.push_back({32U, trap_handler});
  for (const auto &vector : vectors) entries.push_back(vector.handler);
  if (mode != Mode::baseline) {
    config.domains.address = config.domains.memory = config.domains.contexts = true;
    config.domains.frames = mode == Mode::all;
    config.memory.interrupts = mode != Mode::all;
    for (const auto &vector : vectors) config.memory.handler_roots.push_back(vector.handler);
    if (mode == Mode::all) {
      config.frames.vectors = vectors;
      config.frames.main_entries = {main_entry};
      config.frames.reset_entry = main_entry;
      config.frames.reset_ssp = ssp;
    }
  }
  return analyze_m68k_finite_values(view, entries, config);
}

// `historical`: a divergence of a historical configuration (baseline, contexts) caused only by handler register writes (SEG-030-T009
// correction cycle; ADR 0079 decision 8): those models keep the inherited assumption that a handler preserves the registers it
// interrupts. It is counted apart, never as sound and never dropped; in `all` (the corrected model) every divergence is unsound.
enum class Verdict : std::uint8_t { clean, explained, premise_violation, incomplete, historical, unsound };

struct Totals {
  std::size_t runs{}, clean{}, explained{}, premise_violation{}, incomplete{}, historical{}, unsound{};
  std::size_t modified_resumptions{};  // concrete RTEs from an intact interrupt frame with some register changed by the handler
  std::size_t steps_checked{}, precise_values{}, resolved_transfers{}, slot_rewrite_returns{}, interrupts{};
};

struct Checked {
  Verdict verdict{Verdict::clean};
  std::string why;
  std::size_t step{};
  bool after_modified{};  // an interrupt resumed with a register its handler changed at or before `step`
};

// The status of a dynamic site: resolved with targets, typed Unknown, or not classified.
struct SiteStatus {
  enum Kind : std::uint8_t { resolved, unknown, none } kind{none};
  std::vector<std::uint32_t> targets;
};
SiteStatus dynamic_site(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  if (const auto found = result.pc_index_sites.find(pc); found != result.pc_index_sites.end()) {
    if (found->second.outcome == M68kPcIndexOutcome::resolved) return {SiteStatus::resolved, found->second.targets};
    return {SiteStatus::unknown, {}};
  }
  if (const auto found = result.address_sites.find(pc); found != result.address_sites.end()) {
    if (found->second.resolved) return {SiteStatus::resolved, found->second.targets};
    return {SiteStatus::unknown, {}};
  }
  if (result.unresolved_computed.contains(pc)) return {SiteStatus::unknown, {}};
  return {};
}
SiteStatus return_site(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  if (const auto found = result.return_sites.find(pc); found != result.return_sites.end()) {
    if (found->second.resolved) return {SiteStatus::resolved, found->second.targets};
    return {SiteStatus::unknown, {}};
  }
  if (result.unresolved_computed.contains(pc)) return {SiteStatus::unknown, {}};
  return {};
}
bool contains(const std::vector<std::uint32_t> &targets, std::uint32_t pc) {
  return std::find(targets.begin(), targets.end(), pc) != targets.end();
}

Checked check(const M68kFiniteAnalysisResult &result, const Trace &trace, Mode mode, Totals &totals) {
  if (!result.complete) return {Verdict::incomplete, "analysis incomplete (typed bound)", 0U};
  bool modified = false;
  const auto unsound = [&](std::size_t step, const std::string &why) { return Checked{Verdict::unsound, why, step, modified}; };
  for (std::size_t i = 0; i < trace.steps.size(); ++i) {
    const auto &step = trace.steps[i];
    if (step.modified) {
      modified = true;
      ++totals.modified_resumptions;
    }
    std::ostringstream where;
    where << "pc " << std::hex << step.pc << " from " << step.from;
    // How the step was entered.
    switch (step.entry) {
    case Entry::start:
    case Entry::flow:
    case Entry::trap:
    case Entry::interrupt: break;
    case Entry::dynamic: {
      const auto site = dynamic_site(result, step.from);
      if (site.kind == SiteStatus::resolved) {
        ++totals.resolved_transfers;
        if (!contains(site.targets, step.pc)) return unsound(i, "resolved site target outside its set: " + where.str());
      } else if (site.kind == SiteStatus::unknown) {
        return {Verdict::explained, "typed-Unknown dynamic site", i};
      } else {
        return unsound(i, "unclassified dynamic site: " + where.str());
      }
      break;
    }
    case Entry::rts: {
      if (step.intact) break;  // the call's own continuation
      ++totals.slot_rewrite_returns;
      const auto site = return_site(result, step.from);
      if (site.kind == SiteStatus::resolved) {
        ++totals.resolved_transfers;
        if (!contains(site.targets, step.pc)) return unsound(i, "resolved return outside its set: " + where.str());
        break;
      }
      if (site.kind == SiteStatus::unknown) return {Verdict::explained, "typed-Unknown return site", i};
      // The baseline (no memory domain) inherits the premise wholesale; otherwise only a counted premise site may be violated.
      if (mode == Mode::baseline || result.return_slots.premise_pcs.contains(step.from))
        return {Verdict::premise_violation, "return slot rewritten under the return-slot integrity premise", i};
      return unsound(i, "rewritten return slot classified normal: " + where.str());
    }
    case Entry::rte: {
      const auto site = return_site(result, step.from);
      if (site.kind == SiteStatus::resolved) {
        ++totals.resolved_transfers;
        if (!contains(site.targets, step.pc)) return unsound(i, "resolved RTE outside its set: " + where.str());
        break;
      }
      // An intact interrupt frame resumes the preempted flow (the analysis's resumption premise); a TRAP's continuation is not
      // modelled (typed Unknown: interrupt_resumption, or the rte family without frames).
      if (step.intact) break;
      return {Verdict::explained, "typed-Unknown RTE (exception continuation not modelled)", i};
    }
    }
    if (!result.reached.contains(step.pc)) return unsound(i, "reached PC outside D: " + where.str());
    ++totals.steps_checked;
    for (unsigned reg = 0; reg < 8U; ++reg) {
      for (const auto width : {16U, 32U}) {
        const auto value = m68k_query_data_register(result, step.pc, reg, width);
        if (!value.is_precise()) continue;
        ++totals.precise_values;
        const auto concrete = width == 16U ? step.d[reg] & 0xFFFFU : step.d[reg];
        if (std::find(value.values().begin(), value.values().end(), concrete) == value.values().end())
          return unsound(i, "precise D" + std::to_string(reg) + "." + std::to_string(width) + " excludes the concrete value: " + where.str());
      }
      const auto pointer = m68k_query_address_register(result, step.pc, reg);
      if (const auto values = pointer.values()) {
        ++totals.precise_values;
        if (!contains(*values, step.a[reg]))
          return unsound(i, "precise A" + std::to_string(reg) + " excludes the concrete value: " + where.str());
      }
    }
  }
  return {};
}

}  // namespace

int main(int argc, char **argv) {
  // A fixed seed list: seeds base + 0 .. base + count - 1.
  std::uint64_t base = UINT64_C(0x5E6030008);
  std::size_t count = 400U;
  if (argc > 1) count = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  if (argc > 2) base = std::strtoull(argv[2], nullptr, 0);
  const auto started = std::chrono::steady_clock::now();
  std::map<Mode, Totals> totals;
  std::size_t images = 0U, rejected = 0U, failures = 0U;
  std::map<std::string, std::size_t> ends;
  for (std::size_t n = 0; n < count; ++n) {
    const auto seed = base + n;
    const auto image = Generator{seed}.build();
    if (image.bytes.empty()) {
      ++rejected;  // a routine overflowed its slot (deterministic; never silently counted as checked)
      continue;
    }
    ++images;
    const auto trace = Machine{image, seed ^ UINT64_C(0xA5A5A5A5)}.run();
    // The same run under the historical reference semantics (an interrupt's RTE restores D0-D7/A0-A6): it attributes a historical
    // configuration's divergence to handler register writes only when that run is not itself unsound.
    const auto preserved = Machine{image, seed ^ UINT64_C(0xA5A5A5A5), true}.run();
    if (transcript(trace) != transcript(Machine{image, seed ^ UINT64_C(0xA5A5A5A5)}.run())) {
      std::cerr << "FAIL: non-deterministic concrete run, seed " << seed << '\n';
      ++failures;
    }
    ++ends[trace.end];
    for (const auto mode : {Mode::baseline, Mode::contexts, Mode::all}) {
      auto &sum = totals[mode];
      const auto result = analyze(image, mode);
      if (format_m68k_finite_analysis(result) != format_m68k_finite_analysis(analyze(image, mode))) {
        std::cerr << "FAIL: non-deterministic analysis, seed " << seed << " mode " << mode_name(mode) << '\n';
        ++failures;
      }
      ++sum.runs;
      sum.interrupts += trace.interrupts;
      auto checked = check(result, trace, mode, sum);
      if (checked.verdict == Verdict::unsound && mode != Mode::all && checked.after_modified) {
        Totals scratch;
        if (check(result, preserved, mode, scratch).verdict != Verdict::unsound) checked.verdict = Verdict::historical;
      }
      switch (checked.verdict) {
      case Verdict::clean: ++sum.clean; break;
      case Verdict::explained: ++sum.explained; break;
      case Verdict::premise_violation: ++sum.premise_violation; break;
      case Verdict::incomplete: ++sum.incomplete; break;
      case Verdict::historical: ++sum.historical; break;
      case Verdict::unsound:
        ++sum.unsound;
        ++failures;
        std::cerr << "UNSOUND seed=" << seed << " mode=" << mode_name(mode) << " step=" << checked.step << ": " << checked.why << '\n'
                  << image.listing << "--- concrete steps ---\n";
        for (std::size_t i = checked.step >= 12U ? checked.step - 12U : 0U; i <= checked.step && i < trace.steps.size(); ++i)
          std::cerr << std::hex << trace.steps[i].pc << std::dec << " entry=" << static_cast<int>(trace.steps[i].entry) << '\n';
        if (std::getenv("DIFFERENTIAL_DEBUG") != nullptr) std::cerr << format_m68k_finite_analysis(result);
        break;
      }
    }
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "analysis_m68k_differential_test: seeds " << base << ".." << base + count - 1U << " images=" << images
            << " rejected=" << rejected << " seconds=" << elapsed << '\n';
  for (const auto &[why, n] : ends) std::cout << "  concrete end " << why << '=' << n << '\n';
  for (const auto &[mode, sum] : totals)
    std::cout << "  " << mode_name(mode) << ": runs=" << sum.runs << " clean=" << sum.clean << " explained=" << sum.explained
              << " premise_violation=" << sum.premise_violation << " incomplete=" << sum.incomplete
              << " historical_interrupt_register_assumption=" << sum.historical << " unsound=" << sum.unsound
              << " steps_checked=" << sum.steps_checked << " precise_values=" << sum.precise_values
              << " resolved_transfers=" << sum.resolved_transfers << " slot_rewrite_returns=" << sum.slot_rewrite_returns
              << " interrupts=" << sum.interrupts << " modified_resumptions=" << sum.modified_resumptions << '\n';
  if (failures != 0U) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  return 0;
}
