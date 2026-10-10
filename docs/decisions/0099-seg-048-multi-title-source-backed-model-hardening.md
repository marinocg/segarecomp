# ADR 0099: SEG-048 multi-title source-backed executable-region model hardening

- Status: T001 protocol frozen (this section is immutable once committed; later sections only append results)
- Predecessors: ADR 0093 (exact source universe), ADR 0095 (SEG-046 experiment), ADR 0096 (SEG-047 native v1 integration)
- Scope statement: **SEG-048 DOES NOT CHANGE PRODUCTION OPTIMIZED AOT.** SEG-047 native v1 remains the production optimized model. SEG-048
  produces an offline frozen v2 *candidate* only; any native/product adoption of v2 belongs to SEG-049.

## 1. Question

Does the SEG-046/047 M68K executable-region producer generalize across genuinely different Genesis titles strongly enough to justify a
production model-v2 port in SEG-049? Evidence is source-backed executable truth `C` (instruction starts assembled in MC68000 mode, from an
exact external rebuild), not bounded runtime traces. `V1 RETAINED` and `BLOCKED` are valid results; no v2 win is forced.

Architecture (unchanged): `U` (broad immutable-ROM universe) -> ML region proposal `R` (window probabilities >= threshold, union the
certain-code windows) -> unchanged structural prune -> `K` -> unchanged production validator -> generated-native AOT. No interpreter/JIT/
runtime decoding/runtime ML; broad AOT stays the unconditional compatibility fallback; exact source maps outrank ML; the validator stays the
correctness authority; fail closed. Production v1 artifacts (`tools/segarecomp_ml_region.{py,frozen.json,model.json,parity.json}`,
`..._native_data.py`, `..._native_parity.py`) are not modified by this milestone.

## 2. Synchronized baselines

