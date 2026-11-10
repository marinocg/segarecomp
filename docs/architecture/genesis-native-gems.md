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
| Command mailbox | The 68000 appends bytes to a 64-byte ring in the sound RAM (`$1B40`) and publishes its write pointer (`$36`) after each byte; the driver publishes its read pointer (`$37`). A command is `$FF`, an id and its arguments. Bytes that do not start a command are discarded; a write pointer outside the ring is a fault. A command whose arguments are not all written yet waits. |
| Ownership / delivery | The 68000 writes only while it holds the Z80 bus, so the driver looks at the ring when the bus is returned (and at boot). |
| Acknowledgement | The driver's only visible acknowledgement is the read pointer, advanced exactly when a command is consumed. An unsupported command is **not** consumed (no fabricated acknowledgement). The transfer-in-progress mailbox (`$1B21` bit 0) stays 0: the replacement reads the cartridge atomically and never holds the bus; the 68000's block flag (`$1B20`) therefore has nothing to hold off. |
| Initialisation / reset | Boot silences the four PSG voices. A `/RESET` restart re-boots the driver and clears its state; the ring bytes and pointers (owned by the 68000) are not touched. While the 68000 holds the bus the driver does nothing, work due earlier happens when the bus returns, and a VBlank interrupt edge that falls entirely inside a hold is lost (as on the real Z80). |
| Tick | One tick per VBlank interrupt. Sub-beat accumulator (default 204 / 256 per tick) selects music-time-base sequencers; SFX-time-base sequencers run every tick. |
| Resources | The "get pointers" command (11) delivers four 24-bit little-endian pointers (patches, envelopes, sequences, samples). Patches, sequence headers and tracks, sample headers and sample bytes are read from the cartridge image the runtime owns, never from anywhere else; an unmapped read is a fault. |
| Sequences | start (16) / stop (18) / stop all (22). Track events: delay, duration, note, end, no-op, priority, SFX time base, program change, sample rate. |
| Digital voice | One-shot, uncompressed samples at the fast Timer A rates: Timer A programmed from the rate, DAC enabled, channel 6 panned centre, one 8-bit write to the DAC register per Timer A expiry (the expiry is confirmed on the YM2612 itself - a mismatch is a fault, never an assumption), 128-byte refills of a two-half FIFO exactly as the original (including its refill-time shutdown that cuts the tail of a non-looped sample), priority-based voice preemption, note-off semantics. |
| Idle clipping | Every stop clips all FM voices (clamp the four operator levels, key off) and queues the PSG stop that the next tick writes, as the original does. |

## Coverage (what is verified) and what is not

Verified and reference-compared: the commands above; sequences made only of the events above; one-shot uncompressed digital samples at rate codes 1-9 longer than one
refill half.

Not covered - each is an explicit fault (`gems_native.h: GemsNativeFault`), after which the replacement is silent for the run: FM and PSG patches (music), envelopes,
loops / goto / conditionals / mailbox stores, pitch bend, tempo / mute / start-song events, looped, compressed or slow-rate samples, shorter samples, and every other
command. Not modelled: the real driver's polling latency (events are placed on exact chip / interrupt times), stalls while the 68000 is mid-command, the
`$1B20` block flag. Save states are unavailable while the replacement is active.

## Build and disclosure

`segarecomp build ... --sound-driver gems` compiles `gems_native.c` and the adapter into the program (and into the build-time observation pass, so the 600-frame start-up
run reports what the replacement did). `status.json` `audio_implementation` records requested vs actual implementation, driver / version / evidence tier,
implementation version, coverage, verification tier, reason, audio status and the observation (commands / sequences / DAC samples / first fault); an original build records
`original_z80_aot` with its audio status and any compiled structural alternatives. `genesis_audio` is `native_partial` (or `degraded` if the start-up run met an unsupported
operation, with `z80_audio_outcome = native_driver_unsupported_operation`). The headless run summary (`SOUND_SUMMARY`) gains a `native` object, and
`SEGARECOMP_AUDIO_WRITE_LOG=<file>` logs every chip access (validation seam). The launcher shows the record in Build provenance and can request the replacement from the
game's Advanced build options; the request is part of its cache identity.

## Path forward (not part of this change)

Extend the verified subset operation by operation (FM patch load and key-on, PSG tone, envelopes, tempo, loops), each with a reference comparison; add the next driver
images / versions as catalog lines with their own validation; reuse the module's scheduler for other drivers only when a concrete second driver needs it.
