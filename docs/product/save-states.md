# Save states, game saves and rewind (Genesis)

Design: [ADR 0278](../decisions/0278-genesis-save-states-persistence-and-rewind.md); architecture contract:
[genesis-save-states-persistence-and-rewind.md](../architecture/genesis-save-states-persistence-and-rewind.md).

## Three different things

| | What it is | When it is written | Where it lives |
| --- | --- | --- | --- |
| **Game save** (battery memory) | the cartridge's own SRAM, or the 128-byte EEPROM of the cartridges that have one: what the game itself saves | automatically, when **you** end the session (Return to Launcher / Quit / close the window) | `cartridge.srm` / `cartridge.eep` |
| **Save state** | a snapshot of the **whole machine**: CPU, memory, video, sound chips, the cartridge memory | only when you choose it: *Save State* in the pause menu, or **F5** | `states/quick.sgstate`, `states/slot-1.sgstate` ... `slot-5.sgstate` |
| **Rewind** | the last minute of play, kept **in memory only**, as pictures you can go back to | never written to disk | nowhere (it is gone when the game closes) |

A save state is **not** the game's own save and does not replace it: a state also captures the position of the game, music and everything in flight, and loading it is always your explicit
choice. **A state is never loaded when a game starts.**

## What is supported

* **Genesis / Mega Drive** (generated-native programs started from the launcher): game saves, five numbered states plus a quick slot, rewind.
* **Master System** and **Sega CD**: no whole-machine states and no rewind yet. The pause menu still shows *Save State*, *Load State* and *Rewind*, greyed out with that reason. A Sega CD game keeps
  saving its internal backup RAM as before ([ADR 0252](../decisions/0252-seg-108-m12-persistent-sega-cd-backup-ram.md)); nothing about that changed.
* Cartridge memory is supported for the cartridges Segarecomp already emulates: header-declared byte-wide SRAM and the Sega-board X24C01 serial EEPROM. No new chip is guessed from a title.

## How to use it

| Action | Keyboard | Gamepad | Mouse |
| --- | --- | --- | --- |
| Quick save / quick load | **F5** / **F8** | pause menu | pause menu |
| Pause menu: *Save State*, *Load State*, *Rewind* | **Esc** | **Back / Select** | |
| Open rewind | **F7** (or *Rewind* in the pause menu) | pause menu, or hold **both analog triggers** for about a third of a second | |
| Go back / forward one moment | **Left / Right** | D-pad or left stick | click a picture |
| Jump five moments | **Page Up / Page Down** (Home / End: oldest / now) | left / right shoulder buttons | mouse wheel steps one |
| Restore the selected moment | **Enter** | the **south** face button (**A** on Xbox pads, **B** on Nintendo pads) | the *Restore* button |
| Leave without changing anything | **Esc** or **F7** | the **east** face button / **Back** | the *Cancel* button |

* **Save State** shows the quick slot and slots 1-5 with the picture and the play time of the state in each. Choosing an occupied slot asks before it overwrites it, with *No* selected.
* **Load State** shows the same list; an empty slot is greyed out. Choosing one asks before it replaces the running game, with *No* selected, and then you are straight back in the game with a short
  *State loaded - Slot 2* message.
* **Rewind** freezes the game and shrinks the picture into a preview. Below it is a strip of real pictures from the last moments, oldest on the left and **NOW** on the right, labelled with how long ago
  they were (`-3.5s`, `-12s`, `-1:05`; guest time, so a pause in between does not count). The preview follows your selection at once. **Nothing runs and nothing is written to disk while you look.**
  Choosing a moment restores the machine to exactly that state and the game continues from there as a new future; the moments after it are forgotten. Cancelling changes nothing.
* The two-trigger shortcut is a deliberate chord, held briefly: it never reaches the game. If your controller does not report triggers reliably, use the pause menu.
* F5, F7 and F8 are host keys like F3, F11 and F12: they cannot be bound to a game button.

## Where your data lives

In the library folder (the same place as your library and settings: `SEGARECOMP_LIBRARY_DIR`, otherwise the per-user data folder), **never** in the disposable build cache:

```
<library>/saves/genesis/<64-hex SHA-256 of the original game image>/
    vanilla/                      cartridge.srm  cartridge.eep  states/{quick,slot-1,...,slot-5}.sgstate
    patches/<patch identity>/     the same, for that exact patched version
```

* Each game is identified by the full digest of its image, each patched version by the patch package's identity. **A patched version never shares saves or states with the plain game** by default,
  because a patch changes the program's behaviour and an old state could be wrong for it.
* Build mode changes, rebuilds, recompiling, re-importing a game and every cache cleanup leave these folders alone, and the launcher's **Storage** view counts them as your data. Removing a game from the
  library keeps them too; adding the same game again finds them. Segarecomp never deletes a save or a state.
* To back up or move a game's data, copy its folder.

## Game saves: what is guaranteed and what is not

* The session's data is written when you **end the session yourself**. A game that stops with an error, hits an instruction limit, crashes or is killed does **not** overwrite your previous save: that run's
  progress is not stored, and the launcher says so.
* The new data is written next to the old file, flushed, checked, and then swapped in with one rename; the previous generation is kept as `cartridge.srm.prev` (or `.eep.prev`). This is **not** a promise
  that data survives a power cut at the worst possible instant.
* A save file of the wrong size, a directory or a symbolic link in its place, or an unreadable file is **refused before the game runs**, with a message, and is left exactly as it was. A zero-filled save is
  a valid save and is loaded as it is. A missing file starts the game with its documented blank memory.
* Replays, recordings and the build process never read or modify these files.

## Save states: what is guaranteed and what is not

* A state holds the CPUs, work memory, video memory and registers (including a DMA in progress), the Z80, the PSG, the YM2612 and the mixer, the controller and interrupt state, the cartridge memory and the
  EEPROM's serial protocol position. Loading it continues **exactly** from there: the same game behaviour and the same sound, sample for sample.
* A state belongs to **one game image and one version** and to the machine layout it was made with. Loading refuses, with the reason shown, a state from another game or patch, another console region, a
  different feature set, a damaged or truncated file, or a file that is too large (4 MiB), **without touching the running game**.
* A state made by an older or newer Segarecomp is loaded only if its format version is the same; there is no conversion yet. A *rebuild* of the same game (a new build directory, a newer compile of the same
  program) keeps its states usable. If a state names Z80 sound code that a build does not contain, it is refused.
* Saving writes a complete candidate next to the slot, checks it, then swaps it in: a failed save (full disk, no permission) **never damages the previous state** and is reported as a failure, never as a success.
* A state's integrity is checked, but it is **not authenticated**: you can edit your own state file, just as you can edit a save. Segarecomp validates every value it needs before using it.
* Loading a state (or rewinding) puts the cartridge memory **in the running game** back as it was at that moment; if you then quit normally, that older memory becomes the new game save (the file written just before is kept as `.prev`). Two copies of the same game running at once are not supported: the later quit wins.
* Loading a state restores the cartridge memory **in the running game**, as part of the machine. It does not write `cartridge.srm` / `cartridge.eep`: those change only when a session ends cleanly.

## Rewind: limits and battery memory

* By default a snapshot is taken every **500 ms of game time**, up to **60 seconds** back (about 120 moments), using at most **96 MiB**; the oldest moments are dropped first. On the synthetic workload measured for
  this feature, a snapshot is about 150-180 KB (182,509 bytes for a patched Sonic the Hedgehog), so 120 moments are about **22 MiB** (measured 21.9 MiB on the synthetic workload); a game with larger cartridge memory needs more. If the memory limit is what bounds the history, the
  rewind screen says how many seconds are really kept. If a single moment cannot fit the limit at all, rewind is switched off with the reason and the game is unaffected.
* Rewinding restores the **cartridge memory as it was at that moment**, including SRAM and EEPROM, in the running game. Your game-save **files** change only when a session ends cleanly, so browsing, cancelling
  and rewinding never touch the disk.
