# Sega CD machine contract (SEG-062-T001)

Audience: every SEG-062 implementer (T002-T005) and the planners of SEG-063/064/065. This document fixes the observable
behaviour of the one supported Sega CD machine profile (the **synthetic post-BIOS fixture profile**), with a
classification and at least one citation for every hardware claim. Later tasks implement it; they do not re-decide it.
Anything still open is listed in section 13 with the experiment that resolves it and its owner. It is never guessed in
code: until resolved, behaviour that depends on it stops with a typed error.

Decisions: ADR 0126 (executable images per processor instance), ADR 0127 (overlay dispatch contract, design only),
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
| **U** | unresolved: sources disagree or are silent; the machine stops with a typed error when it is reached |

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
| audio | none: PCM, CD-DA and the fader are typed stops | C |
| graphics ASIC | none: stamp/rotation registers are typed stops | C |
| backup RAM | none: typed stop | C |

## 2. Processor instances and clocks

The machine owns two **processor instances** of the same instruction-set variant, `CpuVariant::mc68000`
(ADR 0126). Their identity (`main`, `sub`) is a platform enumerator in `platforms/sega-cd`; it never becomes a CPU
variant, an executable-image field or an emitter mode.

| item | value | tag |
| --- | --- | --- |
| main CPU clock | MCLK / 7 = 7,670,453.57 Hz | V2 (GPGX, repository Genesis runtime) |
| sub CPU clock | 12,500,000 Hz (50 MHz Sega CD master clock / 4) | V2 (GPGX `SCD_CLOCK` and `s68k` cycle scaling; MAME `SEGACD_CLOCK 12500000`) |
| CDD interrupt rate | 75 Hz (one CD sector time) | V2 (GPGX `cdd.cycles` modulus; CDROM) |
| timer unit | 384 sub-CPU cycles = 30.72 us per count | V2 (GPGX `TIMERS_SCYCLES_RATIO 384*4` SCD cycles; MAME `30720` ns) |
| CDC DMA rate | 4 sub-CPU cycles per byte, transferred in 8-byte groups | V1 (GPGX `DMA_CYCLES_PER_BYTE 16` SCD cycles citing MiSTer logic-analyser logs) |

Integer clock domain (ADR 0128): one **tick** is 1 / 26,846,587,500,000 s, the least common multiple of the three
rates (53,693,175, 12,500,000 and 75 Hz numerators). Exactly: main cycle = 3,500,000 ticks; sub cycle = 2,147,727 ticks;
CDD sector = 357,954,500,000 ticks; timer unit = 384 sub cycles. All machine time is `uint64_t` ticks (190 hours of
range). No floating point is used anywhere in timing.

## 3. Main CPU bus view (24-bit)

| range | content | behaviour in this profile | tag |
| --- | --- | --- | --- |
| `$000000-$01FFFF` | Boot ROM (BIOS) | absent: every access is the typed stop `bios_absent` | V2 location (GPGX `scd_init`, MAME), C absence |
| `$020000-$03FFFF` | 128 KiB window onto PRG RAM bank `BK1:BK0` (`$A12003` bits 7-6) | read/write **only while `SBRQ` reads 1** (sub bus requested or sub in reset); otherwise typed stop `prg_ram_window_without_bus` | window and bank: V2 (GPGX `scd.prg_ram + ((data & 0xc0) << 11)`, MAME `scd_4m_prgbank_ram_r`); bus gate: **U** (GPGX unmaps without BUSREQ, MAME does not gate) |
| `$040000-$1FFFFF` | mirrors of the above | typed stop `unmapped` | U (mirroring not cross-checked) |
| `$200000-$23FFFF` | Word RAM, 2M mode, when owned by main (`RET` = 1) | linear 256 KiB | V2 (GPGX, MAME) |
| `$200000-$21FFFF` | Word RAM, 1M mode: the bank assigned to main | linear 128 KiB of one bank (section 6) | V2 |
| `$220000-$23FFFF` | 1M-mode cell-image view | typed stop `wordram_cell_image_unsupported` | C (out of scope) |
| `$A12000-$A1203F` | Gate Array, main side (section 5) | per register | V2 |
| `$C00000-$C00007` | VDP data/control ports | composed Genesis VDP device (ADR 0128); VDP DMA enable is a typed stop | V2 (Genesis map) |
| `$FF0000-$FFFFFF` | 64 KiB work RAM | linear | V2 |
| anything else | | typed stop `unmapped` | C |

