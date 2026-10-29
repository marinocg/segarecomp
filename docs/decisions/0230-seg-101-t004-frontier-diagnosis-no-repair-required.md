# ADR 0230: SEG-101-T004 - frontier diagnosis; no repair was required

- Status: Accepted. Milestone SEG-101 (M5), task T004. Related: ADR 0227, 0229; the PCM contract of ADR 0095-class (`scd_pcm.c`: only write-only state is modelled).
- Privacy: classes, counts and widths only; no address of the commercial image appears (the chip offsets below are hardware register offsets).

## What the three routes found

The registered intervals of R1 and R2 (to entry+1500) end at their deadline, not at a stop. Within them every typed refusal class of the binder (`ledger_overflow`, `no_delivery`, `no_source`, `too_short`, `ambiguous`, `body_not_compiled`, `disc_unreadable`, `instruction_differs`) has count **0**, `unsupported_dynamic_body` and `static_dispatch_conflict` did not occur, and the runtime compared **0** unmatched executable fetches. `non_iso_source`, `oversize_file` and `transformed_code` would have appeared as `no_source` / `no_delivery`; none did: every executed instruction of every route came from one of the 120 compiled images or the three boot extents. The one frontier encountered is the independent hardware stop below. **No production repair was made in this task; none was authorized by a reproduced, on-path failure.** (The Compatibility control of R1 stops `entry_not_compiled` at a Word RAM module - a limit of that producer, which the file-first path removes; ADR 0229.)

## `pcm_unsupported` (the stop of R0 and, later, of R1)

1. **Which processor.** The sub processor, in both occurrences (R0: a byte read; R1, after about 12,450 frames, a word read).
2. **Which register.** The PCM sound chip's wave-RAM **address-pointer read registers**: chip offsets `$10-$1F` (the sub's `$FF0020-$FF003F` byte-wide map), per channel low/high byte of the channel's running wave address. R0's read is at the first channel's pointer (byte), R1's later read is a word at a later channel's pointer. They are not control writes and not wave-RAM reads.
3. **Documented / reference behaviour.** The independent reference (Genesis Plus GX `pcm_read`) answers a read of `$10-$1F` with the channel's running address (`addr >> 11` low, `addr >> 19` high) after running the chip to the reading instant, and `$FF` for every other illegal read. The pointer advances by the channel's frequency step at the chip's sample clock while the channel is on, wraps to the loop start when the wave byte read is the loop marker, and is reset by the channel's start register.
4. **What this runtime does.** It models the write-only register file and wave RAM (no audio), so a read of running-channel state is the typed stop `pcm_unsupported` by design (`scd_pcm.c`), not an accident.
5. **Does it block the experiment?** No: the pre-registered intervals end 600+ frames before R0's stop and 11,000 frames before R1's (exploratory) stop; R2 never reaches one (it idles on its screen). It is the next barrier of any longer Sonic CD route, independent of file-first.
6. **Repair?** A correct read needs the chip's sample-clock address advance and loop handling in addition to the register file (the pointer is observable game state, and the reference's value depends on the exact reading instant in sub cycles). That is a small device model but not a "narrow register read": a fabricated constant would be wrong by construction and is refused. It does not remove a measured blocker of this milestone and is recorded for a dedicated successor. Full PCM mixing, CD-DA output and an audio subsystem remain out of scope.

## Gate

`FRONTIER_PARTIAL`: no frontier stood on the pre-registered path (nothing needed repair); the independent PCM address-pointer frontier is diagnosed and deferred, with the missing behaviour specified above.
