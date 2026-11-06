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

The frozen trigger is wider than this cause: closure runs on any prune rejection once a thunk exists. A proposal the prune would reject for another reason, for example one missing the machine roots, is completed instead of falling back. This is the pre-registered candidate, so it was not narrowed; a synthetic test pins it.

## 2. Closure statistics

`CLOSE`, final emission. Rounds are closure waves. `added` is `|K \ K0|`; `roots_added` is the part of it that the mandatory rule adds directly.

| workload | U | K0 | K | K/U | roots_added | other structural | closure rounds | RAM thunks | alias bytes | validator | policy |
|---|---|---|---|---|---|---|---|---|---|---|---|
| T1 standalone | 989,196 | 357,821 | 365,268 | 36.9 % | 1 | 7,446 | 1,572 | 1 | 0 | accepted | `ml_region_gen2` |
| T2 combined | 1,981,348 | 711,028 | 733,827 | 37.0 % | 1 | 22,798 | 1,740 | 1 | 0 | accepted | `ml_region_gen2` |
| T3 combined + patch | 1,981,349 | 711,060 | 733,857 | 37.0 % | 1 | 22,796 | 1,740 | 1 | 0 | accepted | `ml_region_gen2` |
| Flicky | 87,967 | 38,544 | 46,133 | 52.4 % | 7,463 | 126 | 13 | 14 | 49,152 | accepted | `ml_region_gen2` |

- **Verified ROM-copy aliases:** none on T1-T3. Flicky has one 49,152-byte mixed code/data copy, whose identities are mandatory, as before; `roots_added` minus its 14 thunks is 7,449, which assumes no machine root fell outside K0 (not separately checked). Runtime byte-identity guards are unchanged.
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

Stop classes on T1-T3 are none. "Route end" is the pad script's quit: the report says `runner_resource_limit` with viewer outcome `window_closed`, which is not budget exhaustion. Limits:

- The route does not enter bosses, later zones, the ending, or every code path.
- The Sonic 3 control's local exact 2 MiB image is a Japan/Korea one, which stops at a region screen on the route's USA console. Its generated C is byte-identical to `INC`, so the route adds little.
- Flicky's route ends early, in both arms, at an existing limit that this change neither causes nor fixes. It stops before the first capture and its report has no dispatch count, so those two fields compare empty with empty.
- Typed deviation from "instruction progress": dispatch count is the available measure; the report has no retired-instruction counter.
- Not run: the Windows and macOS builds with commercial images, a human at the window, and the packaged launcher's compiler (not installed here).
- Typed deviation from "launcher-equivalent": ADR 0269 fixed `--optimize 1`, the existing lock-on tooling's setting, for host memory. The launcher passes no `--optimize`, so its builds use the default `2`. G3 is therefore established at `-O1` only. T2's CPU margin is 2.6 points, and the launcher's reduction was not measured.

## 6. Delivered code

Candidate patch SHA-256 `17485a8101b189e9b21f030734be3ca1f3f66de88ae0b8d61a9c0f015fe22fdc` (`INC` measurement-only patch `04c5d9dc81b916e4df214e100c8009736eb2e5f5cc1ae6bc34b24afd6b7b657a`). Local, not committed. The pre-registration commit preceded every candidate build.

The delivered code was rebuilt on all six workloads. `k_sha256` and generated C digests equal the candidate's on all six. Executables differ only in build-path strings and the build ID. Routes of the delivered T1, T2, T3 and Flicky programs equal `BROAD` on every compared field.

## 7. Identity and compatibility consequences

- `genesis_admission_analysis_contract` is now `segarecomp.admission_contract.v2`. Policy identity changes for every Genesis request, so the launcher's Optimized cache keys change and **every Genesis Optimized library entry needs one rebuild to be current**, including v1 entries and Sonic 3 or Golden Axe, whose admitted sets did not change. Exact-key reuse of a build made under the previous policy is refused; the launcher can still offer such a same-mode build as a playable older build until then.
- Genesis Compatibility: its frozen build `identity` and its launcher cache key are unchanged, and the delivered CLI reproduced the base CLI's generated C digest on T1 and Golden Axe (the two rebuilt). Only the `policy_identity` string in its `status.json` changes.
- v1 stays prune-only: not evaluated. Master System and Sega CD have their own identities and do not use this route. The explicit `--admission-plan` route stays broad on a thunk emission.
- `status.json` adds `region_closure`, `added`, `roots_added` and `ram_thunks` only when closure ran; `pruned` is 0 then. Existing keys stay.

