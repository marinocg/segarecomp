# ADR 0229: SEG-101-T003 - one frozen file-first program, three authentic routes

- Status: Accepted. Milestone SEG-101 (M5), task T003. Judged against ADR 0227 (routes, frames, definitions, fixed before any generated run with input; nothing was changed afterwards) with the accounting of ADR 0228.
- Privacy: counts and classes only. The program hash, image ids, file identities, addresses, hashes, captures and reports are private artifacts of the operator's machine.
- Tool: `tools/sega_cd_m5_routes.py` (generic; prints aggregates; the private JSON holds ids).

## Method

Program B1: one `segarecomp sega-cd build --file-first structural` of the operator's Sonic CD (USA) with the US Model 1 BIOS, no input script, no module list, no extent list, no per-title option. Its generated-source tree and program identity equal B0's (the ADR 0227 baseline built before the T002 runtime change): the only difference is the runtime object code of the observer-only account. 120 files were compiled before launch (22.0 MB; 1,536,888 offsets; 69,057 shared bodies); the build admitted **3 extents, all boot-system; 0 game-module extents, 0 static-scan extents** (7 rounds). B1's directory is verified (hash and inventory of every file) before and after every route; each route runs with `PATH=/nonexistent` (no compiler or linker is found by name; the runtime has no exec, dlopen or mmap); nothing is emitted, compiled, linked, cached or admitted between routes. The same B1 ran R0, R1 and R2; the reports of eight further runs (four deadline-partition seeds each of R1 and R2) are byte-identical to the first.

## Results (one frozen executable)

| | R0 (no input) | R1 (Checkpoint H schedule) | R2 (RAM DATA branch) |
|---|---|---|---|
| stop | `pcm_unsupported` (the M4 frontier; same tick) | none (deadline at entry+1500) | none (deadline at entry+1500) |
| retired instructions | 74,463,733 (= M4's) | 69,012,450 | 67,316,975 |
| through dynamic bodies | 44,168,194 | 35,871,769 | 37,461,560 |
| images compiled / bound / executed | 120 / 9 / 9 | 120 / 9 / 9 | 120 / 8 / 8 |
| executed images the build observed | 9 | 7 | 7 |
| **previously unvisited authentic images executed** | 0 | **2** | **1** |
| instructions retired in those images (distinct source offsets) | - | 7,028,012 (3,854) | 8,867,879 (1,265) |
| new instruction starts against R0 (main / sub) | - | 3,923 / 160 | 1,239 / 58 |
| refusals of any class, ambiguous mappings, mismatches | 0 | 0 | 0 |
| bindings (main / sub), replacements | 9 (5/4), 4 | 9, 3 | 8 (5/3), 4 |
| compared reference frames equal | - | **8 of 8** | **7 of 7** |

The unvisited images are one 256 KiB main-processor module loaded over the 2M Word RAM view (3,761 distinct source offsets executed, 6.77 million instructions), one 213 KiB sub-processor module in PRG RAM (93 offsets) - both on R1 - and one 31 KiB main-processor module in Work RAM on R2 (1,265 offsets). Each was loaded by the original game through its own loader, at a base the build did not know; each was identified by the runtime binder from the live bytes and the machine's delivery ledger; no instruction of any of them retired in the build route; the program has no decoder. Of the new instruction starts against R0, 3,854 (R1) and 1,265 (R2) lie in the unvisited images; the remaining 229 and 32, derived by subtraction, are new starts inside already-known modules or static boot code and are **not** counted toward the central claim. The two R1 images are the same two sources the historical Checkpoint H procedure had to be told about by hand (developer-established extents); here nothing supplied them.

## Equality with the independent reference (same route, same frames, same input)

Canonical frame (digest and dimensions), VRAM, and Word RAM are equal at every compared frame of both routes (0 differing pixels; Word RAM 0 bytes). Executed instruction starts of the reference run to entry+1500: **sub 5,241 and 5,139, main 4,246 (R2) all equal as sets**; R1 main 6,905 of the reference's 6,907, the two reference-only starts being the documented expansion-port control-register read-back difference of ADR 0177 (typed difference 1), unchanged by this work. Residual Work RAM / PRG RAM differences are the established asynchronous classes and nothing else: **Work RAM 1 byte (R1, frames from 1,150; 6 at the Checkpoint G frames) and 6-35 bytes (R2), all below the main stack pointer (stack residue); PRG RAM 24-67 bytes (R1) and 22-74 bytes (R2): the same 18 bytes of one sub-side record-slot table constant from entry+510, the rest sub-stack residue** - equal in kind and count to ADR 0177's Checkpoint H table for R1. Every differing byte of R2 is below a stack pointer or in the record-slot region; R1's are the same classes as ADR 0177 reports for Checkpoint H (below the sub stack pointer; the 18 slot bytes; at entry+1,150 only, one further byte at the sub's interrupt-entry phase), at the same counts.

