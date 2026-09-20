# ADR 0009: Bounded Computed Indirect Control-Flow Target Resolution

- Status: Accepted
- Date: 2026-08-30
- Amends: ADR 0002 and ADR 0003
- Related: ADR 0006

## Context

The static pipeline records unresolved indirect control flow rather than guessing
(`docs/architecture/pipeline.md`, lines 15-19 and 26-30).  Its current M68k
walker applies that policy at the `JMP`/`JSR` foldability gate:
`src/cpu/m68k/static_discovery.cpp`, lines 516-556 rejects a non-foldable EA
with `DirectFlowDiagnostic::reached_unresolved_direct_edge` before it constructs
an edge or call frame.  The gate accepts only absolute word, absolute long, and
PC-displacement EAs (`src/cpu/m68k/effective_address.cpp`, lines 5-8).

The decoded EA and source provenance already cross into IR exactly once
(`src/cpu/m68k/ir.cpp`, lines 5-25), and `JMP`/`JSR` become `jump_general` and
`call_general` (`src/cpu/m68k/ir.cpp`, lines 54-55).  Their current effect
provides a direct target only through that foldability predicate
(`src/cpu/m68k/effects.cpp`, lines 490-512).  C4 correspondingly requires one
direct call edge and a matching direct target (`src/codegen/c11/frontend.cpp`,
lines 1809-1859 and 2153-2169); the shared M68k static-program facts currently
model direct calls/returns only (`include/segarecomp/cpu/m68k/static_program.hpp`,
lines 38-62).

The runtime is intentionally not an alternative discovery engine. Generated
`genesis_dispatch` selects only an emitted block identity and otherwise calls
`genesis_internal_dispatch_inconsistency_stop`
(`src/codegen/c11/frontend.cpp`, lines 2257-2264); the runtime drive loop only
consumes `GENESIS_CONTINUE_AT_PC` (`runtime/genesis/runtime.c`, lines
1012-1028). This implements ADR 0002's
rule that runtime dispatch must not fetch or decode target opcodes
(`docs/decisions/0002-static-translation-and-fail-closed-execution.md`, lines
16-19).

The immediate reached form is the base-MC68000 brief PC-relative indexed EA.
The decoder presently treats mode 7/register 3 as out of scope
(`src/cpu/m68k/decode.cpp`, lines 143-177); the adjacent An-indexed parser
defines the reusable brief-extension validation and index facts
(`src/cpu/m68k/decode.cpp`, lines 185-227).  The general-startup projection
already maps `reached_unresolved_direct_edge` to
`GenesisFrontierClass::unresolved_indirect_target`
(`src/machine/genesis/frontend.cpp`, lines 1415-1429).

## Decision

Use a **bounded hybrid**:

1. **Static discovery owns proof and reachability.**  It must recover an
   ordered finite target set for a particular non-foldable control EA, admit
   every member with the existing target-admission policy, and walk every
   admitted member before emission.  It records a typed indirect-target-set
   fact keyed by the source instruction provenance and decoded control-EA
   descriptor, plus a resolved-indirect edge (and, for `JSR`, a
   candidate-specific call/frame identity) for each member.
2. **C11 lowering owns runtime EA evaluation and set membership.**  It computes
   the decoded EA from architectural registers, checks that the resulting
   canonical 24-bit address is a member of the retained ordered set, then sets
   the existing program counter.  It does not select from source-image bytes,
   inspect an opcode, or create a new dispatch mechanism.
3. **The existing dispatcher remains the only address-to-emitted-block
   dispatcher.**  The next drive iteration dispatches the admitted PC through
   the existing statically emitted block list.  Its inconsistency fallthrough
   remains defensive only and is not the normal indirect-target diagnostic.

This is deliberately neither pure runtime dispatch (which could not establish
which blocks to translate) nor pure static recovery (which would not evaluate a
runtime register-dependent EA). It preserves ADR 0002's static-translation
invariant while using the existing C4/runtime PC seam only after static proof.

### Target-set proof and boundaries

