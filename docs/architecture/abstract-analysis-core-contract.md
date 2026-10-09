> **RETIRED (SEG-047-T007, ADR 0096).** The abstract-analysis core, its M68K/Z80 adapters and the Genesis analysis-report driver described
> below were removed from the product: the live admission path is the broad immutable-ROM universe, the native ML region producer, structural
> pruning and the admission validator. This document is kept only as the historical contract of ADRs 0076-0091.

# Abstract-analysis core contract (SEG-027-T004; accepted by SEG-029-T001)

- Status: **Accepted** by ADR 0078 (SEG-029-T001). Drafted by SEG-027-T004. Where this text and ADR 0078 differ, ADR 0078 is
  authoritative. Corrections at acceptance:
  - placement: generic `segarecomp::analysis` (header-only, `libs/analysis`); adapters `segarecomp::cpu_<cpu>_analysis` under
    `libs/cpu/<cpu>/analysis`; no production target links any of them (ADR 0078 decision 1);
  - the `Unknown` vocabulary is closed and final: `unknown_input`, `unsupported_transfer`, `non_immutable_read`, `set_bound`,
    `iteration_bound`, `state_bound`; `imprecise_join` is dropped (decision 3);
  - no staged capability is admitted in SEG-029 (decision 7);
  - section 6: SEG-028 has landed; the generic core takes no image, and adapters own the immutable-read oracle (decision 9).
- Decision context: ADR 0076 (SEG-029 ACTIVATE as an incremental core); evidence in `gen3-evidence-ledger.md` (cited as `[L x]`) sections 2.2, 3 (S4, S5)
  and 4.
- Section 8 (M68K instantiation) is Accepted by ADR 0079 (SEG-030-T001); where it and ADR 0079 differ, ADR 0079 is authoritative.
- Not this contract's job:
  - implementing the M68K staged domains (SEG-030-T003..T006; section 8 fixes their contract);
  - production admission (SEG-031);
  - executable-image production (SEG-028, `executable-image-contract.md`).

## 1. Shape

```text
CPU decoder + CPU effect owner  ->  CPU-owned abstract transfer (adapter)  ->  generic deterministic solver  ->  precise result OR Unknown
       (libs/cpu/<cpu>)                    (libs/cpu/<cpu>)                       (generic library)
```

- The generic solver never decodes or interprets an instruction. It sees opaque program points, CPU-supplied successor edges, and
  abstract states that it can only join, compare and hand back to the adapter.
- There is no universal CPU IR. Each CPU adapter reads its own decoded instructions and effect owners:
  - M68K: `m68k_operation_effect`, `M68kIrOperation`, `m68k_control_successors`;
  - Z80: `DecodedInstruction` / `FormDescriptor`, plus a small CPU-owned effect projection (section 5).

## 2. Generic / CPU boundary

| generic (may name) | CPU adapter (owns) |
| --- | --- |
| program point (opaque, totally ordered key) | what a program point is: execution PC plus image identity |
| edge kind: `fallthrough`, `branch`, `call`, `return`, `computed`, `exceptional` (closed vocabulary, used only for scheduling and reporting) | which instructions produce which edges, and the targets of computed edges as abstract values |
| abstract state as an adapter-defined type with `join`, `leq`, `is_bottom` | registers, flags, stack, exception frames, addressing modes |
| baseline domain building blocks (section 3) | the mapping from architectural locations to domain values |
| worklist order, iteration and resource accounting, termination | the transfer function for one instruction |
| result: per-query `Precise(set)` or `Unknown(reason)` | what a query means (for example "targets of this computed site") |

The generic part names no M68K or Z80 register, effective address, stack frame, exception, prefix, bank or mapper concept. A
forbidden-identifier scan of the generic library enforces this (`D0`-`D7`, `A0`-`A7`, `HL`, `IX`, `IY`, `SP`, `RTE`, `RETI`, `EA`, `bank`,
`epoch` and similar terms).

## 3. Baseline vs staged capabilities

