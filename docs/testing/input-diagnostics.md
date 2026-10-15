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

## Switch 2 verdicts (what the diagnostics say instead of a bare "B")

A Switch 2 controller (vendor 057E, products 2066-2073) gets its own plain-language block on the launcher's Input Diagnostics page (also in the copyable
report, the start-up log and the in-game **Controller status** page). It names the **first broken link** of the native chain and states the connection only
when SDL or the OS scan actually reported it. Pure logic: `sv_switch2_assess` / `sv_switch2_message` (`libs/viewer/src/sv_input_state.c`); facts: `sv_diag_switch2_message`.

| Verdict | Condition | Message (abridged) |
| --- | --- | --- |
| SDL driver missing | raw joystick, this SDL does not declare the driver | "Native USB support: NOT declared by this SDL build ... use the SDL in the Segarecomp package, or Steam Input / a compatible bridge" |
| libusb runtime unavailable | driver declared, libusb cannot be loaded | "Native USB support is available ... libusb runtime: NOT found" |
| Bluetooth | transport reported as Bluetooth | "Connection: Bluetooth. This SDL version does not provide native Switch 2 Bluetooth support ... Steam Input or a compatible controller bridge" |
| USB interface not claimed | driver + libusb present, still not a gamepad | "SDL did not turn the controller into a gamepad. The USB interface may not be accessible to libusb (on Windows it must be bound to WinUSB; Segarecomp never installs or changes drivers ...)". Run with `SEGARECOMP_INPUT_TRACE=1` for SDL's own libusb message |
| open failed | gamepad recognised, `SDL_OpenGamepad` failed | "Gamepad initialization: failed. Details: <SDL's error text>" |
| no events | opened, nothing arrived | "... press a button" |
| bindings | events arrive, game bindings missing/conflicting | "Fix them in Controls" |
| working | opened and events arrive | "button and stick events are arriving through SDL's gamepad layer" |

"Driver declared" is read once from `SDL_GetRevision()`: the Segarecomp SDL recipe appends `(segarecomp-switch2-usb)` only after its gate has checked that the driver
is compiled into the binary. For a different SDL it is a declaration, not a run-time proof, and a stock SDL reports "not declared". Connection type comes from
`SDL_GetJoystickConnectionState` (wired/wireless, once the device is open) or from the on-demand OS scan (`SDL_hid_enumerate` bus type); with neither, no connection
line is printed. Nothing here enumerates hardware per frame: the libusb probe is cached at start-up and the OS scan runs only when asked.

## Root cause of "Joystick found, but not a gamepad" on the reporter's Windows PC

Reported (Windows, release package, SDL 3.4.16): `libusb-1.0 loadable: no`, one device `"If_Hid"` `057e:2069`, `raw joystick (no gamepad mapping)`, `unknown, opened`,
3725 raw events, State B. The controller works through Steam Input. Nothing in this tells whether Steam used USB or Bluetooth.

