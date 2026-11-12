# Luau-only HUD example

A synthetic, ROM-free example of the restricted Luau scripting API (ADR 0295, [Luau scripting](../../../../../docs/product/luau-scripting.md)).
`scripts/main.luau` reads the values the host publishes for the frame (`sega.frame.value`) and requests a score/lives panel (`sega.overlay.rect`, `sega.overlay.text`). It is the same file `segarecomp mod init --template luau` writes for Genesis.

The package is **Luau-only**: no native code, so installing it shows the consent sheet (not the native-code approval).

## The fixture image

`mod.toml` names one revision, `fixture`: a 16 KiB deterministic image that is **not a game** and contains no commercial data. Generate it with:

```python
rom = bytearray(((i * 131 + 17) ^ (i >> 8)) & 0xFF for i in range(0x4000))
rom[0x100:0x110] = b"SEGA MEGA DRIVE "
open("fixture.rom", "wb").write(rom)      # SHA-256 ea70e3d7f81f91e58c184c4194fe605ed67d87ab1bab90900a9b3ee8a9c3dcb5
```

## Build, install, run

```sh
segarecomp mod check   platforms/genesis/sdk/examples/luau-hud
segarecomp mod build   platforms/genesis/sdk/examples/luau-hud --out luau-hud.segapatch
segarecomp mod install luau-hud.segapatch --rom fixture.rom --library lib          # prints "approval_kind":"consent" and the install sheet
segarecomp mod session lib/mods/<package_identity>.segapatch --rom fixture.rom --frames 60 --bmp last.bmp
```

`mod session` runs the deterministic fixture session: a synthetic 320x224 picture seeded by the fixture image, the published values `score`, `lives` and `timer`, your script's overlay composited the way the viewer's fixed overlay seam does, and a SHA-256 per frame. The expected hashes are pinned in `tests/fixtures/mod_luau/hud_session.json` and checked by CI (`mod_luau_api_test`) on every platform. It does **not** run the Genesis machine; wiring scripts into the live viewer is a later milestone.
