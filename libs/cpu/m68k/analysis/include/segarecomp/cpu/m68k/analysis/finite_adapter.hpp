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
// SEG-030-T006 (ADR 0079 decisions 5, 7, 9 and 10): when `domains.frames` is set (it implies `contexts`, `memory` and `address`),
// the state also carries the CPU-owned status (S and I2-I0, frames.hpp) and the analysis runs one partition per handler INSTANCE: a
// program point is `(tag << 48) | (context << 24) | pc`, tag 0 being the main flow (the reset or bridge entry) and every other tag
// one handler taken from one parent partition (M68kHandlerInstance). Partitions never share an edge. A handler instance is entered
// with S = 1, the mask of the accepted interrupt (or the parent's for a synchronous exception) and A7 = the frame address (the join
// over the parent's eligible or raising points of A7 - 6, only when S = 1 is proven there; else Unknown). The asynchronous writers
// of a partition are the stores, interrupt frames and asynchronous writers of the handler instances that can preempt it AND whose
// resumption is analysed (interrupts; divide-by-zero, CHK and TRAPV): a per-partition policy instead of every cell. RTE/RTR and an
// RTS away from the entry stack delta are resolved only from precise frame or return cells written by analysed code (computed
// edges); every other one stays Unknown (`frame_unproven`, `interrupt_resumption` in a handler partition, ...). A resuming child that
// may rewrite its saved SR (frame integrity) makes its parent's status Unknown after the boundaries where it can be taken. An
// opaque continuation's status is its partition's status bound; a balanced callee's continuation (summary or merged) keeps the
// caller's A7.
// The handlers without an analysed instance are seeded in one more partition (for D) whose every cell is asynchronous. The driver
// first settles the contexts rounds with no frames configuration (warm start), then grows the frames configuration only from
// settled rounds and returns a validated round only (else the T005 contexts result, with the reason).
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
#include "segarecomp/cpu/m68k/analysis/frames.hpp"
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
  // SEG-030-T006: the status `(S << 3) | I` (frames.hpp), frames domain only (bottom otherwise; Unknown: SR not tracked).
  analysis::FiniteValue status;
  // SEG-030-T009 correction cycle (frames domain only; all zero otherwise): register-preservation facts of a handler instance
  // partition, used only to prove which registers a handler's exits restore (M68kResumption). origin[i] for register i (D0-D7, then
  // A0-A6): 0 = no fact; r + 1 = the register provably holds the value register r had at the entry of the partition's handler (its
  // root). `saved` maps a long stack slot, as its byte offset from the current activation's entry A7, to the same encoding: the slot
  // provably holds that entry value (written by a long MOVE/MOVEM of the register through A7, and no store since may have touched
  // it). A fact is used only when the slot is not asynchronous or externally written under the partition's policy. A call rebases the
  // slots into the callee's activation and its continuation rebases them back (both need a precise stack delta). Join keeps the
  // equal facts only.
  std::array<std::uint8_t, 15> origin{};
  std::map<std::int64_t, std::uint8_t> saved;
  // Location of this handler instance's saved hardware-frame SR while it is proven untouched by analysed code. The
  // asynchronous/external policy is checked separately when the fact is consumed. Absent for main/code-built frames and opaque
  // entries. This permits an exact replacement of the hardware frame's PC to use ordinary RTE target resolution without inventing
  // a restored SR; an Unknown restored SR still fails closed.
  std::optional<std::int64_t> hardware_sr_offset;  // byte offset from this activation's entry A7
  // SEG-030-T009 correction cycle (frames domain only): D0-D7 (bit n: Dn) made Unknown at a boundary by an unproven handler
  // resumption (M68kAnalysisSubReason::interrupt_resumption_unproven); cleared when the register is written. Report attribution only.
  std::uint8_t resumption_unknown{};

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
  // SEG-030-T009 correction cycle (frames domain): interrupt_resumption_unproven when the Unknown index comes from an unproven
  // handler resumption; none otherwise.
  M68kAnalysisSubReason sub{M68kAnalysisSubReason::none};
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
  // SEG-030-T010: a credited bound on the work-RAM writes of the released bus master, supplied by a platform proof. nullopt (the
  // default) is unbounded: a release store makes every work-RAM cell externally written. Otherwise a release store makes exactly
  // these tracked ranges asynchronous; an empty set means the released master never writes mutable RAM.
  std::optional<std::vector<M68kAsyncRange>> external_writer_bound;
  // SEG-030-T010: bus ranges [first, last) whose stores the run reports for a platform proof (Genesis: the Z80 area). Report only.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> observed_store_ranges;
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
// SEG-030-T006: a context carries the partition tag in its top byte (always 0 unless the frames domain is enabled).
[[nodiscard]] constexpr std::uint32_t m68k_context_tag(std::uint32_t context) noexcept { return context >> 24U; }
[[nodiscard]] constexpr std::uint32_t m68k_context_site(std::uint32_t context) noexcept { return context & UINT32_C(0x00FFFFFF); }
[[nodiscard]] constexpr std::uint32_t m68k_tagged_context(std::uint32_t tag, std::uint32_t site) noexcept {
  return (tag << 24U) | (site & UINT32_C(0x00FFFFFF));
}
[[nodiscard]] constexpr std::uint32_t m68k_point_tag(std::uint64_t point) noexcept {
  return m68k_context_tag(m68k_point_context(point));
}
// SEG-030-T006: the largest instance tag; above it the frames domain fails closed (`state_bound`).
inline constexpr std::uint32_t m68k_max_instance_tag = 0xFDU;
// The tag of the partition that seeds the roots of handlers without an analysed instance (kept for D; never a writer or a parent; every
// cell is asynchronous there).
inline constexpr std::uint32_t m68k_dead_handler_tag = 0xFEU;
// SEG-030-T006/T008: the partition of the handlers entered from a taking point whose state is not modelled (M68kFrameConfig::
// unknown_entries): a boundary of this partition itself, or a handler taken but not analysed as an instance (any unanalysed cause:
// Unknown entry A7, nesting, the depth bound, a widened entry). Its roots start Unknown, every cell is asynchronous there, and its own
// boundaries are taking points (their handlers join this partition, and a resuming one clobbers its status).
inline constexpr std::uint32_t m68k_unknown_entry_tag = 0xFFU;
// The instance depth bound: a handler taken from a chain of this many handler instances is not analysed (its parent's writers are
// every cell).
inline constexpr std::uint32_t m68k_instance_depth_bound = 3U;
// Growth bound of an instance's entry A7 across rounds: beyond it the entry A7 is Unknown(frame_unproven) (widening).
inline constexpr std::uint32_t m68k_instance_growth_bound = 4U;

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
  // SEG-030-T006 (frames domain only): `m68k_tagged_context(tag, callee)` of a merged callee proven balanced -> the join of its exit
  // statuses. Its continuations keep the caller's A7 and take this status (registers and memory stay opaque).
  std::map<std::uint32_t, analysis::FiniteValue> merged_exits;
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

