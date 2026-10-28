# ADR 0220: SEG-099 - milestone M3 decision: authentic loader discovery, bounded state analysis and recursive AOT inventory

- Status: Accepted. Final decision of milestone SEG-099 (whole-game static AOT, M3). Related: ADR 0215-0219; ADR 0210-0214 (M1/M2 evidence).
- Privacy: classes and counts only for the authorized local titles; no address, hash, name, LBA or byte.

## Roadmap refinement (M2 evidence)

SEG-099 was a directional placeholder for offline reconstruction of compressed, relocated or transformed code. M2 (ADR 0214, `M2_PARTIAL`) showed a nearer blocker: authentic loaders whose arguments are not constants. This milestone therefore delivered, in order: the BIOS file/CD-service ABI and loader-state feasibility (T001), work-variable state with source-to-mapping proofs (T002), state convergence (T003), bounded recursive loader discovery (T004), and the small coverage-policy / diagnostics / provenance items (T005).
**Deferred, not discarded:** provenance-carrying offline decompression and relocation producers (ADR 0142 build-time materialization). The only transformation fixture of this milestone (a module decoded in place) is reported as unresolved (`loaded_bytes_unknown`), not reconstructed; this milestone did not establish that such a producer is needed *before* the command-supplied file selectors below, and no bounded, independently justified producer was built. Nothing here implements or claims it.

## Task decisions

| Task | ADR | Decision |
|---|---|---|
| T001 BIOS service ABI | 0215 | `BIOS_LOADER_ABI_FEASIBLE` for the sector-read subset (ROMREADN/ROMREAD, CDCSTOP/STAT/READ/TRN/ACK, device byte); `UNRESOLVED` for what an SP-only walk of an authentic title needs: the BIOS has no file-name service; names come from the other processor's commands |
| T002 work-variable state | 0216 | `MEMORY_CARRIED_DISCOVERY_PARTIAL` |
| T003 state convergence | 0217 | `STATE_CONVERGENCE_GO` (mechanism; thin authentic margin; precision cost recorded) |
| T004 recursive discovery | 0218 | `RECURSIVE_DISCOVERY_GO` (mechanism; no authentic effect) |
| T005 policy, diagnostics, provenance | 0219 | `COVERAGE_POLICY_VALIDATED` (keep `hull`); computed destination reported separately; `static_extent_unproven` guard |

## What the authentic loaders showed, and what was demonstrated

Demonstrated on project-authored BIOS-image discs through the supported `segarecomp sega-cd build` and launcher path (Compatibility, no module list, no gameplay script): a loader that keeps its arguments in a RAM work area and calls the documented BIOS services is analyzed by the existing CPU-owned walk; the file is found *by name through the ISO9660 volume descriptor and root directory evaluated over validated disc bytes*;
a two-sector module is proven as one run; the module's own loads (boot SP -> A -> R) are found recursively; every proven module is compiled before the build pass, and the SAME binary then executes a module the build pass never loaded; a changed instruction is a typed `code_bytes_mismatch`; a transformed module, an unknown alias, an unmodelled or unsupported service, a corrupt descriptor or an extent outside the track, a load that is never executed and an unknown entry each produce a typed refusal, never a module.

Not demonstrated on an authentic title: any mechanism. The authentic SPs call exactly the documented ABI, but (a) the file selector of each load is a name supplied by the main processor's command and looked up by title code, (b) the command is dispatched through a computed transfer on a value the SP cannot know, (c) the interrupt entry reaches a BIOS vector the ABI table does not cover or a transfer the walk cannot resolve, so no tracked memory is trusted there (`async_volatile_all`), and (d) 7 of 12 observed Sonic CD sources are main-processor destinations (the sub program only sets the CDC device; the main CPU reads the data).

## Authentic before / after (identical authorized inputs, Model 1 BIOS, no gameplay script, formatted empty backup RAM)

