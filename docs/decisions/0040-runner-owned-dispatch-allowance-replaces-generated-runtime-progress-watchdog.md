# ADR-0040: Runner-Owned Dispatch Allowance Replaces the Generated-Runtime Progress Watchdog

- Status: Accepted
- Date: 2026-09-18
- Supersedes (for generated-runtime termination/progress policy only): ADR-0007
  (`docs/decisions/0007-generated-runtime-loop-progress-watchdog.md`), ADR-0016
  (`docs/decisions/0016-bounded-static-finite-loop-progress-proof.md`), ADR-0017
  (`docs/decisions/0017-bounded-generated-data-transform-progress-proof.md`), ADR-0018
  (`docs/decisions/0018-data-transform-progress-scope-identity-vs-whole-scope-cursor-purity.md`),
  ADR-0019 (`docs/decisions/0019-finite-region-cursor-progress-write-and-read.md`), and ADR-0035
  (`docs/decisions/0035-per-invocation-finite-dbf-loop-completion-and-slot-re-arm.md`). This decision
  supersedes those documents for generated-runtime termination/progress policy. Those documents remain
  as historical provenance and are not deleted.
- Preserves in full (unchanged operational semantics): ADR-0020
  (`docs/decisions/0020-deterministic-vblank-irq6-interrupt-progression.md`) — the VBlank scheduler,
  pending/acknowledgment behavior, IE0 gating, IRQ6 arbitration/admission, exception-frame
  construction, RTE, and post-RTE grace are all unchanged. Only ADR-0020's own incidental participation
  in the ADR-0007 watchdog progress-credit pool (a successfully admitted interrupt consuming one shared
  credit unit) is retired, because that credit pool no longer exists.
- Also relates to: ADR-0002 (static translation and fail-closed execution — unaffected), ADR-0037
  (synchronous MC68000 divide-by-zero vector-5 exception entry — unaffected, uses the same shared
  exception-frame helper ADR-0020 introduced), and SEG-007-T252 (the implementation task this ADR
  authorizes).
- Does not amend, and does not relitigate: the wire report schema's existing stop/diagnostic enums
  (`GenesisStopClass`, `GenesisDiagnosticCategory`), ADR-0002's static-dispatch boundary (no
  target-byte fetch, no runtime decoder, no interpreter, no JIT, no second dispatcher), or the C4
  lowering/emission contracts for any individual MC68000 instruction family.

## Context

ADR-0007 introduced a generated-runtime "no-progress watchdog" inside `genesis_runtime_drive`: a
deterministic no-progress window `W` (a fixed dispatch-step count, historically pinned at 128) plus a
single global, never-replenished `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` credit pool. A generated block
could emit a guarded "progress note" (`genesis_note_loop_backedge` for a CPU/codegen-proven finite DBcc
back edge, later extended by ADR-0017/ADR-0018/ADR-0019 to `genesis_note_data_progress` for a
CPU/codegen-proven monotonic data-transform cursor, and by ADR-0035 to `genesis_note_loop_completion`
for DBF loop-invocation completion/slot re-arm). Each accepted note consumed at most one unit of the
shared credit pool per dispatch step; once the pool was exhausted, or whenever `W` consecutive dispatch
steps produced no accepted note, the drive failed closed with
`GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED` / `GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED`. ADR-0020
additionally wired a successfully admitted IRQ6 interrupt as a third bounded consumer of that same
shared credit pool, under the same "at most one shared credit unit per dispatch step" rule.

SEG-007's full-refinement diagnosis (recorded in the SEG-007 milestone record and consumed by
SEG-007-T252's own Evidence) showed that the current generated-native Sonic execution stop is this
global watchdog-credit exhaustion during continuing recurring execution — not a genuine semantic
frontier. A local outer-limit experiment exposed no new genuine semantic frontier either: it bypassed
the semantic-watchdog termination mechanism entirely (rather than raising its credit ceiling and/or
no-progress window `W` as its own diagnostic approach) and instead drove execution under a finite outer
runner allowance, confirming the mechanism was bounding a program that keeps making real, recurring,
architecturally legitimate progress (VBlank-driven IRQ6 admission cycles) — the watchdog's own bounded,
non-renewable credit accounting, not any CPU/device/decoding limitation, was the actual thing
terminating the run.

