# ADR 0080: Hybrid Selective Admission and the Per-CPU Production Strategy

- Status: Accepted (SEG-031-T001; the production strategy, decision 12, is recorded by SEG-031-T007).
- Date: 2026-10-04
- Task: SEG-031-T001..T007 (T001 accepts the contract; later children append their records below).
- Related: ADR 0079 (M68K analysis instantiation; decision 1 report-only placement, decision 8 closure premise), ADR 0078 (generic core,
  no production linkage), ADR 0077 (executable images), ADR 0076 (Gen-3 activation and per-CPU strategy), ADR 0051 (the SEG-024
  operand-width region rule and its snowball), ADR 0049 (immutable-copy aliases), ADR 0052 / ADR 0071 (broad-AOT build economics),
  ADR 0053 (execution-PC coverage).

## Context

SEG-030 delivered a sound, report-only M68K instantiation (`precise target set OR Unknown`). Its soundness infrastructure is proven, but
sound precision on Sonic 1 is poor: `U` 246,293 broad identities, `D` 1,276 (D/U 0.52%), observed recall 9.73%, zero escapes. The
earlier ~43% recall relied on an unsound interrupt-register assumption and is historical.

Low recall alone does not decide production value. The production question is:

> Given sound precise discovery `D`, how much of broad AOT `U` must be retained as conservative fallback to produce a complete static
> generated-native program?

## Decision

1. **Production metric.** `hybrid_admission = D ∪ fallback islands ∪ transitive static closure`; the primary metric is
   `hybrid_ratio = |hybrid_admission ∩ U| / |U|`, followed by measured generated-C, build, binary and runtime effects. Observed recall is
   a precision diagnostic and falsifier only.
2. **Safety invariant.** For every uncovered dynamic control site of the hybrid set (an unresolved computed site, or a return the
   SEG-030 continuation model does not cover): `PossibleTargets(site) ⊆ island entries(site) ⊆ H`. The exact target need not be known;
   the container of the uncertainty must be proven.
3. **Authorities and the widening ladder.** Uncertainty only enlarges the admitted set:
   1. exact SEG-030 target sets (followed by the analysis; no island);
   2. a `JMP/JSR (An)` or `d16(An)` site whose address register holds a proven SEG-030 points-to set (joined over every context and
      partition of the site, not width-derived): the island entries are exactly the set's members plus the displacement (strided
      members enumerated) that are mapped even PCs of the executable-image view (`points_to_region`, `executable_image` when the set
      strides over a whole image, `materialized_image` when it names `static_proof` alias PCs);
   3. anything else has no bound narrower than the whole program: the result is whole broad AOT (`whole_image`).

   The executable-image view is the SEG-028 artifact (ADR 0077) through ADR 0079 decision 3. **Forbidden** as authorities: byte
   appearance, Ghidra or any external disassembler, runtime coverage (it can neither create nor shrink an island), operand width (the
   SEG-024 rule, ADR 0051), nearest functions, and title identity.
4. **Real fixed point.** Island members enter the same SEG-030 instantiation as computed edges of their site that carry the site's own
   state, exactly as a resolved site's targets (a call enters its callees in the call-site context and they become callees of its
   continuation). Island code is therefore analysed (its returns, stores, further transfers and interrupt boundaries), and the ADR 0079
   closure premise ("every fact is relative to the discovered set") holds over the hybrid set `H` instead of over `D`. Each round's
   uncovered sites widen the islands (a union per site); the planner stops at the first round whose islands do not grow. The CPU-owned
   adapter change is opt-in (`M68kAnalysisConfig::island_entries` / `opaque_entries`, empty for every SEG-030 report, whose output is
   byte-identical).
5. **Fail-closed outcomes.** Each of these yields whole broad AOT, never a partial hybrid program, and a resource ceiling is never
   evidence that omitted code is unreachable: an image set that does not validate; a broad analysis that rejects the program; an
   incomplete solve; a frames round that does not validate (`historical_assumption` is never credited); an unbounded container; more
   than 65,536 island entries; no fixed point within 8 rounds; a failed independent validation.
6. **Independent validator.** At the fixed point the planner re-checks, outside the solver's bookkeeping: every machine root, every
   fixed successor and stacked call continuation (`m68k_control_successors`) of every admitted instruction, every resolved target and
   every island entry that decodes is admitted; every uncovered site is configured with its whole recomputed container; no reached
   PC that the analysis cannot decode is a broad identity (otherwise the hybrid could stop where broad runs).
7. **Machine roots.** The roots are the machine-delivered vector roots (`genesis_reachability_roots`, shared with SEG-030), always
   analysed. Every `static_proof` (ADR 0049) executable image is admitted whole as a mandatory root set, entered as opaque all-Unknown
   entries from the startup entry (no proven source transfer gives their entry state). A handler that SEG-030 cannot analyse precisely
   is covered conservatively, never removed.
8. **Inherited premise.** The hybrid inherits exactly SEG-030's named return-slot integrity premise for an ordinary RTS (ADR 0079
   decision 8); the planner reports the premise sites. It adds no premise of its own.
9. **Placement.** The planner consumes the analysis, so it lives in the report-only `platforms/genesis/analysis_report` target (ADR 0079
   decision 1); `segarecomp-genesis-analysis-report --hybrid-plan <path>` writes the plan. Production never links the analysis
   (`analysis_build_graph_test` unchanged). The M68K interpretation of SEG-030 facts is M68K/Genesis-owned; no generic CPU-neutral
   abstraction is added because no second CPU exercises it.
10. **Production seam (explicit candidate).** The plan is a strict, bounded text artifact (`segarecomp.m68k_hybrid_admission_plan.v1`:
    ROM digest, alias set, strategy, admitted half-open ranges over the broad identities). The production emitter consumes it only
    through `emit-general-startup-bridge-c --immutable-rom-aot-admission <plan>` (and `segarecomp build --admission-plan <plan>`). The
    machine owner (`platforms/genesis/machine` `hybrid_admission.hpp`) validates it fail-closed (digest, alias set, well-formed ranges,
    non-empty, structural closure over the broad identities: machine roots, `static_proof` identities, fixed successors and stacked call
    continuations) and only then filters `FrontendAnalysis::immutable_rom_aot_entries`. Dynamic containment is the planner's proof; the
    production owner cannot re-derive it without the analysis. The filtered program is an ordinary generated-native program: no
    identity knows whether it came from `D`, an island or broad admission, and the runtime is unchanged. Without the option the emission
    is byte-identical to pre-SEG-031 `main`.
11. **Production-default rule (pre-registered before any candidate measurement; milestone record).** Mandatory per title: zero observed
    escapes on its complete oracle, identical guest behaviour to broad on the oracle workload, deterministic output, no title-specific
    logic. Material benefit per title: generated C bytes -30% or better **and** generated-code compile CPU -25% or better. No runtime
    regression beyond +15% (the documented host noise band). A class switches its default only with at least two complete-oracle titles
    meeting the benefit and no title failing the mandatory checks.
12. **Production strategy.** Recorded by SEG-031-T007 below.

## Consequences

- SEG-031 can answer the milestone question per title with a sound plan and sanitized attribution, without a second decoder, discovery
  or recompiler: islands select from the existing broad identities.
- If every title degenerates to broad, broad AOT stays production and the attribution names the generic precision blockers.
- The plan artifact holds exact addresses: for a commercial input it is private and ignored, like the SEG-030 private outputs.

## Records

### SEG-031-T001 (contract and report-only feasibility)

Pending.
