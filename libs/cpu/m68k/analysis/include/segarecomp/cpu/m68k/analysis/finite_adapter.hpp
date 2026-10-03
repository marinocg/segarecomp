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
// SEG-030-T003 (ADR 0079): when `M68kAnalysisConfig::domains.address` is set, the state also tracks A0-A7 as CPU-owned
// points-to values (address_value.hpp) through LEA, MOVEA, ADDA/SUBA, ADDQ/SUBQ, EXG, (An)+/-(An) and MOVEM auto-updates,
// MOVEA from exact immutable table entries and CMPA #imm + Bcc guards; every other address-register writer is Unknown.
// `JMP/JSR (An)`, `d16(An)` and `(d8,An,Xn)` then produce computed edges from an exact code-address set (each target an even,
// mapped image PC; any target outside the image fails the whole site; a strided set is never enumerated). With the flag clear
// every address register stays Unknown and the baseline behaviour is unchanged.
//
// SEG-030-T004 (ADR 0079): when `domains.memory` is set (it implies `address`), the state also carries a CPU-owned abstract memory
// (abstract_memory.hpp). Every instruction's memory writes follow the writer description (an undescribed writer poisons every cell);
// a MOVE/ADD/SUB/AND/OR into Dn or a MOVEA whose memory source names precise cells reads their value; JSR/BSR/PEA push into the
// cells below A7. Initial memory and opaque entries are Unknown. The driver rounds (decision 9) grow a monotone policy: the
// asynchronous-writer ranges of the code reachable from the handler roots (every cell when interrupts can be taken, see
// `M68kMemoryConfig::interrupts`, or when handler code stores through an Unknown address) and the external writer (when any store in
// D may alias a release range or has an Unknown address); the final round's own policy must be below the one it ran with.
//
// SEG-030-T005 (ADR 0079 decisions 6 and 9): when `domains.contexts` is set (it implies `memory` and `address`), a program point is
// `(context << 24) | pc` with a call string of depth k = 1: context 0 (roots, and callees merged past the context bound) or the call
// site's PC + 1. A call edge enters the callee in the call site's context (or in context 0 when the callee is merged) and resets the
// CPU-owned stack delta (A7 relative to the activation's entry; `stack_delta`). The call's continuation receives the proven summary
// of the call-site context from the driver configuration (the join of the callee's exit states at every RTS, with the return address
// popped; register AND memory effects, weak and poisoning stores included), or a typed opaque entry: `context_bound` (generic
// `state_bound`) for a merged callee or a recursive activation, `stack_unbalanced` for an RTS/RTE/RTR not at the entry stack delta or
// a nested unbalanced activation, and the T004 opaque entry for an unresolved callee or an unknown effect. The driver rounds derive
// the summaries from each solve and validate them against the configuration the solve used; only a validated round is returned.
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
#include "segarecomp/cpu/m68k/analysis/abstract_memory.hpp"
#include "segarecomp/cpu/m68k/analysis/address_value.hpp"
#include "segarecomp/cpu/m68k/control_successors.hpp"
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
  // Big-endian read of `bytes` (1, 2 or 4; any other width is nullopt) at the bus `address` from provably immutable image bytes,
  // else nullopt.
  [[nodiscard]] virtual std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) const = 0;
  // The machine region extent containing the 24-bit bus `address` (ADR 0079 decision 4), else nullopt (no region: a pointer
  // there is Unknown). The default view knows no region.
  [[nodiscard]] virtual std::optional<M68kRegionExtent> region_of(std::uint32_t address) const {
    (void)address;
    return std::nullopt;
  }
};

// A flat immutable image mapped at bus address `base` (caller-owned bytes; the span must outlive the view).
class M68kFlatAnalysisImage final : public M68kAnalysisImage {
public:
  M68kFlatAnalysisImage(std::span<const std::uint8_t> bytes, std::uint32_t base) : bytes_(bytes), base_(base) {}
  [[nodiscard]] std::optional<Instruction> decode(std::uint32_t pc) const override;
  [[nodiscard]] bool mapped(std::uint32_t pc) const override;
  [[nodiscard]] std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) const override;
  // The whole flat image is one immutable image region (id 0).
  [[nodiscard]] std::optional<M68kRegionExtent> region_of(std::uint32_t address) const override;

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
  // A0-A7 (SEG-030-T003). Unknown in every reachable state unless the address domain is enabled.
  std::array<M68kPointsTo, 8> address{};
  // SEG-030-T004: abstract memory (no cell, annotation `none`, unless the memory domain is enabled).
  M68kAbstractMemory memory;
  // SEG-030-T005: A7 minus A7 at the entry of the current activation (modulo 2^32), contexts domain only (bottom otherwise).
  analysis::FiniteValue stack_delta;

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

