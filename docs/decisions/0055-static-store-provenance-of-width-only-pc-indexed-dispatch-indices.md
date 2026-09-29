# ADR 0055: Static Store Provenance of Width-Only PC-Indexed Dispatch Indices (Experiment)

- Status: Accepted (decision: **STOP** for the reachability-first discovery direction; broad Gen-2 immutable-ROM AOT
  stays the production representation)
- Date: 2026-09-29
- Task: SEG-026-T003
- Related, unchanged: ADR 0054 (the exact PC-indexed recovery this experiment extends), ADR 0053 (the challenger and
  the attract-mode coverage oracle), ADR 0051 (operand-width regions snowball), ADR 0002 (static translation, fail
  closed).

## Question

Can the finite values of mutable-state dispatch indices be proven from statically discoverable stores, without
general pointer/alias analysis, interprocedural value-set analysis or symbolic execution?

After ADR 0054, 94.4% of the missing observed Sonic attract execution (`O − D`) lies behind at least one
`JMP/JSR (d8,PC,Xn)` whose index is a byte loaded from mutable memory with no explicit bound (`width_only_domain`).
This experiment asks whether such a byte `S` can be given an exact store domain `Values(S)` whose completeness is
itself locally provable, and whether that unlocks the remaining execution.

Everything below is report-only. Admission, generated C, emission, compiled-entry membership, runtime opcode
behaviour and runtime execution authority are unchanged. Runtime coverage is validation only: it never supplies
index values, stores, aliases, targets or roots.

## Mechanism (retained, off by default)

`--pc-index-store-provenance classify|prove` (requires `--pc-index-recovery`).

- `classify` records the generic addressing class of every mutable byte index source a proof reads. Discovery is
  identical to T002.
- `prove` also attempts an exact store domain for the source.

Source classes:
- absolute RAM byte: `(xxx).W` / `(xxx).L`;
- register-relative field: `(An)` / `d16(An)`;
- indexed field: `(d8,An,Xn)`;
- auto-update pointer: `(An)+` / `-(An)`;
- other.

A register-relative source names a location only when its base `An` has an exact value proven locally by the same
backward evaluation. That covers `LEA`/`MOVEA` of an absolute, PC-relative or immediate address, `LEA d16(Am)` of
a known `Am`, and `ADDA`/`SUBA`/`ADDQ`/`SUBQ` of an immediate.

**Store completeness (the core proof obligation).** `Values(S)` is computed from exactly the challenger's current
discovered set `D`, at every recovery step.

1. Every instruction in `D` is described by a conservative CPU-owned store model (`libs/cpu/m68k`
   `m68k_memory_stores`). It covers:
   - the destination of every memory-writing form;
   - `MOVEM` register→memory spans;
   - `MOVEP` spans;
   - implicit `JSR`/`BSR`/`PEA`/`LINK` pushes;
   - the exception frames of `TRAP`/`TRAPV`/`CHK`/divide/illegal-style instructions.

   An interrupt entry stacks a frame whenever an interrupt vector root exists. An operation the model does not
   describe poisons.
2. A store whose destination resolves is excluded when it names only other bytes. Resolved means absolute, or
   `An`/`Xn` with exact local values; the RAM mirror is normalized.
3. A store that may cover `S` contributes its exact stored value set.
4. A store whose destination cannot be excluded may cover every RAM byte. This includes an unknown `An`, `A7`
   stack operands and pushes, and exception frames. If its value is not an exact finite set, `S` is
   `alias_poison`.
5. The initial value is the machine model's reset work RAM (zero, the project runtime's `{0}`-initialised
   state). Real hardware leaves work RAM undefined, so this is a stated model assumption. The report counts
   domains where 0 is present only through it.
6. Supported stored values are deliberately few:
   - immediate and `CLR`;
   - `MOVEQ`/register-derived exact values from the existing T002 domain;
   - a copy from another exactly proven state source;
   - byte read-modify-write `ADD`/`SUB`/`AND`/`OR`/`EOR` with an immediate or exact register;
   - the constant address pushed by `JSR`/`BSR`/`PEA`.