inline constexpr std::size_t m68k_origin_registers = 15U;  // D0-D7, A0-A6 (A7 follows the frame: the RTE pops the frame taken there)

// SEG-030-T009 correction cycle (ADR 0079 decisions 7 and 8): what resuming one or more handler instances does to D0-D7/A0-A6 of the
// boundary it resumes at. A register every analysed exit of the handler provably restores to its entry value (origin facts) is bottom:
// the boundary keeps its own value (the no-interrupt path already carries it). Any other is the join of the handler's exit values at
// its proven RTEs of its own frame, which the boundary state joins. `unproven`: the handler's resumption is not proven (unanalysed,
// Unknown entry, unmodelled parent, depth bound, or an exit that is not a proven RTE of its frame): every D0-D7/A0-A6 at the
// boundary becomes Unknown(interrupt_resumption_unproven). No register-preservation premise is ever applied.
struct M68kResumption {
  std::array<analysis::FiniteValue, m68k_analysis_slot_count> data{};
  std::array<bool, m68k_analysis_slot_count> width_derived{};
  std::array<M68kPointsTo, 7> address{};
  bool unproven{};
  friend M68kResumption join(const M68kResumption &left, const M68kResumption &right);
  friend bool leq(const M68kResumption &left, const M68kResumption &right);
  friend bool operator==(const M68kResumption &, const M68kResumption &) = default;
};

