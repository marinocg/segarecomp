# Master System mods (developer preview)

A Master System mod can change gameplay through Luau or a native C11 provider, change what is **presented**, keep its own state across save states and rewind, and change sound through the game's own PSG
driver. Design and limits: [ADR 0325](../decisions/0325-master-system-mod-lifecycle-state-presentation-and-psg-sound-mods.md), [ADR 0324](../decisions/0324-master-system-target-edits-z80-injection-and-asset-adapters.md).
Support levels: [support matrix](mod-support-matrix.md). Everything below runs on **synthetic, project-authored cartridges**; nothing here is evidence about any commercial game.

Examples (`platforms/master-system/sdk/examples/`):

* [`gameplay`](../../platforms/master-system/sdk/examples/gameplay/README.md): hold button 2 to run (Luau), with HUD, tint and an authored marker;
* [`gameplay-native`](../../platforms/master-system/sdk/examples/gameplay-native/README.md): the identical mod in C11;
* [`sound`](../../platforms/master-system/sdk/examples/sound/README.md): PSG data edits and a driver patch;
* [`asset-replace`](../../platforms/master-system/sdk/examples/asset-replace/README.md): art, palette and text.

## Workflow

```sh
segarecomp mod init <dir> --platform master-system --id <id> --template luau --rom-sha256 <sha256 of your image>
segarecomp mod check <dir>
segarecomp mod build <dir> --out mod.segapatch
segarecomp mod test  <dir> --rom <original> --cc <compiler> --sdk-dir <platforms-dir>
segarecomp mod pack  <dir> --out mod.segapatch
segarecomp mod install mod.segapatch --rom <original> --library <dir>
segarecomp build --rom <original> --output <dir> --cc <compiler> --runtime-dir <platforms-dir>/master-system --platform master-system \
  --luau-package mod.segapatch --luau-identity <package_identity>          # or --native-package / --native-identity
```

Add `--sdl3-include` and `--sdl3-lib` for the played program. A headless program must always be run with a bound: `--frames` and/or `--cycle-budget`. The cycle budget covers the whole run, a mod checkpoint included; if it ends first, or a loaded state is already past it, the run exits 2. **The launcher does not install Master System mods yet.**

## What a mod can do

| Phase | When | Read | Change |
| --- | --- | --- | --- |
| `reset` | before frame 0, and again after a machine reset | declared ranges | mod state |
| `frame_begin` | frame start, before the input sample | declared ranges, previous final input | declared work RAM |
| `input` | after the sample | input | the pad bits the game will read |
| `post_input` | before the game runs the frame | declared ranges, final input | declared work RAM |
| `frame_end` | after the frame | declared ranges | declared work RAM |
| `render` | after the frame, outside guest time | copies only (`frame.work_ram`, `frame.sms_vdp`) | presentation only |

* **Memory** is declared per revision, and a capability alone authorizes nothing:

  ```toml
  [[memory]]
  revision = "rev-a"
  space = "z80.work_ram"      # 8 KiB, offset 0 = $C000; or "z80.cart_ram" (read only, sega mapper only)
  offset = 22
  length = 1
  access = "write"            # read | write | read-write
  ```

  Widths are 1 or 2 bytes, little-endian. Needs `memory.read.<space>` / `memory.write.<space>`.
* **Input** (`input.read`, `input.intercept`): ports 1 and 2; up `0x01`, down `0x02`, left `0x04`, right `0x08`, button 1 `0x10`, button 2 `0x20`. The Pause button cannot be read or changed. Recordings
  keep the physical input.
* **Presentation** never writes the VDP or the game: `sega.overlay.rect` / `text` (picture pixels, 256 wide, at most 224 high; `overlay.rgba`), `sega.present.pattern` / `colors` / `sprite` (your own
  authored art; `overlay.rgba`), `sega.present.color(cram_value, 0xRRGGBB)` and `sega.present.scroll(dx, dy)` (`present.transform`). `sega.frame.sms_vdp(region, index)` reads a copy of VRAM, CRAM, the
  registers or the sprite table (`frame.sms_vdp`). To change the game's real palette or scrolling, change guest data (typed edits) or its variables (declared RAM writes).
* **State** (`state.mod`, `[state] schema = 1`): `save()` / `load(value)` (C11: `save_state` / `load_state`). A mod that changes guest behavior without it disables save states. Initialize your state in
  `reset`: a machine reset runs it again without recreating the script's globals, and a recording made after a reset replays only for a mod that initializes everything there and had not faulted before.
* **After a fault**, a Luau mod that uses `state.mod` cannot be revived: loading or rewinding to an earlier state is refused. A C11 mod disabled by a rule violation accepts the earlier state and runs again. A Luau `load()` that
  fails also disables the mod.
* **Faults** (an undeclared range, a write outside the legal phases, a bad request, an exhausted budget) discard that callback's effects, disable only the mod and print one `E_MOD_FAULT cause=... phase=...
  frame=...` line. Luau and C11 report the same cause.

Native C11 providers include `segarecomp_mod_sms.h` only (strict C11, `<stddef.h>` and `<stdint.h>`); they are trusted code, **not** sandboxed.

## Sound

Change verified sound data and command tables with typed edits; change the driver with a `guest-asm` routine in declared free space (statically admitted; no port I/O or indirect jumps). Compare the headless
`audio.sha256` PCM digest, or use `expect_pcm` in `tests.toml`. There is no universal Master System song format or driver, no audio-command API on this platform yet, and **the YM2413 FM unit is
unsupported**.

## Not supported

Function hooks, VDP writes, raw ports, Game Gear, multiple mods, audio-command mappings, launcher installation, `mod diff-run`.
