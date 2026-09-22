# ADR-0039: Runtime-state-owned dynamic control and monotonic AOT representation

- Status: Accepted
- Date: 2026-09-15
- Deciders: SEG-007-T236
- Extends: ADR-0011 and ADR-0024.
- Narrowly supersedes: ADR-0028 §8's and ADR-0038's assumption that an
  RTS-terminal block with no surviving `return_to_continuation` edge has no
  executable representation.

## Context

The assisted Genesis route reached a valid, decoded, lowerable RTS through an
ordinary retained predecessor, but completed-prefix construction erased the RTS
solely because no exact static return edge owned it. The generated runtime did
not need such an edge to execute RTS semantics: its existing lowering already
reads the real longword at emulated A7 through `genesis_route_access`, validates
the observed PC against ADR-0011's deterministic whole-program continuation
authority, advances A7, and dispatches the observed PC.

A bounded experiment retained this ownerless RTS only in the existing assisted
runtime-routed profile. The first experiment exposed a second copy of the old
assumption in C4's `downstream_ok` / `candidate_has_executable_closure`: every
RTS with zero return edges was classified as an unrepresentable Tier-1
candidate closure before the final emitted-set predicate ran. Aligning those
two checks allowed generation, strict-C11 compilation, and generated-native
execution. The previously selected RTS consumed the real stack PC, accepted it
through the existing generic continuation membership, and execution advanced
to a later runtime-selected unresolved-indirect-target frontier.

## Decision

### Runtime state owns dynamic-control values

For generated-native runtime-routed execution:

- JSR and BSR push their real architectural continuation and dispatch their
  real target.
- JMP dispatches its real target.
- RTS pops and dispatches the real PC from emulated A7/stack.
- RTE restores architectural state and PC from its exception frame and
  dispatches that restored PC.

Static control-flow facts may prove and optimize these transfers, but they do
not substitute a guessed target for the value produced by emulated CPU state.

### Existing AOT membership remains the safety authority

Every dynamic target must already be represented by generated AOT code or pass
an existing strictly stronger generic authority. RTS continues to use
ADR-0011's sorted whole-program return-continuation set, including call-shaped
Tier-2 continuations already admitted by ADR-0024. Non-membership, malformed
stack state, and routed-memory failure remain deterministic source-provenanced
stops. This decision adds no dispatcher, target database, learner, runtime
decoder, interpreter, JIT, or runtime code generation.

### Representation is monotonic

**Representation is monotonic; static CFG ownership refines knowledge but does
not erase valid AOT code merely because ownership is incomplete.**

The architecture distinguishes these facts rather than treating them as one:

1. decoded and lowerable;
2. AOT represented and emitted;
3. statically reachable;
4. exact static target known;
5. exact frame known;
6. return owner known; and
7. optimization eligible.

For the narrow ownerless-RTS case implemented here, retention and C4 emission
require all of the following:

- the assisted offline-inventory/runtime-routed profile is active;
- an ordinary retained predecessor targets the RTS block;
- generic runtime return authority exists prospectively during retention and
  concretely after cut-aware emitted-set construction; and
- the final predecessor is emitted.

Tier-1 candidate closure uses the same structural predicate with the retained
source set, while final terminal validation repeats it with the final emitted
source set and final return-target authority. No `M68kStaticCall`, frame,
`return_to_continuation` edge, caller, callee, continuation, or target is
fabricated.

## Supersession boundaries

ADR-0028 and ADR-0038 remain authoritative for their graphs, transactional
closure, exact-frame facts, typed frontiers, pruning of genuinely unreachable
or unsafe blocks, deterministic ordering, and fail-closed behavior. Only their
existential inference that zero surviving static return edges necessarily means
“no executable RTS representation” is superseded for the predicate above.

ADR-0011's whole-program continuation membership and ADR-0024's Tier-2 emitted
code-address membership remain unchanged. The non-runtime-routed shadow-frame
profile and the unassisted/raw profile remain unchanged.

## Validation and consequences

Project-authored synthetic coverage proves a Tier-1-mediated ownerless RTS has
no frame or return edge, survives candidate closure without a Tier-1 pre-PC
cut, is emitted with its predecessor, and is reached by two call-shaped Tier-2
sites. Strict-C11 compiled execution proves both real stack continuations
return through the shared RTS. Non-member return PCs, malformed stack state,
missing ordinary predecessors, and Tier-2 targets absent from the emitted set
remain fail-closed. Existing exact-frame and unassisted-profile fixtures remain
passing, and repeated generated output is byte-identical.

The canonical pinned assisted route generated once, compiled once, executed the
ownerless RTS through generic membership, and advanced to a later runtime
unresolved-indirect-target frontier. No runtime-confirmed seed, resource-ceiling
change, hint change, or title-specific production rule was introduced.

