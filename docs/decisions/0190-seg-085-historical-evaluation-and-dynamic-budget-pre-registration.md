# ADR 0190: SEG-085 - historical M68K evaluation and dynamic retention budgets: evaluation-distribution correction and pre-registration (checkpoint C0)

- Status: Accepted (SEG-085, checkpoint C0). Follows ADRs 0151-0189; none is amended. SEG-072 ADAPT and its frozen results, SEG-076 structural model A, SEG-084's plan `f93e4023...` and its decision (`MODEL_OR_EVALUATION_LIMITATION`) are unchanged.
- Protocol version: `seg085.protocol.v1`. No SEG-085 branch, PR or frozen protocol existed before this record, so nothing is superseded. An earlier informal draft that emphasised SGDK/GCC homebrew was never registered; this record
  replaces it, and any later correction must be a new, explicitly versioned protocol.
- Privacy: aggregate only. Identities, digests and counts are recorded; no ROM bytes, source, traces, addresses or label maps enter the public repository.

## 1. Evaluation-distribution correction

The principal use case is commercial-era Sega Genesis software (c. 1988-1998): hand-written MC68000 assembly, period assemblers, original-era C or mixed C/assembly, jump tables, indirect branches, inline constants and
embedded data. Modern SGDK/GCC programs are a secondary robustness population.

SEG-084's development corpus is imbalanced against that use case, and this is stated here before any new result:

| category (T001 definition) | lineages | note |
|---|---|---|
| commercial-era Genesis, community reconstruction (build-identical rebuild) | D01 (Sonic 1/2/S&K + 36 Sonic CD modules; one engine family), D02 Flicky, D03 Shining Force I+II, D04 Phantasy Star II | 4 of 11; D01 and D03 are multi-image |
| modern SGDK/GCC homebrew | D05 Genesis 2048, D06 TetrisMD, D07 240p Test Suite, D09 MegaPong, D10 Genesis Megatetris, D11 Penguin World, D12 MegaDriving | 7 of 11 (SGDK 1.11-2.11, GCC 3.4.6-13.2) |
| cross-platform M68K (SEG-075, transfer only) | BattleIsleEditor (Amiga, author's 1991 source, A68K 2.71 + vlink), amiga-realtime3d (vasm, modern), toslibc (GCC 13), EmuTOS (GCC 4.6 + gas), HelloAmi (diagnostic) | only BattleIsleEditor has period tooling; each lineage's era is taken from ADR 0159, not from its name |
| partially observed commercial TAS evidence | SEG-081/083 games | partial, never source-backed truth; reported separately |

Rules adopted: (a) the SGDK share and D07's B35 miss count must not drive acquisition, calibration, model selection or the primary success metric; (b) a real recall failure in a modern program is still reported and disclosed, and a
policy that misses there is not promotable on the strength of that lineage being "modern" (any future scope restriction needs independent, non-game-specific justification); (c) the selector uses only image features and calibrated
scores, never a game name, an era label or a benchmark category; categories exist only for analysis.

## 2. Evidence tiers (never merged into one "source truth")

| tier | meaning | use in this milestone |
|---|---|---|
| T-A | original source/build-backed (the original build or an equivalent listing from the original toolchain) | primary if the era qualifies |
| T-B | reconstructed code, byte-identical rebuild **plus** independent boundary corroboration (see below) | primary |
| T-C | high-confidence but incomplete instruction evidence (partially observed / execution-corroborated) | diagnostic only, never primary |
| T-U | unknown or disputed bytes | masked, never negative |

A byte-identical rebuild alone is **not** boundary corroboration. For T-B, every accepted image must additionally pass, with checks that do not use the model: (1) the CPU-owned decoder agrees with every labelled start length
(zero length mismatches, zero odd starts); (2) no labelled code span overlaps labelled data; (3) every direct control-flow target found in labelled code lands on a labelled instruction start - a target landing on a labelled
data byte or inside an instruction is a label contradiction, and the contradicted bytes become T-U (masked); (4) where an independent execution record of the image exists it must be a subset of the labelled starts, otherwise the
same masking applies. Unexecuted or undisassembled bytes are never negative. A label that cannot be corroborated is masked. Each image records: project identity, era and toolchain evidence, CPU/platform, exact input
SHA-256, lineage-independence assessment (12-byte shingle containment of labelled code runs, threshold 0.30, ADR 0152), label provenance and tier, reproduction recipe, known gaps and masking, licence and retention boundary.

## 3. Acquisition protocol (T002)

Order of preference: (1) 1990s Genesis/Mega Drive commercial games with a public disassembly that rebuilds byte-identically from a locally authorized image (ROMs are never supplied by this project; an image that is not
already held under the ignored `games/` directory cannot be used); (2) other period-toolchain M68K programs; (3) original-era Atari ST/Amiga programs that exercise commercial-Genesis coding patterns. Modern SGDK/GCC builds
are not selected to reach a count. Target 3-5 independent lineages. Exclusions: every development lineage (D01-D12), every SEG-075 development lineage, the sealed/reserved cohorts (SEG-072 E01-E07, SEG-075 reserved, SEG-077 reserved
titles), and any title whose labels or bytes entered training, fitting, calibration or selection of any model. Titles that appeared in SEG-077/081/083 partial evidence are allowed only with the exposure disclosed, because no
model consumed their source-backed labels. If fewer than 3 credible independent historical lineages qualify, partial results are preserved and the milestone reports `EVIDENCE_INSUFFICIENT`; the shortage is never filled with SGDK
examples. Cohort membership, qualification outcome and tier of every image are frozen in a committed record (checkpoint C2) before any policy is scored on it.

## 4. Policies (frozen in detail at C3; parameters never change after C3)

- References: structural model A + B35, A + B50 (`ml_v3` fixed budgets over the unchanged model-A ranking), the production M3 incremental baseline where it applies, and broad immutable-ROM AOT.
- ADAPT (SEG-072): reproduced from the stored scores first; then run unchanged on model A through the minimum compatibility adapter (model-A logits and `sigmoid(logit)` as the probability); not reinterpreted.
- DYN_SCORE: unchanged model-A cell scores; one score threshold fitted on development data only; every cell above it plus the independently established seed cells; the existing structural closure and admission validator
  unchanged; abstain to broad AOT when the calibration/applicability assumptions fail or the retained fraction exceeds the frozen cap (0.60, the SEG-072 `MAX_RETENTION`). The retained fraction is a result, never an input.
- Calibration (frozen at C3): deterministic, lineage-separated, fitted only on data whose scores were produced by a model that did not train on the target lineage (bounded inner refits), with no access to the evaluated
  image's labels. No search over calibration formulas; one pre-registered statistic and at most one pre-registered label-free normalisation if development evidence shows raw scores are not comparable.

## 5. Evaluation and decision rules (pre-registered)

Reporting is per category and never pooled into one score; aggregation is per lineage with per-image detail. The primary set is the independent cohort (T-A/T-B images only).

Recorded for every policy and image: retained cells before and after seeds/closure, percentage retained, missed verified starts, boundary and indirect-control-related misses, worst-image and worst-lineage recall, abstentions,
validator rejections, broad fallbacks, generated C bytes, emitted instruction functions, compiler and total build CPU and wall time, memory where available, policy/model inference overhead, and runtime equivalence with broad
execution where supported. Economics follow the SEG-074/076 accounting; rejected work followed by a broad fallback is charged in full; an image that cannot pass the generated-native pipeline is an **economics coverage gap**,
and theoretical retention is never substituted for measured savings.

Outcomes (exactly one):

| outcome | rule |
|---|---|
| `EVIDENCE_INSUFFICIENT` | fewer than 3 qualified independent historical lineages, or fewer than 3 primary images measured through the generated-native pipeline, or calibration isolation cannot be established |
| `RECALL_FAILED` | one or more verified instruction starts of a primary image are outside the DYN_SCORE region after seeds/closure/admission, or an admission/runtime-equivalence safety gate fails |
| `STATIC_MODEL_OR_CALIBRATION_LIMITATION` | no primary miss, but a development/secondary miss, an incomparable-score finding or a non-viable cap interaction shows a concrete limitation; the limitation and the narrow future study it would justify are named |
| `ECONOMICS_NOT_JUSTIFIED` | recall passes everywhere above, but pooled total build CPU over measured primary images is not at least 10% below the same-session A + B50, or any material regression versus the deployed incremental baseline (pooled total build CPU more than 5% above it), or the savings are explained by excluded failures or fallbacks |
| `DYNAMIC_BUDGET_PROMISING` | none of the above: zero missed starts on every primary image and every disclosed secondary image, admission and equivalence gates pass, pooled total build CPU at least 10% below same-session A + B50, no material regression versus the incremental baseline, savings not explained by fallbacks |

The result is a research threshold, not a production authorisation. A zero-miss result on a small cohort is empirical recall, not a completeness guarantee: a model score is not proof that a region is non-executable and
the admission validator cannot show that previously unidentified executable code was not excluded. Uncertainty is reported (per-lineage dispersion and a bootstrap over images where defensible; none is claimed from a
cohort of three).

## 6. Constraints carried

No interpreter, JIT, runtime opcode decoding or dynamic code expansion; no game-specific address or target lists or identity rules; no truth-informed budget decision; no runtime traces in the selector; no label leakage into fitting
or calibration; existing discovery, lowering, native admission, the admission validator and production defaults unchanged; deterministic, bounded and fail-closed; no model retraining or hyperparameter change; no sealed or reserved
cohort opened.
