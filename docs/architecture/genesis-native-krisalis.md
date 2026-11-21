# Native Krisalis sound-driver replacement (ADR 0318)

A plain C11 re-implementation of the observable behaviour of the Krisalis (Shaun Hollingworth) Mega Drive sound driver, for games whose uploaded Z80 image the generated-native
Z80 path cannot run (the driver rewrites its own code and copies code into the vector area, ADR 0134). It is **not** an interpreter, decoder or emulator of Z80 instructions: the
uploaded image is never fetched or executed. Sibling of the GEMS replacement (ADR 0291); the two share the machine seam (`GenesisZ80NativeDriver`), the automatic selection and the
save-state contract and nothing else - no sequencer, no tables, no code.

Sources: `platforms/genesis/runtime/krisalis_native.{h,c}`, `krisalis_driver_catalog.inc`, the adapter block in `genesis_sound.c`. Validation: `docs/testing/genesis-native-krisalis-validation.md`.

## 1. The driver family (what the 68000 and the Z80 each do)

The 68000 is the **tracker sequencer** and stays generated-native code. Once per frame it reads the patterns and hands the Z80 either a whole row (six two-byte pattern cells, command
`$0A`) or a tick (command 1) through a mailbox in the sound RAM; the Z80 is a **player** that turns cells into YM2612 writes and plays one 8-bit PCM channel on FM6 through the DAC.
The two CPUs meet only in that mailbox: the 68000 requests the bus, stores the cells and then the command byte, releases the bus and (before its next command) waits until the Z80
has cleared the byte. The 68000 learns where the mailbox is from the Z80's own RAM (`$0004/$0005`, written by the driver at start) and loads a 64-voice bank (32 bytes each) to `$0400`
itself. A game uploads **two** Z80 images in sequence: a 27-byte boot stub (sets the ROM bank register, writes a "driver alive" flag the 68000 polls) and then the driver proper.

Only the second is replaced. The stub is compiled-native and works; the replacement binds to the *driver* image only, which is why a boot stub or any unrelated Z80 program is never touched.

## 2. Identity

The replacement serves an uploaded image only if its **activation signature** (the digest the runtime already computes for every image epoch: the 68000-written extents of the sound RAM and
their bytes) is a line of `krisalis_driver_catalog.inc`. The signature covers the driver bytes **and** the voice bank uploaded with them, so a line names one exact upload; it is not a
title, filename, address or release date and nothing of the kind is compared anywhere. Each line states the published driver revision whose behaviour the image has and the strongest
evidence tier reached for it (`build_qualified`, or `reference_checked` = compared operation by operation with an independent execution of the original driver). An unlisted image is
never served by a replacement (the automatic policy ignores it; a forced `--sound-driver krisalis` fails with a typed `sound_driver_unverified` diagnostic).

Revision evidence is exact: for each catalogued image the driver code the 68000 uploads was compared, byte for byte over its real length, with the code of the corresponding published
reverse-engineered revision (offline, privately; no byte or disassembly is recorded in this repository). The version text is `1`, `2` or `3`.

## 3. Revisions and where they differ

| | Revision 1 | Revision 2 | Revision 3 |
| --- | --- | --- | --- |
| Mailbox / channel state (RAM) | `$0E58` / `$0E27` x 7 bytes | `$0F58` / `$0EE8` x 14 | `$0FAC` / `$0F3C` x 14 |
| Command acknowledge (68000 waits for the byte to clear) | when the work is **done** | when the work is done | when the command is **read** |
| Boot stub bank / sample bank | stub sets bank 2, never changed | per sample (music bank / SFX bank) | per sample |
| DAC player | writes the current byte on every poll, pointer advances by rate/256 | byte on each phase carry (polled only in the wait loop) | byte on each carry, polled from inside the row code |
| Sample start/end | whole sample | skips first 8 / last 4 bytes | skips first 8 / last 4 bytes |
| Cell commands | `$7D` bank flag | `$7A` portamento, `$7B` pan, `$7C` FMS, `$7D` | same |
| Volume / fades | none | music + jingle attenuation | same |
| Slides | add step, no block wrap | add 2x step, wrap `$269..$4D0` | same |

Implemented and verified: **1 and 3** (the supplied games). Revision 2 is analysed and added only after it is verified independently (section 8).

