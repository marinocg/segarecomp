# Shared in-game viewer: implemented behavior and review

Genesis and Master System use the same strict-C11 SDL3 host UI, not the same
console execution loop. Gameplay has no permanent menu chrome. Esc, Back/Select
(Nintendo minus) or Guide/Home opens host pause; execution is blocked until the
overlay closes. **Start/Plus is a guest input**, not a menu shortcut. SMS Pause
keeps its short-tap latch and NMI semantics. Focus loss and unplug release inputs
at the adapter's next sampling point. Esc always remains reachable.

## Pages and limitations

* **Pause / Quick Settings:** responsive stepped blue frames, bitmap body text and
  Silkscreen headings, repository wordmark/icons, keyboard/gamepad focus cues.
  Fullscreen is borderless, not a disruptive display-mode switch. Scaling,
  corrected 4:3 versus square pixels, CRT preset, VSync and HUD apply immediately.
* **Video:** current-frame centre-crop preview uses the same effect pipeline as
  gameplay. Four distinct presets plus Custom; scanlines, RGB mask, brightness,
  mesh curvature and four additive bloom taps are real controls. **SDL_Renderer
  only**, including the software fallback: no SDL_GPU shaders or GPU compatibility
  claim. Curvature is a mesh approximation; bloom is not HDR/thresholded
  multipass bloom. Pause uses dimming, not blur. Pixel Perfect stays nearest and
  byte-exact at integer scale. Screenshots save the source game pixels, not the
  effect/menu image, to Pictures/Segarecomp or the configured screenshot directory.
* **Controls:** keyboard/gamepad binding columns, fresh-press listener, timeout,
  clear/cancel, duplicate-owner warning, restore confirmation, stick dead zone
  and connected-device selection. Tests exercise real SDL virtual setter events,
  not hand-invented gamepad events. The current Genesis adapter supports two
  three-button ports (the original kit's older port-2 limitation is superseded by
  the existing runtime seam); no six-button X/Y/Z/Mode claims are made.
* **Audio:** playback volume/mute and restore defaults. Output follows the OS
  default; no output-device selector or latency/buffer control is offered. A
  missing audio stream is shown as silent; guest PCM generation is unchanged.
* **System:** actual renderer/output/session/frame-rate/audio diagnostics,
  source-pixel screenshot, controller status and HUD toggle. Reset is available
  only for SMS's existing reset seam, with confirmation; Genesis Reset is disabled
  with explanation. Return to Launcher/Quit confirms and starts on **No**.
* **Controller status:** distinguishes no joystick, raw/unmapped joystick,
  gamepad open failure with SDL error, opened-but-silent and actual received input.
  The OS HID scan is an explicit background job, not a render-loop scan.
  libusb availability is a startup snapshot; the scan is also a snapshot, not
  physical-gameplay validation. Shutdown waits for outstanding scans.

No synchronous asset reads, mapping-file existence checks, HID enumeration or
libusb load probes run continuously during viewer/diagnostic drawing. Explicit
user actions such as saving a screenshot or reloading a mapping file still do
their requested I/O. Normal UI has no SDL debug-font calls. Bitmap atlas fonts
are embedded C data; packages ship ProggyClean's full MIT and Silkscreen's OFL
notices. Only viewer `src/` and `include/` ship, not gallery/test sources.

**Visual fidelity pass (SEG-058, PR #105).** One measured design system replaces per-page sizes (`libs/viewer/src/sv_draw.c`):

* **Page classes.** *Hero* (pause menu, confirmations): few large rows whose labels use a separate, larger whole-number *label scale*
  (2x the body face at 1x UI, 3x at 2x, then in whole steps), 32 px icons, a bold primary row; *Standard* (Quick Settings, Audio): 34-unit rows;
  *Dense* (Video, Controls, System, Controller status): 28-unit rows in wider panels. The header (icon, wordmark, page title), the footer (the focused item's
  explanation and the Enter/Esc keycaps or gamepad face-button cues) and the chrome (stepped frames, outlines, focus) are identical on every page, so moving
  between pages never changes the UI scale. Hero labels fall back to the body scale when the window is too short (they keep the large labels at 720x520).
* **Widths.** Pause ~500 units, Quick Settings 460 (anchored left so the picture stays visible), Audio 600, Controls 700, System 780, Video 840, each with a
  minimum below which the UI scale steps down; a page never relies on a fractional scale.
* **Pause.** *Resume Game* is bold and keeps a blue frame even without focus; *Return to Launcher / Quit* sits after a divider with a red-tinted icon.
* **Confirmations.** An amber band, warning icon and bold question at the label scale, the consequence in body text, then the answers as full-width rows:
  the destructive answer is drawn in danger red, the safe answer stays the default focus; the game behind is dimmed more than for settings pages.
* **Video & Display.** Two equal columns: window/picture settings, then a *CRT effect* section (the preset choice and the five strengths, still all
  keyboard/gamepad reachable), and on the right the live preview (the real last frame with the current settings, sized from the column), four separate preset
  buttons (the selected one filled, with a check mark) and the preset's description. The SDL_Renderer CRT passes are not described as GPU shaders.
* **Controls.** Player 1 / Player 2 as two large tabs, the device line and the controller status button, then a table with *Keyboard* and *Gamepad*
  column headers and a glyph per console input (arrows, button letters, a pill for Start/Pause). Only the console's real inputs appear (Genesis three-button;
  Master System Button 1/2 and the console Pause); capture and conflict states are amber.
