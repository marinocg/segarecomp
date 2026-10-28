# ADR 0217: SEG-099-T003 - state convergence in the static loader scan without unbounded exploration

- Status: Accepted (pre-registration, then result and decision). Milestone SEG-099 (M3). Related: ADR 0206 (state-keyed visited set), 0214 (Earthworm Jim SE exhausts the bound), 0216 (work-variable state).
- Privacy: project-authored fixtures; authentic measurements are counts and classes only.

## Why the bound was exhausted (analysis before the change)

A per-location census of the exhausted walk of the authentic Earthworm Jim SE boot image (aggregate: no address) shows two causes, neither of which is a deep search:
(1) counted/copy loops over known, advancing pointers whose exit depends on data (an ISO9660 directory-name copy loop: every iteration is a new state because a pointer and a counter differ), and (2) the same routines reached
through many call contexts - the visited set is keyed by the complete state *including the return stack*, so one loop body is re-walked for every caller. The bound (4,096 states, 100,000 steps) is a correct guard; the states it counts are mostly irrelevant variation.

## Pre-registered rule (written before the sensitivity matrix; limits not raised)

Option `state_convergence` (`discover-modules --state-convergence [--widen-after N]`; the build enables it; default `widen_after = 24`):

0. **Convergence is a fallback.** The exact walk runs first, so every earlier result (including the 48-module overlay inventories of ADR 0213, which joins at the shared dispatcher would degrade) is reproduced unchanged; only a walk that exhausted a bound is repeated with joins (`survey.convergence_used`). A first all-joins design failed `sega_cd_bios_scale_test` for exactly that reason and was replaced.
1. Exact exploration up to `widen_after` distinct states at one control-flow location (head = pc + module-table slot + a two-bit knowledge profile: "abstract memory empty", "boot image possibly rewritten"). A path that has lost its work-area knowledge cannot degrade one that has not.
2. Beyond it the arriving state is **joined** with the head's joined state: a value (register, hardware-shadow field, read position, condition code, memory byte) keeps its knowledge only if both states know the *identical* value; any difference makes it **unknown** (all-or-nothing, so a location changes at most once and the lattice has finite height). No join ever picks one of two constants or computes a third. May-facts are united (loaded ranges, header read, dirtiness).
   A state already covered by the joined state ends its path; a changed joined state is explored in place of the arriving one.
3. Call contexts are summarised at a joined head: the return stack becomes a **set** of possible stacks (at most 64, `kLoaderScanMaxReturnContexts`); `RTS` forks one continuation per member. More callers than the bound is the typed `scan_limit_exceeded`.
4. Disc-derived knowledge is tracked per value and per byte; a **comparison of immutable disc data with a value the walk does not know** (a directory search with an unknown key) is the typed refusal `lookup_key_unknown`, the path ends there, and no source is proposed behind it. (Found while testing the unknown-alias case: without it an unknown key enumerates every directory record as a candidate file.)
5. The state, step, depth, operation and module bounds are unchanged. The exploration order is deterministic (depth first); `--scan-order 0..3` and `--converge-always` (skips the exact attempt) are test-only.

Soundness argument: a join only removes knowledge or adds may-facts, and unknown values only ever widen the set of explored arms or turn a proof into a typed refusal. What can be lost is *precision*, never correctness of an admitted proof; every proposal is still byte-verified at run time.

| # | Case (project-authored; scan level) | Expected |
|---|---|---|
| 1 | counted read loop over a known advancing pointer (6,000 exact iterations) | without convergence: `scan_limit_exceeded`, nothing admitted; with: completes, A and B still proven |
| 2 | register that grows without bound, exit on an unknown value | same |
| 3 | work variable that changes every iteration | same |
| 4 | loop that changes the destination itself | converges; the destination is not chosen; only bases the program's arithmetic really produces |
| 5 | two candidate destinations meeting at a shared loader | exact: both bases; forced join: unknown, never a third value |
| 6 | unknown-pointer store loop; store loop that widens a pointer; computed transfer | converges; the work area is forgotten (typed refusal); computed transfer stays `computed_control_unresolved` |
| 7 | four exploration orders | same proven modules and completeness (refusal classes too when no join happens) |
| 8 | authentic Earthworm Jim SE | measured; false admissions must stay zero |

