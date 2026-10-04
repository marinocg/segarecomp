// SEG-030-T006 (ADR 0079 decision 7): the named Genesis main-68000 interrupt-source premise of the report-only analysis driver.
//
// Under the Genesis board premise the potential interrupt sources are the installed handlers of levels 2, 4 and 6 only (level 6 is
// the delivered vector; levels 2 and 4 are installed but undelivered); under the unconfigured MC68000 default every installed
// interrupt vector is one. The CPU analysis takes the selected set as configuration and analyses each source as a writer-only handler
// instance, so a level-7 handler's store is an asynchronous writer under the default but not under the Genesis premise, while the
// level-2 and level-4 handlers' stores are writers under both.
//
// Project-authored synthetic fixtures only: real MC68000 encodings over a small flat image plus a work-RAM region. No commercial input.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"
#include "segarecomp/genesis_analysis_report/interrupt_premise.hpp"

namespace {

using namespace segarecomp;
using analysis::FiniteValue;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t irq2 = 0x400U, irq4 = 0x480U, irq6 = 0x500U, irq7 = 0x580U, spurious = 0x600U;
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;
constexpr std::uint32_t ssp = 0x00FFFF00U;
constexpr std::uint32_t cell_2 = 0x00FF0100U, cell_4 = 0x00FF0200U, cell_7 = 0x00FF0300U, cell_s = 0x00FF0400U;

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
  Asm &move_sr(std::uint32_t sr) { return w({0x46FCU, sr}); }                                            // MOVE #sr,SR
  Asm &store_imm(std::uint32_t value, std::uint32_t address) { return w({0x33FCU, value}).l(address); }  // MOVE.W #v,(xxx).L
  Asm &load_word(unsigned reg, std::uint32_t address) { return w({0x3039U | (reg << 9U)}).l(address); }  // MOVE.W (xxx).L,Dn
  Asm &rte() { return w({0x4E73U}); }
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
      return M68kRegionExtent{M68kRegionKind::mutable_ram, 0U, work_ram_base, 0x1000000U - work_ram_base, 0x10000U};
    return flat_.region_of(address);
  }

private:
  M68kFlatAnalysisImage flat_;
};

// Reset (S = 1, I = 7), then I = 1: every level from 2 up is eligible. Each cell is stored, then read back: D1 = level-2 cell,
// D2 = level-4 cell, D3 = level-7 cell, D4 = spurious-vector cell. Every installed handler stores 5 into its own cell.
struct Program {
  Asm a;
  std::uint32_t read_done{};
};
Program build() {
  Program p;
  auto &a = p.a;
  a.move_sr(0x2100U);
  for (const auto cell : {cell_2, cell_4, cell_7, cell_s}) a.store_imm(1U, cell);
  a.load_word(1, cell_2).load_word(2, cell_4).load_word(3, cell_7).load_word(4, cell_s);
  p.read_done = a.pc;
  a.stop();
  a.at(irq2).store_imm(5U, cell_2).rte();
  a.at(irq4).store_imm(5U, cell_4).rte();
  a.at(irq6).rte();
  a.at(irq7).store_imm(5U, cell_7).rte();
  a.at(spurious).store_imm(5U, cell_s).rte();
  return p;
}

// The installed, undelivered interrupt vectors (the reachability roots' unconfigured set): spurious, levels 2, 4 and 7.
const std::vector<std::pair<std::uint32_t, std::uint32_t>> installed{{24U, spurious}, {26U, irq2}, {28U, irq4}, {31U, irq7}};

M68kFiniteAnalysisResult run(const Program &program, GenesisInterruptPremise premise) {
  const RegionImage view{program.a};
  M68kAnalysisConfig config{};
  config.domains.address = true;
  config.domains.memory = true;
  config.domains.contexts = true;
  config.domains.frames = true;
  config.frames.vectors = {{30U, irq6}};  // the delivered level 6
  config.frames.potential_interrupts = genesis_potential_interrupt_sources(installed, premise);
  config.frames.main_entries = {entry};
  config.frames.reset_entry = entry;
  config.frames.reset_ssp = ssp;
  return analyze_m68k_finite_values(view, {entry, irq6}, config);
}

