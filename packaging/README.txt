Segarecomp
==========

Recompile Sega games to native code. Experimental.

Quick start
-----------
1. Extract this archive anywhere you can write to (do not run it from inside the archive).
2. Open Segarecomp (Segarecomp.exe on Windows, Segarecomp.app on macOS, ./Segarecomp on Linux).
3. Drop your ROM onto the window, or click "Browse...".
4. Click "Recompile & Play". The first build of a game can take a few minutes; later launches are instant.

Nothing else needs to be installed: the C compiler (Zig, used only to compile the generated C) and SDL3
are included in this package.

Good to know
------------
* No ROMs are included. Use only software you are legally entitled to analyze.
* Compatibility is experimental. Only Sega Genesis / Mega Drive images are supported, and many games
  will not translate or run correctly yet. "Compatibility unknown" means nothing is known about that ROM.
* Controls: arrow keys = D-pad, Z = A, X = B, C = C, Return = Start, Escape = quit. There is no remapping.
* Built games are cached per user:
    Windows: %LOCALAPPDATA%\Segarecomp\
    macOS:   ~/Library/Application Support/Segarecomp/
    Linux:   ~/.local/share/Segarecomp/
  Your ROM is never copied there. "Open cache folder" (under Advanced) opens it; delete a folder to force a rebuild.
* If a build fails choose "View diagnostics"; the same text is in build.log inside the cache folder
  (games/<id>.failed/). Attach it to bug reports.
* On Windows keep the package path to characters in your system code page.
* macOS: the app is not notarized. On first launch use right-click > Open, or run
  xattr -dr com.apple.quarantine Segarecomp.app
* Linux: needs glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+, Fedora 36+) and a graphical session (X11 or Wayland).
* WSL (Windows Subsystem for Linux): the launcher can run under WSLg, but WSLg does not ship a
  file-chooser service, so "Browse..." will show an error ("The file picker could not be opened");
  it also does not bridge drag-and-drop from Windows Explorer into a WSLg window. This is a WSLg
  limitation, not specific to Segarecomp; native Linux, Windows and macOS are unaffected. As a
  workaround under WSL, use the bundled CLI directly: `bin/segarecomp build --rom <file> --output <dir>
  --cc toolchain/zig --cc-arg cc --cc-arg -target --cc-arg <your target> --runtime-dir runtime/platforms/genesis`,
  then run the produced `<dir>/game`.

Licenses and third-party notices are in the licenses folder.
