# ADR 0226: SEG-100 - milestone M4 decision: file-first broad AOT and dynamic verified module mapping

- Status: Accepted. Final decision of milestone SEG-100 (whole-game static AOT, M4). Related: ADR 0221-0225; ADR 0220 (M3, `M3_PARTIAL`).
- Privacy: classes and counts only for the authorized local titles.

## Task decisions

| Task | ADR | Gate |
|---|---|---|
| T001 survey and feasibility | 0221 | `FILE_FIRST_AOT_FEASIBLE` (with the limits listed there) |
| T002 source-offset bodies | 0222 | `DYNAMIC_BASE_AOT_GO` (native correctness only) |
| T003 dynamic binding | 0223 | `DYNAMIC_MAPPING_GO` |
| T004 inventory and build | 0224 | opt-in `--file-first all|structural`, typed accounting, no default change |
| T005 authentic evaluation | 0225 | Sonic CD: 9 authentic modules from pre-compiled bodies, no base supplied, state identical to the reference; Earthworm Jim SE: no gain, no regression |
| T006 review | this record | one independent adversarial review; two blockers found and fixed (below) |

## Explicit milestone decision: `M4_GO`

Conditions and their status:

1. Dynamic-base generated-native execution works on project fixtures - **met** (three routes, one module at two bases, a module over another's address, a static extent shadowing an address; run reports equal the static reference's).
2. Dynamic source-to-RAM binding is verified and deterministic - **met** (40+ binder checks including replacement, partial overwrite, bank change, mutated data next to intact code, ties, ledger overflow, region boundaries, a negative base; determinism of events and of emission across worker counts).
3. At least one additional authentic Sonic CD module is statically compiled and executed at a base not supplied at build time - **met, under the definition stated here**: nine authentic modules (both processors; four of them successively over one Work RAM base) executed from bodies compiled before the first instruction ran, with none of their bases, names or entries supplied, where the M3 static producers admitted none. **It is not an improvement over the route-derived pipeline in executed code or in gameplay reach** (identical coverage, the same independent audio frontier); the criterion's "additional" is read against the static admission that M3 failed at, not against the execution-derived producer that learns bases by running the game. A reviewer who reads it against the latter should treat the result as `M4_PARTIAL`; the evidence supports either reading and is recorded in full.
4. The authentic module executes through the supported build - **met** (`segarecomp sega-cd build --file-first`). The launcher's Build button runs that same command (the option is passed through its documented `SEGARECOMP_SCD_BUILD_ARGS` seam); the SDL3 launcher itself was not built or run here.
5. No runtime opcode decoding or compilation - **met** (the runtime contains table lookups and byte comparisons; review item 4; the generated text contains no decoder; the binary is unchanged by runs).
6. No false image selection, no executable-authority regression - **met on the evidence available**: zero ambiguous mappings and refusals, and equality of the complete machine state with the reference over 74.5 million instructions, 59 % of them through dynamic bodies. Authority is per instruction (ADR 0223); the structural policy and compilability claim nothing.
7. Build and runtime costs bounded and measured - **met** (ADR 0221, 0224, 0225): +66 MB of executable, +38 % run time, 7 rounds instead of 17, bounded memory in every stage, every limit typed.
8. Existing Compatibility functionality remains valid - **met**: the fast tier passes 184 of 184 locally (2 skipped by their own preconditions); a program built without `--file-first` is byte-identical and compiles with the strict flags (see B1 below).

## Independent review (T006) and fixes

One adversarial pass (separate agent, falsification oriented, fuzz under ASan/UBSan) reproduced two **blockers**, now fixed with regressions: **B1** a new trailing member of the brace-initialized `ScdProgram` made every program without `--file-first` fail the repository's strict compile flags (24 of 45 Sega CD suites) - the dynamic program is now registered by the generated `main` and `ScdProgram` is unchanged, so non-file-first generated text is unchanged and compiles; **B2** an image whose identifying run lies in the image's tail near address 0 gave a negative base and an out-of-range bit write - such a candidate is skipped, binding extents are clipped to the memory they were found in, and a test covers it. Minor findings: M1/M2 fixed by the same clipping (a binding no longer covers the work-RAM mirror, so writes to the canonical alias dirty it); M3 (search cost) is bounded and measured (75 reads, 3.9 MB on the authentic route) and left; M4 (the 32-byte anchor need not be in a delivered block - safe because every instruction is verified, noted); M5 (a duplicate-content file is shipped once, with only its first copy's blocks: availability only, visible as `duplicate_files`) carried as a limitation; M6 (track offsets above 4 GiB are typed `digest_mismatch`) accepted; M7 privacy: counts only, no address, hash, LBA, name or byte (the per-size-class file histogram was kept: it identifies no content). Untouched and unaffected: Genesis and Master System (the diff touches only Sega CD paths).

## What M4 does not establish (remaining blockers to whole-game static compilation)

- Any gameplay beyond the route the machine can run: the `pcm_unsupported` frontier of Sonic CD (documented, independent, not addressed here).
- Code that is transformed or patched into RAM (the Earthworm Jim SE frontier is `no_source`): decompression/relocation producers remain deferred (ADR 0220).
- Files above 512 KiB (82 % of Sonic CD's bytes) and any executable that is not an ISO9660 file.
- A route that loads a module the build pass never loads, on an authentic title (shown on project-authored discs only).
- Executables that cost less: the program is 2.2 times larger and the route 38 % slower; adoption as a default would need a smaller index and a measured benefit on a route the existing pipeline cannot cover.

## Consequences

The capability is retained as an explicit opt-in option of `emit`/`build` with its tests; nothing in the Compatibility build, Genesis or Master System changes; no default is proposed. The existing static, execution-derived and overlay producers remain the fallback and keep precedence. SEG-101 and any successor are **not** started.

