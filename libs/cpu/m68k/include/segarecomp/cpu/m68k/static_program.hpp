#pragma once

// SEG-014-T002: CPU-level static program representation (docs/architecture/
// post-seg007-architecture-refactor-contract.md, section 8.2 "cpu/m68k/").
// Relocated from include/segarecomp/m68k_pipeline.hpp per
// docs/architecture/seg-014-t001-symbol-migration-map.md's `cpu/m68k/`
// section ("CPU-level static program/discovery"). These are profile-neutral
// facts beyond a bare decoded/lifted operation: static block/unit membership,
// edges, and direct call/return identity. They currently have only one
// populating profile (genesis_rom_startup/general_startup) because no other
// selected profile needs call/return facts, not because they are
// startup-specific types.
//
// The functions that populate these types (discover_m68k_static_call_return,
// discover_m68k_general_startup) are NOT moved here: both take/return the
// Genesis-shaped FrontendProgram/FrontendAnalysis/FrontendResult family
// (machine/genesis-owned per the migration map), which does not move in
// this task. Moving the functions here without those types would either
// duplicate them or reach back into m68k_pipeline.hpp -- the forbidden
// cpu/m68k -> machine/genesis direction. The migration map itself defers
// this exact split to SEG-014-T003 ("Ownership decisions and deferred
// seams identified by T001"); this header only relocates the pure fact
// types that have zero Genesis dependency of their own, matching the map's
// explicit list.

#include "segarecomp/core/address.hpp"
#include "segarecomp/core/provenance.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/direct_flow.hpp"
#include "segarecomp/cpu/m68k/effective_address.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace segarecomp {

struct M68kStaticCall { InstructionProvenance caller{}; M68kProgramAddress continuation{}; M68kProgramAddress callee{}; };
// `direct_branch`: a direct (conditional-taken or unconditional) branch edge
// discovered by discover_m68k_general_startup (BNE.short taken, or BRA.short
// taken). It deliberately covers both cases with no added `condition` field:
// the distinction is always recoverable from the edge's `source_instruction`
// provenance and analysis.decoded by looking up that instruction's kind, so
// no second copy of the condition is carried here.
// SEG-007-T124 / ADR-0009 (docs/decisions/0009-computed-indirect-control-
// flow-target-resolution.md): `indirect_branch`/`indirect_call` represent one
// candidate member of a proven, finite `M68kIndirectTargetEaSet` for a
// non-foldable computed control EA (currently the brief PC-relative indexed
// form). Each such edge still carries exactly one candidate target, matching
// every other edge kind's shape; the multi-candidate set itself is recovered
// from the source instruction's own retained `M68kIndirectTargetEaSet` fact,
// never encoded redundantly on the edge.
// SEG-007-T181 / ADR-0027: `fallthrough_continuation` is a distinct edge kind
// from the intra-walk `fallthrough` block split. It is synthesized only by the
// Phase-2 driver when a root walk reaches its per-root instruction ceiling at a
// sequential-fallthrough / branch-or-call-continuation `next_pc` step whose
// continuation address is an environment-admitted, cleanly decoding
// fallthrough-continuation unit boundary. It carries only decode position +
// entry-connectivity (ADR-0027 §2); it introduces no runtime-visible control
// boundary and no branch/jump is emitted at it (generated C already splits
// adjacent address-order blocks). It keeps its target a retained aggregate
// block entry exactly like `fallthrough` so the merged program stays
// entry-connected (ADR-0014 P1).
enum class M68kStaticEdgeKind { fallthrough, direct_call, return_to_continuation, direct_branch, indirect_branch, indirect_call, fallthrough_continuation };
struct M68kStaticBlock { BlockId id{}; std::vector<InstructionProvenance> instructions; };
struct M68kStaticFrame { M68kStaticCall call{}; };
struct M68kStaticEdge {
  InstructionProvenance source_instruction{};
  M68kStaticEdgeKind kind{M68kStaticEdgeKind::fallthrough};
  M68kProgramAddress target{};
  std::optional<M68kStaticCall> call;
};
// Shared static call/return discovery (SEG-005-T006). Consumes the shared
// decoded JSR and candidate RTS operations already produced by
// decode_m68k_instruction/lift_m68k_instruction and constructs the one
// verified call identity, its direct-call/return edges, and its LIFO static
// frame. It recovers no other call, indirect edge, or dynamic state; an RTS
// is associated with this exact call identity by verified source address,
// never by a numeric stack value. This is the sole owner of the
// call/continuation/callee identity fact: no other stage (static decode-time
// validation, execution, or C lowering) recomputes it independently.
struct StaticCallReturnDiscovery {
  M68kStaticCall call;
  M68kStaticEdge call_edge;
  M68kStaticEdge return_edge;
  M68kStaticFrame frame;
};

