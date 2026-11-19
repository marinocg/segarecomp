# Recording and replaying a play session

*SEG-146-T006.* A player (or a developer) can record a real play session in the Genesis viewer and replay it deterministically later: against the original game,
against a presentation-only package, or against any gameplay-changing variant. Recorded sessions are the gameplay input of acceptance tests (they replace hand-written
pad scripts and bots). The recorder is shared code (`libs/viewer/include/sv_session.h`, no SDL); the Genesis adapter is `platforms/genesis/viewer/genesis_session.h`.
Sega CD keeps its own recorder (see [sega-cd-play.md](sega-cd-play.md)); Master System is not wired yet (the shared code does not preclude it).

## Using it

**Launcher window (Genesis games).** Open the game's Details page: *Input recording* has the switch "Record my next play session from power-on when I press Play", the list of this game's
recordings (newest six) with a **Replay** button each, and "Replay even if the game version differs". In the game, **Esc > Stop recording** ends a recording (quitting does too).
Recordings live under the library folder, `sessions/genesis/<first 16 digits of the image digest>/session-<time>.rec`.

**Launcher command line.**

```text
Segarecomp --build game.bin --run --record session.rec
Segarecomp --build game.bin --run --replay session.rec [--allow-variant]
```

(Genesis only; a Master System game is refused: the shared code is not wired there yet.) **Bridge tool:**

```text
# record from power-on (viewer); finish with  Pause menu > Stop recording,  or just quit
genesis_startup_bridge.py --rom game.bin --viewer --record session.rec
# replay in the viewer (it closes by itself at the end of the recording)
genesis_startup_bridge.py --rom game.bin --viewer --replay session.rec
# replay headless with the SAME file; the checkpoint digests are compared as it runs. The replay always runs to the end marker (the capture window only
# selects which frames are written); if the game stops or the budget runs out first it is reported as incomplete (exit status 6)
genesis_startup_bridge.py --rom game.bin --capture-frames 1:1 --replay session.rec
# replay against another effective image / package: the mismatch is reported and the replay continues
genesis_startup_bridge.py --rom patched.bin --viewer --replay session.rec --allow-variant
```

The options travel as `SEGARECOMP_RECORD`, `SEGARECOMP_REPLAY` and `SEGARECOMP_ALLOW_VARIANT` (the generated program's argument list is unchanged), so a launcher can
set them directly. The session binds to the **effective image** identity in `SEGARECOMP_ROM_SHA256` (the launcher passes its verified, possibly patched, image) and
the package identity in `SEGARECOMP_SAVE_VARIANT`; the bridge computes the image hash itself when none is given.

While a session is recorded or replayed **saves, save states, quick-load and rewind are off** (the pause menu says so) and the cartridge save file is neither read nor
written: a session always starts from power-on, so no frame number can ever be reused. Saving and loading states *inside* a recording is deferred.

Messages you can see: `recording stopped at frame N: the 4000000-event capacity was reached (...)`, `the recording has no end marker: it was not finished ... cannot be replayed`,
`the recording was made on a different image or version (...); pass --allow-variant to replay it anyway`, `replay diverged from the recording at frame N (checkpoint digest mismatch)`.
A diverged, failed or (headless) incomplete session makes the program exit with status 6.

## Format `segarecomp-session 1`

ASCII, LF (CR tolerated), `#` starts a comment line.

```text
segarecomp-session 1
platform genesis
image <64 hex>               effective image SHA-256
variant <64 hex | vanilla>   package identity
digest <profile>             which machine digest the checkpoints use (see Determinism): machine-<flags hex> or basic
<frame> <player> <mask>      player 1|2, mask 0..255 decimal; that pad is held from <frame> on (pads start released)
checkpoint <frame> <64 hex>  at power-on and every 600 guest frames; strictly increasing frames, none beyond the end
end <frame>                  explicit end, on a guest-frame boundary: a file without it is refused
```

* Entries are written only when a pad **changes**, in non-decreasing frame order. `<frame>` is the guest's virtual frame counter at the boundary where the pad becomes
  visible to the guest.
* Every line is streamed and flushed, so a crash leaves a valid prefix - which replay refuses because it has no end marker (the duration is unknown).
* Capacity: at most 4,000,000 pad changes and 400,000 checkpoints. Reaching the recording cap **ends the recording with an explicit marker and an error message** (the
  prefix up to that frame is complete and replayable); nothing is truncated silently. Lines are at most 160 bytes.
* A recording whose checkpoints are out of order, duplicated, or beyond the end marker is refused; a replay that passes a checkpoint without comparing it, or ends before all of them were
  compared, fails. A checkpoint that cannot be written ends the recording with an error and no end marker.
* A platform mismatch is always refused. An image or package mismatch is refused unless `--allow-variant`; with it the mismatch is printed and checkpoint digests are
  informational only (a gameplay-changing variant is expected to diverge).

## Determinism

The pad is stamped / applied at the **guest-frame boundary** through the runtime's frame-hook seam, before the guest executes another instruction of the new frame, and
the guest reads that latched value (the runtime's `mod_pad_*` override) for the whole frame. Host polls between boundaries (the viewer samples once per slice) cannot
change what the guest sees. If a native mod scheduler owns the hooks it is wrapped (its input phase sees the recorded physical pad, exactly as live; the file stores the physical pad).

**Exact end.** A session ends on a boundary. A replay asks the runner to yield right after the dispatch that crosses its end marker (`GenesisRuntime::host_yield_request`), so no
instruction beyond that boundary runs whatever the host's slice size. A recording made with Stop recording ends at the next boundary; a recording ended by quitting ends at the last
boundary reached (what ran after it is not part of the recording).

**Checkpoints.** At each boundary the order is: stamp/apply this frame's pads, take the digest, then (maybe) end. The digest of a viewer or a sound/mod headless program is the SHA-256 of the
whole-machine snapshot container (`genesis_session_machine.h`): CPU, work RAM, VDP (VRAM/CRAM/VSRAM/registers), I/O latches, cartridge SRAM/EEPROM, Z80, PSG, YM2612, mixer and mod state, in the
same build-independent encoding save states use (profile `machine-<flags>`). A host that does not link the snapshot codec (the Python bridge's plain headless build) uses the `basic`
profile - work RAM, guest time and frame counter only; checkpoints are enforced only between identical profiles, otherwise the mismatch is reported and the replay is not judged by them.

**Evidence.** `genesis_session_tests` shows the whole runtime state at the end boundary is byte-identical between the recorded run (random mid-frame host pads, one slicing) and replays at
slices 1, 500, 7919 and 65536, with and without host pads; `genesis_session_machine_tests` shows the same under the whole-machine digest, including around a real input-intercepting mod
scheduler, and that a single flipped VRAM bit is caught by the power-on checkpoint.

## Privacy

A recording contains header hashes, button masks, frame numbers and checkpoint digests only - no image bytes, saves, audio, video, paths or trace data
(`sv_session_test` asserts the file is short printable text). The image hash identifies a game image the way a save-state header already does; treat a recording as
you would a save file.
