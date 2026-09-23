#pragma once

// CPU-side source/decode boundary for static discovery.  A scenario supplies
// only a bounded, already-validated instruction span and its independent
// target provenance; this layer owns the selected decode profile, decoder
// invocation, rejection interpretation, and canonical address cache.

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/direct_flow.hpp"
#include "segarecomp/cpu/m68k/static_program.hpp"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace segarecomp {

struct M68kInstructionSource {
  std::span<const std::uint8_t> bytes{};
  // `source.image_offset` is relative to this bounded span because the shared
  // decoder uses it for bounds checks.  The independent image-global value is
  // restored into retained provenance before this CPU cache stores a result.
  DecodeSource source{};
  MoveqImageOffset provenance_image_offset{};
};

// External source admission failures deliberately stay typed.  They express
// only properties a CPU decoder cannot establish from a byte span; they are
// not machine/device diagnostic strings or opaque adapter codes.
enum class M68kInstructionSourceIssueKind { unmapped, conflicting_mapping };
struct M68kInstructionSourceIssue {
  M68kInstructionSourceIssueKind kind{M68kInstructionSourceIssueKind::unmapped};
  M68kProgramAddress address{};
  // SEG-014-T003: the exact mapping-claim query result the scenario's own
  // address-space admission already computed for `address` -- empty for
  // `unmapped`, the full (possibly >1) matched list for `conflicting_mapping`.
  // A discovery-time issue built from this carries it through unchanged so a
  // later scenario-side report projection never re-queries mapping claims for
  // an address this CPU boundary has already asked about.
  std::vector<MappingClaim> matched_claims;
};
using M68kInstructionSourceResult = std::variant<M68kInstructionSource, M68kInstructionSourceIssue>;

enum class M68kDiscoveryDecodeIssueKind {
  truncated_instruction,
  illegal_instruction,
  unsupported_instruction_form,
  valid_but_unsupported_instruction,
};
struct M68kDiscoveryDecodeIssue {
  M68kDiscoveryDecodeIssueKind kind{M68kDiscoveryDecodeIssueKind::valid_but_unsupported_instruction};
  M68kProgramAddress address{};
  std::optional<InstructionProvenance> provenance;
  std::optional<std::uint64_t> available_bytes;
  std::optional<std::uint32_t> requested_length;
  std::optional<std::uint32_t> instruction_length;
};
using M68kStaticDecodeResult = std::variant<M68kDecodedInstruction, M68kDiscoveryDecodeIssue>;

// One concrete cache used by a static discovery pass.  It is intentionally a
// value type rather than a callback framework: callers obtain a source only
// for a cache miss, while this CPU owner calls decode_m68k_instruction exactly
// once per source address and retains deterministic insertion order.
//
// SEG-014-T003 (post-checkpoint hardening): `entries()` exposes a
// const-only, read-only view. `decode_or_get` (the sole production mutator,
// used only by this CPU boundary's own discovery walk) inserts directly into
// the private `entries_` member; no legitimate caller -- inside or outside
// cpu/m68k -- has ever needed to insert, erase, or replace a canonical
// decoded instruction through this accessor, so no mutable overload is
// exposed. A hosting scenario's own compatibility frontend may consume the
// canonical decoded-instruction map this cache already populated, but must
// never mutate it.
class M68kStaticDecodeCache {
 public:
  [[nodiscard]] const M68kDecodedInstruction *find(M68kProgramAddress address) const;
  [[nodiscard]] M68kStaticDecodeResult decode_or_get(const M68kInstructionSource &source,
                                                      M68kDecodeProfile profile);
  [[nodiscard]] const std::map<std::uint32_t, M68kDecodedInstruction> &entries() const noexcept {
    return entries_;
  }

 private:
  std::map<std::uint32_t, M68kDecodedInstruction> entries_;
};