7. A self-update iterates a least fixed point from the reset value, bounded at 16 iterations
   (`unbounded_update`). A mutual dependence between two sources is `source_cycle`.

**Feeding back.** An accepted domain replaces the width rule for that byte (proof bit `store_domain`). The T002
pipeline is unchanged: index transform, exact immutable table reads, exact targets, ordinary closure. It is still
strict (no width fallback), fails closed on out-of-image entries or targets, and keeps its invalidation restart.

**Anti-circularity.** Domains are recomputed at every recovery step against the grown `D`. A site whose proof used
a store domain and later loses its proof or any target is invalidated exactly as in ADR 0054. Discovery restarts
from scratch with that site pinned, so stale targets leave `D`. The pinned set only grows, so this terminates. The
report counts `store_domain_invalidations`.

**Measurement variants (never proofs).** `--store-alias-policy exclude-stack` ignores stack and exception-frame
stores. `exclude-unresolved` ignores every store with an unresolved destination. Both are unsound. They exist only
to attribute what blocks a strict proof.

**Correction to the shared domain.** A register query narrower than a memory load's access size (for example a
`.W` index loaded by `MOVE.L` from an immutable table) previously read the high bytes of the big-endian load
instead of the low ones. `read_source` now reads at the operation's own size and then restricts. A fixture covers
it. The Sonic 1 T002 outputs are byte-identical, so the defect never fired on the measured workload.

## Local soundness vs global completeness

A T003 proof would establish only this: under the stores in the challenger's current `D`, and under the stated
exclusions, the location takes exactly these values. It does not establish that no undiscovered executable path
anywhere in the image writes another value. Runtime coverage can falsify such a proof (an observed target outside
the recovered set), but runtime non-observation never proves global completeness. Nothing here is promoted to
production admission.

## Workload

- The unchanged SEG-026-T001/T002 oracle: pinned authorized Sonic 1 image, no input, 23,200 no-render frames.
- Re-run at this task's head, the oracle reproduced T002 exactly (see the task evidence for digests).
- The challenger outputs are byte-identical across runs. With store provenance off or `classify`, `D` and every
  T002 site outcome are identical to T002.

## Results (Sonic 1; sanitized aggregates)

### Width-only sources are classified first

| population | absolute RAM byte | register-relative field | auto-update pointer |
| --- | --- | --- | --- |
| challenger-reachable width-only sites (9) | **8** | 0 | 1 |
| observed width-only sites (70; classification only) | 10 (7 in `D`) | **59** (all outside `D`) | 1 |
| missing PCs behind width-only dispatch (5,681 of 6,019 `O − D`) | 425 | **5,070** | 186 |

- The register-relative sources are all object fields: for all 59, the base register has no locally exact value on
  the observed fixed-flow graph.
- On the challenger's own frontier the problem looks favourable: a handful of absolute state bytes. Across the
  execution still missing, it is not. **84.2% of `O − D` lies behind object-relative mutable fields.**

### Exact store domains (strict)

The 8 absolute-RAM width-only sites read 6 distinct work-RAM state locations.

| measure | value |
| --- | --- |
| state locations attempted | 6 |
| resolved | **0** |
| `alias_poison` | 6 (each poisoned by stack, exception-frame, unknown-base `(An)`/`d16(An)`, indexed and auto-update stores) |
| store operations in `D` | 2,183 |

Store operations in `D`, by class (non-exact-value stores in brackets):

| class | stores | non-exact value |
| --- | --- | --- |
| exact-address | 945 | 337 |
| stack | 531 | 32 |
| exception frame | 37 | 37 |
| unknown-base register-relative | 495 | 387 |
| indexed | 8 | 7 |
| auto-update | 167 | 152 |

Attribution by variant (unsound, measurement only):

- **`exclude-stack`.** All 6 locations stay poisoned, by unknown-base object and pointer stores.
- **`exclude-unresolved`** (every unresolved-address store ignored).
  - 2 of 6 locations resolve (1 and 12 values).
  - 3 have an exact-address writer storing a non-exact register or word value.
  - 1 is an unguarded increment (`unbounded_update`).
  - One site resolved from a store domain. Its only target exposed a new self-update writer, so the domain became
    unbounded and the site was invalidated (1 restart). The final `D` is unchanged.