// SEG-030-T009 correction cycle 2 (ADR 0079 decision 7): the clobber level of a child, i.e. where its handler's RTE through an
// unmodified frame resumes: 1-7 an autovector interrupt and 8 an interrupt of unknown level (at the interrupted boundary); 0 divide by
// zero, CHK or TRAPV and 9 TRAP #n (after the raising instruction); 10 illegal, line 1010/1111 or privilege violation (the raising
// instruction itself, which re-executes).
inline constexpr std::uint32_t m68k_level_resuming_synchronous = 0U;
inline constexpr std::uint32_t m68k_level_unknown_interrupt = 8U;
inline constexpr std::uint32_t m68k_level_trap = 9U;
inline constexpr std::uint32_t m68k_level_fault = 10U;
[[nodiscard]] constexpr bool m68k_interrupt_clobber_level(std::uint32_t level) noexcept { return level >= 1U && level <= 8U; }
// A resumption key (M68kFrameConfig::resumptions): the clobber level and the byte offset the handler's exits add to the stacked PC
// (0: an unmodified frame PC; an ADDQ/SUBQ/ADDI/SUBI.L #imm of the frame PC cell gives the immediate). The full-width fields avoid
// aliases: malformed/out-of-contract values cannot collide after truncation.
[[nodiscard]] constexpr std::uint64_t m68k_resumption_key(std::uint32_t level, std::int32_t offset) noexcept {
  return static_cast<std::uint64_t>(level) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(offset)) << 32U);
}
[[nodiscard]] constexpr std::uint32_t m68k_resumption_level(std::uint64_t key) noexcept { return static_cast<std::uint32_t>(key); }
[[nodiscard]] constexpr std::int32_t m68k_resumption_offset(std::uint64_t key) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(key >> 32U));
}

// SEG-030-T006: one machine-delivered vector and its handler (ADR 0021 / ADR 0043 delivered set).
struct M68kHandlerVector {
  std::uint32_t vector{};
  std::uint32_t handler{};  // 24-bit bus PC
  friend bool operator==(const M68kHandlerVector &, const M68kHandlerVector &) = default;
};

// SEG-030-T006: one handler instance (a partition): `handler` taken from the partition `parent` (0: the main flow). An instance is
// analysed only when its entry A7 is known, its parent is the main flow or an analysed instance (resuming or not: every boundary of
// an analysed partition is a taking point), its handler is not already on the parent's chain and the chain is below the depth bound;
// any other handler taken from a partition is unanalysed: it is entered with an Unknown entry (M68kFrameConfig::unknown_entries) and,
// when it can resume, makes that partition's asynchronous writers every cell and its status Unknown after the boundaries where it can
// be taken.
struct M68kHandlerInstance {
  std::uint32_t handler{};
  std::uint32_t parent{};
  analysis::FiniteValue status;      // entry status
  analysis::FiniteValue stacked_status;  // status saved in this instance's hardware frame
  M68kPointsTo a7;                   // entry A7 (the frame address)
  std::set<std::uint32_t> vectors;   // the vectors entering it from the parent
  bool resuming{};                   // some vector entering it resumes at an analysed point of its parent
  bool interrupt{};                  // some vector entering it is an interrupt
  // Some delivered vector (M68kFrameConfig::vectors) enters it from a credited partition (the main flow or a credited instance):
  // its points count towards the discovered set and the site and read reports. Otherwise it is entered only by potential
  // interrupts (M68kFrameConfig::potential_interrupts) or below such an instance, and is analysed for asynchronous writers,
  // eligibility and frame integrity only (writer-only; SEG-030-T006 iteration 3).
  bool credited{};
  std::uint32_t growth{};            // rounds in which the entry A7 grew (widening, m68k_instance_growth_bound)
  friend bool operator==(const M68kHandlerInstance &, const M68kHandlerInstance &) = default;
};

