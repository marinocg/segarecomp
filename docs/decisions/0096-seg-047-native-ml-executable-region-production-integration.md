# ADR 0096: SEG-047 native ML executable-region production integration (contract)

- Status: Accepted at T001 (contract frozen before any native code). Gate outcome is appended by T010.
- Predecessors: ADR 0095 (SEG-046 frozen v1 region model, parity artifacts), ADR 0094 (SEG-045 pruning, FLOW8-D failure), ADR 0093.

## 1. Question and non-goals

Can the frozen SEG-046 v1 region model run as a dependency-light NATIVE capability that reproduces the committed selected-window sets exactly
on the five parity titles and then feed the unchanged `proposal -> structural pruning -> admission plan -> validator -> selective generated-native
AOT` path as an opt-in mode? No retraining, feature change, threshold change, model change, runtime ML, generic ML framework or `libs/ml`, and no
Python/numpy/sklearn/ONNX product dependency. ML is a heuristic proposal producer for `R`; the validator remains the correctness authority.

## 2. Frozen identity (any mismatch fails closed)

| item | value |
| --- | --- |
| feature version | `seg046-features-v1` |
| feature count / window | 141 / 512 bytes |
| CPU / owner | MC68000 / Genesis machine platform admission code (beside `hybrid_admission` and region pruning) |
| probability threshold | `0.00835406801187952` |
| logit threshold | `-4.776617512896361` |
| feature-schema SHA-256 | `d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570` |
| original sklearn artifact SHA-256 | `b5ddae5aa0fe6571455803c244d2a6be4a2c3349e3436c8780bfc3d8aa8fa62b` |
| canonical model JSON SHA-256 | `5863742d3b84a934d7ab335695a3748426de53e43ddc9d42ca679a6fc9fab0a2` (`tools/segarecomp_ml_region.model.json`) |
| parity artifact | `tools/segarecomp_ml_region.parity.json` (selection digests, five titles) |

The native producer checks, at startup of every use, the feature version string, the feature count, the window size and the schema digest of
its embedded model against these constants and refuses to propose (reason `model_identity`) on any difference. The checks are compiled-in
constants compared to the embedded table's own metadata; a drift test (section 7) ties the table to the committed JSON.

## 3. Feature contract

Feature order is exactly `base ++ prev_base ++ next_base`, `base = [16 byte features] ++ [27+4 decoder features]` as in
`tools/segarecomp_ml_region.py` (`feature_names()`; names are embedded and compared to the schema digest input). Byte features (binary64):
`b_entropy = H/8` (`H = -Σ p·log2 p` over non-zero byte counts, summed in ascending byte value), `b_zero`, `b_ff`, `b_printable`
(bytes 0x20..0x7e), `b_unique = distinct/256`, `b_longest_run = longest/n`, `b_word_repeat` (equal consecutive big-endian words over
`max(words-1,1)`), `b_zlib = min(len(zlib_level6(window))/n, 2.0)`, `b_hist0..7` (32-value bins / n). `n` is the window length; the
last window may be short (n < 512), an empty window yields 16 zeros. Decoder features are the C++ `genesis_window_feature_report` columns
transformed exactly as `decoder_features` (ident/words, per-ident ratios, `edge_in_*` per words, four edge ratios; windows absent from the
report are all-zero). Missing neighbours (before window 0, after the last window) are the all-zero base vector, **frozen as-is for v1**; no
other treatment and no v2 idea is introduced. Features are computed over every window of the image `[0, ceil(size/512))`.

## 4. Numeric representation and scoring (determinism)

* All parameters and features are IEEE-754 binary64 (`double`). Decimal literals are the shortest round-trip repr from the model JSON; every
  supported compiler parses decimal floating literals with correct rounding, so the embedded table equals the JSON bit for bit (drift test
  compares a re-printed `%.17g` round trip).
* Score: `logit = folded_bias; for i in 0..140: logit += folded_weight[i] * x[i]` — a plain left-to-right accumulation in index order, one
  binary64 multiply and one binary64 add per feature. A window is selected iff `logit >= logit_threshold` (non-strict, binary64 compare, exactly
  the canonical rule). No sigmoid, no exp, no probability.
