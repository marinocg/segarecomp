# ADR 0048: Push-Then-RTS Computed Jump Admission in Immutable-ROM AOT

- Status: Accepted
- Date: 2026-09-28
- Task: SEG-021-T041
- Related, unchanged: ADR 0009 (computed indirect control-flow), ADR 0011 (whole-program JSR-continuation set for
  RTS), ADR 0039 (compiled-entry precedence), ADR 0047 (runtime-owned `(d8,An,Xn)` control transfer).

## Context

A common 68000 idiom is a computed jump through the stack: `MOVE.L <src>,-(A7)` immediately followed by `RTS`.
The `RTS` pops exactly the long just pushed, so its target is a computed jump target, not a return address.
ADR 0011's RTS check validates the popped value against the whole-program set of JSR/BSR continuations, which by
construction never contains such a target, so the generated program stopped with `return_target_mismatch` on a
commercial title's table dispatch (runtime-selected evidence, sanitized to this shape).

## Decision

1. In the immutable-ROM AOT route, an admitted `RTS` identity R is a *computed-jump RTS* when it is reached from an
   admitted `MOVE.L <any EA>,-(A7)` identity by a bounded (8 instructions) forward walk over admitted identities
   that neither touch the stack nor write A7 (complete register-write footprint without A7, no stack effect,
   sequential PC or direct branch). Both arms of conditional branches are followed and the walk abandons any path at
   the first instruction outside that class. The immediately-adjacent `MOVE.L ...,-(A7); RTS` is the degenerate
   case; the runtime-selected evidence also shows push, a short stack-neutral diamond, then RTS. All facts come
   from the immutable image (both identities decode from unmodifiable ROM), not from inference about run-time state.
2. Nothing is fused. Every identity keeps their own bodies, timing, interrupt-boundary and exact-PC behavior,
   so SP, flags, memory writes and timing are exactly those of the two instructions executed separately. The push
   still writes the long to the stack slot; the RTS still performs the routed stack read (alignment, range and bus
   guards unchanged) and `A7 += 4`.
3. The only change: a computed-jump RTS validates the popped value against the final compiled-entry lookup
   (`genesis_compiled_entry_lookup`, the same authority ADR 0047's runtime-owned `JMP (d8,An,Xn)` uses) instead
   of the JSR-continuation set. A value that is not a compiled identity fails closed with the existing
   `return_target_mismatch` stop and the RTS uncommitted. No run-time opcode decode is introduced.
4. Every other RTS (no push reaching it through the window, including a push followed by an A7-writing
   instruction) keeps ADR 0011's
   continuation-set authority unchanged.

## Soundness

- Transferring to any compiled identity is exactly what a `JMP` to that address does; the compiled-entry lookup
  contains only generated identities, so dispatch is architecturally exact and unknown targets fail closed.
- The window walk is not needed for architectural correctness of the transfer (the RTS still reads the real stack
  slot, so an intervening memory write to the slot is honored); it only scopes the weaker (compiled-membership)
  authority to the idiom where the popped value is a computed target. If some other path (a branch target, or an
  indirect predecessor) enters R with a real return address on the stack, that address is a compiled continuation
  and is also accepted, which is still architecturally exact. This is why no whole-program predecessor analysis is
  required (indirect predecessors are unknowable statically) and why the check fails closed rather than open.
- Because nothing is merged, an interrupt taken between them behaves as on hardware.
- Adversarial review (SEG-021-T041, shared join point): the marking is existential per RTS identity (some push
  candidate reaches it through the bounded, stack-neutral window), so an RTS that is a join of a push-arm (branch or
  diamond) and an arm entered by an ordinary `JSR`/`BSR` return is marked as a computed-jump RTS. This admits no
  transfer that hardware would not perform: the RTS still pops the real stack slot and the popped value must be a
  compiled identity; a genuine return address is a compiled continuation and is accepted exactly as before, while a
  corrupted return value that happens to be another compiled identity is now followed instead of stopping with
  `return_target_mismatch` (a loss of that RTS's strictness, never of architectural exactness). Any instruction with a
  stack effect (`JSR`/`BSR`/`PEA`/`LINK`/A7 writes) terminates the walk, so an intervening call never extends the
  window. No whole-program predecessor analysis is added, and unknown targets still fail closed.

## Consequences

- No finite producer is built; ADR 0047's decision not to build a Tier-1 cross-product producer stands.
- Tests: `genesis_push_then_rts_computed_jump_generated_test` (positive dispatch with SP and stack-slot checks,
  non-compiled value fails closed, stack-altering instruction between push and RTS and plain RTS unchanged, diamond window on both arms, ordinary JSR/RTS unchanged,
  deterministic output, strict C11).

## Amendment (SEG-068-T005): the window crosses block-route-owned identities

First failed condition of a commercial H-interrupt handler (`MOVE.L <RAM>,-(A7)`, a conditional branch, `RTS`):
the push and the branch are owned by the block route (the additive H-interrupt root of SEG-055 emits its body
there) while the `RTS` is an AOT-only identity (the discovery cut the root's body at the call-frame-less RTS). The
bounded walk of Decision 1 started only from AOT-entry pushes and abandoned a path at the first identity owned by the
block route, so the AOT-only RTS was never marked and its popped value was validated against the JSR-continuation set
(`return_target_mismatch`) although it is a member of the compiled-entry set.

Amended rule: the push seed and the stack-neutral identities of the window may be owned by either the block route or the
AOT map (both decode from the same immutable image; the operation identity of an ordinary/AOT collision is already
required to agree). The marked `RTS` must still be **AOT-only** (not an ordinary compiled owner), exactly as ADR 0121
section 5 requires: a statically reached RTS keeps its static return-frame authority and is never re-authorized by this
rule. Nothing else changes: the window size (8 identities), the A7/stack-effect exclusions, the compiled-entry
membership check, the uncommitted RTS on mismatch and the soundness argument above all apply unchanged. Covered by
`genesis_hint_root_push_then_rts_generated_test` (member dispatches with SP net unchanged; non-member stops
`return_target_mismatch` with the RTS uncommitted; the zero arm never reaches the RTS; deterministic) in addition to the
unchanged `genesis_push_then_rts_computed_jump_generated_test`.
