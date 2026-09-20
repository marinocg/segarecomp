# ADR 0007: Generated-Runtime Loop-Progress Watchdog

- Status: Accepted
- Date: 2026-08-29
- Amends: the finite-dispatch termination rule established by ADR 0002 ("Static Translation and
  Fail-Closed Execution") and physically owned by `runtime/genesis/runtime.c`
  (`genesis_runtime_drive`), as described in
  `docs/architecture/seg-014-t005-generated-runtime-and-codegen-seam.md`. Does not amend, and does
  not relitigate, ADR 0002's static-dispatch boundary (no target-byte fetch, no decoder, no
  interpreter, no JIT, no second dispatcher), the wire report schema, the stop/diagnostic enums, or
  `STOP_DIAGNOSTIC_PAIRS`.

## Context

Before this ADR, `genesis_runtime_drive`'s third argument was a fixed **total** dispatch-block
execution count (`UINT32_C(128)`, emitted by `src/codegen/c11/genesis.cpp`
`emit_genesis_bridge_c11_main_finish`). SEG-007-T104 widened it once (64 -> 128); SEG-007-T106
reassessed every permitted higher candidate through the established `4096` ceiling and correctly
retained 128 rather than adopt an unbounded increase.

Authorized ephemeral diagnosis then established that the canonical generated-native route stops at
`instruction_budget_exhausted` while the runtime is inside a **productive finite self-loop block**: a
single generated block whose body performs a routed LONG memory write and whose terminal instruction
is a `DBF Dn,<self>` (`M68kIrKind::dbcc_loop`, condition F). Its low-word counter decrements
monotonically toward `0x0000 -> 0xFFFF` expiry and takes the back edge until then. The fixed total
cutoff trips mid-loop even though the loop is provably finite and provably making progress toward its
own termination on every taken back edge.

A larger fixed total is not the right fix: it is still an arbitrary number, it still trips a
legitimately finite loop whose trip count happens to exceed it, and (per SEG-007-T106) every
permitted larger value through the ceiling still exhausted. What the loop actually provides is a
**CPU-semantics-owned monotonic finite-progress proof** that the generic driver was throwing away.

## Decision

`genesis_runtime_drive` keeps its signature and its
`runtime == 0 || dispatch == 0 || dispatch_budget == 0U` guard (returning
`genesis_internal_dispatch_inconsistency_stop`). Its third argument is re-interpreted, comment-only,
from a total cutoff to a deterministic **no-progress window** `W`; the emitted literal is unchanged
(`UINT32_C(128)`, appearing exactly once) and is explicitly **not** retuned.

### Runtime ABI additions (`runtime/genesis/runtime.h`)

- `GenesisLoopProgressNote { uint8_t present; uint32_t loop_id; uint32_t remaining_bound; }`, added as
  the trailing `GenesisRuntime.loop_progress` field. Zero-valued (`present == 0`) by every existing
  `GenesisRuntime runtime = {0};` construction, so a program that never proves a finite-loop back
  edge is provably unaffected.
- `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` (`UINT32_C(0x00200000)`, i.e. 32 full MC68000 16-bit DBF
  counter ranges): a bounded **project-engineering ceiling** on cumulative proven finite-loop
  iteration credit for the whole generated run. It is not console timing, not a hardware fact, and
  not "the dispatch budget"; its only job is to guarantee the watchdog still terminates a program
  that re-enters proven loops without bound.
- `void genesis_note_loop_backedge(GenesisRuntime *runtime, uint32_t loop_id, uint32_t remaining_bound);`
  stores `{present = 1, loop_id, remaining_bound}` into `runtime->loop_progress` and has no other
  effect. NULL-safe.

### Watchdog semantics (`genesis_runtime_drive`)

Local, fully deterministic state (no time, no uninitialised reads): `steps_since_progress = 0`,
`progress_credit = GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` (**never replenished**), and exactly **one**
active-instance slot (`slot_active`, `slot_loop_id`, `slot_low_water`).

Per unbounded dispatch step: clear `loop_progress.present`; dispatch; return non-`CONTINUE` results
verbatim; advance `pc`. Then, if the block set `present`:

- if the note's `loop_id` matches the active slot: a **strict** decrease of `remaining_bound` below
  `slot_low_water` lowers the low-water mark and, if `progress_credit != 0`, consumes one credit unit
  and counts as progress; `remaining_bound >= slot_low_water` (counter modified, re-initialised, or
  re-entered) is **not** progress;
- otherwise the slot is **replaced** with this loop instance, and the replacement step itself is not
  progress.

A step with `present == 0` is not progress and does **not** clear the slot (a multi-block DBF loop
body legitimately contains non-backedge steps). On a progress step `steps_since_progress` resets; on
a non-progress step it increments, and reaching `W` returns the fail-closed stop built from the last
dispatch result with `stop_class = GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED` and
`diagnostic_category = GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED` (the existing pair; the wire
taxonomy is unchanged).

### Codegen (`M68kMemoryEmissionContext::loop_progress_object`, DBcc lowering)

`src/codegen/c11/m68k.cpp`'s `M68kIrKind::dbcc_loop` case is restructured from a single ternary into
an explicit `if (expired) {...} else {...}` so the note is emitted **only on the taken
(non-expired) back edge**, immediately after the back-edge target is chosen. When
`M68kMemoryEmissionContext::loop_progress_object` is empty the emitted text is exactly equivalent to
before (verified against the DBcc static-slice and Musashi differential fixtures, whose behavior is
unchanged). The DBcc semantic order is intact: condition first; decrement only when the condition is
false; one shared `M68kDbccDecrementSpecification` decrement/expiry formula; SR untouched.

`loop_id` is the DBcc instruction's own source address (static identity). `remaining_bound` after the
decrement, when not expired, is exactly `(Dn & 0xFFFF) + 1`, which strictly decreases by 1 per taken
back edge within one instance -- the CPU-proven monotonic finite-progress fact.

Only the general-startup runtime block emitter for a `genesis_runtime_drive`-driven program sets
`loop_progress_object` (to `"runtime"`). Every other emitter path (direct-flow, structured
direct-flow, static-slice probes, moveq/branch scaffolds, synthetic-completion) leaves it empty and
is byte-for-byte unchanged.

## Why the proof is not renewable without bound

- **Same static loop re-entered from an infinite outer loop.** The slot keeps its `loop_id` and its
  `slot_low_water`; the fresh instance's first bound is `>= slot_low_water`, so it is never credited,
  `steps_since_progress` reaches `W`, and the run fails closed.
- **Two loops alternated by an infinite outer loop.** Slot replacement lets each re-credit, but every
  credited step decrements the global, never-replenished `progress_credit`; once it hits 0 no step is
  credited and the run fails closed.
- **Non-DBF loop / `BRA` self-loop / non-monotonic DBF body.** No note, or a `remaining_bound` that
  does not strictly decrease, so no credit is ever granted and the run fails closed at `W`.

## Consequences

- A generated finite DBF loop whose proven trip count exceeds the former cutoff now runs to
  completion through static emitted dispatch only, by consuming CPU/codegen-proven monotonic
  bounded-progress credit for its one static loop identity/instance. The generic runtime still does
  not decode DBF or any target opcode; it consumes only the emitted `(loop_id, remaining_bound)`
  note.
- A plain non-progress loop, an infinite outer loop that reinitialises and re-enters a finite inner
  DBF loop, and a DBF body that destroys the counter's monotonic progress all still stop
  deterministically and fail closed at the unchanged `instruction_budget_exhausted` /
  `instruction_budget_exhausted` pair.
- The wire report schema, stop/diagnostic enums, `STOP_DIAGNOSTIC_PAIRS`, routing, provenance, and
  privacy behavior are all unchanged. No new stop or diagnostic value is added.
- This ADR does not add generic loop analysis, inferred loop equivalence, runtime DBcc/DBF/opcode
  recognition, a second dispatcher, an interpreter, or a JIT. It supports only the existing
  CPU-owned DBcc/DBF decrement/taken-back-edge shape as the source of the emitted proof.
- A later task that wants a different loop family (a non-DBF back edge, a data-dependent bound, a
  nested-instance model) must emit its own CPU/codegen-proven monotonic note through the same
  `genesis_note_loop_backedge` seam and existing semantic owner, or record why that is unsound; it
  must not reintroduce a fixed total cutoff or a runtime opcode decoder.