* FMA/contraction: the scorer and feature translation units are built with `-ffp-contract=off` (GCC/Clang; MSVC `/fp:precise` plus
  `/fp:contract-` equivalent not needed on the supported zig/clang/gcc builds) and every product is stored to a `volatile`-free named `double`
  before the add so that no compiler may fuse it; x87 extended precision is excluded because supported targets are SSE2/AArch64. `-ffast-math`
  and reassociation are forbidden for these files.
* The Python reference sums with `math.fsum` (exactly rounded), so native and reference may differ in the last ulps (measured canonical vs sklearn
  1.8e-14). The contract is therefore **selected-window set equality**, not probability or logit bits. The gate additionally reports the minimum
  absolute distance of any window's logit from the threshold per title; an ulp-scale distance (< 1e-9) would be recorded as a fragility and
  STOP (it cannot occur silently). The log2 in `b_entropy` and any libm difference (<= 1 ulp) lie far below the observed margins (ADR 0095: weakest
  executed window 1.12x the probability threshold, i.e. ~0.11 logit units).
* Synthetic check required by T003: folded vs canonical-unfolded logits agree to < 1e-12, the decision flips exactly at the threshold
  neighbourhood, and compiling the scorer with and without contraction flags yields identical selections.

## 5. zlib decision (single owner: this ADR)

`b_zlib` needs the length of `zlib.compress(window, 6)` (zlib wrapper: 2-byte header, deflate stream, 4-byte Adler-32; level 6, default
strategy, windowBits 15, memLevel 8, no dictionary, single-shot). A custom DEFLATE encoder or size model is rejected, and the feature is not
changed (SEG-048 owns any removal).

Decision: **vendor a pinned subset of the real zlib 1.3.1 C sources** under `third_party/zlib/` (`adler32.c deflate.c trees.c zutil.c` and their
headers, unmodified apart from provenance comments; zlib license kept in the tree as `third_party/zlib/LICENSE` and listed in `packaging/THIRD-PARTY-NOTICES.txt`, shipped as `licenses/zlib.txt`), compiled as a private static
library with the machine platform; one small local `zlib_compress6` wrapper calls `deflateInit/deflate(Z_FINISH)/deflateEnd` exactly as zlib's
`compress2` does. `deflate.c` references `crc32` only in gzip-wrapper paths (`windowBits` > 15), which the wrapper never selects; a minimal local
`crc32` definition satisfies the linker for those unreachable paths. The decision rests on: (a) the release builds are zig cross-builds for
Linux/macOS/Windows that have no system zlib; (b) a system zlib may be zlib-ng, Apple- or Chromium-modified and produce different lengths, which
would silently change classifications; (c) the sources are small, license-compatible (zlib license) and pinned by SHA-256 recorded in the
tree. Exact reproduction is proven against the Python 3.13 reference build (system zlib 1.3.1) in two ways: T002 compares compressed lengths for
a fixed synthetic corpus, and T004 compares the selected-window digests of all five titles. Supported platforms are exactly the product's
existing release targets; if the five-title digests cannot be reproduced there with the vendored zlib, SEG-047 STOPS at T001/T004 and reports the
contradiction (no workaround by feature or threshold change).

## 6. Pipeline contract

Exact source/recomp map (when supplied) always outranks ML: if a source-derived selective plan exists the ML producer is not consulted. Otherwise,
in Optimized mode the pipeline is: native ML proposes `R` (selected windows ∪ certain-code windows from the precise direct-control discovery, as
in SEG-046) → unchanged `prune_genesis_region_admission` derives `K` → unchanged hybrid-plan validator → ordinary selective generated-native
AOT. Any producer failure (identity mismatch, resource bound, empty/rejected proposal, prune rejection, validator rejection) falls back to
the broad immutable-ROM universe with a stable sanitized reason; it never weakens or bypasses validation and never emits a partial program.
Broad AOT remains the unconditional compatibility fallback and the default through SEG-047. The generated runtime remains ML-free: no
interpreter, JIT, runtime decoder or runtime model.

## 7. Ownership, embedding, reference oracle