FiniteValue word_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg) {
  FiniteValue out;
  for (const auto &[point, state] : m68k_points_of(result, pc))
    if (m68k_point_tag(point) == 0U) out = join(out, state->values.values[m68k_analysis_slot(reg, 16U)]);
  return out;
}

void premise_selection() {
  const auto board = GenesisInterruptPremise::genesis_board;
  const auto open = GenesisInterruptPremise::unconfigured;
  expect(genesis_interrupt_source(26U, board) && genesis_interrupt_source(28U, board) && genesis_interrupt_source(30U, board),
         "board: levels 2, 4 and 6 are sources");
  for (const auto vector : {15U, 24U, 25U, 27U, 29U, 31U, 64U})
    expect(!genesis_interrupt_source(vector, board), "board: vector " + std::to_string(vector) + " is never asserted");
  for (const auto vector : {15U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U})
    expect(genesis_interrupt_source(vector, open), "unconfigured: vector " + std::to_string(vector) + " is a source");
  expect(!genesis_interrupt_source(4U, open) && !genesis_interrupt_source(32U, open), "synchronous vectors are never interrupt sources");
  const auto selected = genesis_potential_interrupt_sources(installed, board);
  expect(selected == std::vector<M68kHandlerVector>{{26U, irq2}, {28U, irq4}}, "board: the installed sources are levels 2 and 4");
  expect(genesis_potential_interrupt_sources(installed, open).size() == installed.size(), "unconfigured: every installed vector");
}

void premise_analysis() {
  const auto program = build();
  const auto board = run(program, GenesisInterruptPremise::genesis_board);
  const auto open = run(program, GenesisInterruptPremise::unconfigured);
  expect(board.complete && board.frames.validated && open.complete && open.frames.validated, "both premises validate");
  expect(board.frames.potential_interrupt_vectors == 2U && open.frames.potential_interrupt_vectors == 4U,
         "the premise selects the potential sources");
  expect(word_at(board, program.read_done, 1U).is_unknown() && word_at(board, program.read_done, 2U).is_unknown(),
         "board: the level-2 and level-4 handlers' stores are asynchronous writers");
  expect(word_at(board, program.read_done, 3U) == FiniteValue::of({1U}) && word_at(board, program.read_done, 4U) == FiniteValue::of({1U}),
         "board: the level-7 and spurious handlers are not sources (their cells stay precise)");
  expect(!board.frames.main_async_all && board.frames.unanalysed.empty() && board.frames.writer_only_instances >= 2U,
         "board: the level-2 and level-4 handlers are analysed writer-only instances with bounded writers");
  // Level 7 and the spurious vector (unknown level) are never masked, so each can preempt its own handler (unbounded nesting): its
  // resuming child is unanalysed, the main flow's status becomes Unknown, every frame address with it, and every cell is asynchronous.
  expect(open.frames.main_async_all && !open.frames.unanalysed.empty(),
         "unconfigured: the non-maskable level-7/spurious handlers nest without bound (every cell asynchronous)");
  expect(word_at(open, program.read_done, 3U).is_unknown() && word_at(open, program.read_done, 4U).is_unknown() &&
             word_at(open, program.read_done, 1U).is_unknown() && word_at(open, program.read_done, 2U).is_unknown(),
         "unconfigured: every installed handler's store is an asynchronous writer");
  for (const auto handler : {irq2, irq4, irq7, spurious})
    expect(!board.reached.contains(handler) && !open.reached.contains(handler), "undelivered handlers are never credited to D");
}

}  // namespace

int main() {
  premise_selection();
  premise_analysis();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_genesis_interrupt_premise_test: all checks passed\n";
  return EXIT_SUCCESS;
}
