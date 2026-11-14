# Gameplay example, native twin (Genesis, synthetic)

The **native ABI 2** counterpart of [`../gameplay`](../gameplay) (ADR 0305): the same fixture image, the same rule (while button C is held the provider adds 10 to the fixture's score word, through the typed,
declared work-RAM range), the same HUD panel and the same mod state (the number of bonuses granted), written as strict C11 against `segarecomp_mod_genesis.h`. It exists to prove, in CI and without any
commercial input, that a native provider and a Luau mod run through the SAME scheduler and produce identical per-frame RAM, replay witness, HUD pixels and end state
(`tests/mod_native_viewer_test.py`).

* `mod.toml` declares the platform, the revision (the fixture), `[abi] native = 2`, the capabilities, the mod state schema and the ONE guest-memory range the provider may touch.
* `src/provider.c` is the provider. Its HUD uses the **typed ABI 2 render feature** (`SegaGenesisFeatures::render`, present exactly when `overlay.rgba` was granted): `overlay_rect`, `overlay_text`,
  `overlay_layer`, `overlay_target` and `scene`, the counterparts of the Luau `sega.overlay.*` calls with the same rules.
* `tests.toml` declares the `segarecomp mod test` scenarios (the per-frame RAM expectations are the Luau example's).

**Native code is host code.** The package is compiled by the host with its own strict flags and linked into the program; the scheduler contains what it can (capability, phase, range, budget, a faulting
callback), but a provider can do anything native code can do. The boundary is the approval of the exact package identity; this is not a sandbox. `segarecomp build --native-package <p> --native-identity <id>`
refuses a package that is not the approved identity (`NATIVE_IDENTITY_CHANGED`).

```sh
python3 ../gameplay/fixture_rom.py fixture.rom
segarecomp mod test . --rom fixture.rom --cc cc --sdk-dir <platforms-dir>
segarecomp mod pack . --out gameplay-example-native.segapatch
```
