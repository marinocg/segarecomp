# ADR-0035: Per-invocation finite-DBF-loop completion and progress-slot re-arm

- Status: Accepted
- Date: 2026-09-12
- Amends: ADR-0007 (`docs/decisions/0007-generated-runtime-loop-progress-watchdog.md`), specifically
  its "Why the proof is not renewable without bound" section's "Same static loop re-entered from an
  infinite outer loop" case and its "Consequences" section's explicit "a later task" pointer. Does not
  amend, and does not relitigate, ADR-0002's static-dispatch boundary (no target-byte fetch, no
  decoder, no interpreter, no JIT, no second dispatcher), the wire report schema, the stop/diagnostic
  enums, `STOP_DIAGNOSTIC_PAIRS`, or ADR-0016's static finite-loop proof contract
  (`m68k_prove_finite_loop_progress`) beyond what is stated below.
- Related: SEG-007-T210 (diagnosed the frontier this ADR resolves; recorded
  `needs_full_refinement`), SEG-007-T185 (established the diagnostic precedent that this stop class
  can be either a preservation regression or a genuine architecture limit), the successor
  implementation task this ADR authorizes.

## Context

SEG-007-T210 diagnosed a generated-native `instruction_budget_exhausted` stop at a normalized
`finite_dbf_nemesis_code_table_loop` shape: a shared, per-entry, code-table-fill subroutine containing
a single `DBF Dn,<self>` back edge, reached repeatedly from an outer per-entry driver. Debugger
inspection confirmed, for the exact reached instance:

- the loop had genuinely, provably completed (the reached PC was the loop's own natural
  fall-through/exit branch, not a mid-loop address);
- `genesis_note_loop_backedge` was emitted correctly and currently on every taken back edge for that
  instance (not a T185-style lost/incorrectly-scoped annotation);
- the ADR-0007 watchdog's single active-instance slot was pinned on that loop's static identity with
  `slot_low_water` at the algebraic floor `1` (the minimum possible value of `remaining_bound =
  (Dn & 0xFFFF) + 1`);
- the global `progress_credit` pool was nowhere near exhausted (~15% consumed).

ADR-0007's own text anticipates exactly this outcome: because the slot never resets except by strict
decrease or replacement by a *different* loop identity, once one invocation of a given static loop
reaches the floor, no later invocation of the *same* static loop -- however genuinely, individually
finite -- can ever again present a strictly smaller bound. The loop is correctly, repeatedly finite;
the proof contract simply cannot express "this invocation finished, so a new invocation is legitimate
and separately provable." ADR-0007 explicitly calls this a case for "a later task" to add "its own
CPU/codegen-proven monotonic note through the same `genesis_note_loop_backedge` seam ... for a
nested-instance model, or record why that is unsound."

This ADR is that later task's architecture decision.

## Decision

Adopt **explicit CPU/codegen-proven finite DBF invocation completion, with per-invocation
progress-slot re-arm bounded by the existing, never-replenished global credit ceiling** (alternative
(A) below). The runtime does not gain a nested/multi-instance model, a wider static-loop-proof
comparison family, or any renewal of the global credit pool. It gains exactly one new fact: "this
loop instance is now provably over," which lets the *next* invocation of the same static loop start
its own low-water mark from a fresh, unconstrained floor rather than the retired instance's.

### 1. Why static loop identity sufficed for one invocation but fails for reusable finite routines

ADR-0007's slot models a **single active invocation** of a loop identity: it is enough to prove one
finite run of the loop makes monotonic progress toward `0`. It was never designed to distinguish "the
same static loop code re-entered for a brand-new, separately finite invocation" from "the same
invocation still running with its counter reinitialized or widened mid-flight" -- both look identical
from the watchdog's point of view (a `remaining_bound` that is not strictly less than the recorded
low-water mark). A reusable finite subroutine invoked repeatedly by an outer driver is exactly the
first case, but the existing contract can only ever see the second, more dangerous one, and correctly
refuses to credit it. Solving this requires a genuinely new fact channel -- proof that the *previous*
invocation is over -- not a change to how bounds are compared within one still-open invocation.

### 2. What constitutes the start of a new finite-loop invocation

A new invocation begins at the same static site (`loop_id`, the DBcc instruction's own source
address) as an existing but now-completed instance. No new `loop_id` allocation or per-call-site
instance counter is introduced: `loop_id` remains the loop's static source address, exactly as
ADR-0007 defines it. The only new signal is *whether the previously active instance for that
`loop_id` is closed*; while it is open, a fresh entry into the same static loop is not a "new
invocation" in the sense that matters here -- it is governed by rule 8 below.