### T002 vs T003

| measure | T002 strict | T003 strict | T003 `exclude-unresolved` (unsound) |
| --- | --- | --- | --- |
| `U` | 246,293 | 246,293 | 246,293 |
| `D` | 6,765 | 6,765 | 6,765 |
| `D/U` | 2.75% | 2.75% | 2.75% |
| `O ∩ D` | 4,493 | 4,493 | 4,493 |
| `O − D` | 6,019 | 6,019 | 6,019 |
| observed recall | 42.74% | 42.74% | 42.74% |
| PC-index width-only sites | 9 | 9 | 8 (+1 invalidated) |
| targets recovered through store domains | — | 0 | 0 net |
| store-domain invalidations / restarts | — | 0 / 0 | 1 / 1 |
| recovery escapes | 0 | 0 | 0 |
| overlapping starts / rejected / exception-raising decodes | 70 / 1 / 31 | 70 / 1 / 31 | 70 / 1 / 31 |

**Structural attribution after T003 (unchanged from T002).**
- First gate: `JSR (An)` 5,367, `JMP (d8,PC,Xn)` 616, `JSR (d8,PC,Xn)` 25, `JMP (An)` 11.
- Nearest mechanism:
  - PC-indexed 5,622 (width-only 3,722);
  - `(An)` 397.
- Missing PCs behind no width-only dispatch: 338. Of these, 308 are behind only `(An)` control.

**Supplementary (static only; Sonic 2 has no attract oracle).**
- 9 width-only sites, all absolute-RAM sources, reading 7 locations: all 7 are `alias_poison` under strict.
- Under the unsound `exclude-unresolved` variant, 5 store-derived proofs are invalidated when their targets expose
  new writers.

## Decision

**STOP** for the reachability-first discovery direction. This experiment crosses the complexity boundary, and
three of the stated STOP criteria hold:

1. **Proving the state sources requires general pointer/alias analysis.**
   - Every one of the 6 challenger-reachable state bytes is poisoned under strict rules. The poisoning stores have
     destinations that only pointer/alias analysis could exclude: object pointers, stream pointers and the stack
     pointer.
   - Excluding the stack is not enough.
2. **Object-relative fields dominate, and their base provenance cannot be established locally.**
   - 59 of 70 observed width-only sites are object fields, and 84.2% of `O − D` lies behind them.
   - None has a locally exact base. The next step would be object/heap identity, which is interprocedural pointer
     analysis.
3. **The important width-only sites stay unresolved after simple exact-store analysis.**
   - Strict: 0 of 9 resolve.
   - Even with every unresolved store unsoundly ignored, the net gain is 0 PCs. Exact-address writers store
     non-exact values, updates are unbounded, and store-derived proofs are invalidated by the code they expose (5
     invalidations on Sonic 2).

Useful store completeness therefore required general alias/VSA machinery, which this experiment was forbidden to
build, correctly. Exact PC-indexed recovery (ADR 0054) remains the measured ceiling of the local, report-only
approach: 42.7% recall at `D/U` 2.75%. No successor experiment is proposed for this direction.

## Limits

- Proofs are relative to `D` (see above). The store model is conservative (an undescribed operation poisons), and
  it is experiment-scoped: it is not an effect-owner contract.
- Z80 or DMA writes into 68000 work RAM are not modelled, and neither are external bus masters. With the strict
  result at zero resolutions this is moot.
- A may-alias store with an exact value is not poison: it contributes its value bytes at every offset.
- Classification labels over observed PCs use only the observed fixed-flow graph (ADR 0054) and never enter `D`.
  No store analysis is run over observed code, since that would make runtime coverage a store source.

## Revisit when

- A production proposal introduces a sound whole-program memory/alias model, or object-identity analysis, for
  another reason. Store-domain recovery could then be re-measured on this oracle with the retained mechanism.
- Nothing here authorizes selective discovery or admission.
