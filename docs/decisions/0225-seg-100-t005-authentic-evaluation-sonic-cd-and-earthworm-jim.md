# ADR 0225: SEG-100-T005 - authentic evaluation: Sonic CD (primary) and Earthworm Jim SE (secondary)

- Status: Accepted. Milestone SEG-100 (M4). Related: ADR 0221 (survey), 0222 (bodies), 0223 (binding), 0224 (inventory and build), 0220 (the M3 baseline).
- Privacy: classes, counts, times and sizes only. No address, offset, LBA, hash, name or byte of a disc, BIOS or backup RAM appears here; RAM destinations are reported as processor and memory class.
- Inputs and conditions: the operator's legally held Sonic CD (USA) and Earthworm Jim - Special Edition (USA) discs, the Model 1 (USA) BIOS image, a formatted empty backup RAM image, no gameplay script, no module list, no per-title manifest. One host, 16 cores. Every figure is from the supported `segarecomp sega-cd build` and the program it produces. Tool: `tools/sega_cd_file_first_eval.py` (builds the same disc as Compatibility reference and with `--file-first`, runs both on the same bounded route, compares them).

## Control matrix (Sonic CD, Model 1 BIOS, route from reset to the first unmodelled device)

| arm | modules compiled before any run | module extents supplied to the emitter | rounds | build wall / CPU / peak child memory | program | run (same route) | stop |
|---|---|---|---|---|---|---|---|
| M3 static loader scan (ADR 0220) | 0 | 0 | - | scan: seconds | - | - | - (0 static modules; the build then proceeds as the next row) |
| route-derived Compatibility (reference) | 0 (the build pass observes, then compiles each extent) | 9 game extents (+3 boot) | 17 | 616 s / 1,033 s / 1,130 MiB | 53.6 MB | 14.1 s, 74,463,733 instructions | `pcm_unsupported` (independent audio boundary) |
| **file-first (`--file-first structural`)** | **120 files (22.0 MB; 1,536,888 offsets; 69,057 shared bodies)** | **0 (+3 boot)** | **7** | **319 s / 1,129 s / 640 MiB** | 119.6 MB | 19.5 s, 74,463,733 instructions | `pcm_unsupported`, same tick, same PC |
| combined (the file-first build above also runs the static scan and the extent fallback) | as file-first | as file-first | 7 | as file-first | | | |

(The CPU figure of the file-first arm benefits from the runtime objects the reference build cached; its wall time includes generating 114.5 MB of C each round, about 25 s.)

## Equality with the reference (the "false selection must be zero" evidence)

The complete run reports of the two final programs are **equal in every machine-state field**: both processors' registers, retired instructions (74,463,733), cycles, instruction-trace hashes, every memory digest, the device log, the frame digest and the typed stop; the sets of executed instruction starts are identical (8,342: 3,088 main, 5,254 sub). If any instruction had run through a wrong image, the trace hash would differ. Of those instructions **44,168,194 (59 %) retired through dynamic bodies**.

## What the binder did (run report `dynamic`, one run)

9 bindings from 120 candidate images (5 main in Work RAM, 4 sub in PRG RAM), 9 distinct images bound and 9 exercised; 4 replacements: **four different authentic modules were loaded, one after another, over the same main Work RAM base, and two over one sub PRG RAM base, each re-identified and executed only through precompiled bodies**; 4 mapping invalidations (an instruction no longer the old image's), 391 instruction comparisons in mutated granules (7 failures, all re-identified), 45,793 granule comparisons (45,690 trusted again); 37,849 computed-control/return lookups, all hits; **0 refusals of any class, 0 ambiguous mappings, 0 unmatched executable fetches, 0 unsupported bodies** (0 refused encodings at build time). Search cost: 75 image reads, 3.9 MB in total; the run is 38 % slower than the all-static reference (19.5 s against 14.1 s: 59 % of the instructions go through the extra table lookup and the per-instruction checks of mutated granules).

Two findings of the authentic runs changed the design (both are regression tests): (1) a module's variables sit next to intact code - a whole-granule check dropped a correct mapping, so identity became a contiguous exact copy and executability became per instruction (ADR 0223); (2) an extent compiled for one module shadowed a different module later loaded at the same addresses, and an anchor taken before a PC read the 64 KiB mirror of work RAM - static chains now fall through to the bindings and anchors/runs never cross a memory region.

## What this does and does not show

- Authentic modules ran from native bodies compiled **before the first instruction ran** and with **no base, name, address or entry supplied** (the 9 module extents of the reference are not given; the three extents left are the boot system area). Relative to the M3 static scan, which admitted 0 modules, this is +9; relative to the route-derived pipeline it is the **same** 9 modules and **no more executed code** (identical coverage): that pipeline learns each base by running the game, this one does not need to, and it needs 7 build rounds instead of 17.
- It does **not** show new gameplay coverage: the route ends at the same unmodelled-audio frontier (`pcm_unsupported`, independent of this work, not addressed). A module that only a different route would load is shown on project-authored discs (modules B and C that the build pass never loads), not on an authentic one.
- Cost: the executable is 2.2 times larger (66 MB of tables and bodies), the run 38 % slower; the build is shorter in wall time because it has fewer rounds, not cheaper in CPU.

## Earthworm Jim SE (secondary generalization test)

Identical machine state (28,418,828 instructions), identical stop (`entry_not_compiled` at the main Work RAM frontier) and 6 rounds in both arms. File-first has nothing to bind here: no instruction of any disc file runs before the frontier (only boot-area extents), and at the frontier the binder's refusal is the typed `no_source`: the live window is not an exact copy of any delivered file (ADR 0221: transformed or patched code). Build +50 s wall (+311 CPU-s), run +0.2 s, executable larger. **Verdict: no gain and no regression; the mechanism is not exercised by this title's current frontier, so it neither confirms nor refutes generalization beyond Sonic CD.** The candidate policy contains nothing specific to either title (the Sonic CD labels were only an evaluation control).