The producer lives in the Genesis machine platform next to `hybrid_admission` (no `libs/ml`, no universal producer interface before a second
CPU). The model is embedded as a **checked-in generated C++ table** (`ml_region_model_v1_data.inc`, binary64 decimal literals, feature names and
identity constants) generated from `tools/segarecomp_ml_region.model.json` by a small deterministic Python generator; the release/CI product
build never runs Python. A drift test regenerates the table and requires byte equality plus identity-constant equality. `tools/segarecomp_ml_region.py`
remains a non-shipped reference/test oracle; the private sklearn pickle stays reference-only evidence, never required by the product or CI, and
is retired as a requirement once T004 passes.

## 8. Pre-registered gates and STOP conditions

| gate | pass condition | task |
| --- | --- | --- |
| contract | this ADR, real-zlib decision recorded | T001 |
| feature exactness | synthetic feature-semantics tests incl. zlib length table, missing neighbours | T002 |
| scorer | folded vs canonical, boundary, contraction-independence | T003 |
| **parity** | native ML-only and final region digests equal `parity.json` for Sonic 1, Sonic 2, Cool Spot, Streets of Rage, OutRun; min logit margin recorded | T004 |
| integration | proposal → prune → validator accepts; K counts equal ADR 0095 §4 (Sonic 2 137,230; Cool Spot 56,980; Streets 61,374; OutRun 78,283; Sonic 1 64,233); bounded broad-vs-selective differential equal digests, 0 escapes | T005 |
| economics | generated C and compile CPU reductions each within 5 percentage points of ADR 0095 §5 per title (K identical ⇒ expected equal) | T005/T010 |
| retired route | FLOW8-D producer, tests and CLI surface absent | T006 |
| cleanup | no report-only analysis framework on the product/release graph; broad `U` count/digest unchanged on the five titles (with/without external hints compared) | T007 |
| Python-free | clean build/test of the product with no Python available to the build | T010 |
| CLI/launcher | Optimized vs Compatibility parity, mode-safe cache identity, visible fallback | T008/T009 |

STOP conditions (leave the single PR open and draft, mark the task BLOCKED, report last good SHA): vendored zlib cannot reproduce parity;
any selected-set mismatch (no epsilon, threshold, coefficient, feature, schema or window exclusion repair); validator or frozen model must be
weakened; deleting analysis code changes a required correctness input and needs a new architecture decision; economics outside tolerance without
a K difference; supported-platform nondeterminism.

## 9. CLI and cache scope (T008/T009)

The public concept is an optimization policy `compatibility | optimized`, not ML configuration: no model path, threshold, coefficient or schema
option is user-visible. Compatibility stays the default in SEG-047. The existing machine-readable build status gains requested mode, effective
producer (`broad` | `ml_region`), fallback flag and stable sanitized reason, model/schema identity, candidate/admitted metrics, validator status
and a stable cache identity that includes the mode, model identity and the K digest, so Compatibility and Optimized artifacts never alias. The
launcher is a thin consumer of that interface.

## 10. Amendments discovered during implementation (no model or feature change)

- T002: the v1 reference computes `b_entropy` with Python's builtin `sum()`, which since CPython 3.12 is Neumaier-compensated (the frozen
  features were produced by Python 3.13.5). The native extraction therefore uses the same compensated left-to-right (ascending byte value)
  summation; a plain accumulation differs by ~2e-15. This is a reproduction of the existing v1 definition, not a feature change. Only `b_entropy`
  depends on libm `log2`; the native test allows 1e-15 there and requires equality for every other feature.
- T002: the vendored subset is the files found in the zlib 1.3.1 release that product needs plus one local `crc32` definition (see
  `third_party/zlib/README.md`); drift is guarded by recorded SHA-256 digests in `tests/segarecomp_ml_region_test.py`.

## 11. T004 hard gate result: exact five-title native parity (PASS)

