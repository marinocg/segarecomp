#pragma once

// SEG-029-T003 (ADR 0078, report-only): the MC68000 first-consumer adapter of the generic abstract-analysis core.
//
// A FORWARD transfer adapter for `analysis::solve`. A program point is a 24-bit bus PC. The abstract state is the
// eight data registers, each tracked independently modulo 2^16 and modulo 2^32 as generic `FiniteValue`s, plus the
// CPU-owned `width_derived` annotation of each value and a flag-provenance fact: the PC of the physically preceding
// instruction when that instruction is the point's SOLE predecessor through a sequential edge (joined to "none" by
// any other incoming edge). Every per-instruction value transfer, guard filter and PC-indexed address computation is
// delegated to the existing CPU semantic owners (`m68k_finite_register_after`, `m68k_finite_branch_filter`,
// `m68k_pc_index_address`, `m68k_control_successors`); this adapter adds no decoding or execution semantics.
//
// Edges: fixed successors carry the state (a conditional edge is filtered exactly when its branch's sole predecessor
// is the physically preceding flag setter); a direct call target carries the state; a stacked continuation (when
// enabled by the configuration) is an edge carrying the all-Unknown state, i.e. an opaque entry. A `JMP/JSR
// (d8,PC,Dn)` site whose index is a precise set produces computed edges to the exact even targets, failing closed
// (unresolved, no partial set) on an entry read outside the immutable image, a target outside the image, an empty
// domain, a width-only domain under the strict policy, or an Unknown index. Every other dynamic control form is
// reported unresolved and never followed. Returns are modelled only through their continuations.
//
// Report-only: no production target links this library.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "segarecomp/analysis/finite_value.hpp"
#include "segarecomp/analysis/solver.hpp"
#include "segarecomp/cpu/m68k/finite_register_values.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

// Read-only view of the executable image the adapter analyses. The machine owns the mapping (claims, aliases);
// the adapter only asks for one decoded instruction, executability and immutable bytes.
class M68kAnalysisImage {
public:
  struct Instruction {
    M68kIrOperation operation;
    std::uint32_t length{};
  };
  virtual ~M68kAnalysisImage() = default;
  // The one instruction whose execution address is the 24-bit bus `pc`; nullopt when odd, unmapped or rejected.
  [[nodiscard]] virtual std::optional<Instruction> decode(std::uint32_t pc) const = 0;
  // True when `pc` is an even PC with executable image bytes (decodability aside).
  [[nodiscard]] virtual bool mapped(std::uint32_t pc) const = 0;
  // Big-endian read of `bytes` (1, 2 or 4) at the bus `address` from provably immutable image bytes, else nullopt.
  [[nodiscard]] virtual std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) const = 0;
};

// A flat immutable image mapped at bus address `base` (caller-owned bytes; the span must outlive the view).
class M68kFlatAnalysisImage final : public M68kAnalysisImage {
public:
  M68kFlatAnalysisImage(std::span<const std::uint8_t> bytes, std::uint32_t base) : bytes_(bytes), base_(base) {}
  [[nodiscard]] std::optional<Instruction> decode(std::uint32_t pc) const override;
  [[nodiscard]] bool mapped(std::uint32_t pc) const override;
  [[nodiscard]] std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) const override;

private:
  [[nodiscard]] bool contains(std::uint32_t address) const noexcept;
  std::span<const std::uint8_t> bytes_;
  std::uint32_t base_{};
};

// Value slot of data register `reg` (0..7) at `width` (16 or 32).
inline constexpr std::size_t m68k_analysis_slot_count = 16U;
[[nodiscard]] constexpr std::size_t m68k_analysis_slot(unsigned reg, unsigned width) noexcept {
  return static_cast<std::size_t>(reg) * 2U + (width == 32U ? 1U : 0U);
}

struct M68kAnalysisState {
  analysis::ValueVector<m68k_analysis_slot_count> values;
  // CPU-owned annotation (M68kFiniteValues::width_derived); always false for an Unknown slot.
  std::array<bool, m68k_analysis_slot_count> width_derived{};
  // PC of the physically preceding instruction when it is the sole predecessor via a sequential edge; nullopt:
  // any other (or an additional) predecessor, an entry, or a non-sequential edge.
  std::optional<std::uint32_t> flag_setter;

  [[nodiscard]] static M68kAnalysisState unreachable() { return {}; }
  [[nodiscard]] static M68kAnalysisState all_unknown(analysis::UnknownReason reason = analysis::UnknownReason::unknown_input);