* **Audio.** The volume row is the large row (thicker track, larger knob, bold value); toggles read On/Off next to the switch; the device line stays factual.
* **Text.** The body face stays ProggyClean (MIT) and headings Silkscreen (OFL): candidates were rendered and rejected (GNU Unifont as packaged here is GPL-2+
  only; Pixelify Sans draws 5 like S, unreadable in values). Fewer all-caps strings ("Live preview", "On/Off").

**Start-up hint.** The first picture of a session shows a toast for six seconds saying how to open the pause menu ("Esc or the gamepad's Back
button", plus the custom menu key when one is set). It uses the normal toast (top right, small) and never replaces a toast that is already showing.

**Review picture.** The gallery's picture (`libs/viewer/test/sv_gallery_scene.c`, generated by `tools/gen_gallery_scene.py`) is an original, fictional
pixel-art landscape at 320x224, 256x192 and 256x224 (no game, ROM, screenshot or third-party sprite). It is compiled into `sv_gallery` only, never into the
viewer library, a game or a package, and never drawn over a real picture; it is a review fixture, not emulator progress.

Earlier, launcher visuals were unchanged by the viewer phase. Its only source changes
here are asynchronous HID diagnostics and cached/no-passive-I/O diagnostic facts.
Build cancellation remains **unsupported**. Covers are local images, an explicit
Libretro download (only on request, see `launcher-library.md`) or the browser lookup.

## New-binary synthetic captures

`screenshots/viewer-capture.json` records the gallery binary SHA-256, individual
published image hashes and a reproduction command. The full ignored manifest
also records every exact command, environment and 30-second per-process timeout.
No generated guest or ROM is executed by the gallery; its UI script is finite.

The full review is **24 bounded gallery runs / 448 screenshots**:

* Genesis, SMS 192-line and SMS 224-line modes;
* 720x520 and 1080x740 output pixels (1x text), 1440x1040 and 2160x1480
  (doubled logical-size simulations), and 2560x1440 (3x text where the page fits);
* no controller, raw virtual joystick, injected open failure and active virtual
  gamepad at 1080x740 for each console mode;
* gameplay, pause, quick settings, video/preview, controls, capture/conflict/
  restore, controller status, audio, system, SMS reset confirmation, quit,
  toast/HUD and four CRT outputs.

**124 synthetic images** are published: all pages at the two review sizes and
the four diagnostic cases for each mode. Larger captures and **120 individual
concept/final comparison sheets** remain under ignored `build/visual-review/`.
Concept artwork is never committed, installed or drawn by production code.
The virtual controller fixture does not prove that any physical device works.

```sh
cmake --build build/dev --target sv_gallery sv_host_test sv_input_diag_test
python3 tools/viewer_visual_capture.py \
  --gallery build/dev/libs/viewer/sv_gallery \
  --assets apps/segarecomp-launcher/assets --out build/visual-review/viewer \
  --publish docs/product/screenshots \
  --references <local-kit>/references/viewer_screens \
  --comparisons build/visual-review/viewer-comparisons
```

Pillow is required by the capture tool, not the product. Use the `.exe` gallery
path on Windows. This review used Linux's SDL dummy driver and software renderer.
Doubling output dimensions exercises layout/text scaling, **not actual OS HiDPI,
display timing, physical-controller or three-OS release validation**.

## Focused validation of the earlier reconciliation

Nine distinct CTest cases passed: `sv_host_test`, `sv_input_diag_test`,
`sv_viewer_core_test`, `viewer_foundation_layout_test`, `ci_shard_guard_test`,
`launcher_input_diagnostics_test`, `genesis_viewer_tests`, `sms_viewer_tests` and
`genesis_viewer_generated_integration_test`. The shard guard's existing
cross-directory fix was preserved. A first launcher invocation could not find
the local SDL shared library; rerunning with the SDL runtime environment passed.
The generated integration test was rerun with `SDL3_PREFIX` so it compiled and
executed instead of skipping. Generated execution uses instruction budget 8;
the enclosing test command has a finite wall timeout.

The installed 11 viewer C sources also compile and link with pinned Zig 0.15.2
using `-std=c11 -Wall -Wextra -Werror -pedantic -shared -fPIC`, including the
font and threaded diagnostic code. This is a staged-source strict-C11 check,
not a complete extracted-release smoke test on three OSes. A preliminary Zig
`-fsyntax-only` invocation rejected Zig's own injected `-c` as an unused argument;
the real compile/link check passed without suppressing warnings.

Full/fast final gates, SMS generated replay equivalence, packaged consumer smoke,
real-display pacing and hardware input were not rerun in this phase. Console
execution, adapters, state/PCM production and pacing code are untouched. The
validator owns the final local fast gate and targeted falsification.