// SEG-030-T006: the frames-domain part of the configuration (ADR 0079 decisions 7 and 9).
struct M68kFrameConfig {
  std::vector<M68kHandlerVector> vectors;           // machine facts
  // Installed interrupt vectors the machine model does not deliver but the machine may assert (the caller's machine premise selects
  // them; an unconfigured machine passes every installed interrupt vector). Real hardware may deliver them once the program enables
  // their source, so wherever their level is eligible they enter a handler instance exactly like a delivered interrupt (analysed
  // when admitted, else an unanalysed resuming interrupt: every cell asynchronous in that partition and its status Unknown after
  // those boundaries). Such an instance is writer-only (M68kHandlerInstance::credited): its handler is never seeded as a root and its
  // points never enter the discovered set or the site reports.
  std::vector<M68kHandlerVector> potential_interrupts;
  std::set<std::uint32_t> main_entries;             // roots of the main flow (tag 0)
  std::optional<std::uint32_t> reset_entry;         // the reset root (S = 1, I = 7, A7 = reset_ssp)
  std::optional<std::uint32_t> reset_ssp;           // the long at vector 0
  std::map<std::uint32_t, M68kHandlerInstance> instances;  // tag (1..m68k_max_instance_tag) -> analysed instance
  std::uint32_t next_tag{1U};  // tags are never reused (a dropped instance's tag stays retired)
  std::map<std::uint32_t, M68kMemoryPolicy> policies;      // tag -> its asynchronous writers; grows monotonically
  // Partitions whose status after an interrupted boundary is Unknown because a resuming child instance may rewrite its saved SR,
  // with the clobber levels of those children (m68k_level_*: 1-7, 8 interrupts; 0, 9, 10 synchronous vectors, at or after their
  // raising instruction).
  std::map<std::uint32_t, std::set<std::uint32_t>> clobbered;
  // The status bound of every live partition (tag 0 and each analysed instance): the join of its entry statuses (its main roots, or
  // the instance's entry), every SR writer's result and every proven RTE's restored status in the partition; Unknown when a clobber
  // level of the partition is eligible under that join (or a resuming synchronous child may clobber it). It is the status of an
  // opaque continuation in that partition, relative to the closure premise (ADR 0079 decision 8): the code an opaque continuation
  // skips runs in the same partition, and a child instance either restores the partition's status (frame integrity) or clobbers it.
  // Each bound grows monotonically from bottom (an absent tag is bottom).
  std::map<std::uint32_t, analysis::FiniteValue> status_bounds;
  // Delivered handler PCs also entered from a taking point whose state is not modelled (an unanalysed taking from a credited partition,
  // a dropped instance, or a boundary of the unknown-entry partition itself): seeded in m68k_unknown_entry_tag with an Unknown entry,
  // so that the handler's entry state joins an Unknown entry and never only the analysed instances' entries. Grows monotonically.
  std::set<std::uint32_t> unknown_entries;
  // SEG-030-T009 correction cycle: per partition, per resumption key of its children (m68k_resumption_key: the clobber level and the
  // frame-PC offset of the exits), the join of their register resumptions, applied where such a child resumes: an interrupt at the
  // boundary eligible under its status, a synchronous vector after (levels 0, 9) or at (level 10) its raising instruction, each plus
  // the offset (correction cycle 2: every synchronous class may resume; an offset exit resumes at the stacked PC plus the offset). An
  // analysed child contributes its own exits (which already include its own children's resumptions: the join is transitive); an
  // unanalysed or unproven one is `unproven` (at offset 0). Grows monotonically.
  std::map<std::uint32_t, std::map<std::uint64_t, M68kResumption>> resumptions;
  friend bool operator==(const M68kFrameConfig &, const M68kFrameConfig &) = default;
};

// SEG-030-T006: an RTE, RTR, or RTS away from the entry stack delta, resolved only from precise analysed frame/return cells.
struct M68kReturnSiteReport {
  M68kDynamicControlFamily family{M68kDynamicControlFamily::return_from_exception};
  bool resolved{};
  std::vector<std::uint32_t> targets;  // sorted distinct even mapped PCs; non-empty only when resolved
  std::uint32_t odd_targets_excluded{};
  analysis::UnknownReason reason{analysis::UnknownReason::unsupported_transfer};
  M68kAnalysisSubReason sub{M68kAnalysisSubReason::none};
  analysis::FiniteValue restored_status;  // RTE only: the status restored from the proven frame
  // Exact hardware-frame PC offsets proven for this handler RTE. The site remains a typed Unknown because its target is relative to
  // the parent taking point; consumers use this set to distinguish a modelled resumption from an unproven RTE.
  std::vector<std::int32_t> resumption_offsets;
  bool resumption_unproven{};  // at least one merged handler context cannot prove its hardware-frame PC relation
};

