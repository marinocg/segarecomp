# ADR 0221: SEG-100-T001 - file-first broad AOT with dynamic verified module mapping: architecture and authentic feasibility survey

- Status: Accepted. Milestone SEG-100 (whole-game static AOT, M4). Builds on ADR 0126/0127/0132 (images, overlays, module lists), 0142 (the BIOS-image profile and bounded materialization), 0205 (the self-service build and `resolve_source`), 0206-0214 (M1/M2), 0215-0220 (M3, `M3_PARTIAL`). Companion records: ADR 0222 (bodies), 0223 (binding), 0224 (inventory and build), 0225 (authentic evaluation), 0226 (decision).
- Privacy: classes, counts and size classes only for the two authorized local discs. No address, offset, LBA, name, hash or byte of a disc or BIOS appears here.

## The hypothesis (and what it deliberately does not do)

M3 tried to *reconstruct the game's loading commands* statically and found, on both authentic titles, no module (`M3_PARTIAL`): the file selector of each load is a name the **main** processor supplies, dispatched through a computed transfer. M4 asks whether that reconstruction is necessary at all.

> **Compile the executable bytes of the disc's own files before gameplay, without knowing where they will live; let the original game run its own loading code; bind the precompiled bodies to the guest memory only when independently verified.**

Three things stay separate and are never conflated: **AOT files are immutable inputs to native compilation; guest memory mappings are dynamic machine state; the only thing the runtime decides is which precompiled body serves a PC** (and it can be refused). Nothing here is an interpreter, a JIT, a runtime decoder or a runtime compiler: ADR 0222 and 0223 give the argument and the tests.

The design in one paragraph. The build enumerates the ISO9660 files of the data track with the one reader of the project and compiles **processor-neutral, base-less native bodies** for their source offsets (ADR 0222, 0224). The original game's code then performs the loads. When the machine executes a PC that no static producer owns, the runtime looks for the *one delivered candidate image the live bytes are an exact copy of* (ADR 0223) and serves its bodies by `(image, source offset)`; each body still compares its own instruction bytes with the live bytes before it runs. The existing static, execution-derived and overlay producers keep precedence and remain the fallback; the Compatibility build is the unchanged correctness reference.

## Terms that must not be equated

| term | meaning here | where it is established |
|---|---|---|
| **source bytes on disc** | the bytes of an ISO9660 file of track 1 | the one ISO9660 reader; the disc identity is verified at build and at start-up |
| **candidate bytes compiled** | the source offsets for which a native body exists in the program | build time; the CPU owner's decode and lowering; the policy (`all` / `structural`); *compilability, not permission to execute* |
| **source-to-RAM mapping proven** | a binding `(image, base, verified run)` | run time; delivered blocks + byte equality (ADR 0223) |
| **executed instructions verified** | an instruction ran because its bytes equalled the live bytes at the actual PC | every body's `scd_code_guard` |
| **whole-game coverage demonstrated** | a route that exercised the game's code paths | **not claimed by anyone in this milestone** |

## Survey method

`segarecomp sega-cd file-survey` (counts only) and `tools/sega_cd_file_first_survey.py`: the ISO9660 files are enumerated with the project's reader; for each file of at most 512 KiB (the largest executable region of a processor) the CPU-owned decoder and lowering are asked, at **every even source offset**, whether a source-offset body exists (`derive_dynamic_body`); the structural closure (ADR 0224) is computed; the independent execution evidence of an unmodified Compatibility build (`status.json` extents and the `--coverage` of its final program) and, for Sonic CD, the private source-backed class maps of 36 module files (ADR 0140; `private-corpus/seg072-dev-v1`, read-only, restricted, never an input of any policy) are joined **in memory** and reported as counts. Oversize files are accounted, never silently dropped. Filenames, extensions and paths are never used as authority.

## Measurements (one authorized local run of each disc; classes and counts only)

