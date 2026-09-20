# ADR 0005: The CLI Static Discovery Report Is Outside §14's Wire-Report Privacy Boundary

- Status: Accepted
- Date: 2026-08-21
- Amends: ADR 0004 (`0004-generic-frontier-classification-privacy-boundary.md`); corrects a scope error
  ADR 0004's own consuming task (SEG-007-T064) introduced into the T028 architecture contract's §14
  ("Runtime-stop privacy boundary", `docs/architecture/genesis-generalized-startup-runtime-bridge-
  contract.md`).

## Context

An independent adversarial review of SEG-007-T064's own Checkpoint 8 found a real, internal
contradiction introduced by this task's own earlier Checkpoint 7: §14's original text (predating this
task entirely) is scoped explicitly to "a generated bridge program's full report" — i.e. the compiled
bridge binary's own runtime wire report (§13.3), produced only after a `FrontendPartialProgram`
successfully lowers to C, compiles, and executes. Checkpoint 7's own revision-history entry and §14
amendment, written earlier in this same task, incorrectly extended that scope with a parenthetical
claiming §14 also governs "the unrelated, independently existing `genesis-general-startup` CLI's own
static discovery-time report." That claim was never true of the actual codebase and was not something
Checkpoint 7 needed to claim to accomplish its own goal (adding `opcode_line`/`region_class`); it was an
unforced overreach.

Direct code reading performed during Checkpoint 8 confirms the actual, pre-existing fact: `format_m68k_
frontend_result_legacy` — the formatter `genesis-general-startup` has used for a plain `FrontendRejected`
result since long before SEG-007-T064 existed, exercised unconditionally by every prior real-ROM
validation task in this milestone (SEG-007-T041 through T063) — has always, by default, with no separate
"full" mode and no flag, included `source_address` (hex), `provenance.raw_bytes` (hex), `mapping_claims`
(hex `target_begin`/`target_end`), and `accesses` (hex `address`/`bytes`) in its output. This CLI static
discovery report has never had a sanitized-vs-full split at all — it has only ever had one shape, and
that shape has always included full raw provenance. §14's own restriction, both in its original form and
in every one of its prior revisions, was never about this report; it was always specifically about the
_wire_ report a _compiled and executed_ generated program produces, because only that surface has the
sanitized-vs-full distinction §14 actually describes (§13.3's `report_kind: "sanitized"` vs `"full"`).

Checkpoint 7's own mistaken parenthetical therefore created a genuine, in-diff self-contradiction once
Checkpoint 8 (correctly, per this actual precedent) widened the CLI static report's `partial` branch to
carry the same raw fields the `rejected` branch already always had: the contract's own text, as amended
by Checkpoint 7, appeared to forbid exactly what Checkpoint 8's code (accurately reflecting long-standing
project behavior) did. This is a documentation defect from Checkpoint 7, not a defect in Checkpoint 8's
actual behavior, which matches this project's own established precedent exactly.

## Decision

§14 ("Runtime-stop privacy boundary") governs, and has only ever governed, the compiled bridge binary's
own wire report (§13.3) — the report produced by a `FrontendPartialProgram` that has been lowered to C,
compiled, and executed. It does not govern, and has never governed, the CLI's own static discovery-time
report (`genesis-general-startup`, `format_m68k_frontend_result`), which is a separate, always-single-
shape, always-full-detail local tool output, exactly as its own pre-existing `rejected`-branch behavior
already demonstrates and has demonstrated since long before this task.

The T028 contract's §14 text is corrected (an eleventh revision) to remove Checkpoint 7's own incorrect
parenthetical extension of scope. `opcode_line`/`region_class` (ADR 0004) remain exactly as useful and
exactly as narrowly justified as before for the CLI's own report — this decision does not remove them or
change their own justification — but they were never _required_ to be narrowly justified by §14 for that
specific report surface in the first place; they are independently good, useful, low-risk additions on
their own merits (ADR 0004's own Context/Decision reasoning), not a carve-out from a boundary that was
never actually there for this report.

This decision changes nothing about the compiled bridge binary's own wire report, its own §13.3
sanitized-vs-full split, the driver's `--full-report-path`/`--full-report-fd`/`--compare-runs` behavior,
or any other already-settled T028 contract decision. It is a scope _correction_, not a boundary
_removal_: the wire-report privacy boundary §14 actually describes remains exactly as strict as it has
always been, for the report surface it has always governed.

## Clarification: Private Diagnostic Consumption

The CLI static report may be consumed in detail during a bounded, user-authorized diagnostic session when the execution environment permits it. This includes information needed to classify the active frontier, such as addresses, instruction words, access addresses, and raw diagnostic fields.

Being outside the compiled bridge wire-report privacy boundary does not make that information suitable for publication or durable project evidence. Before an agent passes the result to backlog refinement, planning, implementation evidence, pull-request text, CI, or documentation, it must reduce the report to the minimum non-reconstructable classification required for the next decision.

The durable handoff may contain fields such as:

- pipeline stage;
- stable result and stop class;
- completed-block count;
- instruction family, size, and addressing-mode class;
- device or memory-region class;
- access width and direction;
- emission or runtime-selection result.

It must not contain ROM bytes, instruction words, raw addresses or offsets, disassembly, complete diagnostic reports, or local commercial-image paths.

This clarification does not override restrictions imposed by the agent provider, execution environment, or other external service.

## Consequences

- Reviewers checking privacy discipline for the compiled bridge binary's own wire report must continue
  to apply §14's full strictness (never a raw address/byte/opcode/offset/disassembly/RAM content/local
  path/trace in the sanitized wire report, under any circumstance) — unchanged by this decision.
- Reviewers checking the CLI's own static discovery report (`genesis-general-startup`) should evaluate it
  against this project's ordinary agent-level discipline (`docs/testing/commercial-games.md`: what an
  agent may itself paste into committed text, chat, or logs) rather than against §14's wire-report rule,
  which was never the correct standard for this report surface.
- Any future task amending the T028 contract's §14 should cite this correction rather than repeat
  Checkpoint 7's own mistaken scope extension.
