# ADR 0144: SEG-064-T004 - sub-side boot hardware, disc-extent images and the application handoff of the BIOS-image profile (G-A)

- Status: Accepted (SEG-064-T004). Builds on ADR 0142 (decisions 1-8, the G-M result) and ADR 0143 (BIOS-scale emission, main-side
  boot hardware, the sub runnable epoch); ADR 0141's historical `NO_GO` is unchanged.
- Scope: both generated-native processors of an authentic BIOS advance from the verified sub epoch to the application IP entry, every
  device behaviour they need coming from independent evidence. It claims the application handoff of the reference's definition (the
  first main instruction of the application IP, after the disc's security stub). It does **not** claim BIOS services after the
  handoff, Sonic CD progress, SEG-064 completion or Checkpoint E.
- Privacy: no BIOS or disc byte, hash, internal address, routine inventory, trace or disassembly appears here or in any test; the
  comparisons with the independent reference were made locally and are reported as aggregate identity results only.

## Context

ADR 0143 stopped, by design, at the first frontier after the epoch: an unmodelled sub-side Gate Array register (the stopwatch) and the
main BIOS's level-6 work-RAM thunk. The independent boot inventory of ADR 0141 lists what lies between there and the application IP:
sub levels 1, 2, 4 and 5 and the timer, CDD commands, CDC host data, the graphics ASIC with its completion interrupt, write-only PCM,
the SP replacing transient BIOS code, and the main side after the release. This ADR takes them in the order the generated program
reached them (executed evidence first), batching each family.

## Decisions

1. **Stopwatch (`$FF800C`).** A free-running 12-bit count of 384-sub-cycle units on a grid anchored at power-on or at the peripheral
   reset; a word write of any value clears the count and keeps the phase; byte accesses are typed stops (GPGX flags the width itself as
   uncertain). Contract section 17.
2. **PCM write state.** The RF5C164's registers and 64 KiB wave RAM are stored (an odd-address byte write or the low byte of a word
   write reaches the chip; the channel and bank selects follow `CTRL`); the power-on and the peripheral reset clear them as GPGX's
   `pcm_reset` does; no sound is generated and every read is the typed stop `pcm_unsupported`. The authentic boot path only writes; the
   register file and the whole wave RAM at the handoff are compared with the reference (below).
3. **Graphics ASIC.** The stamp renderer of GPGX `gfx.c` for 2M Word RAM, with its line cost (3 sub cycles per Word RAM access), halting
   while main owns Word RAM, observation-point synchronization and a completion event raising sub level 1 with `IEN1`. It is tested
   against an independent geometric oracle (logical stamp bitmaps, sampled through the trace vectors, written into the cell-column image
   buffer) for identity, magnification, rotations, flips, rolls, repetition, clipping, all four priority modes, 16x16 and 32x32 stamps.
   1M mode, byte accesses and parameter reads stay typed stops. Two documented deviations from GPGX: `$58` stores only the three
   control bits (bit 15 is the read-only busy flag), while a `$64` write during an operation sets the remaining lines (the live counter). *Lesson recorded:* the first implementation took the reference's
   `4 * 3` as 12 sub cycles per access; the reference counts sub time in units of 4 per cycle (so 3 cycles). The error was invisible to
   the register-level tests and was found by the end-to-end comparison (the reference ran four times as many operations per frame).
4. **CDD commands.** TOC (request codes 0-6), seek, pause, resume and N-track jump, from GPGX `cdd_process`; scan and tray stay typed
   stops, an invalid TOC request changes nothing.
5. **CDC host reads** (destinations 2 and 3): the host-data register, `DSR`/`EDT`/`DBC`/`DAC` progression and the level-5 transfer end of
   GPGX `cdc_host_r`, without duration. PCM RAM and PRG RAM destinations stay typed stops.
6. **Sub interrupts.** Level 1 is now an accepted source (through the CPU-owned exception core, autovector, acknowledged at acceptance);
   levels 6 and 7 stay typed stops. No source is invented: level 1 arises only from a completed graphics operation.
7. **VDP DMA from 2M Word RAM owned by main** is plain memory for the bus (a second read-only region of the Genesis device host,
   observation-synchronized); 1M, sub-owned and PRG-window sources stay typed stops. Duration stays zero (ADR 0143 decision 6).
8. **Disc-extent images** (`--disc-image main|sub:<track-1 byte offset>:<base>:<length>`, at most four). An extent of the verified
   disc's track 1 is read at build time through the disc reader and becomes an `immutable_input` image with per-instruction byte
   identity, mapped in a processor's RAM, admitted by the same broad AOT as every other image and selected among overlapping variants by
   live bytes (ADR 0142 decision 5). The disc identity is verified at start-up and the extents' bytes are digested into the program
   identity, so another disc is another program; an extent never found in RAM never runs; a live byte that differs stops
   `code_bytes_mismatch` before the instruction. This is the "one more admitted image" ADR 0142 anticipated for the disc SP over
   transient BIOS code, and it covers the disc's boot area (entered by the BIOS at its base in main work RAM) and an SP-loaded module.
   The extents are build inputs found by local analysis of the executed frontier; they are not recorded here.