// SEG-014-T003: the production static-graph traversal boundary. The prior
// discover_m68k_general_startup implementation (src/m68k_pipeline_frontend.cpp,
// pre-T003) owned one recursive DFS + `switch (decoded.kind)` successor
// selector directly against the hosting scenario's own program/analysis
// types. That control-flow algorithm is pure MC68000 CPU behavior (which
// successor addresses a given decoded instruction kind produces, and how a
// LIFO call-frame stack is threaded through them) and belongs here; only the
// FACTS it needs from a hosting scenario -- an instruction's byte source,
// whether a candidate direct-branch/call target is legally admissible, how a
// statically-foldable memory operand's address classifies, and which RTS (if
// any) is the scenario's own opt-in synthetic completion sentinel -- are
// genuinely scenario-owned (address-space mapping, ROM/RAM/device routing,
// and the synthetic-completion contract are all scenario policy, not MC68000
// architecture). `M68kStaticDiscoveryEnvironment` below is exactly that
// narrow fact boundary: it never receives or returns a walk/recurse/
// successor-set/frame-stack decision, only typed facts about one already-
// identified address or request.

// A statically-known memory access request a decoded operand resolves to
// once its effective address has already been established as foldable
// (m68k_is_statically_foldable_control_ea) and canonicalized
// (m68k_canonical_ea_address) and its alignment already verified
// (m68k_startup_absolute_operand_alignment) by this CPU boundary itself --
// never by the environment, which only classifies an already-canonical,
// already-aligned address.
struct M68kCpuMemoryAccessRequest {
  M68kProgramAddress address{};
  M68kMemoryAccessWidth width{M68kMemoryAccessWidth::long_word};
  M68kMemoryAccessDirection direction{M68kMemoryAccessDirection::read};
  InstructionProvenance provenance{};
};

// A direct-branch target (BNE.short/BRA.short/the generalized Bcc/BRA
// `branch` kind/DBcc) is odd/mid-instruction checked by this CPU boundary
// itself (see discover_m68k_static_graph's own implementation) before ever
// reaching the environment; only mapped-address admission is asked of
// `admit_target`. A direct-call target (JMP/JSR/BSR) has never had a
// mid-instruction check and keeps its own distinct odd/24-bit/mapped
// admission rule entirely inside the environment, matching the pre-T003
// asymmetry between branch-target and call-target validation exactly.
// SEG-007-T047 / ADR-0020 §6: `interrupt_vector` is a bounded static hardware
// discovery root -- the long word at the mapped cartridge image's MC68000
// level-6 autovector table offset (0x78), or `synchronous_exception_vector`
// for a build-time-resolved synchronous exception vector such as vector 5.
// Both are resolved at build time exactly as
// the reset PC at offset 0x4 is. It reuses `direct_call`'s odd/unmapped/24-bit
// rejection precedent for admission but is entirely outside ADR-0013 §7's seed
// set `S = {reset entry} ∪ runtime_confirmed_seeds` (`|S| <= 4`): it never
// enters S, never increments seed_count, and never touches that ceiling.
enum class M68kDiscoveryTargetRole {
  direct_branch,
  direct_call,
  interrupt_vector,
  synchronous_exception_vector,
};

// The environment's mapped-address admission verdict for one candidate
// direct target. `matched_claims` mirrors the pre-T003 asymmetry precisely:
// for `direct_branch`, it is empty for `unmapped_direct_target` and the full
// (>1) matched list for `conflicting_address_mapping`; for `direct_call`, it
// is always empty (call-target admission collapses "zero matches" and "more
// than one match" into the same `unmapped_direct_target` category and never
// attaches a claims list, per validate_m68k_static_call_target's own
// pre-T003 behavior).
struct M68kMappingIssue {
  DirectFlowDiagnostic category{};
  std::vector<MappingClaim> matched_claims;
};