**Files.** Sonic CD (USA): 131 ISO9660 files, 120.9 MB; by size: 7 under 2 KiB, 34 of 2-32 KiB, 12 of 32-256 KiB, 74 of 256-512 KiB (the median file is exactly 256 KiB), and 4 above 512 KiB (98.9 MB, 82 % of the bytes: not candidates, see below). The 127 candidate files hold 22.07 MB, of which 22.00 MB are unique (4 duplicate files). Earthworm Jim SE (USA): 200 files, 13.5 MB, all at most 512 KiB; 1 duplicate.

**What the CPU owner can compile at every even offset (`all`).** Sonic CD: 11,033,378 even offsets, 10,554,591 (95.7 %) decode, lift and lower to a source-offset body, 478,787 do not decode, **0 refused by the dynamic-base derivation** (no token-structure, non-linear, suffix or constant-context refusal, no re-decode difference). Earthworm Jim SE: 6,762,992 offsets, 6,552,309 (96.9 %) compile, 210,683 undecoded, 0 refused. Compilable is not executable: the MC68000 is dense, so most data decodes.

**Sharing makes the inventory tractable.** A body is a function of the instruction's bytes alone (ADR 0222), so offsets with equal bytes share one body: 368,097 distinct encodings for Sonic CD's 10.55 million compilable offsets (3.5 %), 393,108 for Earthworm Jim SE's 6.55 million (6.0 %). One body per offset would be 6.4 GB of C for Sonic CD; the distinct encodings are 316 MB (Earthworm Jim SE: 319 MB). That is still a large compile for the unfiltered policy.

**The structural policy (ADR 0224) is the bounded one.** Sonic CD: 1,541,520 candidate offsets (14.6 % of the decodable ones), 69,057 distinct encodings, 50 MB of distinct body text; as emitted (processor-neutral bodies, tables, bytes arrays) 114.5 MB of C in 68 body units and 120 map units, compiled in **278 CPU-seconds** (46 s wall at six jobs on a shared host; peak child memory 199 MiB; 82 MB of objects), generated in 25 s wall / 64 CPU-s / 346 MB peak. Earthworm Jim SE: 328,894 candidate offsets (5.0 %), 34,132 encodings, 20 MB of distinct text; 52 MB of C (38 MB of bodies in 34 units, 14 MB of tables), compiled in about 117 CPU-seconds (16 s wall at eight jobs).