## 4. Sub CPU bus view (24-bit)

| range | content | behaviour | tag |
| --- | --- | --- | --- |
| `$000000-$07FFFF` | 512 KiB PRG RAM | read/write; writes below `WP * $200` are **ignored** (write protect) | V2 (GPGX `prg_ram_write_byte`: `address >= WP << 9`; MAME `m_write_boundary = data << 8` words) |
| `$080000-$0BFFFF` | Word RAM 2M when owned by sub (`RET` = 0); 1M dot-image view in 1M mode | 2M: linear; dot image: typed stop `wordram_dot_image_unsupported` | V2 |
| `$0C0000-$0DFFFF` | 1M mode: bank assigned to sub; 2M mode: nothing | 1M: linear 128 KiB; 2M: typed stop `unmapped` | V2 |
| `$080000-$0BFFFF` while main owns 2M Word RAM | | real hardware withholds DTACK (the sub CPU hangs); modelled as typed stop `wordram_not_owned` | V2 (GPGX `s68k_lockup_*`; MAME silent) - the stop represents a hang |
| `$FE0000-$FE3FFF` | backup RAM (odd bytes) | typed stop `backup_ram_unsupported` | C |
| `$FF0000-$FF7FFF` | PCM | typed stop `pcm_unsupported` | C |
| `$FF8000-$FF81FF` | Gate Array, sub side (section 5) | per register | V2 |
| higher mirrors (`$100000+`) | | typed stop `unmapped` | U (GPGX mirrors every 1 MiB, MAME does not) |

The sub CPU's exception vectors live in PRG RAM `$000000-$0003FF` (the sub CPU's address 0), so the vector table is
ordinary writable memory: the sub resets from `PRG[0..7]` and takes autovector `24 + level` through `PRG[$60 + 4 *
level]`. V2 (GPGX `s68k` reset/IRQ paths through `s68k.memory_map[0]`; MAME `segacd_map` 0x000000 = PRG RAM).

## 5. Gate Array registers implemented by this profile

Byte addresses; main side `$A120xx`, sub side `$FF80xx`. A register or access not listed here is the typed stop
`gate_array_register_unsupported` (it carries the side, address, width and direction).

