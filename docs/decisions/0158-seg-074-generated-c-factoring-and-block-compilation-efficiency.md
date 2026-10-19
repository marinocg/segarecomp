# ADR 0158: SEG-074-T002 generated-C factoring, block compilation efficiency and decision

- Status: **Section 1 (pre-registration) frozen** - committed and pushed before any emitter change. Sections 2 onward (results and the
  final decision) are appended in later commits; section 1 is not edited afterwards except for typos that do not change meaning.
- Task: SEG-074-T002, the single consolidated implementation issue after full refinement (ADR 0157 selected **G2**). One product branch, one PR.
- Scope: generated-C representation and compilation efficiency of the existing Genesis M68K generated-native path. No change to admission,
  the ML model/weights/budgets, the validator, discovery semantics, the runtime, Sega CD BIOS materialization, or the user-facing defaults
  (Compatibility = broad AOT; optimized = production M3 + B35). Any mechanism that ships is **opt-in**.
- Related: ADR 0045/0052/0083 (existing AOT helper factoring, own-PC helpers, direct entries), ADR 0156/0157 (oracle and cost floor; not reinterpreted).

## 1. Pre-registration

### 1.1 Build economics (ADR 0157, restated; not re-derived)

- Cold build CPU (plan + emit + compile + link, clang 18.1.3 `-std=c11 -Wall -Wextra -Werror -pedantic -O2`, 4 jobs, no object cache):
  broad AOT 22-1159 CPU-s per image (e.g. Sonic 1 274, Shining Force 1159); deployed M3 + B35 ("INC") 19-629 CPU-s.
- `rho = 0.247` (ADR 0156) is **an opportunity indicator, not a realizable saving**: it normalizes every hexadecimal and long decimal literal,
  and distinct operands are distinct semantics. ADR 0157 diagnostic 2: realizable only by operand-parameterized sharing of residual owner
  segments, one non-PC operand literal = 0.11-0.25 of generated C (lineage mean 0.16). Text-normalized equality is never a soundness argument.
- Precise-discovery `genesis_block_*` TUs took a lineage mean of 0.62-0.70 of the oracle-build CPU on D03 (Shining Force blocks: 20.5 MB of C
  in 463 functions, largest about 400 KB; top-5 TUs 85-100% of block cost).

### 1.2 Pre-implementation observation (descriptive; recorded before the change, not a result)

Inspection of one Shining Force incumbent emission, before any code change, shows that 19.9 of its 22.4 MB of block C is a single construct:
the retired-return validation of each `RTS` block spells the whole-program return-target set as a chain of about 8,650 `!=` comparisons
(`m68k_observed_return != UINT32_C(..) && ...`), repeated in each of 48 block functions. A 6 MB block TU compiles in about 76 CPU-s against
about 8 CPU-s for a 6 MB AOT TU. The AOT path already shares bodies, so it carries one copy.

### 1.3 Hypotheses

- **HA (operand-parameterized sharing).** Residual single-use AOT owner bodies that are identical except for exactly one *operand literal*
  (after the existing own-PC normalization) can be emitted as one `genesis_aot_shared_*` helper taking that literal as a typed parameter. This
  reduces generated C by at least 5% and total build CPU by at least 5% on lineage mean, without changing any guest-observable behaviour.
- **HB (block return-target membership).** Replacing the per-`RTS`-block inline return-target chain with a call to one program-wide, sorted,
  generated `genesis_return_target_member` table-lookup (identical set, identical stop) removes most precise-discovery block C and compile
  cost on images with large return-target sets (D03 first), without changing behaviour.
- Further block strategies (splitting oversized block functions, reusing AOT helpers inside blocks, TU partitioning) are tried **only if** a
  measurement after HB shows block compilation is still a hotspot; each needs its own measured justification and may be dropped.

### 1.4 Designs and soundness conditions (fixed in advance)

- **A** extends the existing generation-time exact-text grouping of `frontend.cpp` (a third step after exact grouping and own-PC grouping, applied
  only to bodies still single-use). It replaces exactly one `UINT8_C/UINT16_C/UINT32_C(<constant>)` token (never UINT64_C, never the
  provenance line) with an explicit parameter of the matching `uint8_t/uint16_t/uint32_t` type. A body containing `case `, `static ` or
  `switch (` is never rewritten. Every member of a helper is verified by exact restored-text equality (hash collisions fall back to inline), and
  each caller passes exactly the literal that stood there. By C11 7.20.4, `UINTn_C(c)` has the integer-promoted type of `uint_leastn_t`; a
  parameter of type `uintn_t` promotes identically in every use. No independent postprocessor, no new CPU semantics, no runtime decoding.
- **B** adds one generated sorted `uint32_t` table of the existing `runtime_return_targets` and a function `genesis_return_target_member`
  (binary search) emitted once; the `RTS` guard text becomes `if (!genesis_return_target_member(m68k_observed_return)) return <same stop>`.
  The set, the stop class/diagnostic, the evaluation point and ordering relative to the pop are unchanged.
- Both are selected by explicit opt-in switches (default off), so the previous representation stays available for comparison and compatibility.

### 1.5 Workloads and toolchain

