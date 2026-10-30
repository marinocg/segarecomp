# Sega CD: playing in the viewer and in the launcher

What works, what does not, and how to get a game into the launcher. Design: [ADR 0204](../decisions/0204-seg-078-sega-cd-interactive-viewer-and-launcher-integration.md);
machine contract: [sega-cd-machine-contract.md](../architecture/sega-cd-machine-contract.md); developer procedure:
[platforms/sega-cd/README.md](../../platforms/sega-cd/README.md).

## Status in one paragraph

Segarecomp can run a **generated-native Sega CD program** (a program compiled ahead of time from your own BIOS image and disc) in the same
window as Genesis and Master System games. Sonic CD (USA) is playable this way with live CD music (CD-DA) but **no sound effects**; its generated execution is also checked against an independent reference at pre-registered checkpoints (title screen, START presses, level start, special-stage attract demo, intro) - checkpoint evidence, **not** a complete-compatibility claim - and a second title has only experimental observations. **Only the CD music (CD-DA) is audible**; there are no sound effects (PCM), FM or PSG sounds.
**Segarecomp can build a program for you from your own BIOS and disc** (the launcher's *Build*, experimental; ADR 0205): it discovers by itself which disc
spans the console loads into RAM and executes, and compiles them. It can run only the code its build proved to come from the disc (anything else stops with a
typed stop such as `entry_not_compiled`), so coverage of a given title is partial until the obstacles listed below are removed. A program built elsewhere with
the developer procedure can still be imported. Everything is yours: the BIOS, the disc and the program are referenced
by path, never copied, never uploaded and never part of a release.

## In the launcher

1. **Import ROMs > Add Sega CD game...** (or `Segarecomp --sega-cd ...` without a window). Choose the BIOS image, the CUE file (the track files must be
   in the same folder) and - if you have one - the verified program (`program-viewer`).
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
   Esc or the gamepad's Back button opens the pause menu; **F6** holds the machine, **F7** steps one frame while held. Closing the window returns to the
   launcher (a clean end).
4. **Record this session** (details page) writes a small text script of your button presses at exact frame boundaries. **Replay** plays it back in the
   viewer; **No window** replays it as fast as possible and reports the result. A replay reproduces the run exactly. The recording is written when the viewer closes (a crash loses it); sessions you do not record have no length limit, a recording is limited to about 2,000 button changes. "No window" stops by itself two seconds of game time after the last recorded button change.

## Building a program (Build)

**Build** (the details page, or `Segarecomp --sega-cd --bios ... --cue ... --build` without a window) runs `segarecomp sega-cd build` with the compiler bundled in the
release. Several minutes the first time (a real disc compiles tens of millions of lines of generated C; later builds reuse the compiled objects), cancellable
at any time (the whole tool tree is stopped and the attempt's files are removed; an earlier finished build is kept). The built program is then verified like an
imported one (it must state that it was built for exactly your BIOS and disc). Progress shows the discovery round and its stages.

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

Other audio (no Z80 execution, no RF5C164 PCM sound effects, no FM or PSG; CD-DA tracks other than 2,352-byte BIN audio - compressed or WAV-backed tracks - are not supported); saving (each run starts from a new console's empty internal backup RAM, which the launcher supplies, and nothing is written back); complete compilation of every title (code that is generated or decompressed at run time, code reached only after input the build never saw, and computed jump destinations the build never observed stop with a typed stop); any code region a program's build did not admit; 6-button pads and
player 2 input in the Sega CD viewer; other titles' compatibility.