| reg | main side | sub side | behaviour | tag |
| --- | --- | --- | --- | --- |
| `$00` (hi) | `IEN2` (bit 7, read), `IFL2` (bit 0: write 1 raises the sub level-2 request when `IEN2` = 1; otherwise dropped) | LED byte (write stored, read back) | | V2 (GPGX `mem68k.c` case 0x00, MAME `scd_a12000_halt_reset_w` `& 0x0100 && irq_mask & 0x04`) |
| `$01` (lo) | `SBRQ` (bit 1), `SRES` (bit 0, 0 = reset asserted); `SRES` 0 -> 1 makes the sub take its reset exception from `PRG[0..7]`; setting `SBRQ` while the sub is in STOP is the typed stop `busreq_while_sub_stopped_unsupported` (U, section 13) | `RESET` status: reads 1; writing 0 resets the CD peripherals: typed stop | main: V2 (GPGX, MAME); power-on value `$02` (reset asserted, bus requested): V1 (GPGX `scd_reset` regs[0] = `0x0002`) |
| `$02` (hi) | write-protect `WP` (main writes) | `WP` read-only (sub writes ignored) | | V2 (GPGX, MAME `segacd_sub_memory_mode_w_8_15` "no effect") |
| `$03` (lo) | `BK1:BK0` (7-6), `MODE` (2, read), `DMNA` (1), `RET` (0) | `PM1:PM0` (4-3), `MODE` (2), `DMNA` (1, read), `RET` (0) | ownership state machine, section 6; `PM` != 0 is a typed stop | V2 for the 2M handoff and 1M swap (GPGX, MAME); details section 6 |
| `$04` | `EDT`/`DSR` (7-6), `DD` (2-0) read | same, `DD` writable (CDC destination) | writing resets the DMA address register `$0A` | V2 destinations (GPGX `cdc_dma_init`, MAME `DMA_PRG 0x0500`, `DMA_WRAM 0x0700`); `$0A` reset on write: V1 (GPGX citing MCDV) |
| `$05` | - | CDC register address (4 bits) | | V2 |
| `$07` | - | CDC register data (section 7) | | V2 (GPGX `cdc_reg_w/r`; register map of the LC8951) |
| `$0A` | - | CDC DMA destination address (bytes / 8 for Word RAM and PRG RAM) | | V2 (GPGX `<< 3`, MAME `<< 3`) |
| `$0E` | main flag byte (write) | main flag byte (read) | a word write by main stores its **low** byte | V2 (GPGX "D8-D15 ignored", MAME "low bits take priority") |
| `$0F` | sub flag byte (read) | sub flag byte (write) | | V2 |
| `$10-$1F` | command words COMCMD0-7 (read/write) | read-only; a sub write is a typed stop | main side V2; sub-write behaviour **U** (GPGX ignores, MAME stores) |
| `$20-$2F` | status words COMSTAT0-7 (read-only; a main write is a typed stop) | read/write | | V2 for the direction; main-write behaviour U |
| `$30-$31` | - | timer (8 bits; 0 stops) | INT3 every `(N + 1) * 384` sub cycles while `IEN3` | rate V2; the `+ 1` is V1 (GPGX citing MCDV test "REG 8030 #7"; MAME uses `N * 384`) - see section 13 |
| `$32-$33` | - | interrupt enables `IEN1`-`IEN6` (bits 1-6) | `IEN2` mirrors to main `$00` bit 7; clearing an enable drops nothing else except a pending level 1 | V2 (GPGX case 0x33, MAME `segacd_irq_mask_w`) |
| `$36-$37` | - | CDD control: `HOCK` (bit 2 of `$37`) | | V1 (GPGX case 0x36) |
| `$38-$41` | - | CDD status nibbles RS0-RS9 (read) | section 8 | V1 (GPGX `cdd_process`), layout V2 with MAME `segacd_cdd_rx_r` map |
| `$42-$4B` | - | CDD command nibbles TX0-TX9 (write); a word write to `$4A` sends the command | section 8 | V1 (GPGX case 0x4a), map V2 (MAME `segacd_cdd_tx_w`) |

## 6. Word RAM ownership

256 KiB. Two modes selected by `MODE` (written by the sub CPU, `$FF8003` bit 2).

**2M mode** (single owner; power-on: main owns, `RET` = 1, `DMNA` = 0; V1 GPGX `scd_reset` regs[2] = `0x0001`):

| event | effect | tag |
| --- | --- | --- |
| main writes `DMNA` = 1 | ownership passes to the sub immediately; `RET` reads 0, `DMNA` reads 1 | V2 (GPGX `mem68k.c` "Word-RAM is assigned to SUB-CPU"; MAME `scd_rammode = 1`) |
| main writes `DMNA` = 0 | no effect | V1 (GPGX "writing 0 to DMNA in 2M mode does nothing") |
| sub writes `RET` = 1 | ownership returns to main; `RET` reads 1, `DMNA` reads 0 | V2 |
| sub writes `RET` = 0 while sub owns | no effect | V2 |
| non-owner access | main: typed stop `wordram_not_owned` (hardware: unmapped/open bus, value U); sub: typed stop `wordram_not_owned` (hardware: CPU hang) | V2 that the access does not complete; the stop is C |
| repeated `DMNA` = 1 while the sub already owns | no further effect | V1 |

