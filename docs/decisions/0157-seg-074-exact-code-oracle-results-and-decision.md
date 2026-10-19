# ADR 0157: SEG-074-T001 exact-code oracle and compilation-cost floor - results and decision

- Status: Accepted. **Pre-registered gate (ADR 0156) result: G2 - region admission is already close to instruction-level cost; generated-C
  representation headroom is sufficient.** T002 is redirected toward generated-C factoring and compilation-pipeline efficiency. No new
  admission architecture is built.
- Scope: offline research only. The only code change is the report-only least-closed-superset producer (`--region-proposal-closure close`,
  default `prune` unchanged) plus the benchmark tool and tests. Production M3 + B35, Compatibility (broad AOT) as the default and fallback, the
  structural prune, the validator, the emitter, the runtime, the launchers and Sega CD admission are unchanged. Nothing was trained. SEG-072's
  `sealed` command was not run, and no SEG-072/073 artifact, label, marker or decision was changed.
- Tooling: `tools/segarecomp_oracle_bench.py` (`fixture`, `measure`, `aggregate`, `density`, `diag-repr`); tests
  `tests/segarecomp_oracle_bench_test.py` and `tests/genesis_region_prune_test.cpp` (closure section). Public aggregate record:
  `tools/segarecomp_oracle_bench.results.json`. It is byte-identical when regenerated from the private work files, and it holds no address,
  digest, per-start truth or generated commercial C.

## Setup

- **Handoff:** `segarecomp_ml_v3_handoff.py verify` returned `PASS`.
  - `MANIFEST.json` `e6244a6d…`;
  - 758/758 bundled files and 46/46 external inputs verified;
  - weights and reference inference bitwise;
  - 50 registry images.
- **Binaries and host:**
  - Release CLI built from this branch (product base `604c076` plus the closure producer);
  - clang 18.1.3, `-std=c11 -Wall -Wextra -Werror -pedantic -O2`, 4 concurrent compile jobs, object cache disabled for cold builds;
  - one image at a time on a 16-core, 15 GB host.
- **CPU accounting:** per-process user + system time from `wait4`.
- **Bounded runs:** 120 frames, instruction budget 12,000,000, `quick` profile, wall timeout.
- **Instrumentation:** ADR 0156 was committed in `5cb36dc` and the tooling in `b61a187`, both before any oracle measurement. The per-TU-family
  CPU breakdown and the non-gating diagnostics were added to the tool afterwards (the measurement ran on that working copy, committed in the
  results commit). They change no gate input.
- **Correction after independent review:** the first aggregation charged `INC`'s D07 fallback as broad (101.6 CPU-s). It is now charged
  its own measured build (102.9 CPU-s), which already contains the rejected attempt, as ADR 0156 requires. `h` moves from 0.3413 to
  0.3424; no gate condition changes.
- **Commands:**

```sh
python3 tools/segarecomp_oracle_bench.py measure --cli build/release/apps/segarecomp/segarecomp --cc clang --opt O2 --jobs 4 --frames 120 \
    --work build/seg074work --bundle <H>/.cache/seg072-handoff/bundle-v1
python3 tools/segarecomp_oracle_bench.py measure ... --images d05 d06 d07 d09 --strategies BROAD DIAG_INSTR_LABELLED --result-name diag.json
python3 tools/segarecomp_oracle_bench.py diag-repr --cli ... --cc clang --opt O2 --work build/seg074work --bundle ... --out <ignored>/diag_repr.json
python3 tools/segarecomp_oracle_bench.py density --bundle ... --out <ignored>/density.json
python3 tools/segarecomp_oracle_bench.py aggregate --work build/seg074work --out tools/segarecomp_oracle_bench.results.json \
    --diag-repr <ignored>/diag_repr.json --density <ignored>/density.json
```

## Measured results (11 gate images, 8 lineages)

Each cell is the total cold build in CPU seconds / generated C in MB:

- **Build CPU** = plan + emission (+ native inference for `INC`) + compile + link.
- **Deployed:** a rejected plan counts as the broad build plus the attempt.

