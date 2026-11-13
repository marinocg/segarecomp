# Luau-only HUD example

A synthetic, ROM-free example of the restricted Luau scripting API ([Luau scripting](../../../../docs/product/luau-scripting.md), ADR 0295/0296). `scripts/main.luau` requests a title panel, a frame counter and a growing bar with
`sega.overlay.rect` / `sega.overlay.text`, from the `reset()` / `render(frame)` callbacks. It is the file `segarecomp mod init --template luau` writes for Genesis. The package is **Luau-only**: installing it shows the
consent sheet, not the native-code approval, and it declares only the two capabilities the live path grants (`host.log`, `overlay.rgba`).

## The fixture image

`fixture_rom.py` writes a tiny **runnable** Genesis program written for this repository (not a game, no vendor code): it configures the VDP, loads a palette and two tiles, fills two rows of plane A, busy-waits
about 120 frames and stops. `mod.toml` names it as the revision `fixture`.

```sh
python3 fixture_rom.py fixture.rom      # prints its SHA-256 (the one in mod.toml)
```

## Build, install, play

```sh
segarecomp mod check   platforms/genesis/sdk/examples/luau-hud
segarecomp mod build   platforms/genesis/sdk/examples/luau-hud --out luau-hud.segapatch
segarecomp mod install luau-hud.segapatch --rom fixture.rom --library lib          # prints "approval_kind":"consent" and the install sheet
segarecomp build --rom fixture.rom --output out --cc <compiler> --runtime-dir <platforms>/genesis --sdl3-include <SDL3>/include --sdl3-lib <SDL3>/lib \
                 --luau-package lib/mods/<package_identity>.segapatch --luau-identity <package_identity>
out/game --instruction-budget 4000000       # a window; the HUD is drawn over the VDP picture
# (build with `--test-frame-seam 1` to be able to save the final frame with SEGARECOMP_VIEWER_FINAL_FRAME=last.bmp, as the test does)
```

`tests/mod_luau_executable_test.py` does exactly this headless (dummy video driver) against the Vanilla build of the same image and checks the HUD in the rendered frame.

`segarecomp mod session` is a separate **diagnostic**: a synthetic picture, not the Genesis machine. Its pinned hashes are in `tests/fixtures/mod_luau/hud_session.json`.