| Title | M2 baseline | This head |
|---|---|---|
| Sonic CD (USA) | 17 rounds, 12 execution-derived extents, **0 static modules**, stop `pcm_unsupported` (`device_stop`), inventory `partial`; static scan 1 `computed_control_unresolved` | **identical**: 17 rounds, 12 extents, 0 static modules, `pcm_unsupported`, `partial`; scan 13 states / 44 steps (the final head; 84 / 1,229 before the review fixes, when a PC-relative table read was still trusted after an unknown store), 0 sector transfers resolved; build 383 s wall / 398 s CPU / 583 MB peak on a quiet host (620 s when run beside the validator and the test suite; the M2 reference run took 616 s); the static refusal is again the single `computed_control_unresolved` of M2 |
| Earthworm Jim SE (USA) | 6 rounds, 2 extents, static scan exhausted its 4,096 states, stop `entry_not_compiled` (`no_source`), inventory `blocked` | 6 rounds, 2 extents, same stop and inventory; static scan **completes** (3,528 states / 64,589 steps / 294 joins, 5.5 s, 10 MB) with the typed refusal `destination_unknown`; 0 static modules; build 151 s wall / 298 s CPU / 554 MB |

Sources / mappings / instruction starts added before gameplay by the static producer: **0 / 0 / 0** on both titles; false admissions: 0; no valid overlay variant shadowed (no static admission exists to shadow one). The Compatibility route did not regress: 180/180 fast-tier tests pass locally, including every earlier Sega CD suite; the build's `static_scan_joined` and the corrected inventory labels are the only status changes. The frozen M3+B35 model, its default and Compatibility's role as reference were not touched. No claim is made that either title is complete or playable, and the static-coverage result is reported separately from the `pcm_unsupported` hardware stop.

## Economics and determinism

Scan time is seconds on both titles (5.5-5.8 s; peak 10-12 MB); the cost of the whole build is dominated by the build passes, not by the scan. Walks are deterministic (same module list, refusals and completeness on repeat and, for every project fixture, across four exploration orders; refusal classes too when no join happens). Resource bounds were not raised except where a new mechanism introduced its own bound (64 abstract-RAM pages, 64 caller contexts, 64 walked module entries); the state/step/operation/module bounds are the M2 ones.

## Explicit milestone decision: `M3_PARTIAL`

Conditions of `M3_GO` and their status: (1) the new memory-carried mechanism works - **met** on hermetic discs through the supported build and launcher; (2) at least one additional executable module on an authorized authentic title independently justified and compiled before execution - **not met** (0 on both); (3) recursive discovery and state handling pass their safety gates - **met**; (4) no compatibility regression or unsound source admission - **met** (no false admission; one guard added for the developer-facing extent interface). Therefore `M3_PARTIAL`: the bounded mechanisms are useful, deterministic and safe enough to retain; authentic coverage remains zero, and the exact remaining frontier is identified.

## Remaining frontier and the evidence-based successor (a proposal, not an authorization)

The unsolved authentic behaviour is the **cross-processor command protocol**: file names (and destinations in main memory) are supplied by main-processor code - the boot IP and the modules it loads - to the sub program through the Gate Array communication registers; the sub program's command dispatch is a computed transfer on the received command.
Smallest evidence-backed next step: a bounded **feasibility survey** (aggregate, no implementation first) of whether the names / commands the main processor sends are constants of main-side code that the same walk can reach (the IP is a static boot image; its Word RAM/Work RAM modules are loaded by the sub or by the main itself), and of the comm-register protocol's regularity - i.e. a two-image walk with an explicit, cited register ABI. Only if that survey finds constant command words should an implementation task follow. Offline decompression/relocation producers remain deferred until a title shows a loader whose *selection* is statically resolvable but whose *bytes* are transformed.

## Not claimed

Whole-game completeness of any title; a commercial game becoming playable; any authentic static coverage; main-processor loader analysis; offline decompression or relocation; any benefit of `loaded` over `hull`; robustness of the convergence threshold beyond the measured range.