**Independent evidence that the structure matters, not decodability (Sonic CD, 36 source-backed module files; ADR 0140's labels, local, never an input).** 40 disc files match the 36 labelled module digests; they contain 99,532 source-labelled instruction starts, **all of which decode**; the structural closure contains **99,529 of them (99.997 %)**, 37 of the 40 files at 100 % and the weakest at 99.85 %; the three it misses are instructions that are themselves faults (ILLEGAL-style words). It also marks 15,415 data bytes and 44,531 bytes inside instructions valid (candidates on 159,475 offsets, 62 % of which are real instruction starts) while excluding **1,222,055 of the 1,237,470 data offsets that merely decode (98.8 %)**. Two variants were measured and **rejected** (they are evaluation flags, not policies): letting a PC-relative branch or call that leaves the file be a boundary changes nothing material (+181 candidates, no recall gain), and letting an always-faulting word be a valid terminating instruction reaches 100 % recall at the price of **5.3 times as many candidates** (8.2 million offsets, 306,359 encodings, 257 MB of distinct text).

**Independent execution evidence (the unmodified Compatibility build, no module list, no gameplay script, formatted empty backup RAM).** Sonic CD reproduces M3 exactly: 17 rounds, 12 execution-derived extents (7 main, 5 sub), stop `pcm_unsupported`, 0 static modules. Of the 12 extents 3 lie in the boot system area, 2 are whole files and **6 are the payload of a file after a 12-20 byte header that the loader patches in RAM** (so the live image never equals the whole file: identity must be a verified run, not whole-file equality); both processors execute disc files (7 main, 5 sub), so a body cannot be tied to a processor at build time; 14 pairs of extents are different sources over one RAM range (overlays). The run executes 8,342 distinct instruction starts: 1,527 in the BIOS ROM, 905 in boot-area extents, 3,795 in RAM that is not a disc copy (BIOS-derived epoch and copy images), and 2,115 in disc files - of these 364 are attributable to one source, **346 of which (95 %) are in the structural closure** (the rest: 9 faulting words, 5 odd destinations, 4 not decodable), and the 1,751 in overlaid RAM are in the closure for at least one candidate source (for every candidate: 224, a lower bound). Earthworm Jim SE stops after 6 rounds at a main-processor Work RAM frontier whose live window is **not** an exact copy of any delivered disc span (about half of the surrounding 64-byte blocks are verbatim file bytes, the entry instructions are not): code that was transformed or patched. No disc-file instruction executes before it, so file-first has nothing to bind there; that frontier is classified, not addressed.

**Binding economics bound (T003).** The candidate images the delivery ledger names at a binding event are a small fraction of the 120 admitted images; ADR 0225 gives the measured search cost.



## Comparison of the three approaches

| | A. SP loader static analysis (M1-M3) | B. file-first candidate enumeration, broad AOT | C. hybrid (this milestone's product design) |
|---|---|---|---|
| what is compiled before gameplay | modules whose load the walk can prove (0 on both authentic titles) | candidate files (bounded, typed) | B, plus everything A/the build pass supplies |
| needs the loader semantics | yes (the whole difficulty) | no | no |
| needs the guest base | yes (an extent per mapping) | no | no (bound from live bytes) |
| runtime authority | per-instruction live-byte guard on an admitted extent | none by itself: compilability is not permission | **verified binding + per-instruction guard** |
| transformed / decompressed code | not covered | not covered (no file contains the executed bytes) | not covered (typed `no_source`) |
| cost | seconds of scan; modules compiled per extent | measured below | B's bodies shared by identical encodings |
| verdict | `M3_PARTIAL` | compilable at scale, not an authority | **the proposed architecture** |

**Reused unchanged:** the ISO9660 reader and the disc identity checks, the CPU-owned decode, lift, lowering and timing, `derive_source_relative_text`, the BIOS-image profile's per-processor protocol (`scd_code_guard`, `scd_retire`, prefetch, refresh skip), the delivery ledger, the overlay write/view notifications, the compiler cache and partitioned units, the build driver's frontier resolution, the static and execution-derived producers, the typed stop vocabulary.

## What the survey cannot show

Whole-game coverage; the behaviour of code that exists on the disc only in compressed or generated form; the content of files above 512 KiB; any claim about gameplay beyond the route the machine can currently run (the `pcm_unsupported` frontier of Sonic CD is independent of this work and is documented, not addressed).

## Gate

`FILE_FIRST_AOT_FEASIBLE`, **with limits that this milestone does not hide**:

- *Immutable modules are demonstrated*: on Sonic CD the executable code that the machine runs from RAM is, for the files it uses, a byte-exact copy of ISO9660 files (payload after a patched header), on both processors.
- *A bounded potential compilation inventory is demonstrated*: the structural policy admits 14.6 % (Sonic CD) and 5.0 % (Earthworm Jim SE) of the decodable offsets, covers 99.997 % of an independent source-backed set of instruction starts and 95 % of the executed ones that can be attributed to a source, and is compiled in minutes of CPU. The unfiltered policy exists, is correct and is bounded by sharing, but at hundreds of megabytes of C it is a measured cost, not a default.
- *Not covered, by evidence*: files above 512 KiB (82 % of Sonic CD's bytes: data in this route, but not proven so), code that the loader transforms (the Earthworm Jim SE frontier), and any title whose executable modules are not ISO9660 files. These stay on the existing producers.
- *No claim of completeness or of authority*: compilability and structure grant nothing to execute; ADR 0223 decides what may run.
