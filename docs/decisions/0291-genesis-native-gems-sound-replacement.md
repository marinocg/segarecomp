# ADR 0291: Native GEMS sound-driver replacement for Genesis games whose Z80 driver rewrites its own code

- Status: Accepted (opt-in, explicit selection).
- Builds on: ADR 0072/0073 (Genesis Z80 platform integration, RAM-backed images), 0134 (structural variants NO-GO), 0135 (static mutation analysis), 0259 (Sega CD
  compiled structural alternatives), 0277 (static-first Z80 production). Changes none of their conclusions and adds no Z80 mechanism.
- Architecture, coverage and limits: `docs/architecture/genesis-native-gems.md`. Evidence: `docs/testing/genesis-native-gems-validation.md`.

## Context

Games whose sound driver patches its own executable bytes end with `genesis_audio = degraded / structural_code_mismatch` (ADR 0134): the generated-native Z80
isolates itself and the game runs silent. The driver that causes most of these in the local corpus is GEMS. ADR 0134 observed the same three-epoch shape and the same
RET / EXX / EX AF,AF' alternation in three titles; this record establishes why, identifies the driver and decides what to do about it.

## Facts established (aggregates only; no byte, address, disassembly or title data is recorded)

1. **Four titles share one driver image.** The image the 68000 uploads in Aladdin, Earthworm Jim, Jurassic Park and Cool Spot has one and the same activation
   signature (the digest the runtime already computes: the 68000-written extents of the sound RAM and their bytes). It is one driver binary, not four lookalikes.
2. **It is GEMS 2.5.** The image's address map matches the published 2.5 driver byte for byte at every point checked: the PSG pseudo-registers, the command FIFO
   pointers, the interrupt handler, the three self-modified instruction sites and the saved copies of the first, the stack initialisation, the sub-beat tempo
   default, the FM and PSG frequency tables and the start of the patch buffers. The 2.0 driver differs (other stack and no compressed-sample hook); the 2.8 driver factors
   the delay / duration decoding that 2.5 repeats inline, and this image repeats it. A byte-exact comparison with an original binary was not possible; the version is
   therefore recorded as "2.5" with the evidence tier `address_map_and_tables`.
3. **The self-modification is the driver's DAC pump.** Its DAC routine, its processing hook and its refill routine have their first bytes rewritten between RET, EXX,
   EX AF,AF', JR and NOP; some of the written values are copies the driver saved from its own image earlier and then loads from RAM. That is exactly what ADR 0259's
   static analysis cannot enumerate.

## Decision 1 - AOT first, measured; it does not close, so the replacement is justified

The Sega CD structural-alternatives mechanism was enabled for the Genesis build behind the opt-in `--z80-alternatives 1` (a three-line change: the emitter already
supports it). On Cool Spot the first fault moves from frame 4 to frame 109 and the sound CPU is still isolated afterwards. The static analysis finds 11 candidates in a
driver with 498 stores, 409 of them with an unknown destination and 173 with a value it cannot model (memory loads, ALU results, call continuations); the writes that
remain are the saved-copy writes above. Closing the family would need interprocedural data-flow over RAM-resident values: the broad symbolic-analysis project this
work was told not to start, and an observation-driven compile / fault / recompile loop is forbidden (ADR 0134). Decision: **keep `--z80-alternatives` as an opt-in
original-path improvement (default off, recorded in the build metadata), and add the replacement.**

## Decision 2 - what the replacement is

An independently written, portable C11 module (`platforms/genesis/runtime/gems_native.{h,c}`), written from the observed protocol and the published driver's documented
behaviour (no source of either reference was copied; both are third-party and are used offline only). It is **not** an interpreter, decoder or emulator of Z80 code:
the original image is never fetched or executed. It reads the same RAM-visible mailboxes the 68000 writes, resolves its resources from the cartridge exactly as the
driver does, and schedules real YM2612 / PSG writes on the guest master clock through the existing devices and mixer.