// Every discovery-time issue this walk can produce, whichever of the CPU-
// native (decode/structural) or environment-admitted (mapping/access) origins
// produced it. Deliberately a flat, verbose superset of what a hosting
// scenario's own rejection report needs, rather than a minimal-but-lossy
// shape, so an existing scenario-side projection can reconstruct its
// established report fields byte-for-byte from it. `address` is always the
// issue's own source/offending-instruction context address (never a
// candidate target address, which is carried separately in `target`).
struct M68kDiscoveryIssue {
  DirectFlowDiagnostic category{};
  M68kProgramAddress address{};
  std::optional<InstructionProvenance> provenance;
  std::optional<M68kProgramAddress> target;
  std::optional<std::uint32_t> instruction_length;
  std::optional<std::uint64_t> available_bytes;
  std::optional<std::uint32_t> requested_length;
  std::optional<std::vector<MappingClaim>> mapping_claims;
  std::optional<std::string> unresolved_reason;
  // Set whenever this issue arose from a `classify_memory_access`-rejected,
  // already-foldable memory operand (the only case this CPU boundary ever
  // calls `classify_memory_access` at all) -- regardless of the rejection's
  // own category. A pre-T003 general-startup frontier capture only ever
  // acted on this for two specific categories; that is deliberately a
  // scenario-side reporting decision (which categories are worth a retained,
  // sanitized access shape), not a CPU-side one, so this field is populated
  // uniformly here and the scenario's own projection selects which
  // categories it cares about.
  std::optional<M68kCpuMemoryAccessRequest> access;
  // True when a hosting scenario's own rejection projection must attach the
  // single mapping claim covering `address`, re-derived by that projection
  // exactly as the pre-T003 monolith's own decode-time
  // `set_claims(r, {claim})` calls always did for a decode-issue or
  // resolved-operand rejection reached only after `address` was already
  // uniquely claimed. Never set together with `mapping_claims` already
  // populated (the two other claim-populating cases --
  // M68kInstructionSourceIssue::matched_claims and
  // M68kMappingIssue::matched_claims -- always supply the exact list
  // directly instead, since it may legitimately be empty or contain more
  // than one claim).
  bool reconstruct_single_mapping_claim{};
  // True when a hosting scenario's own rejection projection must attach one
  // reconstructed "raw_cartridge_rom" instruction-fetch access record for
  // `provenance`, mirroring the pre-T003 monolith's own two identically-
  // shaped access-record pushes (a decode-issue's own verified provenance,
  // and a fully decoded instruction's own already-verified raw bytes).
  // Meaningful only when `provenance` is also present.
  bool reconstruct_instruction_read_access{};
  // True only for the decode-issue rejection shape whose own image offset
  // was always derived from `address`'s own containing claim regardless of
  // whether `provenance` happened to be available (the sole pre-T003
  // exception to "image offset is always exactly `provenance`'s own image
  // offset when provenance is present").
  bool set_pc_based_image_offset{};
};

// The three discovery-time engineering budgets a hosting scenario already
// owns (m68k_discovery_max_instructions/max_blocks/max_call_frame_depth in
// the pre-T003 monolith): bounded internal resource limits for this CPU
// boundary's own graph-construction effort, never a console hardware or
// timing fact -- see those constants' own extensive citation history for why
// they carry no hardware reference.
struct M68kStaticDiscoveryLimits {
  std::uint32_t max_instructions{};
  std::uint32_t max_blocks{};
  std::uint32_t max_call_frame_depth{};
};

// SEG-007-T161 / ADR-0022: the result of one bounded generation-time read of
// immutable in-cartridge bytes for the ADR-0009 offset-table finite-value
// producer. `bytes` is exactly the requested length in image order; `claim`
// is the single immutable mapping claim that fully covers the read.
struct M68kImmutableCartridgeBytes {
  std::vector<std::uint8_t> bytes;
  MappingClaim claim{};
};

// SEG-007-T164 / ADR-0023: the two logical-position parameters a trusted
// external logical-table-descriptor annotation supplies once its
// `base_address` and `entry_width_bytes` have already been matched by the
// environment against the reached site's own recognized table base and
// shape (ADR-0022 condition 1/3). This is NOT an independent `entry_count`
// field threaded into `m68k_fold_immutable_offset_table` -- it is consumed
// entirely inside `m68k_apply_finite_value_transfer`/
// `analyze_finite_index_values` to construct an alternate finite selector-
// value input (the ordered set `{0, stride_bytes, ...,
// (entry_count-1)*stride_bytes}`), which is then fed into the existing,
// unchanged folding function exactly as an ANDI/MOVEQ-derived selector
// would be.
struct M68kLogicalTableDescriptorHint {
  std::uint32_t stride_bytes{};
  std::uint32_t entry_count{};
};