// SEG-030-T008 (ADR 0079 decision 8): the classification of an RTS at its activation's entry stack delta (or of any RTS when the stack
// delta is not tracked), from the abstract return cell (A7).L and the recorded return slot (abstract_memory.hpp), memory domain only.
//   normal:   the cell holds only return addresses a call pushed into it, or the slot is recorded and no store may have written it;
//   premise:  the cell is Unknown only through what the return-slot integrity premise assumes away (see M68kReturnSlotPremise);
//   computed: the cell holds a precise set with some value no call pushed: a computed return resolved to that set;
//   unknown:  a known-target store may have rewritten the slot and the cell is not precise (return_slot_rewritten), or no call
//             pushed the slot and the cell is Unknown (initial memory, the cell bound), or the site was pinned (invalidated).
// A `computed` or `unknown` RTS is never an exit of a proven activation.
enum class M68kReturnSlotClass : std::uint8_t { normal, premise, computed, unknown };
// What the return-slot integrity premise assumes away at a `premise` RTS.
enum class M68kReturnSlotPremise : std::uint8_t {
  none,
  slot_untracked,        // A7 is Unknown at the RTS: the slot cannot be located (every store relation is unknown)
  unknown_target_store,  // a store with an Unknown target (or an undescribed writer) may have written the slot
  opaque_callee,         // the slot's memory went through an opaque call continuation (an unmodelled callee effect)
  async_writer,          // the cell has an asynchronous writer (an interrupt handler)
  external_writer,       // the cell may be written by another bus master (the Z80)
};
[[nodiscard]] const char *m68k_return_slot_premise_name(M68kReturnSlotPremise premise) noexcept;

struct M68kReturnSlotOutcome {
  M68kReturnSlotClass kind{M68kReturnSlotClass::normal};
  M68kReturnSlotPremise premise{M68kReturnSlotPremise::none};
  std::vector<std::uint32_t> fresh;  // computed: the targets no call continuation of this slot already covers (the computed edges)
  std::uint32_t odd_targets_excluded{};
  std::vector<std::uint32_t> targets;  // computed: every even mapped target (the site's target set)
  analysis::UnknownReason reason{analysis::UnknownReason::unsupported_transfer};  // unknown only
  M68kAnalysisSubReason sub{M68kAnalysisSubReason::none};                         // unknown only
};

// SEG-030-T008: the RTS sites of a run classified from their return slot (per PC, credited points only).
struct M68kReturnSlotReport {
  bool enabled{};
  std::size_t sites{};           // RTS PCs with a classified point
  std::size_t normal_sites{};    // every point normal
  std::size_t premise_sites{};   // some point applied the return-slot integrity premise (return_slot_premise_sites)
  std::map<M68kReturnSlotPremise, std::size_t> premise_by_cause;  // premise sites by the cause of their first premise point
  std::size_t computed_sites{};  // some point computed, none unknown
  std::size_t unknown_sites{};   // some point unknown
  std::map<std::uint32_t, M68kReturnSlotPremise> premise_pcs;  // the premise sites and their causes (falsification attribution)
};

