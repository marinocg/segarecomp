# ADR 0151: SEG-072 M68K ML v3 - pre-registration (fixed corpus, dense ranking, adaptive budgets)

- Status: **Pre-registration, frozen.** Committed as checkpoint C1 of the combined SEG-072 PR, before any new truth extraction, sealed
  qualification, model fit, budget fit or benchmark. The machine-readable protocol is `tools/segarecomp_ml_v3.plan.json`
  (digest `tools/segarecomp_ml_v3.plan.digest`, generated from and verified against the constants of `tools/segarecomp_ml_v3.py`).
  This record is immutable; results are appended in later ADRs (0152 qualification, 0153 development results and freeze, 0154 sealed
  evaluation, economics and decision).
- Scope: **offline research only.** Production gen2 M3 (`gen2`, B35), Optimized v1, the structural prune, the admission validator, launcher
  defaults and the Sega CD admission policy are unchanged; SEG-065's Sonic CD implementation is untouched. No production integration, model
  replacement or default change is part of SEG-072; justified production work would be a separately approved task.
- Related: ADR 0099 (source-backed truth method), 0100 (gen2 M3/M1), 0101 (B35), 0117 (SEG-056 Z80 dense model, ROCm benchmark), 0123/0124
  (adaptive-budget lessons), 0131/0133/0140 (Sega CD ML boundary, frozen-M3 transfer on Sonic CD modules).

## Question

Does a fixed, pinned, expanded MC68000 source-backed corpus justify (1) retraining the frozen M3 architecture (M3-R), (2) an M68K-specific
byte-dense CNN (M4-Dense), (3) a conservative adaptive retention budget instead of fixed B35, or (4) none of these (retain M3+B35 or broad AOT)?
The experiment succeeds by finding the simplest model and budget strategy that survives independent recall and economic tests.

## Corpus (fixed; no additions, substitutions or reassignments)

Twelve development lineages (D01 Sonic engine: Sonic 1, Sonic & Knuckles, Sonic CD modules, Sonic 2; D02 Flicky; D03 Shining Force I+II;
D04 Phantasy Star II; D05-D12 the eight pinned homebrew projects, D12 `streeter` only), seven sealed lineages (E01 Alien Soldier JP, E02
Landstalker US, E03 Kid Chameleon, E04 QuackShot, E05 Sonic Spinball, E06 Tank Battle, E07 The Weave) and the TMSS diagnostic. Every source is
pinned to a 40-hex revision; identities are in the plan's `corpus` block. Related titles form one lineage; modules of one game are not
independent games. Qualification requires reproducible executable bytes and independently supportable instruction-start labels; failures are
recorded per entry and never shrink the denominator (minimums 12 development, 7 sealed). The frozen-M3 training titles are exposed development
material, never fresh blind evidence. Historical ROM-only exposure of Alien Soldier and Landstalker is disclosed; Sonic Spinball's Sonic-ecosystem
overlap and the shared SGDK/GCC ecosystem of E06/E07 with D05-D12 are stated limitations (cross-project, not cross-compiler-family, evidence).

TMSS is never trained on, never used for selection and never counted; without an authorized firmware input it is `TMSS_DIAGNOSTIC_UNAVAILABLE`
(building it from public source would reconstruct commercial binary data, which is forbidden).

## Labels

Per-byte classes: `I` instruction start of a source-backed MC68000 row, `c` its continuation bytes, `D` known data, `U` unknown/masked (shared
SGDK/runtime/libgcc/startup code and data, ambiguous or unverifiable bytes). A 128-byte cell is positive iff it holds an `I`, negative iff every
byte is `D`, masked otherwise. M4-Dense learns a per-word occupancy label (`I`/`c` = 1, `DD` = 0, else masked). Recall is scored only on `I`.
Disassemblies use `tools/segarecomp_source_map_extract.py` listing semantics (ADR 0099; fails closed). SGDK projects keep the LTO link-time
assembly (`-save-temps`), re-assemble it with listings, place sections with the linker map, and require the instrumented build to be
byte-identical to the plain build and two clean builds to be identical; app code/data are `I`/`c`/`D`, SDK/libgcc/startup and functions of
ambiguous origin after LTO are `U`, app resources are `D`. Sonic Spinball (IDA-derived): only instruction rows are labels; FIXME/inline-hex rows
and non-instruction bytes are `U`.

Independence: 12-byte shingle containment of labelled app-code runs between development projects (>= 0.30 = substantially shared, the two
projects then count and fold as one lineage); raw-byte shingle containment for sealed-versus-development checks (no sealed labels used).

## Candidates (exactly three, plus the frozen baseline)

- **M3_FROZEN** (baseline, unchanged): historical title-held-out outer-fold predictions for the five original titles (reproduced and
  digest-verified first), the production weights for every other image (zero-shot for D05-D12, lineage-correlated for s2/scd/sf2, flagged).
