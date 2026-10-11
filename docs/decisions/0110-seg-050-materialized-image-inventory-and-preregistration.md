# ADR 0110: SEG-050 materialized-image inventory, corpus screening and economic pre-registration

- Status: **pre-registration (checkpoint C1a)**. Sections 1-4 are frozen BEFORE any screening or economic result is interpreted; results are appended
  below in later commits (section 5 onward) without editing sections 1-4.
- Scope: SEG-050-T001 (inventory, corpus, baseline). Product behaviour is unchanged by this checkpoint (research tool + documentation).
- Delivery format: SEG-050-T001..T006 are delivered as one product branch / PR with sequential checkpoint commits (precedent: SEG-049 ADR 0108/0109).
  "Merged before dependent task" in the task records is reconciled as "committed, independently reviewed and validated checkpoint on the shared
  branch"; the whole PR is merged only after the final gate.

## 1. Disclosure of what was seen before freezing

One smoke run of the shipped Compatibility build of the Flicky image was executed to learn the tool interface before this ADR was written. It exposed
aggregate counts only (one ADR 0049 alias, fourteen RAM thunks, termination `route_advanced`, alias-bearing emission universe larger than the
alias-less one). The thresholds below are generic (not derived from that title's numbers) and apply uniformly to every screened image, but the
reader should treat Flicky as "seen". No other image, no model result and no truth comparison had been looked at.

## 2. Corpus classes (pre-registered; applied to every locally available authorized Genesis image)

| class | definition | admissible for the supervised materialized-image experiment |
| --- | --- | --- |
| A | the shipped build proves an executable ROM-to-RAM copy: at least one ADR 0049 alias (`genesis.copy_alias`, `static_proof`, `byte_identity` guard) | candidate, only if instruction-start truth for the copied source span is independently supportable |
| B | a deterministic materialized/decompressed M68K executable image produced by an existing producer (verification `structural` or a transform with verified output bytes) | candidate, same truth condition |
| C | work-RAM execution observed (RAM thunks or a work-RAM frontier) but no byte-identity provenance and no exact executable truth | diagnostic / acquisition target only |
| D | the bounded build-time preparation observed no materialized executable M68K image (`route_advanced` or `no_work_ram_frontier` with zero aliases and thunks) | never a negative example (the preparation is workload-bounded) |
| E | unsupported build, insufficient runtime evidence, timeout or tool failure | never a negative example |

Compressed graphics, sound data and ordinary RAM data do not establish executable images. Only A/B qualify and only with independent truth; C/D/E
never become labels. Several images of one title, or two titles of one engine/source lineage, are ONE lineage.

## 3. Screening procedure (fixed)

For every `games/*.bin|*.md` Mega Drive image (SMS excluded: M68K only): `tools/seg050_materialized_screen.py --screen-only` over the shipped
`segarecomp build` route, Compatibility, `--optimize 0`, gcc, one sample, stopped right after the `m68k-alias-prepare` stage. The class is a pure
function of the preparation aggregates (`aliases`, `ram_thunks`, `termination`) and the build result. No title-specific rule. The preparation
is bounded by ADR 0049/0076 (boot/attract path, at most 64 rounds); D therefore means "none observed", not "none exist".

Decompression arm: whether an M68K deterministic-decompression executable-image producer exists is decided by code inspection of the producers
named in ADR 0077 (`genesis.cartridge`, `genesis.copy_alias`, `genesis.ram_thunk`, `genesis.z80_materializer`). If none exists, the decompression arm
is `NO_DECOMPRESSION_PRODUCER`; no decompressor or discovery system is built to supply it.

## 4. Economic thresholds (fixed before interpretation)

Definitions (all from the shipped `segarecomp build` of one image, one sample, gcc -O1, `--jobs 8`):

- `U0` = broad universe of the alias-less (first) emission; `U1` = broad universe of the final emission. Materialized-image share `M = (U1 - U0) / U1`.
- Fallback event (Problem A): an `optimized` request whose first emission is ML-admitted and a later alias/RAM-thunk emission is rejected by the
  structural stage with `materialized_image_not_admitted` (ADR 0104 finding), so the build is the broad program.
- Lost saving `L = 1 - K0/U0` where `K0` is the first emission's admitted count (fraction of whole-ROM broad compile an admitted build would save).

Thresholds:

1. **T002 is promoted iff** at least one screened authorized image shows a fallback event with `L >= 0.30` (the lost saving is at least 30 % of the
   broad ROM compile) under the experimental gen2 opt-in, OR a synthetic alias fixture proves that fallback independently of any commercial title.
2. **Materialized images are an important compile-cost problem for conclusion 2 (ML on the images themselves) iff** `M >= 0.05` (5 % of the final broad
   universe) in at least TWO independent lineages, and the images are not trivially covered by an exact source mapping. Otherwise T001 records
   `NOT_ECONOMICALLY_IMPORTANT` for conclusion 2.
3. Because the operator amendment makes the materialized-image ML question a mandatory research deliverable, a `NOT_ECONOMICALLY_IMPORTANT`
   finding does NOT silently cancel T003/T004: T003 still pre-registers the experiment and T004 still judges it, and the economics finding is
   carried into T004's `MATERIALIZED_ML_ECONOMICS_INSUFFICIENT` gate. A `CORPUS_BLOCKER` (insufficient lineages/truth) remains a valid terminal outcome.
4. Truth availability per candidate: `exact_copy_truth` (ROM source-backed instruction starts cover the alias source span; the copied-image starts are
   derived by byte identity and a constant delta), `no_truth`, or `dynamic` (computed/self-modifying). Observed runtime PC coverage is never truth.

T001 classification vocabulary (backlog): `CORPUS_SUFFICIENT_FOR_SUPERVISED_EXPERIMENT`, `CORPUS_SCARCE_COPY_ONLY`, `NO_DECOMPRESSION_PRODUCER`,
`NOT_ECONOMICALLY_IMPORTANT`, `CORPUS_BLOCKER`. Several may be recorded (they are not mutually exclusive); the stop paths of the backlog apply.
Contamination rule: the exact-source clones kept for SEG-053 candidate evaluation (titles outside the SEG-048/049 five-title corpus) are not
consumed, evaluated or used for training in SEG-050.