The architectural source of truth is a typed, finite **proven target-EA set**,
not a pointer-table encoding. Its initial producer is bounded index-value/range
recovery from existing statically represented dataflow and CPU effects. For a
particular control instruction, `M68kFiniteIndexValueSet` carries the source
instruction provenance, selected Dn/An index register and decoded word/long
interpretation, and an ordered, deduplicated non-empty set of possible raw
register values. `M68kEffectiveAddress` gains a typed `pc_base_address`, set by
decode to the first extension-word address for every PC-relative EA; the
target-EA evaluator consumes that field and may not rederive a PC base from an
emitter-local instruction length or host pointer.

The initial `M68kFiniteIndexValueSet` producer is a finite forward register
analysis over already discovered direct edges. Its state for each Dn is either
`unknown` or a sorted unique set of low-word values with a hard maximum of 256
members. At a merge, equal register sets are retained and unequal finite sets
are unioned only if the result remains within 256; otherwise that register
becomes `unknown`. The analysis iterates the direct graph to a fixed point;
revisiting a state that would widen a set past the cap produces `unknown`, never
a partial set.

Initially it supports only these generic Dn transfers: `MOVEQ` creates its
single low-word value; `ANDI.W #mask,Dn` maps each finite input through the
ordinary word-width AND effect, and maps an unknown input to the finite set of
all low-word submasks of `mask` when `popcount(mask) <= 8`; and any other write
to that Dn produces `unknown`. The value at an indexed control instruction is
available only if every retained predecessor fixed-point state is finite. This
is a generic CPU-effect rule, not a title-, table-, address-, or input-specific
rule. An An index, long index, other transfer, or a mask with more than eight
set bits is initially unrepresentable and fails closed; a later ADR may add a
new producer without changing the target-EA-set abstraction.

`M68kIndirectTargetEaSet` carries that value-set fact, the decoded PC-indexed
brief EA, source instruction provenance, and its ordered, deduplicated
`M68kProgramAddress` candidates. Discovery constructs it verbatim by evaluating
`canonical(extension_word_address + signed_d8 + index_value)` once for every
proven raw value, where `index_value` is `sign_extend_16(Xn)` for a word index
and the 32-bit `Xn` value for a long index, using the decoded Dn/An kind and
checked 24-bit canonicalization. It then sorts and deduplicates the resulting
addresses, rejects an empty result, applies existing target admission and
independent decode checks to every candidate, and walks every admitted target
before emission. A candidate is an ordinary static block entry: when it is a
branch stub or any other ordinary translated block, discovery follows its
normal direct successors rather than treating it as a table entry or terminal.

The initial producer neither reads a ROM pointer table nor requires target
addresses to be encoded as pointers. A future producer may prove a finite index
value set from an immutable table descriptor, but that is an alternative input
to `M68kFiniteIndexValueSet`, not a prerequisite or a different dispatch path.
It must satisfy the same finite-value and per-candidate EA evaluation contract.

### Multi-candidate control and emission contract

`M68kStaticEdgeKind` gains `indirect_branch` and `indirect_call`. Each indirect
edge carries the source provenance, exactly one candidate target, and the
source-keyed `M68kIndirectTargetEaSet` identity. An indirect `JSR` creates one
`M68kStaticCall`/`M68kStaticFrame` per candidate with the common decoded
continuation and its own callee; its return edge refers to that same
candidate-specific call identity. This is the only representation T124 may use
for a multi-candidate call; it must not overload `direct_call` or pretend that
one direct target represents all candidates.

C4 validates a computed `JMP`/`JSR` only when exactly one retained target-EA
set belongs to the terminal operation, its sorted candidates equal the complete
set of `indirect_branch`/`indirect_call` edges, every candidate is a static
block entry, and every indirect-call candidate has exactly one matching static
frame. C4 emits `static const uint32_t indirect_targets_<source>[]` containing
only those sorted address literals. It evaluates the typed EA, tests that value
against that exact array, and only on membership performs the existing call
stack write (for `JSR`) and assigns `runtime->pc`. It returns the existing
`GENESIS_CONTINUE_AT_PC` transfer so the unchanged `genesis_dispatch` selects
the already emitted candidate block on the next drive iteration. No C4 code
may inspect source bytes, reconstruct a candidate, or enumerate a target at
runtime.