| image (lineage) | BROAD | INC (deployed) | M3RB50 | ORACLE_REGION | ORACLE_INSTR |
|---|---|---|---|---|---|
| Sonic 1 (D01) | 274.2 / 178.0 | 102.9 / 69.9 | 151.1 / 101.3 | 51.4 / 39.7 | 33.2 / 25.9 |
| Sonic 2 (D01) | 464.2 / 302.3 | 174.0 / 120.5 | 260.0 / 173.7 | 89.8 / 70.8 | 54.6 / 46.1 |
| Sonic & Knuckles (D01) | 800.6 / 510.3 | 336.4 / 212.4 | 482.9 / 306.5 | 230.1 / 153.2 | 152.0 / 99.0 |
| Flicky (D02) | 36.5 / 26.6 | 24.4 / 17.7 | 21.5 / 16.2 | 14.3 / 10.8 | 9.6 / 7.2 |
| Shining Force (D03) | 1159.0 / 488.5 | 629.2 / 229.1 | 699.0 / 283.4 | 345.5 / 93.9 | 317.2 / 76.1 |
| Shining Force II (D03) | 1007.5 / 566.8 | 464.1 / 254.8 | 573.3 / 330.1 | 173.3 / 96.1 | 137.4 / 76.4 |
| Phantasy Star II (D04) | 319.8 / 221.3 | 139.9 / 100.7 | 189.2 / 132.2 | 38.1 / 33.8 | 24.7 / 25.3 |
| SGDK program (D05) | 21.8 / 16.3 | 19.4 / 14.0 | 17.7 / 13.1 | 20.6 / 14.9 | 19.3 / 15.1 |
| SGDK program (D06) | 69.6 / 54.4 | 49.2 / 37.5 | 37.4 / 30.1 | 37.6 / 29.2 | 34.4 / 28.9 |
| 240p Test Suite (D07) | 101.6 / 54.9 | 102.9 / 54.9 (fallback: `prune_rejected`) | 60.1 / 33.8 | 79.8 / 40.4 | 72.5 / 35.3 |
| SGDK program (D09) | 29.9 / 22.2 | 20.2 / 14.2 | 18.4 / 13.6 | 30.5 / 21.2 | 30.8 / 21.8 |

Not measured:

- **D10, D11, D12** keep their typed status, broad `effective_address_not_24bit`.
- **Sonic CD modules** have no region-admission path. They are count-only (below).

Correctness and acceptance:

- **Validator acceptance:** the unchanged validator accepted every `ORACLE_REGION`, `ORACLE_INSTR` and `M3RB50` plan. `INC` fell back
  only on D07.
- **C outside K (labelled starts not admitted)** is 0 for every accepted plan.
- **Runtime equivalence:** **11/11** images for both oracles, and for every other accepted strategy. Outcome, frames, final state,
  coverage and frame-stream digests are equal to `BROAD`, with 0 executed PCs outside broad's.
- **Short broad references:** the broad reference itself stops early with a typed `guest_stop` on Flicky (26 frames), Sonic & Knuckles (9),
  D05 (9) and D07 (2). There, equivalence covers only that prefix. As always, it is not evidence of static recall.

Other measured effects (lineage means relative to broad):

| strategy | executable size | total wall time | peak compiler RSS |
|---|---|---|---|
| `INC` | 0.61 | 0.69 | 0.91 |
| `ORACLE_REGION` | 0.48 | 0.55 | 0.88 |
| `ORACLE_INSTR` | 0.46 | 0.50 | 0.89 |

Cache-assisted rebuild (production content-addressed object cache, same strategy and directory):

- Every unit hits. The warm rebuild takes 0.5-3.5 s wall, against 1-5 min cold.
- Computing the keys (one preprocessing pass per TU) costs 1.7-11.5 s.
- Cross-strategy reuse is 0 **by construction**: the key includes the TU path and the preprocessed content, and a different admitted set
  changes every AOT TU.

## Gate (ADR 0156, applied unchanged)