## Independent adversarial review (fresh validator, scratch reproducers under /tmp) and fixes

Six defects were confirmed against head 671aabe and are fixed in this PR; each now has a regression where a project fixture can express it.

1. *Recursive walk dropped the taken arm of an undecided branch inside a proven module* (a load on that arm was missed with `complete: true`). Fixed (`queue_taken`); `recursive_taken` fixture + test. Side effect also fixed: a module's own internal direct branches/calls (same run of sectors) are no longer recorded as entries; only transfers from outside the module are.
2. *PC-relative reads ignored `image_dirty` and volatility* in the work-variable model. Fixed: in that model the generic path alone reads the image, and only until it may have changed.
3. *A refused or unknown hardware CDC transfer left tracked memory intact.* Fixed: it forgets all abstract state (`unresolved_hw_transfer`).
4. *MOVEP invalidated too few bytes* (spans 2n-1). Fixed.
5. *CDCTRN's header clear used an unmasked A1.* Fixed (24-bit bus mask, image-dirty on overlap); `hdr_alias` fixture + test.
6. *M2 behaviour with the new options off was not byte-identical.* The module-grouping rewrite is now used only with `work_state` (the M1/M2 grouping is kept verbatim otherwise). The DBcc counter-register correction (the legacy scan cleared D0 instead of the counter register - an unsoundness) is deliberately unconditional and is recorded here as a correction of the M1/M2 scan, not a feature.

Also taken from the unconfirmed concerns: `top_dirty` is set by any store through the stack pointer; a tail `JMP $5F22` returns to the caller's continuation instead of ending the path; proven-module code that cannot be decoded is `loaded_bytes_unknown`, not a silent path end; entries must be even and inside the image and a run is capped at the module list's size bound.
Known and accepted: runs may merge sectors loaded on exclusive paths (byte-verified at admission, wasteful at worst); `static_extent_is_proven` caches one scan per process (one disc per process); the async-volatility fixpoint has no shipped fixture (verified by the validator with a scratch interrupt entry); the BIOS-model assumptions of ADR 0216 cannot be falsified with project fixtures.

## Follow-up review (provenance and abstract-memory soundness; head after bcfd019)

Two further findings were investigated by reproduction first.
**A. `static_extent_unproven` validated containment, not range authority** - confirmed: of the five probes (exact hull, exact loaded, header subrange, boundary-crossing range, unproven base) the header subrange, a boundary-crossing range, a strict hull subrange and a range past the hull end were wrongly admitted. The guard now admits exactly the extents the producer emits under either supported policy (ADR 0219 section 3); `--static-range loaded` and program identity are unchanged.
**B. Unresolved BIOS CDCTRN left tracked memory intact** - confirmed for an unknown data-buffer pointer (a stored base survived and module A was still proven) and for an unknown header-buffer pointer (memory cleared but the boot image still trusted). A CDCTRN whose device, position or buffers cannot be bounded now forgets the memory it may write (`cdctrn_may_write`): an unknown pointer forgets everything and marks the image dirty; a known, disjoint buffer is forgotten alone, so independent knowledge is retained (`trn_disjoint`, `trn_dev`, `trn_lba` keep module A; `trn_a0`, `trn_hdr` do not). The documented buffer advance is also treated as a possible write into the boot image. The historical `M3_PARTIAL` result and the authentic aggregates are unchanged.

Independent review of these two fixes found one further hole, fixed: a CDCTRN issued after the walk had *lost* its read state (a documented drive call, an unmodelled service) was treated as the documented no-op "no sector read", so it forgot nothing. The walk now distinguishes "known idle" (after ROMREADN / ROMREAD / CDCSTOP / CDCACK) from "lost"; a CDCTRN in a lost state is treated as one that may write (`trn_lost`), and an unresolved CDCTRN also drops any loaded-range record it may overwrite. The exact-range guard had no finding (every extent the producer emitted across 35 fixture cases, both policies, with and without recursion, was admitted; every subrange was refused).