No inferred or ambiguous value set; unknown/unbounded producer; unsupported
extension format; full-format or memory-indirect EA; non-base-MC68000 scale;
overflow or non-24-bit EA evaluation; empty result; failed target admission; or
failed target decode is representable. The decoder must model PC-indexed brief EA distinctly from
An-indexed EA: the latter's current legal mask deliberately excludes it from
control addressing (`include/segarecomp/cpu/m68k/instruction.hpp`, lines
123-129), so T124 must add a separate typed EA mode and control legality rather
than reuse an An base.

If `M68kFiniteIndexValueSet` or `M68kIndirectTargetEaSet` construction cannot
establish every value and candidate condition above, discovery must retain the existing
`reached_unresolved_direct_edge` at the source instruction provenance; the
Genesis frontend then reports `unresolved_indirect_target`. If a proved set
exists but C11's `m68k_indirect_target_member` guard sees a non-member runtime
EA, generated C must return `genesis_static_stop` with
`GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET`,
`GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE`, and the control instruction's
`runtime_source`, before changing PC or, for `JSR`, pushing the continuation.
The emitter serializes only that set's sorted candidate address literals in the
containing emitted block and compares the computed canonical EA against them;
it does not serialize source bytes or a table. This follows the existing C4
frontier stop-class mapping (`src/codegen/c11/frontend.cpp`, lines 792-823) and
the existing call-lowering stop construction (`src/codegen/c11/m68k.cpp`, lines
1765-1787). Neither case may fall back to interpretation, target-opcode
decoding, speculative walking, or a ROM-specific allowlist.

### Ownership and provenance

T124's changes are bounded as follows:

- `instruction.hpp` and `decode.cpp` add the distinct PC-indexed brief decoded
  EA and its `pc_base_address`; `effective_address.*` owns pure CPU EA
  evaluation/canonicalization.
- `static_program.hpp` and `static_discovery.*` own finite index-value facts,
  target-EA-set facts, resolved-indirect edges, candidate-specific JSR frames,
  deterministic target ordering, and static target admission/walking.
- `ir.*` remains the sole decoded-to-lifted carrier of source provenance and EA
  facts.  `effects.*` states the typed control-transfer request but does not
  make a Genesis mapping or table-proof decision.
- `src/codegen/c11/m68k.cpp` lowers the computed EA and membership guard;
  `src/codegen/c11/frontend.cpp` validates/emits the multi-candidate static
  shape and reuses, rather than replaces, `genesis_dispatch`.
- `genesis_dispatch` and `genesis_runtime_drive` must not gain source-image
  access, target instruction decoding, or a second dispatch loop.

The facts retain source-instruction provenance, the index-value producer
provenance, and each walked target's decoded provenance. Edges and frames retain
their source/candidate identities through IR and C4; the generated stop uses
the source instruction provenance. This extends ADR 0003's one-owner rule:
direct call identity is currently computed once by discovery and consumed by
execution/emission (`docs/decisions/0003-genesis-startup-shared-route-boundary.md`,
lines 93-118), and indirect candidate identity must have the same owner.

ADR 0006 applies only if a runtime data access to a proved immutable cartridge
region is needed. Its generated backing region is a read-only data mechanism
(`docs/decisions/0006-generic-cartridge-data-region-ownership.md`, lines 60-68)
and explicitly leaves instruction dispatch unaffected (lines 128-153). A future
immutable-table value-set producer may inspect the generation-time input image
under its own proof; it does not invoke ADR 0006. ADR 0006 may route a later
dynamic immutable *data* read, never target fetch, target decode, or dispatch.

## Consequences and T124 Acceptance Shape

SEG-007-T124 must implement and test this one mechanism with synthetic legal
fixtures. A positive fixture must establish a finite index-register value set
from preceding ordinary, statically represented CPU effects, evaluate the
brief PC-indexed EA for every value, and prove the sorted/deduplicated targets.
It must demonstrate that a brief PC-indexed `JSR` resolves through that set to
ordinary translated branch-entry blocks, follows their normal direct successors,
and returns through the matching candidate-specific static continuation. `JMP`
may share the mechanism only where it uses the same proof, representation, and
emission seams. The current
C11 operation-lowering owner is
`src/codegen/c11/m68k.cpp`, lines 1747-1809: it currently writes the static JSR
continuation and direct target, and must not acquire a runtime decoder.

