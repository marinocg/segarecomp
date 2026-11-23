# Widescreen + gameplay example, native (Genesis, synthetic)

ONE schema 3 / native ABI 2 package (SEG-149-T001, ADR 0309) that shows, without any commercial input, every capability a real game mod of this kind needs:

* **authored scenery** beside the picture (`scenery.words`, typed `scenery_window` (and, for lines where the original provider answered with the VDP's own plane, `scenery_generic_rows`, ABI minor 2): a per-frame name-table window of cells the original program never loaded);
* an **actor** that crosses the right edge of the picture as a mod-authored **virtual sprite** (presentation only);
* a small **HUD** panel;
* a **gameplay change**: pressing C restores the fixture's power value to 5 (its maximum), only while it holds a verified in-game value (1..4), through one declared read-write work-RAM word;
* **mod state** (the restore counter) in save states, rewind and replay.

The revision is the deterministic fixture of `fixture_rom.py` (a tiny original Genesis program, not a game). Presentation-only behaviour (scenery, sprite, HUD) is separate in the source from the gameplay-changing
write. Native code is host code: the package is approved by identity, it is not a sandbox.

```sh
python3 fixture_rom.py fixture.rom
segarecomp mod check . && segarecomp mod pack . --out widescreen-gameplay-example.segapatch
```

`tests/mod_widescreen_example_test.py` runs it end to end: pack, launcher install and approval, build, SDL3 play at 4:3, 16:9, 21:9 and 32:9 against independently constructed expectations, save/restore/rewind, and
Vanilla unchanged.
