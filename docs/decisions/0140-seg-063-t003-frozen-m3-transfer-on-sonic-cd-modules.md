# ADR 0140: SEG-063-T003 frozen M3 transfer on source-backed Sonic CD modules

- Status: Accepted. **Outcome: `EVIDENCE_UNAVAILABLE`** (missing prerequisite: a measured compile-size/time problem the transfer
  would address). Finding: the frozen M3 does not keep complete recall at B35 on Sega CD modules (`C outside R = 306` in 3 of 36
  modules), so it is not adopted; static traversal stays the only Sega CD discovery and no retraining design is specified.
- The pre-registration below was committed in `bbaa61e` (as ADR 0136, renumbered to 0140 for a number collision with no text
  change) before any benchmark result was computed; the results are appended after it.
- Related: ADR 0133 (the benchmark question, closed outcomes and retraining gate this record runs), ADR 0131 (ML reuse boundary),
  ADR 0132 (module lists), ADR 0099 (source-backed label method), ADR 0100/0101/0102/0109 (frozen gen2 M3 + B35), ADR 0103.

## Pre-registration

All raw inputs (disc, built modules, listings, labels, per-address results) stay in ignored local paths; only the normalized figures
below are recorded.

| item | registered value |
|---|---|
| question | ADR 0133's, unchanged: does the frozen gen2 M3 at B35, applied only inside module images already admitted by an explicit module list, keep acceptable recall of source-backed instruction starts, with build cost compared against static traversal from declared roots? |
| model identity | gen2 M3 (`tools/segarecomp_ml_region_gen2.model.json` SHA-256 `11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1`, embedded weight table `72699dbcf8be5b8ceaffb36cb3cf4736eafe635e13ad564e752b81a8ee127d98`, representation `seg049-gen2-repr-v1` `cf43e758da215283c08b3f8c1271c6119fd83b44eee755974559a6cb5af40c67`, token schema `9a1f4f4fc03d26c63209128fc59fec1584b266bd1f61dbe8236f1105a1d61d75`, rank plan `8c4d98b9ee3874ff6a38005ce4745913c9ea01defd333d6bee9c1ea700c21e29`), policy B35 (`N = ceil(cells * 35 / 100)`, logit descending, cell ordinal ascending); scored by the unchanged native `segarecomp ml-gen2-cell-logits` on the module bytes alone (no address is a model input), and the recomputed selection must reproduce that command's `selected_cells_sha256` exactly. No training, no new network, no budget or admission change |
| input | the operator's legally held Sonic CD (USA) disc, through its original CUE (SEG-063-T003 adds `INDEX 00` support; contract section 9a) |
| source build | the public Sonic CD disassembly at revision `73a6a358496116bc892c7c068189b0771f273c58`, built locally by its own bundled SN 68k 2.53 (`REGION=1`, the project's own `.Make.bat` steps and options plus the macro-expansion listing option `m+`, which does not change output bytes). A disc file is **matched** only when the built file is byte-identical to the ISO9660 file of the disc |
| labels `C` | instruction starts of the matched file's own listing, by `tools/segarecomp_source_map_extract.py --dialect asm68k --align-post-pc --image-base <the listing's lowest byte-emitting address>` (independent of the model and of the recompiler's decoder); every extraction is run twice in separate processes and must give identical artifact digests |
| mapping and roots | from the source only: an in-place Word RAM MMD (macro origin `WORDRAM2M`) runs whole at `$200000` (main); a copy MMD is assembled at `origin - $100` (the macro's `org`) and is mapped there (main); a sub-CPU file at its `org` (`$10000`, `$30000`, `PCMDriver = $40000`, `SPX = $B800`). Declared entry roots: MMD header entry, H-INT and V-INT when non-zero; sub-CPU program start (`org`); PCM driver `PCMDrvRun` (`+$10`); SPX `SPXStart` (the target of the SP's `jmp`) |
| module subset | every matched file with a non-empty label universe that the ordinary `segarecomp sega-cd inspect --modules` route admits as a one-module list (with the boot IP/SP). Exclusions are reported by typed class. Aggregates count distinct module SHA-256 once; per-processor (main vs sub) and per-module-class breakdowns are reported |
| baseline arm `S` | static traversal from the declared roots inside the admitted image: the instruction starts for which `segarecomp sega-cd emit --modules` (same one-module list) emits a function (reached), and the subset it lowers (compiled) |
| experimental arm `R` | the ADR 0100 R stage inside the same admitted image and the same roots: `R` = B35-selected cells union the seed cells (cells holding a start of `S`, the analogue of the certain-code seeds); `ML` = B35 cells alone (diagnostic) |
| metrics | `C outside R` and `C outside ML` (counts; zero tolerance); `R/image`; static recall `|C ∩ S| / |C|` and static misses `C \ S`; misses recovered by `R`; candidate false admissions = even offsets inside `R` that the CPU-owned decoder accepts as an ordinary instruction and that are not in `C` (count, class: inside a labelled instruction's extent / elsewhere); baseline generated-C bytes, emission time (median of 3) and native compile time (`cc -std=c11 -O1 -c`, median of 3) of the one-module program, and their deltas against the boot-only program |
| leakage | Sonic CD shares no title with the M3 training titles (Sonic 1, Flicky, Shining Force, Phantasy Star II, Sonic & Knuckles); the shared Sonic engine lineage with Sonic 1 is a stated limitation. Several matched files are variants of one program (level acts, sound-test images); distinct digests are reported, not independent titles |
| gates | **G_recall**: `C outside R = 0` on every module of the subset. **G_select**: `R/image <= 0.60` on every module of at least 8 cells. **P** (cost problem): some module's one-module baseline program exceeds 32 MiB of generated C or 120 s of native compile |
| outcomes | `TRANSFER_ADEQUATE` iff G_recall and G_select. `TRANSFER_INADEQUATE_RETRAIN_JUSTIFIED` iff not (G_recall and G_select) and P; then a retraining design is specified, never run. If not (G_recall and G_select) and not P, retraining is not justified and the record closes `EVIDENCE_UNAVAILABLE` naming the missing prerequisite "a measured compile-size/time problem the transfer would address" (ADR 0133: the benchmark matters only then), with the transfer failure stated as the finding. An empty subset closes `EVIDENCE_UNAVAILABLE` naming the failed prerequisite |
| not decided here | adoption (ADR 0131 point 3) additionally needs recall improvement over static traversal and a measured zero count of behaviour-changing false admissions through a Sega CD R-admission path; no such path exists (ADR 0131/0133: static traversal is the only Sega CD discovery) and building one is out of scope, so the M3 arm's admitted set, its behaviour-changing false admissions and its compile size/time are **not measurable** here and are not claimed. Static traversal and deterministic admission stay the correctness authority; the default build output is unchanged |

## Results (two full passes; every non-timing figure identical between them)

Inputs and source identity:

| item | result |
|---|---|
| disc | the original CUE loads (35 tracks, 34 audio tracks with stored `INDEX 00` pregaps; identical output of two inspections); ISO9660 resolves 131 root files; boot IP/SP produced |
| source build | 44 of 44 built disc files byte-identical to the disc (the bundled toolchain, `REGION=1`; one file additionally needs the project's own Z80 driver step); the built SP equals the disc's SP extent up to its zero padding; the built IP differs from the disc's IP extent (boot image, outside the module subset) |
| labels | 37 matched files yield a label universe, each extracted twice with identical digests (the remaining matched file is data only and its build step writes no listing); 96,276 instruction starts in the admitted subset |
| exclusions (typed) | `size_invalid` x6 (sub-CPU PCM drivers of odd byte size; the label tool rejects the same images as `rom_size`); `mapping_outside_executable_ram` x1 (a copy MMD whose header precedes Work RAM) |
| subset | 36 modules, 36 distinct digests. main 24: 6 in-place Word RAM programs (>= 128 KiB), 18 Work RAM copy programs (4-32 KiB); sub 12: 3 PCM drivers (>= 128 KiB), 9 programs (5 < 4 KiB, 2 4-32 KiB, 1 32-128 KiB, 1 >= 128 KiB) |
| parity | the recomputed B35 selection equals the native command's `selected_cells_sha256` on all 36; every declared root is a labelled start |

Static traversal (baseline) and the R stage:

| group | modules | labels | static recall | `C outside R` (modules) | `C outside ML` (modules) | static misses recovered by `R` | `R/image` max (>= 8 cells) |
|---|---|---|---|---|---|---|---|
| all | 36 | 96,276 | 0.127 | **306 (3)** | 821 (7) | 83,716 of 84,022 | 0.397 |
| main | 24 | 89,465 | 0.098 | 306 (3) | 547 (3) | 80,421 of 80,727 | 0.397 |
| sub | 12 | 6,811 | 0.516 | 0 | 274 (4) | 3,295 of 3,295 | 0.381 |
| main Word RAM in place | 6 | 73,596 | 0.012 | 0 | 0 | all | 0.350 |
| main Work RAM copy | 18 | 15,869 | 0.494 | 306 (3) | 547 (3) | 7,725 of 8,031 | 0.397 |
| sub PCM driver | 3 | 1,974 | 0.739 | 0 | 0 | all | 0.351 |
| sub program | 9 | 4,837 | 0.426 | 0 | 274 (4) | all | 0.381 |

- Every module has static misses (their causes were not classified here); no statically reached start is outside the labels (an independent corroboration of the labels in
  the other direction), and no reached instruction is unsupported.
- Diagnostics, not selections (32 modules of >= 8 cells): FRF mean 0.21, median 0.18, worst 0.95. Of the 3 failing modules, 2
  hold code in 42-44 % of their cells, above the 35 % budget, with the code cells ranked first (FRF within 0.02 of the code-cell
  fraction): a budget-bound miss; 1 is a ranking miss (one code cell ranked at 0.95). 31 of 32 modules have FRF <= 0.60.
- Candidate false admissions inside `R` (even offsets the CPU-owned decoder accepts and that are not labels): 287,921, of which
  53,011 lie inside a labelled instruction. A precision signal of an `R`-wide admission, not a behaviour claim (no Sega CD
  `R`-admission path exists; see "not decided here").

Build cost of the baseline (one-module programs, `cc -O1`, median of 3): at most 1.09 MiB of generated C (boot-only program
13 KiB), at most 2.7 s of native compile (boot-only 0.03 s) and at most 5.9 s of emission, of which about 5.7 s is the boot-only
route itself (disc identity hashing). P is false by two orders of magnitude.

Gates: G_recall **fails** (306 in 3 modules), G_select passes, P false. By the pre-registered table the outcome is
`EVIDENCE_UNAVAILABLE`, missing prerequisite "a measured compile-size/time problem the transfer would address"; retraining is not
justified and no design is specified. The default build output is unchanged.

Typed admission findings on the real disc (recorded, not fixed here): beyond the exclusions above, one list holding every admitted
module is `too_many_modules` (36 > 32), and the 18 copy programs sharing one Work RAM address are `too_many_candidates` for one
overlay group (> 16); the 6 in-place Word RAM programs sharing `$200000` emit as one overlay group. A copy MMD is executed without
its 256-byte header, so its whole-file mapping at `origin - $100` (the source's own assembly address) cannot become live at run time
under whole-range liveness; that needs a sub-range mapping in the list (ADR 0132 "a later list refinement"). The benchmark is
build-time only and is unaffected.

Instrument note: a first aggregation used a symbol pattern that missed upper-case hexadecimal function names; it disagreed with
the emitter's own per-image counts and was discarded before interpretation. The measured runs assert that the parsed reached set
equals the emitter's compiled plus unsupported count for every module.

Limitations: one title (Sonic engine lineage shared with the M3 training title Sonic 1); files are variants of a few programs, so
per-module counts are not independent samples; the `R`-stage is the only measured model stage.