// The narrow fact boundary discover_m68k_static_graph consumes. Every method
// answers a question about one already-identified address or request; none
// receives or returns a walk/recurse/successor-set/frame-stack decision, and
// none is called more than the traversal itself would have called the
// pre-T003 monolith's own equivalent inline logic.
class M68kStaticDiscoveryEnvironment {
 public:
  virtual ~M68kStaticDiscoveryEnvironment() = default;
  // The bounded, already-validated instruction byte span and independent
  // target provenance for `pc`, or the typed admission failure
  // (unmapped/conflicting) a scenario's own address-space mapping already
  // established for it.
  [[nodiscard]] virtual M68kInstructionSourceResult instruction_source(M68kProgramAddress pc) = 0;
  // Mapped-address admission for one already odd/mid-instruction-checked (for
  // `direct_branch`) or not-yet-checked (for `direct_call`, which owns its
  // own odd/24-bit/mapped rule entirely) candidate target. `std::nullopt`
  // admits the target; a populated `M68kMappingIssue` rejects it.
  [[nodiscard]] virtual std::optional<M68kMappingIssue> admit_target(
      M68kProgramAddress target, M68kDiscoveryTargetRole role) = 0;
  // Region/ROM-write/device-routing classification for an already-foldable,
  // already-canonical, already-aligned memory operand address. `std::nullopt`
  // accepts the access; a populated diagnostic rejects it.
  [[nodiscard]] virtual std::optional<DirectFlowDiagnostic> classify_memory_access(
      const M68kCpuMemoryAccessRequest &request) = 0;
  // Whether `rts_provenance` is the scenario's own opt-in synthetic-
  // completion terminal RTS (the pre-T003 monolith's own
  // `program.synthetic_completion && same(...)` check) -- always `false` for
  // a scenario that never opts into synthetic completion.
  [[nodiscard]] virtual bool is_completion_rts(const InstructionProvenance &rts_provenance) = 0;

  // SEG-007-T161 / ADR-0022: one bounded read of generation-time immutable
  // in-cartridge bytes, used only by the ADR-0009 immutable offset-table
  // finite-value producer. Returns `length` bytes in image order
  // only when `[base, base + length)` resolves entirely through exactly one
  // mapping claim (an immutable cartridge-image region by construction) with a
  // structurally valid affine image mapping and no arithmetic overflow; `std::nullopt` otherwise
  // (the producer then fails closed to ADR-0009's `unknown` state). This is a
  // generation-time image-inspection fact only: it is never consulted for a
  // control-flow, target-fetch, target-decode, or dispatch decision, and the
  // generated executable gains no runtime read path from it.
  [[nodiscard]] virtual std::optional<M68kImmutableCartridgeBytes> read_immutable_cartridge_bytes(
      M68kProgramAddress base, std::uint32_t length) = 0;

  // SEG-007-T164 / ADR-0023: an optional, explicitly opted-in, ROM-identity-
  // bound trusted logical-table-descriptor annotation for the ADR-0022
  // offset-table producer's *selector-position input* -- never a second
  // dispatch path or a new proof obligation bypass. `base_address` is the
  // reached site's own recognized table base
  // (`source_ea.pc_base_address + source_ea.displacement`); `entry_width_bytes`
  // is the recognized `MOVE.W (d8,PC,Xn),Dn` entry width (2). Returns the
  // matching annotation's `stride_bytes`/`entry_count` only when both fields
  // exactly match an opted-in, ROM-hash-verified annotation; `std::nullopt`
  // otherwise (no annotation opted in, no match at this base/width, or the
  // annotation failed a precondition upstream of this call) -- the caller
  // then falls through to the existing register-proven selector exactly as
  // before this task. The default implementation returns `std::nullopt`
  // unconditionally, so every environment that does not override it (every
  // synthetic/test environment, and any scenario that never wires an
  // external hints file) is completely unaffected by this method's
  // existence.
  [[nodiscard]] virtual std::optional<M68kLogicalTableDescriptorHint> logical_table_descriptor_hint(
      M68kProgramAddress /*base_address*/, std::uint32_t /*entry_width_bytes*/) {
    return std::nullopt;
  }
};

