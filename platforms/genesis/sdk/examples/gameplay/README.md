# Gameplay example (Genesis, synthetic)

A redistributable, ROM-free example of a **gameplay** mod and its **HUD**, written with the official Luau API (ADR 0295, 0297, 0300). It is built, tested, packed and installed in CI by
`tests/mod_gameplay_example_test.py` without any commercial input. See [Gameplay mods](../../../../../docs/product/gameplay-example.md).

* `fixture_rom.py` writes the deterministic **fixture image** (a tiny runnable Genesis program, not a game). SHA-256 `fc7126ee2497cfbc7e1e3337c230a967d21c4c98e44493a4dfd01ae3ff6a5a34`.
* `mod.toml` declares the revision, the capabilities, the mod state schema and the ONE guest-memory range the mod may touch (the fixture's score word, work RAM `0xFF0010`, 2 bytes).
* `scripts/main.luau` is the mod: while button C is held, every frame adds 10 to the score (typed work-RAM access) and a HUD panel shows how many bonuses were granted.
* `tests.toml` declares the `segarecomp mod test` scenarios (bounded; hand-derived per-frame RAM expectations).

```sh
python3 fixture_rom.py fixture.rom
segarecomp mod check .
segarecomp mod test  . --rom fixture.rom --cc cc --sdk-dir <platforms-dir>
segarecomp mod diff-run . --rom fixture.rom --cc cc --sdk-dir <platforms-dir> --frames 30 --budget 4000000 --timeout 120 --input "10:1:0x20,20:1:0x00" --ram 16:4
segarecomp mod pack .  --out gameplay-example.segapatch
segarecomp mod install gameplay-example.segapatch --rom fixture.rom --library lib
```
