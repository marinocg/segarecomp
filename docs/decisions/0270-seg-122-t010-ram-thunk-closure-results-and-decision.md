# ADR 0270: SEG-122-T010 selective Optimized AOT for RAM-thunk Genesis cartridges - results and decision

- Status: Accepted. **Decision: `PROMOTE`.** Every gate of ADR 0269 passed, applied unchanged.
- Change: for a gen2 Optimized emission, when the unchanged prune rejects and the emission holds at least one materialized RAM thunk, the existing `close_genesis_region_admission`
  runs on the same proposal and its plan goes to the unchanged validator. Any failure, including the closure's resource cap, stays the broad fallback. Nothing else changes admission.
- Limits: the routes below are evidence for those routes only. They do not show game compatibility or that all dynamic targets are known. Structural closure proves nothing about dynamic targets.

## 1. Why the prune rejects (G1)

Reproduced on standalone Sonic & Knuckles (T1), combined Sonic 3 & Knuckles (T2) and the combined image with the native patch (T3). In one preparation round:

1. The first emission has no thunk. Its proposal is admitted (`K/U` about 35 %).
2. Its confirming run stops at a work-RAM target that holds exactly one `JMP`. Preparation materializes it (ADR 0097 amendment): 1 thunk, 6 bytes, 0 aliases.
3. The next emission's universe has one more identity. The validator makes every materialized identity mandatory, and a thunk has no cartridge source, so it is never in the model's cartridge ranges.
4. The prune therefore rejects with `materialized_image_not_admitted` / `outside_region`, reported as `prune_rejected` / `proposal_rejected`, and the program is broad. The proposal digest is the same in both emissions: only the thunk identity changed the outcome.

## 2. Closure statistics

`CLOSE`, final emission. Rounds are closure waves. `added` is `|K \ K0|`; `roots_added` is the part of it that the mandatory rule adds directly.

| workload | U | K0 | K | K/U | roots_added | other structural | closure rounds | RAM thunks | alias bytes | validator | policy |
|---|---|---|---|---|---|---|---|---|---|---|---|
| T1 standalone | 989,196 | 357,821 | 365,268 | 36.9 % | 1 | 7,446 | 1,572 | 1 | 0 | accepted | `ml_region_gen2` |
| T2 combined | 1,981,348 | 711,028 | 733,827 | 37.0 % | 1 | 22,798 | 1,740 | 1 | 0 | accepted | `ml_region_gen2` |
| T3 combined + patch | 1,981,349 | 711,060 | 733,857 | 37.0 % | 1 | 22,796 | 1,740 | 1 | 0 | accepted | `ml_region_gen2` |
| Flicky | 87,967 | 38,544 | 46,133 | 52.4 % | 7,463 | 126 | 13 | 14 | 49,152 | accepted | `ml_region_gen2` |