// SEG-030-T006: frames-domain outcome of a run (counts only; the configuration of the returned round).
struct M68kFrameReport {
  bool enabled{};
  bool validated{};
  analysis::UnknownReason reason{analysis::UnknownReason::iteration_bound};  // when not validated (frames switched off)
  std::string failure;               // a generic failure class when not validated
  std::uint32_t warm_rounds{};       // contexts rounds before the frames configuration grows
  std::uint32_t frame_rounds{};      // rounds of the frames configuration
  bool reset_state{};                // the main flow starts from the 68000 reset state
  std::size_t handler_vectors{};
  std::size_t instances{};           // analysed instances
  std::size_t interrupt_instances{};
  std::size_t resuming_instances{};  // resuming synchronous (not interrupt)
  std::size_t synchronous_instances{};
  std::size_t writer_only_instances{};  // analysed instances that are not credited (entered only by potential interrupts)
  std::size_t writer_only_points{};     // points of writer-only instances (analysed for writers only; not in `points`)
  std::size_t dead_handlers{};       // handler PCs with no analysed instance
  std::size_t unknown_entry_handlers{};  // handler PCs also entered with an Unknown entry (M68kFrameConfig::unknown_entries)
  std::map<std::string, std::size_t> unanalysed;  // handlers taken from a live partition but not analysed, by cause
  std::size_t frame_integrity_failures{};
  std::size_t clobbered_partitions{};
  // Points of live credited partitions (the main flow and the credited instances).
  std::size_t points{};
  std::size_t status_unknown{};
  std::size_t supervisor_proven{};
  std::size_t potential_interrupt_vectors{};
  std::size_t interrupt_eligible{};  // some delivered or potential interrupt can be taken
  std::size_t interrupt_masked{};    // status known and no delivered or potential interrupt can be taken
  std::size_t potential_eligible{};  // some potential (installed, undelivered) interrupt can be taken
  std::size_t raising_points{};      // some delivered synchronous vector may be raised
  std::size_t frame_unknown_a7{};          // eligible/raising points whose frame address is Unknown: S = 1 proven, A7 Unknown
  std::size_t frame_unproven_supervisor{}; // ... S = 1 not proven
  std::map<std::string, std::size_t> frame_a7_unknown_by_reason;  // the first: by A7's generic reason / CPU sub-reason
  // The same boundary counts in the first frames round (before a clobbered status propagates): where the frame address is lost first.
  std::size_t first_round_points{};
  std::size_t first_round_status_unknown{};
  std::size_t first_round_interrupt_eligible{};
  std::size_t first_round_interrupt_masked{};
  std::size_t first_round_frame_unknown_a7{};
  std::size_t first_round_frame_unproven_supervisor{};
  std::map<std::string, std::size_t> first_round_a7_unknown_by_reason;
  // The main-flow (tag 0) asynchronous writers.
  bool main_async_all{};
  std::size_t main_async_ranges{};
  std::size_t main_async_bytes{};
  std::size_t unknown_target_writer_stores{};  // stores with an Unknown target in analysed resuming instances
  // SEG-030-T009 correction cycle: live credited points where some child's register resumption is joined (a register effect, or
  // an unproven resumption), the subset where it is unproven, the partitions with a resumption, and the (partition, level) entries.
  std::size_t resumption_points{};
  std::size_t resumption_unproven_points{};
  std::size_t resumed_partitions{};
  std::size_t resumptions{};
  std::size_t unproven_resumptions{};
  std::size_t offset_resumptions{};  // SEG-030-T009 correction cycle 2: entries whose exits advance the frame PC (offset != 0)
  std::map<std::string, std::size_t> unproven_resumption_causes;  // per unproven child resumption, by cause
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
  std::size_t precise_reads{};          // memory-source operands of mutable RAM (or an Unknown address) read precisely
  std::map<std::pair<analysis::UnknownReason, M68kAnalysisSubReason>, std::size_t> unknown_reads;
  // SEG-030-T010: the stores in the analysed states (writer-only instances included) that may touch an observed range: the merged
  // bus ranges [first, last) of the known targets, clipped to the observed ranges, and the count of stores with an Unknown target.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> observed_store_ranges;
  std::size_t observed_known_stores{};
  std::size_t observed_unknown_target_stores{};
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
  M68kFrameConfig frames{};              // SEG-030-T006 (frames domain only)
  // SEG-031 (ADR 0080, hybrid admission closure). Both maps are empty for every SEG-030 report, whose output is unchanged.
  // island_entries: computed-site PC -> sorted distinct island target PCs (a proven finite superset of the site's targets). Wherever
  // the site is reached, each target is a computed edge carrying the site's own output state, exactly as a resolved site's targets
  // (a call enters the callee in the call-site context, and the targets are callees of its continuation). The edges are emitted
  // whether or not the site resolves (monotone; a superset of targets is sound).
  std::map<std::uint32_t, std::vector<std::uint32_t>> island_entries;
  // opaque_entries: source PC -> sorted distinct PCs entered with the opaque continuation state of the source's partition
  // (`entry_state(true, tag)`: registers and memory Unknown, stack delta Unknown, status = the partition's status bound) in context 0.
  // Used only for code with no proven source transfer (mandatory materialized executable images, entered from the startup entry).
  std::map<std::uint32_t, std::vector<std::uint32_t>> opaque_entries;
  // SEG-034 DIAGNOSTIC premise ablation, never credited (report-only ceiling measurements): a handler instance that the frames domain
  // cannot analyse (unmodelled parent, Unknown entry A7, depth bound) is treated as transparent: its resumption does not clobber
  // registers, memory, the status or the frame integrity of its parent. The handler's own code is still entered (covered). This is
  // exactly the assumption SEG-030-T009 removed; the flag measures what the interrupt-resumption class costs, nothing more.
  bool diagnostic_transparent_handlers{};
};

