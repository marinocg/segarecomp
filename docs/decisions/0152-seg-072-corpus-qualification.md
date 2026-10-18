# ADR 0152: SEG-072 corpus qualification, truth extraction and sealed binding

- Status: Accepted. Results record for SEG-072-T001 (development corpus) and SEG-072-T002 (sealed corpus and TMSS), appended after the
  frozen pre-registration (ADR 0151, plan digest `48da926fe0729680fa966f8272148662a66f7a09becc9b7de59046a4c45e4e36`). No model was fitted,
  scored or benchmarked before this record and the sealed binding were committed.
- Classification (T001): **development 11 of 12 qualified.** The pre-registered development minimum (12) is not met, so the generalization
  evidence is incomplete; qualified-entry results are reported separately and no model may be promoted on this cohort (ADR 0151). The
  sealed section is appended by T002.
- Machine-readable identities and aggregates: `tools/segarecomp_ml_v3.corpus_manifest.json` (digests and counts only).

## Development corpus (T001)

Truth sources: `tools/segarecomp_source_map_extract.py` listing semantics (ADR 0099) for disassemblies, extended with `--classes-output`
(per-byte `I`/`c`/`D` derived from the same fail-closed accounting); `tools/segarecomp_ml_v3_sgdk.py` for SGDK projects. Every extraction
was run twice with identical digests; every image is bound to its SHA-256. Independent decoder audit (CPU-owned `m68k-decode-token-report`,
never used to create labels): for **all 50 development images**, every labelled start decodes as an ordinary instruction whose decoder length
equals the labelled extent (0 odd starts, 0 non-ordinary decodes, 0 length mismatches).

| lineage | images | starts | positive / negative / masked 128-B cells | result |
|---|---|---|---|---|
| D01 Sonic engine | 39 (s1, sk, s2, 36 Sonic CD modules) | 288,508 | 9,683 / 41,605 / 7 | QUALIFIED |
| D02 Flicky | 1 | 5,707 | 180 / 844 / 0 | QUALIFIED |
| D03 Shining Force I+II | 2 | 93,367 | 2,828 / 25,842 / 2 | QUALIFIED |
| D04 Phantasy Star II | 1 | 19,261 | 565 / 5,579 / 0 | QUALIFIED |
| D05 Genesis 2048 | 1 | 3,388 | 89 / 651 / 284 | QUALIFIED |
| D06 TetrisMD | 1 | 6,750 | 214 / 2,297 / 561 | QUALIFIED |
| D07 240p Test Suite | 1 | 25,246 | 667 / 974 / 407 | QUALIFIED |
| D08 Mega Car Wars | - | - | - | **UNQUALIFIED**: build inputs missing at the pinned revision (only C sources/headers are tracked; no resource definitions, `sprite.h` cannot be generated) |
| D09 MegaPong | 1 | 742 | 24 / 448 / 552 | QUALIFIED |
| D10 Genesis Megatetris | 1 | 10,592 | 283 / 2,275 / 514 | QUALIFIED |
| D11 Penguin World | 1 | 6,343 | 179 / 1,282 / 587 | QUALIFIED |
| D12 MegaDriving (streeter) | 1 | 11,010 | 312 / 1,407 / 329 | QUALIFIED |

Per-entry identity notes:
- **s1, sk, flicky**: the ADR 0099 truth digests reproduce (`fd21b96b…`, `76be1ccc…`, `33916a8e…`) from the SEG-061 listings.
- **sf1, ps2**: rebuilt under Wine (SN 68k 2.53 / bundled AS Bld 89; `asw` needs Windows-style paths), byte-identical images; truth digests
  `7f5289dc…` and `87174c56…` reproduce ADR 0099 exactly. This removes the ADR 0124 blocker.
- **s2**: `build.lua` at the pinned revision (default `gameRevision = 1`) is byte-identical to the local REV01 image (SHA-1 `8bca5dce…`);
  48,733 starts.
- **sf2**: the pinned revision is reachable only from upstream `build/standard` (no `master` branch exists any more); its `VANILLA_BUILD=1`
  original-ROM configuration with the repository's split/assembly is byte-identical to the operator image (`9adf662d…`); 48,627 starts.