**1M mode** (two 128 KiB banks, one per CPU, simultaneously accessible):

| fact | tag |
| --- | --- |
| bank `b` word `i` is 2M-space word `2 i + b` (word interleave); switching modes preserves the bytes through this mapping | V2 (GPGX `word_ram_switch`, MAME `segacd_1meg_mode_word_read` `offset*2 (+1)`) |
| the sub CPU sees its bank at `$0C0000-$0DFFFF`, the main CPU sees the other bank at `$200000-$21FFFF` | V2 |
| a sub write of `RET` selects which bank each CPU gets; writing the opposite value swaps the banks | V2 (behaviour), **U** (which physical bank is called 0 for a given `RET`; GPGX: `RET` = 1 gives main bank 1). The fixtures observe only "the bank the sub wrote is the bank the main reads after the swap", which is invariant to that naming. |
| main writes of `DMNA` in 1M mode (swap request semantics) | U - typed stop `wordram_1m_dmna_unsupported` |
| `PM` priority modes | C: typed stop |

## 7. CDC (LC8951) subset

Registers through `$FF8005` (address, auto-incrementing except at 0) and `$FF8007` (data). Implemented:

| register (write / read) | behaviour | tag |
| --- | --- | --- |
| 1 `IFCTRL` / `IFSTAT` | `DOUTEN` (bit 1) enables data output, clearing it aborts a transfer (already transferred bytes stay); `DTEIEN` (6) and `DECIEN` (5) enable the CDC interrupt; `IFSTAT` bits are active-low (`DTEI` 6, `DECI` 5, `DTBSY` 3, `DTEN` 1) | V1 (GPGX `cdc_reg_w` case 1; LC8951 register names as used by GPGX and MAME's `lc89510_temp` map) |
| 2-3 `DBCL/DBCH` | byte count minus one (12 bits) | V1 |
| 4-5 `DACL/DACH` | buffer source address | V1 |
| 6 `DTRG` | starts the transfer to the destination in `$FF8004` when `DOUTEN` | V1 |
| 7 `DTACK` | clears a pending transfer-end interrupt | V1 |
| 8-9 `WAL/WAH`, 12-13 `PTL/PTH` | write/block pointers; decoded blocks advance both by 2352 | V1 |
| 10 `CTRL0` | `DECEN` (7) decoder enable, `WRRQ` (2) buffer write | V1 |
| 11 `CTRL1` | `SHDREN` and mode bits; only 0 is supported (Mode 1 data) | C |
| read 4-7 `HEAD0-3` | the BCD minute, second, frame and mode of the last decoded block | V1, CDROM |
| read 8-9 `PTL/PTH`, 10-11 `WAL/WAH` | pointers | V1 |
| read 12 `STAT0` | `CRCOK` = decoder enable | V1 |
| read 15 `STAT3` | `VALST` (bit 7) = 0 after a block was decoded; reading sets it back to 1 | V1 |
| 15 (write) `RESET` | resets the CDC registers | V1 |

Buffer: 16 KiB, addressed modulo `$4000`; a decoded Mode-1 block stores its 4-byte header at `PT` and the 2048 user bytes
after it. **Transfer chain implemented: destination 7 (Word RAM)** in 2M mode while the sub owns Word RAM (or 1M
mode into the sub's bank): 16-bit big-endian words from `DAC` to Word RAM offset `($FF800A) * 8`, `DAC` and the
destination advance as data moves; `DBC + 1` bytes in all; at the end `EDT` (`$FF8004` bit 7) is set, `IFSTAT.DTEI`
goes low and, with `DTEIEN` and `IEN5`, the sub takes level 5. V2 for addressing (GPGX `word_ram_2M_dma_w`, MAME
`SegaCD_CDC_Do_DMA`); progress rate V1. Partial progress is real (DMA is not atomic): an abort leaves the bytes moved so
far in Word RAM and no `EDT` (V1, GPGX `cdc_reg_w` `DOUTEN` clear). DMA into 2M Word RAM owned by main: **U** (GPGX
halts until the sub owns it, MAME writes anyway): typed stop `cdc_dma_destination_not_owned`.
Destinations 2/3 (host read through `$A12008/$FF8008`), 4 (PCM) and 5 (PRG RAM) are typed stops
`cdc_destination_unsupported` (eligible for later refinement; PRG RAM addressing is V2 and recorded for SEG-063).

## 8. CDD subset

| item | behaviour | tag |
| --- | --- | --- |
| command transfer | the sub writes TX0..TX9 (low nibbles of `$42..$4B`); the word write to `$4A` sends it | V1 (GPGX), map V2 (MAME) |
| command checksum | TX9 = `~(TX0 + ... + TX8) & $F`; a mismatch is the typed stop `cdd_bad_checksum` (hardware response not modelled) | checksum rule V1 (GPGX status checksum, same formula for RS9); stop C |
| status checksum | RS9 = `~(RS0 + ... + RS8) & $F` | V1 |
| command 0 (status) | RS0 = drive status | V1 |
| command 1 (stop) | status `STOP`, decoding of further sectors ends | V1 |
| command 3 (read/play) | TX2..TX7 = BCD MM:SS:FF; LBA = MSF - 150; status `SEEK` until the read starts; the first sector is delivered on the third 75 Hz tick after the command (two ticks of latency, GPGX default `cdd.latency = 2`), then one sector per tick | rule V1, latency C (deterministic model; real seek time not modelled) |
| target LBA | must lie inside a data track of the build-time track table; otherwise typed stop `cd_read_out_of_range` or `cd_read_audio_track` | C (fail closed) |
| sector delivery | each tick decodes the current LBA if `DECEN`: header `HEAD0-3` = BCD MSF of `LBA + 150` and mode 1; with `WRRQ` the block is written to the buffer | V1 + CDROM |
| reading past the end of the data track | typed stop `cd_read_out_of_range` | C |
| `HOCK` = 1 and `IEN4` | level-4 interrupt every tick | V1 |
| other commands (TOC, seek, pause, scan, tray) | typed stop `cdd_command_unsupported` | C |

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
track <number> <mode1_2048|mode1_2352|audio> file <index> offset <byte offset of INDEX 01> sector <2048|2352> pregap <sectors> lba <first LBA> count <sectors>
total <sectors including pregaps>
```

LBAs are assigned in CUE order: each track starts after the previous track plus its own `PREGAP`; a track ends at the next
`INDEX 01` of the same file or at the end of the file. Changing any byte of any file, any name, or the layout changes the
identity. The generated program stores the per-file size and SHA-256 and the track table and refuses a disc whose files do
not match at start-up.

## 10. Interrupts

| level (sub) | source | implemented | tag |
| --- | --- | --- | --- |
| 1 | graphics completion | typed stop if enabled and raised (never raised: ASIC absent) | C |
| 2 | main `IFL2` | yes; acknowledging clears `IFL2` | V2 (GPGX `scd_68k_irq_ack`, MAME HOLD_LINE) |
| 3 | timer | yes | V2 (rate), V1 (`+ 1`) |
| 4 | CDD (`HOCK`) | yes | V1 |
| 5 | CDC (`DTEI`/`DECI` falling edge of the CDC /INT with `IEN5`) | yes | V1 |
| 6 | subcode | never raised | C |

All sub interrupts are autovectored (vector `24 + level`); the handler is the long at `PRG[$60 + 4 * level]` and must be
a compiled entry of the sub's image set, otherwise the acceptance stops with `interrupt_handler_not_compiled`.
Acceptance and the exception frame are the CPU-owned rules of ADR 0043 (`exception_core.h`). Main CPU interrupts
(VDP, external) are not wired in this profile: the main vectors live in the absent BIOS ROM, so any main exception
stops with `bios_absent`.

## 11. Execution and verification rules

- Both processors execute only generated-native C compiled from their own `ExecutableImageSet` (ADR 0126).
- A computed control transfer (`RTS`, `RTE`, `JMP (An)`, a reset or interrupt vector) selects among the processor's
  compiled entries; an address without one is the typed stop `entry_not_compiled`.
- Images mapped in writable memory carry `ImageVerification::byte_identity`: before a compiled instruction runs, the
  live bytes at its address are compared with the image bytes (comparison only; nothing is decoded). A mismatch is the
  typed stop `code_bytes_mismatch`.
- Every run takes a finite `--instruction-budget`; exhaustion is the typed result `budget_exhausted`, not success.
- Success is `idle`: both processors are stopped (or the sub is held) and no enabled source can wake either.

## 12. Timing approximations (recorded deviations, not hardware proof)

- A bus access takes effect at the start time of its instruction; there is no wait-state, refresh, DMA or Word-RAM
  contention modelling. Cross-CPU ordering is exact only at instruction granularity.
- The CDD seek latency is the fixed two-tick model of section 8.
- CDC decode/transfer interrupt edges follow GPGX's model; the decoder "active period" that clears `DECI`
  automatically is not modelled (`DECI` clears on `STAT3` read or decoder disable).
- `SBRQ` takes effect at the sub's next instruction boundary (the real acknowledge latency is not modelled).

## 13. Unresolved items

| item | why | experiment that resolves it | owner |
| --- | --- | --- | --- |
| main PRG-RAM window access while `SBRQ` = 0 | GPGX unmaps it, MAME allows it | hardware test (mcd-verificator class) or a third reference (BlastEm, MiSTer RTL) | SEG-064 |
| timer period `N` vs `N + 1` counts | GPGX (citing MCDV) and MAME disagree | run MCDV "REG 8030 #7" or read MiSTer RTL | SEG-065 |
| which 1M bank is "0" for a given `RET` | single source | MiSTer RTL / hardware test | SEG-065 |
| sub writes to COMCMD and main writes to COMSTAT | GPGX ignores, MAME stores | hardware test | SEG-064 |
| sub address mirrors above `$0FFFFF` | GPGX mirrors, MAME does not | hardware test | SEG-065 |
| CDC DMA into 2M Word RAM owned by main | GPGX halts, MAME writes | hardware test | SEG-063 |
| system-header longwords `$38/$3C/$40-$4C` | one primary source in reach of this cloud | read the Mega-CD Disc Format specification and the Sega CD BIOS manual directly | SEG-064 |
| SP base `$6000`, IP base `$FF0000` | BIOS-owned facts | BIOS feasibility gate (ADR 0130) | SEG-064-T001 |
| `SBRQ` readback when the bus is requested while the sub is in STOP | GPGX: reads 0 ("verified on real hardware"), MAME: no special case | hardware test | SEG-064 |
| CDD seek latency, decoder active period | timing not modelled | logic-analyser logs (MiSTer `mcd logs`) | SEG-065 |
| raw-sector EDC/ECC | not checked; sync and header are | decision: optional until a title needs it | SEG-063 |

## 14. What later milestones consume

- SEG-063 (modules and overlays) consumes sections 4, 6, 7 and ADR 0127; ISO9660 file lookup belongs there.
- SEG-064 (authentic BIOS) consumes sections 3 (BIOS ROM), 9 and ADR 0130.
- SEG-065 (graphics ASIC, PCM, CD-DA, viewer) consumes sections 5 (registers left as stops), 10 and 13.