class M68kFiniteAdapter {
public:
  using State = M68kAnalysisState;

  M68kFiniteAdapter(const M68kAnalysisImage &image, M68kAnalysisConfig config);

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
  // SEG-030-T006: the same read under the asynchronous-writer policy of partition `tag`.
  [[nodiscard]] M68kMemoryRead read_memory_operand(std::uint32_t tag, const State &in, const M68kEffectiveAddress &ea,
                                                   std::uint32_t bytes, bool *tracked = nullptr) const;
  // SEG-030-T006: the memory policy of partition `tag` (the global policy unless the frames domain is enabled).
  [[nodiscard]] const M68kMemoryPolicy &policy_for(std::uint32_t tag) const;
  // SEG-030-T006: the status an instruction of partition `tag` runs with (Unknown when a child instance that may rewrite its saved SR
  // can be taken at the boundary before it).
  [[nodiscard]] analysis::FiniteValue effective_status(std::uint32_t tag, const State &in) const;
  // SEG-030-T006: true when a resuming synchronous child that may rewrite its saved SR resumes after `operation` (its status is then
  // Unknown).
  [[nodiscard]] bool clobbered_after(std::uint32_t tag, const M68kIrOperation &operation, const analysis::FiniteValue &status) const;
  // SEG-030-T006: the exception frame address of a boundary (A7 - 6 when S = 1 is proven, else Unknown).
  [[nodiscard]] M68kPointsTo frame_address(const analysis::FiniteValue &status, const State &in) const;
  // SEG-030-T006: classification of an RTE, RTR or RTS-away-from-the-entry-delta site (frames domain).
  [[nodiscard]] std::optional<M68kReturnSiteReport> evaluate_return_site(std::uint64_t point, const M68kIrOperation &operation,
                                                                         const State &in) const;
  // SEG-030-T008: the return-slot classification of an RTS at the entry stack delta (or with the delta untracked), memory domain
  // only; nullopt for any other point.
  [[nodiscard]] std::optional<M68kReturnSlotOutcome> classify_return_slot(std::uint64_t point, const M68kIrOperation &operation,
                                                                          const State &in) const;
  // The state of an entry: a root (initial memory) or an opaque continuation (callee stores).
  // SEG-030-T006: an opaque continuation's status is the status bound of its partition `tag`.
  [[nodiscard]] State entry_state(bool continuation, std::uint32_t tag = 0U) const;
  // SEG-030-T006: the seed of a root in partition `tag` (the reset state, a live handler instance's entry, or the entry state).
  [[nodiscard]] State root_state(std::uint32_t tag, std::uint32_t pc) const;
  // SEG-030-T005: an opaque continuation typed by `sub` (none: the T004 opaque entry).
  [[nodiscard]] State opaque_continuation(M68kAnalysisSubReason sub, std::uint32_t tag = 0U) const;
  // SEG-030-T006: the status bound of partition `tag` (M68kFrameConfig::status_bounds; Unknown in the dead-handler partition).
  [[nodiscard]] analysis::FiniteValue status_bound(std::uint32_t tag) const;
  // SEG-030-T005: the stack delta after `operation` (normal successors), from its input state.
  [[nodiscard]] analysis::FiniteValue stack_delta_after(const M68kIrOperation &operation, const State &in) const;
  // SEG-030-T005: the continuation state of the call at `pc` whose callees in this transfer are `callees` (empty: unresolved).
  [[nodiscard]] State continuation(std::uint32_t pc, const std::vector<std::uint32_t> &callees, const State &in,
                                   std::uint32_t tag = 0U) const;
  // SEG-030-T006: the memory write targets with the exception frame of `status` (frames domain).
  [[nodiscard]] std::vector<std::pair<M68kPointsTo, std::uint32_t>> memory_write_targets(const M68kIrOperation &operation,
                                                                                         const State &in,
                                                                                         const analysis::FiniteValue &status) const;