1. The package shipped the **official** `SDL3.dll` from `SDL3-devel-3.4.16-VC.zip`. It has **no Switch 2 driver** and no libusb loader (`SDL_JOYSTICK_HIDAPI_SWITCH2` and the
   driver's error strings occur 0 times). SDL therefore never ran the controller's initialisation protocol: it only saw the generic HID interface Windows exposes
   (the `If_Hid` name) as a raw joystick, whose reports SDL has no mapping for. Raw events are not a gamepad implementation, and Segarecomp deliberately never guesses axes
   as buttons.
2. `libusb-1.0.dll` was not present either (and could not have helped: a libusb DLL cannot enable a driver that was left out when SDL was compiled).

What this change fixes (compile + package): a pinned SDL built **with** the driver and the libusb loader, plus the libusb runtime. What it cannot fix from software alone: see
"Windows USB access" below.

## Four different claims (never conflate them)

| Claim | Evidence in this change | Status |
| --- | --- | --- |
| **Driver compiled into SDL** | `build-sdl3.sh` configure gate (`HAVE_LIBUSB`, lazy `libusb-1.0.dll`) and `sdl_check.py driver` on the built binary (3 independent driver literals, not only the hint name); negative control: the same recipe with libusb disabled fails; verified on Linux locally and by CI on all three hosts | verified by CI |
| **Runtime dependency available** | the extracted package ships `libusb-1.0.dll` beside the launcher and in `sdl3/lib` (byte-identical to the built one); the package smoke test runs `Segarecomp --input-report` and the generated Genesis and Master System games from the extracted package under a scrubbed `PATH` and requires the packaged SDL path, the driver declaration and `libusb-1.0 loadable: yes` | verified by CI |
| **Controller successfully initialised** | needs a real controller whose USB interface is accessible to libusb | **NOT VERIFIED ON PHYSICAL HARDWARE** |
| **Actual physical input verified** | the manual procedure below | **NOT VERIFIED ON PHYSICAL HARDWARE** |

Automated tests use SDL **virtual** joysticks. They prove the diagnostic paths, hot-plug, remapping and navigation; they cannot prove the Nintendo protocol, the Windows USB
binding or a physical controller, and no virtual "Switch 2" test is offered as such.

## Findings for the Nintendo Switch 2 Pro Controller (VID 057E, PID 2069)

Investigated against the **pinned SDL 3.4.16** (sources in `packaging/build-sdl3.sh`). Reproducible without any controller:

1. **SDL's Switch 2 driver is USB-only and needs libusb.** `src/joystick/hidapi/SDL_hidapi_switch2.c` finds the bulk endpoints of **USB interface 1** through libusb,
   claims that interface (`libusb_claim_interface`) and then sends its own initialisation sequence; its Bluetooth initialiser is
   `return SDL_SetError("Nintendo Switch2 controllers not supported over Bluetooth")` with `FIXME: Need to add Bluetooth support`. Upstream tracks this in libsdl-org/SDL
   issue 15963 (open). **Bluetooth is not supported by the pinned native driver.**
2. **The driver is only compiled into an SDL configured with libusb.** `SDL_JOYSTICK_HIDAPI_SWITCH2` is defined under `#ifdef HAVE_LIBUSB` (`SDL_hidapijoystick_c.h`), and
   `HAVE_LIBUSB` comes from `find_package(LibUSB)` at SDL **build** time. With shared linking SDL loads libusb **at run time by name** (`SDL_LIBUSB_DYNAMIC`:
   `libusb-1.0.so.0`, `libusb-1.0.0.dylib` or `libusb-1.0.dll`).
3. **Windows build requirement (issue 14742).** CMake must be given the **full path of the `.lib`** (`-DLibUSB_LIBRARY=.../libusb-1.0.lib`), not a directory: otherwise SDL warns
   "Unable to extract dynamic library", loads nothing lazily, and the result "didn't work at all". Fixed by the recipe (and guarded: the build fails unless
   `SDL_LIBUSB_DYNAMIC "libusb-1.0.dll"` was generated). The same issue shows that a libusb DLL found only through the global `PATH` made it work for the reporter, which is why
   this package places the DLL where the loader looks: SDL's `SDL_LoadObject` is `LoadLibrary` by name, which searches the **executable's directory**, then the system
   directories, then `PATH`; it does **not** search SDL3.dll's own directory.
4. **Windows USB access (not solvable by packaging).** libusb's Windows backend can only open an interface that is bound to **WinUSB** (or libusbK/libusb0/UsbDk). A claim on an
   interface owned by another driver (for example the HID class driver) fails and SDL silently falls back to the generic HID/raw-joystick view. Whether Windows binds WinUSB
   **automatically** to interface 1 of `057e:2069` (it does when the firmware advertises a Microsoft OS descriptor) is **NOT VERIFIED** here: no physical controller or Windows
   host was available to this work. If it does not, the only driver-level remedy is a manual binding (for example with Zadig), which **replaces a Windows driver**, needs
   administrator rights and is therefore **not** done, installed, bundled or required by Segarecomp (no Zadig, ViGEmBus, HidHide, Steam, kernel driver or service). In that case
   the diagnostics report "USB interface not claimed", Steam Input keeps working, and nothing else regresses.
5. **Both import libraries describe the same DLL.** The launcher (clang/MSVC) links `SDL3.lib` produced by the SDL build; the bundled Zig links `libSDL3.dll.a`
   generated by `zig dlltool` from the exports of **that same `SDL3.dll`**. `sdl_check.py windows` verifies on the real files that both cover every export, that the DLLs are
   x86-64, that SDL3.dll does not import libusb statically (so it still loads without it), that every libusb function SDL3.dll looks up is exported by the shipped
   `libusb-1.0.dll`, and that neither needs a Visual C++ redistributable.
6. **A `gamecontrollerdb.txt` line is not a driver.** A mapping only names raw buttons; it does not send the Nintendo initialisation protocol, so it cannot replace the native driver.
7. **Newer SDL.** `release-3.4.18` (2026-10-02) and `main` were compared on 2026-10-15: `SDL_hidapi_switch2.c` is byte-identical to 3.4.16 and still contains the same
   Bluetooth `FIXME`; the 3.4.18 notes mention no Switch 2 change. A release number is not evidence of Bluetooth support, so the pin stays at 3.4.16 and Bluetooth is
   documented as unsupported (path forward: a future upstream release that implements the Bluetooth initialiser, then bump the pin and drop the limitation).
8. **Linux needs device permissions.** Open issue 15245 implicates permissions, but the reporter's proposed udev rule was insufficient. Do not run Segarecomp as root.

Sources checked 2026-10-15: [14742](https://github.com/libsdl-org/SDL/issues/14742) (closed, not planned; build/link discussion), [15963](https://github.com/libsdl-org/SDL/issues/15963) (open),
[15245](https://github.com/libsdl-org/SDL/issues/15245) (open), [pinned driver](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/joystick/hidapi/SDL_hidapi_switch2.c#L323-L391),
[libusb compile guard](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/joystick/hidapi/SDL_hidapijoystick_c.h#L38-L40).

### Follow-up: `driver = NONE (DISABLED)` for `057e:2069` on Windows (real trace)

Reported by the owner of a physical controller: interfaces MI_00 = HidUsb, MI_01 = WinUSB, MI_02 = usbaudio; SDL (`SDL_HINT_EVENT_LOGGING`/debug) says
`Added HIDAPI device 'Nintendo Switch Pro Controller' ... bluetooth 0 ... interface 0 ... path = 1-7:1.0, driver = NONE (DISABLED)`; driver compiled, libusb loads, joystick still unmapped.
So the **driver is compiled, libusb loads and Windows already binds WinUSB to interface 1**: the "WinUSB binding" suspicion of the first version is not the cause. What the log cannot say, from the SDL 3.4.16 source:

* `driver = NONE` is printed after `HIDAPI_SetupDeviceDriver()`; `(DISABLED)` is only the literal for "no driver object", not the hint's state. Four code paths end there silently: (1) `HIDAPI_GetDeviceDriver()` returns NULL (switch-2 hint off, `SDL_ShouldIgnoreJoystick`, usage-page filter); (2) `SDL_hid_open_path()` fails, logged only at DEBUG and without cleanup; (3) `InitDevice()` fails and `HIDAPI_CleanupDeviceDriver()` runs, discarding `SDL_GetError()`; (4) it never got that far.
* The device SDL lists is the **HID interface 0** (`path 1-7:1.0`, the libusb backend enumerates HID-class interfaces only, `libusb/hid.c should_enumerate_interface`). `SDL_hid_open_path` therefore does `libusb_open` + **`libusb_claim_interface(0)`** on a HidUsb-bound interface (`hidapi_initialize_device`), and only afterwards the Switch 2 driver looks for the bulk endpoints of interface **1** and claims it on that same handle. Whether the claim of interface 0, the claim of interface 1 or the init sequence fails on Windows is exactly what the source cannot tell.

**Diagnostic build.** `packaging/sdl-switch2-diagnostics.patch` (applied by `build-sdl3.sh` with `patch`, which fails if a hunk does not apply; the built DLL is checked for it) changes no behaviour. It records, per Nintendo Switch 2 device, every setup step with SDL's/libusb's own error text *before* failed cleanup, and
publishes it as the hint `SEGARECOMP_HIDAPI_SETUP_057e_<pid>`; libusb's open/claim failures become `SDL_SetError` texts with the numeric libusb code (`-3` ACCESS, `-5` NOT_FOUND, `-6` BUSY, `-12` NOT_SUPPORTED, ...). The Input Diagnostics page, the copied report and the in-game status page show it as a **setup trace**, one step per line, codes named:

| Trace step | Meaning |
| --- | --- |
| `begin iface=.. class=.. usage_page=..` | what SDL enumerated (interface, class, usage) |
| `stage=no_driver (switch2_driver_enabled=0/1, ignored=0/1, usage_filter_applies=0/1)` | path (1): hint disabled, ignored, or usage filter |
| `stage=open_path FAILED error="libusb_open failed..." / "libusb_claim_interface(0) failed: libusb error N"` | path (2): the libusb HID open, i.e. claiming the HID interface |
| `stage=open_path ok libusb_backed=0/1` | the SDL HID handle is (not) libusb-backed |
| `usb: libusb loaded` / `handle is libusb-backed` / `bulk endpoints interface=1 out=0x.. in=0x..` / `interface 1 claimed` / `initialization sequence sent` | driver progress (`FindBulkEndpoints`, claim, init) |
| `stage=init_device FAILED driver=.. error="..."` | path (3): the real `InitDevice` error |

Reproduction status: the patched SDL builds and its diagnostics logic is unit tested (including synthetic traces). The failing operation itself **cannot be reproduced without the physical controller on Windows**; the development artifact of this PR is built to expose it. No Windows driver is touched, no protocol code is copied into Segarecomp, no mapping is added, and no workaround is attempted until the trace names the failure.

### Packaging decisions

* **libusb is loaded dynamically and shipped as a separate DLL** (static linking would put LGPL-2.1 code inside SDL3.dll and require relinkable object files; a separate unmodified,
  replaceable DLL needs only the license text and the corresponding source, both shipped: `licenses/libusb-LGPL-2.1.txt`, `licenses/libusb-1.0.29-source.tar.bz2`).
* **MinGW build of `libusb-1.0.dll`** (imports only KERNEL32/msvcrt/USER32). The official Visual C++ build needs `VCRUNTIME140.dll`, which a clean machine may lack; its import library is
  used only to configure and compile SDL. Both come from the pinned libusb 1.0.29 release (SHA-256 in `packaging/build-sdl3.sh`).
* **DLL locations**: beside `Segarecomp.exe` (the launcher's executable directory) and in `sdl3/lib` (the directory the launcher prepends to `PATH` for generated games, where their
  `SDL3.dll` is found too). Generated games run from the cache and have no `SDL3.dll` of their own beside them.
* Linux/macOS are unchanged: libusb comes from the operating system (development files at SDL build time, `dlopen` at run time).

### Status matrix

| OS | USB-C / USB | Bluetooth |
| --- | --- | --- |
| Windows 10/11 | SDL with the driver + libusb **built and packaged, verified by CI**; controller initialisation depends on the interface being bound to WinUSB: **NOT VERIFIED ON PHYSICAL HARDWARE** | not supported by the pinned SDL driver; Steam Input / a bridge still work |
| Linux | release recipe requires the driver; needs OS libusb and udev permissions: **NOT VERIFIED on hardware** | not supported |
| macOS | release recipe requires the driver; needs OS libusb: **NOT VERIFIED on hardware** | not supported |

**Real-device support is NOT VERIFIED.** Do not read "driver compiled" or "libusb resolvable" as "the controller works".

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

## Physical test on Windows (Switch 2 Pro Controller over USB-C) — NOT VERIFIED ON PHYSICAL HARDWARE

No physical controller or Windows machine was available to the author, so everything below is the procedure for the person who owns one. Download the Windows development
artifact: open the pull request's **release** workflow run (Actions tab) and download `segarecomp-windows-x86_64` (a ZIP of the package, built from the PR head; it is
never published as a release). Extract it into an empty folder you can write to (not onto the Steam folder), then:

1. Close Steam, or disable Steam Input for this controller (Steam > Settings > Controller), so Steam does not capture it.
2. Connect the Switch 2 Pro Controller with a USB-C cable. Do not use Bluetooth for this test.
3. Start `Segarecomp.exe` normally (double-click), not through Steam.
4. Open **Settings > Input Diagnostics**. In the **SDL** block expect `Switch 2 USB driver: declared by this SDL build` and `libusb-1.0: found at startup`. Click **Scan OS devices**.
5. Read the **Nintendo Switch 2 controller** block, press buttons and move both sticks, then click **Copy report** and keep the text. Expected on success: a gamepad (not "raw joystick"),
   state OK, "Gamepad initialization: succeeded", Nintendo labels (South = B, East = A), live stick values. Any other verdict names the first broken link (see the table above).
6. Use the **Menu test** and navigate the launcher with the controller (D-pad/stick to move, South/B to activate).
7. Launch an already-recompiled **Genesis** game from the library. Check D-pad and left stick, the A/B/C buttons, Start (+), and the pause menu on Back (-) or Home. Open
   **Controls > Controller status & test** in the game and compare.
8. Repeat with a recompiled **Master System** game.
9. Unplug the cable while a button is held (it must release), plug it back in and confirm it works again without restarting.

Report: Windows version, the copied report from step 5, whether Device Manager shows the controller's USB interfaces as `WinUSB` (or "unknown device" / HID), and which steps passed. A
"USB interface not claimed" verdict with libusb available points at the Windows driver binding of interface 1 (see Findings, item 4), not at a missing build.
To see SDL's own libusb message start from a terminal with `SEGARECOMP_INPUT_TRACE=1`.

## Manual validation procedure (physical controller, all hosts)

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
* Switch 2 verdicts: pure logic (every verdict, negative controls, line length bound) `tests/sv_viewer_core_test.cpp`; SDL facts (driver declaration, raw device vs failed open, OS-scan
  transport, no invented connection type) `libs/viewer/test/sv_input_diag_test.c`, using virtual devices that carry the Switch 2 vendor/product: they prove the diagnostic chain only
* packaging gates (all hosts, CI `release.yml`): `packaging/build-sdl3.sh` configure gate + negative control (a build without libusb fails), post-build driver check on the real binary,
  Windows `packaging/sdl_check.py windows` (x86-64, lazy libusb, SDL's libusb lookups vs the shipped DLL, both import libraries vs SDL3.dll's exports, no redistributable runtime),
  `packaging/dependency_check.py` (packaged DLL dependencies), byte comparison of the extracted package with the built binaries, and `packaging/smoke_test.py` (`Segarecomp --input-report`,
  the generated Genesis and Master System games' start-up log, under a scrubbed `PATH`). The checker's own logic and negative controls: `tests/packaging_sdl_check_test.py`;
  recipe wiring: `tests/packaging_windows_sdl_recipe_test.py`