## 8. Consequence for players

Sonic & Knuckles and Sonic 3 & Knuckles were broad under the T009 fallback. They now run about 37 % of the broad universe. If play leaves the measured route and a dynamic transfer reaches a start outside K, the program stops with a typed stop. The viewer shows a "The game stopped" screen that does not mention Mode or Compatibility. The stops observed here exited with status 0 (with a full-report path set; the launcher's path to its exit was not traced), and the launcher treats exit 0 as clean, so the library would then show that the game ended normally rather than flag the stop. The automatic restart in Compatibility exists for Master System only, so a Genesis Optimized build does not recover by itself. A player can switch the Mode to Compatibility unprompted; that restarts from reset and needs a full broad build if none exists (about 390 s wall for T2 at `-O1`). Optimized is the launcher's default for Genesis, so these titles reach this path by default. This already holds for every selective Genesis title. The gates permit it, the routes do not show it, and later zones remain unmeasured.

## 9. Additional observation: every act of the combined image (not a gate)

Requested after the decision, to look for stops caused by missing code. It is not part of ADR 0269: it changes no gate and no decision.

Method: the delivered selective program of the combined image with one byte retargeted to re-enable the game's own level select and debug checks. Its `k_sha256` equals the real image's, so it admits the same set. All 28 acts in the level select, three routes each, 84 runs and 685,978 virtual frames, frame exact, finite instruction budget and wall timeout:

| route | runs | virtual frames | runs with a typed stop |
|---|---|---|---|
| debug-mode serpentine flight over the act | 28 | 358,303 | 1 |
| played start: run right with jumps | 28 | 110,460 | 0 |
| fly to the act's end, leave debug mode, play on | 28 | 217,215 | 2 |

The three stopping routes were replayed on a broad build of the same image. **All three stop identically in broad:** same stop class, same virtual frame (1,582, 3,488 and 1,497), and equal registers, SR, PC, final work RAM and report. The classes are `unresolved_indirect_target` with `reached_unresolved_direct_edge`, and `unsupported_memory_region` with `rom_write_prohibited` and with `odd_effective_address`. The broad program was built by the pre-change CLI, which supports "existing behaviour". **No stop on these routes is attributable to selective admission.** They are existing broad behaviour on routes that fly a debug cursor through a level; whether normal play can reach them is unknown, and they were not diagnosed here. Identical stops show admission independence, not correctness: a defect shared by both programs is not excluded, and a ROM write or odd access that stops the program may differ from hardware.

Limits:

- Only the stopping routes were replayed in broad. The other 81 runs show the absence of a typed stop, not equality with broad. Their VRAM, CRAM and VSRAM are not in these reports.
- The three stopped routes ended at about 20 %, 41 % and 54 % of their planned frames, so the remainder was never exercised.
- Flight coverage is unmeasured, with no per-frame trace. On 22 of 28 acts the sweep's final camera height is above a height another route of that act reached, which suggests the camera bounds clamp the descent, so a sweep may not cover its whole act.
- Not all frames are in the named act: 26 of 28 played routes and 23 of 28 end-of-act routes ended in the correct act; the others ended at the title screen, a demo, a loading state or the stop above.
- Debug flight activates objects along its path but mostly does not exercise player physics. The played routes are short, and many end in a level rather than at its boss. Boss fights and act transitions happen on these routes only incidentally, and were not counted.
- Not exercised: Knuckles and Tails alone, special and bonus stages, the ending, save paths, and most object interactions. Standalone S&K and the patched image were not swept.

The stop risk in section 8 stands: this lowers it for the covered paths and does not remove it.

## 10. Test coverage limits

Synthetic tests cover the trigger and every case that must not use it, the cap, tampered closed plans, alias plus thunk, determinism, the typed dynamic stop of a program built from a closed plan, and strict C11 of the real gen2 route with a thunk. Not synthetic: the `status.json` closure members, a run of a thunk-bearing selective program and its guards, and `--admission-plan` on a thunk emission (unchanged code). These were checked on the local images or by inspection. A byte mismatch at run time stays covered by the existing generated alias tests, whose code is unchanged.

## 11. Not answered

ADR 0114's `O3` (mandatory identities as K0 members, then a downward prune) stays unevaluated. It is not needed for this result. Whether a smaller K is possible, or whether the second compile pass could be avoided, was not investigated.

## 12. Post-decision, non-gating compilation-cost attribution

**Everything in this section was measured after the `PROMOTE` decision. It is not part of ADR 0269, changes no gate and no decision, and reinterprets nothing above.** The delivered implementation is unchanged.

**Answer:** selective admission does remove about 59 % of the compile cost of the final program. A thunk-bearing selective build then gives most of that back by compiling a complete first program it later discards, because re-emission shifts nearly every unit. Broad builds reuse their objects across the same round. Multi-pass compilation is the largest recoverable cost, not generated-C complexity.

### 12.1 Method and limits

- Inputs and settings: standalone S&K (T1) and combined S3&K (T2), delivered CLI, GCC 13.3, `--jobs 4`, one build at a time, clean directories, no shared object cache. `--optimize 1`, and `--optimize 2` for section 12.5.
- External accounting, no production change: a temporary `--cc` wrapper logging each compiler invocation's `wait4` CPU, RSS, input and output bytes and a text digest; `wait4` on `segarecomp build` for the total; `/proc` self and waited-children CPU sampled at 5 Hz with stage and log timestamps. Each cost below is counted once: invocation CPU comes from the wrapper log, and "other children" is the remainder after in-process, compiler and link CPU.
- Reproduction: fresh `-O1` totals are within 1 % of section 3 (T1 719.4 / 541.2 s, T2 1251.3 / 972.5 s).
- **Limits:**
  - Sections 12.2-12.4 and the counterfactual are single builds. CPU repeated within 1 % at `-O2` and 2.5 % at `-O1`, but they are not medians.
  - `analyze` and ML run inside the in-process `generate` stage, so that split comes from separate timed CLI runs.
  - Sampling can move about a second between stages, not between totals. Windows assign a compile to the stage it ended in.
  - "Other children" also holds the wrapper's small overhead.
  - The toolchain is not the packaged launcher's (Zig). Same `-O2` is not launcher equivalence.
  - Peak compiler RSS is unchanged by optimization level, because it is set by one large unit.

### 12.2 Stage accounting at `-O1`

CPU seconds. Rows add up to the total.

| stage | T1 broad | T1 selective | T2 broad | T2 selective |
|---|---|---|---|---|
| in-process, first emission (analysis, ML, planning, closure, emission) | 18.0 | 17.9 | 40.0 | 37.7 |
| in-process, second emission and other | 24.5 | 21.9 | 50.0 | 45.1 |
| M68K compile: objects in the final program | 603.2 | 234.0 | 1058.6 | 435.0 |
| M68K compile: superseded objects | 26.1 | **220.6** | 53.1 | **408.2** |
| Z80 generated-code compile | 37.4 | 38.3 | 37.1 | 37.0 |
| runtime, device and viewer compile | 4.0 | 3.7 | 4.0 | 3.7 |
| all linking (5 links) | 2.3 | 1.1 | 4.1 | 1.8 |
| other children (preparation and Z80 pass runs, wrapper overhead) | 3.9 | 3.7 | 4.5 | 4.0 |
| **total** | **719.4** | **541.2** | **1251.3** | **972.5** |

- Timed separately on T2: analysis plus prune 4.6 s, analysis plus ML inference 24.8 s, so inference is about 20 s **per emission**, run twice. Closure and prune cost about the same. Emission of the closed set is about 21 s.
- Running the preparation and Z80 pass programs costs almost nothing.
- Work outside the two M68K compile rows is 11 % of the T2 broad build and 13 % of T2 selective. That includes ML and emission, which selective admission does not remove.

### 12.3 Emissions and compiled volume

T2, `-O1`. Emission 1 is the alias-less one; emission 2 holds the thunk. "Units" counts M68K compile invocations, including the two variants of the main unit.

| | broad 1 | broad 2 | selective 1 | selective 2 |
|---|---|---|---|---|
| universe U | 1,981,347 | 1,981,348 | 1,981,347 | 1,981,348 |
| admitted | all | all | 678,653 (prune) | 733,827 (closure) |
| effective policy of that emission | broad | broad | `ml_region_gen2` | `ml_region_gen2` |
| units compiled / reused | 57 / 0 | **4 / 53** | 57 / 0 | **57 / 0** |
| C compiled | 935.4 MB | 227.7 MB | 436.8 MB | 456.7 MB |
| compile CPU | 1063.3 s | 48.3 s | 408.2 s | 435.0 s |
| objects carried to the final program | 53 | 4 | **0** | 57 |
| superseded CPU | 53.1 s | | 408.2 s | |

The 227.7 MB of broad's second round is mostly the main and entries units, which are cheap to compile, and only one AOT unit.

| generated C | T1 broad | T1 selective | T2 broad | T2 selective |
|---|---|---|---|---|
| final program | 515.1 MB | 227.3 MB | 883.9 MB | 405.2 MB |
| compiled over the whole build | 666.4 MB | 505.3 MB | 1180.6 MB | 910.9 MB |
| reduction, final / lifetime | | -55.9 % / -24.2 % | | -54.2 % / -22.8 % |

The lifetime reduction matches the CPU reduction. The final emission's size is not what the build compiles.

### 12.4 Why unit reuse fails, and hotspots

- **The content-keyed cache works when text is stable.** Broad reuses 53 of 57 units between rounds. Selective reuses none.
- **Instruction text is not what changes.** On T2, 124,612 of the 126,577 labelled AOT entries present in both rounds (98.4 %) keep identical segment text, but only 62,956 (49.7 %) stay in the same shard. The admitted set grows from prune to closure (+55,174 identities), so positional shards shift and almost all unit text changes. Three units are textually identical but are recompiled anyway, because the key covers the changed shared header. That costs only 0.6 s. Closure is what changes K between the rounds; the single thunk identity is not. Emission-only check through the delivered CLI: a closed first emission without the thunk has 55 of the final emission's 58 generated files byte-identical, including the shared header and unit manifest.
- **No small hotspot dominates.** Final T2 selective compile: largest unit 2.9 %, top 5 13.2 %, top 10 24.3 %. Within the largest AOT unit, the largest top-level function is about 0.1 % of its text. Spread across 56 units, there is no outlier to fix.
- **Source size alone mispredicts cost.** T2 selective, final round, `-O1`:

| family | units | CPU s | share | MB | s/MB |
|---|---|---|---|---|---|
| immutable-ROM AOT bodies | 32 | 265.3 | 61 % | 224.1 | 1.18 |
| shared helpers | 16 | 124.8 | 29 % | 71.9 | 1.74 |
| precise-discovery blocks | 4 | 24.7 | 6 % | 4.2 | 5.9 |
| main | 2 | 15.4 | 4 % | 125.8 | 0.12 |
| entry tables | 1 | 4.4 | 1 % | 30.6 | 0.14 |

### 12.5 `-O2` (CLI default, the optimization level the launcher's builds use)

Not a gate, and T010's `-O1` result stands. No compiler failure, and available memory stayed above 6 GB. T2 is the median of three cold builds per arm; T1 is one each, so preliminary.

| workload | broad CPU / wall | selective CPU / wall | CPU reduction | C reduction | executable broad / selective | peak compiler RSS |
|---|---|---|---|---|---|---|
| T2 | 2040.3 s / 573.8 s | 1582.2 s / 440.9 s | -22.5 % | -54.2 % | 187.1 / 81.3 MB | 1,924 / 1,884 MiB |
| T1 (preliminary) | 1166.5 s / 322.7 s | 878.4 s / 240.9 s | -24.7 % | -55.9 % | 106.7 / 46.1 MB | 815 / 789 MiB |

T2 stage shape at `-O2`, first sample: final-object M68K compile 1813.1 s against 734.0 s (-59.5 %), superseded 73.1 s against 697.3 s, Z80 57.9 / 59.5 s. The pattern is the same as `-O1` and larger in absolute terms.

### 12.6 Reconciliation of the discrepancy

T2 at `-O1`, total difference selective minus broad, -278.8 s:

| component | change |
|---|---|
| final-program M68K compile (-58.9 %, comparable to the C reduction) | **-623.6 s** |
| extra superseded M68K compile | **+355.1 s** |
| in-process work | -7.2 s |
| Z80, runtime, linking and other children | -3.1 s |
| total | -278.8 s |

The same reconciliation on T1: -369.2, +194.5, -2.7, -0.8, total -178.2 s. **So about 57 % of the T2 saving is spent on superseded objects (53 % on T1), and non-M68K work dilutes the rest a little.** This is measured, not inferred.

**Counterfactual (local scratch patch, never committed):** close every gen2 emission, so the first emission already holds the final set apart from the thunk. On T1 and T2 the final generated C and `k_sha256` are identical to the delivered build, and only 4 units recompile in round two. The executables differ in 12,385-12,390 bytes, unexplained; these programs were not route-tested, and the patch is a measurement, not a proposal.

| workload | broad | delivered | counterfactual | vs broad | K, delivered / counterfactual |
|---|---|---|---|---|---|
| T1 `-O1` | 719.4 s | 541.2 s | **330.2 s** | -54.1 % | equal |
| T2 `-O1` | 1251.3 s | 972.5 s | **602.1 s** | -51.9 % | equal |
| T2 `-O2` | 2040.3 s | 1582.2 s | **932.2 s** | -54.3 % | equal |
| Sonic 3 `-O1` (no thunk) | | 303.1 s | 317.1 s | +4.6 % vs delivered | 338,838 / 369,013 (+8.9 %) |
| Golden Axe `-O1` (alias, no thunk) | | 195.7 s | 208.2 s | +6.4 % vs delivered | 83,924 / 93,188 (+11.0 %) |

Golden Axe compiles everything twice in both, because its alias round also shifts the units. Flicky was not attributed.

### 12.7 Existing opt-in switches

`--aot-operand-sharing` and `--shared-return-targets` are accepted by the emit command and `tools/genesis_startup_bridge.py --emitter-flag`, but **`segarecomp build` does not forward them**, so the launcher's path cannot use them. Measured through the delivered emit command with the delivered thunk on T2, final emission only: the base arm's generated files equal the delivered build's. Objects were compiled with the same flags; **no program was linked or run**, so behaviour and executable size are unmeasured and object text stands in. One sample each.

| arm | C MB | CPU `-O1` | CPU `-O2` | object text `-O1` | peak RSS |
|---|---|---|---|---|---|
| current | 400.6 | 433.2 s | 729.9 s | 74.9 MB | 1,883 MiB |
| operand sharing | 344.8 (-13.9 %) | 363.2 s (-16.2 %) | 605.1 s (-17.1 %) | 66.3 MB | 1,891 MiB |
| shared return targets | 396.3 (-1.1 %) | 406.7 s (-6.1 %) | 691.8 s (-5.2 %) | 73.7 MB | 1,884 MiB |
| both | 340.5 (-15.0 %) | 335.3 s (-22.6 %) | 564.8 s (-22.6 %) | 65.2 MB | 1,891 MiB |

### 12.8 Ranked hypotheses

Bounds are upper limits, not predictions. T2 figures.

| rank | hypothesis | observed cost | bound, `-O1` / `-O2` | scope and reuse | risk and evidence needed |
|---|---|---|---|---|---|
| 1 | **H1 repeated compilation** | 408.2 s / 697.3 s superseded, 42-44 % of the build | 408 / 697 s; the counterfactual actually recovered 370 / 650 s | Small: which set the first emission uses. Genesis gen2 thunk titles; alias titles need more | Changes K on thunk-less titles (+5-6 % CPU measured on two). K only grows, so no identity is lost. Needs its own pre-registration, wider title set, behaviour routes |
| 2 | H2 generated-C representation | 390 s / 675 s final AOT and shared compile | measured 98 / 165 s on the final emission with both switches; multi-pass effect unmeasured | Small plumbing to expose existing switches; all Genesis | Behaviour on these titles and runtime speed unmeasured; ADR 0158 kept them opt-in. Needs a full-build and route comparison |
| 3 | H3 admission precision | same 390 s pool | under half of it even if K halved, roughly 195 / 340 s, and only on stated assumptions | Model or policy work; cross-title | Highest risk: excluding code raises fail-closed stops. No exact-code evidence exists for these images |
| 4 | H4 portable position-independent bodies (ADR 0221-0226) | none attributable | not above H1's bound | Large: bodies, binding and runtime dispatch | The cost is unit churn from a changed K, not RAM code: the thunk is one 6-byte instruction with no ROM source to key on. Sega CD solves late binding of ROM-copied code, a different problem, and adds runtime and size costs |

H1 and H2 are not exclusive. H1 is the larger on every figure above, so it comes first. The second ML inference (about 20 s per build) is a small independent saving.

### 12.9 Recommendation

**Next task: pre-registered evaluation of a stable first emission for gen2 Optimized**, closing the first emission so a thunk discovered later recompiles only a few units. The evidence supports this as the smallest next step, and it reuses the T010 mechanism. Open questions for that task, none answered here: whether to accept +5-6 % on thunk-less titles or find a cheaper way to keep them unchanged; alias titles such as Golden Axe and Flicky; and route evidence. Exposing the switches (H2) is the follow-on, after a full-build comparison.

Hypotheses versus established results: sections 12.2-12.7 are measurements, and the counterfactual measures a local patch; the "bound" column and the recommendation are judgement. None of it is implemented.