### 3. What constitutes proven completion

Completion is a CPU/codegen-owned static fact, symmetric with how progress itself is proven: the
DBcc lowering in `src/codegen/c11/m68k.cpp` already distinguishes the **expired** branch (condition
true, or the decrement wraps `0x0000 -> 0xFFFF`) from the **taken-back-edge** branch in its `if
(expired) {...} else {...}` structure (ADR-0007). Completion is proven exactly when control reaches
the expired branch for a `loop_id` that emitted at least one backedge note (i.e., the branch the
current code already takes when a loop naturally finishes). This is a fact already fully determined
by existing DBcc semantics; no new CPU analysis, no opcode/byte inspection, and no runtime decode are
introduced. The runtime only consumes a new typed emission from the same, already-existing lowering
site.

### 4. Whether completion itself counts as progress

No. Completion is a **closing** event for one invocation's slot occupancy, not a `remaining_bound`
decrease. It must not consume `progress_credit` and must not, by itself, reset
`steps_since_progress`. Treating completion as free progress would let an adversarial or malformed
program manufacture unlimited no-cost "progress" merely by entering and immediately expiring a DBF
loop in a tight outer loop, defeating the watchdog. Completion only re-arms the slot so that a
**separately, strictly-decreasing** subsequent invocation can be credited under the existing rules --
credit is still earned exactly the same way as before (rule 12 makes the boundedness argument
precise).

### 5. Whether completion resets `steps_since_progress`

No, for the same reason as point 4. The single dispatch step on which completion is observed is
still evaluated under the ordinary rule: it resets `steps_since_progress` only if that step's own
note is a genuine, distinct progress-decrease under the (now possibly freshly re-armed) slot state,
never merely because completion occurred. A run that repeatedly opens and completes a loop without
ever making a credited decreasing step still exhausts `W` and fails closed, exactly as ADR-0007
intends for any other non-progress sequence.

### 6. When the slot is cleared/re-armed

The slot clears/re-arms in the same dispatch step that proves completion for the `loop_id` currently
occupying it: `slot_active` is set back to false (or equivalently marked "closed for this loop_id"),
so the **next** note carrying that same `loop_id` is treated as a fresh instance with an
unconstrained low-water mark (rule 2), rather than being compared against the retired instance's
floor. If the completion note's `loop_id` does not match the currently active slot's `loop_id`, the
slot is left untouched (rule 7).

### 7. How mismatched completion notes fail closed

A completion note whose `loop_id` does not match `slot_loop_id`, or that arrives while
`slot_active` is false, has no effect: it does not clear any slot, does not manufacture progress, and
does not reset `steps_since_progress`. This mirrors ADR-0007's own existing non-progress-step rule
("a step with `present == 0` ... does not clear the slot") and prevents a stale, reordered, or
unrelated completion signal from ever laundering credit for a different loop instance.

### 8. What happens if a loop exits through another control-flow edge instead of normal DBF expiry

Only the DBcc lowering's own expired branch (rule 3) proves completion. If control leaves the loop
body through any other edge (an early exit, a bounds check, a computed jump, or any construct outside
the shared `M68kIrKind::dbcc_loop` shape), no completion note is emitted, the slot is left exactly as
it was (still occupied and pinned at its current low-water mark, per existing ADR-0007 behavior), and
a subsequent re-entry into the same static loop is judged under the unchanged, stricter existing rule
(no credit unless it beats the still-recorded low-water mark). This is intentionally conservative:
this ADR only recognizes the one CPU-provable completion shape DBcc lowering already fully determines,
and never infers completion from any other control-flow shape.

### 9. Interaction with interrupts/exceptions

`genesis_runtime_drive` already models IRQ6 scheduling/admission (SEG-007-T047 / ADR-0020 §5) and RTE
handling; ADR-0035 does not introduce new interrupt architecture and must be read against the actual
current per-dispatch-step ordering:

```text
dispatch
-> loop-progress processing (ADR-0007, this ADR's completion check included)
-> data-progress processing (ADR-0017)
-> IRQ6 scheduler/admission (ADR-0020 §5)
-> shared-credit accounting for a successfully admitted interrupt
-> watchdog (steps_since_progress / W check)
```

The required contract:

- a DBF completion note produced by the just-dispatched generated block is processed (rules 6/7,
  closing/re-arming the slot for `loop_id` if it matches) strictly before IRQ6 scheduler/admission runs
  for that same dispatch step;
- IRQ admission does not itself close, re-arm, or otherwise touch a DBF loop instance's slot; the two
  mechanisms are evaluated independently in the same step, exactly as loop-progress and data-progress
  already are;
- if an IRQ preempts execution while a DBF invocation is still open (the DBcc lowering's `expired`
  branch has not executed for that `loop_id`), no completion note is emitted and none is inferred from
  the interrupt itself -- the slot is left exactly as rule 8 already specifies for any non-expiry exit;
- `RTE` does not manufacture, infer, or trigger loop completion; it is not a source of the completion
  signal, which is proven only by the DBcc lowering's own expired branch (rule 3);
- once control resumes inside a handler (or back in the interrupted stream after `RTE`), the existing
  ADR-0007 slot semantics apply unchanged: a handler's own DBF loop may replace the active static-loop
  slot exactly as ADR-0007's existing "otherwise the slot is replaced" rule already allows, with no
  ADR-0035-specific interrupt handling;
- ADR-0035 introduces no interrupt-specific instance tracking, no per-handler slot, and no change to
  IRQ6 admission's own shared-credit accounting.

### 10. Interaction with data-progress notes

ADR-0017 data-progress is already implemented and already shares the single, never-replenished global
`progress_credit` pool with loop-progress: it has its own independent `data_slot_active` /
`data_slot_proof_id` / `data_slot_low_water` state, evaluated after loop-progress in the same dispatch
step, under the same low-water strict-decrease rule. The current runtime already enforces "at most one
shared credit unit consumed per dispatch step" across loop-progress, data-progress, and a successfully
admitted IRQ (a `credit_consumed` flag for the step, checked before each family's own credit
deduction). ADR-0035 does not add a second note channel, does not change `data_slot_*` semantics, and
does not change the at-most-one-unit-per-step accounting. Explicitly:

> DBF completion itself consumes no credit and does not reset `steps_since_progress`; it also must not
> prevent a valid loop decrease, data-progress decrease, or IRQ admission on the same dispatch step from
> receiving the existing shared progress treatment.

Because completion is evaluated as part of the loop-progress family's own processing (rules 6/7) and
never itself sets `made_progress`/consumes `credit_consumed` (rule 4), it cannot suppress or interfere
with a same-step data-progress or IRQ-admission credit event; those families' existing
`credit_consumed`-aware accounting is preserved unchanged, unless T211's own implementation evidence
demonstrates a minimal, narrowly-scoped ordering adjustment is required, in which case that adjustment
must be recorded and justified in T211's own Evidence rather than assumed here.

### 11. Interaction with the shared global progress-credit pool

Unchanged and unaffected in the sense that matters most: `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`
remains a fixed constant, initialized once, and consumed only by genuine strict-decrease progress
steps (loop-progress, data-progress, or a successfully admitted IRQ; see rule 10) exactly as the
current runtime already defines, at most one shared unit per dispatch step. Completion never adds to,
resets, or replenishes `progress_credit`; it only changes which *future* strict-decrease steps are
eligible to consume existing, already-allocated credit. Two loops alternated by an infinite outer loop
still each consume the same shared, non-replenished pool exactly as ADR-0007's existing "Two loops
alternated" case describes; this ADR does not change that case at all.

### 12. Proof that infinite repeated finite-loop invocation still terminates

This is the critical boundedness argument, and it is the reason completion must never manufacture or
replenish credit (rules 4, 5, 11): **per-invocation slot renewal only changes which steps are
*eligible* to be judged as progress; it never changes how many total progress-credit units the whole
run may ever consume.** `progress_credit` starts at the fixed `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`
and is monotonically, irreversibly decremented by credited steps (loop-progress, data-progress, or IRQ
admission; rule 10) and by nothing else, for the entire process lifetime. Consider an infinite outer
loop that repeatedly invokes the same finite DBF helper to completion: each invocation may now be
credited (up to its own bound's number of strictly decreasing steps) because re-arming lets its
low-water mark start unconstrained, but every credited step it takes still consumes one unit from the
same fixed, whole-run pool. `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` remains the fixed, never-replenished
whole-run bound on cumulative credited progress. Together with the unchanged no-progress window `W`,
it guarantees deterministic termination: once credit reaches zero, no further credited event -- however
freshly re-armed the invocation's slot -- can reset `steps_since_progress` (the existing
`if (progress_credit != 0)` guard in `genesis_runtime_drive` is unchanged), so `W` eventually causes the
existing fail-closed stop. Completion is therefore a **within-budget re-arm**, never a budget increase:
it lets the watchdog correctly recognize genuinely separate, individually finite work, but the total
amount of work the whole run may ever be credited for remains bounded by the same fixed ceiling and the
same fixed window ADR-0007 already established. This is the formal statement of the conservative rule
adopted below.