// ADR 0079 decision 5: the staged CPU-owned domains (T003: address; the others are delivered by later children).
struct M68kAnalysisDomains {
  bool address{};
  bool memory{};
  bool contexts{};
  bool frames{};
};

// A `JMP/JSR (An)`, `d16(An)` or `(d8,An,Xn)` site (SEG-030-T003, address domain only).
struct M68kAddressSiteReport {
  M68kDynamicControlFamily family{M68kDynamicControlFamily::jump_address_indirect};
  bool resolved{};
  std::vector<std::uint32_t> targets;  // sorted distinct even mapped PCs; non-empty only when resolved
  std::uint32_t odd_targets_excluded{};
  analysis::UnknownReason reason{analysis::UnknownReason::unsupported_transfer};  // when not resolved
  M68kAnalysisSubReason sub{M68kAnalysisSubReason::none};                         // when not resolved
};

// SEG-030-T004 (ADR 0079 decision 7): the asynchronous/external writer model of the memory domain.
struct M68kMemoryConfig {
  // Interrupts may be taken at any instruction boundary (the SR interrupt mask is not tracked). An interrupt can then also preempt
  // handler code, whose entry A7 is therefore Unknown (the join over every interruptible point, handler points included, does not
  // converge); the nested exception frame is a handler store through an Unknown address, so every cell is asynchronous.
  bool interrupts{};
  std::vector<std::uint32_t> handler_roots;  // machine-delivered vector handlers (potential asynchronous writers)
  // Bus ranges [first, last) a store to which may release another bus master that writes work RAM (Genesis: the Z80 BUSREQ/RESET
  // control block). A store with an Unknown address also counts.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> release_ranges;
  // DIAGNOSTIC premise ablation only (never credited): never derive the external writer.
  bool assume_no_external_writer{};
  M68kMemoryPolicy policy;  // the starting configuration (normally empty)
};

// ADR 0079 decision 11: R, the driver-round bound.
inline constexpr std::uint32_t m68k_memory_round_bound = 16U;

// SEG-030-T005 (ADR 0079 decisions 6 and 11): call-string depth k and K, the contexts per callee entry.
inline constexpr std::uint32_t m68k_context_depth = 1U;
inline constexpr std::size_t m68k_context_bound = 8U;
// A program point of the contexts domain: `(context << 24) | pc`. Context 0 is the context-free point (every point when the contexts
// domain is off); a call site's context is its PC + 1.
[[nodiscard]] constexpr std::uint64_t m68k_analysis_point(std::uint32_t context, std::uint32_t pc) noexcept {
  return (static_cast<std::uint64_t>(context) << 24U) | (pc & UINT32_C(0x00FFFFFF));
}
[[nodiscard]] constexpr std::uint32_t m68k_point_pc(std::uint64_t point) noexcept {
  return static_cast<std::uint32_t>(point) & UINT32_C(0x00FFFFFF);
}
[[nodiscard]] constexpr std::uint32_t m68k_point_context(std::uint64_t point) noexcept {
  return static_cast<std::uint32_t>(point >> 24U);
}
[[nodiscard]] constexpr std::uint32_t m68k_call_context(std::uint32_t call_site) noexcept {
  return (call_site & UINT32_C(0x00FFFFFF)) + 1U;
}

// SEG-030-T005: the contexts-domain part of the driver configuration (ADR 0079 decision 9).
struct M68kContextConfig {
  // Callee entries with more than K call-site contexts: analysed in context 0, continuation Unknown(context_bound). Monotone.
  std::set<std::uint32_t> merged;
  // Call-site context -> the proven continuation state (the callee summary). A candidate: a round is returned only when every
  // summary it used is above the summary its own solution derives (final validation).
  std::map<std::uint32_t, M68kAnalysisState> summaries;
  // Call-site context without a proven summary -> the CPU sub-reason of its opaque continuation (none, stack_unbalanced,
  // context_bound).
  std::map<std::uint32_t, M68kAnalysisSubReason> opaque;
  std::uint32_t round_bound{m68k_memory_round_bound};  // R (tests may lower it, never raise it)
  friend bool operator==(const M68kContextConfig &, const M68kContextConfig &) = default;
};