// SEG-007-T124 / ADR-0009: a proven finite set of possible raw low-word
// register values for one Dn used as a control-EA index, established by a
// bounded forward register-value analysis over already discovered direct
// control flow (see ADR-0009, "Target-set proof and boundaries"). `values`
// is ordered, deduplicated, and non-empty whenever this fact exists at all
// (an unresolved/unbounded/unrepresentable producer never constructs one).
// An index register other than Dn, or a long-size index, is initially
// unrepresentable per the ADR and never reaches this fact at all.
struct M68kFiniteIndexValueSet {
  InstructionProvenance source_instruction{};
  DataRegister index_register{DataRegister::d0};
  std::vector<std::uint16_t> values;
};

// SEG-007-T124 / ADR-0009: the proven, finite set of candidate target
// addresses for one non-foldable brief PC-relative indexed control EA
// (`(d8,PC,Xn)`), constructed verbatim by discovery by evaluating
// `canonical(pc_base_address + signed_d8 + sign_extend_16(index_value))` for
// every proven index value, then sorting, deduplicating, rejecting an empty
// result, and applying existing target admission to every candidate.
// `control_ea` is the exact decoded control EA this set was proven for (its
// `pc_base_address`/`displacement`/`index_reg` fields are the ones C11
// lowering evaluates at run time); `candidates` is ordered and deduplicated.
struct M68kIndirectTargetEaSet {
  InstructionProvenance source_instruction{};
  M68kEffectiveAddress control_ea{};
  M68kFiniteIndexValueSet index_values{};
  std::vector<M68kProgramAddress> candidates;
};

// SEG-007-T174 / ADR-0024: the weaker sibling fact recorded when a control
// instruction's decoded EA is the recognized brief PC-relative indexed form
// (`pc_index8`, ADR-0009's own target-EA shape) but `compute_indirect_target_set`
// could not prove a finite index-value set for it (an unknown/unbounded
// producer, an unrepresentable index kind, or a candidate that failed
// evaluation/canonicalization) -- i.e. the EA FORM is recognized, but the
// VALUE-FLOW proof failed. This carries no candidate list and no value set:
// it is deliberately just enough for Tier 2 (ADR-0024) to re-derive the exact
// same faithful runtime EA computation ADR-0009's Tier 1 already lowers, so
// generated C can check that computed value against the generation-time
// `EmittedCodeAddressSet` instead of a per-site proven candidate array. A
// site with a full `M68kIndirectTargetEaSet` (Tier 1) never also gets one of
// these -- discovery constructs at most one of the two facts for a given
// source instruction, chosen only by whether the finite-value proof
// succeeded, and C4 lowering picks Tier 1 XOR Tier 2 by which one exists.
struct M68kUnprovenIndirectControlEaSet {
  InstructionProvenance source_instruction{};
  M68kEffectiveAddress control_ea{};
  // `true` for JSR (a call -- Tier 2 pushes the ordinary continuation
  // `source_instruction.source.address.value + source_instruction.length.value`
  // onto the runtime call stack before dispatch, mirroring Tier 1's own JSR
  // lowering), `false` for JMP (a plain branch -- no stack write at all).
  bool is_call{};
};