This is a policy-ownership problem, not a CPU-semantics problem: the generated runtime was conflating
two genuinely different concerns inside one function and one stop pair:

1. **Guest CPU/device semantics** — dispatch a decoded/lowered block, drain a DMA, tick the VBlank
   scheduler, admit an eligible IRQ6, construct/consume exception frames, restore via RTE. All of this
   is architecturally faithful and must be preserved exactly.
2. **Host/runner execution-resource policy** — "how many dispatch steps may this one invocation take
   before returning control to its caller." This is not a guest semantic fact at all; it is invocation
   policy that belongs to whatever process is driving the generated program (a headless/automated CLI,
   a test harness, or a future interactive viewer).

Conflating these two concerns forced every long-running, recurring, architecturally correct program
(any real IRQ6-driven main loop, for example) to eventually collide with a credit ceiling that was
never a hardware fact and was explicitly documented as "a bounded PROJECT-ENGINEERING ceiling," not
"the dispatch budget." The watchdog's own proof-renewal rules (documented at length in ADR-0007,
ADR-0016 through ADR-0019, and ADR-0035) existed only to make that non-renewable credit pool survive
legitimate recurring structure; removing the pool removes the need for all of that machinery.

## Decision

1. **Runner/guest ownership boundary.** The generated runtime now exposes a step/boundary contract with
   no notion of "no progress":
   - `genesis_runtime_step(GenesisRuntime *runtime, GenesisDispatchFunction dispatch)` is the
     guest-owned single-step contract. It performs exactly the SEG-007-T175 defensive VDP DMA drain, one
     `dispatch()` call, the T131 checkpoint-class observation, and the ADR-0020 device-scheduler
     tick / SR-masked IRQ6 admission — in that order — and returns immediately with whatever
     `GenesisControlTransfer` results (`GENESIS_STOP`/`GENESIS_COMPLETE` on those genuine outcomes,
     else `GENESIS_CONTINUE_AT_PC` with `runtime->pc` already advanced, possibly overridden by an
     admitted IRQ6 exception's handler entry). It never loops and never reads or writes any
     progress-credit/watchdog state, because none exists any more.
   - `genesis_runtime_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t
     dispatch_allowance)` is the runner-owned finite dispatch allowance — the sole caller-visible
     repetition mechanism left in this runtime. It calls `genesis_runtime_step` up to
     `dispatch_allowance` times using an overflow-safe bounded loop (never wraps, even when
     `dispatch_allowance == UINT32_MAX`), returning immediately on any genuine guest
     `GENESIS_STOP`/`GENESIS_COMPLETE`. `dispatch_allowance == 0` is rejected the same way the former
     `dispatch_budget == 0` case was. It performs no guest-semantic reasoning of any kind.
   - `genesis_runtime_drive` (the prior combined function) is retired; nothing in this codebase or its
     generated output calls it any longer.

2. **New disjoint outcome: `GENESIS_RUNNER_RESOURCE_LIMIT`.** A new `GenesisControlTransferKind`
   enumerator, produced ONLY by `genesis_runtime_run` when its allowance is exhausted while the guest is
   still `GENESIS_CONTINUE_AT_PC`. It is a host resource limit, never a guest semantic stop, and is
   never conflated with `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED`. Its `GenesisRuntimeStop` payload
   (`GenesisControlTransfer.stop`) is left structurally meaningless/zeroed (stop_class 0 is not a valid
   `GenesisStopClass` value), so a caller must switch on `.kind` first and can never mistake this for a
   real guest stop by inspecting `.stop.stop_class` alone. A new `runner_dispatch_count` field on
   `GenesisControlTransfer` carries the deterministic count of guest dispatch steps actually taken
   before exhaustion — derived only from the runner's own finite loop counter, never from any guest
   state, so an identical fixture with an identical allowance always reports an identical count.
   `genesis_write_sanitized_report`/`genesis_write_full_report` emit this as a new top-level
   `"result":"runner_resource_limit"` value (a sibling of `"completed"`/`"stop"`, never nested under
   `stop_class`/`diagnostic_category`, which stay `null`), plus the `runner_dispatch_count` field.

3. **Headless/automated CLI wiring.** The generated bridge `main()` now accepts an optional
   `--instruction-budget <digits>` argv pair (order-independent alongside the existing
   `--full-report-path`/`--full-report-fd` pair, via a new `genesis_parse_bridge_argv` helper that fails
   closed on anything unrecognized or malformed, including a value of exactly zero, before
   `genesis_runtime_run` is ever called), defaulting to the compiled-in value `128` — retained ONLY as
   the DEFAULT for zero-argument argv-shape compatibility (an unchanged, no-option invocation still
   parses and runs), carrying no hardware/timing meaning. This is explicitly NOT a claim of behaviorally
   equivalent execution depth to the former semantic-watchdog window: that window (historically pinned
   at `W = 128`) was a *consecutive no-progress* window that legitimate recurring guest progress kept
   resetting, so real execution ran for millions of dispatches, whereas this `128` is a *total* dispatch
   count — a radically shorter bound. `tools/genesis_startup_bridge.py` gained a matching
   `--instruction-budget N` option with a validating type function: malformed/negative/fractional/
   out-of-range values are rejected, zero is rejected (this CLI is itself the automated/headless entry
   point), and `UINT32_MAX` is accepted. Its own canonical/headless one-shot `--diagnose-frontier` route
   additionally uses a named canonical allowance, `GENESIS_CANONICAL_RUNNER_DISPATCH_ALLOWANCE`
   (`16777216`; declared as a Python module constant in `tools/genesis_startup_bridge.py`, the sole
   implementation source of truth -- this is host runner/tooling policy, not a Genesis runtime header
   constant), as its own default when the operator omits
   `--instruction-budget`, rather than silently falling through to the generated binary's lower-level
   128 default. This named constant is host runner policy only — not hardware timing, not guest
   semantics, not a discovery-completeness knob, and not an instruction-count correctness claim — and is
   always explicitly overridable via `--instruction-budget`.

4. **`GenesisDeterministicOptions` version-1 compatibility.** The wire/source field `instruction_budget`
   (schema version 1, unchanged) is retained as the compatibility field name for the runner allowance,
   reinterpreted — not renamed — as non-guest-semantic runner policy. No field reorder, no schema-version
   bump, and no serialization/digest byte change: `genesis_sha_options`'s exact hashing field order is
   untouched. Version 1's existing tolerance for a serialized zero (meaning "no bound named") is
   preserved at the wire-schema level even though every live automated/headless caller now rejects a
   zero request at its own CLI boundary.

5. **Dispatch-period-16 classification (value and behavior unchanged).**
   `GENESIS_VBLANK_DISPATCH_STEP_PERIOD == 16` is unchanged in value and behavior. It is explicitly
   classified as a **legacy synthetic compatibility cadence**, sized relative to the historical
   pre-ADR-0040 watchdog no-progress window `W = 128` (`16 == 128 / 8`) so that window would see at
   least one VBlank onset — it was never real Genesis video timing, and the runner's dispatch allowance
   has no influence on it, on VBlank scheduler state, or on any event position. T253 owns replacing this
   coarse dispatch-step source with an explicit virtual video-timing contract; this ADR does not
   implement virtual video timing, a viewer, or pacing.

6. **Removed watchdog-only state and codegen.** The following are removed entirely, along with their
   dedicated tests: `GenesisLoopProgressNote`, `GenesisLoopCompletionNote`, `GenesisDataProgressNote`
   types and their `GenesisRuntime` fields; `genesis_note_loop_backedge`, `genesis_note_loop_completion`,
   `genesis_note_data_progress`; `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`; the `progress_credit` /
   `credit_consumed` / `made_progress` / `steps_since_progress` locals and the IRQ6-admission
   credit-consumption block from the former drive loop; the codegen-side guarded note-emission text for
   DBcc back-edge/completion and for `write_move` WRITE/READ data-transform progress
   (`src/codegen/c11/m68k.cpp`, `src/codegen/c11/frontend.cpp`), and the now-fully-orphaned
   `M68kMemoryEmissionContext` fields that fed only that emission (`loop_progress_object`,
   `finite_loop_progress_proof`, `data_progress_proof`, `read_data_progress_proof`,
   `data_progress_read_region`). `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED` /
   `GENESIS_DIAG_INSTRUCTION_BUDGET_EXHAUSTED` and their `tools/genesis_startup_bridge.py`
   `STOP_DIAGNOSTIC_PAIRS` whitelist entry are retained for wire/ABI stability, documented as currently
   unreachable via this runner path.

7. **Now-inert static loop/data-progress analysis subsystem: removed in this same task.** The static
   CPU-analysis producers of `M68kFiniteLoopProgressProof` / `M68kGeneratedDataProgressProof`
   (`src/cpu/m68k/static_loop_proof.cpp`, their `FrontendPartialProgram` fields, and their own dedicated
   unit-test coverage in `tests/cpu_m68k_static_discovery_test.cpp`) computed proofs no codegen consumer
   read once the note-emission machinery above was removed, making them provably inert dead output. This
   task removes that entire subsystem (the file, its two `FrontendPartialProgram` fields and their
   population call sites, and its dedicated tests) rather than deferring the cleanup: it was confirmed
   to have zero remaining production references anywhere in the codebase before removal. Independently
   truthful, general CPU metadata it happened to consume as input (architectural register-write masks,
   EA auto-update effects, and other `M68kOperationEffect` facts) is unaffected and remains in place.
   A bounded, fixed-capacity (64-entry) circular diagnostic history of the latest architectural PC
   values was added on `GenesisRuntime` (`recent_pc_history`), recorded at one single documented
   boundary inside `genesis_runtime_step` (the PC about to dispatch, before `dispatch()` runs). It has no
   semantic effect, is excluded from every stable serialization (`genesis_sha_options`,
   `genesis_write_sanitized_report`, and the full-report's own required schema), and is surfaced only in
   `tools/genesis_startup_bridge.py`'s private `EPHEMERAL_FRONTIER` diagnostic line for a
   `GENESIS_RUNNER_RESOURCE_LIMIT` frontier, via the existing in-memory full-report pipe — no new
   temporary trace file. A resumable execution-slice primitive remains explicitly deferred to T254.

## Consequences

- Removed watchdog ABI surface: `genesis_runtime_drive`, `genesis_note_loop_backedge`,
  `genesis_note_loop_completion`, `genesis_note_data_progress`, `GenesisLoopProgressNote`,
  `GenesisLoopCompletionNote`, `GenesisDataProgressNote`, `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`, and
  the codegen note-emission text paths above. `M68kMemoryEmissionContext` no longer carries the five
  fields that fed only that emission.
- New disjoint result type: `GENESIS_RUNNER_RESOURCE_LIMIT` / `"result":"runner_resource_limit"`, with
  its own deterministic `runner_dispatch_count` field, at both the sanitized and full report levels.
- Headless/automated callers must now supply an explicit finite allowance when the compiled-in default
  (128) is not the desired bound; `tools/genesis_startup_bridge.py --instruction-budget N` is the
  supported way to do this.
- Honest note on `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED`: a repository-wide grep after this task's
  implementation confirms nothing in `runtime/genesis/runtime.c`, the codegen emitters, or any generated
  bridge `main()` produces this stop class any longer — it is genuinely dead code on this path today,
  kept only for wire/ABI stability (a foreign or historical producer of this exact enum pair must still
  validate against the schema). This ADR does not invent a phantom future owner for it; if a later task
  finds it truly has zero remaining producers anywhere in the codebase, removing the enumerator itself
  would be that task's own bounded decision, not implied here.
- Future ownership boundaries: T253 owns replacing `GENESIS_VBLANK_DISPATCH_STEP_PERIOD`'s coarse
  dispatch-step cadence with an explicit virtual video-timing contract (non-gating). T254 owns an
  optional generic resumable execution-slice primitive and optional host presentation pacing, if a
  concrete headless/test caller later justifies either (neither is needed by this ADR).
- The now-inert static finite-loop/data-progress proof analysis subsystem
  (`src/cpu/m68k/static_loop_proof.cpp`, its `FrontendPartialProgram` fields, and its dedicated tests)
  is removed in this same task, not deferred; see §7. `GenesisRuntime.recent_pc_history` (bounded,
  diagnostic-only, private/ephemeral) is the one new piece of state this task adds.
