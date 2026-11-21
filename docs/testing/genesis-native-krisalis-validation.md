# Native Krisalis replacement - validation (ADR 0318)

Commercial images are used locally and ephemerally (`docs/testing/commercial-games.md`): nothing below is a byte, address, trace, asset or disassembly; it is counts, classes and pass / fail.
CI never depends on them. The committed tests use synthetic data only (section 7).

## 1. Identification (aggregates; the per-image matrix stays private)

Every local image of the family (Chuck Rock (USA), Mickey Mania (E), The Lion King (UEJ), Boogerman (U), Sensible Soccer (E) and its International Edition) uploads **three** Z80 images: a small program shared by
all of them (one signature), a 27-byte boot stub (revision 1 selects ROM bank 2, revisions 2/3 bank 0) and the driver (one signature per game: the 5 KiB block it copies is the code plus whatever follows it in that game's ROM).

| Game | Driver code (byte-identical, over its real length, to the published reverse-engineered revision; offline, private) | Revision | Evidence tier | Mailbox |
| --- | --- | --- | --- | --- |
| Chuck Rock | `$694` bytes = revision 1 | **1** | exact code identity | `$0E58` |
| Mickey Mania | `$C64` bytes = revision 3 | **3** | exact code identity | `$0FAC` |
| The Lion King (UEJ) | `$C64` bytes = revision 3 (the same code image as Mickey Mania's) | **3** | exact code identity | `$0FAC` |
| Boogerman (U) | `$C64` bytes = revision 3 | **3** | exact code identity | `$0FAC` |
| Sensible Soccer (E) and (E) Intl. | `$A97` bytes = revision 2 | 2 (not implemented) | exact code identity | `$0F58` |

The Lion King is **not** a revision of its own and not "nearest": its code image is revision 3, byte for byte. Its failure mode (the original path isolated at frame 81 by a structural code mismatch) is the revision-3 driver's own
self-modification (operand patches and the vector-area helpers). Whether two games share a binary was checked, not assumed: five different driver signatures, three code images.

**Baseline failure (default build before this change), all six images:** `genesis_audio = degraded / structural_code_mismatch`, Z80 isolated at virtual frame 6 / 5 / 17 / 81 / 36 / 36 (Chuck Rock, Mickey Mania, Boogerman, The Lion King, Sensible Soccer x2),
no structural alternative form for any image (`skipped_no_admissible_forms`: the new pre-check of this branch's base, no second pass), no replacement. The 68000 never stops on a VDP or CPU fault there: it spins on the mailbox command byte it wrote (a bus request / release pulse per iteration:
1.64 M / 1.46 M / 1.52 M pulses for Chuck Rock / The Lion King / Mickey Mania in a bounded run) because the isolated Z80 never clears it. Frames: Chuck Rock frozen on its title (frame digest identical at every sample after frame 1000), Mickey Mania and The Lion King
frozen on a blank frame (identical digests across the whole run). Boogerman stops at virtual frame 18 on an unrelated 68000 discovery frontier before it ever speaks to the Z80 (section 6).

## 2. What is compared with what

The reference is the **original driver executed by an independent emulator** (the third-party `z80` Python package, installed locally, never vendored or linked): the image is rebuilt from the 68000's own stores that a real run logged
(`SEGARECOMP_AUDIO_WRITE_LOG` with `SEGARECOMP_AUDIO_WRITE_LOG_RAM=all`), so it is exactly the code that game uploads. `tools/krisalis_reference_check.py` turns the log into a stimulus script - the 68000's bus transactions - and drives **both** implementations through it as a closed loop:
a transaction that stores a command waits (polling the byte with short bus holds, as the 68000's wait loop does) until the implementation under test has cleared the previous one, so neither is fed an impossible stimulus and both execute the **same command sequence**.
`tools/krisalis_replay_harness.c` runs `krisalis_native.c` over the script without the game, so a change of the replacement is measured in seconds. Each of these is a failure, not a tolerance:

1. every command the 68000 committed is consumed by the replacement, once, in order, with the same cells, not before it was written;
2. the register-level YM2612 write sequence (everything except the DAC player's own groups: its data bytes, its DAC-enable and its end-of-sample pan restore) is identical, up to one shared horizon, with no extra or missing write inside it;
3. per sample, the DAC byte stream is the same; where a sample is cut (a new sample, a silence, a note on FM6) the two may differ in **length** by the polls the timing moves across the cut (at most 64 bytes, reported) and by up to 6 bytes at its boundary;
4. the number of samples started agrees.

Time is reported (skew per compared write, percentiles, in master ticks: one frame is 896,040; one Z80 cycle 15), never folded into identity. The largest skew (Mickey Mania, 299,570 ticks, about 0.33 frame) is the cost of the event-level model on its longest commands. **Not claimed:** cycle accuracy, the interleaving of DAC bytes with FM writes (the original polls the DAC from inside its row code in revision 3 and not at all during a row in revision 1; the replacement paces it by an event model),
the order in which an SFX sample start and a tick that are both pending are served (a race of a few hundred Z80 cycles that follows the timing), or real-hardware equivalence.

## 3. Results

| Image (game, revision) | Run | Commands written / consumed | YM2612 register writes compared | DAC bytes compared | Longest DAC length difference | Skew p50 / p95 / p99 (ticks) | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Chuck Rock, rev 1 | about 65 s of game time, title + level 1, scripted Start / D-pad | 3,712 / 3,712 | 18,072 | 454,053 | 42 | 3,391 / 14,900 / 32,530 (max 45,106) | identical |
| Mickey Mania, rev 3 | about 73 s, title, level 1 | 2,177 / 2,177 | 11,794 | 42,147 | 51 | 14,377 / 55,148 / 168,306 (max 299,570) | identical |
| The Lion King, rev 3 | its 288 frames (it stops, section 6) | 5 / 5 | all | all | 0 | 9,720 / 28,444 / 43,279 | identical |

Each earlier version of the replacement failed this check and each failure was a defect of the replacement or of the comparison, fixed: a missing wrap condition in the pitch arithmetic (found by Mickey Mania), a sample start applied before the register writes that precede it in the original (found by Chuck Rock and Mickey Mania:
the DAC start and an FM6 note's sample cut are now effects of the command, due at their point in the row), a DAC poll splitting a register pair or the nine bank-register writes (the original never polls inside them), and, decisively, the wait loop's phase across the 68000's bus holds (Decision 3 of the ADR).
The Boogerman image has no run of its own (it stops before the sound code is reached) and The Lion King's is five commands: both are `build_qualified` (identity checked, compiles and runs), `reference_checked` is claimed only for the two images whose own run was compared.

## 4. Real-game behaviour of the default build (no option; the automatic policy)

`segarecomp build` of Chuck Rock records `native_driver / automatic / original_aot_failure_verified_replacement`, `driver = krisalis`, `version = 1`, `version_evidence = published_code_identity`, `verification = reference_checked`, the original AOT's failure (`structural_code_mismatch`, frame 6) as evidence, and takes two passes.

| Game | Before | After (bounded headless runs, frame-keyed pad script, frame captures viewed) |
| --- | --- | --- |
| Chuck Rock | title frozen from frame 1000; 68000 in the sound wait loop | title with music; **Start** presses enter level 1 (playfield, HUD, animated sprites); scripted D-pad input moves the player; 4,405 virtual frames, 3,712 commands, 5,148 key-ons, 454,578 DAC samples, PCM non-silent, no fault |
| Mickey Mania | blank frame forever (3,300 frames) | SEGA logo, then the title (**Press Start**), then scripted Start enters the first level (Steamboat Willie) and runs; 4,402 frames, 2,177 commands, 1,700 key-ons, 42,226 DAC samples, non-silent, no fault |
| The Lion King | black frame forever | SEGA logo shown, the 68000 starts its music (5 commands, 12 key-ons) and then **stops at frame 288 on an independent 68000 discovery frontier** (section 6): not a sound fault |
| Boogerman | stop at frame 18 | unchanged: the same frontier family, reached before any sound |
| Sensible Soccer | degraded as before | unchanged (revision 2 is not claimed) |

Repeatability: two runs of the same scripted route give the same PCM SHA-256 (Chuck Rock and Mickey Mania). The build's own start-up run records `genesis_audio = native` and no unsupported operation.

## 5. Save states and rewind

Driver level (`tests/krisalis_native_test.c`, committed): a state taken with effects pending is restored into a different running driver and the continuation is exactly equal (every chip write with port, value and time); an empty, truncated, over-long, unknown-revision or future-clock payload is refused with the live driver untouched; a faulted
driver has no state. Machine level (local, `tools/genesis_state_demo.py`, **the default automatic build**): Chuck Rock, state taken while the replacement plays (146,842 bytes, 527 commands, 98,686 DAC samples so far), restored state equal to the saved one, the following 77,462 DAC samples / 407 commands / 299,395 mixer frames **identical** after the first and the second restore, non-silent.
The viewer's save / load / rewind uses the same service. The payload limit was raised from 4 to 6 KiB (the GEMS payload is unchanged; `genesis_native_snapshot_test` is unchanged).

## 6. Remaining limits (separated from what is done)

* **The Lion King and Boogerman do not reach gameplay.** After the sound handshake is satisfied both stop with `unresolved_indirect_target / reached_unresolved_direct_edge` in the revision-3 68000 sound module's entry (`lsl.w #2,d7` / `jmp table(pc,d7.w)` over a run of `bra.w` slots, index a caller constant). This is the 68000 static discovery's
  unproven target set for an indexed computed jump; it is not a sound fault, and the project's strict extent policy (ADR 0054) deliberately does not admit "a run of slots" as proof. Recorded as follow-up SEG-151-T002 (the generic slot-run admission with the runtime membership guard, or the trusted table descriptor). It was identified, not masked.
* Revision 2 (Sensible Soccer) is analysed, not implemented, not in the catalog.
* Event-level timing; the oracle is an emulator running the published original, not a hardware capture; audibility of a play session is not claimed.
* Not run locally: the full CTest gate and the launcher (SDL3) tests, which are CI's.

## 7. Committed tests (synthetic data only)

`krisalis_native_tests`: start-up (the mailbox address published at `$0004`, the vector, LFO / timer initialisation), the acknowledge protocol of both revisions (read-time versus done-time), a note with a voice load (26 registers in register-table order, pitch, key-off then key-on),
the channel to part mapping and the key code, carrier attenuation with its clamp, an SFX sample (nine bank-register writes, FM6 pan, DAC enable, the eight bytes between the skipped head and tail, DAC off, flag cleared), a sample outside the cartridge (typed fault, silence, no state), the effect of a bus hold, cell commands (`$7D`, `$7B`), the state codec and its refusals, the catalog.
`krisalis_reference_check_test.py`: the detection power of section 2 on synthetic logs (changed value, premature stop, extra / missing write, wrong DAC byte, short sample, a sample never started, a missed / invented / altered / early command, missing end marker) and the script round trip.
`genesis_native_gems_build_test` (extended): a verified Krisalis identity of the **faulting** image selects the replacement automatically and records the revision, the image and the original's failure; an image both catalogs name stays GEMS (precedence); a verified identity alone changes nothing; an unlisted image and `--sound-driver krisalis` without identity
are refused with `sound_driver_unverified` and no executable; a Master System build rejects the option. `launcher_policy_test` (extended; run by the SDL3 jobs): the Krisalis record, wording and cache identity.

## 8. Reproduce locally

```
segarecomp build --rom <game>.md --output out --cc cc --runtime-dir platforms/genesis          # default policy: original first, then the verified replacement
SEGARECOMP_AUDIO_WRITE_LOG=run.log SEGARECOMP_AUDIO_WRITE_LOG_RAM=all out/game --instruction-budget 60000000
python3 tools/krisalis_reference_check.py --rom <game>.md --revision N --game-log run.log --script run.script
cc -std=c11 -O2 -Iplatforms/genesis/runtime -o harness tools/krisalis_replay_harness.c platforms/genesis/runtime/krisalis_native.c
./harness run.script <game>.md > replay.log
python3 -m venv v && v/bin/pip install z80 && v/bin/python tools/krisalis_reference_check.py --rom <game>.md --revision N --script run.script --native-log replay.log
python3 tools/genesis_state_demo.py --rom <game>.md --work demo        # save / load while the driver plays
```