  // SEG-034 (ADR 0081): a store of `span` bytes through `target` whose offsets may run past a region's end is replaced by an
  // over-approximation that stays inside the regions: the in-region part clipped to the region's last `span` bytes plus the bytes
  // the spill can reach in the bus region that follows (at most `span` bytes starting at the region's end). Identical to `target`
  // when nothing spills; the unchanged (fail-closed) target when the following bytes are not a bounded region.
  [[nodiscard]] M68kPointsTo resolve_store_spill(const M68kPointsTo &target, std::uint32_t span) const;

private:
  void transfer_address_registers(const M68kIrOperation &operation, const State &in, State &out,
                                  const M68kMemoryPolicy *policy = nullptr) const;
  void transfer_memory(const M68kIrOperation &operation, std::uint32_t next, const State &in, State &out,
                       const M68kMemoryPolicy &policy, const analysis::FiniteValue &status) const;
  [[nodiscard]] M68kPointsTo operand_address(const State &in, const M68kEffectiveAddress &ea, std::uint32_t predecrement) const;
  [[nodiscard]] std::optional<M68kCellValue> operand_value(const State &in, const M68kEffectiveAddress &ea, std::uint32_t bytes,
                                                           const M68kMemoryPolicy &policy) const;
  [[nodiscard]] M68kMemoryRead read_memory_with(const M68kMemoryPolicy &policy, const State &in, const M68kEffectiveAddress &ea,
                                                std::uint32_t bytes, bool *tracked) const;
  // SEG-030-T009 correction cycle (frames domain): the register-preservation facts after `operation` (origin and saved cells).
  void transfer_origins(const M68kIrOperation &operation, const State &in, State &out, std::uint8_t data_written,
                        const M68kMemoryPolicy &policy, const analysis::FiniteValue &status) const;

public:
  // SEG-030-T009 correction cycle (frames domain): joins into `edge` (a state entering a boundary of partition `tag`) the register
  // resumption of every child of the partition that can be taken there (an interrupt eligible under the edge's status, unless
  // `interrupts` is false). With `raised_from` (the input of an instruction raising synchronous vectors), also the resumptions of the
  // synchronous clobber levels in `synchronous` (bit l: level l), whose preserved registers hold the values the instruction was
  // entered with. Correction cycle 2: only the entries whose exits advance the frame PC by `offset`. True when some entry applied.
  bool apply_resumptions(std::uint32_t tag, State &edge, const State *raised_from = nullptr, std::uint32_t synchronous = 0U,
                         std::int32_t offset = 0, bool interrupts = true) const;
  // SEG-030-T009 correction cycle 2 (frames domain): the offset the frame PC cell (A7)+2 of handler partition `tag` holds relative to
  // the stacked PC at `in` (an RTE there resumes at the stacked PC plus it), when the frame-PC fact proves it: the cell was written by
  // no store since the exception built it, or only by ADDQ/SUBQ/ADDI/SUBI.L #imm, and no asynchronous or external writer of the
  // partition may write it. nullopt otherwise (the frame PC may differ: never a resumption at the stacked PC).
  [[nodiscard]] std::optional<std::int32_t> frame_pc_offset(std::uint32_t tag, const State &in) const;

private:
  const M68kAnalysisImage &image_;
  M68kAnalysisConfig config_;
  std::map<std::uint32_t, M68kMemoryPolicy> tag_policies_;  // SEG-030-T006: the global policy joined with each partition's
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
  std::map<std::uint32_t, M68kReturnSiteReport> return_sites;  // SEG-030-T006 (frames domain only)
  M68kFrameReport frames;      // SEG-030-T006 (frames domain only)
  M68kReturnSlotReport return_slots;  // SEG-030-T008 (memory domain only)
  // SEG-030-T006: the partition tags of the writer-only instances of the returned round (M68kHandlerInstance::credited); their
  // points are in `solution` but never in `reached`, `undecodable`, the site reports or the read counts.
  std::set<std::uint32_t> writer_only_tags;
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