## 4. What the replacement reproduces (the contract)

Commands: 1 tick (per-channel pitch slide / portamento), 2 reset channel state, 3 load voice (never issued by any module; implemented, a channel above 5 faults), 4 silence, 7 reload voices (pause end),
any other (`$0A`) a row. Row: for each of six cells, in order: drum sample (`N = $6D..$74`, FM6 only, priority to an SFX sample), cell commands (`$7A..$7D`), instrument change (voice load: 26 YM2612 registers in
register-table order, carrier attenuation, pan/FMS overrides), note (block / F-number from the published note table, key-off then key-on, legato under portamento), effect nibble (slide), then the tick.
SFX sample trigger (`+16`): bank selection (nine serial bank-register writes), DAC enable, FM6 pan, then the DAC stream; the end of a sample restores FM6's pan from its voice, switches the DAC off and clears the SFX flag.
Every effect is a real chip write through the existing YM2612 device model; nothing is pre-recorded and no ready flag is invented: the mailbox command byte is cleared exactly when the original clears it.

## 5. Timing model

Event-level, deterministic and on the guest master clock (one Z80 cycle = 15 master ticks). The *order* of every chip write is the original's; their *spacing* is a per-revision table (`k_timing`) fitted to an independent execution of the
original images (gaps between the writes of a voice load, a frequency/key write, the wait loop's period). The 68000's repeated short bus holds are modelled by the wait loop's *phase* (a Z80 that stands still in the middle of its loop reaches its command read after the
cumulative running time), and work due during a hold is delayed by it. Not claimed: instruction-cycle accuracy, the busy-flag waits, the original's DAC polling inside the row code (revisions 2/3 pace the DAC by a carry-to-carry model), or a command race inside a row (a row reads its
cells when it starts; the 68000 writes the next row about a frame later).

## 6. Failure semantics

A sample byte outside the owned cartridge image (`unmapped_resource`), an effect queue overflow or a channel number above 5 in command 3 (`invalid_state`), or a revision the build does not have (`unsupported_revision`) faults the driver: it stays silent and the machine records
`GENESIS_DIAG_Z80_NATIVE_DRIVER_FAULT` (visible in the build record and the launcher). No command is acknowledged on behalf of a faulted driver, and there is no fallback driver, interpreter or JIT.

## 7. Build integration and provenance

`segarecomp build` (no option): pass 1 is the original generated-native Z80. If it keeps the sound CPU healthy nothing else happens. If the original isolates the sound CPU by a structural code mismatch **and** the faulting image is in the GEMS catalog, pass 2 builds with GEMS (precedence unchanged);
**else if** it is in the Krisalis catalog, pass 2 builds with the Krisalis replacement (`reason: original_aot_failure_verified_replacement`); else the structural-alternatives pass of ADR 0291 Decision 6 runs when the emitter reports admissible forms, else the build is `degraded` as before. Only one replacement can be
compiled into a program. `--sound-driver krisalis` is a developer override (never offered by the launcher). `status.json` `audio_implementation` records `driver = "krisalis"`, the revision as `version`, `version_evidence = "published_code_identity"`, the identity prefix, the verification tier, the catalog revision,
the implementation version, `save_states = "supported"` and the build's own start-up observation.

## 8. Save states and rewind

The driver's private state (the effect queue, the DAC player, the wait-loop phase, the clocks and the counters) is a canonical, versioned, pointer-free payload (`krisalis_native_state_put/get`, schema 1); the mailbox, the voice bank and the channel state live in the sound RAM, which the machine snapshot already carries, so a re-upload of the driver
resets them exactly as it resets the original. The machine's existing staged-restore contract applies unchanged: decode into private storage, validate against the guest clock, commit atomically, no chip write on restore, a state is refused by a program without this replacement and by a replacement of another driver, version, implementation or signature. The payload bound
(`GENESIS_Z80_NATIVE_STATE_MAX`) was raised from 4 KiB to 6 KiB for the longest effect queue.

## 9. Limits

Event-level timing, no cycle accuracy; the oracle for validation is the original driver executed by an independent emulator, not real hardware; revision 2 is not claimed unless the validation document says so; audibility of a play session is not claimed.
