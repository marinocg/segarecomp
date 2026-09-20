# ADR-0030: Bounded call-context-sensitive ADR-0009 finite-An proof

- Status: Accepted
- Date: 2026-09-08
- Deciders: SEG-007-T189 (architecture-class task), consuming SEG-007-T188's
  final corrected evidence (ADR-0029) and the SEG-007-T189 executed
  falsification gate over the T188-corrected canonical stitched control-edge
  relation.
- Amends: ADR-0009 (computed indirect control-flow target resolution) and
  ADR-0029 (canonical stitched control-edge relation consumption). Every rule
  either ADR states -- the fixed-point merge semantics (commutative,
  idempotent, monotone toward `unknown`, finite + unknown = unknown), the
  256-member finite-set cap, the A7/SP finite-proof exclusion, the sole
  post-stitch reachability authority (`m68k_canonical_control_adjacency`),
  and the two recognized An producers (`LEA <foldable>,An`;
  `ADDA.W #imm,An`) -- is unchanged. This ADR adds one new, bounded,
  additive analysis capability at the existing post-stitch An-proof
  consumption boundary; it changes no decoder, no lifter, no code generator,
  and no emitted-code identity.
- Related: ADR-0011 (bare-address worklist, non-recursive call/return),
  ADR-0026 (offline inventory partitions into stitched units), ADR-0028
  (ceiling-triggered resolved-control-target unit synthesis).

## Context

SEG-007-T188 established that, after canonical-graph parity, a
runtime-selected blocker remains: `context_insensitive_finite_plus_unknown_
shared_entry_join` (case C). Two authoritative caller contexts reach the same
computed `JMP (An)` site through the canonical control adjacency
(`m68k_canonical_control_adjacency`, ADR-0029): one reaches it with `An`
proven finite by a supported producer chain (foldable `LEA` / `ADDA.W #imm`,
preserved across any intervening direct call by the existing bounded
callee-write-footprint proof); the other reaches it with `An` genuinely
`unknown`. The unweakened context-insensitive ADR-0009 merge (finite +
unknown = unknown) collapses the site to `unknown`, discarding caller A's
otherwise-sound proof.

SEG-007-T189's mandatory falsification gate performed the definitive
backward trace of the second (unknown) caller over
`m68k_canonical_control_adjacency`, from the shared computed `JMP (An)` site
to the first causal seam where the potentially-finite `An` fact becomes
`unknown`. The trace found: the finite caller's last `An` writer is a
supported `LEA <foldable>,An` producer, optionally composed with a
conditionally-guarded `ADDA.W #imm,An` (both branches of that guard remain
finite, so the join at the guard itself stays finite), then crosses one
direct call whose bounded callee-write-footprint proof shows the callee never
writes that `An` (so the finite fact survives the call unmodified). The
unknown caller's last `An` writer, by contrast, is a genuine memory-load
producer (an `An`-register load from a fixed absolute/indexed memory
operand) -- a producer class ADR-0009 has never recognized and Non-goals
explicitly excludes recovering (memory SSA / general alias analysis is out
of scope for both this ADR and the parent task). No bounded producer or
propagation recovery exists for the second caller's `An` value without
crossing that explicit boundary. The gate therefore CONFIRMS the RESULT-D
premise recorded by SEG-007-T188's continuation disposition: both caller
contexts are authoritative, caller A's finite proof is genuinely valid,
caller B is genuinely unknown/incompatible, and the join legitimately
produces `unknown` under a context-INSENSITIVE analysis. Only a
call-context-sensitive extension can preserve caller A's proof at the shared
site while caller B correctly stays fail-closed.

## Decision

### 1. Context identity