| figure | value | threshold | met |
|---|---|---|---|
| `g` = 1 - mean_L(ORACLE_INSTR / ORACLE_REGION), CPU | **0.176** | >= 0.20 | no |
| `g_c`, generated C | **0.154** | >= 0.20 | no |
| `h` = 1 - mean_L(ORACLE_INSTR / INC deployed), CPU | **0.342** | >= 0.30 | yes |
| lineages with instruction-vs-region gain >= 0.10 | **4 / 8** (D01 0.36, D02 0.32, D03 0.14, D04 0.35; D05 0.06, D06 0.08, D07 0.09, D09 -0.01) | >= 5 | no |
| lineages whose `ORACLE_INSTR` is accepted | 8 / 8 | >= 6 | yes |
| runtime blocker | none (11/11 equal) | - | - |
| `rho` (representation headroom, `ORACLE_REGION` builds) | **0.247** (lineages 0.18-0.38) | >= 0.20 | yes |

**G1 fails** (`g`, `g_c` and the per-lineage condition), and `rho >= 0.20`, so the gate selects **G2**.

## Attribution (descriptive; lineage means of CPU ratios)

| question | comparison | commercial D01-D04 | SGDK D05-D09 | all 8 |
|---|---|---|---|---|
| A: ML ranking error (same granularity) | ORACLE_REGION / INC | **0.47** | 1.03 | 0.75 |
| B: coarse granularity | ORACLE_INSTR / ORACLE_REGION | **0.70** | 0.94 | 0.82 |
| deployed incumbent vs broad | INC / BROAD | 0.50 | 0.82 | 0.66 |
| floor vs broad | ORACLE_INSTR / BROAD | 0.17 | 0.78 | 0.48 |
| research pair | M3RB50 / INC | 1.22 | 0.79 | 1.00 |

- **A (ranking) and B (granularity) are both large on fully source-labelled commercial images.**
  - A perfect 128-byte region is about half the incumbent's cost.
  - Exact starts remove a further ~30%. Structural closure adds **nothing** there: the upward closure of the labelled starts added 0-9
    identities on every commercial image, so source-backed code is already closed under the validator's obligations.
- **On SGDK programs neither oracle helps.** Their truth leaves the SDK runtime and data unknown (`U` is 18-54% of image bytes):
  - The pre-registered oracle treats `U` as possibly executable.
  - Closing those unknown positions upward re-admits most of the broad universe. On D05, 21,195 proposed identities grow to 62,015 of
    65,160.
  - The non-gating diagnostic below shows that this caution is necessary, not conservative.
- **C (closure) therefore matters only through unknown bytes.** Its cost is the cost of not knowing.
- **D (representation and compiler behaviour) bounds the floor:**
  - On D03 the precise-discovery `block` TUs, which every strategy emits, take a lineage mean of **0.70** of the `ORACLE_INSTR` build CPU
    (`ORACLE_REGION` 0.62): 0.82 / 0.75 on Shining Force and 0.58 / 0.48 on Shining Force II. Shining Force's blocks are 20.5 MB of C
    in 463 functions, the largest about 400 KB. Sonic 1 has 2.8 MB.
  - Admission precision cannot touch that cost. It is why D03's instruction-vs-region gain is only 0.14.
- **`rho`:** a quarter of the generated C sits in per-identity entry segments that duplicate another segment once literals are normalized.

### Non-gating diagnostics (added after the first measurement; not pre-registered, never part of the gate)

1. **Labelled-only instruction admission** (`DIAG_INSTR_LABELLED`: only `I` starts, unknown bytes excluded, closed upward) on the SGDK
   lineages. Cost vs broad falls to 0.18-0.47 (D05 0.47, D06 0.20, D07 0.36, D09 0.18). But **D06 and D09 fail closed** with a typed
   `guest_stop` at frame 7, where broad reaches 120 frames: unlabelled SDK code executes. **The absence of a label is not evidence of data;
   excluding unknown bytes is unsafe.** D05 and D07 stay equal only within their short broad prefixes.
2. **Representation decomposition** of `rho` (the provenance line is excluded, since existing shared helpers already take it as a parameter):
   - **PC-derivable** (every differing literal shifts by the address delta): 0.005-0.085 of the generated C.
   - **Exactly one non-PC operand literal** (an immediate or absolute operand): 0.11-0.25 (lineage mean 0.16).
   - So `rho` is realizable only by **operand-parameterized** sharing of the residual owner segments, not by PC-only sharing. This
     extends the existing `genesis_aot_shared_*` helper scheme. It is not a new mechanism.
