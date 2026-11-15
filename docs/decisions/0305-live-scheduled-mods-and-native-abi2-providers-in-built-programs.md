# ADR 0305: scheduled Luau mods in the played viewer, and native ABI 2 providers in built programs

- Status: accepted (combined delivery F28: SEG-138-T010, SEG-138-T011, with the presentation consumers of SEG-140-T008/T009/T010, [ADR 0306](0306-virtual-sprite-authoring-transport-and-coverage.md) and [ADR 0307](0307-virtual-sprites-in-the-widescreen-side-columns.md)). Completes the three executable-integration gaps that [ADR 0300](0300-scheduled-mods-in-built-programs-diff-run-test-and-gameplay-example.md) deferred, except function hooks (SEG-138-T012), which stay out.
- Presentation and mod effects only: no game-specific behavior, no interpreter and no JIT. The guest is the generated-native program; a Luau mod is the pinned restricted VM; a native provider is compiled C11.

## Decision 1: one scheduler adapter for every route

`platforms/genesis/viewer/mod_capture.cpp` is the ONE adapter between the program and `mod_scheduler.c` (the scheduler of [ADR 0297](0297-genesis-mod-phase-scheduler-and-work-ram-services.md)). The same file is compiled three ways:

| Program | Defines | Calls the adapter |
| --- | --- | --- |
| headless Luau (ADR 0300) | none | the frame capture (`SEGARECOMP_CAPTURE_MOD`) |
| played (SDL3) Luau | `SEGARECOMP_MOD_VIEWER` | `viewer_main_hook.c`: attach before the first guest instruction, `end` after the run |
| native ABI 2 (headless or played) | `SEGARECOMP_MOD_NATIVE` (+ `MOD_VIEWER`) | the same two entry points; no Luau VM is linked |

There is no second scheduler, no second Luau runner and no separate native lifecycle: phases, ranges, capability checks, staging, faults and the replay witness are the scheduler's. A mod the scheduler refuses (`E_RANGE_DECLARATION`, `E_ABI_*`, ...) stops the program with exit 5 before the guest runs.

**Which Luau path a viewer build takes.** A package that stays inside the HUD-only seam (`live_support`: `host.log` and `overlay.rgba`) keeps the ABI 1 provider seam of [ADR 0296](0296-luau-only-mod-executable-integration.md) (`luau_provider.cpp`, unchanged output, every existing golden). A package that needs more (phases, bounded work RAM, input, `state.mod`) runs through the scheduler (`scheduled_support`); the headless build is always scheduled. Both present their render output through the same compatibility seam (`luau_hud_pack.hpp` packs the fixed HUD, the layers and the virtual sprites exactly once).

**Presentation and state.** The scheduler's `render` output reaches the viewer through a `GenesisNativeCompat` table ("scheduled-mod-host", HUD component) only while the scheduler reports no fault; a faulted or disabled mod presents nothing and the guest continues. The mod's state joins the viewer's save states and rewind through the existing `GenesisSnapshotModState` seam (`GenesisSnapshotMachine::mod`), so a state saved under another package identity, or by a Vanilla program, is refused with the stable diagnostic. `render` runs on the boundary after the frame was published, so a mod's overlay appears with the NEXT published frame (one frame of latency, constant and deterministic; the replay witness is unaffected).

## Decision 2: a native ABI 2 provider in a built program (`--native-package`)

`segarecomp build --native-package <p> --native-identity <id>` links the `native-c11` units of a schema 3 package (trust class native, `abi.native` 2) into the headless or the played program. The build re-verifies the package (archive, manifest, **that its identity is the one the user approved**: `NATIVE_IDENTITY_CHANGED`), the effective image (`NATIVE_IMAGE_MISMATCH`), the support set (`native_scheduled_support`: Genesis, native-only, no hooks, capabilities inside the scheduled set; `E_LIVE_*`) and the declared memory ranges for the exact image digest (`E_RANGE_DECLARATION`). Every unit is staged under its package path in the build's own folder, **re-hashed immediately before each compile**, and compiled by the host with the same host-owned flags `mod check` validates with (`kNativeCompileFlags`, `-DSEGA_MOD_TARGET_*`, SDK headers and the package tree only). A refused build leaves no executable. The constants the program needs (identity, capability bitsets, state schema, ranges of the exact revision) are generated into `native_artifact.c`.

The provider table is wrapped, not re-implemented: the wrapper copies the provider's table, replaces only `phase`, and commits what a `render` callback staged **only if the callback did not fault**. `mod test` and `mod diff-run` accept native packages through the same child builds.

**Typed ABI 2 render feature.** `SegaGenesisFeatures::render` (append-only; non-NULL exactly when `overlay.rgba` was granted) points to `SegaGenesisRenderApi`: `overlay_layer`, `overlay_target`, `overlay_rect`, `overlay_text`, `scene` (and `virtual_sprites`, ADR 0306). The rules are the Luau bindings' rules because they are one implementation (`overlay_*` in `luau_api.cpp`: validation, clamping, notes, the 64-request budget); a refusal is a scheduler fault of the callback with its stable cause (`range`, `budget`, `phase`), never a silent repair.

## Honest limits

- **Native code is host code.** The scheduler contains what it can (capability, phase, range, budget, a faulting callback); a native provider can do anything native code can do. The boundary is the approval of the exact package identity (as for ABI 1). This is not a sandbox and nothing here claims one.
- The launcher does not select native ABI 2 packages yet; the supported entry points are `segarecomp build --native-package`, `mod test` and `mod diff-run`. Function hooks and several mods (SEG-138-T012, SEG-141) are not linked.
- The Windows CI host skips the generated-native program tests like the other Luau tests; Linux and macOS build them.

## Rejected alternatives

Always running Luau through the scheduler in a viewer build (it would change the log tags, frame numbering and the pinned goldens of the HUD-only path for no benefit); a second native lifecycle in the viewer; a dedicated overlay structure for native providers (the Luau rules would be copied and drift); compiling native units with the runtime's private headers (a provider sees the SDK only).

## Verification

`mod_viewer_gameplay_test` (the gameplay example played in the SDL3 program: guest RAM equal to an independent derivation and to the headless reference, the HUD pixels, save/restore/rewind identity, refusals), `mod_native_viewer_test` (the native-versus-Luau twin: per-frame RAM, replay witness, overlay hash, played pixels; `mod test`/`diff-run`; layers; containment; build refusals), `mod_f28_integration_test`, plus the existing `mod_luau_executable_test`, `mod_gameplay_example_test`, `mod_diff_run_test`, `luau_api_test` and the F10 suites unchanged.