**Baseline** means it is justified by the first consumer and the fixtures. The first consumer is the re-expression of the SEG-026-T002
exact PC-indexed recovery plus the existing Gen-2 finite register-state walker [L S5]: both are exact finite-set domains over registers
with immutable-byte reads.

- `Unknown`/top, bottom, a constant, and a **small finite set** with a set-size resource bound (exceeding it gives `Unknown`, never a
  wider guess);
- join;
- a deterministic worklist / fixed-point solver with an iteration bound and a state-size bound;
- an immutable-image read oracle supplied by the caller (an executable-image view; section 6);
- the CPU-adapter seam.

This list illustrates the baseline; it is not a frozen minimum. SEG-029's first child may shrink it if the first fixture needs less.

**Staged capabilities.** Each one is admitted only under the four-part rule:

1. a named consumer;
2. a named precision problem;
3. why the simpler domains already present are insufficient;
4. observable acceptance.

| candidate | likely consumer | precision problem it would address | status |
| --- | --- | --- | --- |
| address region plus offset | SEG-030 `(An)`/`d16(An)` object fields | 59 of 70 observed width-only sites read a register-relative field with a non-provable base [L 2.2] | staged: the first expected staged child |
| points-to sets | SEG-030 `JSR (An)` (first gate for 5,367 missed PCs) | pointer provenance across calls | staged |
| abstract memory (store/load over regions) | SEG-030 state-field dispatch | store-provenance poisoning (ADR 0055) | staged; needs regions first |
| intervals / strided intervals | jump-table extents larger than the finite-set bound | sets exceed the bound | staged |
| widening | loops over unbounded counters | non-termination without widening | staged; widening never yields certainty; the solver iteration bound remains |
| call contexts / summaries | interprocedural object pointers | context-insensitive merge loses identity | staged; bounded context depth only |
| exception/return state | RTE resumption (ADR 0051 first gate) | RTE target provenance | SEG-030 workstream, reported `Unknown` until proven |

SEG-026-T002's exact PC-indexed recovery is the **regression baseline**, split in two levels. SEG-029's M68K first-consumer adapter
reproduces the fixture-level finite-value results of the SEG-026-T002 index-domain proof (`reachability_pc_index_recovery_test`) exactly.
SEG-030 reproduces the oracle-level result: `D/U = 2.75%` and 42.74% recall with zero escapes on the same oracle. It is not a foundation to extend, and nothing grows from the removed
SEG-026-T003 code.

## 4. Soundness, resource and determinism rules

- **Never guess.** A query answer is `Precise(set)` only when every abstract value that contributes to it is exact. Otherwise it is
  `Unknown(reason)`. The reason is a closed vocabulary: `resource_bound`, `set_bound`, `unsupported_transfer`, `unknown_input`,
  `imprecise_join` and similar.
- **Bounded.** Iteration count, state count and set sizes are constants (as SEG-032 bounds are). Exhaustion is `Unknown`, never partial
  truth and never a silent widening into false certainty.
- **Deterministic.** Worklist order is a pure function of the program-point order. There is no hash-order iteration and no wall-clock
  dependence. Repeated runs give byte-identical results.
- **Inputs.**
  - No runtime evidence feeds analysis or admission: coverage, traces and external disassemblers are falsifiers only.
  - Relative proofs must state their premise (for example "relative to the discovered predecessor set"). SEG-030 owns invalidation and
    restart, as ADR 0054 does.

## 5. Z80 second-CPU proof requirement

SEG-029 must instantiate the generic interface with a **bounded, project-authored synthetic Z80 consumer**. It is architectural
validation, not a production Z80 reachability mode.

- It uses `libs/cpu/z80` decode (`DecodedInstruction`, `FormDescriptor`).
- `libs/cpu/z80` has no effect projection comparable to `m68k_operation_effect` today: the semantics reach code only through C11
  lowering (ADR 0059 split). SEG-029 therefore needs a small CPU-owned Z80 effect/successor projection inside `libs/cpu/z80`, covering
  register reads/writes, memory access shape and control successors, for the forms the fixture uses. It must not parse lowering text,
  and it must not move lowering into the CPU library.
