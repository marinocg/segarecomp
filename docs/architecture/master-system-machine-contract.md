# Master System baseline machine contract (SEG-009-T001)

Audience: every SEG-009 implementer (T002-T013). This document fixes the observable behaviour of the one
supported Master System machine profile, with a public citation for every hardware fact. Later tasks implement
it; they do not re-decide it. A fact that is still open is listed in section 14 with a bound, an owner and a
resolution method. It is never guessed in code: until the owner resolves it, the behaviour that depends on it
stops with a typed error or follows the decision written next to the item.

Decisions: ADR 0061 (profile, BIOS, mapper identity, timebase), ADR 0062 (independent references), ADR 0063
(placement and reuse, PSG device placement), ADR 0064 (execution architecture, artifacts, audio pipeline,
fixture builder). Z80 boundary: `docs/architecture/z80-master-system-integration-contract.md` (the SEG-008
contract) wins on anything CPU-owned. Machine-readable scope: `tests/fixtures/sms-capabilities.json`
(`tools/sms_capabilities.py`); capability ids are quoted in square brackets with a `cap:` prefix.

## References

| key | source | locator |
| --- | --- | --- |
| MD-HW | Charles MacDonald, *SMS/GG hardware notes*, 2002-11-12 | <https://www.smspower.org/uploads/Development/smstech-20021112.txt> |
| MD-VDP | Charles MacDonald, *Sega Master System VDP documentation*, 2002-11-12 | <https://www.smspower.org/uploads/Development/msvdp-20021112.txt> |
| SP-MAP | SMS Power!, *Mappers* | <https://www.smspower.org/Development/Mappers> |
| SP-MEM | SMS Power!, *Memory map* | <https://www.smspower.org/Development/MemoryMap> |
| SP-HDR | SMS Power!, *ROM header* | <https://www.smspower.org/Development/ROMHeader> |
| SP-3E | SMS Power!, *Port $3E* | <https://www.smspower.org/Development/Port3E> |
| SP-CLK | SMS Power!, *Clock rate* | <https://www.smspower.org/Development/ClockRate> |
| SP-PSG | SMS Power! (Maxim), *SN76489* | <https://www.smspower.org/Development/SN76489> |
| SP-PAUSE | SMS Power!, *Pause button* | <https://www.smspower.org/Development/PauseButton> |
| SP-REGION | SMS Power!, *Region detection* | <https://www.smspower.org/Development/RegionDetection> |
| SP-BIOS | SMS Power!, *BIOSes* | <https://www.smspower.org/Development/BIOSes> |
| SP-PAL | SMS Power!, *Palette* | <https://www.smspower.org/Development/Palette> |
| SP-VREG | SMS Power!, *VDP registers* | <https://www.smspower.org/Development/VDPRegisters> |
| SP-VCNT | SMS Power!, *Scanline counter* | <https://www.smspower.org/Development/ScanlineCounter> |
| TMS | Texas Instruments, *TMS9918A/TMS9928A/TMS9929A Video Display Processors* data manual (1982), horizontal timing | SMS Power! Development/Documents |
| GPGX-NOTE | Genesis Plus GX `core/sound/psg.c` at the ADR 0062 pin: comments recording tests on 315-5313A/315-5660 integrated PSGs | ADR 0062 |

All pages were read on 2026-09-30. MacDonald's notes forbid re-hosting; they are cited by link only.
"Oracle smoke" and "PSG smoke" name the SEG-009-T001 checks of ADR 0062 run against the pinned references.

## 1. Profile

| item | decision | evidence |
| --- | --- | --- |
| console | Master System II: 315-5246 VDP (with integrated SN76489-family PSG), cartridge slot only, pause button, no reset button, no card/expansion slot | MD-HW §1, MD-VDP §1 |
| TV standard | NTSC: 262 lines per frame | MD-VDP §11 |
| region | export (header region code `$4`; I/O-control readback equals the written level) | SP-HDR, MD-HW §4, SP-REGION |
| boot | post-BIOS documented state; no BIOS execution (section 8) | MD-HW §4/§6, SP-BIOS |
| mapper | declared, never inferred (section 4); baseline families `sega` and `rom_only` | SP-MAP |
| timebase | Z80 T-states (section 2) | SP-CLK, MD-VDP §11, TMS |
| audio | mono PSG, native chip-tick model, deterministic decimation to 44,100 Hz s16le (section 10) | SP-PSG, ADR 0064 |

Why this profile (ADR 0061): the SMS 2 decodes every port in a documented way (reads of `$00-$3F` return `$FF`;
the SMS 1 returns open-bus bytes) and always presents `$FF` on the interrupt-acknowledge data bus (the SMS 1
presents a random byte, which would violate the SEG-008 RST-only IM0 contract for IM0 software). The 315-5246 has
no SMS 1 table-mask quirks. NTSC export matches the authorized local images (section 18) and the NTSC SMS 2 on
which MacDonald measured the display timing. Everything else is profile data or an exclusion:
PAL, Japanese region, SMS 1 VDP, Game Gear, SG-1000 and Mark III fail closed with `SMS_ERROR_PROFILE_UNSUPPORTED`
(or `SMS_ERROR_VDP_MODE_UNSUPPORTED` for a revision-specific mode) [cap:timing.pal] [cap:ingest.region_japan]
[cap:ingest.game_gear] [cap:vdp.revision_5124].

## 2. Clocks and the unified timebase

- Master clock 53.693175 MHz (NTSC); the Z80 runs at master / 15 = 3,579,545 Hz (the crystal is rated 53.6931 MHz;
  the chrominance subcarrier value 315/88 MHz is the one commonly used) [SP-CLK].
