# Controller input diagnostics and the Nintendo Switch 2 Pro Controller

This page records what was **verified**, what is **not verified**, and how to validate a physical controller. It exists because a real Switch 2 Pro
Controller did not work with the first SEG-058 launcher/viewer and nothing in the UI could say why.

## What the diagnostics are

One detection engine, `libs/viewer/src/sv_input_diag.c` (SDL3, no window, no ROM), is shared by the launcher (**Settings > Input Diagnostics**) and by
every game (**pause menu > Controls > Controller status & test**, also **System**). It records:

* the SDL version, the result of `SDL_Init` for video and for the gamepad subsystem (a failure is **logged and shown**, no longer a silent fall-back to
  keyboard-only);
* every joystick SDL enumerates, including those SDL cannot treat as a gamepad: name, SDL id, GUID, vendor:product, link (wired/wireless) where SDL
  reports it, SDL player index;
* the outcome of every `SDL_OpenGamepad` with SDL's own error text;
* live buttons by SDL **position** (South/East/West/North/Back/Guide/Start/...) plus, for Nintendo controllers, the label printed on the button
  ("B" for South, "A" for East, "-" for Back, "+" for Start, "Home" for Guide), live stick/trigger values with the dead zone, event counts and the time of
  the last event;
* on demand, a read-only SDL HIDAPI view of Nintendo devices (`SDL_hid_enumerate`, vendor 057E) with the bus (USB or Bluetooth). This is not an exhaustive independent OS inventory. Matching by VID/PID is model-level correlation, not individual-device identification.

The OS scan runs in a background worker in both UIs. It returns an isolated snapshot;
SDL device matching is done on the UI thread, not against concurrently changing records.
The libusb availability probe is cached at startup (restart after changing system
libraries). Diagnostic rendering does not load libusb or stat the mapping file on
each frame. Shutdown waits for an outstanding scan; cancellation is not offered.
The viewer gallery now verifies its requested states, and produces live input only
after SDL opens its virtual device. Its injected open-failure fixture is labelled
synthetic, never evidence of an actual locked physical controller.

A controller is **never reported as working because it was enumerated**. The classification (`sv_input_state.c`, SDL free, unit tested):

| State | Meaning | Typical cause |
| --- | --- | --- |
| **A** | no joystick is visible to SDL | not connected/awake, wrong cable, or a controller SDL has no driver for on this transport |
| **B** | a joystick is listed but SDL has no gamepad mapping for it | no mapping/driver; Segarecomp never guesses raw axes as buttons. A `gamecontrollerdb.txt` line for the GUID makes it a gamepad |
| **C** | recognised as a gamepad but `SDL_OpenGamepad` failed (exact SDL error shown) | another program holds it, OS permissions (udev on Linux) |
| **D** | opened, but no button/axis event has arrived | silent driver, controller asleep; press a button |
| **E** | events arrive, but the game bindings are missing or conflicting | fix in Controls or Restore Defaults |
| **F** | SDL delivers events, but the launcher's own menu navigation has not reacted | report it |
| **OK** | button/axis events are arriving (and the bindings/navigation check out) | |

## Findings for the Nintendo Switch 2 Pro Controller (VID 057E, PID 2069)

Investigated against the **pinned SDL 3.4.16** (the version in `packaging/`). Evidence is reproducible without any controller:

1. **SDL's Switch 2 driver is USB-only.** `src/joystick/hidapi/SDL_hidapi_switch2.c` talks to the controller through libusb bulk endpoints;
   its Bluetooth initialiser is `return SDL_SetError("Nintendo Switch2 controllers not supported over Bluetooth")` with a `FIXME: Need to add Bluetooth
   support`. Upstream tracks this in libsdl-org/SDL issue 15963 ("Nintendo Switch 2 Pro Controller Bluetooth Implementation", open). **A controller
   connected by Bluetooth is not supported by the pinned SDL 3.4.16 native Switch 2 driver**. External translation paths are outside this claim.