// The complete result of one bounded discovery pass: every canonically
// decoded instruction (via `decode_cache`, in first-decode order via
// `decode_order`), every recognized block-entry address in first-recognition
// order (`block_entries`), every discovered static edge/call frame, the
// synthetic-completion RTS's provenance if this pass reached it, and either
// nothing (the whole reachable graph, up to the configured budgets, resolved
// cleanly) or the primary blocking issue plus any SEG-007-T064-style
// best-effort sibling issues a hosting scenario's own frontier-promotion
// policy may separately evaluate.
struct M68kStaticDiscoveryResult {
  M68kStaticDecodeCache decode_cache;
  std::vector<M68kProgramAddress> decode_order;
  std::vector<M68kProgramAddress> block_entries;
  std::vector<M68kStaticEdge> edges;
  std::vector<M68kStaticFrame> frames;
  // SEG-007-T124 / ADR-0009: one retained fact per source instruction whose
  // computed/indirect control EA was proven via the bounded finite-index-
  // value producer. Empty for a program with no such instruction.
  std::vector<M68kIndirectTargetEaSet> indirect_target_ea_sets;
  // SEG-007-T174 / ADR-0024: one retained fact per source instruction whose
  // control EA was the recognized brief PC-relative indexed form but whose
  // value-flow proof failed (see `M68kUnprovenIndirectControlEaSet`). Empty
  // for a program with no such instruction; never overlaps
  // `indirect_target_ea_sets` for the same source address.
  std::vector<M68kUnprovenIndirectControlEaSet> unproven_indirect_control_ea_sets;
  // SEG-021-T028: final-owner invariant. Every retained computed-control source
  // whose EA shape is Tier-2 eligible must end with a Tier-1 fact or a Tier-2
  // fact; sources of an ineligible shape (long/address-indexed, A7) are the
  // explicit typed-unsupported class and are never listed. Must be empty.
  std::vector<M68kProgramAddress> ownerless_tier2_eligible_control_sources;
  std::optional<InstructionProvenance> completion_rts;
  std::optional<M68kDiscoveryIssue> primary_issue;
  std::vector<M68kDiscoveryIssue> secondary_issues;
  // SEG-007-T180 / ADR-0026: count of statically-resolved control transfers
  // (unconditional/conditional direct branch taken target, JSR/BSR/foldable
  // JMP-JSR callee, ADR-0009 indirect candidate) whose destination was a
  // member of `independent_unit_boundaries` and therefore had its edge/frame
  // recorded and its block entry noted, but was NOT recursively walked in
  // this pass's own discovery budget -- its body is owned by that boundary
  // address's own independently-validated unit. Zero for a pass invoked with
  // an empty boundary set (every existing caller), keeping behaviour
  // byte-identical.
  std::uint32_t stitched_boundary_edges{};
  // SEG-007-T213 / ADR-0026 §7: the exact set of `independent_unit_boundaries`
  // member addresses this pass actually reached as a validated
  // control-transfer destination and stopped its recursive walk at (the
  // dependency set the fixed-point admission computation uses to decide which
  // OTHER surviving candidates must be revalidated when a boundary member is
  // removed). Empty for a pass invoked with an empty boundary set.
  std::set<std::uint32_t> stitched_boundary_addresses;
  // SEG-007-T181 / ADR-0027: count of `fallthrough_continuation` edges this
  // pass recorded because a `next_pc` step (sequential fallthrough or
  // branch/call continuation) landed on a member of
  // `fallthrough_continuation_boundaries` -- the continuation body is charged
  // to that boundary address's own unit, not this pass's budget. Distinct from
  // `stitched_boundary_edges` (control transfers). Zero for every pre-T181
  // caller (empty boundary set).
  std::uint32_t stitched_fallthrough_continuation_edges{};
  // SEG-007-T181 / ADR-0027: canonical MC68000 program addresses at which this
  // pass hit its instruction ceiling on a `next_pc` step whose continuation
  // decoded cleanly as a prefix-boundary shape (no open control target, empty
  // unresolved reason) and was reached by sequential fallthrough / branch or
  // call continuation -- i.e. eligible for a Phase-2-synthesized
  // fallthrough-continuation unit. A genuine unresolved open control edge at
  // the ceiling is NOT reported here and keeps its existing fatal diagnostic.
  // Empty for every pre-T181 caller.
  std::vector<M68kProgramAddress> fallthrough_continuation_frontier;
  // SEG-007-T182 / ADR-0028: canonical MC68000 program addresses at which this
  // pass hit its instruction ceiling on the FIRST instruction of a
  // statically-resolved+validated direct control-transfer destination that was
  // NOT also reached by a `next_pc` step, and whose entry decoded cleanly as a
  // prefix-boundary shape. Eligible for a Phase-2-synthesized
  // resolved-control-target unit; the A -> X edge (and, for a call, the frame)
  // are already recorded and only X's body walk is partitioned. A genuinely
  // unresolved open control edge at the ceiling is NOT reported here. Empty for
  // every pre-T182 caller.
  std::vector<M68kProgramAddress> resolved_control_target_frontier;
};

