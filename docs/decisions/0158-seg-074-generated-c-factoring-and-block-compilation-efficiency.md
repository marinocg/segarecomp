# ADR 0158: SEG-074-T002 generated-C factoring, block compilation efficiency and decision

- Status: Accepted. **Decision: `G2_COMBINED_OPTIMIZATION_ACCEPTED`, as two opt-in switches that stay off by default** (section 5). Section 1 (pre-registration) was frozen: committed and pushed before any emitter change. Sections 2 onward (results and the
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

## 2. What was implemented

Two emitter switches, both off by default (default output is byte-identical to the previous emitter; verified on three gate images with and
without the switches against the build that was benchmarked). Nothing else changed: admission, ML model/weights/budgets, validator, discovery,
runtime, launchers, Sega CD materialization and the Compatibility/optimized defaults are untouched.

- **A, `--aot-operand-sharing`** (`libs/codegen/c11/src/frontend.cpp`, "group D"). After the existing exact (ADR 0045) and own-PC (ADR 0052)
  groupings, a body that is still single-use is grouped with others identical except for exactly one `UINT8_C/UINT16_C/UINT32_C(n)` constant,
  found by a skeleton hash, then a per-hole hash. The helper takes that constant as a `uint8_t/uint16_t/uint32_t` parameter
  (`genesis_aot_operand`). Safeguards:
  - each member is verified by exact restored-text equality (a hash collision can only cost sharing);
  - a hole must be a plain constant that fits its width, not an array size or subscript, and the body must contain no `case`, `static`,
    `switch (` or provenance compound literal;
  - an operand helper is reached from its owner `case` and is never a direct-entry table row, so the ADR 0083 tables are unchanged;
  - group order is a total order (size, then first member), independent of hash-map iteration.
- **B, `--shared-return-targets`**. Each RTS (ordinary block and AOT body) tested the popped return against an inline `!=` chain of the whole
  program return-target set; a precise-discovery RTS block therefore carried a copy of the set (about 8,650 comparisons, ~417 KB of C, in each
  of 48 Shining Force blocks: 19.9 of the 22.4 MB of block C). It now tests `!genesis_return_target_member(popped)`: one generated sorted table
  and a binary search, emitted once. Same set, same typed stop, same evaluation point. The synthetic completion return keeps its inline chain.
- Plumbing: `GeneratedCSharedReturnTargetsScope` / `ImmutableRomAotOperandSharingScope`, CLI `--shared-return-targets` /
  `--aot-operand-sharing`, `genesis_startup_bridge.py --emitter-flag=<switch>`, and `segarecomp_oracle_bench.py` `--emit-flag`, bounded
  runtime-throughput measurement and `t002-aggregate` (T001 tool reused).
- Not implemented (stopped by the pre-registered order): splitting oversized block functions, reusing AOT helpers inside blocks, TU
  partitioning. After B the largest block TU compiles in under a second (76 CPU-s before, Shining Force), so block compilation is no longer a
  hotspot and none of those strategies has a measured justification.

## 3. Correctness evidence

- `tests/genesis_generated_c_compaction_differential_test.py` (clang and gcc, strict C11 `-Werror`, sharded direct-entry and single-file
  forms, -O0 and an -O2 rebuild): the fixture emitted with each switch combination enters every compiled entry under 5 architectural scenarios
  plus a probe of the return-target set (every member, both neighbours, 0, 0xFFFFFFFF, out-of-range) on every entry that can execute an RTS, in
  single step and bounded run. Registers, SR, USP, PC/transfer, work RAM, typed stop class, full provenance, checkpoint and whole-runtime digests
  are identical to the reference. Mutants are detected: wrong operand argument (all sites and per helper), dropped first, median and last table
  member, accept-all membership. Determinism: repeated emission is byte-identical.
- Existing suites touching the AOT factoring, direct entries and the startup bridge pass unchanged
  (`genesis_immutable_rom_aot_body_factoring_differential_test`, owner, bridge tests, oracle bench tests).
- Gate images: for each of 11 images x BROAD/INC x {A, B, AB} (66 builds) the bounded 120-frame run has the same typed outcome, frames,
  final-state, coverage and frame-stream digests as BASE (66/66, and 22/22 per variant), the 60M-instruction bounded runs of the optimized
  `-O2` executables print the same output digest (22/22 per variant, deterministic over 5 runs), admission lines and compiled-entry counts are
  identical. This is short-trace equivalence, not full-game equivalence, and not static recall.

## 4. Measured results (`tools/segarecomp_oracle_bench.t002.json`; clang 18.1.3 `-O2` strict, 4 jobs, cold cache, 11 images / 8 lineages)

Ratios are variant / BASE total build CPU (plan + emit + compile + link), mean over the images of a lineage, then over the 8 lineages. INC is
the deployed incumbent including its real fallback (D07 falls back to broad in every variant, as in ADR 0157).

| variant | strategy | CPU mean | D01 | D02 | D03 | D04 | D05 | D06 | D07 | D09 | generated C | peak compiler RSS | bounded throughput |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A | BROAD | **0.891** | .877 | .879 | .889 | .886 | .896 | .909 | .865 | .928 | .883 | .975 | 1.018 |
| A | INC | **0.900** | .869 | .912 | .889 | .895 | .937 | .895 | .886 | .914 | .873 | .973 | 1.005 |
| B | BROAD | **0.899** | .957 | .916 | **.708** | .995 | .979 | .998 | **.629** | 1.012 | .981 | .929 | 1.016 |
| B | INC | **0.883** | .893 | 1.006 | **.609** | .967 | .981 | .978 | **.654** | .980 | .974 | .918 | 1.002 |
| AB | BROAD | **0.793** | .827 | .833 | .601 | .862 | .915 | .873 | .519 | .914 | .864 | .948 | 1.016 |
| AB | INC | **0.780** | .767 | .904 | .514 | .838 | .875 | .902 | .528 | .909 | .847 | .943 | 1.014 |

Totals over the 11 images (CPU-s): BROAD 4262 -> A 3759 / B 3494 / AB 2982 (-30%); INC 2051 -> A 1814 / B 1484 / AB 1261 (-39%). Wall time
follows (lineage-mean total wall 0.90 A, 0.90 B for BROAD; AB 0.81 BROAD, 0.80 INC). Executable size AB 0.91 / 0.89 of BASE. Emission CPU
rises 12% under A (0.7 to 1.1 s on the large images; already counted in the totals). The cache-assisted warm rebuild is unchanged (0.96-0.99).
Per-image extremes (variant / BASE, either strategy): A 0.847-0.937 (all 22 pairs below 1); B 0.482 (Shining Force INC) to 1.012 (D09 BROAD, the
only pair above 1); AB 0.402 (Shining Force INC) to 0.915 (D05 BROAD). The largest image ratio is 1.012, below the registered 1.05.

### Pre-registered thresholds (section 1.7)

All pass: A (BROAD 0.891 <= 0.95, INC 0.900 <= 0.97, generated C .883/.873 <= 0.95, RSS <= 1.10, throughput >= 0.97, lineage max <= 1.03,
image max <= 1.05); B (0.899 <= 0.97, 0.883 <= 0.97, best lineage 0.609 <= 0.85, other bounds); AB (0.793 / 0.780, each <= min(A, B) + 0.02, i.e. A
and B are close to additive). The combined result is not an artefact of one workload: D03 and D07 carry the B gain, but A is 0.85-0.94 on every
image and AB passes on all 8 lineages.

### Noise and limits of the evidence

- BASE repeats (3 builds each): total-CPU spread (max-min)/mean is 7.1% (D05 BROAD), 7.3% (Flicky BROAD), 2.0% (D05 INC), 3.8% (Flicky INC).
  For the two smallest images, A's effect (about 10%) is below twice that spread, so by the registered rule the **per-image A effect on small
  images is noise-level**. The decision statistic is the lineage mean over 22 image-strategy pairs, where A is lower than BASE in all 22 and
  corroborated by deterministic quantities (generated C -12%/-13%, executable size -5%, object size -2%); but a single small image does not
  demonstrate A on its own.
- Single host, single compiler (clang 18.1.3 `-O2`); CPU time, wall descriptive. The pre-existing short broad reference (Flicky 26 frames, S&K 9,
  D05 9, D07 2) bounds the runtime evidence on those images to the prefix.
- Throughput ratios are within noise of 1.0 (A does not slow the bounded runs, as expected: the helpers are ordinary calls).
- Not measured: D10-D12 (typed broad failure), Sega CD (unchanged), gcc build economics (only gcc strict compilation of the fixtures),
  non-Linux hosts.

## 5. Decision

**`G2_COMBINED_OPTIMIZATION_ACCEPTED`**, integrated as two **opt-in** switches (`--aot-operand-sharing`, `--shared-return-targets`) that stay
off by default. Both individually and together meet every pre-registered economic and correctness condition: a lineage-mean total build CPU
reduction of 10-11% for operand sharing (all 8 lineages), 10-12% for the shared return-target table (60%+ on the large precise-discovery
lineages D03 and D07), and 21-22% combined, with identical observable behaviour on the synthetic differential, the 11-image bounded runs and the
bounded executable runs.

- Making either switch the default (a transparent internal replacement, which would not need a user-visible mode) is **not** done here: it
  needs independent evidence beyond a 120-frame / 60M-instruction trace per image and a non-Linux/gcc economics check. Compatibility (broad AOT)
  and the optimized (M3 + B35) defaults, ML weights, budgets and admission are unchanged.
- The previous representation remains the default and the comparison baseline.

## 6. Remaining platform opportunities (no task created)

- Operand sharing covers one constant per body; 2-4 parameters add about 3-6 points of the inline AOT bytes (measured on Flicky before the
  change), and the PC-derivable literals (`own PC +/- k`) are untouched.
- The AOT TUs still dominate compile CPU after B; the remaining lever is the number of distinct bodies, not block size.
- The research items of section 1.10 stand: instruction-start recognition, graph-based control-flow evidence, ROM-to-RAM copy provenance,
  compressed executable-image discovery, build-time materialization and batched admission. Deferred T003/T004 remain deferred; a new task
  needs a separate operator decision.