2. **The driver is only compiled into an SDL built with libusb.** In `SDL_hidapijoystick_c.h` `SDL_JOYSTICK_HIDAPI_SWITCH2` is defined under
   `#ifdef HAVE_LIBUSB`, and libusb is found at SDL *build* time by `find_package(LibUSB)`. libusb itself is loaded at **run time** (`dlopen`), so the
    package need not bundle it, but it must be available on the host for this native USB driver. Upstream issue 14742 documents Windows build/link requirements (closed as a discussion redirect, not a support fix).
3. **The previously inspected release binaries lacked the native Switch 2 driver.** This does not exclude third-party input translation paths.
   * The release workflow built SDL on Linux/macOS without libusb development files: `HAVE_LIBUSB` was undefined (checked with a local build of the
     pinned tarball: `strings libSDL3.so | grep -c SDL_JOYSTICK_HIDAPI_SWITCH2` -> `0`).
   * The official Windows `SDL3.dll` 3.4.16 (`SDL3-devel-3.4.16-VC.zip`, SHA-256 pinned in `release.yml`) contains **no** Switch 2 driver and no libusb loader
     (`grep -c -a SDL_JOYSTICK_HIDAPI_SWITCH2 SDL3.dll` -> `0`, `libusb-1.0` -> `0`).
4. **What changed.** `packaging/build-sdl3.sh` now reports whether the driver was compiled and, with `SDL_REQUIRE_LIBUSB=1` (set by the release workflow, which
   now installs `libusb-1.0-0-dev` on Linux and `libusb` on macOS), fails when it was not. Verified locally against the pinned tarball with libusb headers
   present: the hint string appears once and the library lists `libusb-1.0.so.0` as its dynamically loaded dependency; without them the script fails
   as intended. The runtime hint `SDL_HINT_JOYSTICK_HIDAPI_SWITCH2` is on by default whenever HIDAPI is, so no override is set.
5. **Windows remains a limitation.** The inspected official prebuilt DLL has no native driver. Issue 14742 establishes libusb build/link requirements, not a reason to replace a user's USB driver. The launcher neither changes drivers nor requests elevated execution.
6. **Linux needs device permissions.** Open issue 15245 implicates permissions, but the reporter's proposed udev rule was insufficient. C/D are possible diagnostic classifications, not a proven attribution. Do not run Segarecomp as root to work around it.

