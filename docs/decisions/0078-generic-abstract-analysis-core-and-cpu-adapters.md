# ADR 0078: Generic Abstract-Analysis Core and CPU Adapters

- Status: Accepted (SEG-029-T001).
- Date: 2026-10-02
- Task: SEG-029-T001..T006 (T001 accepts the contract; later children append their records below).
- Contract: `docs/architecture/abstract-analysis-core-contract.md` (Accepted by this ADR).
- Related: ADR 0076 (SEG-029 ACTIVATE as an incremental core; STOP list), ADR 0077 (executable-image artifact), ADR 0054/0055
  (relative proofs, store-provenance poisoning), ADR 0056/0059 (Z80 decode/lowering split). Evidence: `docs/architecture/gen3-evidence-ledger.md`
  sections 2.2, 3 (S4, S5) and 4.

## Context

Two ad-hoc M68K finite-value analyses exist with no shared solver: the Gen-2 static-discovery walker and the SEG-026-T002
`IndexEvaluator`, a demand-driven backward evaluator over the CPU-owned `finite_register_values` domain [ledger S5]. SEG-026-T003 showed
that the remaining M68K gap needs pointer/alias/object reasoning (SEG-030), which needs a reusable engine. ADR 0076 activated SEG-029 as
the *smallest* deterministic, bounded, generic dataflow core that the measured first consumer justifies, validated by a second CPU so
the generic part is not M68K-shaped.

## Decision

1. **Placement and dependency direction.**
   - The generic core is the header-only `segarecomp::analysis` (`libs/analysis/include/segarecomp/analysis/`), linking only
     `segarecomp::base`. It depends on no CPU, codegen, recompiler or platform library.
   - Each CPU adapter is a CPU-owned, separate target under `libs/cpu/<cpu>/analysis/`: `segarecomp::cpu_m68k_analysis` and
     `segarecomp::cpu_z80_analysis`. They link their CPU library plus `segarecomp::analysis`. The CPU libraries themselves never link the
     analysis.
   - **No production target links any analysis target.** Only tests do. `tests/analysis_core_boundary_test.py` enforces this over every
     `CMakeLists.txt`. Production generated output and admission are therefore unchanged by construction; production Z80 stays broad AOT.
2. **The seam.** The solver sees opaque, totally ordered 64-bit program points (the adapter encodes them), an adapter-defined
   `State` with ADL `join`/`leq`, and a pure monotone `transfer(point, in) -> TransferResult` that returns typed edges. Edge kinds are the
   closed set `fallthrough, branch, call, return_edge, computed, exceptional`, used for reporting only. A computed edge carries a target
   the adapter derived from a *precise* abstract value. A computed site whose value is not precise reports `unresolved_computed(reason)`
   and contributes no edge, so results are relative to the discovered edge set (the ADR 0054 premise). The solver never decodes an
   instruction.
3. **`Unknown` vocabulary (closed).** `unknown_input`, `unsupported_transfer`, `non_immutable_read`, `set_bound`, `iteration_bound`,
   `state_bound`. The enum order is the deterministic join priority (joining two Unknowns keeps the smaller reason). The draft's
   `imprecise_join` is dropped: a join never creates imprecision on its own; it only propagates an input's Unknown or exceeds the set bound.
4. **Bounds (constants).** `default_set_bound = 4096` (equal to `m68k_finite_values_limit`, the first consumer's bound),
   `default_max_iterations = 1,000,000`, `default_max_points = 2^20`. A caller may lower a bound, never raise it. Exhausting the set bound
   gives `Unknown(set_bound)` for that value. Exhausting an iteration or point bound makes the whole solution incomplete: every query
   returns `Unknown(iteration_bound | state_bound)`, never a partial answer and never bottom.
5. **Determinism.** The worklist is the ordered set of pending points and always pops the smallest key; states live in ordered maps;
   there is no hash-order iteration and no clock. Repeated runs and permuted entry lists give byte-identical results.
6. **Baseline domain (exactly).** `FiniteValue` = bottom | exact sorted set of 64-bit values bounded by the set bound | `Unknown(reason)`,
   with `join`, `leq` and an exact pointwise `map`; `ValueVector<N>` = a pointwise product over a fixed number of adapter-defined
   locations with a reachability bit. Every element is traced to the first consumer: `M68kFiniteValues` is exactly an exact finite set
   with Unknown, union join and a 4096 bound; the M68K adapter needs a per-location product for the eight data registers at two widths.
   The Z80 fixture needs nothing more. Nothing else is baseline.
7. **Staged capabilities: none admitted in SEG-029.** Address region plus offset, points-to sets, abstract memory, intervals / strided
   intervals, widening, call contexts / summaries and exception/return state remain staged under the four-part rule (consumer, precision
   problem, simpler-domain insufficiency, observable acceptance). Address region plus offset names a consumer (SEG-030 `(An)`/`d16(An)`
   object fields) and a precision problem, but no SEG-029 consumer or fixture needs it, so it has no observable SEG-029 acceptance. It is
   the first expected SEG-030 candidate. Without widening, termination rests on the finite set bound plus the iteration bound.
8. **Forbidden identifiers.** The generic headers name no M68K or Z80 register, effective-address, stack, exception, prefix, bank,
   mapper or platform concept. The scan list is in `tests/analysis_core_boundary_test.py`. Generic headers include only other
   `segarecomp/analysis` headers.
9. **Image input.** The generic core takes no image at all: memory reads happen inside CPU transfers, through a caller-supplied
   immutable-read oracle owned by the adapter. SEG-028 has landed (ADR 0077), so an adapter may back the oracle with an
   `ExecutableImageSet` view. The fixtures use project-authored byte arrays. Neither the solver nor the domain depends on ADR 0077 types.

## Consequences

- SEG-030 instantiates the M68K adapter against real indirect-recovery problems and admits staged capabilities under the four-part rule.
- SEG-031 owns any admission change; nothing here affects production.
- The ADR 0076 STOP list applies in full: no universal CPU IR (each adapter reads its own decoded form), no generic memory emulator, no
  banking semantics or platform concepts in generic domains, no SMT, no whole-program path sensitivity, no unbounded contexts.
