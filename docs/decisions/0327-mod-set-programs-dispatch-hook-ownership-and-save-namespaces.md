# ADR 0327: Mod-set programs: callback dispatch, exclusive hook ownership, supported combinations and save namespaces

- Status: proposed (combined delivery F20: SEG-141-T003, SEG-141-T005, SEG-141-T006, SEG-141-T007).
- Builds on: ADR 0297 (scheduler), 0298/0308 (hooks), 0299/0325 (mod state, Master System), 0326 (mod sets).

## Dispatch (SEG-141-T003, SEG-141-T007)

1. A set is not a second scheduler. Each member is an ordinary scheduler instance with its own grants, declared ranges, staged effects, budgets, fault, replay witness and state. `GenesisModSet` / `SmsModSet` own only the runtime seams and the order. Member index is the set order.
2. Each phase runs for all members before the next phase. A callback commits atomically, so later members observe earlier members' committed guest writes; guest memory and the chained pad are the only channels. `input` takes one physical sample and chains it; `post-input` and the next `frame-begin` show every member the final pad. Each `render` starts from copies taken after all `frame-end` callbacks.
3. A fault disables only the faulting member. The faulting callback commits nothing; what it committed earlier remains. The pad interception stays while a live member has `input`.
4. **Hooks** (Genesis only; there is no Z80 hook implementation, so Master System hook declarations stay refused). A `replace` hook owns its function: no other member may hook it (`E_SET_HOOK_OWNER`, at composition and again at attach). `before`/`after` hooks of several members share one generated site and run in set order for entry and for exit. The build merges the sites (union of modes and readable registers; equal anchors required, `E_SET_HOOK_ANCHOR`; equal ids on different functions get a member suffix). Each binding carries what **that** member declared, so a member gets no callback and reads no register it did not declare.
5. **State.** One `MODSTATE` section bound to the composite identity holds every member's state in order (plus the shared hook call frames on Genesis); the compatibility digest covers each member's identity, schema and configuration. Restore is all-or-nothing for the guest. All members' blobs share the existing section bound; a set that does not fit is not capturable.
6. Single-mod programs keep their entry points, artifact symbols, report lines, state bytes and compatibility digest.

## Built programs (SEG-141-T006)

7. `segarecomp build --set-package <p> --set-identity <id>...` (and `segarecomp mod set build`, which runs the same preparation first so every refusal that needs no compiler is reported like `mod set check`) verifies the pinned packages once, runs the F19 composition, compiles the composed image, and links one generated member table. The program's identity is the F19 composite key (equal to `mod set variant`'s). Each native member is compiled with the host's strict flags, its own ABI minor and include tree, and its entry symbol renamed per member; there is no symbol localisation, so two members that define the same external symbol do not link (`E_SET_NATIVE_LINK`).
8. **Supported combinations are explicit; everything else fails closed.** One member at most may have presentation output or a geometry declaration (`E_SET_PRESENTATION_CONFLICT`): composing two mods' pictures is not defined. The audio API is refused in sets (`E_SET_AUDIO_UNSUPPORTED`): the sound gate and mapping identity are single-mod. Sega CD sets of more than one mod are refused by the engine's relation check (`E_SET_PLATFORM_UNSUPPORTED`); disc packages are still reserved in schema 3, so this rule is exercised at engine level only.
9. Runtime memory writes of different mods are not arbitrated (order decides). Disjoint bytes and a successful build do not prove that mods work together.

## Settings and saves (SEG-141-T005)

10. The product has no per-mod user settings (schema 3 declares none and neither API reads one), so none are invented here. The per-mod persisted values are the saved-state fragments, which are stored per member and tagged with the member's identity inside the set's section; equal keys cannot collide.
11. A set program (with or without a provider) is compiled with its composite identity. The viewer keeps battery memory and state slots under `<SEGARECOMP_SAVE_DIR>/sets/<identity>/` and records that identity as the state/session variant, ignoring `SEGARECOMP_SAVE_VARIANT`. Vanilla and single-mod save paths are unchanged. There is no save migration between sets.

## Diagnostics

12. Every composition and set-build class has a pinned exact diagnostic (`tests/data/mod_set_diagnostics.json`) except `E_SET_NATIVE_LINK` (class tested with a real link), `E_SET_PLATFORM_UNSUPPORTED` (engine test), and `E_SET_HOOK_SITE_ID` and `E_SET_ARTIFACT_WRITE` (not exercised). The launcher has its own plain-language text per class (`mod_set_class_text`), used by its set store/verify errors. It has **no mod-set screen**.

## Dispatch overhead

Measured by `genesis_mod_set_tests` (empty callbacks, five boundary callbacks per member per frame, guest instructions excluded; Linux x86-64, GCC, Debug `dev` preset): about 19-24 ns per callback, linear in the member count, about 1.5 us per frame boundary with 16 members. Render work-RAM copies and the mods' own work are not included. The test asserts no host-speed threshold.

## Evidence

`tests/genesis_mod_set_test.cpp` (order permutations, input chain, fault injection in every phase, state, hook ownership and scoping through the runtime seam), `tests/mod_set_build_test.py` (built Genesis sets, headless and SDL3: Luau HUD + native + typed edit, real shared hooks, state, fault isolation, save namespaces), `tests/mod_set_sms_test.py` (built Master System sets, headless and SDL3), `tests/mod_set_test.py` (golden diagnostics), `tests/mod_set_engine_test.cpp`, `apps/segarecomp-launcher/launcher_mod_set_test.cpp`. Synthetic fixtures only: no real-game claim.