- The PSG is clocked by the system clock and divides it by 16 internally [SP-CLK, SP-PSG]: one PSG chip tick = 16 T.
- A scanline is 342 pixel periods [MD-VDP §11]. The VDP pixel clock is the TMS9918A's 5.3693175 MHz [TMS], i.e.
  master / 10. One line therefore lasts 342 x 10 / 15 = **228 T** exactly.
- A frame is 262 lines [MD-VDP §11] = **59,736 T** exactly.

**Timebase decision.** All machine time is the Z80's monotonic `cycles` (u64 T-states) [Z80-ABI §8]. Every
periodic event is an integer multiple of one T-state: line = 228 T, frame = 59,736 T, PSG tick = 16 T. The only
sub-T quantity is the pixel position, which is the exact rational `3/2 x (T within the line)`; it is computed with
integer arithmetic when needed (H counter, section 9.7) and never accumulated, so there is no rounding drift.
Host time is derived only for presentation: CPU frequency = 39,375,000 / 11 Hz (315/88 MHz), frame period =
59,736 x 11 / 39,375,000 s (59.9227 Hz). [cap:timing.timebase_tstates] [cap:timing.ntsc_262_lines] [cap:timing.cpu_clock]
Devices are stepped to the T-state of each CPU access (the `cycles`
argument of the ABI callbacks = start of the current instruction) and at scanline boundaries; nothing is split
inside an instruction [Z80-ABI §3, §8]. Wait states and bus contention are not modelled [cap:timing.wait_states].

**Frame boundary.** Frame *n* is the half-open T interval `[n x 59,736, (n+1) x 59,736)`; line 0 (V counter `$00`)
starts at T = n x 59,736. `run_until_frame(n)` stops at the first instruction boundary with
`cycles >= n x 59,736` [cap:timing.frame_boundary]. The framebuffer of frame *n* is the active area rendered
during that interval.

## 3. Memory map

| range | content | evidence |
| --- | --- | --- |
| `$0000-$03FF` | ROM offset `$0000-$03FF`, fixed regardless of slot 0 | SP-MAP, SP-MEM |
| `$0400-$3FFF` | slot 0: bank `[$FFFD]` offset `$0400-$3FFF` | SP-MAP |
| `$4000-$7FFF` | slot 1: bank `[$FFFE]` | SP-MAP |
| `$8000-$BFFF` | slot 2: bank `[$FFFF]`, or cartridge RAM when `$FFFC` bit 3 = 1 | SP-MAP |
| `$C000-$DFFF` | 8 KiB work RAM | MD-HW §2, SP-MEM |
| `$E000-$FFFF` | work RAM mirror; `$FFFC-$FFFF` are also the mapper registers (write-through) | MD-HW §2, SP-MEM |

[cap:mem.ram_8k] [cap:mem.ram_mirror] [cap:mem.rom_slots]

- Reads of ROM slots serve data from the currently mapped bank (tables and `LDIR` from ROM work); writes to ROM
  addresses have no effect (mask ROM) [cap:mem.rom_write_ignored].
- `code_image(addr)` reports the mapped ROM bank identity and window base for `$0000-$BFFF` when ROM is mapped, and
  0 for work RAM, its mirror and cartridge RAM, so code there stops with `Z80_ERROR_MUTABLE_CODE` [Z80-ABI §5] [cap:mem.code_image_rom_only].
- The 3D-glasses register (`$FFF8-$FFFB`, card-slot adapter) has no device in this profile; writes there are plain
  RAM-mirror writes [MD-HW §2] [cap:mem.3d_glasses].

## 4. Mapper and the mapper identity contract

### 4.1 Identity

Platform recognition and mapper identification are separate decisions.

- The header (`TMR SEGA` at `$7FF0`, `$3FF0` or `$1FF0`; region nibble) may identify the platform, region and
  profile [SP-HDR, MD-HW §6] [cap:ingest.header_platform]. **It never establishes the mapper family.** No byte pattern, size, checksum or
  database lookup infers a mapper, and the Sega mapper is never a silent default [cap:mapper.identity_declared].
- Explicit declaration sources, in precedence order (the first present wins; two conflicting declarations are an
  error):
  1. a build option (`segarecomp build --mapper sega|rom_only`, T010) [cap:mapper.identity_cli];
  2. a profile/cartridge manifest (`<rom>.mapper.json` next to the input or passed with `--manifest`, schema
     `{"mapper": "...", "sha256": "..."}`); the manifest's `sha256` must equal the input's or the build fails;
  3. the fixture builder's declaration (`tools/sms_fixture_rom.py` writes the same manifest shape with
     `declaration_source = "fixture_builder"`);
  4. for an authorized local image: an identity established once by a human and recorded as a manifest with the
     input SHA-256 (kept beside the ignored image, never committed).
- Provenance records the declared family and its declaration source [cap:ingest.identity_record]
  [cap:build.provenance].
- Typed errors (generation time, before emission): `SMS_ERROR_MAPPER_UNDECLARED` (no declaration, or an unknown
  name) and `SMS_ERROR_MAPPER_UNSUPPORTED` (a known non-baseline family: Codemasters, Korean variants, MSX/Nemesis,
  Janggun, 4-PAK, EEPROM 93C46 and other boards) [cap:mapper.identity_undeclared_error] [cap:mapper.codemasters]
  [cap:mapper.korean_msx_janggun] [cap:mapper.eeprom_multicart].
- A trustworthy SHA-256 -> mapper metadata database is possible later work and outside SEG-009.

Baseline families:

- `sega`: the Sega mapper below, any accepted ROM size;
- `rom_only`: a 32 KiB ROM with no mapper hardware. `$0000-$7FFF` is the ROM; `$8000-$BFFF` has no documented
  content, so a data read there stops with `SMS_ERROR_UNMAPPED_READ`. Writes to `$FFFC-$FFFF` are RAM-mirror writes
  only, and `code_image` reports one invariant image for `$0000-$7FFF` [cap:mapper.rom_only].

Accepted ROM sizes are 32, 64, 128, 256 and 512 KiB (power of two; the largest standard mapper revisions address
512 KiB) [SP-MAP]; anything else is `SMS_ERROR_ROM_SIZE_UNSUPPORTED` [cap:ingest.rom_sizes]. The header size nibble and
checksum are recorded (match/mismatch) and never used to accept or reject [SP-HDR] [cap:ingest.checksum_informational].

### 4.2 Sega mapper behaviour

- Registers `$FFFC` (RAM mapping/control), `$FFFD`, `$FFFE`, `$FFFF` (bank for slot 0/1/2) [SP-MAP, MD-HW §2]
  [cap:mapper.registers].
- **Write-through and read-back.** A write to `$FFFC-$FFFF` updates the register *and* work RAM `$1FFC-$1FFF`;
  reads of `$FFFC-$FFFF` and `$DFFC-$DFFF` return the RAM copy. The registers are write-only; writing the RAM copy
  through `$DFFC-$DFFF` does not change the mapping [MD-HW §2, SP-MAP] [cap:mapper.write_through].
- **Bank masking.** The selected bank is `value & (bank_count - 1)` for a power-of-two ROM [SP-MAP]
  [cap:mapper.bank_masking]. (Oracle smoke: bank 10 of an 8-bank ROM reads bank 2 on both finalists.)
- **Power-on values** `$FFFC=0, $FFFD=0, $FFFE=1, $FFFF=2` (315-5235). Older mappers are undefined at reset, but the
  export BIOS never programs the mapper and requires bank 1 in slot 1 to find the header, so the profile uses the
  315-5235 values [SP-MAP, SP-BIOS] [cap:mapper.reset_values].
- **Fixed first 1 KiB.** `$0000-$03FF` is always ROM `$0000-$03FF`; slot 0 exposes bank offset `$0400+` at
  `$0400-$3FFF` [SP-MAP] [cap:mem.fixed_first_1k] [cap:mapper.slot0_remap].
- **Cartridge RAM (in baseline, data only).** `$FFFC` bit 3 maps cartridge RAM into slot 2 and overrides `$FFFF`;
  bit 2 selects the first or second 16 KiB bank. The model is always 32 KiB, zero at power-on, deterministic and
  never persisted ("emulating a full mapper in all cases does not cause any problems") [SP-MAP]
  [cap:mapper.cart_ram_slot2] [cap:mapper.cart_ram_state] [cap:mapper.persistence].
- `$FFFC` bit 7 ("ROM write enable") is stored and has no effect: a retail cartridge is a mask ROM. It is accepted
  because an authorized local image sets it (section 18) [SP-MAP] [cap:mapper.rom_write_enable_bit].