- **Verified ROM-copy aliases:** none on T1-T3. Flicky has one 49,152-byte mixed code/data copy; its 7,449 copy identities outside K0 are mandatory, as before. Runtime byte-identity guards are unchanged.
- **Materialized thunks:** 1 on T1-T3, 14 on Flicky (Flicky's `roots_added` includes them). Runtime whole-instruction guards are unchanged.
- **Fixed structural successors:** the "other structural" column. It holds the fixed targets and call continuations of K0, of the mandatory identities and of each thunk's `JMP`. The counts do not attribute them separately. Most K0 false decodes the prune would remove stay in K. That was the concern before measurement; it cost little here.
- **Parent copy:** T1-T3 have no parent copy, so a thunk there brings only its fixed `JMP` target. On Flicky, whose thunks are called from the copy, closure admits the whole mandatory copy and the validator accepts. Admitting the thunk alone would not have been enough there.
- **Dynamic targets not proven by closure:** not counted, because closure cannot know them. They stay fail-closed; a synthetic generated program shows the typed stop (`unresolved_indirect_target`).

## 3. Cost (G3)

`segarecomp build`, GCC 13.3 `-O1`, `--jobs 4`, one build at a time. CPU is the median of three cold builds. "C" is all generated C including the Z80 units.

| workload | `BROAD` CPU s / C MB | `INC` CPU s / C MB | `CLOSE` CPU s / C MB | `CLOSE` vs `BROAD` CPU / C | `CLOSE` vs `INC` CPU |
|---|---|---|---|---|---|
| T1 | 715.1 / 521.7 | 914.7 / 521.7 (fallback) | 542.1 / 230.7 | **-24.2 % / -55.8 %** | -40.7 % |
| T2 | 1261.0 / 895.8 | 1624.2 / 895.8 (fallback) | 976.3 / 412.0 | **-22.6 % / -54.0 %** | -39.9 % |
| T3 (reported) | 1252.3 / 895.8 | 1627.5 / 895.8 (fallback) | 988.3 / 411.9 | -21.1 % / -54.0 % | -39.3 % |
| Sonic 3 | 626.1 / 494.8 | 304.1 / 236.1 | 300.2 / 236.1 | unchanged vs `INC` | -1.3 % |
| Golden Axe | 433.0 / 174.4 | 196.5 / 78.4 | 198.6 / 78.4 | unchanged vs `INC` | +1.1 % |
| Flicky | 639.6 / 104.2 | 632.7 / 104.2 (fallback) | 594.2 / 95.4 | -7.1 % / -8.4 % | -6.1 % |

| workload | peak compiler RSS `BROAD` / `CLOSE` (MiB) | executable `BROAD` / `CLOSE` (MB) | wall `BROAD` / `CLOSE` (s) |
|---|---|---|---|
| T1 | 815 / 790 | 110.3 / 47.2 | 213 / 163 |
| T2 | 1,924 / 1,884 | 193.5 / 83.2 | 390 / 303 |
| T3 | 1,924 / 1,884 | 193.5 / 83.2 | 381 / 307 |

CPU spread within an arm was at most 2.5 % across the three samples. C falls by more than twice as much as CPU on the targets. A thunk-bearing build compiles the first emission and then the changed units of the next; Sonic 3, with no thunk and about the same C, costs about 300 s. T2's CPU margin is the tightest gated figure, 2.6 points above the threshold.

## 4. Gates

| gate | result | evidence |
|---|---|---|
| G1 reproduction | pass | section 1; `INC` on T1, T2 (and T3): `prune_rejected` / `outside_region` / `proposal_rejected`, 1 thunk |
| G2 structure | pass | T1-T3: accepted, `ml_region_gen2`, no fallback, no cap exhaustion. Proposal digests equal `INC`'s final emission. Preparation termination `route_advanced`, 1 round. Work-RAM identity sets equal `BROAD`'s by digest |
| G3 economics | pass | section 3: T1 and T2 at least 20 % below `BROAD` in both. RSS and executable below `BROAD`. K/U far under the 0.90 early stop |
| G4 reproducibility | pass | all three `CLOSE` builds of each target have identical `k_sha256` and generated C digests; two runs of the route are identical |
| G5 behaviour | pass | section 5; T1-T3 equal `BROAD` on every compared field, with no typed stop |
| G6 controls | pass | Sonic 3, Golden Axe: same `k_sha256` and generated C digest as `INC`, equal route. Flicky: selective; same conditions pass (below); CPU 93.9 % of `INC` against the 110 % limit |

Flicky under G2, G4 and G5: validator accepted, proposal digest equal, preparation 15 rounds, 1 alias and 14 thunks with an equal work-RAM identity set; identical digests across builds and runs; equal to its `BROAD`. Its round after the alias and before the first thunk is broad, as before, because that emission holds no thunk. Compared with ADR 0111's unregistered `O3` (52.2 %), closure admits a similar share (52.4 %).

## 5. Runtime equivalence

Frame-exact pad route (`SEGARECOMP_VIEWER_SLICE=1`, instruction budget 4,294,967,295, wall timeout 1800 s), run on the first build of each arm. Compared between `CLOSE` and `BROAD`: result and stop class, dispatch count, virtual frames, runtime notes, audio frame count and digest, data registers, address registers, USP, SR, final PC, a digest of final work RAM, and six frame captures (polls 1000-10500; fewer where the run ended earlier). The programs report generated-native execution.

| workload | virtual frames | audio frames | end | difference from `BROAD` | what the route reaches |
|---|---|---|---|---|---|
| T1 | 11,301 | 8,316,837 | route end | none | gameplay, capture inspected |
| T2 | 11,301 | 8,316,837 | route end | none | gameplay, capture inspected |
| T3 | 11,301 | 8,316,837 | route end | none | 16:9 gameplay, capture inspected |
| Sonic 3 | 11,301 | 8,316,837 | route end | none | region-lock screen, silent: **weak** |
| Golden Axe | 11,303 | 8,318,309 | route end | none | gameplay, capture inspected |
| Flicky | 501 | 368,649 | typed stop `known_but_unemitted_target` | none: broad stops the same way | early prefix only: **weak** |

Stop classes on T1-T3 are none. Limits:

- The route does not enter bosses, later zones, the ending, or every code path.
- The Sonic 3 control's local exact 2 MiB image is a Japan/Korea one, which stops at a region screen on the route's USA console. Its generated C is byte-identical to `INC`, so the route adds little.
- Flicky's route ends early, in both arms, at an existing limit that this change neither causes nor fixes.
- Dispatch count is the available progress measure; the report has no retired-instruction counter.
- Not run: the Windows and macOS builds with commercial images, a human at the window, and the packaged launcher's compiler (not installed here).

## 6. Delivered code

Candidate patch SHA-256 `17485a8101b189e9b21f030734be3ca1f3f66de88ae0b8d61a9c0f015fe22fdc` (`INC` measurement-only patch `04c5d9dc81b916e4df214e100c8009736eb2e5f5cc1ae6bc34b24afd6b7b657a`). Local, not committed. The pre-registration commit preceded every candidate build.

The delivered code was rebuilt on all six workloads. `k_sha256` and generated C digests equal the candidate's on all six. Executables differ only in build-path strings and the build ID. Routes of the delivered T1, T2, T3 and Flicky programs equal `BROAD` on every compared field.

## 7. Identity and compatibility consequences

- `genesis_admission_analysis_contract` is now `segarecomp.admission_contract.v2`. Policy identity changes for every Genesis request, so the launcher's Optimized cache keys change and **every Genesis Optimized library entry rebuilds once**, including v1 entries and Sonic 3 or Golden Axe, whose admitted sets did not change. A fresh build cannot reuse a build made under the previous policy.
- Genesis Compatibility: generated output, its frozen build `identity` and its launcher cache key are unchanged. Only the `policy_identity` string in its `status.json` changes.
- v1 stays prune-only: not evaluated. Master System and Sega CD have their own identities and do not use this route. The explicit `--admission-plan` route stays broad on a thunk emission.
- `status.json` adds `region_closure`, `added`, `roots_added` and `ram_thunks` only when closure ran; `pruned` is 0 then. Existing keys stay.

## 8. Not answered

ADR 0114's `O3` (mandatory identities as K0 members, then a downward prune) stays unevaluated. It is not needed for this result. Whether a smaller K is possible, or whether the second compile pass could be avoided, was not investigated.
