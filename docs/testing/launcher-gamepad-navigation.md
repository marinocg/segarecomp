# Launcher: operating it with a gamepad only (SEG-069-T004)

The launcher's ordinary journeys can be completed with an SDL gamepad and no mouse or keyboard: first-run setup, importing a ROM, browsing the library, opening a game's
page, building / cancelling / queueing builds, playing an earlier build, Storage cleanup and Settings. This page records what is automated and how to walk the
same journeys by hand on a real controller (CI cannot drive a real device or an OS-specific dialog).

## Button conventions

| Button | Action |
| --- | --- |
| D-pad / left stick | move focus (spatial); in the sidebar Up/Down walk the four areas |
| **A** (South) | activate the focused control (open a card, press a button, tick a row, enter a folder) |
| **B** (East) | back: closes a sheet, leaves Details/Storage/Settings for the Library, goes up one folder in the picker; on the Library page it moves focus to the sidebar |
| **Y** (North) | the focused game's menu (Details, Play, Build, Cancel build, Remove from build queue, favorite, covers, storage, remove) |
| **LB / RB** | previous / next page in the built-in file picker |
| D-pad **Right** in the sidebar | enters the page (the first game card on the Library; the first control elsewhere). **Left** at a page's left edge returns to the sidebar |

A one-line legend of these buttons is shown at the bottom of the window while a controller was the last input device (never for mouse/keyboard users).
Pointer-only shortcuts drawn over a card or cover (the star, Play over the box art, the pencil over a cover, the sidebar's cache-folder icon) are not navigation
targets; their actions are in the game's menu (Y) and Settings.

## Built-in file picker

The native OS file dialog cannot be assumed to accept gamepad input, so when a controller was the last input device **Import ROMs**, **Locate ROM** and
**Change cover** open a built-in picker instead (mouse and keyboard keep the native dialog; `SEGARECOMP_FILE_PICKER=builtin|native` forces either, and the built-in
picker is also used when no system file chooser exists, for example under WSL). It is deliberately not a file manager:

* it starts from a list of locations (Home, Desktop, Documents, Downloads, mounted media or drives) and cannot leave the chosen location: every folder it lists or
  enters and every file it returns must lie inside it (no `..`, no symlinked folder or file is followed or offered);
* it lists only folders and the file types the action accepts (ROM / ZIP files, or PNG / JPEG for a cover), skips hidden names and caps a folder at 4000 entries;
* it hands plain paths to the existing import / locate / cover code, which keeps doing every check (ROM size limits, ZIP policy, SHA-256 match, image validation);
* cancelling (B at the list of locations, or the Cancel button) changes nothing.

## Automated coverage

* `file_picker_test`: the picker's model (containment, `../` and prefix-sibling escapes, symlinks, filtering, truncation, choose-time re-check).
* `launcher_gamepad_journey_test.py` (real window, SDL dummy video driver, virtual joystick; **no mouse event is injected**; run at 1080x740 and 720x520): first-run sheet
  (focus trapped, South confirms, East defers); empty library -> picker -> tick two ROMs -> import, plus back/cancel/root confinement; library toolbar (drop-down popup,
  view switch, search field entered and left), grid hops, card menu (favorite, Details) and Back with focus restoration; Details -> Build -> Cancel build, a second game
  queued and removed from the queue (focus follows Build -> Cancel/Unqueue -> Build); the playable earlier build's Play and Recompile; Settings (tabs or the narrow
  section drop-down, a long tab scrolled to its last control, restore-defaults confirmed/cancelled with focus returning to the control); Storage cleanup confirm/cancel;
  hot-plug and the hint bar. `pad_seek <control> <direction>` presses real virtual-gamepad D-pad buttons until the control has focus, which is how reachability is asserted.
* `launcher_gui_navigation_test.py`, `launcher_input_diagnostics_test.py`: keyboard, mouse overlay buttons and the input diagnostics are unchanged.

## Manual smoke walk (a real controller, once per release)

1. Start with a fresh profile (`SEGARECOMP_LIBRARY_DIR` / `SEGARECOMP_CACHE_DIR` pointing at empty folders) and a controller connected; do not touch the mouse.
2. First-run sheet: D-pad between the two choices, **A** to confirm; relaunch: it does not return. Repeat with **B**: it returns next start.
3. Library is empty: D-pad to **Browse for ROMs...**, **A**. In the picker choose a location, open the folder holding your own legally owned ROMs, tick two files with **A**,
   move to **Import (2)**, **A**. Try **B** repeatedly: it goes up, to the locations, then closes without importing.
4. Open a game with **A**. **Right** from the sidebar enters the page; move to **Build**, **A**; focus lands on **Cancel build**; **A** cancels. Build again, go **B** back, open a
   second game, **Build**: it is queued and focus lands on **Unqueue**.
5. **Y** on a card: favorite it; **Y** again: Details; **B** returns to the same card.
6. Settings: walk the sections, scroll a long one with the D-pad to its last control, open **Restore launcher defaults**, **B** to cancel.
7. Storage: tick a game, **Remove build intermediates**, **B** to cancel, repeat and confirm.
8. Unplug and replug the controller while on a page: the window stays usable with the keyboard and the pad works again after reconnecting.