It implements the **whole behaviour of the verified driver version**: every command (note on/off, program change/update, pitch bend, tempo, envelope, retrigger,
get-pointers, pause/resume, sustain, start/stop sequence, priority, stop all, mute, sample rate, mailbox store, lock/unlock, bend by sequence, volume, master
attenuation), every sequence event (delay, duration, note, end, program change, envelope, no-op, loops, retrigger, sustain, tempo, mute, priority, start song, pitch bend,
SFX time base, sample rate, goto, mailbox store/if, the special codes), FM / PSG tone / PSG noise / digital patches with the original's voice allocation, release timers,
pitch envelopes, pitch bend, PSG envelope emulation, the channel-3 special mode, and digital samples of every kind the driver plays (one-shot, looped, slow-rate, 4-bit
compressed) through its two-half FIFO and Timer A pacing. Where the published C port of the driver deviates from the 2.5 assembly (the SFX flag of envelopes, the target of
the mute event, a sub-beat reset at every sequence start, a refill call inside the DAC routine) the assembly is followed.

- **Selection** is explicit: `segarecomp build --sound-driver gems` (default `original`). The build refuses it (exit 6, stage `sound-driver`, `sound_driver_unverified`,
  no executable) unless at least one uploaded image's activation signature is in `runtime/gems_driver_catalog.inc`. The catalog keys on driver bytes, never on a title; the
  runtime consults the same catalog, and images that are not in it keep running as generated-native Z80. No fallback build, no mid-run switch, no learning.
- **Identity in the build:** the program is compiled with the replacement only when selected; the status record names the driver, version, evidence tier, the
  replacement's implementation version (`GEMS_NATIVE_IMPLEMENTATION_VERSION`), how it was validated and what the build's own start-up run observed.
- **Faults are explicit and now exceptional.** Only malformed or unreadable data stops the replacement: a read outside the cartridge, a track count above 16, a mailbox
  index outside the mailboxes, a FIFO pointer outside the ring, a control-block number above 15, a sequence that never yields. The sound CPU is then latched as faulted
  (`z80_native_driver_fault`) and the replacement writes nothing further; no acknowledgement, status byte or chip write is invented. An unknown command id is *ignored*,
  exactly as the original ignores it. A restart (`/RESET`) re-boots the driver; the 68000's FIFO bytes and pointers survive as they do on hardware.
- **Timing model.** Work is placed on exact guest times (VBlank interrupt edges, Timer A overflows asked of the chip itself, command-FIFO checks when the Z80 bus is
  returned), not on a Z80 poll phase: it is event-accurate, not cycle-accurate. Not modelled: the original's polling latency, its stalls while a command is half written, the
  68000's DMA block flag (the replacement reads the cartridge atomically).
- **Save states** are unavailable while the replacement is active (`NOT_CAPTURABLE`), because its state is not part of the snapshot format; a faulted replacement is
  inert and snapshots normally.

## Decision 3 - disclosure

`status.json` gains `audio_implementation` (schema 1; requested vs actual implementation, driver / version / evidence, coverage, verification tier, reason, audio
status, and the start-up run's observation). The launcher shows one row, "Audio implementation", in the existing Build provenance section, from that record only: it never
infers a backend from the title, the file name or the current selection; a missing record is "Not recorded" and an unrecognised one "Unknown". `verification` is the
evidence tier of the replacement for that driver image (`build_qualified` / `reference_checked`); it never claims that a play session sounded right.

## Consequences

- Cool Spot, Aladdin, Earthworm Jim and Jurassic Park get their sound back - music and effects - from the first command to the end of a long reference run, and each
  operation is compared with an independent implementation (`docs/testing/genesis-native-gems-validation.md`).
- Another driver image or version lands as one catalog line plus its own validation against the reference; a different driver family is a different module. Nothing here constrains the general mod runtime.
- The default build, its generated code and every other platform are byte-for-byte unchanged (`gems_native.c` is not compiled without the option).

## Rejected

A Z80 interpreter or audio-only emulator; a JIT; copying or porting either third-party reference; accepting a driver by title, file name or header; an automatic
(unrequested) replacement; weakening ADR 0134 / 0259's closure arguments to make the family pass; a hard-coded or sample-substituted "demonstration".
