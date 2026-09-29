# ADR 0053: Reachability-First Discovery Challenger vs Observed Attract-Mode Coverage (Experiment)

- Status: Accepted (decision: REFINE — concentrated dynamic gap; next experiment: PC-indexed immutable jump-table
  recovery; broad Gen-2 immutable-ROM AOT stays the production representation)
- Date: 2026-09-29
- Task: SEG-026-T001
- Related, unchanged: ADR 0002 (static translation, fail closed), ADR 0011 (whole-program RTS continuation set),
  ADR 0039 (independent immutable-ROM AOT identities), ADR 0043 (exception model), ADR 0048 (push-then-RTS),
  ADR 0049 (immutable-copy aliases), ADR 0051 (superset reduction rejected; this experiment answers its
  "full observed-PC coverage transport" revisit condition).

## Question

Starting only from architectural roots and ordinary reachable control flow, how much of the MC68000 program can be
discovered without broad whole-ROM AOT, and which dynamic-control mechanisms account for the code a long
deterministic generated-native run actually executes but that discovery misses?

Two independent pieces are compared. Runtime observations may falsify the challenger. They never expand or
authorize it, and a PC that was not observed is not thereby unreachable.

- **Oracle `O`:** every distinct PC retired by the current broad Gen-2 generated-native program during an
  automated no-input attract run.
- **Challenger `D`:** static discovery from architectural roots only.

## Mechanisms (retained, report-only; see `docs/testing/execution-coverage.md`)

1. **Complete execution-PC coverage.** `GenesisRuntime.execution_coverage` is a host-owned observer, NULL in
   every ordinary build.
   - **What it records:** it marks the PC of every retired instruction in a 1 MiB bitmap (one bit per even
     24-bit address). The executing PC is tracked from each dispatch and from every continuing retirement's
     selected successor.
   - **Why retirement:** ordinary blocks retire several instructions per dispatch (Sonic: 292.9M retirements
     over 224.4M dispatches), so dispatch PCs alone would be incomplete.
   - **First-entry witness:** records the predecessor and cause of each new PC. Interrupt resumption is
     attributed to the pre-interrupt instruction, not the handler's RTE.
   - **Guarantees:** no decode, no guest-state read-back, excluded from every report and digest.
   - **Headless run:** a frame-bounded hook (`--execution-coverage FRAMES`, same seam as `--capture-frames`)
     reports aggregates and writes exact PCs only to an ignored private directory.
2. **Challenger.** `m68k_control_successors` (cpu/m68k) is a thin projection of `m68k_operation_effect`.
   `run_genesis_reachability_challenger` (machine/genesis) uses it as follows:
   - **Roots:** reset + handlers of the vectors the machine model can deliver (IRQ6 and the synchronous
     vectors).
   - **Fixed flow:** one decode per reached PC with the unchanged decoder/lifter. It follows only fallthrough,
     BRA, both Bcc/DBcc outcomes and BSR/JSR/JMP direct targets, and never sweeps.
   - **Returns:** a challenger-owned call-continuation set, never the Gen-2 return set. It is resumed once an
     ordinary RTS is reachable; ADR 0048 push-then-RTS is a computed jump and does not count.
   - **Exception returns:** RTE/RTR discover nothing under `strict`. `normal_resumption` is a measurement-only
     hypothesis that resumes only stacked TRAP continuations.
   - **Other runtime-derived PCs:** recorded by family and not followed.
   - **Never consulted:** broad AOT identities, runtime coverage, hints and external disassemblers.
3. **Comparison.** `tools/reachability_coverage_compare.py` prints aggregates only. Each missing PC is traced
   back through first-entry witnesses to the transition that leaves `D` (first gate) and to the closest dynamic
   transition (nearest mechanism). An optional private classification of observed PCs, decoded by the same
   decoder, labels transitions structurally. It never enters `D`, and it traces a return to an undiscovered
   caller back to that caller.

## Workload (pinned authorized Sonic 1 image, no controller input, current broad-AOT build, `-O2`)

- **Calibration (private, once).** A frame capture found the attract loop:
  - title → first demo → intro/title, back at about frame 3,030;
  - four distinct demos (three zone demos and a special-stage demo), with the fourth returning at about frame 11,520;
  - the first demo repeats by about frame 12,240.
  - No product logic uses game knowledge; the runs are a fixed generic frame count.