9. **Application entry** (`--application-entry <address>`): the first main-processor instruction start at that address ends the run with
   the typed stop `application_handoff` before it runs. It is a build input in the program identity, never a runtime constant, and 0
   disables it. The reference defines the application IP as following the disc's security block (contract section 13); for the authentic
   US disc the entry is the boot area's base plus the $584-byte security block.
10. **24-bit program counter.** The generated per-processor dispatch masks the pc to 24 bits (the 68000 address bus). A jump through a
    sign-extended address selected no entry before; the authentic BIOS enters the disc's boot area that way.
11. **Local diagnostics** (`--dump-dir`, `--coverage`, `--access-trace`, `--event-log`): RAM images, executed instruction starts, run-length
    coded Gate Array accesses and device events of a stopped run, for comparison with a reference. They change nothing in the machine and
    are never durable evidence (a report's `sub_devices` block is the observable device state).

Everything else is as in ADR 0142/0143: operand templates through the bounded fixed point (one frontier per pass), exact executable-byte
guards, deterministic variant selection, typed stops for anything without evidence.

## Result (SEG-064-T004)

**Chain of frontiers, in the order the generated authentic program reached them** (each forced by the executed stop; each resolved by
the decision above): stopwatch; the sub's RAM jump-table thunks (ten operand templates) and the main BIOS's two (decision of ADR 0143);
PCM writes; graphics start; CDD TOC; VDP DMA from Word RAM; CDC host read; the entry to the disc boot area (decision 10); the SP
replacing transient code (decision 8); a module the SP loads (decision 8); the application entry (decision 9).

**Authentic run** (authorized US Model 1 class BIOS and Sonic CD (USA) disc, local only, no input; official driver, cold, gcc -O1 strict
C11, 8 jobs): epoch + 12 operand templates (2 main, 10 sub), **14 rounds + the confirming run, 528 s**; 151,712 instruction functions,
144 MB of C in 37 translation units; peak compiler resident set 579 MB, emission 439 MB. Images: BIOS 61,869 compiled starts; disc boot
area 16,081; disc SP 14,331; disc module 1,994; sub epoch 64,628. The run ends in the typed stop `application_handoff` with the main
processor at the application entry after about 1,198 video frames of machine time.

| check at the handoff (private GPGX reference at the same event, no input) | result |
| --- | --- |
| main processor: 19 registers and SR | **identical** |
| sub processor: 19 registers, SR, stack pointers | identical except the pc, which is two instructions apart inside a polling loop |
| main work RAM | 28 of 65,536 bytes differ, none in the disc boot area |
| PRG RAM | 44 of 524,288 bytes differ, none in the SP image |
| Word RAM | identical |
| executed instruction starts, main | 1,531 vs 1,532: the same set (the reference's count includes the entry instruction itself, which this run stops before) |
| executed instruction starts, sub | 3,479 vs 3,479: the same set |
| sub-side and main-side Gate Array access classes (register, width, direction; 86 classes) | 67 classes have equal totals (every graphics, CDC host-data, timer and mode transaction); the other 19 are the polled registers (below) and the CDD command exchange (1,413 vs 1,416: the reference reaches the entry about four frames later) |
| determinism | report, device-event digest and both execution-trace digests identical over a repeat and 12 seeded deadline partitions |

The residual differing bytes are confined to BIOS data variables (command/status message queues and counters). The reference's boot
reaches the application IP about four frames (0.3 %) after this run and had processed three more 75 Hz CDD exchanges when its main
processor reached the entry, which is the only structural difference. At the boot area's base (a few instructions before the
application entry) the states had matched in the same way: every register of both processors, all work RAM, Word RAM and an identical
executed set on both processors, with 51 differing PRG RAM bytes of the same kind.

*Where the reference's own counts cannot be compared:* the reference stops a processor that polls a Gate Array register in a tight
loop after three reads until the register is written (an idle-skip optimization), so loops that wait for the other processor execute
20x-1600x more often here, with identical resulting state; its VDP DMA takes time (a completion poll iterates thousands of times, here
once). Both are recorded deviations (contract section 12), not defects of either side.

**Independent review.** The adversarial validator returned PASS on `8e6ceaa` (fast gate 121/121 plus the focused suites; the authentic
comparison reproduced independently; 31 runtime mutations each failed a test) with one major finding - the peripheral reset did not
reset the PCM state - and minor ones (seven mutations no test noticed, provenance labelling of the disc extents, a diagnostics bound,
parsing and entry-range checks). All were corrected in one cycle with a test each; the corrected program was rebuilt cold and the
comparison repeated (below).

**Not claimed.** BIOS services executed after the handoff, the Z80, the H-interrupt line counter, CD-DA, 1M Word RAM graphics,
Sonic CD progress, SEG-064 completion, Checkpoint E.

## Consequences

- G-A now has its second evidence: the generated-native authentic BIOS runs both processors from reset to the application IP entry and the
  state there equals the independent reference's. Checkpoint E additionally needs the post-handoff service verification and the
  milestone's own acceptance (a separate decision).
- Synthetic post-BIOS programs (SEG-062/063) are byte-identical in generated C; the BIOS-image synthetic proofs (T002/T003) pass
  unchanged; every changed negative expectation is a feature that exists now and has its positive test.
- The disc-extent mechanism is generic: later tasks that execute disc-loaded code in the BIOS-image profile declare their extents the same way.
