# ADR 0258: Sega CD main-side Z80 (SEG-114 / M18)

- Status: Accepted (SEG-114). Builds on ADR 0072/0073 (Genesis Z80 integration, image epochs), ADR 0134 (structural self-modification is an unsupported
  class, the Z80 is isolated), ADR 0143 (main Genesis devices of the BIOS-image profile) and ADR 0257 (M17).
- Local evidence (never committed): `/tmp/m18`. Commercial inputs stay in the ignored `games/` directory; this record keeps classes, counts and sizes.

## 1. The frontier (T001, before any implementation)

Authentic Earthworm Jim SE, Model 1 BIOS V1.10, Compatibility, normal build driver, no hints, no input, M17 `main`:

| Item | Observation |
| --- | --- |
| build | 5 rounds, final stop `main_device_unsupported` (class: main processor, device capability) |
| access | main processor, a **byte read** of the Z80 sound RAM window (mailbox area in the first 64 bytes), from main work-RAM code, with the Z80 bus granted |
| time | frame 1,249, machine tick 559,546,433,500,000 |
| Z80 bus | BUSREQ asserted and granted, /RESET released; the Z80 had been runnable before |
| why the read stops | the legacy rule: once the Z80 has been runnable (`z80_ran`), any main read of the Z80 area is `main_device_unsupported` |
| Z80 history (68000 bus traces, local) | four uploads before the stop: the 38-byte boot stub, two BIOS sound-driver uploads (0.3 KiB, 5.8 KiB), then the game's 6 KiB driver followed by a zero fill of the rest of the 8 KiB; 7 reset assertions; 1,171 BUSREQ cycles |
| why the game reads it | the game's 68000 sound-command routine (RAM-resident) stores into and reads back a command-ring index in sound RAM; a later routine polls a handshake flag the Z80 driver sets and the 68000 clears (8,748 polls in a bounded experiment) |

