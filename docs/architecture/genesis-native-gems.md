# Native GEMS sound-driver replacement (Genesis) - architecture, coverage and limits

ADR 0291. Code: `platforms/genesis/runtime/gems_native.{h,c}` (driver core, no dependency on the Z80 machine), `gems_driver_catalog.inc` (verified images),
`z80_machine.{h,c}` (the binding), `genesis_sound.c` (the program-side adapter, compiled with `SEGARECOMP_NATIVE_SOUND_GEMS` only when selected),
`apps/segarecomp/build_command.cpp` (`--sound-driver`, identity check, `audio_implementation` record). Evidence: `docs/testing/genesis-native-gems-validation.md`.

## What it is - and is not

GEMS (Genesis Editor for Music and Sound Effects) is a Z80 sound driver that games upload into the 8 KiB sound RAM. A widespread build of it (2.5) rewrites its own
executable bytes (its DAC pump), which the generated-native Z80 cannot execute (ADR 0134): the Z80 is isolated and the game is silent.

The replacement is **original C11 code that reproduces the observable behaviour of that one verified driver image**. It never fetches, decodes, interprets or executes
a Z80 byte of the original, has no runtime compilation, no learning and no fallback. It is used only when (1) the build was asked for it and (2) the image the game
uploaded is in the catalog. Everything else about the machine (68000, VDP, bus arbitration, YM2612, PSG, mixer, save data) is unchanged and shared.

## Driver identity and version

The catalog line is the **activation signature** the runtime already computes for every Z80 image epoch (SHA-256 over the 68000-written extents of the sound RAM and their
bytes). It identifies the exact bytes uploaded; no title, header, file name or ROM digest takes part. Aladdin, Earthworm Jim, Jurassic Park and Cool Spot upload the
same image. Its version is 2.5, established by the address map and the data tables (not by a title); ADR 0291 states the evidence and its limit.
Unknown images: `--sound-driver gems` fails the build (`sound_driver_unverified`); at run time an unlisted image simply stays generated-native Z80.

## Protocol reproduced

| Aspect | Behaviour |
| --- | --- |
| Command mailbox | The 68000 appends bytes to a 64-byte ring in the sound RAM (`$1B40`) and publishes its write pointer (`$36`) after each byte; the driver publishes its read pointer (`$37`). A command is `$FF`, an id and its arguments. Bytes that do not start a command are discarded; an unknown id is consumed and ignored (as the original does); a write pointer outside the ring, or a control-block number above 15, is a fault. A command whose arguments are not all written yet waits. |
| Ownership / delivery | The 68000 writes only while it holds the Z80 bus, so the driver looks at the ring when the bus is returned (and at boot). |
| Acknowledgement | The driver's only visible acknowledgement is the read pointer, advanced exactly when a command is consumed. The transfer-in-progress mailbox (`$1B21` bit 0) stays 0: the replacement reads the cartridge atomically and never holds the bus; the 68000's block flag (`$1B20`) therefore has nothing to hold off. Mailboxes `[2..31]` are the sequences' `store`/`if` variables and are read and written in the sound RAM the 68000 sees. |
| Initialisation / reset | Boot silences the four PSG voices. A `/RESET` restart re-boots the driver and clears its state; the ring bytes and pointers (owned by the 68000) are not touched. While the 68000 holds the bus the driver does nothing, work due earlier happens when the bus returns, and a VBlank interrupt edge that falls entirely inside a hold is lost (as on the real Z80). |
| Tick | One tick per VBlank interrupt. The sub-beat accumulator (tempo-scaled) selects music-time-base work; at fast tempos a tick carries several beats and each is a pass of its own, as in the original's main loop. SFX-time-base work runs every tick. |
| Resources | The "get pointers" command delivers four 24-bit little-endian pointers (patches, envelopes, sequences, samples). Patches, envelopes, sequence headers and tracks, sample headers and sample bytes are read from the cartridge image the runtime owns, never from anywhere else; an unmapped read is a fault. Offsets are 8-bit-wrapped where the original wraps them. |
| Commands | note on / off, program change / update, pitch bend, tempo, envelope, retrigger, get pointers, pause, resume, sustain, start / stop sequence, priority, stop all, mute, sample rate, mailbox store, lock / unlock, bend by sequence, volume, master attenuation. |
| Sequences | delay, duration, note, end, program change, envelope, no-op, loop start / end (a four-deep stack, endless loops), retrigger, sustain, tempo, mute, priority, start song, pitch bend, SFX time base, sample rate, goto, mailbox store / if, special codes (stop, pause, resume, attenuation). |
| Voices | Six FM voices (voice 6 shared with the digital voice, voice 3 of the table for channel-3 mode), three PSG tone voices, one noise voice; the original's allocation by priority and release timer, voice timers, release phases, and the clipping every stop performs. FM patches write the full operator map with carrier levels attenuated per note; frequencies come from the original's tables with pitch bend and pitch-envelope interpolation. PSG voices follow the original's attack / decay / sustain / release emulation. |
| Digital voice | One-shot, looped, slow-rate and 4-bit compressed samples: Timer A programmed from the rate, DAC enabled, channel 6 panned centre, one 8-bit write to the DAC register per Timer A overflow - the overflow time is taken from the chip's own schedule and confirmed on it, never assumed - 128-byte refills of a two-half FIFO exactly as the original (including its refill-time shutdown that cuts the tail of a non-looped sample), priority preemption, note-off semantics. |

## Coverage and limits

Implemented and reference-compared: everything in the table above. A fault means malformed or unreadable data, after which the replacement is silent for the run.

Not modelled: the real driver's polling latency (events are placed on exact chip / interrupt times), stalls while the 68000 is mid-command, the `$1B20` block flag, one command per pass of
the original's main loop (the replacement drains the ring at once). Save states are unavailable while the replacement is active. A different driver image or version is not covered until it is
added to the catalog and validated.

## Build and disclosure

`segarecomp build ... --sound-driver gems` compiles `gems_native.c` and the adapter into the program (and into the build-time observation pass, so the 600-frame start-up
run reports what the replacement did). `status.json` `audio_implementation` records requested vs actual implementation, driver / version / evidence tier,
implementation version, coverage (`complete`), verification tier, reason, audio status and the observation (commands / sequences / DAC samples / first fault); an original build records
`original_z80_aot` with its audio status and any compiled structural alternatives. `genesis_audio` is `native` (or `degraded` if the start-up run met malformed data, with `z80_audio_outcome = native_driver_unsupported_operation`). The headless run summary (`SOUND_SUMMARY`) gains a `native` object, and
`SEGARECOMP_AUDIO_WRITE_LOG=<file>` logs every chip access (validation seam). The launcher shows the record in Build provenance and can request the replacement from the
game's Advanced build options; the request is part of its cache identity.

## Path forward (not part of this change)

Add the next driver images / versions as catalog lines with their own validation; if a game needs behaviour the reference comparison cannot reach (cycle-exact DAC / FM timing), extend the model
with evidence. Reuse the module's scheduler for another driver family only when a concrete second driver needs it.
