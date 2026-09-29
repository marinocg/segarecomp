# ADR 0055: Static Store Provenance of Width-Only PC-Indexed Dispatch Indices (Experiment, Not Retained)

- Status: Accepted (decision: **STOP** for the reachability-first discovery direction. The experiment
  implementation is **not retained**. Broad Gen-2 immutable-ROM AOT stays the production representation.)
- Date: 2026-09-29
- Task: SEG-026-T003
- Related, unchanged: ADR 0054 (exact PC-indexed recovery in the reachability challenger), ADR 0053 (the challenger
  and the attract-mode coverage oracle), ADR 0051 (operand-width regions snowball; same simplicity-first precedent
  for removing a rejected experiment), ADR 0002 (static translation, fail closed).

## Question

Can the finite values of mutable-state dispatch indices be proven from statically discoverable stores, without
general pointer/alias analysis, interprocedural value-set analysis or symbolic execution?

After ADR 0054, 94.4% of the missing observed Sonic attract execution (`O − D`) lies behind at least one
`JMP/JSR (d8,PC,Xn)` whose index is a byte loaded from mutable memory with no explicit bound (`width_only_domain`).
The experiment asked whether such a byte `S` can be given an exact store domain `Values(S)` whose completeness is
itself locally provable, and whether that would unlock the remaining execution.

## What remains in the product

Only two things remain:
- this record;
- one independent correctness fix to the SEG-026-T002 finite-value helper (below).

Every mechanism described under "Experiment" existed only on the experiment branch. It was removed before merge
because it has no continuing consumer: no CLI flag, API, type, report field or test of it exists in the product.

Unchanged by the experiment and by its removal:
- SEG-026-T001 execution coverage, first-entry witnesses, the challenger and the O-vs-D comparison;
- SEG-026-T002 exact PC-indexed recovery;
- production admission, emission, generated C and runtime.

**Retained correction (independent of store provenance).** `finite_register_values` could be queried narrower than
the memory operand being loaded, for example a `.W` index register loaded by `MOVE.L` from an immutable table. It
used to read only the high bytes of the big-endian operand. It now reads the operand at its architectural access
width and then restricts to the requested low bits, which lie at the higher addresses. A focused regression in
`reachability_pc_index_recovery_tests` covers the `(d8,PC,Xn)` and absolute forms; reverting the fix fails it. The
T002 Sonic 1 challenger outputs are byte-identical with the fix, so the defect never fired on the measured workload.

## Experiment (implemented and tested on the experiment branch only; removed)

The mechanism was report-only and off by default, behind `--pc-index-recovery`.

- **Source classification.** Each mutable byte index source was labelled by addressing class:
  - absolute RAM byte;
  - `(An)`/`d16(An)` field;
  - `(d8,An,Xn)` field;
  - `(An)+`/`-(An)` pointer.

  A register-relative source counted as a location only if its base `An` had an exact value proven locally, from
  `LEA`/`MOVEA` of a constant address, `LEA d16(Am)` of a known `Am`, or `ADDA`/`SUBA`/`ADDQ`/`SUBQ` of an immediate.
- **Store completeness, relative to the challenger's discovered set `D`.**
  - A conservative store description covered every instruction in `D`: memory destinations, `MOVEM`/`MOVEP` spans,
    implicit `JSR`/`BSR`/`PEA`/`LINK` pushes, and synchronous exception and interrupt frames. An undescribed writer
    poisoned the location.
  - A store whose destination resolved to other bytes was excluded. Resolved meant absolute, or an exact local
    `An`/`Xn`, with the RAM mirror normalized.
  - A store whose destination could not be excluded poisoned `S` unless its stored value was exact. This covered
    an unknown `An`, `A7` stack operands and pushes, and exception frames.
  - The initial value was the project runtime's zero reset work RAM. Real hardware leaves work RAM undefined, so
    this was a stated model assumption.
  - Stored values came only from a tiny exact vocabulary: immediate and `CLR`, exact register values from the T002
    domain, a copy from another proven source, and a byte `ADD`/`SUB`/`AND`/`OR`/`EOR` read-modify-write.
  - Self-updates iterated a least fixed point bounded at 16 iterations. Mutual source cycles stayed unresolved.
- **Anti-circularity.** Domains were recomputed at every recovery step against the grown `D`. A store-derived proof
  that lost a target was invalidated through the T002 restart, with the site pinned, so stale targets left `D`.
- **Measurement variants (unsound, attribution only).** `exclude-stack` ignored stack and frame stores.
  `exclude-unresolved` ignored every store whose destination was not resolved.
- **Validator finding (fixed on the branch before the decision).** A destination based on the same `An` that the
  source operand steps (for example `MOVE.B (A1)+,(A1)`) had been placed at `An`'s prior value, which silently
  excluded a real writer. The fix made such destinations unresolved. Fixtures pinned this case, predecrement spans
  and big-endian byte offsets. The Sonic outputs were unaffected.
- **Scope of any T003 proof.** It would have been local only: under the stores in the current `D` and the stated
  exclusions, the location takes exactly these values. It never established global completeness. Runtime coverage
  could falsify such a proof but never confirm it, and runtime coverage was never an input.

## Workload and validation

- **Oracle.** The unchanged SEG-026-T001/T002 oracle: pinned authorized Sonic 1 image, no input, 23,200 no-render
  frames.
  - Re-run at the experiment head, it reproduced T002 exactly: coverage digest `b41bbfcc…578c`, final-state digest
    `01eab827…d947`, O = 10,512.
  - Challenger outputs were byte-identical across runs.
