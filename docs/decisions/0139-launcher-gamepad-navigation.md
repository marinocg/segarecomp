# ADR 0139: Gamepad-only navigation of the launcher and a built-in file picker

- Status: accepted (SEG-069-T004).
- Context: ImGui keyboard/gamepad navigation was enabled, but real journeys needed a mouse: a parent window's D-pad moves into scrolling child windows are scored as
  "clipped" and skipped controls (the library grid below its toolbar, the storage rows, the settings body); build controls vanished under the focus when a build started;
  closing a sheet with East also took the page back; pointer shortcuts drawn over cards were navigation stops; and the OS file dialog (a mandatory step to add a game)
  cannot be assumed to accept gamepad input.
- Decision:
  1. Keep the existing SDL3 + ImGui navigation; fix the routes instead of adding a navigation layer. The library toolbar and grid share one scrolling child and the
     storage rows are part of the page, so every control is a candidate of every move. Where a parent/child boundary remains (sidebar <-> page, Settings tab column
     <-> body), explicit D-pad hops move focus to the recorded entry control (Right enters, Left at the left edge returns; the narrow Settings layout uses Down/Up).
  2. Focus follows the control the user just used: Build -> Cancel build / Unqueue -> Build on Details; a sheet returns focus to the control that opened it; the first
     control of a sheet gets focus when it opens. A press that closes a sheet is not also a "back" of the page.
  3. Pointer shortcuts drawn over other controls (card star and Play, row Details/Play, the cover pencil, the sidebar cache-folder icon) are not navigation targets;
     their actions are reachable through the game's menu (North) or Settings.
  4. A built-in file picker (`file_picker.*`, `app_picker.cpp`) is used for Import / Locate / Cover when a controller was the last input device, when forced with
     `SEGARECOMP_FILE_PICKER=builtin`, or when no system chooser is available. It is confined to a chosen root, lists only the allowed types, never follows symlinks,
     re-checks a file when it is chosen and delegates every real validation to the existing import/locate/cover code. The native dialog remains for mouse and keyboard.
  5. A one-line button legend is drawn only while a controller is the last input device. No text-entry keyboard is added: no ordinary journey needs typing (search
     and title editing stay keyboard features; the search field can be entered and left with the pad).
- Test seams (not user features): `pad_seek`, `focus_id`, `nav_rect` and the `expect` keys of the GUI script; the first-run and picker environment variables.
- Consequences: new controls must be ui-kit widgets (they record their focus id), must not steal focus when replaced, and must not be drawn as nav-enabled overlays.