  friend M68kAnalysisState join(const M68kAnalysisState &left, const M68kAnalysisState &right);
  friend bool leq(const M68kAnalysisState &left, const M68kAnalysisState &right);
  friend bool operator==(const M68kAnalysisState &, const M68kAnalysisState &) = default;
};

enum class M68kPcIndexOutcome : std::uint8_t {
  resolved,
  index_unknown,
  address_register_index,
  entry_outside_immutable_image,
  target_outside_image,
  empty_domain,
  width_only_domain,
  invalidated,  // emitted targets were later lost by the growing fixed point: pinned unresolved (restart)
};
[[nodiscard]] const char *m68k_pc_index_outcome_name(M68kPcIndexOutcome outcome) noexcept;

struct M68kPcIndexSiteReport {
  M68kPcIndexOutcome outcome{M68kPcIndexOutcome::index_unknown};
  bool call{};
  std::vector<std::uint32_t> targets;  // sorted distinct; non-empty only when resolved
  std::uint32_t odd_targets_excluded{};
  analysis::UnknownReason reason{analysis::UnknownReason::unknown_input};  // when not resolved
};

struct M68kAnalysisConfig {
  bool accept_width_domains{};      // measurement variant: admit width-only index domains
  bool call_continuations{true};    // a call's stacked continuation is an opaque entry
  bool exception_continuations{};   // TRAP/TRAPV stacked continuation (strict model: off)
  bool pushed_code_continuations{}; // PEA of a code address (off)
  std::set<std::uint32_t> pinned_sites;  // PC-indexed sites forced unresolved (invalidated)
};

class M68kFiniteAdapter {
public:
  using State = M68kAnalysisState;

  M68kFiniteAdapter(const M68kAnalysisImage &image, M68kAnalysisConfig config) : image_(image), config_(std::move(config)) {}

  [[nodiscard]] analysis::TransferResult<State> transfer(std::uint64_t point, const State &in);

  // Pure classification of a `JMP/JSR (d8,PC,Xn)` site against its input state.
  [[nodiscard]] M68kPcIndexSiteReport evaluate_pc_index_site(std::uint32_t pc, const M68kIrOperation &operation,
                                                             const State &in) const;

  [[nodiscard]] std::optional<M68kAnalysisImage::Instruction> decode(std::uint32_t pc) const;

private:
  const M68kAnalysisImage &image_;
  M68kAnalysisConfig config_;
  mutable std::map<std::uint32_t, std::optional<M68kAnalysisImage::Instruction>> decoded_;
};
static_assert(analysis::Adapter<M68kFiniteAdapter>);

struct M68kFiniteAnalysisResult {
  bool complete{};
  analysis::UnknownReason reason{analysis::UnknownReason::iteration_bound};  // when incomplete
  std::uint32_t restarts{};  // driver restarts only; the generic solver's own restarts are in `solution.restarts`
  std::size_t iterations{};  // of the final solve
  std::map<std::uint32_t, std::uint32_t> reached;  // decoded reached instruction start -> length
  std::set<std::uint32_t> undecodable;             // reached points with no decodable instruction
  std::map<std::uint32_t, M68kPcIndexSiteReport> pc_index_sites;
  std::map<std::uint32_t, analysis::UnknownReason> unresolved_computed;  // every unresolved computed site
  analysis::Solution<M68kAnalysisState> solution;
};

// Runs the forward fixed point from `entries` (each seeded with the all-Unknown state). A site the generic solver pinned (it lost
// a computed target it had emitted) is reported `invalidated`: the analysis restarts with it pinned at the adapter (monotone,
// terminates).
[[nodiscard]] M68kFiniteAnalysisResult analyze_m68k_finite_values(const M68kAnalysisImage &image,
                                                                  const std::vector<std::uint32_t> &entries,
                                                                  M68kAnalysisConfig config = {},
                                                                  const analysis::Bounds &bounds = {});

// Data register `reg` modulo 2^width immediately before the instruction at `pc` (typed query).
[[nodiscard]] analysis::FiniteValue m68k_query_data_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc,
                                                             unsigned reg, unsigned width);

// Deterministic serialization of the whole result (report/test use).
[[nodiscard]] std::string format_m68k_finite_analysis(const M68kFiniteAnalysisResult &result);

}  // namespace segarecomp