`segarecomp emit-general-startup-bridge-c ... --immutable-rom-aot --ml-region-proposal-output` (report-only; native features + folded scorer +
union with the precise direct-control discovery windows) was run on the five locally held parity images by
`tools/segarecomp_ml_region_native_parity.py` (also CTest `genesis_ml_region_native_parity_local_test`, skipped when no parity image is held).
For every title the window count, ML-selected count, seed-window count, final-selected count, region bytes, the SHA-256 of the final
canonical regions text, and the SHA-256 of the ML-only regions text equal `tools/segarecomp_ml_region.parity.json` EXACTLY
(Sonic 1 257/25, Sonic 2 560/16, Cool Spot 244/14, Streets of Rage 247/21, OutRun 341/23 selected/seed windows; digests
e5210d14…, 1a34f38e…, 7260ec30…, fcfcf223…, 8e310d36…). No threshold, coefficient, feature, schema, epsilon or window exclusion was touched.
Minimum |logit − threshold| per title (the exact-parity fragility measure): Sonic 1 0.048, Sonic 2 0.014, Streets of Rage 0.011, OutRun
2.95e-4, Cool Spot 4.1e-5 — all more than eight orders of magnitude above the binary64 accumulation error (~1e-14), so the result is not
sensitive to summation order, libm `log2` last-ulp differences or FMA. Reproduced with GCC 14.2 aarch64 `-O3` (Release) and `-O0` (Debug) builds.
Clang could not be used as a second product compiler in this container because `frontend.cpp` already fails to build with clang +
libstdc++ (incomplete `JsonValue` in a `std::pair`, unrelated to this PR); x86-64, macOS and Windows determinism is delegated to the CI
matrix and re-checked at T010.

## 12. T005 integration result (PASS)

Final admission architecture (Genesis M68K, `segarecomp emit-general-startup-bridge-c --immutable-rom-aot --immutable-rom-aot-ml-admission`, and
`segarecomp build --aot-policy optimized`):

```
broad immutable-ROM universe U (decoder, unchanged)
  -> native ML region proposal R  (frozen v1 model; R = ML windows ∪ precise direct-control-discovery windows)
  -> unchanged structural pruning  K = greatest closed subset of U ∩ R
  -> ordinary hybrid admission plan for K
  -> unchanged apply_genesis_hybrid_admission validator on the real entries
  -> ordinary selective generated-native AOT (runtime unchanged, ML-free)
```
Any producer failure (`model_identity`, `rom_size`, `empty_proposal`, `prune_rejected[outside_region|pruned]`, `validator_rejected`, `no_analysis`)
broadens to the full universe with one sanitized `m68k admission:` report line; nothing is filtered on failure and the validator is never
weakened or bypassed. An explicit `--immutable-rom-aot-admission` plan (the exact source/recomp-map route) is mutually exclusive with the ML
option at the emitter and, in `segarecomp build`, outranks `--aot-policy optimized` (reported as `effective=admission_plan`,
`reason=exact_plan_precedence`). Copy-alias preparation emissions run the same producer per emission, so no alias-set mismatch can silently
re-broaden a plan.

K counts equal ADR 0095 §4 exactly (the proposal is bit-identical, pruning is unchanged): Sonic 1 64,233 (pruned 277, 42 rounds); Sonic 2
137,230 (3,209, 80); Cool Spot 56,980 (3,479, 77); Streets of Rage 61,374 (397, 70); OutRun 78,283 (6,006, 427). The generated C of the
integrated route is byte-identical to the SEG-046 explicit-plan route (checked on Streets of Rage by SHA-256 of the full emission).

Bounded broad-vs-selective differential (`tools/genesis_ml_admission_differential.py`: unchanged bridge, clang, 600 no-render frames,
explicit instruction budget; final-state, frame-stream and coverage digests and outcome compared): all five titles `frames_reached`, digests
EQUAL; generated C reduction (sharded tree bytes): Sonic 1 −73.8%, Sonic 2 −71.2%, Cool Spot −81.1%, Streets of Rage −70.2%, OutRun −73.8%;
bridge wall (build + 600 frames, 4 vCPU, noisy): 58→14 s, 96→27 s, 87→14 s, 49→15 s, 60→14 s. The 23,200-frame execution-PC oracle bitmaps of
SEG-046 are not present in this checkout, so the long-trace escape check is carried by K-identity with the SEG-046 plan (same K ⇒ same
evidence) rather than re-measured; T010 treats this honestly. Synthetic end-to-end (`genesis_ml_admission_cli_test`): a recall miss of a
dynamic-jump island yields a fail-closed guest stop, never wrong execution. gcc `-Werror` rejects constant division in generated broad C, so the
differential uses clang for the generated programs (as SEG-046 did).

## 13. T006 retirement of the FLOW8-D coarse region producer