It must reject at the named finite-value recovery, EA-evaluation, target-
admission, and `m68k_indirect_target_member` checks: an unknown/unbounded
index-value path, unsupported effect or merge, empty value/target set,
overflow/non-24-bit/unmapped/unaligned/undecodable candidates, unsupported
brief/full/memory-indirect forms, and a runtime target outside the retained
set. It must assert that the non-member generated stop names the source control
instruction and leaves PC/A7 unchanged. It must preserve existing direct forms,
emit byte-for-byte deterministic C for identical input, compile generated C as
strict C11, and contain no runtime instruction fetch or decode. The
runtime-selected PC-indexed `JSR` is accepted only when its finite value-derived
target set is proven and walked through to emitted ordinary blocks; otherwise
its current fail-closed frontier remains the required result.

### Producer extension (SEG-007-T178): finite An code-address set for register-indirect control

- Date: 2026-09-06. Additive under the "a later ADR may add a new producer
  without changing the target-EA-set abstraction" framing above; the Decision
  list, "Target-set proof and boundaries", "Multi-candidate control and emission
  contract", and "Ownership and provenance" sections are unchanged. This
  follows the SEG-007-T124/SEG-007-T160 "apply or additively extend the existing
  mechanism, never a competing one" precedent. Size is not a stop condition.

- Newly reached shape (normalized): the authorized composed-hints Sonic route
  reaches `stop_class = unresolved_indirect_target` /
  `diagnostic_category = reached_unresolved_direct_edge` at a pure
  address-register-indirect control transfer (`JMP (An)` / `JSR (An)`, EA mode 2,
  no displacement/index/extension word). This is outside the existing producer
  (PC-indexed brief EA + finite Dn low-word value set): the control EA is the
  architectural `An` value itself, and the feeding register is an address
  register, not a Dn index. `JMP (An)` / `JSR (An)` already decode as a legal but
  non-statically-foldable control EA; no decoder or EA-legality change is
  required, only a new distinct typed producer and a widened foldability gate.

- New abstract domain (distinct typed fact, not reused from the Dn low-word
  value semantics): per-An state is `unknown` OR a sorted, deduplicated,
  non-empty set of canonical 24-bit code addresses, under the SAME hard 256
  cap as the Dn set. It is carried alongside the Dn value state in the same
  bounded forward fixed-point register analysis over already-discovered direct
  edges, with the same monotone merge/termination argument. An address value is
  available at a control site only if EVERY retained predecessor fixed-point
  state for that An is finite; any `unknown` predecessor forces fail-closed.

- **Scope: ordinary address registers A0-A6 only.** The finite code-address
  proof is available for A0-A6. **A7/SP is intentionally excluded** because of
  its pervasive implicit stack semantics: `PEA` performs `A7 -= 4`; `LINK` /
  `UNLK` mutate A7 alongside the named frame register; `JSR` / `BSR` change A7
  on the callee-entry edge by pushing the return address; `RTS` / `RTE` consume
  stack state. `m68k_written_address_registers` cannot fully enumerate every
  such implicit mutation, so an unrestricted A0-A7 domain would admit a stale
  finite A7 proof (e.g. `LEA Target,A7 -> PEA (...) -> JMP (A7)`). The exclusion
  is made explicit at the producer/consumer boundary
  (`m68k_an_finite_proof_eligible(reg) == (reg < 7)`), not left to instruction
  writers: `LEA <foldable>,A7` and `ADDA.W #imm,A7` leave A7 `unknown`, and
  `JMP (A7)` / `JSR (A7)` stay fail-closed under this Tier-1 mechanism. A future
  one-shot / Tier-2 `EmittedCodeAddressSet` mechanism may handle runtime
  `(A7)` targets; that is out of scope here. The shared
  `m68k_is_supported_computed_control_ea` predicate still classifies pure
  `(A7)` as a computed-control EA so an unresolved `(A7)` site keeps the
  SEG-007-T174 cross-root un-supersession protection.