* Developer / test knobs (environment): `SEGARECOMP_REWIND=0` turns rewind off; `SEGARECOMP_REWIND_BUDGET_MB`, `SEGARECOMP_REWIND_INTERVAL_MS` and `SEGARECOMP_REWIND_TARGET_MS` bound it.
* Presentation settings (fullscreen, scaling, CRT effect, widescreen mode, volume) are not part of a state or a rewind and are never reset by one. A restored picture is drawn from the restored video state;
  a widescreen extension is produced again by the next frame, never carried over.
* Native patch providers ([native-patch-sdk.md](native-patch-sdk.md)) are read-only by construction: they cannot change what the game does, so their state is not part of a snapshot, and a state works
  the same with or without one.

## If something goes wrong

* *Save failed: ...* - the previous state is still there. Free disk space or fix the folder's permissions and save again.
* *Load failed: ...* - the game is exactly as it was. The reason tells you whether the file is damaged, belongs to another game or version, or does not match this build.
* *The game was not started: its save file was refused* - the file in `saves/genesis/.../vanilla/` is the wrong size or not a plain file. Move it away or restore `cartridge.srm.prev` / `cartridge.eep.prev`,
  or a backup of the folder; Segarecomp will not overwrite it.
* *Your game data could not be saved ... The previous save was kept.* - the session ended normally but the new save could not replace the old one (disk full, no permission, the file is locked). Free space and play
  and quit again; the previous data (`.prev` included) is intact.

## What a rewind picture shows

Each moment's picture is the **latest completed frame** the game had published at the instant the snapshot was taken. A frame is published when the game's video timing crosses a frame boundary, and a snapshot is taken between two
execution slices, so the picture can be at most the frame in progress behind the saved machine state (up to about one frame, 16.7 ms of game time, plus the part of a slice). This is a bound from how the frames are produced, not a
measured per-frame comparison. After a restore the picture shown is drawn from the restored video state, and the game's next frame is its own.

## Measurements recorded for this feature

On a synthetic generated-native 68000 + Z80 program (PSG, FM and DAC playing), Linux x86-64, `-O2`, observed values, not guarantees:

| Measure | Result |
| --- | --- |
| Snapshot capture (no thumbnail) | median 0.42 ms, worst 0.71 ms (512 runs); 146,669 bytes |
| Snapshot restore | median 0.42 ms, worst 0.71 ms |
| Interactive play, rewind **off** vs **on with the shipped defaults** (20 guest seconds, unthrottled, software renderer, 5 runs; `tools/genesis_state_perf.py`) | 4.51 s vs 4.68 s median (+3.8 %, run-to-run ranges 4.30-4.61 s and 4.38-4.82 s overlap); the viewer observed exactly 40 snapshots per run |
| Retained history, observed by the viewer with the defaults | 10 s: 20 moments, 3.5 MiB; 30 s: 60 moments, 10.4 MiB; 60 s: 120 moments, 20.9 MiB, no eviction |
| Capture latency inside the viewer (snapshot + thumbnail), observed | mean 0.77 ms, worst 0.90 ms per snapshot (about 0.7 % of run time at two per second) |
| Rewind then resume | a confirmed rewind restores once and the session runs on to a normal end (checked in `genesis_rewind_session_test`, `genesis_production_states_test` and by the perf tool) |
| Real game: widescreen-patched Sonic the Hedgehog (native provider), 3,600 guest frames unthrottled, software renderer, 3 runs each (measured earlier with a 10 s cadence and a 500 ms cadence) | rewind off 17.9-19.8 s; rewind on 17.9-19.9 s (10 s) and 18.1-19.9 s (500 ms): no difference beyond run-to-run noise; state file 182,509 bytes, so 120 moments are about 21 MiB |
| Opening the rewind surface (119 moments listed) | about one 16 ms frame |
| One surface frame while the preview switches (software renderer) | median 2.3 / 4.4 / 9.0 / 19.9 ms at 720x520 / 1080x740 / 1440x1040 / 2560x1440 (a GPU renderer is faster) |

Reproduce: `tests/tools/genesis_rewind_harness.c` (`bench`), `tools/genesis_state_perf.py` and `sv_state_bench`.
