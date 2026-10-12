# Genesis SDL3 viewer (SEG-007-T254)

Optional interactive frontend over the exact generated-native program produced by the canonical
bridge route. Guest execution, T253 virtual timing, and T255 live frame production are unchanged;
the viewer only slices execution, paces presentation, and shows completed `GenesisFrameArtifact`s.

Prerequisite: SDL3 (`brew install sdl3`, `vcpkg install sdl3`, or distro dev packages). `--segarecomp` and `--cc`
are optional (defaults: the dev-preset build; `$CC`, else `cc`/`clang`/`gcc` on PATH). The tool looks in `$SDL3_PREFIX`, `/opt/homebrew`,
`/usr/local`, `/usr`; if SDL3 is not found `--viewer` fails before any generation or execution
(it never falls back to headless).

```sh
python3 tools/genesis_startup_bridge.py \
  --segarecomp build/dev/apps/segarecomp/segarecomp --cc /usr/bin/cc \
  --rom "games/Sonic The Hedgehog (USA, Europe).md" \
  --expect-sha256 46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6 \
  --mode commercial --one-shot --diagnose-frontier \
  --external-hints ".tools/analysis-hints/<sha256>.json" --immutable-rom-aot \
  --out-dir .cache/my-viewer-build \
  --viewer
```

Build profile (SEG-018-T005): `--build-profile {auto,debug,optimized,quick}`. `auto` (default) uses the
optimized host profile (`-O2`) for `--viewer` (long-running interactive execution) and `quick` (`-O0`, no
`-g`) otherwise; `--diagnose-frontier` no longer forces `-O0 -g`. Use `debug` (`-O0 -g`) only for low-level
debugging, e.g. before `tools/genesis_frontier_debug.py --binary <debug-binary>`. All profiles compile the
identical generated C (byte-identical); guest semantics and virtual timing are unchanged. Measured on the
canonical Sonic route (270 MB generated C, this host): total wall debug 455 s vs `-O2` 742 s; C compile
alone `-O0` 198 s, `-O1` 413 s, so `-O2` cuts guest execution (~250 s to ~50 s) but loses on cold one-shot
runs, hence the auto split.

Options: `--viewer-unthrottled` (no presentation sleep; same guest results), `--viewer-slice <N>`
(guest dispatches per host slice, default 500), `--instruction-budget <N>` (total runner allowance; a generated program started with no `--instruction-budget` at all runs unbounded until the guest stops or the window closes;
defaults to the canonical 16777216, host policy only). Close the window or press Escape to stop; that is
normal host termination reported as `window_closed`, distinct from guest stop/complete and
`runner_resource_limit`. A `VIEWER_SUMMARY` line with normalized facts is printed to stderr.

Mechanism: viewer builds compile the unmodified generated C with
`-Dgenesis_runtime_run=genesis_viewer_hook_run`, so the generated `main` hands its own runtime,
dispatcher and allowance to `platforms/genesis/viewer/viewer_main_hook.c`. Headless builds do not define the
macro and link no viewer or SDL code.

## Keyboard input (SEG-011-T004, SEG-055)

Fixed maps. Player 1: arrow keys = D-pad, Z = A, X = B, C = C, Return = Start. Player 2: I/K/J/L = Up/Down/Left/Right,
U = A, O = B, P = C, Right Shift = Start. Escape and window close quit.
Each pad mask is polled once per host slice and injected with `genesis_runtime_set_pad1` / `genesis_runtime_set_pad2`; both ports are
independent three-button pads on DATA1/DATA2 and are host input, not part of device checkpoints or divergence digests.

## Console region (SEG-055)

`--region {usa,japan,europe,auto}` (default `usa`, the historical console) selects the console the guest sees through the hardware
version register ($A10001): `usa` = overseas NTSC `0xA0`, `japan` = domestic NTSC `0x20`, `europe` = overseas PAL `0xE0`. The generated program is the
same for every region, so switching needs no rebuild; the choice is exported to the program as `SEGARECOMP_GENESIS_REGION`, fixed before the
first instruction and stable for the whole run (set that variable directly for a built `segarecomp build ... --play` product). `auto` derives a
*hint* from the ROM header's region-support field (`segarecomp::genesis_region_hint`, the one shared policy: overseas NTSC if declared, else Japan,
else Europe; a lone `E` is ambiguous and keeps `usa`); an explicit choice always wins, and header metadata is never treated as proof of the console.
`europe` is accepted but is an **approximation**: the guest reads the PAL version register while the machine still runs the 60 Hz schedule
(50 Hz timing is not implemented); the bridge prints a notice and `RUNTIME_NOTES` carries `runtime_hz` 60 / `hardware_hz` 50. Cartridges with a region lockout
(for example a Japan-only image) show their own lockout screen under a console they reject -- a normal guest outcome, not an emulator error;
select `--region japan` for such an image. The bridge prints a `REGION_PROFILE {...}` line to stderr when `--region` is given, and the capture/viewer builds print one `RUNTIME_NOTES {...}` line
(region, undelivered H-interrupts, rejected frames, VRAM-journal overflows, runtime_hz/hardware_hz) only when something notable happened.

H-interrupts (VDP register #0 bit 4) are now delivered when the cartridge's handler could be compiled. A handler that cannot be compiled -- for example one the game builds in work RAM at run time -- is
counted as `hint_undelivered` and the program continues (the behavior of every earlier build, now visible); `SEGARECOMP_GENESIS_HINT=strict` stops fail-closed with `hint_handler_not_installed` instead.

## Display modes, pacing and presentation (SEG-055)

The window shows whatever geometry the frame carries -- H40 (320x224), H32 (256x224) or interlace mode 2 (320x448 / 256x448) -- stretched over the
fixed 2x client area, using the same RGB conversion as the headless PPM export (`genesis_vdp_frame_row_rgb888`), including per-row palettes for
mid-frame palette switches. Presentation is paced by the guest's deterministic *virtual frame boundaries*, not by successfully published frames: a frame
the renderer rejects (an unsupported display mode, or a mode switch inside one frame) costs its period and leaves the previous image on screen, so the
guest can never run ahead of real time because of a render failure. `--viewer-unthrottled` still only skips the sleeping. Guest results never depend on host
timing or slicing.

## Headless bounded frame capture (SEG-021-T031)

`--capture-frames FIRST:COUNT[:STRIDE]` builds the same unmodified generated program with a headless
capture hook (`platforms/genesis/viewer/frame_capture*.c`, no SDL) instead of the viewer, runs it once, and
writes the frames the runtime publishes at ordinals `FIRST, FIRST+STRIDE, ...` (`COUNT <= 64`) to
`<out-dir>/frames/frame-<ordinal>.ppm` via the runtime's own PPM exporter, plus a PNG copy for inspection.
Selection is by frame publication ordinal only (no PC, address, or game-state heuristic). The run stops
as soon as the window is complete; if the dispatch allowance ends or the guest stops first, the bridge
exits non-zero and reports the shortfall. A `CAPTURE_SUMMARY` line on stderr lists the ordinals, counts,
and per-frame digests. Captured frames from commercial images are local diagnostics only: keep them under
an ignored output directory and never commit them.

```sh
python3 tools/genesis_startup_bridge.py --rom games/<rom> --mode commercial --one-shot \
  --diagnose-frontier --immutable-rom-aot --capture-frames 40:16:80 --out-dir .cache/capture
```