// SEG-030-T005: contexts-domain outcome of a run.
struct M68kContextReport {
  bool enabled{};
  bool converged{};   // the returned round reproduced its own configuration exactly
  bool validated{};   // the returned round's summaries, merges and memory policy passed the final validation
  analysis::UnknownReason reason{analysis::UnknownReason::iteration_bound};  // when not validated (the T004 result is returned)
  std::uint32_t rounds{};           // contexts rounds run
  std::uint32_t returned_round{};   // the (validated) round whose solution is returned
  std::size_t total_iterations{};   // over every contexts round
  std::size_t contexts{};           // call-site contexts reached
  std::size_t max_contexts_per_callee{};
  std::size_t merged_callees{};
  std::size_t activations{};        // call-site contexts plus merged-callee activations
  std::size_t balanced_activations{};
  std::size_t recursive_activations{};
  std::size_t summaries{};          // call-site contexts with a proven summary
  std::map<M68kAnalysisSubReason, std::size_t> unproven;  // call-site contexts without a summary, by sub-reason
  std::size_t summary_continuations{};  // call points whose continuation applied a summary
  std::map<M68kAnalysisSubReason, std::size_t> opaque_continuations;  // the other call points, by sub-reason
  // Comparator (decision 8): the same program without contexts (the T004 memory result); report only.
  std::size_t discovered_without_contexts{};
  std::size_t sites_resolved_only_with_contexts{};
  std::size_t sites_resolved_only_without_contexts{};
  std::size_t unresolved_sites{};
};

// SEG-030-T004: memory-domain outcome of a run.
struct M68kMemoryReport {
  bool enabled{};
  bool converged{};                     // false: the rounds did not converge and the memory domain was switched off
  std::uint32_t rounds{};
  M68kMemoryPolicy policy;              // the policy of the final round
  bool assumed_no_external_writer{};    // the diagnostic premise ablation was applied
  bool release_store{};                 // a store in D may release an external writer
  std::size_t release_stores{};         // stores in D that alias a release range or have an Unknown address
  std::size_t unknown_target_stores{};  // stores in D with an Unknown address (undescribed writers included)
  std::size_t undescribed_writers{};
  std::size_t handler_points{};         // points reachable from the handler roots
  std::size_t handler_store_sites{};
  std::size_t max_cells{};
  std::size_t precise_reads{};          // memory-source operands of work RAM (or an Unknown address) read precisely
  std::map<std::pair<analysis::UnknownReason, M68kAnalysisSubReason>, std::size_t> unknown_reads;
};

struct M68kAnalysisConfig {
  bool accept_width_domains{};      // measurement variant: admit width-only index domains
  bool call_continuations{true};    // a call's stacked continuation is an opaque entry
  bool exception_continuations{};   // TRAP/TRAPV stacked continuation (strict model: off)
  bool pushed_code_continuations{}; // PEA of a code address (off)
  std::set<std::uint32_t> pinned_sites;  // computed sites forced unresolved (invalidated)
  M68kAnalysisDomains domains{};         // staged domains (all off: the SEG-029 baseline)
  M68kMemoryConfig memory{};             // SEG-030-T004 (memory domain only)
  M68kContextConfig contexts{};          // SEG-030-T005 (contexts domain only)
};

class M68kFiniteAdapter {
public:
  using State = M68kAnalysisState;

  M68kFiniteAdapter(const M68kAnalysisImage &image, M68kAnalysisConfig config) : image_(image), config_(std::move(config)) {}

  [[nodiscard]] analysis::TransferResult<State> transfer(std::uint64_t point, const State &in);

  // Pure classification of a `JMP/JSR (d8,PC,Xn)` site against its input state.
  [[nodiscard]] M68kPcIndexSiteReport evaluate_pc_index_site(std::uint32_t pc, const M68kIrOperation &operation,
                                                             const State &in) const;
  // Pure classification of an address-register-relative `JMP/JSR` site against its input state (address domain).
  [[nodiscard]] M68kAddressSiteReport evaluate_address_site(std::uint32_t pc, const M68kIrOperation &operation,
                                                            M68kDynamicControlFamily family, const State &in) const;

