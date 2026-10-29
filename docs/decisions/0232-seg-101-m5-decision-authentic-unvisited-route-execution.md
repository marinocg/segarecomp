# ADR 0232: SEG-101 - milestone M5 decision: authentic unvisited-route execution with a frozen file-first program

- Status: Accepted. Final decision of milestone SEG-101 (M5). Related: ADR 0227-0231; ADR 0226 (M4).
- Privacy: classes and counts only.

## Explicit milestone decision: `M5_GO`

| Condition | Status |
|---|---|
| 1. One authentic M4 file-first executable, no gameplay input | met: B1, built with no input script, no module/extent list or per-title option; 3 boot extents, 0 game-module extents; rebuilt from an empty cache byte-identically |
| 2. Hash and compiled inventory unchanged over all routes | met: hash and directory inventory verified before/after each route; `PATH=/nonexistent`; 8 further runs byte-identical |
| 3. An authentic module neither loaded nor executed during the build later loaded and executed | met: 2 images on R1, 1 on R2, none on R0 (ADR 0229) |
| 4. Native bodies existed before gameplay | met: all 120 images compiled at build time; sources unchanged |
| 5. Mapped through verified dynamic binding | met: bound by the binder from live bytes and the delivery ledger at bases the build never saw; 0 refusals, 0 ambiguity |
| 6. Executed instructions byte-verified | met: per-instruction guard; independent audit against the reference's RAM and the disc file |
| 7. Matches the independent reference on the accepted interval | met: 8/8 and 7/7 canonical frames, VRAM, Word RAM, executed-start sets; residual RAM differences are the established stack-residue / record-slot classes; state-identical to the best Compatibility control of each route |
| 8. No interpreter, runtime decoder, JIT or compiler | met (runtime has no exec/dlopen/mmap; review) |
| 9. No title-specific production heuristic | met (review; the diff adds only an observer-only account) |
| 10. Determinism, regression, CI | determinism and fast tier (185/185, 2 skipped by preconditions) met locally; exact-head CI recorded in the PR |

`AUTHENTIC_UNVISITED_MODULE_GO`. Strength: R1 carries it cleanly (two modules, one 256 KiB Word RAM module with 3,761 executed offsets and a sub module; different from every observed module). The R2 module shares library-like routines with observed modules (ADR 0229), and "unvisited" means not executed by the input-free route: the build compiled those files' bytes by design.

## Tasks

T001 baseline and pre-registration (0227, `AUTHENTIC_ROUTES_PRE_REGISTERED`); T002 accounting (0228, `COVERAGE_ACCOUNTING_GO`); T003 experiment (0229); T004 frontier diagnosis, no repair (0230, `FRONTIER_PARTIAL`: no frontier on the registered path; the independent PCM wave-address-pointer read is diagnosed and deferred); T005 economics (0231: 2.2x executable, 18-30 % run time, build wall 5.4 min cold, 12 of 120 images executed on any route); T006 this record.

## Independent review (T006) and corrections

One adversarial pass (separate agent; reproduced all route results, ran the fixture suites under ASan/UBSan clean) found no blocker. Applied: (1) latent aliasing of the per-image account for image indexes >= 4096 (the emitter already refuses more images; the runtime now also skips the account); (2) the runner refuses a `--observed-from` report of another program; (3) wording: the "directory the program cannot write to" claim replaced by what is actually enforced, the static BIOS image named among code sources, the R2 module's shared routines stated. Minor and left: early error returns skip the release of the account (process exit; error paths only).

## What M5 does not establish

Any other level or route; whole-game completeness in any dimension (ADR 0231); the PCM pointer-read frontier (about 12,450 frames into R1); transformed code (the Earthworm Jim SE `no_source` frontier); files over 512 KiB and non-ISO sources; cost reduction (119.6 MB, +18-30 % run time). Nothing is merged and no successor is started.