- `$FFFC` bit 4 (cartridge RAM over `$C000-$FFFF`; "no known software") and a non-zero bank shift in bits 1-0 ("no
  known software", only one 512 KiB chip) stop with `SMS_ERROR_CONTROL_BIT_UNSUPPORTED` at the write [SP-MAP]
  [cap:mapper.cart_ram_system_overlay] [cap:mapper.bank_shift].
- **ImageSet (generation time, ADR 0058 reference shape).** Image 1 = invariant `$0000-$03FF`; one banked image
  per 16 KiB bank, admissible in slot 0 (`$0400-$3FFF`, offsets `$0400+`), slot 1 and slot 2, with window-relative
  owners; nothing at `$C000-$FFFF`. `code_image` answers the *current* mapping; a mapper write takes effect for the
  next dispatch [Z80-ABI §4] [cap:mapper.imageset] [cap:mapper.code_image_dispatch].

## 5. Memory control (port `$3E`)

Bits (active low: 1 = disabled): 7 expansion, 6 cartridge, 5 card, 4 work RAM, 3 BIOS, 2 I/O; 1-0 unused. Bits 7
and 5 have no effect on an SMS 2. The BIOS leaves `$AB` when it starts a cartridge [MD-HW §4, SP-3E].

- Power-on value of the model: `$AB` [cap:memctl.post_bios_value].
- A write with bits 6, 4 and 2 clear and bit 3 set (cartridge, RAM and I/O enabled, BIOS disabled) is accepted and
  has no further effect [cap:memctl.write_compatible].
- Any other write (disable the cartridge, work RAM or I/O chip, or enable the BIOS) stops with
  `SMS_ERROR_CONTROL_BIT_UNSUPPORTED` at the `io_out` T-state: slot switching and BIOS mapping are outside the
  baseline [cap:memctl.write_incompatible].

## 6. I/O port decoding

The Z80 ABI supplies a 16-bit port [Z80-ABI §6]. The SMS decodes only A7, A6 and A0; A8-A15 are ignored
[MD-HW §3, MD-VDP §2] [cap:io.decode_a7_a6_a0]. SMS 2 map [MD-HW §3]:

| A7 A6 | A0 = 0 (even) | A0 = 1 (odd) | reads |
| --- | --- | --- | --- |
| 0 0 (`$00-$3F`) | write: memory control | write: I/O control | `$FF` |
| 0 1 (`$40-$7F`) | write: PSG | write: PSG | even: V counter, odd: H counter |
| 1 0 (`$80-$BF`) | VDP data | VDP control | even: data port, odd: status |
| 1 1 (`$C0-$FF`) | write: no effect | write: no effect | even: port `$DC`, odd: port `$DD` |

[cap:io.write_00_3f] [cap:io.read_00_3f] [cap:io.40_7f] [cap:io.80_bf] [cap:io.c0_ff]

- The FM unit ports `$F0-$F2` of the Japanese SMS do not exist here; they decode as the `$C0-$FF` row
  [MD-HW §3, §7] [cap:io.fm_unit].
- While a port class's device is not yet implemented (staged delivery T003-T007), an access to it stops with
  `SMS_ERROR_PORT_UNIMPLEMENTED`, never a silent value [cap:io.unimplemented_class].
- Oracle smoke: reads of `$00` and `$3E` return `$FF`; `$C0` mirrors `$DC`; `$BD`/`$80` mirror the control/data
  ports on both finalists.

## 7. Interrupts

- **/INT is a level.** The VDP asserts it while (frame pending and R1 bit 5) or (line pending and R0 bit 4); clearing
  an enable bit deasserts it and setting it with the flag pending asserts it [MD-VDP §12] [cap:irq.int_level]
  [cap:irq.enable_gating]. The platform drives `Z80State::int_line` from this expression after every VDP state change
  and at every scheduled event [Z80-ABI §7].
- **Acknowledge.** Reading the control port (status) clears the frame flag and the line pending flag (and the
  overflow/collision flags), which deasserts /INT [MD-VDP §4, §12] [cap:irq.ack_status_read]. Interrupt acceptance
  itself does not deassert it.
- **Data bus.** `interrupt_acknowledge` returns `$FF` on the SMS 2 [MD-HW §5] [cap:irq.data_bus_ff]: IM1 goes to
  `$0038` (normal software) [cap:irq.im1]; IM0 executes `RST 38h`, which the SEG-008 RST-only IM0 contract accepts
  (ADR 0056); IM2 reads the vector at `(I << 8) | $FF` (odd addresses work) [MD-HW §5]. Oracle smoke: IM2 with
  `I = $C2` reaches the handler through `$C2FF/$C300` on both finalists.
- **Pause -> NMI.** The pause button is wired to NMI: a press produces one NMI edge (`nmi_pending`), releasing does
  nothing, holding does not repeat [MD-HW §5, SP-PAUSE] [cap:irq.nmi_pause]. The press is sampled at the frame
  boundary where scripted input changes (section 11); mechanical debounce is outside the model. NMI acceptance
  follows the Z80 contract (never inside a prefix run). Oracle smoke: a 6-frame hold then a 1-frame press give
  exactly 2 NMIs on both finalists.
- An interrupt trace (T-state, source frame/line/pause, asserted/accepted/deasserted) is an artifact
  [cap:irq.trace]; the VDP trace (register writes, VRAM/CRAM write digests, status reads, IRQ edges) is another
  [cap:vdp.trace].

## 8. Reset state and BIOS policy

- The BIOS is never executed and BIOS bytes are never embedded or required. A supplied BIOS image is
  `SMS_ERROR_BIOS_UNSUPPORTED` [cap:bios.not_executed] [cap:bios.image].
- The profile starts in the documented post-BIOS state:
  - CPU: SEG-008 `z80_reset` (PC `$0000`, IM0, IFF1=IFF2=0, I=R=0, AF=SP=`$FFFF`, other registers `$FFFF`)
    [Z80-ABI §2]; normal software executes `DI`/`IM 1`/`LD SP,nn` first. See U1 [cap:reset.cpu].
  - Work RAM: all zero, except `$C000 = $AB`: the BIOS keeps the last port `$3E` value at `$C000` and software uses
    it when rewriting port `$3E` [MD-HW §6, SP-BIOS] [cap:memctl.ram_copy_c000] [cap:reset.ram].
  - Mapper `$FFFC..$FFFF = 0,0,1,2`; cartridge RAM zero.
  - Memory control `$AB`; I/O control `$FF` (all pins input) [cap:ioctl.reset_state].
  - VDP: registers as section 9.1 "reset", address/code 0, latch clear, read buffer 0, status 0, line counter `$FF`,
    VRAM and CRAM zero.
  - PSG: tone and noise registers 0, all attenuations `$F` (silent), LFSR `$8000`; no register latched (section 10)
    [SP-PSG]. [cap:reset.devices]
- Software that depends on other BIOS-left state (for example the exact SP) is a bounded open fact (U1).

## 9. VDP (315-5246, Mode 4)

### 9.1 Registers [MD-VDP §7, SP-VREG]

| reg | bits used (SMS 2) | reset |
| --- | --- | --- |
| 0 | 7 vscroll lock cols 24-31, 6 hscroll lock rows 0-15 (lines 0-15), 5 left column blank, 4 IE1 line IRQ, 3 sprite shift -8, 2 M4, 1 M2, 0 no-sync | `$36` |
| 1 | 6 display enable, 5 IE0 frame IRQ, 4 M1, 3 M3, 1 sprite 8x16, 0 sprite zoom; 7 and 2 no effect | `$80` |
| 2 | bits 3-1 name-table base (192 lines); bits 3-2 select `$0700/$1700/$2700/$3700` (224 lines) | `$FF` |
| 3, 4 | no effect in Mode 4 on the SMS 2 | `$FF` |
| 5 | bits 6-1 sprite attribute table base | `$FF` |
| 6 | bit 2 sprite pattern base (bit 8 of the pattern index) | `$FB` |
| 7 | bits 3-0 backdrop/overscan colour (sprite palette) | `$00` |
| 8 | horizontal scroll | `$00` |
| 9 | vertical scroll | `$00` |
| 10 | line counter reload value | `$FF` |

The reset column is the post-BIOS register state documented for the Alex Kidd SMS 2 BIOS path and used by both
finalists; the table-mask behaviour of unused bits (SMS 1) does not exist on the 315-5246 [MD-VDP §7, §15,
SP-VREG] [cap:vdp.revision_5246] [cap:vdp.vram_16k]. Register numbers 11-15 have no effect [MD-VDP §3] [cap:vdp.registers_0_10].

### 9.2 Control/data ports [MD-VDP §3]

- Command word: first byte = address bits 7-0, second byte = code (bits 7-6) and address bits 13-8. **The first
  byte updates the address low byte immediately** (the Genesis VDP differs) [cap:vdp.first_byte_low_address].
- A flag tracks first/second byte; it is cleared by a control-port read and by any data-port read or write
  [cap:vdp.control_latch].
- Code 0: read VRAM at the address into the buffer, increment. Code 1: VRAM write. Code 2: register write
  (register = second byte bits 3-0, value = first byte); data writes still go to VRAM. Code 3: data writes go to
  CRAM [cap:vdp.code0_prefetch] [cap:vdp.data_write_target].
- Data reads return the buffer, then refill it from VRAM[address] and increment; a data write stores the byte,
  loads the buffer with it and increments [cap:vdp.buffered_read] [cap:vdp.write_loads_buffer].
- The 14-bit address wraps past `$3FFF`; CRAM uses the low 5 bits (32 bytes, `--BBGGRR`, addresses `$20-$3F`
  alias `$00-$1F`) [MD-VDP §3, §5, SP-PAL] [cap:vdp.autoincrement_wrap] [cap:vdp.cram_32].

Oracle smoke (both finalists, identical): latch reset by a status read, low byte updated by a lone first byte,
buffer loaded by a write, wrap `$3FFF -> $0000`, CRAM `$21 -> $01`, and port mirrors.

### 9.3 Status and flags [MD-VDP §4, §10, §12]

- Bit 7 frame pending, bit 6 sprite overflow, bit 5 sprite collision; a status read returns them and clears all
  three, the line pending flag and the byte flag [cap:vdp.status_flags].
- Bits 4-0 are documented as undefined; the model returns `%11111`, the value both pinned finalists return
  [cap:vdp.status_low_bits].

### 9.4 Frame and line interrupts [MD-VDP §12]

- Frame pending is set on line `$C1` (192-line) / `$E1` (224-line) [cap:vdp.frame_irq_line]. Oracle smoke: the
  first V counter observed with the flag set, and inside the IM1 handler, is `$C1` on both finalists.
- Line counter: loaded from R10 on every line outside the active display except the line after it; decremented on
  lines 0-192 (192-line mode; 0-224 in 224-line mode); an underflow `$00 -> $FF` reloads it from R10 and sets the
  line pending flag. Writing R10 affects only the next reload [cap:vdp.line_counter] [cap:vdp.line_irq_pending].
  Oracle smoke: R10 = 15 gives 12 line interrupts per frame, the first at V `$0F`, the second at `$1F`.
- The *line* on which each event happens is fixed here; the T offset *within* the line is U2.

### 9.5 Modes [MD-VDP §6, §7]

| M4 M3 M2 M1 | SMS 2 result | contract |
| --- | --- | --- |
| 1 x 0 0, 1 0 1 0, 1 1 0 0, 1 1 1 1 | Mode 4, 192 lines | in scope [cap:vdp.mode4_192] |
| 1 0 1 1 | Mode 4, 224 lines | in scope [cap:vdp.mode4_224] |
| 1 1 1 0 | Mode 4, 240 lines (invalid display on NTSC) | `SMS_ERROR_VDP_MODE_UNSUPPORTED` [cap:vdp.mode4_240_ntsc] |
| 1 x 0 1, 1 1 0 1 | invalid text mode | `SMS_ERROR_VDP_MODE_UNSUPPORTED` [cap:vdp.invalid_text_mode] |
| 0 x x x | TMS9918 modes | `SMS_ERROR_VDP_MODE_UNSUPPORTED` [cap:vdp.tms9918_modes] |

(Exact rows from MacDonald's table: `1000/1010/1100/1111` = Mode 4, `1011` = 224, `1110` = 240, `1001/1101` =
invalid text.) R0 bit 0 set (no sync) also stops typed [cap:vdp.r0_bit0_nosync]. **Check point:** an unsupported
mode stops when it would affect output: at the start of a rendered line with the display enabled, or when a status
flag it would produce is read. Merely writing the register bits (for example during initialisation with the display
blanked) does not stop the machine [cap:vdp.mode_check_point].

### 9.6 V counter [MD-VDP §11, SP-VCNT]

NTSC 192-line: `$00-$DA`, then `$D5-$FF` (262 values); 224-line: `$00-$EA`, then `$E5-$FF`. Oracle smoke: the first
backward step (other than `$FF -> $00`) is `$DA -> $D5` on both finalists [cap:vdp.v_counter_ntsc].

### 9.7 H counter

Port `$7F` returns the upper 8 bits of the 9-bit H counter latched by the last TH transition of either port, frozen
until the next one [MD-VDP §11]. The value table as a function of the T offset in the line and the exact latch
trigger through I/O control are U3; until T004/T006 resolve U3, a read of the H counter stops with
`SMS_ERROR_HCOUNTER_UNRESOLVED` [cap:vdp.h_counter_latch] [cap:ioctl.h_latch_trigger].

### 9.8 Rendering rules [MD-VDP §8-§10]

- **Background.** Name table of 32x28 words (32x32 in 224-line mode) `---pcvhnnnnnnnnn`: pattern index (512),
  h/v flip, palette select, priority. Patterns are 8x8, 4 bitplanes, 32 bytes. Priority tiles cover sprites except
  where the tile pixel is colour 0 [cap:bg.name_table] [cap:bg.tiles_4bpp] [cap:bg.flips_palette] [cap:bg.priority].
- **Horizontal scroll** (R8: coarse column = upper 5 bits subtracted from 32, fine = lower 3); latched per line.
  R0 bit 6 fixes it to 0 on lines 0-15. The fine-scroll gap at the left edge shows the backdrop (U8)
  [cap:bg.hscroll] [cap:bg.hscroll_lock] [cap:bg.fine_scroll_gap] [cap:raster.per_line_latch].
- **Vertical scroll** (R9) wraps at 224 in 192-line mode (values above 223 behave as 0-31) and at 256 in 224-line
  mode; a change during the active display takes effect at the end of the active display. R0 bit 7 fixes it to 0 for
  columns 24-31 [cap:bg.vscroll] [cap:bg.vscroll_lock].
- **Left column blank** (R0 bit 5): pixels 0-7 show the backdrop [cap:bg.left_column_blank]. Oracle smoke: with the
  bit set, column 0 shows the backdrop colour and column 1 is unchanged on both finalists.
- **Backdrop** = sprite palette entry R7 bits 3-0 [cap:bg.backdrop].
- **Sprites.** 64 entries at R5 base: Y bytes at +0 (placed on line Y+1), X/pattern pairs at +`$80`. Y = `$D0` ends
  the list in 192-line mode only. At most 8 sprites per line; another sprite on the line sets overflow regardless of
  X or pattern (Oracle smoke: nine transparent sprites set bit 6). Overlapping opaque pixels set collision (Oracle
  smoke: bit 5). Lower entry wins; colour 0 is transparent; R0 bit 3 shifts left by 8; R1 bit 1 gives 8x16 (index
  bit 0 ignored); R1 bit 0 zooms all eight sprites (SMS 2); R6 bit 2 selects the upper 256 patterns; no horizontal
  wrap [MD-VDP §10, SP-VREG] [cap:spr.sat] [cap:spr.terminator] [cap:spr.limit_overflow] [cap:spr.collision]
  [cap:spr.priority_order] [cap:spr.shift] [cap:spr.size_8x16] [cap:spr.zoom] [cap:spr.pattern_base]
  [cap:spr.no_wrap].
- R1 bit 6 clear blanks the display to the backdrop [cap:raster.display_enable].

### 9.9 Palette and framebuffer artifact

- CRAM byte `--BBGGRR` [MD-VDP §5]. The presentation RGB888 value is `component x 85` per channel (`0, 85, 170,
  255`); this is a presentation mapping, not a hardware claim (the SMS 2 is linear, unlike the SMS 1 blue
  non-linearity [SP-PAL]) [cap:pal.cram_rgb].
- **Framebuffer artifact**: the active area only, 256 x 192 (or 256 x 224) bytes, row-major, each byte the 6-bit CRAM
  colour value of the pixel; its SHA-256 is the frame hash. Borders and blanking are not part of the artifact
  [cap:raster.active_area] [cap:fb.artifact]. Colour-exact comparison is therefore independent of any RGB
  conversion; reference renderers are compared through the colour-class bijection defined in ADR 0062.

## 10. PSG

Chip facts (Sega integrated SN76489 variant) [SP-PSG, MD-VDP §1]:

- Write protocol: `%1cctdddd` latches channel `cc` and type `t` (1 = attenuation) and writes `dddd` into the low bits;
  `%0-DDDDDD` writes the latched register: tone high 6 bits, attenuation low 4 bits, noise low 3 bits. A data byte
  after an attenuation or noise latch is **not** ignored (SP-PSG's Alex Kidd and Micro Machines cases). Tone
  registers update immediately on each byte [cap:psg.latch_data] [cap:psg.tone_immediate].
- A data byte before any latch byte stops with `SMS_ERROR_PSG_DATA_BEFORE_LATCH` (U5) [cap:psg.data_before_latch].
- Internal clock = system clock / 16 [cap:psg.divider_16]. Each chip tick every channel's 10-bit counter decrements;
  on reaching zero it reloads and flips the channel output [cap:psg.tone_counter]. **Tone period 0 behaves as period
  1** (the output flips every chip tick; 111,861 Hz) [GPGX-NOTE; SP-PSG lists `$001` as 111,861 Hz and describes
  periods 0 and 1 as a constant +1 at the audible output] [cap:psg.tone_period_0_1].
- Noise: counter reload `$10`, `$20`, `$40` or the tone 2 register (rate 3); the LFSR shifts once per two counter
  expiries (on the 0 -> 1 flip). 16-bit LFSR; white noise feeds `bit0 XOR bit3` into bit 15, "periodic" feeds bit 0.
  Any write to the noise register (latch or data) resets the LFSR to `$8000`. The channel output is bit 0 of the
  register **after** the shift, and 1 = channel on (SP-PSG reference implementation; GPGX-NOTE)
  [cap:psg.noise_rates] [cap:psg.lfsr_16_taps_0_3] [cap:psg.lfsr_reset] [cap:psg.noise_output_phase].
- Attenuation: 2 dB per step, `$F` = silence. Integer level table (SP-PSG, verbatim): `32767, 26028, 20675, 16422,
  13045, 10362, 8231, 6568, 5193, 4125, 3277, 2603, 2067, 1642, 1304, 0` [cap:psg.attenuation].
- Reset: tone/noise registers 0, attenuations `$F`, LFSR `$8000` [SP-PSG] [cap:psg.reset_state].
- Mono: SMS has no stereo register (Game Gear port `$06` is excluded) [MD-HW §7] [cap:psg.gg_stereo]. Analogue
  decay/filtering is not modelled [SP-PSG "imperfect SN76489"] [cap:psg.analog_imperfection].

Sample-generation contract (ADR 0064) [cap:audio.write_timestamp] [cap:audio.decimation] [cap:audio.pcm_format]
[cap:psg.mono_mix]:

1. The device is stepped in chip ticks (16 T). A PSG write carries the `io_out` T-state; the device first runs every
   whole tick that ends at or before it, then applies the write. The result is independent of how the host slices
   execution.
2. Tick level = sum over the four channels of `(output ? level[attenuation] : 0)` (0..131,068).
3. Output rate 44,100 Hz, integer arithmetic only. With `T_k = ceil(k x 39,375,000 / 485,100)` (the CPU clock
   39,375,000 / 11 Hz divided by 44,100 Hz, in T-states), output sample *k* covers every chip tick *j* whose start
   `16 x j` lies in `[T_k, T_(k+1))`. Each interval is 81.17 T long and always holds 5 or 6 ticks. Its value is
   `mean = floor(sum of tick levels / tick count)`, and the signed sample is `floor(mean x 65,535 / 131,068) - 32,768`
   (range -32,768..32,767; the fixed offset is the DC policy).
4. PCM = signed 16-bit little-endian mono; the artifact is the PCM bytes plus SHA-256 per run and per frame range.

PSG placement: a platform-neutral `libs/device/sega/psg` (ADR 0063).

## 11. Controllers, I/O control and scripted input

- Port `$DC`: bit 0-5 P1 up, down, left, right, TL (button 1), TR (button 2); bits 6-7 P2 up, down. Port `$DD`:
  bits 0-3 P2 left, right, TL, TR; bit 4 reset = 1 (no reset button on the SMS 2); bit 5 = 1; bit 6 port A TH;
  bit 7 port B TH. Pressed = 0 [MD-HW §4] [cap:pad.port_dc] [cap:pad.port_dd] [cap:pad.player2]
  [cap:pad.reset_button]. A read returns the state at the `io_in` T-state [cap:pad.read_timing].
- Port `$3F` (write): bits 3-0 = direction of B.TH, B.TR, A.TH, A.TR (1 = input); bits 7-4 = output levels of
  B.TH, B.TR, A.TH, A.TR. A pin configured as output reads back its output level in `$DC`/`$DD` (export console);
  a pin configured as input reads the pad (TH inputs read 1: no light gun) [MD-HW §4, SP-REGION]
  [cap:ioctl.direction_level] [cap:ioctl.output_readback] [cap:ioctl.nationalization]. Oracle smoke: `$F5` reads
  `$C0`, `$55` reads `$00` in `$DD & $C0` on both finalists.
- Only standard two-button pads; light phaser, paddle, sports pad, multitap, keyboard and Genesis pads are excluded
  [cap:pad.peripherals].
- **Scripted input format** (T003 driver, T006 consumer, T009 recorder) [cap:input.script]: UTF-8 text, one event per
  line, `<frame> <p1> <p2> <pause>` where `<frame>` is a decimal frame number (non-decreasing), `<p1>`/`<p2>` are
  6-character masks over `UDLR12` using `-` for released (for example `U---1-`), and `<pause>` is `P` (pressed) or
  `-`. An event takes effect at the start of that frame (T = frame x 59,736) and holds until the next event; `#`
  starts a comment. A pause edge is a transition from `-` to `P`.

## 12. Execution architecture (summary; ADR 0064)

- Generation time (C++): profile selection, ROM validation, mapper identity, the ImageSet and `emit_image_set`;
  `segarecomp build` routes SMS through it with an explicit platform/profile [cap:build.platform_route].
  Run time (C11, compiled with the generated code): memory map and mapper, port decode, VDP, PSG, controllers,
  scheduler, run API, artifacts.
- ROM data for `read` is embedded at build time as `static const` bank arrays (the Genesis convention, ADR 0006
  and `platforms/genesis/runtime/runtime.h`); the executable never opens the ROM.
- Run API: `sms_run_until_cycle`, `sms_run_until_frame`; resumable vs fail-closed stops; a fail-closed Z80 outcome or
  `SMS_ERROR_*` stops the machine permanently with PC, image identity and class [cap:exec.run_api]
  [cap:exec.error_surface].
- Headless driver: finite `--instruction-budget <N>` (ADR 0050 amendment), `--frames <N>`, `--input <script>`,
  artifact outputs [cap:exec.headless_budget] [cap:exec.state_digest] [cap:exec.mapper_trace].
- Viewer and headless share one guest loop; the viewer only paces, presents and samples input at frame boundaries
  [cap:view.pacing] [cap:view.input_frames] [cap:view.audio_out] [cap:view.equivalence].

## 13. Typed SMS error surface

Distinct from `Z80Outcome` and reported with PC, image identity, T-state and the offending value:

| error | raised by | owner |
| --- | --- | --- |
| `SMS_ERROR_PROFILE_UNSUPPORTED` | non-baseline region/TV standard/console at ingestion | T002 |
| `SMS_ERROR_MAPPER_UNDECLARED` | missing or unknown mapper identity (generation time) | T002 / T010 |
| `SMS_ERROR_MAPPER_UNSUPPORTED` | declared non-baseline mapper family (generation time) | T002 |
| `SMS_ERROR_ROM_SIZE_UNSUPPORTED` | ROM size outside 32-512 KiB power of two | T002 |
| `SMS_ERROR_CONTROL_BIT_UNSUPPORTED` | `$FFFC` bit 4 or bank shift; incompatible port `$3E` write | T002 |
| `SMS_ERROR_UNMAPPED_READ` | `rom_only` data read of `$8000-$BFFF` | T002 |
| `SMS_ERROR_BIOS_UNSUPPORTED` | a supplied BIOS image | T003 |
| `SMS_ERROR_PORT_UNIMPLEMENTED` | a decoded port class whose device is not implemented yet | T003 |
| `SMS_ERROR_VDP_MODE_UNSUPPORTED` | TMS9918/invalid text/240-line NTSC mode or R0 bit 0 at the check point | T004 |
| `SMS_ERROR_HCOUNTER_UNRESOLVED` | H counter read while U3 is open | T004 |
| `SMS_ERROR_PSG_DATA_BEFORE_LATCH` | PSG data byte before any latch byte | T007 |

## 14. Bounded open facts

| id | fact | bound / consequence meanwhile | owner | resolution method |
| --- | --- | --- | --- | --- |
| U1 | BIOS-left CPU state other than PC/IM/IFF (SP in particular) and RAM other than `$C000` | oracles disagree (Gearsystem SP `$DFF0`, ares SP `$FFFD` and `$C700 = $9B`); normal software sets SP first. The profile uses `z80_reset` values | T011 | any real title whose behaviour depends on it is attributed with the frontier tools; a cited BIOS behaviour then becomes a profile delta |
| U2 | T offset within the 228-T line of the frame flag, line flag, line-counter step, V counter increment and horizontal-scroll latch | MacDonald: "I don't have information about where events occur within a single scanline". The line is fixed (section 9.4); meanwhile events happen at T offset 0 of their line | T004 (T005 for the scroll latch) | measure both finalists (they model distinct in-line offsets) with project fixtures that poll status/V counter; adopt only a value both finalists agree on or that a public source states; otherwise keep offset 0 and record the difference as a validation tolerance |
| U3 | H counter value per T offset and the TH-latch trigger via port `$3F` | only relevant to software reading port `$7F`; stops with `SMS_ERROR_HCOUNTER_UNRESOLVED` | T004 (value table), T006 (trigger) | MacDonald's 342-pixel breakdown plus finalist agreement |
| U5 | PSG latch state before the first latch byte | GPGX-NOTE records tone 2 attenuation on 315-5313A/315-5660, ares uses channel 0 tone; untested on 315-5246. Stops with `SMS_ERROR_PSG_DATA_BEFORE_LATCH` | T007 | a public 315-5246 statement, else keep the stop |
| U6 | VDP access-slot loss for rapid data-port writes during active display (MacDonald §15, SMS 2) | writes always land [cap:timing.vdp_access_slots] | T012 | classify with evidence during the residual sweep; model only with a public timing source |
| U8 | content of the fine-scroll gap (backdrop "and sometimes pattern data from sprite #0") | backdrop only | T005 | finalist comparison plus the public statement; keep backdrop if unresolved and record the tolerance |

(U4 and U7 were resolved during T001: noise output phase, and status low bits; sections 9.3 and 10.)

## 15. Independent references (summary; ADR 0062)

| subsystem | primary | secondary / falsification |
| --- | --- | --- |
| memory map, mapper, port decode, interrupts, VDP ports/status/IRQ, raster/renderer | Gearsystem `704a92e` (GPL-3.0) | Genesis Plus GX `939ce4f` (non-commercial licence; local test-only use), plus the project-authored oracle smoke/fixture corpus |
| PSG chip | ares SN76489 component `4cb8d92` (ISC) with a two-entry deviation mask | Blargg Sms_Apu 0.1.4 (LGPL-2.1+, inside the Gearsystem pin) with a three-entry deviation mask |

References are test-only, obtained as ignored `.tools/sms-oracles/` checkouts at the pinned hashes
(`SEGARECOMP_SMS_ORACLE_CHECKOUT`), never linked into production and never run in CI.

## 16. Fixture builder

`tools/sms_fixture_rom.py` (stdlib only) assembles project-authored Z80 sources, writes the header, per-bank
markers and a mapper declaration, and is reproducible with `--check` against `tests/fixtures/sms-fixture-roms.json`
(sizes, declared mapper identity and SHA-256 only). ROM bytes are regenerated at test time and never committed
(ADR 0064).

## 17. Reproduce

```sh
python3 tools/sms_capabilities.py --check --report
python3 tools/sms_fixture_rom.py --check
SEGARECOMP_SMS_ORACLE_CHECKOUT=<dir> python3 tests/sms_oracle_smoke_test.py <cc>
SEGARECOMP_SMS_ORACLE_CHECKOUT=<dir> python3 tests/sms_psg_oracle_smoke_test.py <c++>
```

## 18. Authorized local images (sanitized aggregate)

Three authorized local SMS images (ignored `games/`), classified ephemerally through the primary reference for
900 frames without input: 3/3 carry a `TMR SEGA` header at `$7FF0` with region `$4` (SMS export); size classes
128, 256 and 512 KiB (one each); 3/3 write the Sega slot registers; 1/3 sets `$FFFC` bit 7; none sets `$FFFC` bits
2-4 in that window. No image is a source of any fact in this contract.