A **bounded call context** is identified by exactly one address: the source
instruction address of a single `direct_call` canonical edge
(`M68kStaticEdgeKind::direct_call` with a resolved `edge.call.callee`,
ADR-0029's edge-kind table). This is a depth-1 call-site tag -- a 1-CFA-style
context, not a call string. There is exactly one additional distinguished
context, the **base (context-insensitive) context**, which is always
computed first and is exactly today's (pre-T189) single context-insensitive
fixed point (`M68kStaticGraphWalker::analyze_finite_register_values` called
once over the complete post-stitch `merged_decoded` / `merged_edges`, with no
seed). The base context is always admissible; it is also the implicit
target every non-tracked (widened or ordinary) contribution already flows
into, because it is unchanged from every pre-T189 release.

A call site becomes a **tracked context** only when its OWN contributed
register state -- the base context-insensitive walk's post-instruction state
at that call-site address, computed by the SAME unmodified transfer function
(`m68k_apply_finite_value_transfer`) the base walk always used -- is finite
for at least one A0-A6 register. An indirect call, a call site whose base
walk never reaches it, or a call site whose contributed state is unknown for
every eligible register is never tracked; its analysis is entirely the
existing (unchanged) base context.

**Round 3: two seed points per tracked context, not one.** A tracked call
site's own contributed register state may reach a shared computed-control
site through either of two genuinely distinct sub-shapes, and this ADR's
context identity (Decision 1's single call-site address) covers BOTH without
becoming a deeper/different kind of context:

- **Continuation-seeded (the original, round-1/round-2 sub-shape):** the
  finite fact is established in the CALLER, survives the direct call via the
  existing bounded callee-write-footprint proof, and the shared site is
  reached only AFTER the call returns, by the caller's own post-return
  control flow. The re-walk is seeded at the call's own CONTINUATION address
  with the footprint-adjusted contributed state (`m68k_canonical_control_
  adjacency` deliberately excludes any RTS-to-continuation edge, so a
  callee-entry-rooted re-walk could never reach this sub-shape).
- **Callee-entry-seeded (round 3, added by SEG-007-T189's own continuation
  within this task):** the shared computed-control site is reached
  deterministically from INSIDE the body of the very callee this call site
  directly calls -- never via this call's own post-return continuation (the
  real Sonic frontier's actual shape, confirmed by fresh diagnosis and an
  independent LLDB runtime cross-check; see this task's Evidence). The
  re-walk is seeded at the call's own CALLEE-ENTRY address with the RAW
  call-site contributed state, applying NO callee-write-footprint filtering:
  the callee has not executed yet at its own entry, so it sees the caller's
  pre-call/call-site register state directly. The footprint proof answers a
  different question (does a register survive INTO the caller's post-return
  continuation) and would incorrectly filter registers the callee itself
  writes LATER in its body, before this proof observes their still-finite
  value at entry.

A single tracked call-site context may be relevant via BOTH seed points at
once (for the same or different unresolved sites); each relevant (call site,
seed point) pairing is re-walked independently and unions into the same
per-site candidate set as any other admissible context (Decision 5). This is
still a depth-1 mechanism (Decision 2): the seed-point choice is a property
of HOW one call-site context's own re-walk is rooted, not a second dimension
of context identity, a call-string, or a new context-transport mechanism.

### 2. Maximum context depth: 1 (no call-string stacking)

A tracked context is identified by a single call-site address, never a
sequence. Re-entering another distinguishing call boundary INSIDE a tracked
context's own re-walk does not create a nested/deeper context; that inner
call's own post-call continuation and any inner indirect-control proof are
resolved entirely within that one context's own (otherwise completely
unmodified) fixed point, using the SAME context-insensitive merge ADR-0009
has always used at that inner scope. This is a deliberate, documented bound:
depth-1 call-site sensitivity is the SMALLEST extension that resolves the
confirmed case-C shape (both authoritative callers reach the shared site
through a SINGLE distinguishing direct-call boundary, with no ambiguity
before that boundary on either path), and it requires zero new state-space
management beyond "one extra bounded re-walk per tracked call site." A site
whose ambiguity requires distinguishing TWO OR MORE nested call boundaries to
resolve is not covered by this mechanism and correctly remains `unknown`
under the existing, unweakened merge -- exactly the same fail-closed
behavior as before this ADR for any case this bound does not reach.

### 3. Selection policy: register-aware, seed-point-aware, reachability-directed, then bounded at a fixed cap

**Corrected policy (round 3; this section replaces the round-2 "reachability-
directed, ignoring register/seed-point" description in full -- not an
append -- for the reasons in "Selection-policy corrections" below).** The
unit of selection is no longer a call site alone; it is a **(call site, seed
point)** pair (Decision 1's two seed points). A pair is only WORTH tracking
if it can actually help resolve a real ambiguity at a SPECIFIC unresolved
site's SPECIFIC register. Concretely:

1. First compute the base (context-insensitive) fixed point (Decision 1),
   exactly as every pre-T189 release did.
2. Identify every eligible computed `JMP`/`JSR (An)` site the base walk
   itself leaves unresolved, paired with the EXACT An register that site's
   addressing mode indexes (its own post-instruction state is not finite for
   that register). A site the base walk already resolves needs no additional
   context and never influences selection at all.
3. For every candidate call site (a direct-call site whose own contributed
   register state is finite for at least one eligible An -- Decision 1) and
   every still-unresolved (site, register) pair: first check REGISTER
   relevance -- the candidate's own contributed state must be finite for
   THAT EXACT register (a candidate finite only for A0 is never relevant to
   an unresolved `JMP (A3)`, regardless of reachability). Only a
   register-relevant candidate proceeds to a REACHABILITY check, run
   independently for each of the two seed points, over the SAME canonical
   control adjacency (`m68k_canonical_control_adjacency`, ADR-0029's sole
   post-stitch authority -- never a second graph): does the candidate's own
   CONTINUATION address reach the unresolved site, and/or does the
   candidate's own CALLEE-ENTRY address reach it? Each search terminates
   unconditionally: its visited-address set is bounded by the finite
   decoded-entry count, exactly the same discipline the base walk's own
   worklist and the callee-write-footprint proof's own visited set already
   rely on for termination over this graph -- no new reachability authority,
   no additional unbounded traversal.
4. A (call site, seed point) pair becomes a **tracked unit** only if that
   seed point's reachability search proves it can reach at least one
   register-relevant still-unresolved site. A call site can therefore
   contribute UP TO TWO tracked units (continuation-seeded and/or
   callee-entry-seeded), each independently gated by its own reachability
   check -- a call site relevant only via callee entry for one site and only
   via continuation for a different site is tracked correctly at both,
   never conflated into a single unit or forced to pick one seed point
   globally. A pair that reaches no register-relevant unresolved site
   contributes nothing a tracked re-walk could rescue and is never tracked --
   it costs zero budget regardless of its own address.
5. Among the register- and reachability-relevant UNITS, the total number
   SEPARATELY re-walked across the whole proof pass is still bounded by a
   fixed, documented constant, `kM68kMaxTrackedCallContexts = 4` (this
   constant now bounds tracked UNITS, not call sites -- a single call site
   relevant via both seed points can consume up to two of the four slots).
   Relevant units are ordered canonically by their own call-site numeric
   address, with the continuation-seeded unit for a given call site ordered
   before its callee-entry-seeded unit when both are relevant (never by
   worklist/traversal order, which the T188 determinism argument already
   established is not itself a stable ordering primitive), and only the
   first `kM68kMaxTrackedCallContexts` of that ordered relevant set are
   re-walked; any additional relevant units beyond that bound are not
   separately re-walked. Each selected unit still gets exactly ONE bounded
   re-walk (never once per (unresolved site, unit) pair): the SET of
   relevant units is computed first, then each is re-walked exactly once,
   and its resulting per-address state contributes to every unresolved site
   it happens to reach.

This is sound, never a fabricated fact, in every direction: an untracked
IRRELEVANT (register- or reachability-wise) unit's contribution is exactly
what the base context-insensitive walk already computes for its call site
(unchanged from every pre-T189 release) -- it was never going to help any
unresolved site regardless of budget. An untracked RELEVANT unit excluded
only by the fixed cap (step 5) also falls back to the existing,
always-correct context-insensitive treatment -- conservative widening
toward `unknown` at the union step (see Decision 5), never toward a
falsely-admitted target. The bound is deliberately small: each tracked unit
re-runs the full (already bounded, already terminating) fixed point over the
complete post-stitch graph, so the total work is
`O(kM68kMaxTrackedCallContexts)` additional full re-walks, plus (up to) two
bounded reachability searches per (candidate, unresolved site) pair --
itself `O(|candidates| * |unresolved sites| * |decoded entries|)` in the
worst case, still a fixed, finite, terminating function of the post-stitch
graph, computed once per proof pass, not per node or per traversal step.

**Selection-policy corrections (cumulative).**

- *Round 2:* an earlier iteration selected the `kM68kMaxTrackedCallContexts`
  smallest-address candidates GLOBALLY, without regard to which unresolved
  site (if any) they could reach. That policy is unsound in practice (though
  never unsound in the sense of fabricating a false target): on a real
  program with many call sites, the smallest-address finite-contributing
  candidates are essentially never the ones relevant to any SPECIFIC
  unresolved computed-control site, so the fixed budget was almost always
  spent on irrelevant contexts while the one relevant context -- however
  large its address -- went untracked. Round 2 fixed this by establishing
  reachability relevance (from the continuation seed point only) BEFORE the
  cap is applied.
- *Round 3:* round 2's reachability check considered a call site "relevant"
  if it could reach an unresolved site AT ALL, regardless of which register
  that site needs, and it searched reachability from the continuation seed
  point only. Both were insufficient: a call site finite only for `A0` could
  still consume a tracking slot for an unresolved `JMP (A3)` merely because
  it structurally reached that address (register-blind relevance); and a
  call site whose finite fact reaches a shared computed-control site by a
  direct call straight into the shared callee's own entry -- never via any
  caller-side post-return reconvergence -- could never be selected at all,
  because `m68k_canonical_control_adjacency` deliberately excludes any
  RTS-to-continuation edge, so a continuation-only reachability search can
  never discover this sub-shape (the real Sonic frontier's actual shape;
  see this task's Evidence). Round 3 fixes both: relevance is now
  register-aware (gated on the exact indexed register) AND seed-point-aware
  (checked independently from both the continuation and the callee-entry
  seed points), so the fixed budget is spent only on units that can
  actually affect a specific unresolved site's specific register, via
  whichever seed point genuinely reaches it.

### 4. Recursion / cycle behavior

Each tracked context's re-walk is the SAME unmodified
`analyze_finite_register_values` fixed point the base context already uses
-- the same bare-address worklist (ADR-0011), the same visited/queued sets,
the same monotone lattice and 256-member cap, the same termination argument.
No new recursion or cycle concern is introduced: a call-graph cycle reachable
from a tracked context's seed is handled by that SAME already-existing,
already-terminating machinery (the bounded callee register-write-footprint
proof's own `max_call_frame_depth`-bounded recursion, and the worklist's own
monotone convergence over a finite address space) exactly as it always has.
Because context identity has depth exactly 1 (Decision 2), there is no
context-stack growth to bound separately; "cycle" at the context level would
only mean re-selecting the SAME single call-site address as a tracked
context, which is a no-op (it is either already tracked or not, keyed by its
own address in an ordinary `std::map`/`std::vector`, no re-entrant context
construction).

### 5. Multi-context target-fact production and merge-back

At a given `JMP`/`JSR (An)` computed-control site, the final candidate `An`
address set is the UNION of every admissible context's OWN independently
finite value set for that register: the base context (if finite there) and
every tracked context whose own re-walk proves that register finite at that
address. A context that is unknown there contributes nothing to the union;
it is never promoted and never causes the union to widen to unknown by
itself -- only the ABSENCE of any finite contributor does. This is the
"merge-back": contexts do not merge their raw register STATES before the
union (that would reopen the pre-T189 collapse); they merge only their
already-independently-proven OUTPUT candidate sets, at the exact point
ADR-0009's existing per-site proof already produced one. The existing
256-member cap and 24-bit-address validation apply identically to the union
as they always applied to a single context's set: a union that would exceed
256 members is discarded entirely for that site (fails closed, exactly the
existing "a union past the cap -> unknown" rule extended to the union of
independently-finite contexts, not weakened for any single context).

**Soundness of one global, address-keyed `M68kIndirectTargetEaSet` (Tier 1)
feeding the existing unmodified Tier-1/Tier-2 dispatch.** Emitted/generated
code identity remains program-address-only (Decision 6); the SAME physical
`JMP`/`JSR (An)` instruction executes regardless of which caller context
reached it at runtime. Every member of the published union was
INDEPENDENTLY, statically proven reachable from that exact instruction under
SOME authoritative context, then independently validated exactly as ADR-0009
already requires (decoded, admitted, and a retained block entry -- unchanged
downstream ownership: `M68kIndirectTargetEaSet` -> independent per-candidate
validation -> ADR-0028 synthesis if needed -> `indirect_branch` /
`indirect_call` edges -> retention -> `EmittedCodeAddressSet` -> the existing
Tier-1/Tier-2 runtime membership dispatch). At runtime, an unknown-context
execution's actual `An` value either (a) happens to equal a member of the
published union -- in which case jumping there is CORRECT, because that
target is a genuinely valid destination of this exact instruction under the
context that proved it, and code identity does not depend on which context
is currently executing; or (b) does not equal any member -- in which case
Tier 2's existing runtime membership guard correctly fails closed, exactly
as it always has for any non-member value. No new unsoundness is introduced:
the guard's discriminating power was always "is this runtime value a member
of the statically retained set for this source address", never "which
caller context produced this value", so publishing the per-context union at
one address is exactly as sound as publishing any other single finite,
independently-validated set was before this ADR. No change to Tier-1 or
Tier-2 dispatch code, representation, or ownership was required or made.

### 6. Emitted/generated code identity remains program-address-only

The analysis state key introduced by this ADR is `(program address, bounded
call context)` -- but this key exists ONLY inside the bounded finite-register
proof pass (`M68kStaticGraphWalker::analyze_finite_register_values` /
`m68k_prove_stitched_an_indirect_targets`), which is pure static analysis
producing typed facts (`M68kIndirectTargetEaSet`, canonical edges). It never
reaches code generation. Every emitted C function/block is keyed by program
address alone, exactly as before this ADR; no context-duplicated emitted
block is ever produced, because the context dimension is fully consumed
(unioned away) at the Tier-1 fact-production boundary (Decision 5), before
any edge, block-entry, or emission decision is made downstream.

### 7. Fail-closed behavior on context-budget exhaustion

A (call site, seed point) unit that is register- or reachability-IRRELEVANT
to every still-unresolved site (Decision 3 steps 3-4), an eligible unit
beyond the `kM68kMaxTrackedCallContexts` bound among the RELEVANT set
(Decision 3 step 5), a call site whose contributed state is unknown, or an
indirect call site is simply never separately tracked; its contribution to
any shared site is exactly the existing (unchanged) base context-insensitive
result. If that means a site with more distinguishable RELEVANT finite units
than the bound, PLUS a genuinely unknown context, ends up missing one or more
of the excess units' members from the final union (because the base context
itself is `unknown` there, poisoned by the genuinely unknown context, and the
excess unit was not separately re-walked to rescue it), the site fails closed
for exactly those excess units' members -- never a fabricated target, never a
weakened cap, never a promoted unknown context.

### 8. One-shot / determinism argument

Every additional re-walk this ADR introduces is a call to the SAME,
unmodified, already-proven-deterministic `analyze_finite_register_values`
(T188's own order-independence argument, ADR-0029 Decision 9, applies
verbatim to each individual re-walk). Each bounded forward reachability
search (Decision 3 step 3, run independently per seed point) is likewise a
pure function of the canonical adjacency and a fixed (start, target) address
pair -- no traversal-order dependence, same visited-set discipline as the
base walk's own worklist. The SET of tracked units at a given proof pass is a
pure function of: (a) the base walk's own deterministic output
(`out_states_by_address`, itself order-independent per the existing
argument), (b) the canonical, deterministically sorted-and-deduplicated
`m68k_direct_call_sites` map derived from `canonical_edges`, and (c) the
deterministic register- and seed-point-aware reachability relation computed
from (a) and (b) over the canonical adjacency. Selecting the
`kM68kMaxTrackedCallContexts` smallest-(call-site-address,
continuation-before-callee-entry) RELEVANT units is a canonicalization over
that same deterministic set, not over any traversal order. The whole proof
pass therefore remains a pure, deterministic function
of the complete post-stitch graph, requiring no additional generation
attempt, compile attempt, checkpoint, or runtime-confirmed seed -- it is
computed entirely within the existing single static-analysis phase of the
canonical one-shot route (`generation_attempts = 1`, `compile_attempts = 1`,
`runtime_confirmed_seed_count = 0`).

## Consequences

- A genuine finite-plus-unknown join at a shared computed `JMP`/`JSR (An)`
  site whose finite caller reaches it through a SINGLE identifiable
  direct-call boundary (depth 1) can now retain that caller's independently
  proven finite target set, instead of collapsing to `unknown` merely because
  a second, genuinely incompatible authoritative caller also reaches the same
  site -- whether that caller's finite fact reaches the site by its own
  post-return reconvergence (continuation-seeded) or by a direct call
  straight into the shared callee's own entry (callee-entry-seeded, round 3).
- A join that requires distinguishing more than one nested call boundary, or
  more than `kM68kMaxTrackedCallContexts` distinct REACHABILITY-RELEVANT
  finite call-site contexts co-existing with a genuinely unknown one, remains
  `unknown` -- the SAME fail-closed outcome as every pre-T189 release for
  those (undistinguished) shapes. This is a deliberately bounded mechanism,
  not a general context-sensitive analysis.
- No decoder, lifter, code generator, emitted-block identity, Tier-1/Tier-2
  dispatch representation, candidate-root policy, canonical edge authority,
  256-member cap, A7/SP exclusion, or ADR-0009/ADR-0029 merge-semantics
  change was made or is required.
- If a future case requires depth > 1 (a join whose distinguishing ambiguity
  spans more than one nested call boundary), that is a SEPARATE, NOT
  preselected future decision -- exactly the same posture ADR-0029 took
  toward this ADR's own decision.