- **Independent adversarial validator: DELIVERY: PASS.**
  - Full gate 200/200.
  - Every Sonic aggregate reproduced.
  - STOP judged supported. Its one soundness finding and two test gaps were corrected on the branch.
  - The corrected experiment head also passed the full gate, 200/200.

## Results (Sonic 1; sanitized aggregates)

**Where the width-only indices come from.**

| population | absolute RAM byte | register-relative field | auto-update pointer |
| --- | --- | --- | --- |
| challenger-reachable width-only sites (9) | **8** | 0 | 1 |
| observed width-only sites (70; classification only) | 10 (7 in `D`) | **59** (all outside `D`) | 1 |
| missing PCs with a width-only dispatch on their structural chain (5,681 of 6,019 `O − D`) | 425 | **5,070** | 186 |

- None of the 59 register-relative sources had a base register with a locally exact value on the observed
  fixed-flow graph. That these are object fields is an interpretation; the measured fact is only that the base is
  not locally provable.
- For 5,367 of the chained PCs, the structural first gate is `JSR (An)` anyway.

**Store domains.**
- **Strict:** 6 challenger-reachable state locations; 0 resolved, 6 `alias_poison`. Every location was poisoned by
  stack, exception-frame, unknown-base register-relative, indexed and auto-update stores.
- **Store operations in `D`:** 2,183.

  | class | stores |
  | --- | --- |
  | exact-address | 945 |
  | stack | 531 |
  | exception frame | 37 |
  | unknown-base register-relative | 495 |
  | indexed | 8 |
  | auto-update | 167 |

- **`exclude-stack`:** still 6 of 6 poisoned, now by unknown-base object and pointer stores.
- **`exclude-unresolved` (unsound ceiling):**
  - 2 of 6 locations resolved (1 and 12 values);
  - 3 had an exact-address writer storing a non-exact value;
  - 1 was an unguarded increment (unbounded);
  - one store-derived proof was invalidated by a writer that its own target exposed (1 restart);
  - net gain: 0 PCs.

| measure | T002 strict | T003 strict | T003 `exclude-unresolved` (unsound) |
| --- | --- | --- | --- |
| `U` | 246,293 | 246,293 | 246,293 |
| `D` | 6,765 | 6,765 | 6,765 |
| `D/U` | 2.75% | 2.75% | 2.75% |
| `O ∩ D` | 4,493 | 4,493 | 4,493 |
| `O − D` | 6,019 | 6,019 | 6,019 |
| observed recall | 42.74% | 42.74% | 42.74% |
| width-only PC-index sites | 9 | 9 | 8 (+1 invalidated) |
| targets recovered through store domains | — | 0 | 0 net |
| recovery escapes | 0 | 0 | 0 |
| overlapping starts / rejected / exception-raising decodes | 70 / 1 / 31 | 70 / 1 / 31 | 70 / 1 / 31 |

**Structural attribution (unchanged from T002).**
- First gate: `JSR (An)` 5,367, `JMP (d8,PC,Xn)` 616, `JSR (d8,PC,Xn)` 25, `JMP (An)` 11.
- Nearest mechanism: PC-indexed 5,622 (width-only 3,722), `(An)` 397.

**Supplementary (static only; Sonic 2 has no attract oracle).**
- 9 width-only sites, all absolute-RAM sources: 7 of 7 locations poisoned under strict.
- The unsound ceiling invalidated 5 store-derived proofs.

## Decision

**STOP** for the reachability-first discovery direction. Three of the stated STOP criteria hold.

1. **Proving the state sources requires general pointer/alias analysis.** Every reachable state byte is poisoned by
   stores that only pointer/alias analysis could exclude: object pointers, stream pointers and the stack pointer.
   Excluding the stack does not help.
2. **Object-relative fields dominate, and their base provenance cannot be established locally.**
   - 59 of 70 observed width-only sites read a register-relative field.
   - 84.2% of `O − D` has one on its structural chain.
   - Going further would need object-identity analysis, which is interprocedural pointer analysis.
3. **Simple exact-store analysis leaves the important sites unresolved.**
   - Strict resolves 0 of 9.
   - Even with every unresolved store unsoundly ignored, the net gain is 0 PCs. Store-derived proofs were
     invalidated by the code they exposed.

Exact PC-indexed recovery (ADR 0054: 42.7% recall at `D/U` 2.75%) is therefore the measured ceiling of the local,
report-only approach. No successor experiment is proposed.

**Why the implementation was removed.** The STOP means nothing will consume the store-provenance machinery. Keeping
it as a dormant, off-by-default option would leave product API, CLI, report and test surface with no consumer. That
is the same simplicity-first rule applied to ADR 0051's rejected experiment. This record keeps enough detail to
avoid repeating the experiment.

## Limits of the measurement

- Z80, DMA and external bus-master writes into 68000 work RAM were not modelled. With 0 strict resolutions, this is
  moot.
- A pinned (invalidated) site was not re-evaluated, so its source class was not attributed.
- Classification labels over observed PCs used only the observed fixed-flow graph (ADR 0054) and never entered `D`.
  No store analysis ran over observed code, because that would have made runtime coverage a store source.

## Revisit when

- A production proposal introduces a sound whole-program memory/alias model or object-identity analysis for
  another reason. Store-domain recovery could then be re-measured on this oracle, following the design recorded
  above.
- Nothing here authorizes selective discovery or admission.
