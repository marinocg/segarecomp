# ADR 0291: Native GEMS sound-driver replacement for Genesis games whose Z80 driver rewrites its own code

- Status: Accepted. Amended by PR #216 review: the replacement is selected **automatically** by the build (never by a user option) and it takes part in save states and rewind.
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

- **Selection is automatic** (see "Decision 4"): `segarecomp build` defaults to `--sound-driver auto`. `--sound-driver original|gems` are developer overrides that the launcher
  never uses: `gems` is refused (exit 6, stage `sound-driver`, `sound_driver_unverified`, no executable) unless at least one uploaded image's activation signature is in
  `runtime/gems_driver_catalog.inc`. The catalog keys on driver bytes, never on a title; the runtime consults the same catalog, and images that are not in it keep running as
  generated-native Z80. No fallback build, no mid-run switch, no learning.
- **Identity in the build:** the program is compiled with the replacement only when selected; the status record names the driver, version, evidence tier, the
  replacement's implementation version (`GEMS_NATIVE_IMPLEMENTATION_VERSION`), how it was validated and what the build's own start-up run observed.
- **Faults are explicit and now exceptional.** Only malformed or unreadable data stops the replacement: a read outside the cartridge, a track count above 16, a mailbox
  index outside the mailboxes, a FIFO pointer outside the ring, a control-block number above 15, a sequence that never yields. The sound CPU is then latched as faulted
  (`z80_native_driver_fault`) and the replacement writes nothing further; no acknowledgement, status byte or chip write is invented. An unknown command id is *ignored*,
  exactly as the original ignores it. A restart (`/RESET`) re-boots the driver; the 68000's FIFO bytes and pointers survive as they do on hardware.
- **Timing model.** Work is placed on exact guest times (VBlank interrupt edges, Timer A overflows asked of the chip itself, command-FIFO checks when the Z80 bus is
  returned), not on a Z80 poll phase: it is event-accurate, not cycle-accurate. Not modelled: the original's polling latency, its stalls while a command is half written, the
  68000's DMA block flag (the replacement reads the cartridge atomically).
- **Save states and rewind are supported** (see "Decision 5"). The earlier `NOT_CAPTURABLE` refusal is gone.

## Decision 3 - disclosure

`status.json` gains `audio_implementation` (schema 2; schema 1 records of the first iteration of this PR are still read: requested vs actual implementation, `selection`
`automatic | developer_override`, `reason`, driver / version / evidence, the replacement's implementation version and the catalog revision, `save_states`, coverage,
verification tier, audio status, the original AOT's failure evidence for an automatic selection, and the start-up run's observation). The launcher shows one row, "Audio implementation", in the existing Build provenance section, from that record only: it never
infers a backend from the title, the file name or the current selection; a missing record is "Not recorded" and an unrecognised one "Unknown". `verification` is the
evidence tier of the replacement for that driver image (`build_qualified` / `reference_checked`); it never claims that a play session sounded right.

## Consequences

- Cool Spot, Aladdin, Earthworm Jim and Jurassic Park get their sound back - music and effects - from the first command to the end of a long reference run, and each
  operation is compared with an independent implementation (`docs/testing/genesis-native-gems-validation.md`).
- Another driver image or version lands as one catalog line plus its own validation against the reference; a different driver family is a different module. Nothing here constrains the general mod runtime.
- A game whose original generated-native Z80 works, and every other platform, is built exactly as before (`gems_native.c` is compiled only into a program the build selected it for). The
  player never sets a sound option.

## Rejected

A Z80 interpreter or audio-only emulator; a JIT; copying or porting either third-party reference; accepting a driver by title, file name or header; a replacement chosen by
any evidence other than a verified catalog identity plus the original AOT's own recorded failure; a per-game user option, preference or cache-key effect for the choice; a runtime
fallback between implementations; weakening ADR 0134 / 0259's closure arguments to make the family pass; a hard-coded or sample-substituted "demonstration".

## Decision 4 - automatic selection (amendment)

The first iteration asked the player to tick an experimental option. That is the wrong place for the decision: a supported game must play correctly without anyone changing a
technical sound option, and the build already holds the evidence. `segarecomp build` therefore decides, deterministically, in at most two passes:

1. **Pass 1** is the ordinary build with the original generated-native Z80 (`--sound-driver auto`). If the build's own verification run ends with the sound CPU **not** isolated, the
   original stays (`reason: original_supported`); if it is isolated and **no** uploaded image is a verified GEMS image, the original stays and the record says so
   (`no_verified_replacement`, audio `degraded`).
2. If the original AOT isolated the sound CPU by a **structural code mismatch** *and* an uploaded image's activation signature is in the catalog, the build records that evidence and
   runs **pass 2**: the same program built with the replacement (`reason: original_aot_failure_verified_replacement`). A pass 2 whose image identity is not verified, or that
   was reached without the recorded evidence, fails closed (`sound_driver_unverified`); there is no third pass and no heuristic.

The user's choice of nothing is the product: no checkbox, no per-game preference, no persistence and no cache-key effect of a *choice*. The cache identity of a build does include
what the build may choose from - the replacement's implementation version and the catalog revision - so a product that ships a changed replacement or catalog never reuses an
older program, and a build record is never relabelled after the fact. `--sound-driver original` and `gems` exist for developers (forcing a comparison, or reproducing a result);
they are recorded as `developer_override` and are not reachable from the launcher.

## Decision 5 - save states and rewind with the replacement

The replacement's complete execution state is a canonical, versioned (`GEMS_NATIVE_STATE_SCHEMA`), pointer-free, explicit little-endian payload (`gems_native_state_put`): time and
event cursors, the pointer tables, every control block, voice, envelope block and pitch-bend slot, the PSG envelope processor, the digital voice (sample cursor, FIFO halves, rate),
Timer A pacing and the counters. The 8 KiB sound RAM with the command FIFO and the mailboxes belongs to the machine snapshot and is not repeated. The Genesis snapshot container
is unchanged for programs without the replacement (byte-identical); a program with one sets the `NATIVE_SOUND` machine flag, which changes the execution fingerprint, so states
never cross the boundary in either direction. The Z80 section gains a trailer: the backend tag (not serving / serving / faulted), the driver name and version text, the implementation
version, the payload schema, the activation signature of the bound image and the payload.

Restore keeps the ADR 0278 model: stage, validate, then commit. The driver payload is decoded into private storage and validated against the guest clock and the staged YM2612's Timer A
schedule (`gems_native_state_get`: every cursor and index in range, every time within the clock slack, the DAC due time not after the chip's next overflow, structural invariants of
the voices and blocks); identity (driver, version, implementation version, schema, activation signature of the image) must equal what this program carries and must match the
bound image of the staged Z80. Nothing live is touched until every section has passed; commit installs the staged state and rebinds the host, **writes to no chip** (the chip
state came with the snapshot) and cannot fail. A faulted replacement is recorded as such (identity only, no payload) and restores inert.

The save-state tests are in `docs/testing/genesis-native-gems-validation.md` (driver-level deterministic continuation and refusals, machine-level snapshot test on the real
runtime, local reference demonstration).
