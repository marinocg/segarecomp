# Asset replacement example (Master System, synthetic)

A redistributable, ROM-free example of replacing **tiles, a palette, a room table and text** with typed ROM edits (SEG-147-T007, ADR 0324). There is no universal asset decoder: the mod owns a small
adapter for the one data format it understands. It is built and run in CI by `tests/mod_sms_assets_test.py` without any commercial input.

* `fixture_rom.py` writes the deterministic **demo cartridge** (a tiny program that shows its palette, tiles, an 8x4 room table and one line of text; not a game). `mod.toml` pins its SHA-256.
* `rom_format.json` describes the cartridge's invented data format: where each region is, its size, and the SHA-256 of its **original** bytes.
* `assets/*.txt` are the mod's own art and text, declared in `mod.toml` as `asset` sources of class `data` (they travel in the package as digest-listed payloads).
* `encode_assets.py` is the **mod-owned adapter**: it checks that the image is the declared revision and that every region it replaces still holds the pinned original bytes, checks each asset's size and
  value ranges, and writes `edits/main.edits`. Each edit carries the original bytes as its expected value, so the build asserts them again. Any failure writes nothing.
* `edits/main.edits` is the adapter's committed output (`--check` fails when it is stale).

```sh
python3 fixture_rom.py demo.sms
python3 encode_assets.py demo.sms                 # rewrite edits/main.edits after changing an asset
segarecomp mod check .
segarecomp mod pack . --out asset-demo.segapatch --rom demo.sms
segarecomp build --rom demo.sms --output out --cc cc --runtime-dir <platforms-dir>/master-system --data-package asset-demo.segapatch --data-identity <package_identity printed by pack>
out/game --frames 20 --artifacts <existing-dir>   # frame.json holds the SHA-256 of the last frame
```

For a real game, write the adapter only for data formats you have understood yourself; compressed or unknown data is not guessed. An edit file's expected values are bytes of the original game, so an edit
file made from a commercial image must not be committed or shared.
