# ADR 0214: SEG-098-T005 - inventory completeness status, authentic before/after and the M2 decision

- Status: Accepted. Final decision of milestone SEG-098 (whole-game static AOT, M2). Related: ADR 0205-0213.
- Privacy: classes and counts only for the authorized local titles; no address, hash, name, LBA or byte.

## Inventory status (machine-readable)

`segarecomp sega-cd build` writes `inventory` into `status.json` (schema `segarecomp.sega_cd.inventory.v1`; also on failure):

| status | meaning |
| --- | --- |
| `complete` | only under the explicit proof criterion: the static scan complete with no refusal, no execution-derived extent, no unanalyzed loader recursion, no barrier |
| `partial` | a useful inventory with named unresolved parts (`unresolved`: `nested_loaders_not_analyzed`, `static_refusal:<class>`, `static:<status>`, `static_discovery_disabled`, `execution_derived_extents`) |
| `blocked` | a typed barrier stopped further justified compilation (`barriers`: `stop:<class>` for a code stop, `frontier:<refusal>` for an unresolvable frontier, `build_failed:<stage>`) |

A hardware-capability stop (`*_unsupported`, e.g. an unmodelled PCM/audio feature) is reported as `device_stop: true`, not as a code barrier and not as missing code (its `final_stop.location` is
`device_capability`). Zero frontiers on a bounded no-input run prove nothing: the scan does not analyze loaders inside the modules it admits (`kNestedLoaderAnalysis = false`), so `complete`
is **currently unreachable by construction** and every successful build says `partial` - this is the honest label, covered by tests (`sega_cd_bios_unvisited_test`, `sega_cd_bios_scale_test`,
`sega_cd_native_build_test`). The review note of ADR 0212 stands: the scan's own `complete` means only "not exhausted"; the inventory status keys off refusals.

## Authentic before / after (identical authorized inputs, no gameplay script, formatted empty backup RAM)

| Title | Before M2 (T001 binary, no static producer) | After M2 (this head) |
| --- | --- | --- |
| Sonic CD (USA), Model 1 BIOS | 17 rounds, 12 extents (7 main Work RAM, 5 sub PRG RAM), stop `pcm_unsupported` | **identical**: 17 rounds, 12 extents, same program identity, stop `pcm_unsupported` (`device_stop`); static scan: 0 modules, 1 `computed_control_unresolved`; inventory `partial` |
| Earthworm Jim SE (USA) | not run before | 6 rounds, 2 extents, stop `entry_not_compiled` at an unresolvable frontier (`no_source`: the live code is not an exact copy of any delivered block); static scan exhausts its state bound (`scan_limit_exceeded`, also without the dispatch rule), admits nothing; inventory `blocked` (`stop:entry_not_compiled`, `frontier:no_source`) |

- Additional source-authorized executable coverage before gameplay from the static producer on authentic titles: **0 modules**. The Compatibility route did not regress.
- No fixed gameplay route is available locally, so no route replay was run; **Checkpoint H is not claimed and was not re-run**; its earlier result remains a separate regression reference.
- Earthworm Jim SE shows a second authentic limit: the scan's state-keyed visited set explodes on its boot image (62,314 steps / 4,096 states with module-table roots alone). The bound turns this into a typed
  incomplete result (nothing admitted - never a silent truncation), not a wrong inventory.

## Decision: `M2_PARTIAL`

Pre-registered conditions for `M2_GO`: (1) the BIOS-image builder compiles and later executes an independently justified unvisited module - **met** (T002/T003/T004 fixtures through the supported build and launcher
path, no manifest, no execution-derived admission of the module, runtime guards unchanged); (2) wider discovery demonstrates useful additional coverage - **met on project-authored discs** (module jump table,
constant-argument table dispatch, computed entries with `--static-range loaded`, 48-module overlay inventories), **not met on either authentic title** (0 modules; limits above); (3) safety and determinism hold -
**met** (typed refusals, identity includes every static admission, repeat builds identical, no regression of the authentic baseline).

Therefore: `M2_PARTIAL` - the central integration works; the loader classes that real discs use are unresolved.

## Smallest successors (carried to SEG-099's refinement; none is created here because each changes architecture)

1. **Memory-carried loader arguments in the sub program's service layer** (Sonic CD): bounded abstract modelling of the SP work-variable area plus the public BIOS CD/file service ABI as cited evidence, with ISO9660
   name-to-extent resolution (an immutable disc structure). ADR 0212 found the load parameters live in that memory; this is the highest-value unresolved class.
2. **Scan state joining/widening** so a boot image like Earthworm Jim SE's completes within the bounds instead of exhausting them.
3. **Recursion**: analysis of loaders inside admitted modules (the missing part of the `complete` criterion).
4. **Policy decision**: whether Compatibility should default to `--static-range loaded` (ADR 0213) - sound either way, costs at most one sector per module.
5. **Not an M2 item**: decompression/relocation (M3 roadmap), audio/PCM hardware (the `pcm_unsupported` stop is a device capability).

## Not claimed

Whole-game completeness of any title; a commercial game becoming playable; Checkpoint H restoration; any benefit of the static producer on an authentic title today.