- **Runs.** 23,200 published frames (two full demo rotations), dispatch allowance cap 1.5e9.
  - Result `frames_reached`: 224,352,923 dispatches and 292,926,087 retirements.
  - Checkpoints every 100 frames.
  - No guest stop, and no work-RAM execution observed.

| checkpoint (frames) | distinct observed PCs | in `D` | recall |
| --- | --- | --- | --- |
| 3,100 (first demo cycle) | 6,983 | 1,000 | 14.3% |
| 6,200 | 8,808 | 1,005 | 11.4% |
| 9,100 | 9,340 | 1,005 | 10.8% |
| 11,600 (one full rotation) | 10,507 | 1,018 | 9.7% |
| 23,200 (two rotations) | 10,512 | 1,023 | 9.7% |

The second rotation adds 5 PCs (0.05%). The workload is stable; this is not completeness.

**Determinism and zero effect.**
- Two identical enabled runs agree byte-for-byte:
  - every counter, the coverage digest and all 232 epoch digests;
  - the frame-stream digest and the final CPU/RAM/device-state digest;
  - the private bitmap and witnesses.
- A coverage-disabled run of the same binary gives the identical frame-stream digest, final-state digest and
  dispatch count.
- Synthetic fixtures also check whole-runtime `memcmp` equality with and without the observer.

## Results

| set | size | ratio |
| --- | --- | --- |
| `U` broad immutable-ROM AOT identities | 246,293 | |
| `D` challenger (strict = normal-resumption; no TRAP reached) | 1,276 | D/U = 0.52% |
| `O` observed attract PCs | 10,512 | O/U = 4.27%, O/D = 8.24 |
| `O ∩ D` | 1,023 | observed recall 9.73% |
| `O − D` | 9,489 | |
| `D − O` | 253 | |

**Reachable unresolved sites in `D`:**

| family | reachable sites | executed |
| --- | --- | --- |
| `JMP (d8,PC,Xn)` | 2 | 2 |
| `JSR (d8,PC,Xn)` | 2 | 2 |
| RTE | 2 | 1 |
| ordinary RTS (resolved by the continuation set) | 49 | 36 |

No `(An)`, `d16(An)`, `(d8,An,Xn)`, push-then-RTS, RTR or unclassified site is reachable.

**First-miss attribution (structural).**
- The first gate for all 9,489 missed PCs (100%) is PC-indexed control:
  - `JSR (d8,PC,Xn)`: 9,238 missed PCs behind 2 sites;
  - `JMP (d8,PC,Xn)`: 251 missed PCs behind 2 sites.
- Nearest mechanism:

  | family | missed PCs | sites |
  | --- | --- | --- |
  | PC-indexed (`JSR`/`JMP (d8,PC,Xn)`) | 9,090 (95.8%) | 84 (25 JSR, 59 JMP) |
  | `JSR (An)` | 386 (4.1%) | 3 |
  | `JMP (An)` | 13 (0.1%) | 1 |

  No other family appears.
- How missed PCs were first entered:
  - 8,669 by fixed flow;
  - 552 as ordinary returns to observed callers;
  - 268 by dynamic transfers.
- Model checks on this workload:
  - **RTS model holds.** Across the whole observed program, every RTS first entry returned to an observed
    call's continuation.
  - **RTE normal-resumption hypothesis holds.** 20,955 interrupt redirects and 20,954 resumptions (the last
    interrupt was still in flight). No RTE/RTR transition reaches anything but the interrupted successor.
- The raw temporal witness attribution is kept for transparency: 738 missed PCs look "RTS-caused". Every one of
  them is a return into a caller that was itself reached through the PC-indexed gates.

**Cost.**
- The challenger runs in about 1.2 s, including the broad analysis for `U`.
- **Coverage overhead.** Measured on a quiet host (load average 7–10 on 12 cores) with alternating
  coverage-off/on runs of the identical `-O2` binary, three pairs per mode:

  | mode | off, wall | on, wall | wall | host instructions | cycles |
  | --- | --- | --- | --- | --- | --- |
  | no render, 23,200 frames | 25.4 s (25.0–25.6) | 26.9 s (26.7–27.2) | +6.1% | +3.5% | +4.2% |
  | rendered, 3,000 frames | 27.2 s median (27.1–28.5) | 27.2 s (27.2–27.3) | +0.2% | +0.3% | within noise |

  - The no-render cost is about 37 host instructions per retired guest instruction.
  - An earlier measurement on a heavily loaded host gave the same instruction overheads.
