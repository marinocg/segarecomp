# ADR 0047: No Tier-1 Cross-Product Producer for Address-Register-Indexed Control Transfer

- Status: Accepted
- Date: 2026-09-26
- Task: SEG-021-T025
- Related, unchanged: ADR 0009 (computed indirect control-flow target resolution), ADR 0024/0025 (Tier-2
  emitted-set dispatch, offline whole-ROM analysis), ADR 0030 (bounded call-context-sensitive finite `An`
  analysis), ADR 0039 (monotonic representation / compiled-entry precedence).

## Context

`JMP (d8,An,Xn)` and `JSR (d8,An,Xn)` compute their target at run time from a base address register and an
index register (either bank, word or long). SEG-021-T011 routed this shape through the Tier-2 fallback only
(`process_indirect_control_index8`, `libs/cpu/m68k/src/static_discovery.cpp`): static discovery records the
site as a Tier-2 fact and C4 emits a runtime EA computation plus membership check against the compiled
`EmittedCodeAddressSet`. T011 deliberately did not build a Tier-1 finite-value producer for this shape, because
it needs a new cross-product domain: every finite base value (ADR 0030's `an[]` state) combined with every finite
index value (ADR 0009's index producer), proven to stay inside the existing 256-member candidate cap. SEG-021-T025
owns the decision whether to build it.

Since then, SEG-021-T034 extended the shared runtime-owned dynamic-indirect lowering to every register-relative
control EA, including `(d8,An,Xn)` with every index bank and size. On the no-hints immutable-ROM AOT route such a
site is now an independent AOT identity: its target is computed from architectural registers, checked against the
final compiled-entry authority, and the program fails closed (`unresolved_indirect_target`) otherwise. On the
hinted route the Tier-2 fallback does the same, and ADR 0039's compiled-entry precedence selects the AOT body when a
site is owned by both.

## Decision

1. **Do not build the Tier-1 cross-product producer.** `process_indirect_control_index8` keeps recording the
   Tier-2 fact and nothing else. No new static finite-value domain, no join of ADR 0030 and ADR 0009, no change to
   the 256-member cap, no discovery-budget change.
2. **The final state for this shape is explicitly justified runtime ownership.** Every `(d8,An,Xn)` control
   transfer executes natively through either the AOT runtime-owned lowering (SEG-021-T034) or the Tier-2 fallback
   (SEG-021-T011). Neither decodes an opcode at run time. A target outside the compiled set fails closed.
3. **The coverage report states the reason.** The `route_static_discovery` gap for the `(d8,An,Xn)` JMP/JSR
   forms is reported as a justified restriction citing this ADR, not as unexplained missing support.

## Evidence

- **What a Tier-1 producer would add.** Only a static candidate list for the site: CFG edges to finite targets, so
  that static discovery could root those targets. It would not add execution capability, because execution is
  already native (Decision 2). Targets reachable only through such a site are still represented, because
  immutable-ROM AOT enumerates every legal identity in the immutable image regardless of CFG reachability.
- **Real-site data (aggregate counts only, from local authorized images, not persisted beyond these numbers).**
  Whole-image AOT identities with a `(d8,An,Xn)` control EA: 6 and 4 in two authorized local commercial images. These
  are whole-image candidates, not executed sites. On both images the no-hints route runs to steady-state frame
  execution (VBlank IRQ6 delivered, title screen and attract demo rendered on the first image; SEG-021-T031) with no
  `unresolved_indirect_target` stop, so no executed site of this shape currently leaves the compiled set.
- **Cost of the producer.** A cross-product proof has to track joint base and index values per call context,
  prove every combination stays under the 256-member cap, revalidate across expansion rounds (ADR 0013), and add
  differential vectors for the joined domain. That is a new analysis domain with its own soundness obligations,
  in exchange for no new executed capability.
- **Reversibility.** If a future executed frontier needs static candidates for this shape (for example, a
  target reachable only through `(d8,An,Xn)` that immutable-ROM AOT cannot represent), a later task may revisit
  this with that executed evidence. Runtime-selected evidence, not static lookahead, is the trigger.

## Consequences

- `route_static_discovery` stays at 0 for the `(d8,An,Xn)` JMP/JSR forms, now annotated with this reason.
- No production code changes. A test pins the A7-as-base case for this shape on the runtime-owned route (the
  corner SEG-021-T011's validator flagged as untested).