- Recognized generation-time-provable An producers ONLY:
  1. `LEA <abs.W / abs.L / PC-foldable>,An` -> the single canonical 24-bit
     code-address member `m68k_canonical_ea_address(source_ea) & 0x00FFFFFF`
     (LEA of a non-foldable EA -> `unknown`).
  2. `ADDA.W #imm,An` -> maps each finite input member through the width-correct
     add: `canonical24(member + sign_extend_16_to_32(imm))`. `ADDA.L #imm,An` is
     admitted only if the route requires it (word form is what the route needs).
  Any other write to that An -- a memory/RAM source, `MOVEA`/`MOVE` into An from
  a non-immediate, an `(An)+`/`-(An)` auto-update side effect, `MOVEM` into An,
  an unmodelled op, or crossing a call whose effect on An is not proven by the
  existing bounded call/return graph -- forces `unknown`. An `An` written from
  memory elsewhere in unrelated routines therefore yields `unknown` on any path
  reaching the site through such a write; only paths whose every merged
  predecessor state is finite resolve.

- Merge, cap, and call discipline: alternative CFG paths are joined by
  deterministic sorted-set union under the 256 cap; a union that would exceed
  the cap becomes `unknown` (never a partial set). The existing value-flow
  successor rule that marks a call continuation `force_unknown` discards the
  whole register state across a call, except where the bounded callee
  register-write footprint proof below shows the callee provably never writes a
  given `An`/`Dn` (A7 is never preserved).

- Foldability gate: `JMP (An)` / `JSR (An)` resolve ONLY with a non-empty,
  bounded, independently-validated finite target set. The proven An members ARE
  the canonical candidates directly (EA = architectural `An`; no displacement,
  index, or extension-word arithmetic). Every candidate is admitted through the
  existing static target validation/walk; one inadmissible candidate fails the
  whole proof closed and retains `reached_unresolved_direct_edge`.

- Emission: through the existing `M68kStaticEdgeKind::indirect_branch` /
  `indirect_call` edges, the source-keyed `M68kIndirectTargetEaSet` fact, the
  `indirect_targets_<source>[]` sorted-literal array, and the
  `m68k_indirect_target_member` runtime guard. The guard computes
  `EA = runtime->a[n]` (no base/index arithmetic). A non-member runtime EA
  returns the existing `GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET` /
  `GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE`, leaving PC and A7 unchanged and
  naming the source instruction. C4 validates only when exactly one retained
  target-EA set belongs to the terminal op, its sorted candidates equal the
  complete set of indirect edges, every candidate is a static block entry, and
  every indirect-call candidate has exactly one static frame. Unknown/unbounded
  An retains the exact current fail-closed behavior.

- Bounded callee register-write footprint across a call continuation (added
  when the route's real `JMP (An)` site was found to set `An` before a
  `JSR`/`BSR` whose continuation previously wiped the whole register state):
  a finite `An` (and, symmetrically, `Dn`) value set survives a `JSR`/`BSR`
  continuation only when a bounded proof shows the callee never writes that
  register. The proof walks the already-decoded instructions reachable from the
  statically known callee entry (BSR displacement, or a foldable JSR control
  EA), following non-`force_unknown` value-flow successors, stopping at
  `rts`/`rte`, over the same decoded set and the same
  `max_call_frame_depth` / visited-address ceiling the return-edge machinery
  uses; it unions `m68k_written_data_registers` / `m68k_written_address_registers`
  over that set and recurses into a nested `JSR`/`BSR` callee's own footprint
  under the same depth bound. Any missing decode, unknown/too-deep callee, or
  exhausted depth budget makes the footprint "writes everything" and the
  continuation falls back to the pre-existing whole-state-unknown behavior. A7
  is never preserved (call push/pop). Because a callee body reachable only
  through the same walk may not be fully decoded when the site is first
  drained, the An-indirect site is retried once after the main discovery drain,
  when every reachable callee body is decoded; a still-unprovable set then
  records the unchanged fail-closed `reached_unresolved_direct_edge`. This adds
  no new ceiling, decoder, dispatcher, or hint kind.

- Cross-root aggregation protection: the SEG-007-T174 guard that stops an
  unresolved computed-control site from being treated as superseded merely
  because another independently walked root *decodes* the same instruction
  without proving its target set now keys on a shared
  `m68k_is_supported_computed_control_ea` predicate (`effective_address.hpp`),
  true for exactly the two computed-control EA classes this ADR represents
  through `M68kIndirectTargetEaSet` -- `pc_index8` and pure `address_indirect`
  (`displacement == 0`, `extension_words == 0`). A genuinely Tier-1-proven site
  (a retained `M68kIndirectTargetEaSet` for that source address) is still
  superseded/resolved normally.