- The fixture exercises:
  - constant and finite register values;
  - a branch join;
  - an HL/IX/IY-like address value;
  - a memory load and store;
  - a register-derived indirect jump (`JP (HL)` or similar).

  A smaller fixture is acceptable if it proves the same seam.
- It demonstrates that the solver carries no M68K register, EA, stack-frame or exception assumption (the forbidden-identifier scan plus
  a code-review checklist).
- Production Z80 stays broad AOT and does not link the analysis.

## 6. Image input

The analysis consumes an **executable-image view**: bytes, mapping and an immutable-read oracle.

- Until SEG-028 lands, the M68K view is the immutable cartridge claim set and the Z80 fixture view is a project-authored byte array.
- When SEG-028 lands, the view adapts to its artifact (`executable-image-contract.md` questions 1-4). This needs no change to the solver.
- SEG-029 does **not** depend on SEG-028.

## 7. What SEG-029 must not do

- Implement full M68K indirect recovery (SEG-030) or change production admission (SEG-031).
- Require SMT, or whole-program path-sensitive symbolic execution.
- Pre-commit to a complete VSA framework, or build every staged domain up front.
- Introduce a universal CPU IR, a generic hardware/memory emulator, or platform banking semantics in generic domains.
- Carry the ADR 0076 STOP conditions in weakened form. They apply in full.

## 8. M68K instantiation (SEG-030; ADR 0079)

- **Driver.** Report-only `platforms/genesis/analysis_report/` (`segarecomp::genesis_analysis_report` + `segarecomp-genesis-analysis-report`);
  links `machine_genesis` and `cpu_m68k_analysis`; never installed; never linked by the `segarecomp` CLI or any production target
  (`analysis_core_boundary_test`, amended by ADR 0079 decision 2).
- **Roots.** `genesis_reachability_roots`: the reset entry plus every installed machine-delivered vector handler, the challenger's own
  owner. Each root is seeded with the all-Unknown state.
- **Image view.** `GenesisM68kAnalysisImage` over `genesis_m68k_executable_images`: `immutable_input` cartridge images plus `static_proof`
  ADR 0049 aliases at their work-RAM base execute under the challenger's ownership rules; only `immutable_input` bytes at their cartridge
  address are immutable. An alias execution address is mutable work RAM.
- **Locations.** Data registers (baseline), address registers with points-to, abstract memory cells and stack/frame cells (staged).
  CPU-owned regions: `image(id)`, `work_ram`, `io_device`, `unknown`; the stack is `work_ram` at the tracked absolute A7 offset.
- **Staged domains (admitted, CPU-owned in `libs/cpu/m68k/analysis`, each inert when off).** Address region + offset / points-to (T003),
  abstract memory with object fields and alias exclusion (T004), k = 1 contexts and summaries (T005), exception/return frames (T006).
  Candidates, not admitted: intervals beyond 4,096, general widening, pin minimization, selective symbolic execution.
- **Points.** `(ctx << 24) | pc`, ctx = 0 or call-site PC + 1; `D` = low 24 bits of reached points; at most 8 contexts per callee entry.
- **Asynchronous writers.** Vector-root code is a potential asynchronous writer: its store cells are never strong-updated and read
  Unknown outside handler code; initial work RAM is Unknown; handler entry state is Unknown except a derived A7. Bus-master writes are
  excluded or poisoned as recorded in ADR 0079 decision 7.
- **Rounds.** Monotone configuration growth, at most 16 rounds, mandatory final validation; non-convergence turns the dependent domain off
  (`iteration_bound`).
- **Results.** `Precise(set)` or `Unknown(generic reason x CPU sub-reason)` per site, in the ADR 0079 decision 10 families; no generic
  `Unknown` vocabulary change.
- **Resource constants.** Solver 10^6 iterations / 2^20 points; finite set 4,096; points-to <= 8 pairs; exact offsets <= 64 else strided;
  <= 512 memory cells per state; K = 8; R = 16.
- **Regression baseline.** With every staged domain off, the driver reproduces the SEG-026-T002 strict oracle row exactly
  (`D` = 6,765, D/U 2.75%, recall 42.74%, zero escapes); see ADR 0079 T002 record.