## Controls (route-specialized Compatibility builds; their extents were never transferred to B1)

| route | control (program identity differs from B1) | outcome | B1 against it |
|---|---|---|---|
| R2 | supported Compatibility build with R2's input script (15 rounds, 11 extents, 56.0 MB) | runs R2 to the deadline | **complete machine state, executed starts and all seven capture sets equal** |
| R1, automatic | supported Compatibility build with R1's script (14 rounds, 10 extents, 52.1 MB) | **stops `entry_not_compiled` at the first call into the Word RAM module** (a destination "outside admitted images"): the execution-derived pipeline cannot admit a Word RAM module | B1 equals it until that instant (state, starts, captures), and B1 *binds that very module at that tick* and continues |
| R1, developer extents | the same build plus the two manual `--disc-image` extents of the Checkpoint H procedure (14 rounds, 131.8 MB) | runs R1 to the deadline | **complete machine state, executed starts and all eight capture sets equal** |

So, against the independent reference and against the best Compatibility control that can run each route, the frozen program is state-identical; against the *automatic* Compatibility build it executes more (R1).

## Exploratory (not pre-registered, not part of any gate)

Without `--until-tick` the frozen program continues R1 for about 12,450 reference frames after entry (about 159 and 248 million instructions on the two processors) before a typed `pcm_unsupported` stop; R2 reaches the 600-million-instruction budget idling on the RAM DATA screen. See ADR 0230 for the stop.

## Independent audit of the executed bytes

For each unvisited image the reference's own RAM dump at entry+1500 and the disc file are compared at every instruction start the reference executed inside the image's extent *while the image could be resident*: the two R1 images were the only code ever at their addresses (**3,763 and 93 starts, the program's own counts being 3,761 and 93 offsets**), and for the R2 image, whose Work RAM base earlier hosted three other modules, the starts first executed after its load (found by a second reference run stopped at the load instant) number **1,080**; in every case **each opcode word equals the file's**. The program's 1,265 offsets of that image are those 1,080 plus 185 already executed under the earlier modules (same code at the same offsets). The audit used no output of B1. The three images are distinct files, not copies of observed modules: the same-offset identical-word fraction with the best observed image is 7.9 %, 0.2 % and 0.7 %. The review also measured shared routines at *different* offsets (16-byte windows): 3.7 % of the R1 Word RAM module's new starts, 0 % of the sub module's, but about 42 % of the R2 Work RAM module's 1,237 new starts occur in windows that also appear in observed modules, so the R2 module is partly library-like code the build knew by other files; R1 carries the claim without that caveat. 'Unvisited' means not executed by the input-free route; the build did compile these files' bytes by design.

## Gate

`AUTHENTIC_UNVISITED_MODULE_GO`. Conditions (ADR 0227 decision 5): the images were file-first candidates compiled before any route ran; absent from R0 and the build's observed set; loaded by the original game on R1/R2; mapped by the verified binder; their native bodies retired; the executed bytes equal the source; the observable behaviour equals the independent reference; no rebuild, runtime compilation or decoder.
