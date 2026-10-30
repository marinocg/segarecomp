# Sega CD machine contract (SEG-062-T001)

Audience: every SEG-062 implementer (T002-T005) and the planners of SEG-063/064/065. This document fixes the observable
behaviour of the one supported Sega CD machine profile (the **synthetic post-BIOS fixture profile**), with a
classification and at least one citation for every hardware claim. Later tasks implement it; they do not re-decide it.
Anything still open is listed in section 13 with the experiment that resolves it and its owner. It is never guessed in
code: where the references are silent, behaviour that depends on it stops with a typed error; where they **disagree**, the
profile follows Genesis Plus GX (project decision, tag **G**) and section 13 keeps the item with its resolving experiment.

Decisions: ADR 0126 (executable images per processor instance), ADR 0127 (overlay dispatch contract) and ADR 0132 (its
implementation and the module lists, SEG-063),
ADR 0128 (dual-CPU execution and scheduling), ADR 0129 (disc resources and the build-time/runtime media boundary),
ADR 0130 (BIOS profiles and the SEG-064 go/no-go gate), ADR 0131 (ML reuse boundary). CPU semantics: the MC68000
library contract (`docs/architecture/m68k-reuse-boundary.md`, ADR 0043) wins on anything CPU-owned. Generic image
contract: `docs/architecture/executable-image-contract.md` (ADR 0077).

## Classification of claims

Every claim carries one tag:

| tag | meaning |
| --- | --- |
| **V2** | verified fact: two independent sources agree (or one source plus a cited hardware-test result that the other does not contradict) |
| **V1** | verified fact from one source; not contradicted; the second check is listed in section 13 |
| **C** | software convention of this project (the synthetic profile); not a hardware claim |
| **T** | target-specific: true of the hardware, but owned by a BIOS or a title, not by the machine |
| **G** | the references disagree; this profile follows GPGX (project decision recorded in ADR 0128). The disagreeing source is named and the item stays in section 13 until hardware evidence settles it |
| **U** | unresolved: sources are silent; the machine stops with a typed error when it is reached |

## References

| key | source | locator |
| --- | --- | --- |
| GPGX | Genesis Plus GX, `core/cd_hw/scd.c`, `cdc.c`, `cdd.c`, `gfx.c`, `core/mem68k.c` | github.com/ekeeke/Genesis-Plus-GX at `49c584764893b0505ac7f768a754f97330fa4392` (read 2026-10-15; non-commercial licence: consulted, never copied) |
| MAME | MAME `src/mame/sega/megacd.cpp` (BSD-3-Clause) | github.com/mamedev/mame at `a245d2685179cf8fbefd2311db65e0c09960e553` (read 2026-10-15) |
| MCDV | Krikzz, *mcd-verificator* hardware test ROM, as cited by GPGX comments ("verified on real hardware, cf. Krikzz's mcd-verificator") | cited second-hand through GPGX; not run by this project |
| BUL | Sega, *Sega-CD Technical Bulletins* (IP at `$200`, SP at `$800` = start of sector 1 in the standard header; IP size limits) | segaretro.org/images/2/2e/Sega-CD_Technical_Bulletins.pdf (located 2026-10-15; text seen through search excerpt only, see section 13) |
| RDEV | Mike Pavone, *Sega CD Development* notes (boot identifiers; longwords `$30`/`$34` = IP start/length, `$200`/`$600` defaults) | retrodev.com/segacd.html (excerpt seen 2026-10-15; host unreachable from the build cloud) |
| MDHW | refinement facts recorded in the SEG-062 backlog notes: IP/SP descriptor tuple `{disc address, load size, entry offset, work RAM size}`, IP in work RAM, SP in PRG RAM, Sonic CD loader structure | harness `docs/backlog/issues/seg-062*.md` (treated as inputs to re-verify, not authority) |
| UM68K | Motorola, *M68000 8-/16-/32-Bit Microprocessors User's Manual*, 9th ed. (exception processing, reset timing) | as already cited by ADR 0043 |
| CDROM | ECMA-130 (CD-ROM physical format): 2352-byte raw sector = 12-byte sync, 4-byte header (BCD minute, second, frame, mode), 2048 user bytes in Mode 1; 75 sectors per second; 150-sector (2 s) lead-in offset between MSF and LBA | ecma-international.org ECMA-130 |

"Two sources" in this document always means two of the rows above that do not derive from one another.

## 1. Profile

| item | decision | tag |
| --- | --- | --- |
| console | Sega CD (Mega-CD) model 1 or 2 attached to a Genesis; main MC68000 + sub MC68000; LC8951 CDC; CD drive controlled through the Gate Array CDD port | V2 (GPGX, MAME) |
| boot | **synthetic post-BIOS fixture profile**: no BIOS byte is present or executed; the runtime establishes the documented post-load placement of IP and SP (section 9) and then runs generated code | C |
| cartridge | none (mode 1 cartridge boot is out of scope) | C |
| TV standard | NTSC timing for the main CPU (MCLK 53,693,175 Hz / 7) | V2 (GPGX `system_clock`, repository Genesis runtime) |
| audio | CD-DA only (BIOS-image profile, SEG-104, section 22): the drive's audible sectors become canonical stereo S16 samples at 44.1 kHz; no RF5C164 sound, FM, PSG or Z80 sound (the PCM chip's CPU-visible running pointers are modelled, SEG-102, section 17); the synthetic profile keeps the typed stop `cd_read_audio_track` | C / V1 (see section 22) |
| graphics ASIC | none: stamp/rotation registers are typed stops | C |
| backup RAM | a machine input (`--backup-ram FILE`, exactly 8 KiB; SEG-065-T002, ADR 0147); without it: typed stop | C (input), V2 (access shape) |

## 2. Processor instances and clocks

The machine owns two **processor instances** of the same instruction-set variant, `CpuVariant::mc68000`
(ADR 0126). Their identity (`main`, `sub`) is a platform enumerator in `platforms/sega-cd`; it never becomes a CPU
variant, an executable-image field or an emitter mode.

| item | value | tag |
| --- | --- | --- |
| main CPU clock | MCLK / 7 = 7,670,453.57 Hz | V2 (GPGX, repository Genesis runtime) |
| sub CPU clock | 12,500,000 Hz (50 MHz Sega CD master clock / 4); the BIOS-image profile runs the sub line-locked, 796 cycles per video line (about 12.497 MHz), as the reference does (ADR 0177 decision 1) | V2 (GPGX `SCD_CLOCK` and `s68k` cycle scaling; MAME `SEGACD_CLOCK 12500000`); line-locked: G (GPGX `scd.cycles_per_line`) |
| CDD interrupt rate | 75 Hz (one CD sector time); in the BIOS-image profile examined at video-line ends (average 209.38 lines, 209 or 210 between ticks) | V2 (GPGX `cdd.cycles` modulus; CDROM); line quantisation: G (GPGX `scd_update`) |
| timer unit | 384 sub-CPU cycles = 30.72 us per count | V2 (GPGX `TIMERS_SCYCLES_RATIO 384*4` SCD cycles; MAME `30720` ns) |
| CDC DMA rate | 4 sub-CPU cycles per byte, transferred in 8-byte groups | V1 (GPGX `DMA_CYCLES_PER_BYTE 16` SCD cycles citing MiSTer logic-analyser logs) |

Integer clock domain (ADR 0128): one **tick** is 1 / 26,846,587,500,000 s, the least common multiple of the three
rates (53,693,175, 12,500,000 and 75 Hz numerators). Exactly: main cycle = 3,500,000 ticks; sub cycle = 2,147,727 ticks;
CDD sector = 357,954,500,000 ticks; timer unit = 384 sub cycles. All machine time is `uint64_t` ticks (190 hours of
range). No floating point is used anywhere in timing.

The BIOS-image profile (ADR 0177) changes two of these: its sub cycle is 2,148,241 ticks (796 per 1,710,000,000-tick line; the
timer unit, stopwatch, CDC DMA and decoder and graphics durations are counted in it) and its drive tick is placed on the video-line
grid: tick `k` after an anchor in line `L0` with `acc0 = -12 x` (sub cycles already run in that line) falls at the end of the first
line `n` with `acc0 + 9,552 n >= 2,000,000 k` (power-on: `acc0 = 0`; a peripheral reset re-anchors). The synthetic profile keeps the
values above.

## 3. Main CPU bus view (24-bit)

"Ignored" writes have no effect (counted in the report as `ignored_writes`); an "open bus" read would return the processor's
prefetched instruction word, which the profile does not model: it is the typed stop `open_bus_read`.

| range | content | behaviour in this profile | tag |
| --- | --- | --- | --- |
| `$000000-$01FFFF` | Boot ROM (BIOS) | absent: a read is the typed stop `bios_absent`; a write is ignored (ROM) | V2 location (GPGX `scd_init`, MAME), C absence |
| `$020000-$03FFFF` | 128 KiB window onto PRG RAM bank `BK1:BK0` (`$A12003` bits 7-6) | mapped while the window is open (below); otherwise reads are open bus and writes are ignored | window and bank: V2 (GPGX `scd.prg_ram + ((data & 0xc0) << 11)`, MAME `scd_4m_prgbank_ram_r`); gate: **G** (GPGX unmaps without `SBRQ`, MAME does not gate) |
| `$040000-$1FFFFF` | the BIOS and the window repeat every 256 KiB | as above | **G** (GPGX `scd_init` "mirrored every 256KB"; MAME maps no mirror) |
| `$200000-$23FFFF` | Word RAM, 2M mode, when owned by main (`RET` = 1) | linear 256 KiB; when the sub owns it: reads open bus, writes ignored | V2 mapping (GPGX, MAME); non-owner behaviour **G** (GPGX `m68k_read_bus_8`/`m68k_unused_8_w`) |
| `$200000-$21FFFF` | Word RAM, 1M mode: the bank assigned to main | linear 128 KiB of one bank (section 6) | V2 |
| `$220000-$23FFFF` | 1M-mode cell-image view | typed stop `wordram_cell_image_unsupported` | C (out of scope) |
| `$240000-$3FFFFF` | mirrors of `$200000-$23FFFF` | as above | **G** (GPGX "mirrored up to $3FFFFF") |
| `$A12000-$A120FF` | Gate Array, main side (section 5); A1-A5 decode, `$A12040-$A120FF` mirror `$A12000-$A1203F` | per register | V2 registers; mirror **G** (GPGX "mirrored up to $A120FF") |
| `$C00000-$C00007` | VDP data/control ports | composed Genesis VDP device (ADR 0128); synthetic profile: VDP DMA enable is a typed stop; BIOS-image profile: section 12 | V2 (Genesis map) |
| `$E00000-$FFFFFF` | 64 KiB work RAM, repeated every 64 KiB | linear | V2 (Genesis map) |
| anything else | | typed stop `unmapped` (the BIOS-image profile maps more: section 16) | C |