// SEG-007-T252 / ADR-0040 correction: the former SEG-007-T150/ADR-0016 static
// finite-loop-progress proof and SEG-007-T155/ADR-0017 (extended by ADR-0018/
// ADR-0019) generated data-transform progress proof producers -- along with
// their `M68K_LOOP_PROOF_MAX_*` bounds, `M68kLoopRegisterBank`,
// `M68kFiniteLoopProgressProof`, `M68kDataProgressDirection`,
// `M68kDataProgressAccessDirection`, `M68kDataProgressRegion`,
// `M68kGeneratedDataProgressMember`, `M68kGeneratedDataProgressProof`, and
// their matching/membership helpers -- were removed in this task. Their only
// consumer was the generated-runtime progress-watchdog note-emission
// machinery ADR-0040 retired; after that removal they computed proofs no
// codegen consumer read. See ADR-0040 §7 for the historical record and
// `libs/cpu/m68k/src/static_loop_proof.cpp`'s own removal in this same task.

// CPU-only construction of already-validated direct-call facts.  Mapping and
// rejection policy remain injected by the scenario adaptor; these functions
// never inspect an image, machine map, or device.
[[nodiscard]] M68kStaticCall m68k_make_static_call(const M68kDecodedInstruction &call,
                                                    std::uint32_t callee_address);
[[nodiscard]] M68kStaticEdge m68k_make_static_call_edge(const M68kStaticCall &call);
[[nodiscard]] M68kStaticEdge m68k_make_static_return_edge(
    const M68kStaticCall &call, const M68kDecodedInstruction &return_instruction);

// SEG-007-T151: the reachable-RTS trace `StaticGraphWalk::synthesize_return_edges`
// (static_discovery.cpp) already performs per single-seed walk, extracted here
// as a pure function of an already-established decoded-instruction map and
// call-frame set so it can also run over a MULTI-SEED AGGREGATE decode set.
// A per-seed walk's own reachable-RTS trace is bounded by that walk's own
// local decode cache: if the seed that discovers a call's own frame never
// itself decodes deep enough (past an internal branch, budget permitting) to
// reach one of the callee's legitimate exit RTS instructions, no per-seed
// walk alone ever emits that RTS's own `return_to_continuation` edge, even
// though a DIFFERENT, independently promoted seed's own walk may separately
// admit that exact RTS as a retained decoded instruction. Re-running this
// exact trace over the aggregate cross-seed decode map after every seed's
// own walk has merged recovers exactly the return edges no single seed's
// local view could establish alone -- purely additive over already-verified
// facts (the call frame, and the decoded successor chain from its callee
// entry to the RTS), never a new architecture decision or a narrower/
// broader edge-identity rule. Every RTS this function finds unreachable
// (never decoded, in the aggregate, along any successor path from a call's
// own callee entry) yields no edge for that call, exactly like the
// per-seed version. Deterministic and side-effect-free: two identical
// inputs produce byte-identical, ordered output.
//
// SEG-007-T190 / ADR-0031 (Stage B, purely additive): `accepted_tier1_by_source`
// is the set of ALREADY-ACCEPTED post-stitch Tier-1 `M68kIndirectTargetEaSet`
// facts, keyed by the exact source instruction address of the computed `JMP`
// each was proven for. It defaults empty, so every existing call site -- in
// particular the per-seed walk's own call in `static_discovery.cpp`, which has
// no post-stitch Tier-1 knowledge -- omits it and stays byte-identical. When a
// traced `JMP` source address is a key of this map (mutually exclusive with the
// existing statically-foldable-control-EA branch: a source is only ever
// unproven-then-proven, never both), the reachable-RTS successor set for that
// `JMP` additionally includes the entry's already-validated candidate
// addresses, so an `rts` reached only through a Tier-1-proven computed jump
// still yields its `return_to_continuation` edge under the ORIGINAL enclosing
// `(caller, callee, continuation)` call identity (SEG-007-T151 per-callee
// framing, unchanged -- no new frame is fabricated for the Tier-1 target).
[[nodiscard]] std::vector<M68kStaticEdge> m68k_reachable_return_edges(
    const std::map<std::uint32_t, M68kDecodedInstruction> &decoded_by_address,
    const std::vector<M68kStaticFrame> &frames,
    const std::map<std::uint32_t, M68kIndirectTargetEaSet> &accepted_tier1_by_source = {});

} // namespace segarecomp