Critical question - what would hardware return? The first byte is a variable only the 68000 driver stub writes (its own earlier store); the polled flag is
written by Z80 code. Classes of the brief: (1) an ordinary RAM read with bus ownership - yes for the first byte; (2) a value that depends on Z80 execution - yes for
the handshake; (3-6) not applicable (no YM2612 read, valid shape, routing already the Genesis host's).

## 2. Classification (T002): **B - real Z80 execution dependency**

The read is ordinary RAM, but the memory it reads is part of a conversation with a running Z80 driver (a flag the driver writes and the 68000 polls). The value a
read must return depends on Z80 execution, so no state the existing devices hold justifies a narrow correction (A) and no safe read can be answered by
returning stored bytes (C is not the case: the evidence is sufficient). The independent hardware evidence is the project's frozen Z80 audio contract
(Genesis Technical Overview, Genesis Plus GX, ares) that ADR 0072 implements; no new hardware fact is introduced. No reference emulator binary is available in this
environment, so no emulator frame comparison is claimed.

Reuse evaluation: the Genesis owner already provides everything B needs - the generated-native Z80 (`z80_run`), the image epoch / activation signature model, the
BUSREQ/RESET rules, the lazy-follower scheduling contract, the machine seam (`GenesisZ80Hooks`), and the broad Z80 AOT emitter. The Sega CD device host *is* a
Genesis runtime, and every main access to the Z80 area already goes through `genesis_route_access`, which already synchronises an attached machine. So the
integration is an attach, not a second Z80, scheduler or discovery pipeline.

## 3. Decision (T003)

1. **Attach** the Genesis Z80 machine to the Sega CD device host (`platforms/sega-cd/runtime/z80/scd_z80.c`), registered by the generated `main`
   (`sega-cd emit --main-z80 1`, exactly like the dynamic-image registry). The runtime core only knows `ScdZ80Ops`; without a registration nothing changes.
2. **Image discovery rides the existing rounds.** A build pass records every Z80 image epoch it cannot activate (`z80_epoch_*` files; the Z80 is left
   unexecuted, and a pass is never a confirmed candidate while it recorded one); `segarecomp sega-cd build` registers them in the Genesis `Registry`, emits and
   compiles them with the existing `emit_registry`, and the next round links them. A pass that ends in another admission (epoch, extent, template) absorbs the Z80 rebuild; only a pass that would otherwise be the candidate costs one extra round. Measured: Sonic CD keeps its 10 rounds; Earthworm Jim's 11 rounds are the previous rounds plus the sub-side extents the backup-RAM input uncovers.
3. **Banked window.** `GenesisZ80BankView` (new, optional) replaces the Genesis cartridge/work-RAM view: the Z80 reads the immutable BIOS image; every other 68000-bus
   target and every banked write is unsupported (a mutable 68000 bus cannot be read by a lazily synchronised follower without a causality rule this milestone does not define).
4. **Fail-soft, like the Genesis (audio contract section 18).** A structural self-rewrite of Z80 code (ADR 0134), an unsupported Z80-side access (the new
   `isolate_view_stops` option) or a Z80 isolated this way finishes the faulting instruction first (a read has returned $FF); an uncompiled image in a build pass quiesces the Z80 (`z80.state = unmodelled`; such a pass is never a confirmed candidate); the **main processor is never stopped by it** and reads sound RAM as it
   stands. The recorded deviation: whatever the isolated Z80 would have written is absent. The report says so (`z80.state = faulted`, fault diagnostic, epoch, tick).
5. **Fail-closed where it matters.** An executable program (not a build pass) that meets an image epoch no pass saw stops `z80_unsupported`
   (new stop class; the `address` field carries the typed Genesis diagnostic, 56-61 only; 56 = `z80_unknown_image`; a main access without the bus or to an unmapped Z80-area address stays `main_device_unsupported`); the Z80 never interprets a byte. A Z80 that has no attached
   machine (hand-built programs, `--main-z80 0`) keeps the previous rule.
6. **Not attached:** YM2612 and PSG (a YM read answers 0, writes are discarded, as the Genesis machine does without an audio attach). No sound is produced; adding the
   shared ymfm/SN76489 devices to the Sega CD machine is a separate, optional step.

## 4. What the integration does and does not reach on Earthworm Jim (measured)

- The Z80 images are discovered (4 epochs, 4 images; 6 epochs executed). The BIOS's own sound driver rewrites its opcodes (the ADR 0134 class), so the Z80 is isolated
  from the third epoch on, and the game's driver - uploaded while the Z80 is still running the previous image, then restarted with an empty hold window that re-binds
  the stale image - never executes either. **Sound is therefore silent and the Z80-written handshake flag is never written.** The game nevertheless reads sound RAM
  without stopping and proceeds. (A hold-window rule that accumulates 68000 writes until the next epoch, and structural-variant support for the driver family, are the
  follow-on work; both are explicitly outside this milestone, see section 7.)
- A *bounded, local-only* experiment (guard bypass returning stored sound-RAM bytes, never committed) showed the same flow: the downstream frontier after the Z80 is the
  sub processor's backup RAM, which is an absent **machine input**, not a capability (`--backup-ram`, the formatted-empty image tool).

## 5. Authentic results (T004-T007, local; backup RAM = formatted-empty image)

| Measure | Result |
| --- | --- |
| build | 11 rounds (5 extents), no final stop; cold-cache build 10 min wall, 1.2 GB peak; Sonic CD 10 rounds, identical program identity and report (except the new `z80` object) to M17; note the program identity names the 68000 programs only - `z80.registry_identity` in status.json names the Z80 images |
| scripted discovery (T006) | a build whose passes replay START/RIGHT yields the **same** 11 rounds, extents and program identity: the no-input discovery already covers the whole path |
| experiment A (no input) | SEGA logo (~frame 3,000), Interplay logo (~5,000), title with "PRESS START / OPTIONS" (~5,800-6,600), then without input the game loads "New Junk City" (~7,400) and plays its attract demo (self-playing, lives 3, health falling); 26,627 frames at 600 M instructions without a stop |
| experiment B (START only; press at onset 6,000, hold 5) | the guest's TH-multiplexed pad reads show Start low (`$13` vs `$33`) for frames 6,000-6,004 (input log), the title is replaced, the loading screen appears ~1,100 frames earlier than in A, and the first level starts at ~7,000 with full health and the player idle (distinct from the demo) |
| experiment C (START, RIGHT at 7,400 for 120 frames) | Jim runs right, the scenery and sign scroll away, the bus passes, and he reaches the chain ; the same press released after 60 frames is frame-for-frame identical to C until the release and diverges afterwards (a shorter run) |
| experiment D (control) | START only: Jim stays at the spawn point for the same 120 frames; right-held and control frames differ from the first pressed frame onwards, and the right-input run matches C exactly until its release |
| experiment E (RIGHT held 900 frames, no further input, run to frame 19,999) | enemies, damage, death and a level restart; **no stop**, 4.5 x 10^8 native instructions (main 1.67 x 10^8, sub 2.85 x 10^8), ~5.5 minutes of guest time; no unsupported device, no uncompiled body |
| determinism | final frame hash, work/VRAM digests, device log, instruction count and tick identical for the default schedule, `--slice-seed 1` and `--slice-seed 7` |

"Right controls movement" rests on the divergence of the right-input and control runs from the pressed frame, on visible player and camera displacement across the held
interval, and on the pad reads the guest performed; a changed frame hash alone is not used. Not claimed: full-game compatibility, correct sound, correct death/respawn
timing against hardware, or anything beyond frame 19,999 (the verified bounded window).

## 6. Tests (T008)

`tests/sega_cd_main_z80_test.py` (fixture cases 12-14 of `tools/sega_cd_bios_hw_fixture.py`, hand-assembled Z80 bytes): the generated-native Z80 writes a byte the main
program reads; images discovered without an extra round; identical state/digest/device log for three scheduler partitions; a Z80 rewriting its own opcode is isolated
and the main program continues; an unseen image is the typed `z80_unsupported`/56 stop; `--main-z80 0` keeps `main_device_unsupported` and no `z80` report object.
The pre-existing Sega CD and Genesis Z80/audio suites run unchanged.

Untested paths (recorded gap): Z80 banked BIOS reads, banked non-BIOS isolation (diagnostic 54), YM/PSG accesses, the `z80_epoch_bound_exceeded`/`z80_no_progress` failures and the viewer link are covered by code review only.

## 7. Consequences, limits and follow-on

- Recorded deviations: the Z80's bank window sees the BIOS image only; no Z80 timing contention on the 68000 bus; no YM2612/PSG; isolated Z80 effects absent; the Z80
  follows the machine lazily (so its writes to main work RAM, were they supported, would be unsynchronised - hence unsupported).
- Follow-on (separate milestone): (a) a hold-window rule that accumulates 68000 writes since the last *epoch* for the Sega CD machine, so an upload made while the Z80
  still runs is an image of its own; (b) structural-variant support for the Sega driver family, which ADR 0134/0135 left open (proved existence of exact-valued code writes,
  no closed set); (c) attaching the shared YM2612/PSG devices so the driver's sound is audible. Only (b) needs a new emitter mechanism.
