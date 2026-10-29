# ADR 0231: SEG-101-T005 - whole-game coverage feasibility and economics of the frozen file-first program

- Status: Accepted. Milestone SEG-101 (M5), task T005. Data: ADR 0227-0230 (one frozen executable, three routes, three route-specialized Compatibility controls) and measurements on one 16-core host. Nothing here is a completeness claim: finite tests cannot establish one.
- Privacy: classes, counts, sizes and times only.

## Coverage gained for the bytes compiled

| | value |
|---|---|
| files on the disc / compiled as images / not compiled | 131 / 120 / 11 (4 over 512 KiB = 98.9 MB, 4 duplicates, 1 empty, 2 without structure) |
| candidate bytes / compiled offsets / shared bodies | 22.0 MB / 1,536,888 / 69,057 |
| images executed on R0 / R1 / R2 / **any of the three** | 9 / 9 / 8 / **12 of 120 (10 %; 0.60 MB of 22.0 MB = 2.7 % of the candidate bytes)** |
| distinct source offsets executed on R0 / R1 / R2 | 2,273 / 6,086 / 3,486 (0.15 / 0.40 / 0.23 % of the compiled offsets) |
| images only R1 or R2 executed (not R0) | 3: 0.51 MB |

The coverage the frozen program delivered beyond the build's own route came from three small modules; 98 % of what was compiled served no registered route. That is the price of not knowing the route: it is not a defect to optimise away on this evidence (a smaller inventory would have been a guess), but it is why "all-route coverage" is not shown by it.

## Completeness, dimension by dimension (what is established, what is only exercised)

1. **Source inventory.** Every executed instruction of all three routes came from one of the 120 images or the three boot extents (0 `no_source`, 0 `no_delivery`, 0 `ledger_overflow`). The four files over 512 KiB (98.9 MB, 82 % of the disc's bytes) hold 49,391 of the 1,586,279 structurally valid offsets of the unbounded survey (3 %, shared with the duplicate files): data-like density (0.1 % of their even offsets). That is **evidence, not proof**, that they are not code; a code overlay of that size would be an `oversize_file` class. Raw executable extents of a non-ISO9660 source remain out (`non_iso_source`). Not shown complete.
2. **Instruction admission.** The structural policy compiled every instruction start the independent reference executed on the three routes (reference-executed starts in the unvisited images: 3,763, 93 and 2,531; the program's own distinct offsets 3,761, 93 and 1,265 through them, the rest served by other bindings of identical bytes or static code; 0 `body_not_compiled`). M4's source-backed recall was 99.997 % on its subset. `all` would compile 58.1 M decodable offsets against 1.59 M structural (36x), beyond the 32 Mi emission bound; a source-backed code map, not a bigger policy, would be the way to establish completeness. Not shown complete.
3. **Mapping.** Per route: 8-9 bindings, 3-4 replacements (different modules loaded over one base), 3-5 invalidations, 4-8 granule re-checks that failed and were re-identified (variables next to code), **0 refusals, 0 ambiguous mappings, 0 unmatched executable fetches**; one module at two bases is one identity (ADR 0228 fixtures); a static extent never shadowed a valid binding (the Compatibility control's static variants of R1/R2 and B1 are state-equal). Overlay machinery and Word RAM bank flips were exercised by R1 (the 2M Word RAM module). Soundness under every write pattern is not shown.
4. **Runtime compatibility.** R1 continues, exploratorily, about 12,450 frames past entry (about 3.5 minutes) until the sub's first read of the PCM chip's wave-address pointer (ADR 0230). Other levels, time-attack, special stages and the paths a player may take are unexercised. Game state and input can reach modules no registered route reached; the three routes prove the mechanism, not coverage.
5. **Transformed code.** On these routes: none encountered (0 `no_source`). The Earthworm Jim SE `no_source` frontier of M4 is the documented case that needs an independently justified offline reconstruction producer (decompression / relocation under ADR 0142 build-time materialization); nothing here addresses it.

## Build economics (one host, 16 cores; `--jobs 8`)

| build | wall | CPU | peak child memory | rounds | executable |
|---|---|---|---|---|---|
| frozen B1, empty object cache | 5 min 24 s | 1,162 s | 0.64 GiB | 7 | 119.6 MB |
| B1, reproduced with the old cache | 5 min 19 s | 1,129 s | 0.64 GiB | 7 | 119.6 MB (**byte-identical to the cold build**) |
| B0 (M4 baseline, runtime objects reused) | 4 min 03 s | 601 s | 0.64 GiB | 7 | 119.6 MB |
| Compatibility R0 (ADR 0225) | 10 min 16 s | 1,033 s | 1.13 GiB | 17 | 53.6 MB |
| Compatibility R1, automatic (stops at the Word RAM module) | 8 min 25 s* | 1,027 s | 0.86 GiB | 14 | 52.1 MB |
| Compatibility R1 with developer extents | 10 min 16 s* | 1,538 s | 1.24 GiB | 14 | 131.8 MB |
| Compatibility R2 | 9 min 27 s* | 1,151 s | 0.99 GiB | 15 | 56.0 MB |

(*two builds shared the host.) The frozen build is a fixed cost: route-independent, and the same executable serves R0-R2, whereas each Compatibility control is a different executable per route and the one with developer-found Word RAM extents is the largest. Build CPU is not smaller; its wall time is, through fewer rounds. Generated source: 261 MB (86.6 MB of body text, 27.9 MB of per-image maps).

## Runtime economics (median of three runs, no capture, empty `PATH`)

| route | frozen B1 | Compatibility control | ratio |
|---|---|---|---|
| R0 (74.5 M instructions) | 17.7 s | 15.0 s | 1.18 |
| R1 (69.0 M) | 17.8 s | 13.7 s (developer extents) | 1.30 |
| R2 (67.3 M) | 16.6 s | 13.9 s | 1.20 |

Attribution (callgrind, first 50 M instructions of R0, 22.5 M of them dynamic; share of all machine instructions the process executed, so it counts the shared trace hash, 38 %, and the start-up disc verification, 25 %, which the controls also pay): the dynamic machinery itself is **about 11 %**: the write hook that dirties 64-byte granules `scd_dynamic_note_write` 5.7 %, binding resolution 2.8 %, dispatch 1.4 %, per-instruction image checks 1.1 %, the table lookup and bit set 0.3 %; the per-instruction byte guard (`scd_code_guard`, `scd_code_matches`) 3.5 % exists in both arms. The 18-30 % wall-time excess is larger than 11 % of instructions; the remainder is not attributable with the tools available here (no hardware counters): the 119.6 MB executable's instruction-cache and TLB footprint is the untested hypothesis.

## Optimization decision

None. The unvisited-module proof is complete, so ADR rule 1 would allow a small change; but the largest single item (the write hook, 5.7 % of instructions) would save at most a few percent on a result that is already correct, and no change was measured on the same control; recording the economics is the outcome. Candidate for a later task: restrict the hook to writes inside a live binding's extent, with the same equality tests.

## Reading

The design is feasible and sound on the exercised evidence, at a fixed 2.2x executable size and 18-30 % run time. Whole-game completeness is not demonstrated in any dimension; the two inexpensive next experiments are further independent routes (more levels), and a source-backed code map to price the structural policy's recall.
