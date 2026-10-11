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

---

# Results (checkpoint C1b; appended after sections 1-4 were committed as `1ee15c5`)

## 5. Baselines and tools

| item | value |
| --- | --- |
| product `main` at synchronization | `7814e35d1dc8bdf3bf4ea7619a8fbd82bc22fc2f` (SEG-049 PR #95 squash) |
| harness `main` at synchronization | `1cb9d0d` (SEG-050 refinement PR #90 merged) |
| product branch / PR | `task/seg-050-t001`, draft PR #96 (one PR for T001..T006) |
| tools | `tools/seg050_materialized_screen.py` (sanitized screening / baseline over the shipped `segarecomp build`), `tools/seg050_materialized_cost.py` (attributable generated-C and compile CPU by differencing two emissions); tests `tests/seg050_materialized_screen_test.py` |
| baseline binary | the unmodified `segarecomp` of this branch point (`Release`, gcc 13.3), SHA-256 `eb8b2d7ae65ac96f...`; gcc -O1 generated code, `--jobs 8`, one 16-core host, single samples (measurement noise for identical output is about +/-25 % CPU, see section 9) |

## 6. Inventory: who owns what, and what each mechanism does and does not prove (verified against the code, not assumed)

| property | owner (file / ADR) | what it establishes | what it does NOT establish |
| --- | --- | --- | --- |
| image existence, immutable source | `genesis_m68k_executable_images` (`platforms/genesis/machine/src/frontend.cpp`, producer `genesis.cartridge`, authority `immutable_input`, verification `none`); artifact + validator `libs/recompiler/include/segarecomp/recompiler/executable_image.hpp` (ADR 0077) | a structurally valid `raw_cartridge_rom` claim owns its bytes; dense `ImageId`; mappings and source references validated | which bytes are code |
| byte-for-byte copy into work RAM | ADR 0049 descriptor `(execution, source, length)` -> `static_proof` image, producer `genesis.copy_alias`, verification `byte_identity`, derivation `verbatim_source_copy` (`frontend.cpp`, `m68k_copy_alias.cpp`, `apply_genesis_immutable_copy_alias`) | the descriptor's source span lies wholly in one cartridge claim; the IDENTITIES are the immutable source bytes decoded at the RAM base by the one existing decoder | the descriptor itself is a PROPOSAL derived from an observed build-time stop: the live work-RAM bytes at the stop PC are matched to the maximal verbatim ROM run (min 16 bytes, `kMinRun`). Soundness at run time rests on the per-instruction byte-identity guard, not on the "static_proof" label |
| runtime byte-identity guard | generated alias/thunk bodies (ADR 0049 decision 2) | every executed alias instruction compares its own bytes to RAM before any effect; mismatch = typed `known_but_unemitted_target` stop | nothing is ever decoded from RAM |
| observed work-RAM `JMP` thunks | producer `genesis.ram_thunk`, authority `bounded_build_time_materialization`, verification `byte_identity` (`frontend.cpp` ~2028, ADR 0097 amendment) | exact observed bytes of one `JMP (xxx).W/.L`, runtime-guarded | no cartridge source; not a verbatim copy; `alias_source_address` is its own RAM address |
| deterministic transformed / decompressed M68K code | **none exists** | -- | `grep` over `libs/ platforms/ apps/` finds no M68K decompression or transform producer; ADR 0017 explicitly records that no decompressor semantic owner exists; the only "materializer" is the Genesis **Z80** one (`genesis.z80_materializer`, derivation `runnable_epoch_snapshot`, verification `structural`) |
| image identity / provenance reporting | `ImageProvenance` + `count_image_provenance` -> `status.json` `executable_images` (counts by authority/producer only) | sanitized provenance | no address, byte or hash |
| structural prune + admission validator | `prune_genesis_region_admission`, `apply_genesis_hybrid_admission` (`hybrid_admission.cpp`, ADR 0094/0080) | the closure of mandatory fixed successors/continuations; **every `execution_alias` identity is a mandatory root** (`materialized_image_not_admitted`) | the validator is the correctness authority and accepts no proper subset of an alias image |
| fail closed on change / unproven | runtime guard; `frontier_not_verbatim_copy`, `non_alias_frontier`, `repeated_alias_no_progress`, `max_rounds`, `tool_failure` terminations; broad AOT fallback | a wrong descriptor can never execute wrong code | |

**Decompression arm: `NO_DECOMPRESSION_PRODUCER`.** No new decompressor or executable-image discovery system was built (non-goal).

## 7. Screening of every locally available authorized Genesis image (19; SMS excluded)

Procedure of section 3, one sample each, Compatibility, `--optimize 0`, stopped after `m68k-alias-prepare`. `U0`/`U1`: broad universe of the alias-less / final emission; `M = (U1-U0)/U1` in ppm. Class by section 2; `non_alias_frontier` (the build-time run stops at a guest frontier no copy alias can explain, not work-RAM execution) is class E, `S3` fails in `generate` (a pre-existing broad-AOT shape rejection, ADR 0109).

| image (ROM SHA-256 prefix) | KiB | class | aliases | thunks | alias B | termination | U0 | U1 | M ppm |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Alien Soldier (U) `2754fa0ddf8a` | 2048 | D | 0 | 0 | 0 | route_advanced | 1,007,606 | 1,007,606 | 0 |
| Batman (E) `93860d14494f` | 512 | E | 0 | 0 | 0 | non_alias_frontier | 247,620 | 247,620 | 0 |
| Cool Spot `1868a42c9e61` | 1024 | D | 0 | 0 | 0 | route_advanced | 498,276 | 498,276 | 0 |
| **Flicky (JUE) `4df1a91e0837`** | 128 | **A** | 1 | 14 | 49,152 | route_advanced | 64,352 | 87,967 | **268,452** |
| **Golden Axe `e9f5340ecf81`** | 512 | **A** | 1 | 0 | 432 | route_advanced | 249,843 | 250,058 | **859** |
| Land Stalker (U) `497958caf4e3` | 2048 | D | 0 | 0 | 0 | route_advanced | 988,195 | 988,195 | 0 |
| Moonwalker (JUE) REV00 `1f09b10515bc` | 512 | E | 0 | 0 | 0 | non_alias_frontier | 248,298 | 248,298 | 0 |
| OutRun `231f52844398` | 1024 | D | 0 | 0 | 0 | route_advanced | 496,950 | 496,950 | 0 |
| Phantasy Star II (UE) REV02 `a0bd97f5aaa6` | 768 | D | 0 | 0 | 0 | route_advanced | 367,930 | 367,930 | 0 |
| Phantasy Star IV `511f35cc11f8` | 3072 | D | 0 | 0 | 0 | route_advanced | 1,478,957 | 1,478,957 | 0 |
| Shining Force (U) `6c43b41b60ae` | 1536 | D | 0 | 0 | 0 | route_advanced | 735,560 | 735,560 | 0 |
| Sonic the Hedgehog (USA, Europe) `46160baa0636` | 512 | D | 0 | 0 | 0 | route_advanced | 246,293 | 246,293 | 0 |
| Sonic the Hedgehog 2 (World) `07329f456104` | 1024 | D | 0 | 0 | 0 | route_advanced | 496,387 | 496,387 | 0 |
| Sonic & Knuckles (JUE) `6e12e6b33c26` | 2048 | **C** | 0 | 1 | 0 | route_advanced | 989,195 | 989,196 | 1 |
| Sonic the Hedgehog 2 (JUE) `193bc4064ce0` | 1024 | D | 0 | 0 | 0 | route_advanced | 496,309 | 496,309 | 0 |
| Sonic the Hedgehog 3 (E) `627789250d79` | 2048 | E | - | - | - | generate failed | 991,659 | 991,659 | - |
| Streets of Rage 2 (U) `4a314edbfee9` | 2048 | E | 0 | 0 | 0 | non_alias_frontier | 1,002,076 | 1,002,076 | 0 |
| Streets of Rage 3 (E) `e9ab569b114f` | 3072 | D | 0 | 0 | 0 | route_advanced | 1,495,463 | 1,495,463 | 0 |
| Streets of Rage `95d7efb98e97` | 512 | D | 0 | 0 | 0 | route_advanced | 247,761 | 247,761 | 0 |

Counts: **A = 2, B = 0, C = 1, D = 12, E = 4.** D means "none observed by the bounded boot/attract preparation", not "none exist" (ADR 0049 workload dependence).
Not locally available (named by the operator as screening candidates only; nothing is claimed about them): Golden Axe II, Shining Force II, Streets of Rage rebuild sources. They are NOT downloaded.

## 8. Truth availability and lineages

| lineage | class | independent source-backed instruction truth | copied-image truth |
| --- | --- | --- | --- |
| Flicky | A | yes: exact byte-identical rebuild of `oranguthang/flicky_src @ d9a0d479...e9a89`, 5,707 starts (truth artifact SHA-256 `33916a8e...d6e0`, SEG-048/049, private, re-verified by ROM digest) | **derivable exactly by byte identity**: 4,602 of the 5,707 starts (80.6 %) lie in the 49,152-byte copied source span; the remaining 1,105 are outside it. The 14 RAM thunks have no source and no truth (`no_truth`) |
| Golden Axe | A | no source-backed truth is available locally (no exact-rebuild source) | none (`no_truth`); ADR 0049's 117-instruction decode audit is a decoder statement, not instruction-start truth |
| Sonic & Knuckles | C | yes (skdisasm exact rebuild, 119,319 starts) but no alias | none: one observed RAM `JMP` thunk, not a copy |
| other D/E images | D/E | PS2, S1, SF1 have exact truth (SEG-048/049); Alien Soldier / Land Stalker sources are cloned locally but are held back (contamination rule, SEG-053) | n/a: no materialized executable image observed |

**Independent qualifying lineages: A/B with independently supportable truth = 1 (Flicky), and Flicky is a training/selection title of the frozen M3/B35 model (known-corpus, ADR 0100 evidence statement), so the number of BLIND qualifying lineages is 0.** Distinct A lineages = 2 (Flicky, Golden Axe).

## 9. Baseline economics (single samples; gcc -O1; `--jobs 8`)

Whole-build (shipped `segarecomp build`, baseline binary):

| image | policy | wall s | CPU s | peak RSS KiB | generated C bytes | effective |
| --- | --- | --- | --- | --- | --- | --- |
| Flicky | Compatibility | 231.5 | 1,109.9 | 388,600 | 86,659,819 | broad |
| Flicky | Optimized + gen2 (status quo) | 231.8 | 1,086.2 | 388,444 | 86,659,819 | **broad, fallback `prune_rejected`** |
| Golden Axe | Compatibility | 104.9 | 714.2 | 532,020 | 163,252,739 | broad |
| Golden Axe | Optimized + gen2 (status quo) | 84.9 | 533.9 | 523,504 | 163,252,739 | **broad, fallback `prune_rejected`** |

The last two rows produced byte-identical generated C to their Compatibility rows, so the 714 vs 534 CPU-s difference is the measurement-noise floor of single samples on this host (about +/-25 %); no timing difference below that is claimed anywhere in SEG-050.

Attributable cost of the materialized images (emission differencing: `emit-general-startup-bridge-c --reset-entry --immutable-rom-aot` with and without the ADR 0049 descriptor, every emitted unit compiled at -O1):

| image | emission | identities | generated C bytes | units | compile CPU s |
| --- | --- | --- | --- | --- | --- |
| Flicky | ROM only | 64,352 | 26,617,159 | 56 | 49.2 |
| Flicky | + the copy alias (49,152 B) | 87,953 | 86,851,254 | 57 | 253.1 |
| Flicky | **attributable to the image** | +23,601 (26.8 %) | **+60,234,095 (69.4 %)** | +1 | **+203.9 (80.6 %)** |
| Golden Axe | ROM only | 249,843 | 162,060,243 | 60 | 322.9 |
| Golden Axe | + the copy alias (432 B) | 250,058 | 163,252,739 | 60 | 327.8 |
| Golden Axe | **attributable to the image** | +215 (0.086 %) | +1,192,496 (0.73 %) | 0 | +4.9 (1.5 %, within noise) |

An alias identity costs about 2.6 KiB of generated C against 0.41 KiB for a ROM identity (the per-instruction runtime guard). Alias/RAM-thunk preparation: Flicky 15 rounds / 16 emissions (1 alias + 14 thunks); Golden Axe 1 round / 2 emissions. Model inference cost is already measured by ADR 0102/0104 (0.08-1.6 s token decode + 0.3-5.3 s inference per image) and is not repeated.

## 10. Fallback events and the T002 promotion rule (section 4, threshold 1)

Under the experimental gen2 opt-in (status-quo binary), both alias-bearing images admit the alias-less first emission and then fall back on the first alias emission with `prune_rejected / outside_region` (ADR 0104 finding 3):

| image | first emission `K0`-pruned `K` | `U0` | lost saving `L = 1 - K/U0` |
| --- | --- | --- | --- |
| Flicky | 22,274 | 64,352 | **0.654** |
| Golden Axe | 83,709 | 249,843 | **0.665** |

Both exceed `L >= 0.30` (two independent lineages, one of them with no RAM thunks): **T002 is promoted.**

## 11. T001 classification

- `NO_DECOMPRESSION_PRODUCER` (code inspection, section 6).
- `CORPUS_SCARCE_COPY_ONLY`: two A lineages (copy-only), no B; exact-copy truth for ONE (Flicky, known-corpus); zero blind qualifying lineages.
- `NOT_ECONOMICALLY_IMPORTANT` for conclusion 2 **by the pre-registered rule** (`M >= 0.05` in at least two independent lineages is not met: Flicky 26.8 %, Golden Axe 0.086 %). The finding is honest but narrow: for the one image where materialized code is material (Flicky) it is 69 % of the generated C and 81 % of the M68K compile CPU.
- Per section 4 (3) the operator-mandated materialized-image ML question still proceeds to T003/T004 and carries this finding into the economics gate; a `CORPUS_BLOCKER` is the expected terminal class for the supervised experiment and is stated, not hidden.
- Stop paths: T003/T004 stay executable (operator amendment); never substitute bounded runtime coverage for executable truth.

Additional authorized inputs needed from the operator to reach a credible supervised experiment (none are downloaded or assumed): (a) further local Genesis images that **demonstrably** copy code to work RAM (screen with `tools/seg050_materialized_screen.py --screen-only`; each must show `aliases >= 1`); (b) for each, an exact byte-identical source rebuild (public disassembly + the original ROM where its build requires original assets, e.g. a Shining Force II rebuild needs the original US ROM) whose assembler listing yields instruction-start truth; (c) at least 3 such independent lineages for an exploratory frozen-M3 transfer and 8-12 (with >= 3 held out) for a separate model. No locally held image or source currently satisfies (a)+(b) other than Flicky.

## 12. Errata from the independent C1 review (sections 1-4 are unchanged)

- Section 10 `L` uses the post-prune admitted `K` (22,274 / 83,709), not the pre-prune `K0` of the section 4 definition; with `K0` (22,392 / 88,640) `L` is 0.652 / 0.645. Both exceed 0.30; the promotion is unaffected.
- Section 8: the "known-corpus" statement for Flicky is supported by ADR 0101 (final M3 fit on all five titles), not ADR 0100.
- The E mapping of `non_alias_frontier` and the class of a RAM thunk image (it DOES carry `byte_identity` verification; it lacks a cartridge SOURCE) are clarifications made at result time; class C means "work-RAM execution observed, no cartridge source / no exact executable truth". They change no label that was used as data (E and C are never labels).
- The class assignment was applied by hand from the preparation aggregates (no code computes it); the Flicky attribution is one final-emission difference and does not apportion the 15 preparation rounds, which dominate its wall time.
- Prior knowledge: the Flicky `prune_rejected` fallback was already known from ADR 0104 finding 3 before this ADR.
