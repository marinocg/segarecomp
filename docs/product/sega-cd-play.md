# Sega CD: playing in the viewer and in the launcher

What works, what does not, and how to get a game into the launcher. Design: [ADR 0204](../decisions/0204-seg-078-sega-cd-interactive-viewer-and-launcher-integration.md);
machine contract: [sega-cd-machine-contract.md](../architecture/sega-cd-machine-contract.md); developer procedure:
[platforms/sega-cd/README.md](../../platforms/sega-cd/README.md).

## Status in one paragraph

Segarecomp can run a **generated-native Sega CD program** (a program compiled ahead of time from your own BIOS image and disc) in the same window as Genesis and Master System games, with live CD music (CD-DA), a deterministic
stream of the console's **PCM sound effects** (RF5C164) and the **FM and PSG** sound the main-side Z80 driver produces, all mixed on one timeline and delivered to the audio device. The CD-DA/PCM mix is checked against an independent reference
where one exists; the FM/PSG output is **not reference-checked, not verified by listening, and its level against CD-DA/PCM is not established**.
Two titles are recorded in the [compatibility ledger](compatibility-ledger.json): **Sonic CD (USA)** and **Earthworm Jim Special Edition** were each **completed by the maintainer** (Sonic CD including its special stages).
These are **maintainer-reported** playthroughs: the build mode, exact image identity, revision and save workflow were not recorded, and no session recording or independent replay exists, so they are not an independently validated full-game claim.
The automated evidence is separate and narrower: Sonic CD's boot, title, START presses, first level, timed input, special-stage attract demo and intro are checked frame for frame against an independent reference at pre-registered checkpoints
(checkpoint evidence, **not** a complete-compatibility claim), and Earthworm Jim has a deterministic scripted START-then-RIGHT route of 20,000 frames without a stop (no reference emulator).
**The internal backup RAM persists** between sessions (v0.7.0). The viewer's widescreen Level 2/3 applies to Sega CD but is always Partial **(main only)**.
**Segarecomp can build a program for you from your own BIOS and disc** (the launcher's *Build*, experimental; ADR 0205): it discovers by itself which disc
spans the console loads into RAM and executes, and compiles them. It can run only the code its build proved to come from the disc (anything else stops with a
typed stop such as `entry_not_compiled`), so coverage of a given title is partial until the obstacles listed below are removed. A program built elsewhere with
the developer procedure can still be imported. Everything is yours: the BIOS, the disc and the program are referenced
by path, never copied, never uploaded and never part of a release.

## In the launcher

1. **Add game** takes a Sega CD disc like any other game: choose or drop the disc's **.cue** file (the track files must be in the same folder). The file waits in
   the **review** (nothing is added yet) with the disc's verdict: how many tracks and which files were found, or which track files are missing (named, never a
   path), or - for a layout outside what Segarecomp reads - the product's own typed reason and what to do. The disc is checked by the project's one CUE parser:
   `FILE "name" BINARY`, tracks `MODE1/2048`, `MODE1/2352` or `AUDIO`, every file beside the CUE, at most eight files, and a Sega CD boot header on track 1.
   Then the review asks for **your own BIOS image** (a file chooser; Segarecomp neither includes nor downloads one). **Add & prepare** adds the game and builds a
   playable program on this computer (one build at a time, cancellable); **Add only** keeps it in the library to prepare later. A prebuilt program built with the
   developer procedure is attached afterwards: **Game > Advanced > Program > Import / Replace** (or the More menu), and **Verify** re-checks it. Sega CD support
   is experimental: a successful preparation does not mean the whole game is playable.
   (Headless: `Segarecomp --sega-cd ...` is unchanged.)
