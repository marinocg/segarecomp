# ADR 0159: SEG-075-T001 cross-platform MC68000 source qualification and build inventory

- Status: Accepted. **Qualification gate: PASSED_WITH_LIMITS.** Independently authored, reproducibly built base-68000 programs exist on
  both Atari ST and Amiga, from five distinct assembler/compiler families. Only a minority of candidates is usable without operator review,
  and the strongest Amiga game candidates are blocked by OS-include or licensing prerequisites (section 5).
- Task: SEG-075-T001, the first task of the SEG-075 research milestone (cross-platform M68K corpus and hybrid code recognition). One product
  PR. SEG-074 records, worktrees and contracts are unchanged; no SEG-072/073 artifact, label, marker or decision is touched.
- Scope: offline research tooling only. No production, emitter, validator, runtime, launcher or default change. Nothing is trained.
- Tooling: `tools/segarecomp_xplat_qualify.py` (`validate`, `fetch`, `toolchains`, `build`, `report`), the frozen plan
  `tools/segarecomp_xplat_qualify.plan.json`, pinned toolchain definitions `tools/segarecomp_xplat_toolchains/*.Dockerfile`, tests
  `tests/segarecomp_xplat_qualify_test.py`. Sanitized public record: `tools/segarecomp_xplat_qualify.manifest.json`.

## 1. Inventory and freeze

The 24 candidates of the SEG-075 brief (11 Tier A, 9 Tier B, 4 Tier C) are frozen in the plan at the commits pinned on 2026-10-19. The tool
fetches each candidate at exactly that commit (shallow, by SHA) and fails if `HEAD` differs. No candidate was replaced and no revision was
advanced after a failure. The `ggnkua/Atari_ST_Sources` archive is fetched without blobs, and only a seeded pilot sample of its subprojects
is checked out (section 4); it is never copied. Further nominations must be added to the plan, and frozen there, before any model uses them.

## 2. Toolchains

Every build runs in an offline container (`--network none`) on a fresh copy of the pinned source.

| id | toolchain | provenance |
|---|---|---|
| `tc-gnu` | GNU binutils 2.42 (`m68k-linux-gnu`, assembler/linker only), vasm 2.0f, vlink 0.18a | Ubuntu 24.04 digest-pinned base; vasm/vlink release tarballs SHA-256 pinned |
| `tc-elf` | bare-metal `m68k-elf` GCC 13.3.0 + binutils 2.42, `--with-cpu=68000` | GNU release tarballs, SHA-256 pinned, built from source |
| `tc-mint` | `m68k-atari-mint` GCC 4.6.4 (MiNT 20251014), binutils 2.30 | the candidate `atarist-toolkit-docker` Dockerfile as-is |
| `tc-vbcc` | vbcc 0.9g (`m68k` and 16-bit-int `m68ks` back ends) + m68k-atari target 2019-10-04 | official archives, SHA-256 pinned |
| `tc-a68k` | A68K 2.71 (Charlie Gibbs, 1991, public domain), built from its C source | Aminet `dev/asm/A68kGibbs.lha`, SHA-256 pinned |

Consequential findings:

- **The Ubuntu `m68k-linux-gnu-gcc` C compiler is excluded from the base-68000 corpus.** It emits 68020 code even with `-march=68000` (GCC
  bug 98627); toslibc's own compiler guard rejects it, and the plan keeps that rejection as an executed expected failure. All corpus C is
  compiled with `m68k-elf`, MiNT GCC or vbcc. The GNU *assembler* with `-m68000` is unaffected.
- **Toolchain images are not bit-reproducible.** Their apt layers resolve current Ubuntu/PPA packages, so image IDs change between rebuilds.
  The reproducible identity is the pinned inputs plus the recorded tool and package versions (manifest `toolchains`). `tc-mint` inherits
  unpinned upstream inputs from the candidate Dockerfile (PPA packages, vasm/vlink/agtools "latest"); its versions are recorded, not
  guaranteed. Output reproducibility is measured directly: every successful recipe runs twice and its output digests are compared.
- Toolchain substitutions are declared per recipe, never hidden: vlink for BLINK (BattleIsleEditor), m68k-elf GCC 13.3 for the Neo Geo SDK's
  crosstool-ng GCC, vbcc/vasm for space-invaders' unavailable course toolchain. A substituted build is our build of the authored source, with
  its own truth; it is never presented as a historical rebuild.