3. **Block-TU hotspot:** separate per-TU compiles of the `block` family (`ORACLE_REGION` build):
   - D03 takes 234 CPU-s (Shining Force) and 71 CPU-s (Shining Force II);
   - every other lineage takes 1-36 CPU-s;
   - the top 5 TUs carry 85-100% of the cost.
4. **Count-only density** (full qualified registry). Labelled-instruction positions as a fraction of ordinary-decodable positions:
   - 0.07-0.28 on commercial images, against 0.11-0.46 decodable positions inside the region oracle;
   - 0.31-0.63 on SGDK images, which carry unknown bytes;
   - 0.094 for the 36 Sonic CD verified modules, against 0.161 inside their region oracle. They are count-only: no Sega CD
     region-admission path exists (ADR 0131/0133), and none was built. No generated-C or compile saving is claimed for Sega CD.
5. **Sega CD authentic BIOS / materialized images** (inventory from ADRs 0147/0150; different `sega-cd emit` per-function pipeline, gcc -O1, 12
   jobs):
   - 178,005 instruction functions, 176 MB of C in 38-39 TUs;
   - about 1,020-1,050 s for the full cold admission build (14 rounds plus the confirming run);
   - 922 MB peak compiler RSS.

   The game-module oracle does not apply to BIOS materialization, and this task makes no partial-admission claim for those images.

## Decision and recommendation for T002

**G2.** Two reasons:

- **Instruction-level admission does not clear the pre-registered bar.** Its benefit is real on fully labelled commercial code: about
  -30% vs a perfect region, and -67% vs the deployed incumbent. Across the lineages, however, it is limited:
  - by unknown bytes, which cannot safely be excluded;
  - by precise-discovery block code, which admission cannot reach.
- **About a quarter of the generated C is factorable residual entry code.**

T002 should therefore be refined (full refinement) toward **generated-C factoring and compilation-pipeline efficiency**, measured against
this task's baselines on the same 11 images, with no fine-grained admission layer:

1. **Operand-parameterized sharing** of residual `genesis_aot_entry_*` owner segments:
   - extend the existing shared-helper ABI with one operand parameter;
   - target the measured 0.11-0.25 one-operand share;
   - keep CPU-owned semantics byte-for-byte equivalent;
   - require deterministic output.
2. **Precise-discovery block representation:**
   - bound or split very large `genesis_block_*` functions, and/or share their per-instruction bodies with the AOT shared helpers;
   - target the D03 hotspot (lineage mean 62-70% of oracle-build CPU), with runtime equivalence vs the current build.

Both apply to broad, incumbent and any future admission strategy alike, so they help the default Compatibility path too.

Facts recorded for later refinement:

- The ranking headroom (A) on commercial images is large: a perfect region costs 0.47x the incumbent.
- G2 forbids building a new admission architecture now.
- A later deterministic-evidence task (T003 territory) would need its own pre-registration. It must treat unknown bytes as possibly
  executable, which diagnostic 1 shows is mandatory.
- T003-T005 as currently written assume G1. They must be re-gated by full refinement; continuation cannot edit them.

## Limitations

- **Bias toward the incumbent:** `INC` is in-sample on Sonic 1, Flicky, Shining Force, Phantasy Star II and Sonic & Knuckles. That favours
  the incumbent, which makes `h` conservative. `M3RB50` excludes the research model's PyTorch inference (0.45-7 s per image, ADR 0154),
  which favours `M3RB50`.
- **Short runtime evidence:** runtime equivalence is a 120-frame bounded trace (shorter where the broad reference stops early). It is not
  static recall.
- **Optimistic oracle floor:** the oracles read source-backed truth. Their numbers are a floor that no production producer is shown to
  reach.
- **Single host, single compiler:** CPU time is robust to scheduling; wall time is descriptive.
- **`rho`'s normalization** collapses every hexadecimal literal. Its realizable part depends on the operand-parameter design (diagnostic 2).
  The segment splitter ends a segment at the first column-0 `}`. That equals ADR 0156's "next label or end of owner" for the emitter's
  multi-line owner bodies; a one-line owner entry would merge neighbours.