// SEG-014-T003: the production CPU-owned static-graph traversal, moved
// verbatim in structure from the pre-T003 discover_m68k_general_startup's own
// recursive DFS + `switch (decoded.kind)` successor selector
// (src/m68k_pipeline_frontend.cpp, pre-T003 commit history). Every
// admissibility/classification decision a hosting scenario alone can make is
// asked of `environment`; every control-flow decision (which successor(s) a
// given decoded kind produces, DFS order, the LIFO call-frame stack, T062/
// T064 best-effort sibling exploration, visited-state/edge/frame
// deduplication identity) is owned entirely here, unchanged from the
// pre-T003 monolith.
// SEG-007-T180 / ADR-0026: `independent_unit_boundaries` is an optional set of
// canonical MC68000 program addresses each independently proposed AND locally
// admitted as its own bounded static unit by the offline code-entry inventory.
// When this pass reaches one of those addresses as a statically-resolved
// control-transfer destination (direct branch taken target, JSR/BSR/foldable
// JMP-JSR callee, or an ADR-0009 indirect candidate), the transfer itself is
// still validated, the A->B edge/call-frame is still recorded, and B is still
// noted as a block entry -- but B is NOT pushed onto this pass's worklist:
// B's body is charged to B's own independently-validated unit, not to A's
// discovery budget. Sequential fallthrough and call/branch continuation
// (`next_pc`) are NEVER guarded and are always enqueued. An empty set (the
// default) makes this pass byte-identical to every pre-T180 caller.
// Boundary addresses are canonical MC68000-program address values (the same
// `M68kProgramAddress::value` domain the walker keys its worklist on).
[[nodiscard]] M68kStaticDiscoveryResult discover_m68k_static_graph(
    M68kProgramAddress entry, const M68kStaticDiscoveryLimits &limits,
    M68kStaticDiscoveryEnvironment &environment,
    const std::set<std::uint32_t> &independent_unit_boundaries = {},
    // SEG-007-T181 / ADR-0027: kept SEPARATE from
    // `independent_unit_boundaries` (ADR-0026 §2 forbids guarding a plain
    // `next_pc` as control flow). This set is consulted ONLY when a `next_pc`
    // step is about to be enqueued: if the continuation address is a member,
    // the pass records a `fallthrough_continuation` edge and a block entry but
    // does not walk the continuation body (it is owned by that boundary's own
    // unit). Empty for every pre-T181 caller.
    const std::set<std::uint32_t> &fallthrough_continuation_boundaries = {});

}  // namespace segarecomp