**PRG-RAM window gate (G).** The window is mapped at power-on. On every write of `SRES`/`SBRQ` (`$A12001`) that changes the
sub's halt state, the window becomes mapped exactly when `SBRQ` then reads 1; a write that leaves the halt state unchanged
leaves the mapping alone (GPGX `ctrl_io_write_byte` case `0x01`).

## 4. Sub CPU bus view (24-bit)

Only A1-A19 decode: the 1 MiB map below repeats through the whole 16 MiB space (**G**: GPGX `scd_init` "mirrored every 1MB";
MAME maps the first 1 MiB only).

| range (mod 1 MiB) | content | behaviour | tag |
| --- | --- | --- | --- |
| `$00000-$7FFFF` | 512 KiB PRG RAM | read/write; writes below `WP * $200` are **ignored** (write protect) | V2 (GPGX `prg_ram_write_byte`: `address >= WP << 9`; MAME `m_write_boundary = data << 8` words) |
| `$80000-$BFFFF` | Word RAM 2M when owned by sub (`RET` = 0); 1M dot-image view in 1M mode | 2M: linear; dot image: typed stop `wordram_dot_image_unsupported` | V2 |
| `$80000-$BFFFF` while main owns 2M Word RAM | | DTACK is withheld: the sub **waits**; its instruction is abandoned with its registers as before it (bytes written earlier in the same instruction stay) and retried from its start when main writes `DMNA` = 1 in 2M mode. With no such write the machine ends `idle` with the sub waiting (the hang) | wait: V2 (GPGX `s68k_lockup_*`, MAME silent); retry semantics **G** (GPGX `s68k_pulse_wait` / `s68k_clear_wait`) |
| `$C0000-$DFFFF` | 1M mode: bank assigned to sub; 2M mode: nothing | 1M: linear 128 KiB; 2M: reads open bus, writes ignored | V2 mapping; 2M behaviour **G** (GPGX `s68k_read_bus_8`) |
| `$E0000-$EFFFF` | 8 KiB internal backup RAM on the odd byte lane, repeated every 16 KiB of address (cell = A1-A13) | with the machine input: odd byte reads/writes the cell, even byte reads `$FF` and its write is ignored, a word reads `$FF00 \| cell` and its write stores the low byte, a long is two words; without it: typed stop `backup_ram_unsupported` | access V2 (GPGX `bram_read_byte/word`, `bram_write_byte/word`: `scd.bram[(address >> 1) & 0x1fff]`); presence C |
| `$F0000-$F7FFF` | PCM | write state and channel pointer reads (section 17); other reads typed stop `pcm_unsupported` | C |
| `$F8000-$FFFFF` | Gate Array, sub side (section 5); A1-A8 decode (`$FF8000-$FF81FF` repeated) | per register | V2 registers; repetition G |

The sub CPU's exception vectors live in PRG RAM `$000000-$0003FF` (the sub CPU's address 0), so the vector table is
ordinary writable memory: the sub resets from `PRG[0..7]` and takes autovector `24 + level` through `PRG[$60 + 4 *
level]`. V2 (GPGX `s68k` reset/IRQ paths through `s68k.memory_map[0]`; MAME `segacd_map` 0x000000 = PRG RAM).

## 5. Gate Array registers implemented by this profile

Byte addresses; main side `$A120xx`, sub side `$FF80xx`. Where GPGX and MAME disagree the table follows GPGX (**G**,
`core/mem68k.c` `ctrl_io_*` for the main side, `core/cd_hw/scd.c` `scd_read_*`/`scd_write_*` for the sub side). General
rules (G): a main write to a register without a main-writable function is **ignored**; main reads of `$A12030-$A1203F` are
open bus; registers GPGX marks "!LDS and !UDS are ignored" (`$FF8002/3`, `$FF800E/F`, `$FF8030/1`, `$A1200E/F`) act on a
byte write to either half; word-only sub registers (`$FF8008-$FF800D`) take a byte write as the byte duplicated into both
halves; `$FF8068-$FF80FF` is plain storage. Registers of devices this profile does not model - the font registers
(`$FF804C-$FF8057`) and the subcode buffer (`$FF8100-$FF81FF`) - are typed stops; the stopwatch (`$0C`), the graphics ASIC
(`$FF8058-$FF8066`), the CDC host-data port (`$08` words) and the PCM area are implemented by SEG-064-T004 (section 17), and the H-INT
vector (`$A12006` word, which reads and writes the BIOS ROM) by the BIOS-image profile (section 16). Unmodelled shapes
are typed stops
(`gate_array_register_unsupported`, `cdc_destination_unsupported`, `bios_absent`) carrying the side, address, width and
direction.

