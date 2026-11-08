# Save states, game saves and rewind (Genesis, Master System, Sega CD)

Design: [ADR 0278](../decisions/0278-genesis-save-states-persistence-and-rewind.md) (Genesis) and [ADR 0279](../decisions/0279-master-system-and-sega-cd-save-states-and-rewind.md) (Master System and Sega CD); architecture contracts:
[genesis-save-states-persistence-and-rewind.md](../architecture/genesis-save-states-persistence-and-rewind.md) and
[sms-sega-cd-save-states-inventory.md](../architecture/sms-sega-cd-save-states-inventory.md).

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
* **Master System** (generated-native programs started from the launcher): five numbered states plus a quick slot and rewind, with the same menu, keys and defaults. **Cartridge RAM is part of every state but
  is not saved between sessions**: the supported board declarations (the Sega mapper and the plain 32 KiB ROM) cannot tell a battery-backed cartridge from one that simply maps RAM, and a false save is worse than
  none. A state is bound to the cartridge image **and the mapper you declared**: a state of one mapping never loads under another.
* **Sega CD** (a game started from the launcher with a normal play session): five numbered states plus a quick slot and rewind. A state is bound to the exact **disc**, the exact **BIOS image** and the
  generated program's executable-image set; the game's internal backup RAM is part of the state **in memory only** and its file lifecycle ([ADR 0252](../decisions/0252-seg-108-m12-persistent-sega-cd-backup-ram.md)) is unchanged
  (see "Sega CD" below).
* Cartridge memory is supported for the cartridges Segarecomp already emulates: header-declared byte-wide SRAM and the Sega-board X24C01 serial EEPROM (Genesis). No new chip is guessed from a title.
* **Not offered, with the reason shown in the menu:** a session that records input or replays a script (a restore would rewrite the history the recording describes), a session with no save folder
  (a program started outside the launcher), and a Sega CD program built with the main-side Z80 but without its sound devices, or a diagnostic/build session.

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
* F5, F7 and F8 are host keys like F3, F11 and F12: they cannot be bound to a game button. **F7 is rewind on every console.** The Sega CD viewer's frame step moved from F7 to **F9** (F6 still holds the
  machine, F9 steps one frame while it is held); neither key can trigger the other.

## Master System and Sega CD in detail

### Master System

* **What a state holds:** the Z80 (registers, interrupt state, T-state counter), work RAM, cartridge RAM, the mapper and memory-control registers, the scanline scheduler position, the controller and Pause state, the VDP (VRAM, CRAM,
  registers, the control-port latch, the flags and counters), the renderer's frame in progress and last published frame, and the SN76489 with its sample-clock counters. Loading continues **exactly**: the same game behaviour and the same
  sound samples (checked from every frame boundary of a fixture run).
* **Where it lives:** `<library>/saves/master-system/<64-hex SHA-256 of the cartridge image>/<mapper>/states/{quick,slot-1,...,slot-5}.sgstate`. A different mapper declaration of the same image is a different folder, and the state itself
  also carries the mapper, the ROM size and the PSG variant, so a copied file is refused too.
* **After a load** queued audio is dropped, pacing restarts, held keys are released, and the widescreen extension is produced again by the next frame (so the first frame after a load is shown in Original).
* **Limits:** no game save file (see above); `--viewer-record` sessions have no states.

### Sega CD

* **What a state holds:** both MC68000 processors with their timing and interrupt state, main work RAM, sub PRG RAM and Word RAM, the Gate Array (ownership, banks, communication registers, timer), the CD controller (buffer, a transfer
  in progress), the CD drive (command and status, position, latency), the RF5C164 PCM chip, CD-DA playback and the audio mixer's clocks and queue, the graphics ASIC, the in-memory backup RAM, the live pad, executable-image selection and
  bindings, the main-side VDP / I-O and, when the program has them, the Z80, YM2612, PSG and mixer. The disc and the BIOS are **not** copied: the open, verified ones stay attached and the state records which they are.
* **Where it lives:** `<library>/sega-cd/<game id>/states/{quick,slot-1,...,slot-5}.sgstate`, next to the game's backup RAM and recordings, which Segarecomp never deletes.
* **A state belongs to one disc, one BIOS and one executable-image set.** A state of another disc or BIOS, of a program built from a different set of images, or a damaged or crafted file is refused with the reason, without touching the
  running game. Rebuilding the same game with compatible code keeps its states.
* **Heads-up:** loading an older state or rewinding rolls the in-memory backup RAM back too, and a clean exit then stores THAT timeline as the game's save (the previous generation is kept as `.prev`). Save in the game as you normally would before experimenting.
* **Backup RAM:** a state contains the game's backup RAM as it was; **loading a state or rewinding changes only the copy in memory**. The file in the game's folder is written, as before, only when a normal session **ends cleanly**
  (through the same candidate-and-rename step and `.prev` generation as ADR 0252), and it then holds the timeline you ended the session on. Browsing and cancelling the rewind screen, a refused state and a failed save never write it.
  A state saved in a session that has backup RAM is refused by one that does not, so a state can never hand a session memory it could not store.
* **Rewind span:** a Sega CD moment is about 1.1 MiB (the machine has 832 KiB of RAM and a 64 KiB PCM memory), so the 96 MiB default holds about 86 moments: **about 43 seconds** at the default 500 ms cadence rather than 60. The rewind screen
  says how many seconds are really kept. Nothing is compressed to reach 60 s.
* **After a load** the host audio queue and the CD-DA staging buffer are dropped, the audio presenter and pacing restart, and held keys are released; the machine's own audio state (mixer queue, drive, chips) is the restored one, so
  the music continues from where the state was saved.
* **Not offered:** replay and "No window" replay sessions, recording sessions, and builds made for diagnostics.

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
  this feature, a snapshot is about 150-180 KB (182,509 bytes for a patched Sonic the Hedgehog), so 120 moments are about **22 MiB** (measured 20.9 MiB on the synthetic workload); a game with larger cartridge memory needs more. If the memory limit is what bounds the history, the
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
| Opening the rewind surface (about 120 moments listed) | about one 16 ms frame |
| One surface frame while the preview switches (software renderer) | median 2.3 / 4.4 / 9.0 / 19.9 ms at 720x520 / 1080x740 / 1440x1040 / 2560x1440 (a GPU renderer is faster) |

Reproduce: `tests/tools/genesis_rewind_harness.c` (`bench`), `tools/genesis_state_perf.py` and `sv_state_bench`.

### Master System and Sega CD (SEG-132)

Observed in the automated tests on Linux x86-64 (software renderer, unthrottled, a shared CI-class machine that was also compiling): values, not guarantees.

| Measure | Master System (`state_counter` fixture) | Sega CD (CD-DA fixture) |
| --- | --- | --- |
| One snapshot | 205,675 bytes (with a 149x111 thumbnail) | 1,168,442 bytes (1,141,326 without a thumbnail, with the Z80, YM2612, PSG and mixer) |
| Capture inside the viewer (state + thumbnail) | mean 0.73 ms, worst 1.3 ms | mean 4.2 ms (9.4 ms while the machine was busy compiling), worst 4.5 ms (37 ms under that load); about 1 % of run time at two per second |
| History at the defaults (500 ms, 96 MiB) | 120 moments in about 25 MiB: the full 60 s | 86 moments in 95.8 MiB: **42.6 s** retained after 70 s of play, 54 evictions |

Reproduce: `tests/sms_viewer_states_test.py`, `tests/sega_cd_viewer_states_test.py` (the `VIEWER_REWIND_STATS` line) and a long run of the viewer with `SEGARECOMP_REWIND_INTERVAL_MS` unset.