Direct sources checked 2026-10-14: [15963 (open)](https://github.com/libsdl-org/SDL/issues/15963), [14742 (closed, not planned)](https://github.com/libsdl-org/SDL/issues/14742), [15245 (open)](https://github.com/libsdl-org/SDL/issues/15245), [pinned USB/Bluetooth initialization](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/joystick/hidapi/SDL_hidapi_switch2.c#L323-L391), and [libusb compile guard](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/joystick/hidapi/SDL_hidapijoystick_c.h#L38-L40).

The local configured `sdl3-prefix` and existing staged Linux SDL are 3.4.16 **without** `HAVE_LIBUSB`/the native Switch 2 driver. A separate libusb-enabled build contains it; the updated release recipe requires it on Linux/macOS. Recipe intent is not proof about an arbitrary shipped binary. A loadable libusb library or enabled hint alone does not prove SDL compiled the driver. This WSL2 session exposes no input/USB/HID device nodes and SDL enumerates zero joysticks; that says nothing about controllers connected to the Windows host.

### Status matrix

| OS | USB | Bluetooth |
| --- | --- | --- |
| Linux | release recipe requires driver; verify actual artifact, runtime libusb and permissions — **NOT VERIFIED on hardware** | no native pinned-driver support |
| macOS | release recipe requires driver; verify actual artifact and runtime libusb — **NOT VERIFIED on hardware** | no native pinned-driver support |
| Windows | inspected official SDL binary lacks native driver | no native pinned-driver support |

**Real-device support is NOT VERIFIED.** No physical Switch 2 Pro was available to this work; automated tests use SDL **virtual** joysticks and prove the
code paths (classification, hot-plug, open failure, raw-joystick handling, user mapping files, navigation), not that a physical controller works.

### Available alternatives (not "support")

* The keyboard always works (and Esc always opens the menu).
* Any controller SDL already maps (Xbox/PlayStation/Switch Pro/8BitDo/...) works through the same path; the tests exercise it with virtual gamepads.
* A controller SDL lists only as a joystick (state B) becomes an explicit gamepad with a mapping line in `gamecontrollerdb.txt` in the library folder (or
  `SEGARECOMP_GAMEPAD_MAPPINGS=<file>`), loaded by launcher and games. This is tested with a virtual joystick. Segarecomp ships no mapping database.
* Segarecomp does not add third-party drivers, a Steam Input dependency or a Bluetooth stack.

## Menu button versus game buttons

In a game the pause menu opens with **Back/Select (Nintendo "-")**, **Guide (Home)** or **Esc**. **Start (Nintendo "+") is a game button** (Genesis Start; the Master System
Pause button), so pressing "+" to open the menu does nothing menu-related by design; the Controls page, the Controller status page and the first menu
page say so. SDL names buttons by position: South is the Nintendo **B**, East is **A**; the cue glyphs follow the connected controller's printed labels.

## Manual validation procedure (physical controller)

Nothing is uploaded automatically. Review/redact the copied report and verbose logs before sharing: SDL error text may include device paths or identifying information. ROM data is not collected by the diagnostics engine.

1. Note the OS and version, whether the controller is connected by **USB cable** or **Bluetooth**, and the release you run.
2. Linux only: `lsusb | grep 057e` (expect `057e:2069` for the Switch 2 Pro) and `ls -l /dev/bus/usb/*/*` for the matching node; `ldconfig -p | grep libusb-1.0`.
3. Start the launcher. Open **Settings > Input Diagnostics** *before* connecting, then connect the controller (press a button to wake it).
4. Click **Scan OS devices**. Record whether `057e:2069` appears, the bus (USB/Bluetooth) and whether it says "SDL exposes a joystick for it".
5. Press every button and move both sticks. Record the state letter, the pressed-button chips (SDL position and "prints X"), and the stick values.
6. Use **Menu test** (South, Nintendo B, or Enter) and note "activated Nx, last via gamepad".
7. Click **Copy report** and paste it into the report.
8. Start an already-built viewer using the bounded command below, immediately press **Esc** or **Back/Select**, then open **Controls > Controller status & test** and compare. Start/Plus is still a guest button.
9. For opt-in verbose tracing start from a terminal with `SEGARECOMP_INPUT_TRACE=1` (logs every pad event and SDL's input messages; no ROM data).

What to report: OS, transport, `lsusb`/Device Manager vendor:product, the state letter, the OS-scan lines, whether any chip lights, and the copied report.
A result of "OS scan lists 057e:2069 on USB, SDL exposes NO joystick" is consistent with missing/disabled native support, library/permission problems or initialization failure; it does not identify the cause by itself.

On Linux, use the normal desktop user and substitute actual executable paths:

```sh
# Launcher-only diagnostic session; do not click Play during this bounded check.
SEGARECOMP_INPUT_TRACE=1 timeout --kill-after=5s 180s "/path/to/package/Segarecomp"
# Already-built generated viewer: both wall time and guest execution are bounded.
SEGARECOMP_INPUT_TRACE=1 timeout --kill-after=5s 180s "/path/to/generated-viewer" --instruction-budget 100000000
```

Repeat USB and Bluetooth separately, recording any external bridge. Budget exhaustion or guest stop is not by itself an input failure. On other hosts use an equivalent wall-clock watchdog and the same finite guest instruction budget.

## Where each claim is tested

* classification, advice text bounds, binding audit, Nintendo labels: `tests/sv_viewer_core_test.cpp`
* SDL layer with real SDL events from virtual joysticks (A-F, hot-unplug/replug, raw events never become gameplay, open-failure text, user mapping file,
  `SDL_INIT_GAMEPAD` failure in the summary): `libs/viewer/test/sv_input_diag_test.c`
* the launcher page end to end: `apps/segarecomp-launcher/launcher_input_diagnostics_test.py`
* the viewer's pad handling (hot-plug, remap listener ignoring the activating press, duplicate bindings, Back opens the menu): `libs/viewer/test/sv_host_test.c`
* packaging: `packaging/build-sdl3.sh` (driver presence report; `SDL_REQUIRE_LIBUSB`), `release.yml`