Removed: `tools/segarecomp_region_proposal.py` (the frozen FLOW8-D page policy), `tests/segarecomp_region_proposal_test.py` and its CTest, the
`--page-structure-report` option and its per-2-KiB-bin report in the CLI. Retained: the region-proposal text representation
(`segarecomp.m68k_executable_regions.v1`) and `--immutable-aot-region-proposal`/`--region-admission-plan-output` (used by the native producer's
parity digest, the exact-map plan route and `tools/segarecomp_region_oracle_compare.py`), structural pruning, admission validation, the
broad universe, exact-map support, `--window-feature-report` (the Python reference oracle's input) and the historical ADRs 0094/0095. (The two
deleted files appear in the T005 commit because they were staged with `git rm` before the commits were split.)

## 14. T007 decommission of the report-only analysis architecture (audit-driven)

A live-consumer audit (CMake graph, includes, CTest, tools, release packaging, launcher) preceded every deletion. The abstract-analysis
framework had no product consumer: `analysis_build_graph_test` had always proved that no production target linked it; the release and
launcher builds never used it; its only runtime-adjacent role was producing offline hybrid plans and external-fact reports.

| item | decision | evidence |
| --- | --- | --- |
| `libs/analysis` (SEG-029 generic solver) | REMOVE | consumed only by the two CPU adapters, the driver and their tests |
| `libs/cpu/m68k/analysis`, `libs/cpu/z80/analysis` | REMOVE | report-only adapters; Z80 adapter synthetic-only |
| `platforms/genesis/analysis_report` (driver + library) | REMOVE | not linked by `segarecomp`, the launcher or any release target; planner replaced by the native ML producer |
| 21 `analysis_*` test sources, the mutation harness + selftest, `analysis_build_graph/core_boundary` guards, `genesis_hybrid_admission_{differential,generated}_test` | REMOVE | all required the removed targets; validator coverage retained by `genesis_hybrid_admission_test`, `genesis_region_prune_test`, `genesis_ml_*` and `genesis_ml_admission_cli_test` |
| `apps/m68k-primary-word-classify`, `tools/segarecomp_recomp_map_harvest.py`, `tools/segarecomp_angr_m68k_facts.py` (+ tests) | REMOVE | external-proof producers feeding the removed `--external-m68k-facts` consumer; the classifier helper's only consumer was the harvester |
| `tools/genesis_hybrid_admission_compare.py` | REMOVE | needed the driver; replaced by `tools/genesis_ml_admission_differential.py` |
| active doc presenting the framework as architecture | UPDATED | `abstract-analysis-core-contract.md` carries a RETIRED banner (historical ADR input); the scalability doc describes Optimized AOT |
| `hybrid_admission` plan format/validator, structural pruning, region proposal format, machine roots, broad universe, universe hashes, ML features, window-feature export, reachability challenger, `segarecomp_region_oracle_compare.py`, startup bridge, `segarecomp_source_map_extract.py` + `segarecomp_source_universe_plan.py` (exact source/recomp-map route) | KEEP | live correctness/admission inputs or useful differential/oracle tooling |
| `--external-hints`, `runtime/compat/<rom-sha>.json` | KEEP (AUDITED) | see below |
| Ghidra tooling (`tools/ghidra*`, MCP config) | KEEP, optional | developer-only diagnostic evidence; `report_only_analysis_absent_test` asserts no build file references it; no product/release/launcher path depends on it |

Removed: 62 files, ~24.4k lines. The replacement guard `report_only_analysis_absent_test` fails if a retired directory, target, include or a
Ghidra build dependency returns.

External-hints audit (broad `U`, ML proposal `R`, final admitted `K`, five parity titles, hints off vs on where a hint file exists):
`U` count and digest are IDENTICAL with and without hints on both titles that have a hint file (Sonic 1 `logical_table_descriptor`
shipped file; Streets of Rage 4,425 local Ghidra `code_entry_candidate`/`address_table_candidate` records), and the other three titles have no
hint file. `U` therefore never silently depends on hints. Hints are NOT inert for emitted C, however: the committed Sonic 1 table
descriptors change the generated program by ~55 KB (R and K unchanged, `K` digest identical, one extra certain-code window already selected
by the model); the unshipped Streets of Rage candidates add certain-code windows (R digest and K change: 61,374 → 77,163) and ~80 MB of
generated C. Disposition: the opt-in hint parsers live in the product frontend (not in the analysis framework), the shipped file affects
generated code, and removing them is a frontend semantic change that needs its own behavioural evidence, so they are RETAINED unchanged as
explicit opt-in inputs (nothing reads a candidate hint implicitly; only the committed Sonic 1 descriptor file is auto-applied). Pruning the
unshipped `code_entry_candidate` / `address_table_candidate` / code-pointer-table kinds is deferred to SEG-052 (analysis prioritization).