**Conservative rule.** *Completion permits future re-establishment of the loop instance but does not
itself manufacture or replenish global progress credit.*

### Scope limitation: sequential re-invocation only, not simultaneous nesting/reentrancy

The completion note carries only the static `loop_id` (the DBcc instruction's own source address), the
same identity ADR-0007 already uses. It therefore does **not** establish a unique *dynamic invocation*
identity, and cannot distinguish which open invocation of the same static site a given completion event
closes when more than one invocation of that exact static `loop_id` is simultaneously open (for
example, a reentrant or recursive call path where an outer invocation of loop `X` is still open when an
inner invocation of the same static loop `X` is entered and completes). A completion event carrying
only `loop_id = X` cannot tell that inner-invocation completion apart from the still-open outer
invocation in that shape.

ADR-0035 is therefore explicitly scoped to **sequential re-invocation** of a reused finite DBF site --
one invocation of a given static loop fully completes (its expired branch executes) before the next
invocation of that same site begins -- which is exactly the diagnosed T210 shape
(`finite_dbf_nemesis_code_table_loop`, an outer per-entry driver calling one shared finite subroutine
one invocation at a time). More precisely, CPU/codegen proves natural completion of the currently
observed sequential invocation at this static DBF site under the ADR-0035 supported shape; it does not
claim general dynamic-instance identity for simultaneous nested/reentrant executions of the same static
loop site. That broader shape remains outside this decision and, if it is ever actually diagnosed at a
future runtime frontier, must be diagnosed and refined separately (very likely requiring alternative
(C)'s deferred nested/multi-instance model) rather than assumed solved by this ADR.

**No dynamic slot-ownership guarantee for unsupported nesting.** For simultaneous nested/reentrant
executions of the same static `loop_id`, ADR-0035 defines no semantic correctness guarantee for dynamic
slot ownership. Because completion carries only the static loop identity, a completion emitted by one
dynamic invocation may match and re-arm the shared slot while another invocation of the same static site
remains active (concretely: outer invocation of loop `X` open -> reentrant inner invocation of the same
static loop `X` -> inner `X` reaches DBF expiry and emits `completion(loop_id = X)` -> this matches and
re-arms the single shared slot even though the outer invocation is still logically open -> the outer
invocation's subsequent backedge note may then establish a fresh low-water mark and later receive
progress credit again). This is not "no credit for the unsupported shape" -- it is an unspecified,
potentially incorrect dynamic-ownership outcome. This shape is outside ADR-0035's supported
sequential-reinvocation contract and must be separately diagnosed/refined if encountered.

**Boundedness is preserved regardless.** This unsupported shape does not compromise whole-run
termination (rule 12). A completion event does not consume, create, reset, or replenish
`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`. Any progress that becomes creditable after such a re-arm still
consumes the same finite, never-replenished global pool. Once that pool reaches zero, no further
credited event can reset `steps_since_progress`, and the unchanged no-progress window `W` eventually
causes the existing fail-closed stop.

## Alternatives considered

- **(A) Explicit finite-loop completion/re-arm (adopted).** Minimal, symmetric with the existing
  progress-note seam, is bounded by the unchanged global-credit ceiling and no-progress window `W`
  together (rule 12), and directly answers ADR-0007's own named case. Chosen.
- **(B) Auto-renew the same-loop slot whenever `remaining_bound` increases or is reinitialized.**
  Rejected. This cannot distinguish a legitimate new invocation (fresh, provably bounded counter) from
  a malicious or non-monotonic mid-invocation reset, or from an infinite outer loop that merely
  re-executes the loop's own setup code without the loop having actually finished. It would credit
  exactly the unsafe case ADR-0007's watchdog exists to reject.
- **(C) A general nested/multi-instance runtime progress model (multiple simultaneous slots, call-
  stack-aware instance identity, etc.).** Rejected as unnecessarily broad for the diagnosed frontier:
  the observed shape is sequential re-invocation of one loop by one outer driver, not concurrently
  nested loop instances. Deferred until a diagnosed frontier actually requires simultaneous multi-slot
  tracking (see the sequential-only scope limitation above); alternative (A) is demonstrably sufficient
  for the diagnosed case.
- **(D) Expand `m68k_prove_finite_loop_progress` (ADR-0016) to additionally prove the *outer* re-entry
  driver's own finiteness, and derive credit from that combined proof.** Deferred. This is a strictly
  larger static-analysis undertaking (widening the admitted comparison family beyond long-word
  register-vs-register `CMP`/`CMPA`) for a fact the runtime-side completion signal already supplies
  more directly and more generally, without constraining which outer-driver shapes are legitimate.
  Revisit only if (A) is shown insufficient for a future diagnosed frontier.
- **(E) Raise the dispatch no-progress window `W`.** Rejected, per ADR-0007's own established
  precedent (SEG-007-T104/T106): this only postpones the exact same structural trip and does not fix
  the underlying non-renewability; every permitted larger value still eventually exhausts.
- **(F) Replenish or raise the global credit ceiling `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`.**
  Rejected. This directly weakens the termination proof in point 12: the never-replenished ceiling,
  together with the unchanged no-progress window `W`, is what guarantees that an infinite outer loop
  invoking a finite helper without bound still fails closed. Any replenishment (periodic,
  per-completion, or otherwise) would make an unbounded program run forever, which ADR-0007 exists
  specifically to prevent.

## Codegen/runtime representation

The cleanest minimal representation is a dedicated typed completion note through the same seam
ADR-0007 already established, rather than overloading the existing progress note's fields:

- `runtime/genesis/runtime.h` gains `void genesis_note_loop_completion(GenesisRuntime *runtime,
  uint32_t loop_id);`, NULL-safe, symmetric with `genesis_note_loop_backedge`. It records the
  completion fact for the current dispatch step (e.g. a small `completion_loop_id` /
  `completion_present` pair on `GenesisRuntime`, mirroring the existing `loop_progress` fields'
  shape), cleared at the start of every dispatch step exactly as `loop_progress.present` already is.
  A separate call (rather than a new field on the existing progress note) keeps the "this step made
  progress" and "this step closed an invocation" facts independently representable, since a single
  dispatch step is never both (rule 4): the expired branch never also reports a decreasing
  `remaining_bound`.
- `src/codegen/c11/m68k.cpp`'s existing `M68kIrKind::dbcc_loop` `if (expired) {...} else {...}`
  structure (ADR-0007) emits the new call in its already-existing `expired` branch, guarded by the
  same `M68kMemoryEmissionContext::loop_progress_object` non-empty check that already gates the
  existing backedge note, so every other emitter path (direct-flow, static-slice probes, synthetic-
  completion, etc.) remains byte-for-byte unchanged.
- `genesis_runtime_drive` applies rules 6/7 after dispatch, before the existing progress-step
  evaluation: if a completion note is present and its `loop_id` matches the active slot, close the
  slot (`slot_active = false`); otherwise (mismatch, or no active slot) the completion note has no
  effect. This is evaluated independently of, and does not substitute for, the existing
  progress-note handling in the same step.

## Consequences

- A reusable finite DBF subroutine invoked *sequentially* by an outer driver can now be credited on
  each of its separately-finite invocations, subject to the same never-replenished global credit
  ceiling that already bounds every other progress path.
- `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` remains the fixed, never-replenished whole-run bound on
  cumulative credited progress, and the no-progress window `W` is unchanged; together they remain the
  unweakened deterministic-termination guarantee for any unbounded outer re-entry (rule 12). No path in
  this ADR increases, resets, or replenishes the credit ceiling or widens `W`.
- No opcode/byte-level runtime decoding, second dispatcher, interpreter, JIT, or Sonic-specific
  heuristic is introduced. The runtime still consumes only emitted, CPU/codegen-proven typed facts
  through the existing note seam.
- ADR-0016's admitted static-loop-proof comparison family is unchanged; this ADR does not widen it.
- ADR-0035 is scoped to sequential re-invocation of a reused static DBF site only; it establishes no
  dynamic-instance identity and makes no claim for simultaneous nested/reentrant invocations of the
  same static site (see the sequential-only scope limitation above). A future genuinely
  concurrent/nested multi-instance requirement (alternative (C)) remains deliberately deferred and is
  not implied or authorized by this decision.