Successful adoption requires full post-experiment refinement, not isolated
continuation patches, for exactly these workstreams:

1. monotonic represented executable identity;
2. common emitted/compiled-address membership for dynamic JMP/JSR/RTS/RTE where
   appropriate;
3. demotion of CFG/frame/retention from executable-existence authority;
4. an independent broad aligned-M68k-ROM AOT representation cost experiment
   measuring generated source size, object size, compile cost, and runtime
   dispatch before any exhaustive-representation decision; and
5. a platform-neutral `CodeImage` / `CompiledCodeMap` /
   runtime-address-to-AOT-location abstraction for future backends.

SEG-007-T244 completes workstream 4 with an explicitly opted-in independent
representation model. Every aligned start in uniquely mapped immutable ROM is
considered before execution. A represented entry means only that, if
architectural PC equals that address, an independently decoded, lifted,
validated, already-compiled body faithfully implements the supported start.
It does not assert source-level code intent or static reachability. Therefore
decodable bytes conventionally used as data may be compiled safely and remain
inert unless architectural PC selects them.

These identities live beside, not inside, ordinary CFG-backed blocks. They
create no root, edge, frame, call, return, or target fact. Distinct overlapping
valid starts are separate PC identities; exact same-PC agreement deduplicates,
while any same-PC provenance or semantic disagreement fails the translation
closed. Invalid, unsupported, unsafe, mutable, ambiguously mapped, or
unlowerable starts gain no body, dispatcher entry, or compiled membership.
Runtime dispatch remains architectural PC to immutable compiled-entry lookup
to generated native semantics, with no opcode fetch, decoder, interpreter,
JIT, learning, or runtime generation.

The full-ROM experiment reached generated-native execution and advanced past
the prior Category-3 frontier. A sorted immutable address/body lookup replaced
the initial linear dispatcher only after its benchmark-compilation phase hit
the pre-existing wall-clock ceiling. The replacement changes lookup shape,
not identity or execution semantics, and all resource ceilings remain
unchanged. The capability remains an explicit generic opt-in while its larger
source/object cost is visible; ordinary mode output is unchanged.

### Exact PC obligations of independent AOT entries

SEG-021-T026 closes an ordering gap exposed after the experiment. Immutable-ROM
AOT entries are populated after static discovery and ADR-0038 retention, so an
entry's exact PC assignments were not part of those passes' authoritative edge
or frontier obligations. The final generated program could consequently
contain a valid AOT body whose sequential or direct successor had neither a
compiled entry nor a typed frontier; executing that body then reached the
defensive internal-dispatch-inconsistency stop even though the earlier static
inventory truthfully reported no obligation of its own left unrepresented.

The final cut-aware compiled-address set and existing typed-frontier set remain
the only target authorities. At generation time, codegen derives each AOT
entry's fixed successors from the shared `M68kOperationEffect`: ordinary
advance, direct branch/jump/call target, and both outcomes of conditional
branch/DBcc. A represented successor dispatches unchanged. An exact successor
absent from both existing authorities receives a relation-specific,
source-provenanced `known_but_unemitted_target` stop in that AOT producer after
the instruction retires; it is never handed to the dispatcher. An effect that
cannot be classified this way rejects generation closed. Runtime-derived
control retains its stronger existing membership owner (for example RTS uses
the whole-program return-target set), so this check neither creates a second
CFG/target database nor weakens any dynamic-control guard.

A bounded production diagnosis reproduced the ordering gap with a
runtime-selected identity that classified generically as a legal decoded
PC-indexed dynamic jump. That identity was absent from discovery, unit
admission, semantic partitioning, retained blocks, and typed frontiers.
Immutable-ROM enumeration decoded it, but the existing AOT contract rejected
both its dynamic source-EA class and its lack of complete isolated C emission;
it therefore never became an AOT identity and was never later pruned. A
separate admitted AOT producer nevertheless had a fixed `advance` effect whose
exact-PC obligation selected that identity. The final consistency rule above
now converts precisely this cross-stage absence into the producer's typed,
source-provenanced stop. This is the durable public classification only;
address-specific evidence came from temporary local tracing, is not retained,
and supplies no fixture, target fact, or basis for broadening dynamic-jump AOT
admission here.

The generated stop uses the matching `known_but_unemitted_target` diagnostic
category and locally attaches mapping/instruction-fetch provenance from the
same validated AOT decoded record and unique mapping already accepted for
emission. Only an AOT producer with a non-empty unrepresented exact-PC set
emits this attachment; ordinary retained sources keep the existing global
attachment function unchanged, and all other AOT identities add no provenance
text. This makes sanitized/full reporting valid without a runtime ROM read, a
second provenance authority, or a whole-ROM attachment switch.
