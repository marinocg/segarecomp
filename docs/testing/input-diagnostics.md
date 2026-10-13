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
* on demand, a read-only list of the Nintendo HID devices **the operating system** exposes (`SDL_hid_enumerate`, vendor 057E) with the bus (USB or
  Bluetooth), which explains a controller the OS sees but SDL does not expose.

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
   connected by Bluetooth cannot work through SDL 3.4.x**, whatever Segarecomp does.
2. **The driver is only compiled into an SDL built with libusb.** In `SDL_hidapijoystick_c.h` `SDL_JOYSTICK_HIDAPI_SWITCH2` is defined under
   `#ifdef HAVE_LIBUSB`, and libusb is found at SDL *build* time by `find_package(LibUSB)`. libusb itself is loaded at **run time** (`dlopen`), so the
   package does not need to bundle it. Upstream issue 14742 documents the same requirement on Windows.
3. **Before this change no Segarecomp release could ever have produced input from this controller.**
   * The release workflow built SDL on Linux/macOS without libusb development files: `HAVE_LIBUSB` was undefined (checked with a local build of the
     pinned tarball: `strings libSDL3.so | grep -c SDL_JOYSTICK_HIDAPI_SWITCH2` -> `0`).
   * The official Windows `SDL3.dll` 3.4.16 (`SDL3-devel-3.4.16-VC.zip`, SHA-256 pinned in `release.yml`) contains **no** Switch 2 driver and no libusb loader
     (`grep -c -a SDL_JOYSTICK_HIDAPI_SWITCH2 SDL3.dll` -> `0`, `libusb-1.0` -> `0`).
4. **What changed.** `packaging/build-sdl3.sh` now reports whether the driver was compiled and, with `SDL_REQUIRE_LIBUSB=1` (set by the release workflow, which
   now installs `libusb-1.0-0-dev` on Linux and `libusb` on macOS), fails when it was not. Verified locally against the pinned tarball with libusb headers
   present: the hint string appears once and the library lists `libusb-1.0.so.0` as its dynamically loaded dependency; without them the script fails
   as intended. The runtime hint `SDL_HINT_JOYSTICK_HIDAPI_SWITCH2` is on by default whenever HIDAPI is, so no override is set.
5. **Windows is unchanged and is stated as a limitation.** Wired Switch 2 support in SDL needs libusb and a WinUSB driver binding for the device
   (what upstream issue 14742 describes). The launcher will not replace USB drivers or ask for privileged tools; the official prebuilt DLL has no driver.
6. **Linux needs device permissions.** Upstream issue 15245 shows a Switch 2 Pro that is listed but silent without access to the device node (a udev
   rule); the diagnostics surface this as state C (open error) or D (opened, silent) and the OS-level scan as "NOT a joystick in SDL".

### Status matrix

| OS | USB | Bluetooth |
| --- | --- | --- |
| Linux | driver compiled in (needs libusb-1.0 at run time and device permissions) - **NOT VERIFIED on hardware** | not supported by SDL 3.4.x |
| macOS | driver compiled in (needs libusb at run time, for example Homebrew) - **NOT VERIFIED on hardware** | not supported by SDL 3.4.x |
| Windows | not supported by the official SDL binary we ship | not supported by SDL 3.4.x |

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

Please run this and send the report; nothing in it uploads anything, and the report contains no ROM names or file paths.

1. Note the OS and version, whether the controller is connected by **USB cable** or **Bluetooth**, and the release you run.
2. Linux only: `lsusb | grep 057e` (expect `057e:2069` for the Switch 2 Pro) and `ls -l /dev/bus/usb/*/*` for the matching node; `ldconfig -p | grep libusb-1.0`.
3. Start the launcher. Open **Settings > Input Diagnostics** *before* connecting, then connect the controller (press a button to wake it).
4. Click **Scan OS devices**. Record whether `057e:2069` appears, the bus (USB/Bluetooth) and whether it says "SDL exposes a joystick for it".
5. Press every button and move both sticks. Record the state letter, the pressed-button chips (SDL position and "prints X"), and the stick values.
6. Use **Menu test** (A/South, Enter) and note "activated Nx, last via gamepad".
7. Click **Copy report** and paste it into the report.
8. Start any game, press **Back/Select**: the menu should open. Open **Controls > Controller status & test** and compare.
9. For opt-in verbose tracing start from a terminal with `SEGARECOMP_INPUT_TRACE=1` (logs every pad event and SDL's input messages; no ROM data).

What to report: OS, transport, `lsusb`/Device Manager vendor:product, the state letter, the OS-scan lines, whether any chip lights, and the copied report.
A result of "OS scan lists 057e:2069 on USB, SDL exposes NO joystick" on Linux/macOS means the libusb driver did not load (look at the
libusb-1.0 line and permissions); on Windows it is the expected limitation above.

## Where each claim is tested

* classification, advice text bounds, binding audit, Nintendo labels: `tests/sv_viewer_core_test.cpp`
* SDL layer with real SDL events from virtual joysticks (A-F, hot-unplug/replug, raw events never become gameplay, open-failure text, user mapping file,
  `SDL_INIT_GAMEPAD` failure in the summary): `libs/viewer/test/sv_input_diag_test.c`
* the launcher page end to end: `apps/segarecomp-launcher/launcher_input_diagnostics_test.py`
* the viewer's pad handling (hot-plug, remap listener ignoring the activating press, duplicate bindings, Back opens the menu): `libs/viewer/test/sv_host_test.c`
* packaging: `packaging/build-sdl3.sh` (driver presence report; `SDL_REQUIRE_LIBUSB`), `release.yml`