  [[nodiscard]] std::optional<M68kAnalysisImage::Instruction> decode(std::uint32_t pc) const;

  // SEG-030-T004: the (address set, span) of every memory write of `operation` at `pc` from its input state; an Unknown address set
  // for an undescribed writer or an untracked address.
  [[nodiscard]] std::vector<std::pair<M68kPointsTo, std::uint32_t>> memory_write_targets(const M68kIrOperation &operation,
                                                                                         const State &in) const;
  // SEG-030-T004: the value of a memory source operand of `bytes` bytes (immutable image bytes or abstract-memory cells), with the
  // generic reason and CPU sub-reason when Unknown. `tracked` reports whether a work-RAM cell (or an Unknown address) was involved.
  [[nodiscard]] M68kMemoryRead read_memory_operand(const State &in, const M68kEffectiveAddress &ea, std::uint32_t bytes,
                                                   bool *tracked = nullptr) const;
  // The state of an entry: a root (initial memory) or an opaque continuation (callee stores).
  [[nodiscard]] State entry_state(bool continuation) const;
  // SEG-030-T005: an opaque continuation typed by `sub` (none: the T004 opaque entry).
  [[nodiscard]] State opaque_continuation(M68kAnalysisSubReason sub) const;
  // SEG-030-T005: the stack delta after `operation` (normal successors), from its input state.
  [[nodiscard]] analysis::FiniteValue stack_delta_after(const M68kIrOperation &operation, const State &in) const;
  // SEG-030-T005: the continuation state of the call at `pc` whose callees in this transfer are `callees` (empty: unresolved).
  [[nodiscard]] State continuation(std::uint32_t pc, const std::vector<std::uint32_t> &callees, const State &in) const;

private:
  void transfer_address_registers(const M68kIrOperation &operation, const State &in, State &out) const;
  void transfer_memory(const M68kIrOperation &operation, std::uint32_t next, const State &in, State &out) const;
  [[nodiscard]] M68kPointsTo operand_address(const State &in, const M68kEffectiveAddress &ea, std::uint32_t predecrement) const;
  [[nodiscard]] std::optional<M68kCellValue> operand_value(const State &in, const M68kEffectiveAddress &ea, std::uint32_t bytes) const;
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
  bool address_domain{};                                     // the address domain was enabled
  std::map<std::uint32_t, M68kAddressSiteReport> address_sites;  // address-register-relative sites (address domain only)
  std::map<std::uint32_t, analysis::UnknownReason> unresolved_computed;  // every unresolved computed site
  M68kMemoryReport memory;  // SEG-030-T004 (memory domain only)
  M68kContextReport contexts;  // SEG-030-T005 (contexts domain only)
  analysis::Solution<M68kAnalysisState> solution;
};

// Runs the forward fixed point from `entries` (each seeded with the all-Unknown state). A site the generic solver pinned (it lost
// a computed target it had emitted) is reported `invalidated`: the analysis restarts with it pinned at the adapter (monotone,
// terminates).
[[nodiscard]] M68kFiniteAnalysisResult analyze_m68k_finite_values(const M68kAnalysisImage &image,
                                                                  const std::vector<std::uint32_t> &entries,
                                                                  M68kAnalysisConfig config = {},
                                                                  const analysis::Bounds &bounds = {});

// Every reached point of `pc` (any context) with its input state, in point order (contexts-domain queries).
[[nodiscard]] std::vector<std::pair<std::uint64_t, const M68kAnalysisState *>> m68k_points_of(const M68kFiniteAnalysisResult &result,
                                                                                             std::uint32_t pc);

// Data register `reg` modulo 2^width immediately before the instruction at `pc` (typed query).
[[nodiscard]] analysis::FiniteValue m68k_query_data_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc,
                                                             unsigned reg, unsigned width);

// Address register `reg` (0..7) immediately before the instruction at `pc`: Unknown(reason) when the solve did not complete,
// bottom when the point was not reached.
[[nodiscard]] M68kPointsTo m68k_query_address_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg);

// Deterministic serialization of the whole result (report/test use).
[[nodiscard]] std::string format_m68k_finite_analysis(const M68kFiniteAnalysisResult &result);

}  // namespace segarecomp
