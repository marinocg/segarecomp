# Master System gameplay example: hold button 2 to run

A redistributable example mod (SEG-147-T011, ADR 0325). Everything here is project-authored: `fixture_rom.py` builds a tiny synthetic cartridge (it is not a game).

| | |
| --- | --- |
| Gameplay | while button 2 is held the mod holds Right (input interception) and writes speed 3 into the game's speed byte, through the one declared work-RAM range |
| Presentation only | a HUD, a tinted sky (palette mapping), a one-pixel picture offset and an authored marker; the emulated VDP and every guest byte are untouched |
| State | `save()` / `load()` carry the run counter through save states and rewind |

From this directory (`$CC` is a C compiler, `$SDK` the product's `platforms` directory):

```sh
python3 fixture_rom.py /tmp/runner.sms
segarecomp mod check .
segarecomp mod build . --out /tmp/runner.segapatch
segarecomp mod test  . --rom /tmp/runner.sms --cc "$CC" --sdk-dir "$SDK"
segarecomp mod pack  . --out /tmp/runner.segapatch
segarecomp mod install /tmp/runner.segapatch --rom /tmp/runner.sms --library /tmp/lib
```

To build the program itself (the identity is the `package_identity` that `mod pack` printed), and run it headless, always with a bound:

```sh
segarecomp build --rom /tmp/runner.sms --output /tmp/out --cc "$CC" --runtime-dir "$SDK/master-system" --platform master-system \
  --luau-package /tmp/runner.segapatch --luau-identity <package_identity>
/tmp/out/game --frames 60 --cycle-budget 4000000
```

Add `--sdl3-include` and `--sdl3-lib` to build the played program. The launcher does not install Master System mods yet.
The native C11 form of the same mod is [`../gameplay-native`](../gameplay-native). See [Master System mods](../../../../../docs/product/master-system-mods.md).