- Primary: ADR 0157's 11 gate images in 8 lineages (D01 Sonic 1/2/S&K; D02 Flicky; D03 Shining Force 1/2; D04 Phantasy Star II; D05, D06, D09 SGDK
  programs; D07 240p Test Suite), strategies **BROAD** (Compatibility) and **INC** (production M3 + B35 with its real fallback; D07 falls back).
  Variants: `BASE` (unchanged emitter), `A`, `B`, `AB`. Same host (16 cores, 15 GB), same clang 18.1.3 flags and 4 jobs, cold object cache,
  one image at a time, built from the same product base `8324582` plus this branch.
- The CLI used for `BASE` is the branch binary with both switches off. It is byte-identical to the base emitter's output (verified).
- Synthetic fixtures for each affected lowering and block shape; bounded executable fixtures for runtime timing.
- Private data stays in the ignored research locations; the public record is aggregate-only (`tools/segarecomp_oracle_bench.t002.json`).

### 1.6 Metrics

Generated C bytes; emitted native function/entry counts; cold compile CPU and wall; peak compiler RSS; object and executable sizes; link
CPU/wall; production-cache-assisted rebuild time; analysis + emission CPU/wall/RSS; bounded runtime throughput (explicit instruction budget,
median of 3); total end-to-end build CPU (plan + emit + compile + link). A source-size reduction is never reported as a compile gain without a
measured compile.

### 1.7 Thresholds (per variant against `BASE` of the same session, ratio = variant / BASE, lineage means over the 8 lineages)

Noise floor: BASE is built three times on Flicky and the D05 SGDK image; any effect smaller than twice the observed spread is noise.

| criterion | A adopted | B adopted |
|---|---|---|
| lineage-mean total build CPU, BROAD | <= 0.95 | <= 0.97 |
| lineage-mean total build CPU, INC (deployed, incl. fallback) | <= 0.97 | <= 0.97 |
| best-lineage total build CPU (BROAD or INC) | - | <= 0.85 (the D03 hotspot) |
| any single lineage ratio, either strategy | <= 1.03 | <= 1.03 |
| any single image ratio | <= 1.05 | <= 1.05 |
| generated C bytes, lineage mean | <= 0.95 | no bound (reported) |
| peak compiler RSS, lineage mean | <= 1.10 | <= 1.10 |
| bounded runtime throughput | >= 0.97 | >= 0.97 |
| emission CPU overhead | included in total CPU | included in total CPU |

The combined variant `AB` is accepted only if both pass individually and `AB` total CPU <= min(A, B) + 0.02 per lineage mean (no
double-counting assumption). A pass obtained only through D03 (or any single lineage) is not a pass: the lineage-mean condition and the
no-regression conditions must all hold. A candidate that lowers generated C but raises total CPU is rejected.

### 1.8 Correctness requirements (all must pass; any failure is `G2_CORRECTNESS_FAILED` and nothing is integrated)

- Synthetic differential: for every compiled entry of fixtures with PC-relative reads, immediate/absolute operands of byte/word/long width,
  predecrement/postincrement and register aliasing, arithmetic flags and condition branches, calls/returns/dispatch continuation, exceptions
  and TRAP/ILLEGAL boundaries, legal overlapping interpretations, mutable memory and device-visible ordering, executable-image byte
  verification, and unexpected return/indirect targets that must fail closed: the opt-in output and the baseline output agree on registers, SR,
  USP, PC/transfer, work RAM, typed stop class and full provenance.
- Qualified images: bounded generated-native execution (120 frames, explicit instruction budget and wall limit) agrees with the baseline on
  typed outcome, frames, final-state digest, coverage digest and frame-stream digest, and executes no PC the baseline does not. Short traces
  never claim full-game equivalence.
- Determinism: two emissions are byte-identical. Strict C11 `-Wall -Wextra -Werror -pedantic` (clang and gcc) on all synthetic and gate builds.
- Existing suites touching the AOT factoring and direct-entry representation still pass.

### 1.9 Stop conditions and adoption

- Stop an optimization (remove or leave off, with a supported negative finding) when a correctness gate fails, or its pre-registered thresholds
  are not met after the full measurement; do not tune thresholds afterwards.
- Stop everything with `G2_EVIDENCE_INSUFFICIENT` if fewer than 8 lineages can be measured for a variant, or the BASE spread exceeds the effect.
- Decision classes: `G2_EMITTER_OPTIMIZATION_ACCEPTED` (A only), `G2_BLOCK_OPTIMIZATION_ACCEPTED` (B only), `G2_COMBINED_OPTIMIZATION_ACCEPTED`,
  `G2_ECONOMICS_NOT_JUSTIFIED`, `G2_CORRECTNESS_FAILED`, `G2_EVIDENCE_INSUFFICIENT`.
- Adoption means an **opt-in switch that stays off by default**. Making an optimization the default needs separate, independent evidence of
  semantic equivalence beyond this task, and is not done here. An optimization that fails its thresholds is removed from the branch.

### 1.10 Future-research note (no task created; needs a separate operator decision)

Distinct opportunities, each needing its own measurable hypothesis and independent validation: instruction-start recognition; graph-based
control-flow evidence; ROM-to-RAM copy provenance; compressed executable-image discovery; build-time materialization and batched admission.
A Transformer or GNN is not trained here: it does not address the measured duplicate-body and block-chain costs.