After the cleanup: the full dev build succeeds; `U` counts (246,293 / 496,387 / 498,276 / 247,761 / 496,950) and, for Sonic 1 and Streets
of Rage, the `U` digest and the broad generated-C SHA-256 equal their pre-cleanup values; the five-title native parity is still exact.

## 15. T008 optimized-AOT CLI (PASS)

`segarecomp build --aot-policy <compatibility|optimized>` (default `compatibility`; unknown values are a usage error, exit 2). No model,
threshold, coefficient or schema option exists. The existing machine-readable seam (`status.json`) gains an always-present `aot_policy` object
(requested mode, effective producer `broad|ml_region|admission_plan`, fallback flag, stable sanitized reason code, model/schema identity,
candidate/admitted aggregates, validator status and a stable `identity` digest; see `docs/development/aot-policy.md`). Tested by
`segarecomp_build_command_test` (default Compatibility report; Optimized report with distinct identity; fallback reasons are stable codes;
exact-plan precedence; invalid value rejected) and `genesis_ml_admission_cli_test`. Master System images accept the flag and report
`platform_not_applicable` (Genesis/SMS behaviour unchanged). Compatibility remains the default.

## 16. T009 launcher integration (PASS)

The existing SDL3/Dear ImGui launcher stays a thin consumer of `segarecomp build`: it gains a two-option Mode control (Compatibility, the
default; Optimized) for Genesis images, with no model, threshold or schema control and no inference in launcher code. The choice is passed as
`--aot-policy`, so CLI and launcher share the same policy semantics. The cache identity includes the policy for Genesis (`genesis` for
Compatibility keeps the historical key; `genesis:aot=optimized` for Optimized), so the two modes never alias a cache entry (a Master System
image ignores the request, as it has no Optimized producer). After a build or on a cache hit the launcher reads the machine-readable
`aot_policy` member of status.json and shows the requested vs effective result under "Native build ready": "Compatibility build",
"Optimized build", "Optimized build (exact map)" or, in the error colour, "Optimized unavailable for this game: built Compatibility" with a
"Why?" button that opens the sanitized reason code in the diagnostics panel. The ROM -> build -> ready -> play flow and the Master System
mapper flow are unchanged. The headless smoke mode accepts `--aot-policy` and reports `aot_policy_requested/effective/fallback/reason`.
The launcher's non-UI core became its own target (`segarecomp_launcher_core`) so `launcher_policy_test` can assert, without a window: default
Compatibility, Genesis-only Optimized, distinct and deterministic cache entries, status parsing and fallback wording; the SDL3 CI job builds and runs it through CTest on macOS, Linux and Windows (`ctest -R launcher_policy_test`, a plain CI step, never POST_BUILD, because a POST_BUILD run broke the Windows release build, which cannot find `SDL3.dll` at build time; wiring added in the post-merge hygiene PR, see the addendum). Verification here: the launcher links and its contract test passed on Linux (then run as a POST_BUILD step, since removed) against a static SDL 3.4.16 build; the GUI was
rendered headless (SDL dummy video + software renderer, a fake package home and cache) and the screenshots reviewed for the Genesis ROM
selected state (Mode row: Compatibility selected, Optimized available), a Compatibility Ready entry ("Compatibility build"), and an Optimized
Ready entry that fell back (distinct cache entry loaded; red "Optimized unavailable for this game: built Compatibility" and a "Why?" button).
The interactive click paths, the real zig toolchain build and Windows/macOS rendering are not exercised in this container (SDL3 CI job builds
the launcher; T010 reviews the code path).

## 17. T010 independent adversarial gate