- **Sonic CD**: ADR 0140's 36 admitted modules re-extracted from the cached pinned build; every start digest equals ADR 0140; 96,276 starts;
  module classes 6 main Word RAM in place, 18 main Work RAM copy, 3 sub PCM driver, 9 sub program; seeds = ADR 0140 static-traversal cells.
- **SGDK projects** (three byte-identical builds each: two plain, one instrumented): the SGDK release per project was identified from
  evidence, not results - D05 v1.62 (its bundled boot files match only 1.62; the build is byte-identical to the author-committed
  `out/rom.bin`), D06 v1.41 (README + `sega.s`), D09 1.34 (`sega.s` family 1.22-1.34, project start 2018-12), D10 v1.90 (latest release before
  the project start), D11 v1.70 (its last commit predates 1.80; `MapDefinition` layout), D12 v2.11 (README); D07 uses the SGDK 1.11 fork its
  BUILD.TXT names (`ArtemioUrbina/SGDK_1_11_Suite` pinned at `ea1e49d1…`, GCC 3.4.6, non-LTO). Releases 1.x run their bundled Windows
  toolchains under Wine (a Temurin 8 Windows JRE serves `rescomp.jar`/`sizebnd.jar`); 2.x run in the digest-pinned official SGDK image
  `ghcr.io/stephane-d/sgdk@sha256:b0bb8a71…` (GCC 13.2.0). D12 uses the single final `streeter/04_drive` stage as the fixed representative
  (the four numbered stages build on each other). D07's fork lacks `mv` for a final cosmetic rename; the ROM is complete before it and only that
  exact failure is accepted. D06/D10 committed author ROMs differ from these builds (older builds); qualification rests on reproducibility.

### SGDK truth method (fail closed)

Gas listings and gas DWARF line tables are both skewed by MC68000 branch relaxation, so neither is used. The instrumented build adds only
`-g -save-temps -Wl,-Map` to the link and `-g -save-temps(=obj)` to compile rules (GCC's `-g` invariance; ROM byte identity checked for every
project). Every kept assembly unit (LTO partitions, or the per-object assembly of non-LTO builds) is re-assembled twice: as written, and with a
unique marker label before every byte-emitting statement; loaded section contents must be byte-identical, so each marker value is the exact
post-relaxation offset of its statement. Sections are placed with the linker map; section sizes must equal the map and every non-relocated byte
must equal the ROM. Ownership: a function's source file comes from its first `.loc` (project-relative path = application; absolute SDK path
baked into `libmd`'s LTO bytecode = SDK; no location = ambiguous); data objects use the application/SDK symbol tables. Application
instructions are `I`/`c`, application data and rescomp resources `D`, SDK/libgcc/startup/ambiguous and mergeable string sections `U`. Inlined
SDK code inside an application function counts as application code (stated limitation).

### Independence

12-byte shingle containment of labelled application-code runs: the largest development pair is 0.026 (D02 in D01); no pair reaches 0.30; no
merge. Folds: leave-one-lineage-out over the 11 qualified lineages.

### Observations

- The production broad-AOT enumerator rejects D10, D11 and D12 (`effective_address_not_24bit`): those images have no broad baseline, no
  production seed cells and no structural K stage; static recall remains measurable.
- Erratum to ADR 0151 `labels.historical_titles`: under the v3 cell rule a cell holding only continuation bytes of an instruction started in the
  previous cell (plus data) is masked, where gen2 labelled it negative: s1 2, sk 4, sf1 2 cells. The frozen-M3 reproduction uses the unchanged
  gen2 labels; M3-R simply gives these 8 cells no loss.

## Extractor adapters added (strictly additive; every earlier digest reproduces)

AS: unindented nameless labels (`-`, `+`, `--`, `/`); `CPU Z80UNDOC` is Z80 code; continuation lines of Z80 rows are not accounted.
ASM68K: labels may contain `?`; a byte-less `dcb.<size> count,value` row with literal operands becomes a computed data row that must equal the
ROM. All 7 disassembly titles, the 36 Sonic CD modules and the ADR 0099 digests were re-extracted with the final extractor: identical.