- **M3-R**: identical gen2 architecture, representation and recipe, trained on the expanded development corpus; masked cells carry no loss.
- **M4-Dense**: per-word occupancy CNN: byte + MC68000 decode-token embeddings with a PAD index outside the image, local path (2x k5 conv) +
  stride-4 residual dilated context path (dilations 1..32, radius 504 words), fused per-word logit; cell score = max word logit; cells {128, 64}
  bytes as aggregation only; 4000 steps, lineage-uniform sampling, seeds 1-3 logit ensemble. No Z80 weights, no universal model.
- **M1-HGB**: the gen2 multiscale HistGradientBoosting control on the same corpus.

Folds: leave-one-lineage-out over the qualified development lineages (Sonic titles together, Shining Force titles together, merged projects
together; no cell/module/function splitting).

## Budgets

Fixed B35, B45, B50, B60 (`N = ceil(cells x percent / 100)` ranked cells UNION seed cells; retained = |R| / cells) and **one** adaptive policy
ADAPT: density estimate `e` = fraction of cells with probability >= 0.5; margin `m` = max over fitting images of (residual FRF - e) + 0.05; budget
`b = ceil_to_0.01(e + m)`; abstain to broad AOT when `b > 0.60` (the pre-registered maximum retention). Inputs are label-free (scores, seeds,
cell count); truth, identity, positions and traces are forbidden inputs. Development evaluation fits `m` on the other lineages' out-of-fold
records (no inner refits - a disclosed limitation); the sealed run fits it on all development records. Broad AOT is the reference. Neural
confidence is not treated as proof of recall.

## Selection and freeze

Pairs (configuration x policy). Order: complete development recall (filter) -> lowest worst-lineage FRF (tie set 0.02) -> lowest mean retained
fraction (tie set 0.02) -> lowest mean K/U on Genesis images (tie set 0.02) -> simplest (M3_FROZEN < M1-HGB < M3-R < M4-Dense@128 < @64; fixed
before adaptive; smaller budget). The incumbent M3_FROZEN+B35 is retained unless the best alternative is complete and either the incumbent is
incomplete or the alternative lowers mean retained by >= 0.05 without raising worst-lineage FRF by more than 0.02. If no pair is complete, the pair
with the fewest misses is frozen for descriptive sealed evidence and the outcome is RECALL_FAILED. Exactly one configuration, one policy and the
training backend/environment are recorded in `tools/segarecomp_ml_v3.frozen.json` and committed before any sealed truth is opened; the
configuration is then refit once on all development lineages.

## Hardware

CPU by default. Before M4-Dense training, one bounded benchmark (final architecture and batch, representative data, warm-up + a 500-step epoch,
3 repeats, CPU 2-thread fit and the actual 8-way parallel CPU farm versus ROCm, peak RSS/VRAM, same-seed ROCm reproducibility). ROCm is selected
only for >= 25% estimated end-to-end improvement plus reproducibility; otherwise parallel CPU; an unavailable/unstable ROCm falls back
explicitly. No GPU kernel work, no architecture change for speed, one backend per like-for-like comparison.

## Sealed evaluation

Before any training, each qualified sealed entry is bound (source commit, build inputs, image digest, opaque truth digest); sealed truth exists only
under the ignored private `sealed/` store, which development tooling refuses. The unseal command requires the committed frozen record, verifies
every digest and runs exactly once. Zero-miss gate: a single confidently labelled sealed start outside R fails it; no refit, recalibration or
widening afterwards. Runnable Genesis images additionally compare bounded generated-native execution (explicit instruction budget, wall timeout)
against broad AOT; short runtime-equivalent traces are never evidence of static recall.

## Economics and outcomes

Separately for Genesis immutable ROMs, Sonic CD verified modules (R-only; no Sega CD R-admission path exists and none is built) and broad
materialized images (not comparable: no existing contract admits partial ML proposals). Measures: selected cells, emitted functions, generated C,
compiler CPU/wall/RSS, binary size, inference cost, net benefit, cache reuse. Gates: versus broad, generated C -30% and compile CPU -25% net of
inference; versus the incumbent, -10% generated C and -10% compile CPU, or a reliability improvement at no more than +10% generated C.

Classification precedence: RECALL_FAILED (no complete development pair, or any sealed miss) > CORPUS_INSUFFICIENT (fewer than 12/7 qualified
lineages; qualified-entry results reported separately, no promotion) > ECONOMICS_NOT_JUSTIFIED (incumbent retained or an economics gate failed) >
ADAPTIVE_ONLY_JUSTIFIED (M3_FROZEN+ADAPT) / M3_RETRAIN_SUFFICIENT (M3-R) / V3_READY_FOR_OPT_IN (M4-Dense or M1-HGB); a positive outcome requires
the independent adversarial review's concurrence.

## Privacy

Commercial images are operator supplied. Images, source-derived instruction inventories, training examples, listings, traces, scores and
generated commercial source stay in ignored private storage. Committed results are non-reconstructable aggregates, identities, digests and model
metadata. No weights trained on private inputs are published.
