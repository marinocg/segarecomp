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

**Final design pass (SEG-058).** The pause menu and the confirmation sheets use taller 30-unit rows (the launcher sidebar's rhythm), the pause
menu is slightly wider and sets *Return to Launcher / Quit* apart with a divider. Dense settings pages keep their 26-unit rows so they still fit without
scrolling; the integer UI-scale rule and the bitmap fonts are unchanged (1x up to 720p-class windows, 2x at 1080p, 3x at 1440p, 4x at 4K), because a 2x
pause menu next to 1x settings pages at 1080x740 would make the pages jump in size. Captures were refreshed with the new gallery binary.

Earlier, launcher visuals were unchanged by the viewer phase. Its only source changes
here are asynchronous HID diagnostics and cached/no-passive-I/O diagnostic facts.
Cover HTTPS downloading and build cancellation remain **unsupported**; manual
local covers and the explicitly labelled browser lookup remain available.

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