2. The launcher stores only paths, sizes and SHA-256 values. It measures the BIOS and CUE, runs `<program> --describe`, compares the program's claims with
   your files and checks every disc file the program names. States you can see:

   | State | Meaning |
   | --- | --- |
   | Inputs not configured | the BIOS or the disc is missing or moved (Locate...) |
   | No program yet (can be built) | nothing to run yet; choose **Build**, or import a verified program |
   | Headless-only build | the program has no viewer; rebuild it with `--viewer` |
   | Stale program | the file changed since it was verified, or it speaks another runtime contract (older build) |
   | Mismatched | the program was built for another BIOS or disc, or a file changed |
   | Ready to play | the BIOS, the disc files and the program match each other |
   | Running | the viewer is open |
   | Typed execution stop | the program stopped on a documented condition (the class is shown) |

3. **Play** opens the viewer. Keys: arrows = D-pad, Z/X/C = A/B/C, Return = Start (change them in the viewer's pause menu, shared with Genesis);
   Esc or the gamepad's Back button opens the pause menu; **F6** holds the machine, **F9** steps one frame while held (F7 is the rewind key on every console; **F5** / **F8** quick save / quick load, five more slots in the pause menu: see [save-states.md](save-states.md)). Closing the window returns to the
   launcher (a clean end).
4. **Record this session** (details page) writes a small text script of your button presses at exact frame boundaries. **Replay** plays it back in the
   viewer; **No window** replays it as fast as possible and reports the result. A replay reproduces the run exactly. The recording is written when the viewer closes (a crash loses it); sessions you do not record have no length limit, a recording is limited to about 2,000 button changes. "No window" stops by itself two seconds of game time after the last recorded button change.

## Remembered BIOS (local profiles)

When a BIOS is used to add a game, the launcher remembers it so the next disc does not ask again - as a **reference, never as trust**. The store is a small JSON file
in the library folder, `sega-cd-bios.json` (written atomically through a temporary file and one rename, like the library and settings files):

```json
{ "schema": 1, "profiles": [ { "sha256": "<64 hex>", "size": 131072, "region": "USA", "path": "<where the file was>", "verified_at": 1700000000 } ] }
```

The key is the verified identity (SHA-256 of the whole file, at most 128 KiB); `region` is only what the image's own header declares (`USA`, `Japan`, `Europe`, or
absent when the header says nothing usable - it is never inferred from a file name); `path` is where the file was. Nothing else is stored: no trust flag, no content.
A missing, damaged or newer-schema file reads as "no remembered BIOS"; unknown members are ignored; an older release ignores the file (no existing file changes).
Rules at every use (the review, Add, and the game's later checks):

* The file is **measured again** at its remembered location. Missing (moved or removed), unreadable, too large, or different content (**changed**) is an explicit
  state with its next action: **Locate...** (the chosen file must be exactly that BIOS - the same SHA-256 - and the profile then follows it) or **Choose another
  BIOS...**. A profile is never used because it was once verified.
* One verified BIOS whose header region does not contradict the disc's is offered as the choice. **Several** verified BIOS, or one whose region contradicts the disc, are
  never chosen for the user: each is listed with its state and **Use**.
* The BIOS is measured once more when **Add** runs; a file that changed between choosing and adding is refused and the row asks again.
* A Sega CD game's identity is its BIOS and CUE digests: the same disc with another BIOS is another game. A game's saves, recordings and states live in the game's folder and are never touched by
  adding, preparing, replacing a program or removing a profile.

## Building a program (Build)

**Build** (the details page, or `Segarecomp --sega-cd --bios ... --cue ... --build` without a window) runs `segarecomp sega-cd build` with the compiler bundled in the
release. Several minutes the first time (a real disc compiles tens of millions of lines of generated C; later builds reuse the compiled objects), cancellable
at any time (the whole tool tree is stopped and the attempt's files are removed; an earlier finished build is kept). The built program is then verified like an
imported one (it must state that it was built for exactly your BIOS and disc). A Sega CD build uses the launcher's one build flow: it joins the same queue as every
other game (one build at a time), shows its stages as chips (generate, compile, link, discovery pass, confirm, viewer) with a live log, **Cancel build** (or
**Unqueue**) sits where a native build's does, and a previous successful program is kept when a build fails or is cancelled (the new program is bound to the game only after it is verified against your BIOS and disc).

### Build modes

The details page offers three modes (one at a time; the choice is stored per game and applies at the **next** Build):

| Mode | What it compiles | Use it when |
|---|---|---|
| **Compatibility** (the default of existing games) | the code the build's own no-input run reaches, plus the disc spans that run proves | you want the narrowest, long-established behaviour |
| **File-first (structural)** (recommended for broader coverage) | Compatibility's code **and** the structurally supported instructions of the eligible files of the disc's ISO9660 volume, before the game runs | a title stops at `entry_not_compiled` after a menu choice, a START press or another transition the build never took |
| **Optimized (experimental)** | a frozen model proposes which code to compile; it can omit code the game needs | experiments only |

File-first is the product's `--file-first structural`; headless: `Segarecomp --sega-cd --bios ... --cue ... --build --file-first structural` (exclusive with
`--aot-policy optimized`, as in the product CLI). It reports `build_mode_requested`, `build_mode_effective` and `file_first_inventory=complete|incomplete`.
Files that are oversize, duplicated or without code structure are not compiled, so the inventory is often reported **incomplete**: code outside it still stops at
an uncompiled entry. Compiled bodies are bound to the game only when the bytes the game itself loads match the disc file exactly (dynamic verified binding);
nothing is decoded, compiled or guessed at run time. A disc without a readable ISO9660 volume cannot be inventoried: the build fails with that reason and
nothing is substituted - choose Compatibility explicitly. File-first does **not** add sound and does not guarantee a complete game; code that is generated,
decompressed or transformed at run time stays unsupported.

The page shows the **selected** mode and, separately, how the **current program was built** (read from the build's structured result; a program you imported, or built
by an earlier release, shows "not recorded" - nothing is invented). Changing the selection never changes an existing program: **Build** again to apply it. When a
program built with Compatibility ends at an uncompiled entry, the page offers *Rebuild with File-first*.

How it works, in one paragraph: the build runs the console from reset (a new console's empty backup RAM, **no controller input**) and watches which code the CPUs
execute. When a CPU stops at code in RAM that the build has not compiled, the build asks which part of your disc that RAM is a byte-for-byte copy of, among
only the disc blocks the console itself read (so a coincidental match elsewhere on the disc is never used); an unambiguous, long enough match becomes an
executable region of the program and the pass repeats. Code that is **generated, decompressed or modified at run time**, or that matches the disc in more than one
place, is refused with a reason and never guessed: the build finishes, says where the program stops, and the program ends with a typed stop at that point.
In Compatibility mode no button is pressed, so a title that waits for START is covered up to its first menu and code behind input the build never saw stops with
`entry_not_compiled`; File-first can precompile eligible disc files that code lives in.

## What "verified" does and does not mean

Verified: the BIOS and CUE files are the ones the game was added with; the program file is unchanged since import; the program states that it was built
for exactly this BIOS and these disc files, and each of those files has the stated size and SHA-256; it is a viewer build of the runtime contract this
launcher speaks. **Not verified**: that the program is a faithful translation of the disc (nothing signs it) - the launcher only runs programs you chose.

## Not supported

* **Audio validation:** the main-side Z80 driver executes and the shared YM2612/PSG are mixed in since SEG-115 (ADR 0259), but that output is not reference-checked, not verified by listening, its level against CD-DA/PCM is not established, and it trails the other sources by two output frames. The RF5C164 PCM output is a deterministic 44.1 kHz zero-order-hold conversion, not a model of the analog output stage. CD-DA tracks other than 2,352-byte BIN audio (compressed or WAV-backed tracks) are not supported.
* **Saves:** only the **internal** backup RAM (8 KiB per game) is persisted, stored by the launcher as protected user data (ADR 0252). A RAM cartridge is not supported. Whether a given title's own save workflow works end to end is not claimed.
* **Complete compilation of every title:** code that is generated or decompressed at run time, code reached only after input the build never saw, computed jump destinations the build never observed and any code region a program's build did not admit stop with a typed stop. Automatic discovery and File-first compilation are bounded by what the build observed and by ISO9660 files of at most 512 KiB each.
* 6-button pads and player 2 input in the Sega CD viewer; other titles' compatibility.
