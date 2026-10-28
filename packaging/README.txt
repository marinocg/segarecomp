Segarecomp
==========

Recompile Sega games to native code. Experimental.

Quick start
-----------
1. Extract this archive anywhere you can write to (do not run it from inside the archive).
2. Open Segarecomp (Segarecomp.exe on Windows, Segarecomp.app on macOS, ./Segarecomp on Linux).
3. Drop your ROMs onto the window, or click "Import ROM" (several files at once are fine). They are added to your
   library; adding a game does not build it. ROM files are added by reference and never modified or uploaded.
   ZIP archives (one ROM inside) are read without modification: the ROM is extracted into your local library.
4. Open a game and click "Build" (or "Recompile"), then "Play". The first build of a game can take a few minutes;
   later launches are instant. Each game's page shows its real status, size and build details.

Nothing else needs to be installed: the C compiler (Zig, used only to compile the generated C) and SDL3
are included in this package.

Good to know
------------
* No ROMs are included. Use only software you are legally entitled to analyze.
* Compatibility is experimental. Sega Genesis / Mega Drive images and (baseline) Sega Master System images
  are supported, and many games will not translate or run correctly yet. A Master System cartridge's mapper cannot be
  detected: the launcher asks you to choose it (or reads a `<rom name>.mapper.json` file beside the ROM). "Compatibility unknown" means nothing is known about that ROM.
* Sega CD (experimental): Import ROMs > "Add Sega CD game..." references your own BIOS image and your own disc (the CUE file); nothing is
  copied, and none of it is part of this package. Choose Build and Segarecomp compiles a playable program from those two files on this
  computer (several minutes the first time; it can be cancelled). The build finds which code the disc loads by running the console from reset;
  code that does not exist verbatim on the disc (generated or decompressed at run time) is never guessed: the program stops there with a typed
  stop and the build says so. A program built elsewhere with the developer procedure (docs/product/sega-cd-play.md) can still be imported
  and is verified against your BIOS and disc. No sound is available for Sega CD.
* In a game, Escape (or the gamepad Back button) opens the pause menu: Resume, Quick Settings, Video & Display (scaling,
  aspect, CRT presets), Controls (rebind keys and gamepad buttons, Player 1 and 2), Audio, System & Diagnostics (screenshot,
  and Reset for Master System) and Return to Launcher. F11 toggles fullscreen, F12 saves a screenshot of the game picture.
  Default controls (Genesis): arrow keys = D-pad, Z = A, X = B, C = C, Return = Start; Player 2: I/K/J/L, U = A, O = B,
  P = C, Right Shift = Start. Master System: arrow keys = D-pad, Z = button 1, X = button 2, P = the console's Pause button
  (distinct from the pause menu). Gamepads work too (South/East/West = A/B/C or buttons 1/2, Start).
  Settings > Input Diagnostics explains why a controller does not work. Nintendo Switch 2 controllers: USB only (Bluetooth
  is not supported by SDL's driver); on Windows the controller's USB interface must be usable by libusb (WinUSB), which
  Segarecomp never installs. Steam Input keeps working. Details: docs/testing/input-diagnostics.md in the source repository.
* Built games are cached per user, and your library (the list of games, covers, settings) is kept next to the cache:
    Windows: %LOCALAPPDATA%\Segarecomp\
    macOS:   ~/Library/Application Support/Segarecomp/
    Linux:   ~/.local/share/Segarecomp/
  The library lives in a `library` folder inside it (never cleaned by Storage). A ROM file you add is never copied there;
  a ROM extracted from a ZIP archive is kept in its `roms` folder. Clearing build caches, keeping metadata only or
  removing a game from the library does not delete those extracted copies.
  Use Storage to see what each game uses and to reclaim space safely; nothing is deleted without a confirmation that lists
  exactly what goes and what stays. "Cache folder" opens it.
* If a build fails open the game and choose "View logs"; the same text is in build.log inside the cache folder
  (games/<id>.failed/). Attach it to bug reports.
* Covers: Change cover picks a local image (PNG or JPEG). "Download cover (Libretro)" in a game's menu, or
  Settings > Metadata > "Download missing covers", fetches box art from Libretro's public thumbnails through
  this computer's curl, only when you ask; only the game's name is sent, never ROM data or hashes.
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