| reg | main side | sub side | behaviour | tag |
| --- | --- | --- | --- | --- |
| `$00` (hi) | `IEN2` view (bit 7), `IFL2` (bit 0: writing 1 raises the sub level-2 request when `IEN2` = 1, otherwise dropped; writing 0 does nothing) | LED byte (also read at `$FF8006`; a byte write to `$FF8006` stores it too) | | V2 (GPGX `mem68k.c` case 0x00, MAME `scd_a12000_halt_reset_w` `& 0x0100 && irq_mask & 0x04`); `$FF8006` G |
| `$01` (lo) | `SBRQ` (bit 1), `SRES` (bit 0, 0 = reset/halt asserted). `SRES` 0 -> 1 makes the sub take its reset exception from `PRG[0..7]` (every stop level clears); `SBRQ` = 1 halts the sub; writing `SRES` = 0 halts it and reads back `$02`. `SBRQ` reads 0 when the sub is halted while in STOP | `RESET` status: reads 1; writing 0 is the **peripheral reset** (below) | main: V2 (GPGX, MAME); power-on value `$02`: V1 (GPGX `scd_reset`); the STOP readback rule and the peripheral reset: **G** (GPGX "verified on real hardware"; MAME no special case) |
| `$02` (hi) | write-protect `WP` (main writes) | `WP` read-only (sub writes act as memory-mode writes, below) | | V2 (GPGX, MAME `segacd_sub_memory_mode_w_8_15` "no effect") |
| `$03` (lo) | `BK1:BK0` (7-6), `MODE` (2, read), `DMNA` (1), `RET` (0) | `PM1:PM0` (4-3), `MODE` (2), `DMNA` (1, read), `RET` (0); a byte write to `$02` or `$03` is a memory-mode write, a word write to `$02` has no effect | ownership state machine, section 6; `PM` is stored and read by the sub only (it acts on graphics and dot-image writes, not modelled) | V2 for the 2M handoff and 1M swap (GPGX, MAME); the rest **G** (section 6) |
| `$04` | `EDT`/`DSR`/`DD` (bits 7, 6, 2-0) read; low byte reads 0 | `EDT`/`DSR` (7-6), `DD` (2-0) writable (CDC destination) | a write clears `EDT`/`DSR`, re-selects the writer of a started transfer and resets the DMA address `$0A` | V2 destinations (GPGX `cdc_dma_init`, MAME `DMA_PRG 0x0500`, `DMA_WRAM 0x0700`); `$0A` reset: V1 (GPGX citing MCDV); re-selection G |
| `$05` | reads 0 | CDC register address (4 bits) | | V2 |
| `$06`-`$07` | `$06` byte reads the LED, `$07` reads 1 | `$07` byte / `$06` word: CDC register data (section 7) | | V2 (GPGX `cdc_reg_w/r`; register map of the LC8951); main bytes G |
| `$08`-`$09` | bytes read the host-data register's halves; a word read is the host data path | same; a word read or write is the host data path (section 7) | byte reads do not advance a transfer; a byte write is a typed stop `cdc_destination_unsupported`; the register resets to `$FFFF` | G (GPGX `ctrl_io_read_word`/`scd_read_word` case 8: `cdc_host_r`); SEG-064-T004 |
| `$0C` | - (main side: typed stop) | stopwatch, word access only (section 17) | a free-running 12-bit count, one per 384 sub cycles; any word write clears it | G (GPGX itself marks the width "(word access only ?)"); SEG-064-T004 |
| `$0A` | DMA address (read) | CDC DMA destination address (bytes / 8 for Word RAM and PRG RAM); writable during a transfer | | V2 (GPGX `<< 3`, MAME `<< 3`); writes during a transfer G |
| `$0E` | main flag byte (write either byte; a word write stores its low byte) | main flag byte (read) | | V2 (GPGX "D8-D15 ignored", MAME "low bits take priority") |
| `$0F` | sub flag byte (read) | sub flag byte (write either byte, or the low byte of a word) | | V2 |
| `$10-$1F` | command words COMCMD0-7 (read/write) | read-only; a sub write is ignored | main side V2; sub-write **G** (GPGX ignores, MAME stores) |
| `$20-$2F` | status words COMSTAT0-7 (read-only; a main write is ignored) | read/write | | direction V2; main-write **G** |
| `$30-$31` | - | timer: a write of `N` to either byte (or a word's low byte) starts it, `0` stops it; `$31` reads `N`, `$30` reads 0 | expiry every `(N + 1) * 384` sub cycles; level 3 only while `IEN3` | rate V2; the `+ 1` **G** (GPGX citing MCDV test "REG 8030 #7"; MAME uses `N * 384`) |
| `$32-$33` | - | interrupt enables `IEN1`-`IEN6` (bits 1-6 of `$33`, stored as written; `$32` is plain storage) | `IEN2` mirrors to main `$00` bit 7; clearing `IEN1` drops a pending level 1, nothing else | V2 (GPGX case 0x33, MAME `segacd_irq_mask_w`) |
| `$34-$35` | - | fader (write-only, reads 0): bits 15-4 are the CD-DA volume target (section 22); no other effect | | G; ramp and multiplier table from the reference emulator (not hardware-verified) |
| `$36-$37` | - | `$36`: 1 while no audio track plays; `$37`: CDD control, `HOCK` = bit 2 (a word write stores only `HOCK`) | | V1 (GPGX case 0x36) |
| `$38-$41` | - | CDD status RS0-RS9 (read/write storage, set by the drive) | section 8 | V1 (GPGX `cdd_process`), layout V2 with MAME `segacd_cdd_rx_r` map |
| `$58-$66` | - | graphics ASIC (section 17): `$58` control (read: busy in bit 15), `$5A-$64` parameters, `$66` trace base (the write starts an operation) | word access only except the one byte read `$65` (SEG-105, ADR 0249: the live vertical-dot counter, below); every other byte access and every other register read is a typed stop; **deliberate deviation from GPGX**: a write of `$58` stores only the three control bits (GPGX stores the whole word, so a bit-15 write would read as busy; the busy flag is read-only in this model); a write of `$64` sets the live counter (the low byte) also while idle and while an operation runs (as GPGX) | G (GPGX `gfx.c`); the `$58` rule C; SEG-064-T004 |
| `$42-$4B` | - | CDD command TX0-TX9 (bytes stored as written); the word write to `$4A` clears TX8/TX9 and sends the command | section 8 | V1 (GPGX case 0x4a), map V2 (MAME `segacd_cdd_tx_w`) |

**Peripheral reset (G, GPGX `scd_reset(0)`).** A sub write with `RES0` = 0 (`$FF8001` byte, or a `$FF8000` word): `$FF8004`
and every sub register from `$FF8030` up clear and take their defaults (`$0A` = `$FFFF`, `$36` = `$0100`, RS9 = `$0F`, TX0-TX9
= `$FF`); the timer stops; pending interrupts clear; the CDD returns to its TOC state; the CDC resets; the 75 Hz drive clock
restarts from the writing instruction. The communication registers, the memory mode, `WP`, the LED, the flags and the main-side
`IFL2`/`IEN2` view are kept.

## 6. Word RAM ownership

256 KiB. Two modes selected by `MODE` (written by the sub CPU, `$FF8003` bit 2). Besides the readable `DMNA` and `RET` bits
the Gate Array keeps an internal **request latch** set by main `DMNA` writes (GPGX `scd.dmna`). Power-on: 2M, main owns,
`RET` = 1, `DMNA` = 0, no request (V1 GPGX `scd_reset` regs[2] = `0x0001`).

**2M mode** (single owner):

| event | effect | tag |
| --- | --- | --- |
| main writes `DMNA` = 1 | ownership passes to the sub immediately; `RET` reads 0, `DMNA` reads 1; the request latches; a sub waiting for Word RAM resumes; a CDC DMA halted on main ownership continues | V2 handoff (GPGX `mem68k.c` "Word-RAM is assigned to SUB-CPU"; MAME `scd_rammode = 1`); the rest **G** |
| main writes `DMNA` = 0 | no effect | V1 (GPGX "writing 0 to DMNA in 2M mode does nothing") |
| sub writes `RET` = 1 while it owns | ownership returns to main; `RET` reads 1, `DMNA` reads 0; the request clears; a running CDC DMA into 2M Word RAM halts with its progress kept | V2 return; DMA halt **G** (GPGX halts, MAME keeps writing) |
| sub writes `RET` = 0 or 1 while the owner stays the same | no effect | V2 |
| non-owner access | main: reads open bus, writes ignored; sub: waits for DTACK (section 4) | V2 that the access does not complete; **G** for what happens |
| repeated `DMNA` = 1 while the sub already owns | no further effect | V1 |

**1M mode** (two 128 KiB banks, one per CPU, simultaneously accessible):

| fact | tag |
| --- | --- |
| bank `b` word `i` is 2M-space word `2 i + b` (word interleave); switching modes preserves the bytes through this mapping | V2 (GPGX `word_ram_switch`, MAME `segacd_1meg_mode_word_read` `offset*2 (+1)`) |
| the sub CPU sees its bank at `$0C0000-$0DFFFF`, the main CPU sees the other bank at `$200000-$21FFFF` | V2 |
| a sub write that changes `RET` (or switches 2M -> 1M) selects the banks - `RET` = 1 gives main bank 1, the sub bank 0 - and clears `DMNA`; `RET` = 1 also clears the request; a sub write that changes neither `RET` nor `MODE` only stores `PM` | swap V2; naming of the banks and the request handling **G** (GPGX case 0x03; MAME names no physical bank) |
| main writes `DMNA` = 0: the `DMNA` bit sets (a swap is pending until the sub next changes `RET`); main writes `DMNA` = 1: only the request latches | **G** (GPGX `mem68k.c` "writing 0 to DMNA in 1M mode actually sets DMNA bit") |
| 1M -> 2M: with a latched request and `RET` = 0 written, the sub keeps 2M Word RAM (`DMNA` = 1); otherwise `RET` is forced to 1 and main owns it | **G** (GPGX "RET bit set during 1M mode ?") |
| `PM` priority modes | stored; they act only on graphics and dot-image writes (not modelled) | G |

## 7. CDC (LC8951) subset

Registers through `$FF8005` (address, auto-incrementing except at 0) and `$FF8007` (data). The register file, the /INT
logic and the decoder follow GPGX `cdc.c` (V1 where MAME's `lc89510_temp` agrees on names, **G** where it differs):

| register (write / read) | behaviour | tag |
| --- | --- | --- |
| 0 | not emulated: writes have no effect, reads `$FF` (no auto-increment) | G |
| 1 `IFCTRL` / `IFSTAT` | `DOUTEN` (bit 1) enables data output, clearing it aborts a transfer (already transferred bytes stay); `DTEIEN` (6) and `DECIEN` (5) select the /INT sources; `IFSTAT` bits are active-low (`DTEI` 6, `DECI` 5, `DTBSY` 3, `DTEN` 1); a write or an `IFSTAT` read after the decoder active period clears `DECI` | V1 (GPGX `cdc_reg_w` case 1) |
| 2-3 `DBCL/DBCH` | byte count minus one (12 bits) | V1 |
| 4-5 `DACL/DACH` | buffer source address | V1 |
| 6 `DTRG` | when `DOUTEN`: starts a transfer (clears `EDT`/`DSR`, asserts `DTBSY`/`DTEN`) and selects its writer from `$FF8004` | V1 |
| 7 `DTACK` | clears a pending transfer-end interrupt | V1 |
| 8-9 `WAL/WAH`, 12-13 `PTL/PTH` | write/block pointers; every block written to the buffer advances both by 2352 | V1 |
| 10 `CTRL0` | `DECEN` (7) decoder enable (`STAT0.CRCOK` follows it), `AUTORQ` (4), `WRRQ` (2) buffer write | V1 |
| 11 `CTRL1` | `MODRQ` (3), `FORMRQ` (2), `SHDREN` (0: `HEAD0-3` read the sub-header) | G |
| 14 | not emulated: writes have no effect | G |
| read 4-7 `HEAD0-3` | header (or, with `SHDREN`, sub-header) of the last decoded block | V1, CDROM |
| read 8-9 `PTL/PTH`, 10-11 `WAL/WAH` | pointers | V1 |
| read 12 `STAT0`, 13 `STAT1` (0), 14 `STAT2` | `STAT2` = `MODRQ`/`FORMRQ` from `CTRL1`, or with `AUTORQ` `MODRQ` and the sub-header form bit | G |
| read 15 `STAT3` | `VALST` (bit 7) = 0 after a block was decoded; reading sets it back to 1 and clears `DECI` | V1 |
| 15 (write) `RESET` | resets the CDC registers (and clears a pending level 5) | V1 |

**Decoder (G, GPGX `cdc_decoder_update`).** On every 75 Hz drive tick while `DECEN` is set - whether or not the drive is
reading - the CDC decodes one block: `HEAD0-3` take the block header (BCD MSF and mode 1 for a data block read by the drive,
four zero bytes otherwise), `VALST` goes low, `DECI` goes low for the decoder active period (66,750 sub cycles), and with
`DECIEN` the CDC /INT requests level 5 unless a transfer-end interrupt is already asserted. With `WRRQ` the block goes to
the buffer at the new `PT`: 4 header bytes, then for a Mode-1 block the 2048 user bytes; a block with header 0 takes the
Mode-2 layout (with `MODRQ` two copies of the sub-header first), and its data are the 2048 user bytes of the drive's current
LBA when that lies inside the data track. GPGX reads a raw image's sub-header and 2328 bytes there; this profile uses the
cooked-image behaviour for both formats so that the image format never changes machine state (C). Writes past `$4000`
wrap to the start of the 16 KiB buffer.

**DMA.** Destination 7 (Word RAM): 16-bit big-endian words from `DAC` to Word RAM offset `($FF800A) * 8`; `DAC` and the
destination advance as data moves; `DBC + 1` bytes in all (an odd last byte is not transferred); at the end `EDT`
(`$FF8004` bit 7) is set, `DSR` clears, `IFSTAT.DTEI` goes low and with `DTEIEN` the /INT requests level 5. V2 for
addressing (GPGX `word_ram_2M_dma_w`, MAME `SegaCD_CDC_Do_DMA`); rate V1. The writer is latched when it is selected (at
`DTRG`, a `$FF8004` write, or a resume): in 1M mode the sub's bank at that moment; in 2M mode the 2M space, **halted**
while main owns Word RAM (G: GPGX halts, MAME writes) and resumed from the handover tick by main's `DMNA` = 1. Words a
writer moves while the other Word RAM mode is active are lost (G: GPGX's 2M and 1M buffers are only copied on a mode
switch). Partial progress is real (DMA is not atomic): an abort leaves the bytes moved so far and no `EDT` (V1). A `$FF8004`
write during a transfer re-selects its writer (an invalid destination leaves the transfer started with no writer).
**Host reads (SEG-064-T004; destinations 2 = main through `$A12008`, 3 = sub through `$FF8008`; G, GPGX `cdc_dma_init`/`cdc_host_r`).**
At `DTRG` (or a destination write while a transfer is started) the first word of the buffer at `DAC` is loaded into the host-data
register at once, `DSR` is set, `DAC` advances by 2 and `DBC` drops by 2. A word read of `$FF8008` (sub) or `$A12008` (main) by the
processor the destination names returns the register and, unless `EDT` is set, loads the next word the same way; when `DBC` goes negative
the transfer ends exactly as a DMA transfer does (`DBC` = `$FFFF`, `!DTBSY`/`!DTEN` high, `DTEI` active, level 5 on the /INT edge with
`IEN5`) and `EDT` is set while `DSR` stays; the next read clears `DSR`. A read by the other processor, a byte read, or a read after
`DSR` fell returns the register and moves nothing. A word write to `$FF8008` performs the same read (GPGX: "also read on write"). A byte
write is a typed stop. Destinations 4 (PCM RAM) and 5 (PRG RAM) stay typed stops `cdc_destination_unsupported` (PRG RAM addressing is V2
and recorded for SEG-063). Transfers carry no duration (the data is available at once; the reference's own sub-read timing is "not
immediately available", GPGX comment).

## 8. CDD subset

The drive follows GPGX `cdd_update`/`cdd_process` with its default "CD access latency" setting (V1 where only GPGX models
it; **G** where MAME differs). Its 75 Hz clock is anchored at power-on (re-anchored by the peripheral reset); a tick with
nothing to do (no `HOCK`, no command pending, no latency, not playing, `DECEN` clear) has no effect.

| item | behaviour | tag |
| --- | --- | --- |
| power-on | status `TOC` (`$09`: stopped, disc loaded), LBA 0, RS0-RS8 = 0, RS9 = `$F`, TX0-TX9 = `$FF` | V1 (GPGX `scd_reset`, `cdd_reset`) |
| command transfer | the sub writes TX0..TX9 (`$42..$4B`); the command is `TX0 & $F`; the word write to `$4A` clears TX8/TX9 and sends it - the command checksum is **never inspected** | G (GPGX; MAME validates) |
| status checksum | RS9 = `~(RS0 + ... + RS8) & $F` | V1 |
| command 0 (status) | once the latency is at most 3 ticks: RS0 = drive status; while playing (or paused), RS1 selects RS2-RS8: `$F`/0 absolute time (RS1 becomes 0), 1 track-relative time, 2 track number; RS8 = 4 on a data track | V1 |
| command 1 (stop) | status `TOC`, LBA 0, track 0; RS0 = 0, RS1 = `$F`, RS2-RS9 = 0; a pending play or the latency are kept | V1 |
| command 3 (play) | RS0 = `SEEK`, RS1 = `$F`; on the **next tick** the play starts: target LBA = MSF(TX2..TX7 as written) - 150, latency = 12 ticks + `|target - LBA| * 120 / 270000`, status `PLAY` | G (GPGX default latency; MAME has no seek model) |
| commands 5, B, E, F | unknown to the drive: RS0 = status | G |
| command 2 (TOC; SEG-064-T004) | RS0 = drive status, RS1 = the request code TX3; RS2-RS7 the BCD digits of a time, RS8 a flag or number: 0 current absolute time (`LBA + 150`, RS8 = 4 on a data track), 1 track-relative time, 2 current track number (`$0A0A` past the last), 3 total length (end of the last track + 150), 4 first and last track (`$0001`, last), 5 start of track `TX4 * 10 + TX5` (+ 150; RS6 bit 3 on a data track; RS8 the low digit; the lead-out for last + 1, other numbers a typed stop `cdd_command_unsupported`), 6 error (none); an invalid request code changes nothing and computes no checksum | G (GPGX `cdd_process` case 2) |
| command 4 (seek) | like play (RS0 = `SEEK`, RS1 = `$F`), but the pending status is `PAUSE`: the next tick moves to the target and the drive pauses | G (GPGX case 4) |
| commands 6, 7, A (pause, resume, N-track jump) | `PAUSE` (6, A: no audio playing, `$36` high = 1) / `PLAY` (7) in RS0 and the status; RS1-RS8 unchanged; the checksum is recomputed | G (GPGX cases 6, 7, 0xA) |
| other commands (scan, tray) | typed stop `cdd_command_unsupported` | C |
| each tick while latency > 0 | latency - 1, the decoder runs on a header-0 block (section 7) | G |
| each tick while playing | a block of a data track is decoded with header = BCD MSF of `LBA + 150` and mode 1; LBA + 1; crossing the end of a track moves to the next track; reaching an audio track is, in the synthetic profile, the typed stop `cd_read_audio_track`; in the BIOS-image profile (SEG-065-T002) an audio-track block is played as drive state: from the track start the CDD control high byte reads `$00` (audio playing), the decoder runs without a sector, LBA + 1 (since SEG-104 the sector is also produced as audio samples when a host enabled CD-DA output - section 22; the drive's own behaviour is unchanged); past the last track the status becomes `END` (`$0C`) | V1 + CDROM (GPGX `cdd_update`); synthetic audio stop C |
| `HOCK` | after the drive's tick, level 4 becomes pending (taken with `IEN4`) | V1 |

## 9. Synthetic post-BIOS fixture profile

The profile reproduces only the documented **result** of the BIOS boot path for a disc whose boot area is
well-formed. It executes no BIOS code, contains no BIOS bytes and reproduces no security code.

| item | value | tag |
| --- | --- | --- |
| disc identification | system area sector 0 starts `SEGADISCSYSTEM  ` (16 bytes); cooked images start with it, raw images carry it after the 16-byte sync/header | V2 (GPGX `cdd_load` `memcmp("SEGADISCSYSTEM", header, 14)`, RDEV) |
| system area | logical sectors 0-15 of track 1 (32 KiB) | V2 (BUL "first 16 sectors", RDEV) |
| IP descriptor | longwords at `$30` (disc byte address of IP), `$34` (IP size); `$38` entry offset, `$3C` work-RAM size | `$30/$34`: V2 (RDEV, BUL); `$38/$3C`: V1 (MDHW descriptor tuple) |
| SP descriptor | longwords at `$40` (disc address), `$44` (size), `$48` entry offset, `$4C` work-RAM size | V1 (MDHW; BUL "SP at `$800`" agrees with `$40` = `$800` in the standard header) |
| US-BIOS variant (IP spilling past `$600` into sector 1, SP moved to `$1000`) | represented as descriptor data, never as a special case | V2 (BUL "IP start should move ... SP should start at `$1000`", RDEV "if not `$200`/`$600` more sectors are copied") |
| region field | header `$1F0` (Genesis-style region string); decoded and reported, never used to select code | V2 (repository Genesis header contract, GPGX `region_code`) |
| IP placement | IP bytes are mapped at main `$FF0000` (work RAM) | IP in work RAM: V2 (MDHW, BUL excerpt "IP runs on the Main CPU from Work RAM"); base `$FF0000`: **C** |
| main entry | `$FF0000 + IP entry offset`, SR = `$2700`, SSP = `$FFFE00` | C (the real BIOS enters after the security code) |
| SP placement | SP bytes are mapped at sub PRG RAM `$006000` | SP in PRG RAM: V2 (MDHW, BUL excerpt); base `$6000`: **T** (BIOS-owned; V1 from MDHW; SEG-064 re-verifies) |
| sub initial state | held in reset with the bus requested (`$A12001` = `$02`), so the main program starts it (writes the reset vectors at `PRG[0..7]` through the window, then sets `SRES` = 1, `SBRQ` = 0) | power-on register value V1 (GPGX); the start-up protocol is C |
| everything else | zero-initialized | C |

The profile loader in the runtime reads IP and SP from the **external disc** through the strict-C11 sector reader at
start-up (after the disc identity check), exactly as a BIOS would; the generated program carries their bytes only as
the byte-identity verification constants of compiled instructions (section 11).

### 9a. Disc identity (ADR 0129)

The disc identity is the lowercase hex SHA-256 of this normalized text (one line each, `\n` terminated, decimal numbers):

```text
segarecomp.disc.v1
file <index> size <bytes> sha256 <hex> name <name as written in the CUE>      (one per FILE, in CUE order)
track <number> <mode1_2048|mode1_2352|audio> file <index> offset <byte offset of INDEX 01> sector <2048|2352> pregap <sectors> lba <first LBA> count <sectors>[ index0 <sectors>]
total <sectors including pregaps>
```

LBAs are assigned in CUE order: each track starts after the previous track plus its own `PREGAP` (not stored in the file)
plus its own stored `INDEX 00` pregap (SEG-063-T003: the file sectors from `INDEX 00` to `INDEX 01`); a track's program area
ends where the next track of the same file begins (its `INDEX 00` when present, else its `INDEX 01`) or at the end of the
file. Stored pregap sectors are addressable disc time but belong to no track's readable area. ` index0 <sectors>` is written
only when non-zero, so the identity of every disc without a stored pregap is unchanged. `INDEX 00` must precede `INDEX 01`
once, is exclusive with `PREGAP` (`pregap_conflict`), is not supported on track 1 (no addressable time before LBA 0), and
ordering errors are `index_order`; `INDEX 02` and later remain `cue_unsupported`. Changing any byte of any file, any name, or the layout changes the
identity. The generated program stores the per-file size and SHA-256 and the track table and refuses a disc whose files do
not match at start-up.

## 10. Interrupts

| level (sub) | source | implemented | tag |
| --- | --- | --- | --- |
| 1 | graphics completion (SEG-064-T004) | yes: raised at the end of an operation only with `IEN1` (section 17); cleared at acceptance | G (GPGX `gfx_update`) |
| 2 | main `IFL2` | yes; acknowledging clears `IFL2` | V2 (GPGX `scd_68k_irq_ack`, MAME HOLD_LINE) |
| 3 | timer | yes | V2 (rate), V1 (`+ 1`) |
| 4 | CDD (`HOCK`) | yes | V1 |
| 5 | CDC (`DTEI`/`DECI` /INT edge with `IEN5`, section 7) | yes | V1 / G |
| 6 | subcode | never raised; a request is the typed stop `interrupt_source_unsupported` | C |

All sub interrupts are autovectored (vector `24 + level`); the handler is the long at `PRG[$60 + 4 * level]` and must be
a compiled entry of the sub's image set, otherwise the acceptance stops with `interrupt_handler_not_compiled`.
Acceptance and the exception frame are the CPU-owned rules of ADR 0043 (`exception_core.h`). Main CPU interrupts
(VDP, external) are not wired in this profile: the main vectors live in the absent BIOS ROM, so any main exception
stops with `bios_absent`. The BIOS-image profile wires the VDP's level 6 (section 16).

## 11. Execution and verification rules

- Both processors execute only generated-native C compiled from their own `ExecutableImageSet` (ADR 0126).
- A computed control transfer (`RTS`, `RTE`, `JMP (An)`, a reset or interrupt vector) selects among the processor's
  compiled entries; an address without one is the typed stop `entry_not_compiled`. Inside an overlay group (section 15) the
  address is necessary but not sufficient: the next dispatch selects the image.
- Images mapped in writable memory carry `ImageVerification::byte_identity`: before a compiled instruction runs, the
  live bytes at its address are compared with the image bytes (comparison only; nothing is decoded). A mismatch is the
  typed stop `code_bytes_mismatch`.
- Every run takes a finite `--instruction-budget`; exhaustion is the typed result `budget_exhausted`, not success.
- Success is `idle`: both processors are stopped (or the sub is halted or waiting for Word RAM) and no enabled source can
  wake either.

## 12. Timing approximations (recorded deviations, not hardware proof)

- A bus access takes effect at the start time of its instruction; there is no Word-RAM contention modelling, and the synthetic profile
  has no wait-state, refresh or DMA modelling. Cross-CPU ordering is exact only at instruction granularity. The BIOS-image profile adds
  the main processor's bus conventions of ADR 0177 (below).
- GPGX evaluates the 75 Hz drive clock, DMA progress and DMA completion at its emulation-slice boundaries; this profile
  evaluates them at their exact tick (DMA: 4 sub cycles per byte counted from the transfer's base tick, rounded up to whole
  bytes and down to 8-byte groups; completion at the first tick where the whole count is reached). Every Gate Array CDC/DMA
  access, Word RAM access and ownership change first brings a transfer up to its instruction's start tick.
- The CDD seek latency is GPGX's default model (section 8), not measured drive behaviour.
- `SBRQ` and the sub's wait for Word RAM take effect at instruction boundaries (the real acknowledge latency is not modelled).
- Graphics ASIC (SEG-064-T004): a line is rendered whole as soon as it starts (GPGX `ceil`), at the first observation point (a Word RAM
  access, an ownership change, the end) rather than progressively; the Word RAM access contention of the ASIC and the sub is not modelled.
- GPGX stops a CPU that polls a Gate Array register in a tight loop after three reads until the register is written (its idle-skip
  optimization); the reference's execution counts of such loops are therefore not comparable with a full-rate execution, which this
  runtime performs (SEG-064-T004: observed as 20x-1600x differences in a handful of poll loops, with identical resulting state).
- The CDD 75 Hz clock ticks at its exact instant in the synthetic profile; GPGX processes it at line boundaries, which the BIOS-image
  profile does (SEG-064-T004 observed the reference processing one tick more at the same main instruction; ADR 0177 decision 1).
- BIOS-image profile: a VDP fill takes the slot model's time and its busy status bit lasts as long (ADR 0177 decision 6; SEG-064-T003
  had no duration, observed as the only per-address execution-count difference up to the sub release, ADR 0143). A 68K->VDP transfer
  (SEG-065-T002, ADR 0147) writes its data at the arming write but **freezes the main processor** for the transfer's duration by
  GPGX's slot model (`vdp_dma_update`): per line 16 (H32) / 18 (H40) access slots in the active display and 166 / 204 while blanked
  (vertical blank or display disabled), a VRAM word taking two slots, a CRAM/VSRAM word one with 161 / 198 on a blanked line; the
  frame starts at the vertical-blank onset, its blank window lasts 37 lines, an active-display transfer runs to the end of its line and
  resumes at each line start; the destination class is GPGX's (a code with CD2 or CD1 set moves a word per slot, otherwise a word per
  two slots). The arming instruction's own cycles follow the transfer when it ends inside its first window; a transfer spanning several
  windows re-anchors the processor at its end (GPGX sets the 68K clock to the end again at the next line start), so the next
  instruction starts exactly there. Afterwards the length registers are 0 and #21-#22 have advanced by the length (16-bit); the low
  word of a long control-port write whose high word started the transfer reaches the VDP after that update (GPGX `cached_write`). The data itself is not spread over the lines, so a frame displayed while a
  transfer is still in progress could differ from hardware. The frame capture's `vram_display_changes` is an endpoint comparison only
  (VRAM at the start of active display against VRAM at the vertical-blank onset, byte for byte; it is not a VRAM-write trace and does
  not count writes that restore the original byte or changes that do not survive to the onset); it was 0 at every compared frame, where
  the canonical frames independently equal the reference.
  The vertical-blank onset schedule of the BIOS-image profile is the NTSC schedule with the phase of ADR 0177 decision 4: the first
  onset 111,858 master clocks after power-on, the vertical interrupt 788 (H40) / 770 (H32) master clocks after each onset.

- **BIOS-image profile, main-processor and VDP-port conventions (SEG-071-T002, ADR 0177; rule G = the reference's own source, hardware
  measurement cited there).** The main processor's refresh delay (2 cycles when an instruction is about to execute at or after the
  deadline, deadline +128 cycles; a long shift/rotate or MOVEM.L moves the deadline on by a period when it reaches it - the class and its cycles are a build-time
  record of each verified executable variant, written by the generated instruction after its byte guard (the count register is read
  before the instruction executes) and committed by the scheduler only for an instruction that retired; the runtime never classifies
  guest bytes; division not modelled); one more cycle per main access to the Z80 window; the data-port FIFO (a stall until the oldest of four entries is read out;
  full/empty status bits; emptied at each frame's start) and the VRAM-fill busy bit; one instruction executes before a vertical
  interrupt enabled by a register-#1 MOVE (not MOVE.L) is recognised; an autovector interrupt exception takes 50..59 cycles by the
  processor's cycle count within the frame mod 10 (both processors); a VDP status read is taken 8 cycles after its instruction starts.
  Not modelled: the reference's idle-skip of a Gate Array poll loop and its line-chunk processor synchronisation; the VBLANK / HBLANK / odd-field status bits and the HV counter keep the Genesis host's old 224-line phase; the sub cycle is an integer (drift ~0.2 tick per cycle against exact per-line locking).

## 13. Unresolved items

Items marked **G** above are implemented as GPGX behaves; they stay here until hardware evidence confirms or corrects them.

| item | why | experiment that resolves it | owner |
| --- | --- | --- | --- |
| upper byte of the 68000 program counter after a jump through a sign-extended address (the reference keeps `$FFxxxxxx`, so `LEA (d16,PC)` yields it; the generated dispatch masks to 24 bits, ADR 0144 decision 10): no effect found in the SEG-064-T005 interval (the value is only used as a bus address) | the real silicon behaviour is not established | a hardware test (store `LEA (d16,PC)` after a sign-extended `JSR`) | SEG-065 |
| main PRG-RAM window access while `SBRQ` = 0 (G: unmapped) | GPGX unmaps it, MAME allows it | hardware test (mcd-verificator class) or a third reference (BlastEm, MiSTer RTL) | SEG-064 |
| timer period `N` vs `N + 1` counts (G: `N + 1`) | GPGX (citing MCDV) and MAME disagree | run MCDV "REG 8030 #7" or read MiSTer RTL | SEG-065 |
| which 1M bank is "0" for a given `RET` (G: `RET` = 1 gives main bank 1) | single source | MiSTer RTL / hardware test | SEG-065 |
| sub writes to COMCMD and main writes to COMSTAT (G: ignored) | GPGX ignores, MAME stores | hardware test | SEG-064 |
| address mirrors (main 256 KiB / Word RAM / Gate Array, sub 1 MiB) (G: mirrored) | GPGX mirrors, MAME does not | hardware test | SEG-065 |
| CDC DMA into 2M Word RAM owned by main (G: halted until `DMNA`) | GPGX halts, MAME writes | hardware test | SEG-063: not resolvable without hardware access; carried forward |
| 1M `DMNA` request semantics and 1M -> 2M ownership (G) | GPGX models a request latch; MAME does not | hardware test (MCDV memory-mode tests) | SEG-065 |
| `SBRQ` readback when the bus is requested while the sub is in STOP (G: reads 0) | GPGX: "verified on real hardware", MAME: no special case | hardware test | SEG-064 |
| peripheral reset scope (G: GPGX `scd_reset(0)`) | GPGX itself marks it "TODO: figure what exactly is reset" | hardware test | SEG-064 |
| open-bus read values | the prefetched instruction word; not modelled in the synthetic profile (typed stop `open_bus_read`); the BIOS-image profile uses GPGX's model where the prefetch position is exact (section 16), V1 (GPGX; hardware bus capacitance agrees for the cases used) | hardware test of open-bus reads (empty slot, BUSACK unused bits) | SEG-065 |
| H-INT vector register `$A12006` (BIOS-image profile: the boot ROM's `$72` word) | GPGX writes the ROM copy's `$72` word; one source in reach | MAME `megacd.cpp` H-INT vector handling or a hardware test | SEG-064 |
| VDP vertical-interrupt pending rule (G: pending at every onset, requested while IE0, cleared by the acknowledge) | GPGX `vint_pending`; the repository Genesis runtime raises it only while IE0 is set | hardware test (enable IE0 inside VBlank) | SEG-064 |
| VDP DMA duration and the Word-RAM/PRG DMA source (BIOS-image profile: 68K->VDP transfers freeze main by GPGX's slot model since SEG-065-T002, fill without duration, data written at once; 2M Word RAM owned by main is read as plain memory since SEG-064-T004, with GPGX's one-word source delay (the first VRAM/CRAM/VSRAM word is skipped, the destination advances by the auto-increment, one word fewer is transferred, the source register advances by that count; titles that stream graphics from Word RAM, such as Earthworm Jim SE, depend on it; a one-word transfer is the typed stop `vdp_dma_unsupported`); the 1M bank assigned to main, register #23 = $10 (SEG-103, ADR 0238/0239): the same source delay, the bank read through the ordinary main view with the VDP's 128 KiB window wrap, staged for the one synchronous transfer; sub-owned 2M, the 1M cell image `$220000-$23FFFF`, the mirrors and PRG-window sources a typed stop; a RET/MODE change during the stall and hardware proof of the delay in 1M mode are not modelled) | one source for the slot counts (GPGX `vdp_ctrl.c`, its table cites hardware measurements); GPGX lags Word RAM transfers one word | MiSTer logs / hardware test | SEG-065-T002: the duration is causally observable (the title animation cadence); with it the compared title frames are identical to the reference |
| stopwatch byte accesses and byte-write semantics (G: typed stop) | GPGX: "(word access only ?)" and its byte path is an artefact of its register file | hardware test | SEG-065 |
| graphics ASIC: 1M Word RAM operation, byte accesses other than the `$FF8058`/`$FF8065` reads, word reads of `$FF8064` (high byte unverified: GPGX keeps a written value, MAME clears it), other parameter reads, the font registers (typed stops); line cost `3 * (4 + 2W + ((W + off + 3) >> 2))` sub cycles (G) | one source (GPGX `gfx.c`; its comment cites MiSTer logs) | MiSTer logs / hardware test | SEG-065 |
| CDD seek latency, command checksum handling, decoder active period (G) | GPGX models them (latency is a configurable approximation); MAME differs | logic-analyser logs (MiSTer `mcd logs`) | SEG-065 |
| decoding a non-data block from a raw image | GPGX's raw path differs from its cooked path; the profile uses the cooked behaviour (section 7) | hardware: what the CDC writes for a seek-time block | SEG-063: not resolvable without hardware access; carried forward |
| system-header longwords `$38/$3C/$40-$4C` | one primary source in reach of this cloud | read the Mega-CD Disc Format specification and the Sega CD BIOS manual directly | SEG-064 |
| SP base `$6000`, IP base `$FF0000` | BIOS-owned facts | SEG-064-T001 (ADR 0141): with an authentic US Model 1 BIOS in the independent reference, the disc boot area from `$200` (security block first, then the IP) is placed at main `$FF0000` and entered there, the application IP following the `$584`-byte US security block; the SP module is placed at sub `$6000` and entered through its module jump table. V1 (one reference); the synthetic profile keeps its convention (section 9); an authentic profile would not hard-code either, the BIOS performs the placement | deferred with SEG-064 (ADR 0141 `NO_GO`) |
| raw-sector EDC/ECC | not checked; sync and header are | decision: optional until a title needs it | SEG-063: still optional (no fixture or title needs it); carried forward |

## 14. What later milestones consume

- SEG-063 (modules and overlays) consumes sections 4, 6, 7 and ADR 0127; it adds section 15 and ADR 0132.
- SEG-064 (authentic BIOS) consumes sections 3 (BIOS ROM), 9 and ADR 0130.
- SEG-065 (graphics ASIC, PCM, CD-DA, viewer) consumes sections 5 (registers left as stops), 10 and 13.

## 15. Disc-loaded modules and overlays (SEG-063, ADR 0132)

| item | rule | tag |
| --- | --- | --- |
| admission | only an explicit module list (`segarecomp.sega_cd.modules.v1`): disc identity, ISO9660 path and/or track-1 extent (must agree), SHA-256, processor, execution bases, entry roots, `byte_identity` | C |
| ISO9660 | build time only; one PVD in a terminated descriptor set from block 16, 2048-byte blocks, single-extent files; everything else a typed error (`libs/media/iso9660.hpp`) | V1 (ECMA-119) subset C |
| executable regions | main: PRG-RAM window `$020000-$03FFFF`, Word RAM `$200000-$23FFFF`, work RAM `$FF0000-$FFFFFF`; sub: PRG RAM `$000000-$07FFFF`, 2M Word RAM `$080000-$0BFFFF`, 1M bank `$0C0000-$0DFFFF`; a mapping lies inside one | C (bus maps of sections 3-4) |
| candidates and groups | one candidate per (image, mapping); overlapping candidates with a module form a group; at most 16 groups, 16 candidates per group | C |
| selection | the one candidate covering PC whose whole range equals live memory (64-byte granule digests through the processor's current view); no other live candidate may overlap it | C (ADR 0127) |
| invalidation | CPU stores, CDC DMA words and exception frames mark granules; PRG bank/window and Word RAM owner/mode changes mark whole groups; revalidation before the group's next instruction | C |
| re-entry after replacement | until the next selection, only at a declared root, else `stale_image_selection`; afterwards the selected (verified) image's entries run | C |
| typed stops | `image_unknown`, `image_bytes_mismatch`, `image_partially_overwritten`, `ambiguous_image`, `image_entry_not_compiled`, `stale_image_selection` | C |
| code fetch from Word RAM the sub does not own | fails closed (no live candidate / `code_bytes_mismatch`); hardware would withhold DTACK | C (recorded deviation) |
| CDC DMA into PRG RAM (destination 5) | still `cdc_destination_unsupported`; modules reach PRG RAM by CPU copies | C |

## 16. BIOS-image profile (SEG-064-T002, ADR 0130 Amendment 1, ADR 0142)

A second, explicitly selected profile: the synthetic mechanism proof (SEG-064-T002) and, with a locally held BIOS, the authentic main
BIOS up to the sub runnable epoch (SEG-064-T003, ADR 0143). It changes nothing in sections 1-15 for the
synthetic post-BIOS profile.

| item | rule | tag |
| --- | --- | --- |
| selection | `segarecomp sega-cd emit --bios <image>`; the program requires `--bios <image>` at run time and refuses a missing, wrong-size or different image (SHA-256) before any instruction (`bios_refused`) | C |
| BIOS image | at most 128 KiB, mapped read-only at main `$000000` (the section 3 256 KiB mirroring applies); a read past its end is `bios_absent`; main resets from its `$000000`/`$000004` vectors and takes every exception vector from it | location V2 (section 3); C otherwise |
| images | the BIOS image (`immutable_input`, unguarded), declared verbatim ROM copies (`static_proof`), sub runnable-epoch images (`bounded_build_time_materialization`); each admitted by broad AOT over every aligned start; RAM images verified per instruction | C (ADR 0142) |
| sub runnable epoch | the sub's first runnable transition after a reset (`SRES` 0 -> 1 with `SBRQ` = 0, or `SBRQ` 1 -> 0 after such a reset); live PRG RAM `[0, E)` (E: last non-zero byte rounded up to 256) must equal an admitted epoch image, else `materialized_image_unverified` before the sub retires anything; the sub's reset exception (vector fetch) is taken there, from the verified bytes | rule C; the fetch timing U (GPGX fetches at the `SRES` write; differs only when the reset is released with the bus requested) |
| overlapping RAM images | per-PC variants, one per distinct instruction; a variant runs only when all its bytes are live; else `code_bytes_mismatch` | C |
| disc images (SEG-064-T004) | `--disc-image main\|sub:<track-1 byte offset>:<base>:<length>`: an extent of the verified disc's track 1 (cooked 2048-byte blocks; the 32 KiB system area is offsets 0-$7FFF) read at build time through the disc reader and mapped at `<base>` of the processor's address space as an `immutable_input` image with per-instruction byte identity. Accepted bases: main work RAM or the main 2M Word RAM view `$200000-$23FFFF` (SEG-071-T001, ADR 0176); sub PRG RAM `$000000-$07FFFF` or the sub's linear 2M Word RAM view `$080000-$0BFFFF`. An extent lies wholly inside one of these (it may start at the first and end at the last byte); the 1M bank windows, the PRG-RAM window and a region of the other processor are refused (`disc_image_invalid`). At most 16 (4 until SEG-064-T005); the extents are part of the program identity (bytes digested), so another disc or another declaration is another program; an extent the program never finds in RAM never runs | C (authority: the disc identity is verified at start-up and every instruction's bytes are compared before it runs; ADR 0144, ADR 0145 decision 3) |
| Word RAM executable views | Word RAM is one 256 KiB array with one owner at a time under `RET` (section 6: 2M mode, `RET` = 1 main, `RET` = 0 sub). **Main** executes it in place at `$200000-$23FFFF` only while it owns it (2M with `RET` = 1, or the 1M bank main's view shows); **the sub** executes it at `$080000-$0BFFFF` only while it owns it (2M with `RET` = 0). Both views are the same bytes, so code one processor placed is visible to the other after the hand-over, never during it. A fetch from a Word RAM view the processor does not own does not complete as an instruction: the live view has no bytes (`scd_code_byte` returns none), so the compiled entry's byte guard fails closed with `code_bytes_mismatch` at that pc before anything retires (data accesses by the non-owner keep section 3/4 behaviour: main reads open bus, the sub waits). A declared extent is only a build-time statement that these disc bytes may be the program at that address: it never gives a processor access, never changes `RET`/`DMNA`, and never replaces the per-instruction comparison with the live bytes - a data write into an image that touches no instruction byte leaves the entries valid, a changed instruction byte stops the processor before that instruction. Without a matching extent the pc is `entry_not_compiled`; an extent whose bytes differ from the live ones never runs. Earthworm Jim SE (sub code loaded into Word RAM by the sub) motivated the sub view; nothing here is title-specific | C (bus maps of sections 3-4, ownership of section 6; ADR 0176; `scd_machine.c` `scd_code_byte`) |
| application entry (SEG-064-T004) | `--application-entry <address>`: the first main-processor instruction start at that address (24-bit) ends the run with the typed stop `application_handoff` before the instruction runs, but only after a verify-only pass of the generated dispatch (compiled entry exists; the whole instruction's live bytes match an admitted variant; nothing retires or changes) - otherwise `entry_not_compiled` or `code_bytes_mismatch`; the entry is part of the program identity; absent (0) = no boundary. It is a build input, never a runtime constant: the authentic US Model 1 boot area is entered at its base by the BIOS and its security block (disc-supplied code) hands over to the application IP after $584 bytes (ADR 0141), so the IP entry of that arrangement is base + $584; the address must be even and in Word RAM or work RAM ($200000-$FFFFFF) | C; the $584 offset V1 (SEG-064-T001 reference) |
| local diagnostics (SEG-064-T004, T005) | the program driver's `--dump-dir`, `--coverage` (executed instruction starts, and since SEG-064-T005 the distinct control-flow edges - a processor's consecutive instruction starts - in `<file>.edges`), `--access-trace` and `--event-log` write RAM images, executed instruction starts, run-length coded Gate Array accesses and device events of the stopped machine; the report's `bios_profile` also carries `vblank_onsets`; they change nothing and are never durable evidence (for a real BIOS they are derived data and stay in ignored storage) | C SEG-079: `--ram-write-log` writes 16-byte records of the main processor's BIOS-window reads and PRG/work RAM writes and the sub's PRG RAM writes with the writer PC (a local, ignored diagnostic outside the program identity; `tools/sega_cd_epoch_provenance.py`, `tools/sega_cd_template_provenance.py`). |
| 24-bit program counter | the generated dispatch and the computed-control entry lookup mask the processor's pc to 24 bits (the 68000 address bus): a jump through a sign-extended address such as `$FFFFxxxx` selects the compiled entry at `$00FFxxxx` | V2 (UM68K) |
| operand templates | `JMP (xxx).W/.L` admitted from a build-pass RAM frontier: opcode word guarded, operand read live, destination an even compiled entry of the processor, else `entry_not_compiled`. SEG-079-T002 (ADR 0180): one admission may be a bounded run (at most 128) of contiguous `JMP (xxx).L` slots at a 6-byte stride with an even operand; every slot is guarded and checked individually | C |
| main Genesis devices (SEG-064-T003, ADR 0143) | `$A00000-$A0FFFF` Z80 RAM, `$A10000-$A1001F` I/O, `$A11100`/`$A11200` Z80 BUSREQ/RESET, `$C00000-$C0001F` VDP (PSG at `$C00011`, HV counter) are the Genesis runtime device host's, its master clock = machine time / 500,000 ticks; version register bit 5 (DISK) reads 0; an I/O word read returns the register byte on both lanes; BUSACK's other bits are open bus; unsupported shapes stop `main_device_unsupported` | version bit V2 (GTO1, GPGX `io_reset`); BUSACK bits V1 (GPGX); routing C |
| Z80 | never executed: BUSREQ/RESET and RAM writes only; once the Z80 has been runnable, a main read of the Z80 area stops `main_device_unsupported`; what a running Z80 would do through its 68000 bank window is not modelled (recorded deviation) | C |
| VDP DMA | fill: the device's; 68K->VDP from the BIOS image or main work RAM (the host is fed from the Sega CD memory); Word RAM / PRG-window sources `vdp_dma_unsupported`; copy fail-closed; no duration (section 12) | sources V2 (GPGX, MAME); duration deviation C |
| main interrupts | vertical-blank onset every frame (device event, ADR 0177 decision 4: first onset 111,858 master clocks after power-on); VDP level 6 pending a fixed delay after every onset, requested while register #1 IE0 is set, cleared (with status F) by the acknowledge; accepted through the BIOS vectors (CPU-owned exception core); a register #0 write setting IE1 (H-interrupt) stops `interrupt_source_unsupported` at that write | pending rule G (section 13); acceptance V2 (UM68K) |
| H-INT vector `$A12006` | word register (initial `$FFFF`): written by main, read back; the level-4 autovector is `$FFFF:<register>`; reads of BIOS `$70`/`$71` return `$FFFF` and of `$72`/`$73` the register | V1 (GPGX `scd_reset`, `mem68k.c`; section 13) |
| open bus | a main read nothing answers (empty cartridge slot `$400000-$7FFFFF`, unused areas) returns the instruction-stream word at the prefetch position (byte by A0; both halves of a long), stated per generated instruction where exact; otherwise, for the sub and in the synthetic profile `open_bus_read`; empty-slot writes are ignored | V1 (GPGX `m68k_read_bus_8/16`, section 13) |
| exception into uncompiled RAM | the handler is entered and the dispatch stops `entry_not_compiled` at its PC (a build-pass RAM frontier, ADR 0142 decision 7) | C (ADR 0143 decision 9) |
| sub-side boot hardware (SEG-064-T004) | the stopwatch, PCM write state, graphics ASIC with its level-1 interrupt, the CDD TOC/seek/pause/resume/jump commands and the CDC host-data transfer (sections 7, 8 and 17); 2M Word RAM owned by main as a VDP DMA source | G; C for the routing |
| application handoff and after (SEG-064-T005) | without `--application-entry` the application runs through normal generated-native dispatch (section 18); `--until-tick <ticks>` ends a run at a machine-time instant with the result `deadline` (exit 11, no typed stop, resumable: identical under any deadline partition) | C (ADR 0145) |
| timed standard-pad input (SEG-070-T002, ADR 0149) | `--input-script FILE` (optional; `--input-log FILE` writes the observation log): a text script of `<tick> <port> <press|release> <BUTTON>[,...]` lines under the header `segarecomp-input-script 1`, ticks absolute since reset (500,000 per master clock, 448,020,000,000 per NTSC frame), validated whole before any guest instruction (`input_refused`, exit 23, with the refusal class and line; also refused in the synthetic profile); a transition is a device event that sets the host's pad mask, processed before every other device event and before any processor instruction or interrupt acceptance at or after its tick, in script order, whatever the deadline partition; the Genesis controller pin model serves the pad (TH multiplexing, CTRL direction, data latch, 0 = pressed), so a processor access observes the state in force at its instruction's start tick; without the option no pad is ever pressed and every report is unchanged; with it the report gains an `input` object (events, applied, pad masks, data-port byte reads per port, observation count and FNV digest of the log) | rule C; protocol V2 (GTO1 I/O section, the Genesis hardware notes cited by the controller-I/O policy); the 3-button pad only (6-button and multitap protocols not modelled); a press the independent reference schedules at "frame entry + N" takes effect at the onset that opens that frame (ADR 0176 decision 3; onset numbering equals the reference's frame numbering, ADR 0177 decision 4) |
| not modelled in this profile yet | the H-interrupt line counter, Z80 execution, VDP DMA from sub-owned 2M Word RAM, the 1M cell image and the mirrors (the 1M bank assigned to main is modelled since SEG-103), PCM sound output and every PCM read except the channel pointers, the font registers, and any application behaviour past the interval of section 18 | - |

## 17. Sub-side boot hardware (SEG-064-T004, ADR 0144)

The devices the authentic BIOS reaches between the sub's runnable epoch and the application handoff. All are shared by both
profiles. Sources: Genesis Plus GX (`core/cd_hw/scd.c`, `gfx.c`, `pcm.c`, `cdd.c`, `cdc.c`), consulted only; expectations in the tests are
derived by hand (`tests/tools/sega_cd_subside_table.c`, the `subside` fixture) and, for the graphics operation, from an independent
geometric oracle.

**Stopwatch (`$FF800C`, word access).** A free-running 12-bit count of 384-sub-cycle units (30.72 us) on a grid anchored at power-on or at
the last peripheral reset; a read returns `(units since the anchor - units at the last write) & $FFF`; any word write of any value clears
the count and keeps the grid phase (G, GPGX `scd_write_word` case `$0C`: `scd.stopwatch += ticks * RATIO`). Byte accesses and word
accesses at `$FF800D` are typed stops `gate_array_register_unsupported`. A read observes the machine time of its instruction (section 12).

**PCM (`$FF0000-$FF7FFF`).** The power-on and the peripheral reset clear the chip and its wave RAM, select channel 0 and bank 0, set every panning to `$FF`, switch every channel off and restart the sample grid at the reset instant (GPGX `pcm_reset`, called at the end of `scd_reset`). The area is decoded on /LDS only: a byte write at an odd address, or the low byte of a word write, reaches
the chip at `(address >> 1) & $1FFF` (an even-address byte write is an unused-area write: dropped, counted). Chip offsets `$0-$8` are the
registers (`ENV`, `PAN`, `FD` low/high, `LS` low/high, `ST` per channel; `CTRL` at 7: bit 6 selects the channel (bits 2-0), otherwise the
wave RAM bank (bits 3-0), bit 7 the chip enable; `ON/OFF` at 8: a clear bit turns the channel on); other offsets below `$1000` are ignored; `$1000-$1FFF` is the selected
4 KiB bank of the 64 KiB wave RAM. No sound is generated. SEG-102 (ADR 0233) models what the processor can read back: each channel's running
wave address (16.11 fixed point, 27 bits) and the sample clock that advances it - one sample event per 384 sub cycles (the stopwatch's unit) at
`reset instant + k x period`, k = 0, 1, ...; an access at tick t sees exactly the events strictly before t. Per event, every channel that is on
(and only while the chip is enabled) reads its wave byte at address bits 26-11: `$FF` replaces the address by `LS << 11` (no step on that event),
anything else adds FD. An ST write reloads a channel that is off; an ON/OFF write reloads every channel whose bit is set (off). A state-changing write and every
pointer read synchronize the chip to the access instant first. **Reads:** chip offsets `$10-$1F` return the running address of channel `(offset >> 1) & 7` - an even
offset bits 18-11, an odd offset bits 26-19 - to a byte read at an odd address or to a word read (the byte in the low lane, zero above it: the reference's
convention, not hardware-verified); a byte read at an even address and every other read (the other registers, the wave RAM read path) is the typed stop
`pcm_unsupported`, taken before any state changes. CDC DMA into PCM RAM stays a typed stop. (G for the registers and the pointer split, GPGX `pcm_write`/`pcm_read`
and MAME `rf5c68`; C for the grid anchor, the equal-instant ordering and the loop step.)

**Graphics ASIC (`$FF8058-$FF8066`, word access, 2M Word RAM only).** `$58` control (bit 0 map repeat; bits 2-1 size: 0 = 16x16-dot stamps in a 16x16-stamp map (256x256 dots), 1 = 32x32-dot stamps, 8x8-stamp map (256x256 dots), 2 = 16x16-dot stamps, 256x256-stamp map (4096x4096 dots), 3 = 32x32-dot stamps, 128x128-stamp map (4096x4096 dots); bit 15 reads busy); `$5A` stamp map base (words, `<< 2`
masked by the map size); `$5C` image buffer height in cells - 1 (bits 4-0); `$5E` image buffer start (`<< 3` dots, bits 18-6) and `$60`
horizontal dot offset (bits 5-0); `$62` image width in dots (bits 8-0); `$64` height in lines (low byte, the live counter: counting down while the operation runs, 0 when it has finished; the single copy `ScdGfx::lines_left`; **`$FF8065` byte read** returns it, SEG-105, see below); `$66` trace
base (`<< 2`): the write starts the operation. Per line the ASIC reads four trace words (start x, start y in 13.3, steps in 5.11), then
for each dot samples the stamp map at the position (a map entry holds the stamp number in bits 10-0 and HFLIP/ROLL1/ROLL0 in bits 15-13,
applied in that order; stamp 0 is not used and draws 0; outside the map the dot is 0 unless the map repeats), reads the stamp's 4-bit
pixel (a stamp's four/sixteen 8x8 cells are stored column-major, pixels row-major, two per byte, the left in the high nibble), combines
it with the buffer dot by the priority mode (`PM1:PM0` of `$FF8003`: 0 replace, 1 underwrite, 2 overwrite, 3 keep) and writes it; the
image buffer is cells of 64 dots in columns of `(height + 1)` cells. The operation takes `3 * (4 + 2W + ((W + off + 3) >> 2))` sub cycles
per line (W the width, off the dot offset; 3 cycles per Word RAM access: GPGX counts sub time in units of 4 per cycle), runs only while
the sub owns 2M Word RAM (it halts, with its clock following the machine, while main owns it, and resumes from the hand-over), is brought
up to date at every observation point (a Word RAM access, an ownership change, a register access) and ends `max(lines - 1, 0) * line + 1`
ticks after its base with the busy bit clear and, with `IEN1` set, sub level 1 pending (acknowledged at acceptance). Starting in 1M mode,
byte accesses, reads of the parameter registers and the font registers are typed stops. The peripheral reset abandons a running operation.

**Vertical-dot counter readback (SEG-105, ADR 0249).** A byte read of `$FF8065` returns the low byte of `$64` as the one live counter: the value last written (a word write of `$64`, at any time: idle or running; an operation started without reprogramming runs the counter's current value, which is 0 after a completed operation); while an operation runs it is the programmed lines minus `ceil((t - base) / line_ticks)` lines started at the read's own machine tick (a line counts as soon as it starts; the same rule that renders the line), floored at 0; it is 0 at completion and stays 0; while Word RAM is main's (halted) it does not move; the peripheral reset clears it. The read first brings the operation up to its tick like every graphics register access (it never advances beyond machine time, schedules nothing and never raises the level-1 request: the completion event stays the scheduler's). Not admitted (typed stops, state unchanged): a word read of `$FF8064` and a byte read of `$FF8064` (the high byte), a write of `$FF8065`, every other parameter read. GPGX (the only source that models the register as a counter) updates its graphics clock once per scan line, so its read value can lag by up to one scan line; this model is exact at the read's tick (frame-equivalent to the reference: ADR 0249).

**Handoff boundary.** In the BIOS-image profile the run ends at the application entry named at build time (section 16) with
`application_handoff`.

## 18. The application after the handoff (SEG-064-T005, ADR 0145)

The BIOS-image profile built without `--application-entry` continues past the application entry; nothing else distinguishes it (the entry
is part of the program identity, so this is another program whose admission chain is rebuilt). The application's executable code is
admitted exactly as before: disc extents (section 16), operand templates for the mutable work-RAM jump-table thunks, the verified
sub epoch. The disc IP's own copy of a routine into work RAM is a second extent of the disc's IP bytes mapped at the RAM-resident base
(the live bytes select the variant; an extent never found in RAM never runs). Every executed instruction start is a compiled entry
whose whole instruction was compared with its admitted variant; an uncompiled target (`entry_not_compiled`) and a live byte of no
variant (`code_bytes_mismatch`) stop before the instruction retires, as in every earlier phase.

**Accepted interval (pre-registered before any post-handoff run).** 60 NTSC video frames of machine time from the application entry
(one frame = 262 lines x 3,420 master clocks x 500,000 ticks = 448,020,000,000 ticks), no controller input, the sub and main BIOS
started from reset. The interval is a property of the claim, not of the program: it is not changed after observing a result.

**Reference alignment.** The independent reference (Genesis Plus GX; not shipped, local) counts whole frames and takes the application
entry inside a frame; the comparison is therefore made (i) at the same elapsed machine time after the entry (the reference's frame ends
after the entry: `j + (1 - entry phase)` frames), and (ii) at an equivalent semantic event (the n-th CDC host-data word consumed by the
sub). Polling-loop iteration counts and the sample instants of interrupts are not compared (the reference idle-skips a processor that
polls a Gate Array register); the executed instruction-start sets, the register files at the checkpoints, the RAM images, the Gate Array
command/status/flag/mode/timer registers, the Word RAM, the PCM chip and the Z80 bus activity are.

## 19. Whole-game static AOT, milestone 1 (SEG-097, ADR 0206-0209)

Three additions to the synthetic profile and one experiment, none of which changes an existing default.

- **Static module discovery (ADR 0206).** `segarecomp sega-cd discover-modules --cue <disc>` walks the sub boot image with a bounded, path-sensitive constant-propagation
  over the CPU-owned decode/lift/effect/control APIs and recognises the machine's CD-load operation of sections 7 and 8: a CDD play command (valid checksum, constant MSF)
  followed by a CDC DMA of one 2,048-byte sector to Word RAM, and a control transfer that lands in the loaded range (the entry proof). Both arms of a conditional are
  explored, so a module the player has not chosen yet is found without being run. The output is the module list of section 15 - the unchanged admission re-checks digest, extent,
  executable RAM region and image-set validity - with one new optional directive, `range <offset> <length>`: the executable subrange of the extent (the digest covers the whole
  extent; a file header or trailing data is never executable authority). Resource bounds (64 operations, 4,096 walked states, 100,000 steps, call depth 8) end in the typed
  `scan_limit_exceeded` and an incomplete result. Loaders that use BIOS file services, tables, computed LBAs, PRG RAM/PCM destinations, partial or multi-sector transfers, nested
  loaders or interrupts are refused or invisible (typed), not guessed. The Word RAM mode is assumed to be 2M (its bit is main-owned).
- **Source-relative bodies (ADR 0207).** `emit --position-independent` emits one generated function per `(image, source offset)` of an image with several mappings; the function runs
  under the actual guest PC (`scd_pc0`), compares the live bytes at that PC as every body does, and only PC-derived literals differ from the per-mapping text. A form that does not
  derive cleanly keeps the per-mapping emission and is reported. The default emission is the verified reference and is unchanged.
- **Experimental frozen M3 + B35 (ADR 0208).** `--aot-policy optimized [--aot-model gen2]` on `emit` and `build`; Compatibility is the default and the fallback. A producer proposes
  source-relative regions per image, only for independently admitted disc game modules and extents (contract `selective_admission.v2`: firmware, BIOS-derived images, boot IP/SP, boot-area and user-declared extents stay broad); the emitters compile only the broad entries inside a validated proposal and report requested/effective/fallback/reason. A proposal never adds an
  entry, an image or a mapping; an omitted entry is the typed `entry_not_compiled` stop. M3 is not promoted and is not a completeness certificate.



## 20. Whole-game static AOT, milestone 3 (SEG-099, ADR 0215-0220)

Additions to the static loader scan; every option is off by default for `discover-modules` and enabled by the supported `sega-cd build` (`--static-recursion 0` restores the M2 scan). Nothing changes at run time: the program still only selects among precompiled, byte-verified variants.

- **Documented BIOS CD-service ABI (ADR 0215).** `JSR $5F22` with D0.W: ROMREADN (`{LBA, count}` through A0), ROMREAD, CDCSTOP, CDCSTAT, CDCREAD, CDCTRN (sub-read device, A0 = buffer), CDCACK, with the public register / memory effects. There is no BIOS file-name service.
- **Work-variable state (ADR 0216, `--work-state`).** Bounded abstract RAM (256-byte pages), condition codes (CPU-owned arithmetic and condition tables), the services above, ISO9660 resolved only by evaluating title code over validated disc bytes, a comparison of disc data with an unknown key is `lookup_key_unknown`, interrupt/user entries make what they write unknown to init/main. Unknown aliases, unmodelled services and foreign calls forget everything.
- **State convergence (ADR 0217, `--state-convergence`).** A fallback after an exhausted exact walk: joins beyond 32 states per location, all-or-nothing, return stacks summarised as sets (<= 64).
- **Recursive discovery (ADR 0218, `--recursive-modules`).** A transfer accepted by the entry proof into a proven loaded module is followed; bytes at the target that are not known to be the disc's are `loaded_bytes_unknown`.
- **Provenance and diagnostics (ADR 0219).** `emit --admit static-extent:` is refused unless a proven module mapping contains it (`static_extent_unproven`); `final_stop` reports a computed destination separately (`destination`, `destination_location`); inventory labels `main_processor_loaders_not_analyzed`, `static_scan_joined`.

## 21. Whole-game static AOT, milestone 4: file-first dynamic images (SEG-100, ADR 0221-0226)

An additive, opt-in producer of the BIOS-image profile. Nothing changes without `--file-first`; the Compatibility build, its identity and every existing default are unchanged.

- **Dynamic image.** An immutable file of the disc's ISO9660 volume (enumerated by the one reader of `libs/media`; no name, extension or path is consulted) of at most 512 KiB (the largest executable region of a processor), whose bytes are compiled **before launch** into native instruction bodies for *source offsets*. The image has no guest base in any generated text. Its identity is its track-1 extent and the SHA-256 of its bytes, verified against the disc when the program is built; the program's identity includes the image set and the candidate masks.
- **Bodies (ADR 0222).** One processor-neutral body per distinct instruction byte string (CPU-owned decode, lift, lowering and timing, taken at three sentinel PCs and reduced to `scd_pc0 + constant` literals; absolute references are not rebased). A body reads the actual guest PC, compares that instruction's live bytes (`scd_code_guard`) and retires through the unchanged protocol. A form that does not derive is a typed refusal, never a position-independent guess. A per-image table maps even source offsets to bodies.
- **Candidate policy (ADR 0224).** `all`: every even offset of every candidate file. `structural`: only offsets whose forward flow is closed and decodable under the CPU owner's decode and control rules (a property of the bytes; no threshold). A file that is larger than the executable regions, unreadable, a duplicate, without structure or beyond an explicit offset budget is a typed outcome in the report and makes the inventory `file_first_inventory_incomplete`; the policy is never a completeness claim and `status.json` names `file_first_structural_filter` and `file_first_runtime_binding`.
- **Binding (ADR 0223).** At a PC no static case and no overlay group owns, the runtime identifies the delivered candidate image the live bytes are an exact copy of (32-byte anchor at the PC, or the 32/64 bytes before it; extension while bytes are equal and the blocks were delivered; minimum run 64; the longest run wins; a tie is `ambiguous_image`) and serves bodies by `(image, source offset)` over the image's whole extent at that base. Identity needs a contiguous exact copy; executability is per instruction: a 64-byte granule is trusted only while it is known equal to the image, and an instruction in any other granule is compared byte for byte with the image (exactly the length of the body that would run) before its body is chosen. Bindings are per processor (at most 16, least recently used evicted) and are skipped, never trusted, for an instruction that differs. The runtime never decodes, lowers, compiles or guesses. Typed outcomes reuse the existing stops (`entry_not_compiled`, `code_bytes_mismatch`, `ambiguous_image`) and are counted by reason in the report's `dynamic` section.
- **Authority.** Bodies execute only when (a) a binding identifies the bytes as a copy of verified, delivered disc bytes and (b) the instruction's own bytes still equal the live bytes. The static producers (execution-derived and static extents) keep precedence at their PCs and remain the fallback. Executable-image authority never comes from a model prediction, a filename or a RAM snapshot.

## 22. CD-DA output (SEG-104, ADR 0244-0248)

The CD drive already decides which sector is under the head and whether audio is playing (section 8). SEG-104 turns exactly those sectors into samples
without changing the drive. Applies to the BIOS-image profile; the synthetic profile keeps its typed stop.

| Item | Behaviour | Basis |
|---|---|---|
| source | a track the build-time disc table declares audio with 2,352-byte sectors (a BIN audio file); one sector = 588 stereo frames; frames are `L, R` and each 16-bit sample is stored least-significant byte first (decoded explicitly: the result is host-endianness independent). Data tracks are never read as audio; compressed or WAV-backed tracks are not part of the disc model (unsupported at CUE ingestion) | Red Book (44,100 frames per second, 75 sectors per second), the disc model |
| when a sector is audible | on a 75 Hz drive tick with status PLAY, an audio track, the LBA at or after the track's INDEX 01 (exactly when the drive reports "audio playing", `$36` high byte 0), and no play/seek command pending (the drive already reports SEEK). The sector at the tick's LBA is produced, then LBA + 1. Seek latency, pause, resume (continues at the drive position), stop, peripheral reset, the INDEX 00 / PREGAP region (silent), data tracks, track crossings and end of disc follow from the drive state | drive state V1 (GPGX `cdd_update`); sector-per-tick correspondence verified against the independent reference (ADR 0247) |
| the clock | guest time only: one tick, one sector; no host clock, queue depth or SDL callback enters the machine. Production is event-driven, bounded to one sector, partition independent | design |
| silence | nothing is produced; there is no stale buffer. The host sees only produced sectors; its device plays silence meanwhile | design |
| fader | power-on and peripheral reset: unity (1,024). A write sets the target (bits 15-4, clamped to 1,024); the volume moves one step per produced sample frame; multiplier `v & $7FC` (or `v & 3` below 4), product / 1,024 truncated toward zero | LC7883KM-style stepping as implemented by the reference emulator (G); the ramp rate is not hardware-verified |
| consumers | an optional sink per sector (`ScdCdda`); the headless canonical artifact (`--cdda-pcm`, `--cdda-digest 1`, `--cdda-log`), and the SDL3 viewer through the Genesis audio sink. Output disabled or a refusing sink changes no guest-visible state, no device log entry and no report field except `cdda` | ADR 0246 |
| refusals | a disc read failing at run time while output is enabled is the typed stop `cd_read_failed`; a missing or modified disc file never starts (disc open). A missing or failing audio device only silences the viewer | ADR 0244 |

Not modelled: the hardware fader's real ramp, CD-DA emphasis, subcode, RF5C164/FM/PSG mixing, and audio of compressed or WAV tracks.