| repository | branch | SHA at synchronization |
| --- | --- | --- |
| segarecomp (product) | main | `ed9b26bfbd4d118cf5afbde4e27903e448ed8a3f` (includes PR #88 compatibility repairs, PR #89 cartridge SRAM) |
| segarecomp-harness | main | `5480847029a0b9c9001ae28f044534b038b22721` |

Single product branch `milestone/seg-048-model-hardening`; single product PR "SEG-048: multi-title source-backed executable-region model
hardening" (draft after T001). No per-task PRs. Checkpoint commits (chronological evidence, never squashed before the blind experiment ends):
C1 T001 protocol, C2 T002 truth tooling, C3 T003 baselines, C4 T004 ablation, C5 T005 family comparison, **C6 T006 FINAL V2 CANDIDATE
FREEZE** (`T006_FREEZE_COMMIT`), then the HARD BLIND BARRIER, C7 T007, C8 T008.

## 3. Pre-registered corpus (fixed; never replaced on the basis of results, convenience, runtime compatibility or truth density)

| role | id | title | operator-authorized local image SHA-256 |
| --- | --- | --- | --- |
| LOTO fold member | A | Sonic the Hedgehog (REV00) | `46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6` |
| LOTO fold member | B | Flicky (JUE) | `4df1a91e08376ae773a6eb8e5abe310f94ca39d8f81583b91a9477fedcb68c11` |
| LOTO fold member | C | Shining Force (U) | `6c43b41b60ae8147945b4420caeb5eb266e1dd57a0d125b7496fa8fb6fde522d` |
| LOTO fold member | D | Phantasy Star II (UE, REV 02) | `a0bd97f5aaa67301923ccc367511606bd3471ea9542919165311d4a23a2b696b` |
| sealed formal blind | E | Alien Soldier (U) | `2754fa0ddf8a3f71998f4b7f5294fda7fe94cf452513d51c41d2ff8d3ae0f2d1` |
| sealed formal blind | F | Land Stalker (U) | `497958caf4e357f6862e438b1306e8a124a336b797a5dbe0ff50e09600162004` |
| post-freeze related-engine control (not a blind-gate title) | G | Sonic & Knuckles (JUE) | `6e12e6b33c26ebfcd0be433251d21cf6284eafe9f71b027bda3767ae59affec1` |
| historical v1 comparator/regression only (never SEG-048 truth, never "fresh blind") | - | Sonic 2, Cool Spot, Streets of Rage, OutRun | (SEG-046/047 images) |

LOTO folds (four-way leave-one-title-out; the held-out title contributes zero rows to the fit, the scaler, the threshold or any selection of
its fold): train B+C+D -> Sonic 1; A+C+D -> Flicky; A+B+D -> Shining Force; A+B+C -> Phantasy Star II.

## 4. External source authorities, pinned revisions and exact-rebuild qualification (T001 verification, performed before any new truth)

External repositories are evidence providers only. They are cloned into ignored private storage (`<harness>/.cache/seg048/`), never vendored,
and no source text, listing, assembly, address list or per-window label is ever committed. A branch/HEAD is not an identity: the 40-hex
revision is.

Qualification contract per title: operator-authorized image + public source + pinned revision + known build configuration + **byte-identical
rebuilt ROM** + a sound method for exact MC68000 instruction-start truth.

| id | repository | pinned revision | build identity | rebuilt SHA-256 | exact_match |
| --- | --- | --- | --- | --- | --- |
| A | `github.com/sonicretro/s1disasm` | `7ebe4b3d0c182b2566026b6d3f423d33539558da` | repo `build.lua` (Lua 5.4 host) with the documented switch `Revision = 0` on `sonic.asm` line 14; bundled Linux-x86_64 `asl` (AS V1.42 Bld 212) and `p2bin`; `asl -xx -n -q -A -L -U -E -i . sonic.asm`; config id `Revision0` | `46160baa...f2a6` | **yes** |
| B | `github.com/oranguthang/flicky_src` | `d9a0d4799c25fb23ba6251306508ee40a1ce9a89` | `make verify AS_ARGS="-maxerrors 2 -n -L"` with bundled `bin/linux_x86_64/asl` (Bld 212) + `p2bin -p=FF`; reference name `Flicky (UE) [!].bin` is SHA-1 `83d8bbf0...` == the authorized JUE image; config id `default` | `4df1a91e...8c11` | **yes** |
| C | `github.com/ShiningForceCentral/SF1DISASM` | `1f466bf038a64f6bdf3113e484bace0c3c66c6f7` | `split/sf1splits.txt` data split of the authorized image (`splitrom.exe`), `asw`+`p2bin` for the Z80 sound driver and two music banks, `ASM68K.EXE /k /m /o ae- /p sf1.asm` (SN 68k 2.53), `fixheader.exe`; all Windows tools run under Wine 9.0; config id `default` | `6c43b41b...522d` | **yes** |
| D | `github.com/wyatt8740/ps2disasm` | `c23877b8aa090473562ae2ed571ace2869289167` | `ps2.asm` defaults `revision = 2`, `soundrev = 0`; **repository-bundled `AS/win32/asw.exe` (AS V1.42 Bld 89) under Wine** `-xx -c -A -L ps2.asm`, `ps2p2bin.exe ps2.p out.bin ps2.h`, repository `fixheader.cpp` compiled natively; config id `rev2` | `a0bd97f5...2b` (SHA-1 `0711080e...`) | **yes** (see note) |
| E | `github.com/oranguthang/alien_soldier_src` | `4f3d7e852aed6edd7a869afe6c252813c4acf316` | repo supports exactly one canonical **Japanese** image (SHA-1 `8f6eb584ed9487b8504fbc21d86783f58e6c9cd6`, the init and split scripts hard-refuse any other image; the European/other profiles are declared excluded) | not buildable from the authorized image | **no** |
| F | `github.com/lordmir/landstalker_disasm` | `fb8a9c6c64ec27c05acea75f9556c928d2b9f081` | `build.sh -r <region>` (ASM68K 2.53 + asw under Wine, `fix_checksum.py`); the six region variants US/JP/EUR/FR/DE/BETA were built; the authorized `LAND STALKER 1993.FEB` image is closest to variant `BETA` | `64f4d40a...2a56` | **no** (1 byte: header region character at offset `0x1F0`, `J` in the rebuild, `U` in the authorized image; the other variants differ by >= 1.09 M bytes) |
| G | `github.com/sonicretro/skdisasm` | `044fa46725c71187399e13f5ddb70e11d32dc024` | repo `buildSK.lua` (`-D Sonic3_Complete=0`, Lua 5.4 host, bundled Linux-x86_64 `asl`); config id `SK` | `6e12e6b3...fec1` | **yes** |

Notes:
- Phantasy Star II with the Linux `asl` Bld 212 does **not** reproduce the authorized image (23,173 differing bytes, driver region); the
  repository's own bundled assembler (Bld 89) does, byte-exact. The pinned build tool is therefore the bundled `asw.exe` and is part of the
  recorded build identity.
- Alien Soldier (E) and Land Stalker (F) fail the qualification contract; the fixed corpus is not substituted. See section 12 for the
  pre-registered consequence.
- No executable truth for E, F or G was produced or read during T001. Only source/ROM identities were established.

## 5. Truth methodology (T002)

`C` = instruction-start addresses of listing rows assembled while the effective CPU is MC68000 whose mnemonic is in the closed MC68000 set
(`segarecomp.m68k_source_universe.v1`, `tools/segarecomp_source_map_extract.py`). Listing semantics, not heuristic disassembly; no
runtime-derived, Ghidra-derived or "looks like code" truth; anything unclassifiable fails the whole extraction closed.

Adapters (the smallest deterministic title-agnostic additions; each is opt-in or strictly additive and leaves the existing Sonic 1 output
byte-identical, with a regression test):
1. AS Bld 89 listings print the continuation bytes of a long row on an *address-less* line; they are accepted only when they extend the
   preceding row contiguously.
2. `--trailing-pad`: a ROM tail after the last emitted byte is accepted only if it is a single uniform `00`/`FF` fill running to the end of the
   image (p2bin `-p=FF` padding), never otherwise.
3. ASM68K 2.53 listing dialect (`ADDR BYTES source`): rows are collected by a second row-collector feeding the *same* coverage/overlap/gap
   accounting; data directives, `incbin` and padding macros must be explained exactly like the AS path; macro-definition bodies carry no
   authority.
Every extraction runs twice independently and must yield the same artifact digest. Sanity: all addresses even and in the ROM, every emitted
construct explained, `C` contained in the broad `U`, exact ROM identity. Private outputs live under `<harness>/.cache/seg048/`; only counts,
digests and identities are recorded.

## 6. Frozen feature and model definitions

Baseline v1: `seg046-features-v1`, window 512 B, 141 features (47 base x {current, previous, next}), schema SHA-256
`d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570`, standardized balanced logistic regression (C=1, lbfgs, 2000 iterations),
seed 46, threshold 0.00835406801187952. A window is positive iff it contains at least one `C` instruction start.

Feature candidates (the only ones evaluated; names are `seg048-features-v2-F<k>`; the selected one is re-versioned `seg048-features-v2` with
a new schema hash; `seg046-features-v1` is never overwritten):
- F0: frozen v1 schema exactly (141).
- F1: F0 minus `b_zlib` from the current/previous/next vectors (138).
- F2: F1 plus non-positional edge treatment: a missing previous/next neighbour is the *current* base vector instead of the all-zero sentinel (138).
- F3: F1 with the neighbour context removed entirely (46).
- F4: F2 minus the whole byte-statistics family (`b_*`) (93).
- F5: F2 minus the whole decoder/control family (`d_*`) (45).
Forbidden features (rejected mechanically by `assert_feature_schema` and an extended deny-list): title identity, ROM hash, source-project
identity, absolute/relative/normalized address, ROM offset, window/page ordinal, ROM position, ROM size, path, filename, symbol, label, truth
count, runtime count, execution PC, coverage, frame information, human annotation. The window address is only a row key and is dropped
before modelling. No random window CV; no window-level mixing between titles inside a fit except the explicit pooling of the *training* titles;
pooled training rows are weighted equally (no title weighting). Neighbour features are computed within one ROM only.

Model candidates (only these): `LogisticRegression`, standardized, `class_weight=balanced`, seed 46, lbfgs, `max_iter=2000`, `C in {0.25, 1.0, 4.0}`;
and exactly one bounded comparator `HistGradientBoostingClassifier(max_iter=60, max_depth=3, learning_rate=0.1, min_samples_leaf=20,
early_stopping=False, class_weight=balanced, random_state=46)`. Environment: Python 3.13.14, scikit-learn 1.9.1, numpy 2.5.3, scipy 1.18.1
(private venv; no product dependency).

## 7. Frozen protocol per task

- **T003-A (zero-shot v1)**: the committed canonical v1 model (`segarecomp_ml_region.model.json`, threshold unchanged, no retraining) on A,B,C,D.
- **T003-B (retrained v1-family LOTO)**: F0 + logistic C=1; per fold fit scaler and model on the three training titles; threshold =
  **minimum positive probability over the training titles' own positive windows scored by the fitted model** (policy P0); evaluate the
  held-out title afterwards. The generic certain-code union (windows containing a direct-control-discovery identity) remains.
- **T004 (feature ablation)**: F0..F5 under T003-B conditions (logistic C=1, P0). A simplification replaces F0 only if on **every fold**
  `C subset R` and `C subset K` hold, per-fold `K/U` degradation versus F0 is `<= +0.03` absolute and the mean degradation is `<= +0.02`.
  Preference among passing candidates: fewer external dependencies (no zlib), then fewer features, then simpler neighbour representation, then
  lower mean `K/U`. Otherwise F0 is retained.
- **T005 (model family)**: on the T004 schema, logistic `C in {0.25, 1.0, 4.0}` versus HGB, P0. The best logistic is the one with all folds
  `C subset R/K`, lowest mean `K/U`; ties within 0.005 prefer `C=1.0`, then smaller `C`. HGB replaces it only if all four folds `C subset R`,
  `C subset K`, mean `K/U` improves by `>= 0.05` absolute, no fold has a worse `K/U`, and it stays trivially deterministic/native-embeddable;
  otherwise `LOGISTIC RETAINED` (preferred on a tie).
- **T006 (threshold/margin policy)**: P0 = 1.00x, P1 = 0.50x, P2 = 0.25x the training minimum positive probability, each derived from the
  training titles only. Select the highest factor satisfying on all four folds: `C subset R`, `C subset K`, `margin_factor >= 2.0`, `K/U <= 0.60`,
  validator accepted; tie-break lowest mean `K/U`, then highest threshold. If none qualifies: **V1 RETAINED** (gates are never weakened).
- **Final candidate**: train on A+B+C+D with the selected schema/family/policy (threshold = factor x minimum positive probability over the four
  titles' positive windows). Two independent fits must give identical coefficients and prediction digest. Parallel artifacts
  `tools/segarecomp_ml_region_v2.{model,frozen,parity}.json` (no overwrite of v1) holding only LOTO/synthetic aggregates; the commit is the
  hard freeze (`T006_FREEZE_COMMIT`).

## 8. Numerical gates (fixed; never weakened after observing results)

- Static recall: for every LOTO fold and every formal blind title, `C subset R`, `C subset K`, validator accepts `K` - zero tolerance.
- Selectivity: `K/U <= 0.60` required, `<= 0.50` preferred. `margin_factor = (minimum probability of any positive truth window) / (selected
  threshold) >= 2.0` for every held-out/blind title.
- Economics against broad (generated-C bytes, compile CPU, runtime wall): generated C reduction `>= 30 %`, compile CPU reduction `>= 25 %`,
  runtime wall regression `<= +15 %`; v2 is also compared with native v1 (reported; never tuned).
- Runtime falsifier (secondary, only after the static gate is recorded): 0 observed PCs outside `K`, same terminal outcome, final-state digest,
  coverage digest and (where meaningful) frame-stream digest for broad vs v2; a pre-existing unrelated hardware frontier hit identically by both
  runs is recorded as such, not counted as an ML failure. v2 is exercised only through an explicit validated admission plan
  (`--admission-plan`), never through the production `--immutable-rom-aot-ml-admission` route.
- Economics scope: Shining Force, Phantasy Star II and Flicky (LOTO titles, using their *held-out fold* model for the fold evidence and the
  frozen v2 for the post-freeze run) plus any formal blind/supplementary title that is evaluated.

## 9. HARD BLIND BARRIER and seal

Before the T006 freeze commit no truth artifact may exist for E, F or G. The tooling enforces this: `seal-check` scans the private artifact
store for any `segarecomp.m68k_source_universe.v1` whose `rom_sha256` is a sealed identity and fails; the blind/control truth command refuses
without a valid freeze record (status FROZEN, candidate-file digests that match the committed bytes, and a `--freeze-commit` that is an
ancestor of HEAD containing byte-identical frozen files). Runtime coverage/oracle artifacts are refused before the freeze by the existing
`refuse_oracle_inputs`. After the freeze: no feature, schema, window-size, family, hyperparameter, coefficient, scaler, threshold, margin-rule,
certain-code-union, prune or validator change. A blind failure is evidence, never an invitation to tune. Only implementation/test/documentation
fixes that do not change frozen candidate semantics may follow, and the independent reviewer re-checks them.

## 10. Test obligations

Forbidden title/positional feature rejected; wrong ROM digest, wrong source revision, source rebuild mismatch rejected; held-out title excluded
from fold training, scaler fit and threshold fit; blind truth command rejected pre-freeze; runtime oracle/coverage rejected pre-freeze;
truth extraction deterministic; source-positive omission fails containment; schema reorder, modified model, modified threshold, modified frozen
identity and post-freeze candidate mutation rejected; v1 artifacts byte-unchanged; extractor adapters leave the Sonic 1 universe byte-identical.

## 11. Stop criteria

Stop and report (never silently substitute a title) on: a prerequisite truth failure of a fixed-corpus title, a violated barrier,
contradictory truth/rebuild correspondence, or a failed hard gate. The milestone is complete when a defensible cross-title answer exists - not
necessarily a new production model.

## 12. Pre-registered consequence of the T001 qualification result

Titles E and F are the *formal blind* pair and **both fail the qualification contract** (E: the source reproduces only a different regional
image; F: the closest variant differs by one header byte, so the rebuild is not byte-identical). The corpus is not substituted. Pre-registered
handling, fixed here before any model result exists:
1. The milestone continues through T002-T006 on the four qualified LOTO titles, so the operator receives the cross-title LOTO evidence and a
   frozen candidate regardless.
2. The formal blind gate ("E and F both source-backed, C subset R/K, margin >= 2.0, K/U <= 0.60, validator accepted") cannot be satisfied
   without a corresponding image. Therefore the **only reachable final classes are `V1 RETAINED` (LOTO/selection gates fail) or `BLOCKED`**;
   `V2 CANDIDATE JUSTIFIES SEG-049` is unreachable unless the operator supplies authorized images that rebuild byte-identically from the pinned
   sources (E: the Japanese image `8f6eb584...` SHA-1; F: an image matching a pinned variant) *before* T007 - in which case T007 runs unchanged.
3. If the LOTO/selection stages pass, the final class is `BLOCKED` with the narrow reason "formal blind prerequisite unmet (E, F)"; SEG-049 stays gated.
4. Post-freeze, non-gating supplementary evidence is allowed and clearly labelled as such: Landstalker evaluated on the *source-built BETA
   variant image* (an exact rebuild of the pinned source, one header byte different from the authorized image; truth is exact for that rebuilt
   image, not for the authorized one), and the Sonic & Knuckles control (G, exact). Neither participates in any decision above.

---

# Results (appended chronologically; sections 1-12 above are the immutable T001 protocol)

## 13. T002 - exact source truth for the four LOTO titles (commit C2)

Produced after C1, with `tools/segarecomp_source_map_extract.py` extended by the adapters of section 5, each extraction run **twice
independently with identical artifact digests** (private store `<harness>/.cache/seg048/truth/`; nothing below is address-level):

| id | opaque title | producer / config id | dialect | instruction starts C | broad U | C/U | positive 512 B windows | truth artifact SHA-256 | repro |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A | Sonic 1 REV00 | `s1disasm-listing-v1` / `Revision0` | AS Bld 212 | 24,180 | 246,293 | 0.0982 | 230 / 1024 | `fd21b96bc0d79d62152fa172e2609026822b577d126ddebe5e7f70888127fc13` | 2/2 identical |
| B | Flicky | `as-listing-v1` / `default` | AS Bld 212 | 5,707 | 64,352 | 0.0887 | 52 / 256 | `33916a8e6800a10aa3601a4c35ce5ca929067876994dd9e4c9b1c36e9cf8d6e0` | 2/2 identical |
| C | Shining Force | `asm68k-listing-v1` / `default` | ASM68K 2.53 | 44,740 | 735,560 | 0.0608 | 352 / 3072 | `7f5289dce66deecde1d3a9aa18e5ec7992ef055deb2fe9bee6936a4fd9387059` | 2/2 identical |
| D | Phantasy Star II | `as-listing-v1` / `rev2` | AS Bld 89 | 19,261 | 367,930 | 0.0523 | 145 / 1536 | `87174c5685eddc09c7ea00ac826e2ec1d5d5c470548529a2b3d25a77a17dea96` | 2/2 identical |

Sanity (all four): every address even and inside the ROM; every emitted construct explained (the whole extraction fails closed otherwise);
`C` contained in the broad `U` (0 outside); exact ROM identity (digest bound in the artifact); deterministic. The Sonic 1 universe is
**byte-identical to the pre-adapter extractor's** (same digest, 24,180 starts). Observations kept honest: in Phantasy Star II and Shining Force the
precise direct-control discovery reports 2 addresses each that fall inside a source instruction (not an instruction start) - the certain-code
union still selects the right windows; they are not truth. The ASM68K listing prints first-pass placeholders for forward-referenced
displacements (`bra.s`/`moveq` low byte) and data; the extractor therefore verifies the opcode word (high byte only for 2-byte branch/moveq),
extents and exact coverage, and relies on the byte-exact rebuild of section 4 for operand values. The ASM68K `case`-arm bytes that the
listing omits are explained only when the invoked macro is *data-only* (its echoed definition body contains no instruction and only
data-only calls). `incbin` regions in Shining Force are graphics/maps/scripts/text/sound data by directory taxonomy (no 68000 code is binary-included).
Amendment of adapter 2: `--trailing-pad` was not needed; the equivalent sound rule is "a uniform 00/FF gap ending at an `org` row" (Flicky), plus the
Z80 `save` block anchor for a rebasing `org 0` (Phantasy Star II); `tests/segarecomp_source_map_extract_test.py` covers all adapters.

Blind seal (checked at this commit): `segarecomp_ml_region_v2.py seal-check` over the whole private artifact store reports **INTACT** (no universe
bound to Alien Soldier, Land Stalker or Sonic & Knuckles exists).

## 14. T003 - frozen-v1 zero-shot comparator and v1-family LOTO baseline (commit C3)

Release CLI (`emit-general-startup-bridge-c`, native broad analysis + window export + prune + unchanged production validator), 4 vCPU. `R` = ML
windows (probability >= threshold) union the certain-code windows. `K` = admitted set after pruning. `margin_factor` = (minimum probability of any positive
truth window) / (threshold). "C outside R/K" count instruction starts of the source truth.

### 14.1 T003-A - exact committed SEG-046/047 v1 model, no retraining, no threshold change (threshold 0.00835406801187952)

| held-out | C | R windows | R/ROM | U | K | K/U | C outside R | C outside K | validator | min positive p | threshold | margin_factor |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Sonic 1 | 24180 | 257 | 0.251 | 246293 | 64,233 | 0.2608 | 0 | 0 | accepted | 9.576e-01 | 8.354e-03 | 115 |
| Flicky | 5707 | 60 | 0.234 | 64352 | 14,924 | 0.2319 | 34 | 37 | accepted | 2.074e-04 | 8.354e-03 | 0.0248 |
| Shining Force | 44740 | 409 | 0.133 | 735560 | - | - | 11 | - | rejected (`region proposal REJECTED: machine_root_not_admitted class=pruned`) | 2.922e-08 | 8.354e-03 | 3.5e-06 |
| Phantasy Star II | 19261 | 533 | 0.347 | 367930 | 127,389 | 0.3462 | 0 | 0 | accepted | 1.581e-01 | 8.354e-03 | 18.9 |

Reading: the Sonic 1 row is the model's own training title (in-sample; it reproduces the SEG-046 record exactly: R 257 windows, K 64,233, K/U 0.2608).
It is **not** cross-title evidence. On the three titles the v1 model never saw, containment holds only for Phantasy Star II (margin 18.9x);
**Flicky leaves 34 source instruction starts outside R (37 outside K)** and **Shining Force leaves 11 outside R and the unchanged prune/validator
fail-closes it (`machine_root_not_admitted`: the proposal is not structurally closed over a machine root)**. This is direct evidence about the
production v1 route (which falls back to broad AOT on a prune rejection, but silently loses code windows on Flicky).

### 14.2 T003-B - v1-family LOTO (F0 schema, logistic C=1, balanced, standardized, seed 46; scaler/model/threshold fitted on the three training titles only; P0 = minimum positive probability of the training titles' own positive windows)

| held-out | C | R windows | R/ROM | U | K | K/U | C outside R | C outside K | validator | min positive p | threshold | margin_factor |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Sonic 1 | 24180 | 222 | 0.217 | 246293 | 53,876 | 0.2187 | 377 | 1302 | accepted | 3.833e-03 | 8.029e-01 | 0.00477 |
| Flicky | 5707 | 51 | 0.199 | 64352 | 12,728 | 0.1978 | 63 | 67 | accepted | 1.149e-02 | 7.297e-01 | 0.0157 |
| Shining Force | 44740 | 347 | 0.113 | 735560 | - | - | 921 | - | rejected (`region proposal REJECTED: machine_root_not_admitted class=pruned`) | 4.565e-09 | 8.968e-01 | 5.09e-09 |
| Phantasy Star II | 19261 | 234 | 0.152 | 367930 | 55,578 | 0.1511 | 0 | 0 | accepted | 7.977e-01 | 7.683e-01 | 1.04 |

Reading: the retrained-v1 family is *worse* than the single-title v1 on every held-out title except Phantasy Star II: the in-sample minimum positive
probability of a nearly separable 141-feature logistic fit is a high, fragile threshold (0.73-0.90), and one low-scoring genuine code window in a
held-out title (Shining Force's weakest positive window scores 4.6e-9; in the zero-shot run its weakest window is ordinary code - 101 source instruction starts, decoder identification density 0.98, 22 % zero bytes - so this is a real cross-title distribution shift, not a truth artifact) breaks
containment. Only Phantasy Star II passes the static gates, with margin 1.04 (< 2.0). The two baselines are separate evidence and are not conflated.

## 15. T004 - pre-registered feature ablation (commit C4)

F0..F5 exactly as defined in section 6, each under T003-B conditions (logistic C=1, P0, four LOTO folds). Each cell is
`C-outside-R / C-outside-K / K/U / margin_factor` (`REJ` = the unchanged prune/validator rejected the proposal, K unavailable).

| candidate | features | Sonic 1 | Flicky | Shining Force | Phantasy Star II | all folds C subset R and K |
| --- | --- | --- | --- | --- | --- | --- |
| F0 | 141 | R-377 / K-1302 / 0.219 / m=0.00477 | R-63 / K-67 / 0.198 / m=0.0157 | R-921 / K-- / - REJ / m=5.09e-09 | R-0 / K-0 / 0.151 / m=1.04 | **no** |
| F1 | 138 | R-426 / K-1420 / 0.217 / m=0.00388 | R-63 / K-67 / 0.198 / m=0.019 | R-921 / K-- / - REJ / m=1.32e-08 | R-0 / K-0 / 0.145 / m=1 | **no** |
| F2 | 138 | R-426 / K-1420 / 0.217 / m=0.00382 | R-63 / K-67 / 0.198 / m=0.0286 | R-969 / K-- / - REJ / m=6.44e-09 | R-138 / K-234 / 0.122 / m=0.9 | **no** |
| F3 | 46 | R-22 / K-22 / 0.303 / m=0.0142 | R-0 / K-0 / 0.221 / m=14.6 | R-154 / K-- / - REJ / m=0.00756 | R-0 / K-0 / 0.360 / m=22.5 | **no** |
| F4 | 93 | R-251 / K-833 / 0.223 / m=0.00102 | R-21 / K-22 / 0.202 / m=0.142 | R-897 / K-- / - REJ / m=2.59e-08 | R-280 / K-1126 / 0.117 / m=0.639 | **no** |
| F5 | 45 | R-0 / K-0 / 0.966 / m=40.1 | R-13 / K-15 / 0.988 / m=0.606 | R-1589 / K-- / - REJ / m=5.74e-16 | R-0 / K-0 / 0.960 / m=1.92e+03 | **no** |

Schema hashes: F0 `d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570` (== v1); the others are derived from `seg048-features-v2-F<k>`
and recorded in `tools/segarecomp_ml_region_v2.py` (`schema_hash`).

Decision under the frozen rule (a simplification may replace F0 only if **every fold** has `C subset R` and `C subset K` and the per-fold/mean `K/U`
degradation bounds hold): **no candidate qualifies - every one of F0..F5 leaves source instruction starts outside R in at least one held-out fold,
and Shining Force is rejected by the prune/validator for F0-F5.** Therefore **F0 (the v1 schema, 141 features) is retained**; there is nothing to
re-version. Observations (descriptive, not used for any selection): removing `b_zlib` (F1) changes nothing material; replicating the edge (F2) does not
help; dropping the neighbour context (F3) is the only variant that contains Flicky and Phantasy Star II (and loses only 22 Sonic 1 starts and 154 Shining
Force starts) but at K/U 0.22-0.36; the decoder-only schema (F4) loses recall; the byte-only schema (F5) selects 73-99 % of every ROM, i.e. it is a
non-selective model. These do not satisfy the pre-registered gate and are not pursued (no feature fishing).

## 16. T005 - bounded model-family comparison on the retained F0 schema (commit C5)

Logistic regression `C in {0.25, 1.0, 4.0}` (standardized, balanced, seed 46) versus the single pre-registered comparator
`HistGradientBoostingClassifier(max_iter=60, max_depth=3, learning_rate=0.1, min_samples_leaf=20, early_stopping=False, balanced, seed 46)`; P0 thresholds, four LOTO folds.

| model | features | Sonic 1 | Flicky | Shining Force | Phantasy Star II | all folds C subset R and K |
| --- | --- | --- | --- | --- | --- | --- |
| logistic C 0.25 | 141 | R-191 / K-980 / 0.224 / m=0.0381 | R-63 / K-67 / 0.202 / m=0.0304 | R-736 / K-- / - REJ / m=1.54e-05 | R-0 / K-0 / 0.157 / m=1.46 | **no** |
| logistic C 1.0 (T003-B) | 141 | R-377 / K-1302 / 0.219 / m=0.00477 | R-63 / K-67 / 0.198 / m=0.0157 | R-921 / K-- / - REJ / m=5.09e-09 | R-0 / K-0 / 0.151 / m=1.04 | **no** |
| logistic C 4.0 | 141 | R-505 / K-1530 / 0.214 / m=0.000127 | R-63 / K-67 / 0.194 / m=0.0011 | R-927 / K-- / - REJ / m=9.45e-14 | R-280 / K-1126 / 0.128 / m=0.384 | **no** |
| HGB | 141 | R-125 / K-544 / 0.227 / m=0.0396 | R-21 / K-22 / 0.198 / m=0.15 | R-1827 / K-- / - REJ / m=0.00775 | R-0 / K-0 / 0.099 / m=1.07 | **no** |


Decision: **LOGISTIC RETAINED.** No logistic setting has all four folds `C subset R`/`C subset K` (so no "best logistic by mean K/U" exists under the
section 7 rule, which presumed at least one qualifying setting); the pre-registered default `C = 1.0` (the v1 recipe, preferred on a tie) is therefore
carried forward unchanged to T006. The HGB comparator also fails its own admission criteria (Flicky 21 and Sonic 1 125 starts outside R; Shining Force
1,827 outside R and rejected by the prune) and the required `>= 0.05` mean `K/U` improvement is not even defined without a qualifying baseline; it is not
simpler to embed than a logistic. This choice of `C = 1.0` is the baseline value fixed in advance, not a result-tuned choice.

## 17. T006 - cross-title threshold/margin policy and the freeze decision (commit C6 = `T006_FREEZE_COMMIT`)

F0 + logistic C=1 (the carried-forward defaults of T004/T005); policies `P0 = 1.00x`, `P1 = 0.50x`, `P2 = 0.25x` the minimum positive probability of the training
titles' own positive windows; every threshold derived from the three training titles only; four LOTO folds:

| policy | features | Sonic 1 | Flicky | Shining Force | Phantasy Star II | all folds C subset R and K |
| --- | --- | --- | --- | --- | --- | --- |
| P0 (1.00x) | 141 | R-377 / K-1302 / 0.219 / m=0.00477 | R-63 / K-67 / 0.198 / m=0.0157 | R-921 / K-- / - REJ / m=5.09e-09 | R-0 / K-0 / 0.151 / m=1.04 | **no** |
| P1 (0.50x) | 141 | R-191 / K-980 / 0.224 / m=0.00955 | R-63 / K-67 / 0.202 / m=0.0315 | R-440 / K-- / - REJ / m=1.02e-08 | R-0 / K-0 / 0.180 / m=2.08 | **no** |
| P2 (0.25x) | 141 | R-109 / K-486 / 0.229 / m=0.0191 | R-63 / K-67 / 0.202 / m=0.063 | R-45 / K-- / - REJ / m=2.04e-08 | R-0 / K-0 / 0.203 / m=4.15 | **no** |

Required on all four folds: `C subset R`, `C subset K`, `margin_factor >= 2.0`, `K/U <= 0.60`, validator accepted. **No policy satisfies any fold set**:
Shining Force is rejected by the unchanged prune/validator under every policy (and still leaves 45 starts outside R at the most permissive 0.25x), Sonic 1
leaves 109-377 starts outside R, Flicky leaves 63 outside R under all three, and Phantasy Star II alone passes the static gates (from 0.50x on; margin
2.08 and 4.15). Lowering the factor further would be a new policy search and is forbidden (section 7).

**Decision (pre-registered rule): `V1 RETAINED`.** No v2 candidate qualifies, so **no v2 model, parity or candidate artifact is produced** (training
a "least bad" model would fabricate a candidate the protocol does not allow). The hard-freeze record
`tools/segarecomp_ml_region_v2.frozen.json` (`status: NO_QUALIFYING_CANDIDATE`) records the decision, the per-policy fold aggregates, the corpus/truth
digests, the environment and the SHA-256 of every v1 artifact. The barrier tool (`verify-freeze`) accepts exactly this record (and a `FROZEN` candidate
record) and also pins the v1 artifacts to their recorded digests. Amendment note: section 9 anticipated only a `FROZEN` candidate record; the same
barrier discipline applies to the negative record, no gate was changed, and its only effect is to allow the post-freeze supplementary evaluation of
the *retained v1 model* described in section 12.4. From this commit on nothing about the model, schema, thresholds, margin rule, union, prune or
validator may change.

Final-answer implication already fixed here: the formal blind gate is moot as a model decision (there is no v2 to gate) in addition to being
unsatisfiable for lack of corresponding images (section 12). Production Optimized AOT is untouched: v1 stays the production optimized model.