- Two environment-only shims exist, and both are declared: lower-case alias symlinks for sources written for case-insensitive file systems
  (Mono Mental, the archive pilot) and upper-case aliases of vbcc's own headers (GODLIB). No source file is edited.

## 3. Results

Classes per candidate (`class`; `technical` is the build/label outcome before the licence rule). Output counts are per successful recipe
run. Every successful recipe was run twice. Its code digests (a TOS PRG's text+data, otherwise every byte) are identical between the
runs, with one exception: a declared `__DATE__`/`__TIME__` object (GODLIB `PROFILER.C`). The runs are byte-identical too, except that
the two mintlib-linked toolkit demos differ inside their PRG symbol tables.

| tier | candidate | platform | class | technical | executed evidence |
|---|---|---|---|---|---|
| A | emutos | ST + Amiga | PARTIALLY_QUALIFIED | partial | 4/4 base-68000 targets (MiNT GCC 4.6.4); 4 source files with non-68000 code |
| A | toslibc | ST | QUALIFIED | qualified | 6 example programs + test suite (m68k-elf GCC 13.3); linux-gnu GCC rejected (68020) |
| A | atarist-toolkit-docker | ST | QUALIFIED (infrastructure) | qualified | image builds (= `tc-mint`); 5 demo executables |
| A | space-invaders | ST | PARTIALLY_QUALIFIED | partial | 19 objects (vbcc 0.9g + vasm); link fails (duplicate definitions, cc68x runtime) |
| A | perihelion-m68k-tutorials | ST | LICENSING_REVIEW_REQUIRED | qualified | 14/14 tutorials (vasm `-devpac`) |
| A | AtariST (miniupnp) | ST | LICENSING_REVIEW_REQUIRED | partial | 10 vasm programs + listacsi (MiNT GCC); the_80s fails under vasm 2.0f |
| A | pacman500 | Amiga | BUILD_BLOCKED | blocked | AmigaOS/lvo/WHDLoad includes absent |
| A | scramble500 | Amiga | BUILD_BLOCKED | blocked | as pacman500 |
| A | amiga_asmdev_workflow | Amiga | BUILD_BLOCKED | blocked | only the ZX0 module; assets need GIMP + salvador |
| A | amiga-realtime3d | Amiga | QUALIFIED | qualified | 9/9 programs (vasm, Amiga hunk) |
| A | GODLIB | ST | LICENSING_REVIEW_REQUIRED | partial | 120 C objects (vbcc); PASM assembly unassemblable; library only |
| B | Atari_ST_Sources | ST | LICENSING_REVIEW_REQUIRED | partial | archive pilot, section 4 |
| B | BattleIsleEditor | Amiga | QUALIFIED | qualified | 6/6 executables byte-identical to the author's (A68K 2.71 + vlink) |
| B | AmiGalaga | Amiga | ASSET_DEPENDENCY_UNAVAILABLE | - | build reads the arcade ROM set |
| B | galaga | Amiga | LICENSING_REVIEW_REQUIRED | partial | 7 core modules (GNU as `-m68000`); transcode of commercial arcade code |
| B | xevious | Amiga + Neo Geo | LICENSING_REVIEW_REQUIRED | not attempted | same family as galaga |
| B | mpatrol | Amiga | LICENSING_REVIEW_REQUIRED | not attempted | same family as galaga |
| B | amiga-boing | Amiga | ASSET_DEPENDENCY_UNAVAILABLE | - | NDK 1.3 headers + Kickstart 1.3 absent; reconstruction |
| B | monomental | ST | LABEL_TRUTH_INSUFFICIENT | - | assembles (vasm `-devpac`); reconstructed source |
| B | atari_reverse | ST | ASSET_DEPENDENCY_UNAVAILABLE | - | original commercial executables required; diagnostic |
| C | neogeosdk | Neo Geo | PARTIALLY_QUALIFIED | partial | demo game compiles + links (m68k-elf GCC 13.3, 55 objects); packaging tool absent |
| C | x68000dev | X68000 | LICENSING_REVIEW_REQUIRED | partial | C hello world only (m68k-elf); its assembly program is absent from the tree |
| C | HelloAmi | Amiga | QUALIFIED | qualified | 4 units (vasm); tiny |
| C | amiga-vasm-template | Amiga | LICENSING_REVIEW_REQUIRED | qualified | one-file template |

Totals: QUALIFIED 5 (one is infrastructure), PARTIALLY_QUALIFIED 3, LICENSING_REVIEW_REQUIRED 9, BUILD_BLOCKED 3,
ASSET_DEPENDENCY_UNAVAILABLE 3, LABEL_TRUTH_INSUFFICIENT 1, CPU_VARIANT_UNSUPPORTED 0. No candidate was wholly CPU-variant unsupported. The
CPU-variant findings are partial: EmuTOS's guarded spans, galaga's `vbr.68k`, two GODLIB FPU modules, and four archive pilot subprojects.

Notes on the classes:

- **QUALIFIED with authentic corroboration: BattleIsleEditor.** The author's own assembler (A68K 2.71, rebuilt from 1991 source) plus vlink
  reproduces all six executables committed by the author **byte for byte**. These are original 1990s Amiga application programs: the
  strongest label-truth candidate in the inventory (listing rows corroborated by byte identity).
- **amiga-realtime3d** (nine vasm programs, CC BY-SA 4.0) and **toslibc** (six example applications and a test suite, m68k-elf GCC 13.3) are
  QUALIFIED. toslibc's C runtime must be separated from application code in T003. **HelloAmi** is QUALIFIED but tiny (a diversity probe).
  **atarist-toolkit-docker** is QUALIFIED as infrastructure only (it is `tc-mint`), not as a lineage.
- **EmuTOS** is PARTIALLY_QUALIFIED. All four original-68000 targets build (192 KB, 256 KB, 128 KB cartridge, 256 KB Amiga ROM), but the
  images embed runtime-guarded 68010+/68030/68040 code (CPU, MMU and cache detection and setup). T002 must type those spans as CPU-variant
  regions. The four targets are configurations of one firmware lineage and are scored separately from game workloads.
- **space-invaders** and **neogeosdk** are PARTIALLY_QUALIFIED with declared substitutes. space-invaders yields object-level evidence only:
  the course toolchain is unavailable, and the program links only with its runtime and duplicate-definition semantics. neogeosdk compiles and
  links; only ROM packaging (`srec_cat`) is absent, and its BIOS is needed only at runtime. Its bundled games share one engine, so they form one
  lineage.
- **LICENSING_REVIEW_REQUIRED** marks candidates that are technically usable but declare no licence (perihelion, miniupnp AtariST, GODLIB,
  amiga-vasm-template, x68000dev, the archive). It also marks the jotd666 arcade ports (galaga, xevious, mpatrol), whose 68k code is a
  line-by-line transcode of commercial arcade Z80 programs with ROM-extracted graphics: commercial-derived regardless of buildability. An
  undeclared licence is never treated as permissive. The plan validator forbids a technically usable, undeclared-licence candidate from
  carrying any other class.
- **BUILD_BLOCKED:** pacman500 and scramble500 need AmigaOS assembler includes, the author's private `lvo/*.i` tree and WHDLoad developer
  includes, none of which the repositories contain. Their sprites are arcade rips, so they would also need licence review.
  amiga_asmdev_workflow needs its asset toolchain (GIMP batch conversion of its own XCF files, salvador ZX0); this is not commercial.
- **ASSET_DEPENDENCY_UNAVAILABLE:** AmiGalaga (builds from the user's arcade ROM set), amiga-boing (Amiga 1.3 NDK headers + Kickstart 1.3 in
  a private submodule; also a disassembly reconstruction), atari_reverse (needs the original commercial executables; diagnostic only).
- **LABEL_TRUTH_INSUFFICIENT:** Mono Mental assembles, but its source is the authors' reverse-engineering of a binary whose original source
  is lost. A reconstructed listing is not independent instruction truth, so it is kept for diagnostic and hard-negative use only.

## 4. Atari_ST_Sources pilot

A seeded (seed 75) sample of 40 subprojects was taken from the 425 `ASM/` subprojects that hold exactly one assembly source and one shipped
TOS executable (at most six files). Each source was assembled with vasm (`-devpac -spaces -m68000 -no-fpu -Ftos`, case-insensitive
alias shim) and compared with the shipped executable:

| outcome | count |
|---|---|
| byte-identical to the shipped executable | 3 |
| text and data segments identical (header/symbols differ) | 1 |
| assembles, but differs (packed or different revision/options) | 5 |
| fails: dialect syntax (other assemblers' directives) | 15 |
| fails: missing include/incbin input | 12 |
| fails: non-68000 instruction under `-m68000` | 4 |

So 4/40 (10%) reproduce a shipped executable with zero per-project effort. The archive is a large, diverse but low-yield source, and its
licences are mostly undeclared. Every admitted subproject needs individual qualification (its own toolchain, inputs, revision identity and
licence) and its own lineage identity (author directory). The pilot is evidence of yield, not a qualified corpus.

## 5. Gate, operator inputs and recommendation for T002

**Gate decision: PASSED_WITH_LIMITS.** Programs that are usable without operator review, have a declared licence, are reproducibly
built and have checkable label evidence exist on both target platforms, from five distinct toolchain families:

- Atari ST: toslibc example applications (m68k-elf GCC 13.3, C); space-invaders (vbcc 0.9g C + vasm, object level); EmuTOS firmware
  (MiNT GCC 4.6.4 C + GNU assembly, 4 configurations).
- Amiga: BattleIsleEditor (A68K 2.71, authentic byte-identical rebuild); amiga-realtime3d (vasm); HelloAmi (vasm, tiny); EmuTOS's Amiga ROM
  configuration.
- Neo Geo: the SDK demo game (m68k-elf GCC 13.3), one SDK lineage.

The limits, which T003 must carry:

- Lineage hypothesis only about seven independent lineages, several of them small. Assembly-heavy *games* with mixed code/data, the
  workload the brief prioritized (Pacman500/Scramble500), are blocked.
- No qualified Genesis-independent game lineage with large embedded tables exists yet. The strongest assembly sources (BattleIsleEditor,
  realtime3d) are an application and demo-style programs.
- EmuTOS needs CPU-variant region typing before use.
- Nine technically usable candidates wait for licence review.

T004-T006 must be re-gated against the corpus that T002/T003 actually produce.

Operator inputs that would unlock more qualified data. None of them is required to continue, and none is a commercial ROM requirement for
label qualification:

1. Licence decision (e.g. private-research-only use, or author permission) for the undeclared-licence candidates: perihelion, miniupnp
   AtariST, GODLIB, amiga-vasm-template, x68000dev, and per subproject for the archive.
2. AmigaOS assembler includes (e.g. Hyperion NDK 3.2 R4 from Aminet `dev/misc/NDK3.2`, which carries no explicit licence statement), the
   `lvo/*.i` set, and the WHDLoad developer includes, to build pacman500/scramble500 for label qualification. Their arcade-ripped graphics
   then also need licence review.
3. Optional: the Galaga arcade ROM set for AmiGalaga (MIT code; ROM = build input); Amiga 1.3 NDK headers + Kickstart 1.3 for amiga-boing
   (diagnostic only; not recommended).

No runtime firmware (EmuTOS replaces TOS; Kickstart and Neo Geo BIOS only for optional runtime checks) is needed for T002/T003.

Recommendation for T002, covering only formats actually produced by qualified builds:

- Adapters: TOS PRG (text/data/bss, fixup list); Amiga hunk executable and object (CODE/DATA/BSS, RELOC32); m68k ELF relocatable objects
  (GCC 13.3, MiNT GCC via `-save-temps`/objects, neogeosdk); raw ROM images placed by a GNU linker map (EmuTOS).
- Listing parsers: vasm listings (`-L`), A68K listings, GNU as listings (`-alhn`), vbcc emitted assembly re-assembled with a vasm listing.
- Not needed now: Intel HEX, X68000 `.X`, a.out (vbcc objects can be emitted as ELF instead).
- First: BattleIsleEditor (authentic byte-identical corroboration) and amiga-realtime3d as the assembler path, and toslibc as the compiler
  path. Then EmuTOS with CPU-variant span typing.

## 6. Privacy and licensing

Third-party sources, build trees, listings, objects and binaries stay in the private cache. The tool refuses a cache that is inside a git
work tree and not ignored. The committed manifest holds only repository identities, pinned commits, declared licences, toolchain inputs and
versions, the outcome of every recipe, digests/sizes/formats of open-source build outputs, classes, rationales and counts. It holds no source
text, listings, logs, scripts' outputs or label data. No commercial ROM, firmware, Kickstart, BIOS or game asset was downloaded. Licence
status is recorded per candidate; derived datasets and model redistribution remain subject to review for CC BY-SA and GPL sources (T006
handoff).