Gate: `STATE_CONVERGENCE_GO` (rows 1-7 hold and the authentic scan no longer exhausts), `PARTIAL` (rows hold, the authentic scan still exhausts or the margin is not robust), `NO_BENEFIT`.

## Result

`tests/sega_cd_state_convergence_test.py` (33 checks): rows 1-7 hold. Each loop fixture exhausts the bound without convergence and completes in 660-810 states with it (36-68 joins), A and B still proven where the loop does not store through the widened pointer; a store loop through a pointer that is widened to unknown
correctly forgets the work area (`loop_pointer_late`), and the same loop before the arguments are stored makes the file name image data of unknown content: `lookup_key_unknown`. No fixture admits a module the program cannot load. Exploration order does not change the proven modules or the completeness on any fixture; refusal classes are identical when no join happens (joins are the only order-dependent step).

Authentic Earthworm Jim SE (identical authorized inputs, no gameplay script):

| Measure | M2 / T002 (no convergence) | With convergence |
|---|---|---|
| Scan result | `scan_limit_exceeded`, incomplete | **complete** |
| Visited states / steps | 4,096 / 8,187 (exhausted) | 3,535 / 64,623 |
| Joins / widenings | 0 / 0 | 309 / 156 |
| Branches decided by tracked condition codes | 9 | 17 |
| Statically discovered modules | 0 | 0 (the one transfer's destination is unknown: `destination_unknown`) |
| False admissions | 0 | 0 |
| Scan wall time / peak memory | 5.2 s / 12.7 MB | 5.5 s / 10.0 MB |
| Build (6 rounds, 2 extents) | stop `entry_not_compiled`, inventory `blocked` | the same stop and inventory; the static discovery is `no_modules` with the typed refusal instead of `scan_incomplete` |

Sensitivity of the threshold (aggregate, before the CDCTRN follow-up; see the Amendment for the final numbers): Earthworm Jim SE completed for `widen_after` 12-32 (2,057 / 2,422 / 3,210 / 3,940 states) and exhausts again at 48 (the bound, not the threshold, is the limit). On the project fixtures, three loader paths sharing one reader (`two_paths`) keep all three modules for `widen_after >= 32`, two of three at 16-24 and one at 8-12: *paths that share code compete for the per-location exact budget*, a precision cost of context-insensitive joining, not an unsoundness. The default was 32 at this point and is 24 after the amendment below; the margin on the authentic title is thin.

## Decision: `STATE_CONVERGENCE_GO` (mechanism) with recorded limits

The scan of the authentic title now terminates inside the unchanged bounds with a typed result instead of exhaustion, no joins invent knowledge, and nothing is admitted without proof. Not achieved: any authentic module (the loads of this title are selected by commands, ADR 0215/0216), and a robust margin: a title with more shared-code paths can still exhaust or lose precision.

## Amendment (follow-up review, ADR 0220): threshold re-measured after the CDCTRN soundness fix

Forgetting memory for an unresolved CDCTRN (the sound behaviour) removed knowledge on the authentic Earthworm Jim SE path and its scan again exhausted at the old default 32 (`widen_after` 12-24 complete: 2,230 / 2,606 / 3,052 / 3,535 states; 32 exhausts). The default is now **24** (3,535 states, 86% of the bound); 16 and 20 were rejected because the exploration-order test (`loop_unknown_exit`) then differs between orders. On the project fixtures with all-joins forced (`--converge-always`) the three-path fixture keeps 2 of 3 modules at 16-24 and all 3 at 32; the exact attempt (the default path) is unaffected. The conclusions of this ADR stand: a sound scan that completes inside the unchanged bounds, with the margin and precision cost stated.