- The no-render oracle reproduces the rendered one exactly: same coverage digest, witnesses, final state, and
  all 232 epochs.
  - It runs the two-rotation workload (223 guest seconds) in about 27 s, roughly 8x faster than real time.
  - Rendering every frame costs about 10x more per frame than the guest execution itself.

**Supplementary second title (Phase E, not an attract oracle).**
- The 1 MiB Sonic 2 image under the same no-input route shows its intro text, then stops publishing renderable
  frames. Its coverage plateaus by frame 2,000 at `O` = 2,478, and it never reaches an attract demo.
- Automating that title would need new support, so it is only a supplementary data point:

  | measure | value |
  | --- | --- |
  | `U` | 496,387 |
  | `D` | 335 |
  | observed recall | 10.8% |
  | first gate: 2 `JSR (d8,PC,Xn)` sites | 2,208 of 2,211 missed PCs |
  | first gate: 1 RTE site | 3 of 2,211 missed PCs |
  | nearest mechanism: PC-indexed | 96.9% |
  | nearest mechanism: `(An)` | 2.9% |

- The RTE site is the one counterexample to the normal-resumption hypothesis: that exception return does not
  resume an interrupted successor.

## Decision

**REFINE — concentrated dynamic gap.**
- Ordinary reachability is dramatically smaller than `U` (0.5%). The observed workload itself is 4.3% of `U`.
- Every miss enters through PC-indexed control. It is the first gate for 100% of missed PCs and the nearest
  mechanism for 95.8% of them, across a bounded number of sites (4 gates, 84 observed sites).
- A small secondary `(An)` family remains behind those gates.
- No broad RTE/RTS problem appears on this workload once interrupt resumption is attributed correctly. The
  supplementary title has one small RTE counterexample, so the normal-resumption model stays a hypothesis.

The single recommended next experiment is **PC-indexed immutable jump-table recovery**:

- derive the extents and targets of `JMP/JSR (d8,PC,Xn)` tables from immutable image bytes and fixed facts
  only, never from runtime coverage;
- measure how `D` grows and whether it balloons (ADR 0051's operand-width regions did);
- check the new recall against this same private oracle, and the families exposed next (`(An)` object
  dispatch is expected).

`(An)` target provenance and exception-frame provenance are not recommended now.

Broad Gen-2 AOT remains production; nothing here changes admission, emission or runtime authority.

## Limits

- **First gate vs whole picture.** "100% first gate" rests on only four reachable sites that sit in front of the
  whole game-mode program. It identifies the first mechanism to resolve, not a guarantee about the rest.
  - The nearest-mechanism figures describe transitions that were observed.
  - Statically resolving the tables may still expose further families, such as `(An)` object dispatch, or growth
    from decodes that are really data. Measuring that is the next experiment's job.
- One title and one no-input workload. It does not cover every level, boss, special stage, ending or menu path.
- **Coarse continuation set.** The challenger's continuation set is context-insensitive, global and latched. Once
  one ordinary RTS is reachable, every discovered call's continuation is discovered, including continuations of
  calls that never return. Data that follows such a call could therefore enter `D`.
  - On Sonic this is harmless: the 16 continuations that were discovered but never observed all belong to calls
    that were never observed.
  - `D − O` (253) keeps any such growth visible.
- Recall is a validation metric only, and `O` is not an executable-set bound.
- The attribution after the first gate uses observed transitions. A static resolution of the PC-indexed tables
  could still expose additional dynamic families or data-decode growth that this workload never executes.
- `D` identity is per execution PC. ADR 0049 aliases are supported, but none were needed.

## Architectural note (not implemented)

A complete discovery architecture is expected to distinguish:

- control-flow discovery inside a known executable image;
- **materialization** of new executable images: ROM→RAM copies, decompression, Z80 uploads, overlays.

ADR 0049 verbatim copy aliases are unchanged. Materialization is a separate experiment.
