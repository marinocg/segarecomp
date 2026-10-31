# ADR 0259: Sega CD main-side Z80 - image epochs, re-arm and the driver family (SEG-115 / M19)

- Status: Accepted (SEG-115, T001-T002). Builds on ADR 0258 (M18), ADR 0072/0073 (Genesis Z80, image epochs), ADR 0134 (structural variants NO-GO) and ADR 0135
  (static mutation proof spike). It changes none of their conclusions; the Genesis machine is unchanged (both options below are off unless the Sega CD attach sets them).
- Local evidence (never committed): `/tmp/m19`. The authorized disc and BIOS stay in the ignored inputs; this record keeps classes, counts and sizes.

## 1. Root cause (T001, before any change)

An ephemeral probe (a scratch copy of the runtime that logged every 68000 write to Z80 RAM with its time and every Z80 store that changed a byte, the epochs and the first
structural mismatch; not part of the product) was run on the authentic Earthworm Jim SE build of M18.

| Epoch | Hold window of the signature (extents) | Image chosen | What happened |
| --- | --- | --- | --- |
| 1 | 38-byte stub (1) | stub | runs |
| 2 | 5,144 bytes (3) | BIOS sound driver | runs ~37 Mticks, no code-byte store |
| 3 | 5,148 bytes (4), same driver re-uploaded | BIOS driver (new signature) | **first structural mismatch ~2.1 Mticks after activation** |
| 4 | 3,463 bytes (3) | a BIOS image | never executes: the Z80 was already isolated |
| 5 | 39 bytes (2) | stub | never executes (isolated) |
| 6 | **empty** (the whole 8 KiB was written in an earlier hold that a plain resume ended) | the previously bound image (stale) | never executes (isolated) |

Two independent faults:

- **A - activation / upload provenance.** The 68000 writes Z80 RAM only while it holds the bus, and the Genesis hold window ends at *every* runnable transition,
  including a plain BUSREQ release. An upload spread over several holds, or made while the previous image is still being resumed, therefore reaches the next
  `/RESET`-restart epoch with an empty (or partial) window, and a restart with an empty window re-binds the *previous* image (a "plain restart"). This is not
  specific to the title; it needs no title knowledge to explain.
- **B - genuine structural self-modification.** At epoch 3 the driver's own instructions store constants into low code bytes that the Z80 then executes (the
  modifying agent is the Z80 for every differing byte: the live bytes of the mismatching instruction were all Z80-written or unchanged image bytes; no
  registered image has those bytes). The same family modifies the game driver at its initial setup (below).

Answers to the T001 questions: the executable image of each epoch is in the table; the RAM ranges were written by the 68000 (uploads, mailbox/command bytes) and by the Z80
(stack, variables, and the five code bytes); no other registered image explains the live bytes; the mismatch at epoch 3 occurs in the BIOS driver, **before** the game's
driver can run; the game driver never reached its handshake write in M18 because of both faults, in that order (B at epoch 3 isolated the Z80 permanently, then A bound the
stale stub-era image at epoch 6).

## 2. Decision (T002)

1. **Accumulate the hold window until an image epoch** (Sega CD only; `GenesisZ80EpochObserver.accumulate_until_epoch`). The window is cleared at an image epoch, not at a plain
   resume, so every 68000 write since the previous epoch is part of the next epoch's signature S1\*. The signature definition itself (extents of 68000-written bytes with the
   bytes at the transition) is unchanged. Consequences, recorded: bytes the 68000 writes between two epochs (command-ring bytes) join the next epoch's identity; a Z80 restart
   with no 68000 write since the previous epoch is still a plain restart; an image whose bytes outside the written extents differ (the Z80's own data) is the same image.
   A new signature is a new image, found by the existing build rounds; an unseen one stays the typed `z80_unsupported` stop of an executable program.
2. **Re-arm an isolated Z80 at the next image epoch** (Sega CD only; `GenesisZ80Machine.rearm_faulted_on_epoch`). Contract section 18.4 made the isolation permanent as "the simplest safe
   choice" and left re-arming a future decision. A genuine epoch (a pristine Z80 that starts from RAM that the epoch's signature selects) runs a *registered* image from the
   architectural reset state; the RAM-backed guard is unchanged, so a mutation of the new image isolates it again. Nothing is interpreted or guessed, no state is fabricated, the
   YM2612 reset is the same device effect as before, `z80_reset` runs at the release exactly as for a healthy Z80, and the count is reported (`z80.recoveries`; the state digest
   changes only for a run that recovered). The isolated period itself stays silent and its effects absent (the recorded deviation of ADR 0258 decision 4).
3. A Z80 that is resumed over code the 68000 has just overwritten (the bus released between the upload and the `/RESET` restart) executes the *old* image's compiled bytes
   against new live bytes; the guard isolates it (epoch 5 above). That is the faithful outcome of an unsupported hardware interleaving (the real Z80 would execute the new
   bytes from a stale program counter), not a defect to hide.

## 3. Tests

`tools/sega_cd_bios_hw_fixture.py` cases 15-16 (hand-assembled Z80 programs; the fixture ROM is 0x200 bytes larger) and `tests/sega_cd_main_z80_test.py`: first upload and
activation, a different image uploaded across two holds *while the previous image runs* and restarted (the case that selected the stale image), a restart without change,
the same program again with different irrelevant Z80 data (the same image, not a new one), an isolated Z80 re-armed by a registered image, identical state for scheduler
partitions; the existing unregistered-image stop (case 14) is unchanged. Negative control (both options off): case 15 faults with `z80_code_mismatch` at the restart and
runs the old image; case 16 stays isolated.
