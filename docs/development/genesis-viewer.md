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
(guest dispatches per host slice, default 20000), `--instruction-budget <N>` (total runner allowance;
defaults to the canonical 16777216, host policy only). Close the window or press Escape to stop; that is
normal host termination reported as `window_closed`, distinct from guest stop/complete and
`runner_resource_limit`. A `VIEWER_SUMMARY` line with normalized facts is printed to stderr.

Mechanism: viewer builds compile the unmodified generated C with
`-Dgenesis_runtime_run=genesis_viewer_hook_run`, so the generated `main` hands its own runtime,
dispatcher and allowance to `platforms/genesis/viewer/viewer_main_hook.c`. Headless builds do not define the
macro and link no viewer or SDL code.

## Player-1 keyboard input (SEG-011-T004)

Fixed map: arrow keys = D-pad, Z = A, X = B, C = C, Return = Start. Escape and window close quit.
The pad mask is polled once per host slice and injected with `genesis_runtime_set_pad1`; it is host
input and is not part of device checkpoints or divergence digests. Port 2 always reads released.

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