Independent reviewer (separate session) on head 1212779: **PASS WITH FINDINGS, no blocking finding.** Reproduced: five-title exact native parity
(also re-derived with the pure-Python reference for three titles); 0 vendored-zlib length mismatches on 22,000 buffers; Python-free configure+build;
mutation rejection of feature-name/NaN/version/probability-threshold edits; byte-identical fallback emission; mutual exclusion of plan and ML
option; `U` count/digest, window-feature report and full broad generated-C SHA-256 IDENTICAL to origin/main on all five titles; K counts equal
ADR 0095; hints audit reproduced; Streets of Rage real `segarecomp build` Compatibility → Optimized: generated C −70.2%, compile CPU −62.8%
(SEG-046: −65.6% / −65.2%; within the 5 pp tolerance on the sharded-tree measure only; single-file measure −72.1% is 6.5 pp away), wall −58.7%.
Fixed in response: (1) the runtime identity check now also binds the logit threshold, the folded bias and a SHA-256 of the folded weight table;
(3) `status.json` reports `effective=broad, fallback=true, reason=plan_alias_set_mismatch` when an explicit plan did not apply to an emission;
(4) stale bridge help text. Accepted non-blocking: coarse `prune_rejected` detail; cross-platform (MSVC/clang-cl/x87/libm) determinism argued
by margin (≥4.1e-5 vs ~1e-13 error) and left to the CI matrix; 23,200-frame oracle not re-measured (R bit-identical, K counts identical);
`segarecomp_build_command_test` (fdopen hook) and `m68k_conformance_harness_test` (killed) fail identically on origin/main.


## Addendum (post-merge hygiene, no behaviour change)

**Final-K identity.** Digest = SHA-256 of the sorted admitted source identities as `%08x\n` lines (the same digest the production route logs as
`k_sha256`; no address listing is stored). Reference route: window features from the C++ export -> pure-Python canonical model
(`tools/segarecomp_ml_region.py`, model JSON; the private sklearn pickle is not required) -> regions file (SHA-256 equal to the committed SEG-046
`regions_sha256` for all five titles) -> explicit `--immutable-aot-region-proposal` plan -> K = universe ∩ plan ranges. Native route: the integrated
`--immutable-rom-aot-ml-admission`. Both sets were produced from the same current binary.

| title | K | K SHA-256 | reference vs native |
| --- | --- | --- | --- |
| Sonic 1 | 64,233 | `c195721ca2170533e8acdb3dd7a753657acb419acf56e83d3de18b9eb82d122e` | equal |
| Sonic 2 | 137,230 | `0948039b5bcad5ac2d6f38e60c8e69a183e12a7b74742e7905a22c4255506c95` | equal |
| Cool Spot | 56,980 | `41c37f6ece4d65eb10f1851f2378916c4e1df3d3ef0a309527e2c5c3ff3aa7d9` | equal |
| Streets of Rage | 61,374 | `ca2c0f94812fc2cceee089c81b338708dab39af9d6f06e8cf71a1ae356a1fd8e` | equal |
| OutRun | 78,283 | `77ad497c5f8158a9bb0091cd98756f518a404cab7a66493453060d53cffadaf8` | equal |

**Long-run differential (rerun, same tooling).** `tools/genesis_ml_admission_differential.py` workload at 23,200 no-render frames (clang 18, explicit
instruction budget), broad vs integrated optimized, plus a second optimized run for determinism: all five titles `frames_reached` x3; outcome,
frames, final-state, frame-stream and coverage digests equal broad-vs-optimized and optimized-vs-optimized-repeat; coverage digest equality means the
observed execution-PC set is identical, so 0 PCs outside K (an escape would stop or change coverage). Generated C reduction: Sonic 1 -73.8%, Sonic 2
-71.2%, Cool Spot -81.1%, Streets of Rage -70.2%, OutRun -73.8%. The original SEG-046 bitmaps were not used or recreated. No model, threshold or
schema change.

**Notices.** The zlib license is `third_party/zlib/LICENSE`, listed in `packaging/THIRD-PARTY-NOTICES.txt` and installed as `licenses/zlib.txt`.
**CI.** `launcher_policy_test` is registered with CTest and run by the SDL3 CI job on macOS, Linux and Windows (`ctest -R launcher_policy_test`), never POST_BUILD.
