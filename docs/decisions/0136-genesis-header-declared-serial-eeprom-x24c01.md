# ADR 0136: Header-declared serial EEPROM (X24C01 profile)

- Status: Accepted.
- Task: SEG-068-T006.
- Predecessors: ADR 0098 (cartridge SRAM; this ADR is its stated "EEPROM" non-goal for one class), ADR 0006, ADR 0097.

## 1. Question

A locally held cartridge declares extra memory with the EEPROM header form and stops early with `rom_write_prohibited` at its first
byte write to the `$200000` window. What device is it, how is it wired and sized, and how is the class recognised without a title list?

## 2. Evidence (public sources) and its limits

| Claim | Source | Kind |
| --- | --- | --- |
| The EEPROM header form is `'RA',$E8,$40` + start + end (the range "should indicate on which range EEPROM is accessible"); the header "doesn't include information about the way the EEPROM is wired, and different publishers opted for different ways to wire it". | Plutiedev, "ROM header" (plutiedev.com/rom-header), "If it has EEPROM" | hardware/documentation fact |
| Serial EEPROMs of this era use a two-wire protocol: START = SDA falling while SCL high, STOP = SDA rising while SCL high, MSB-first bytes sampled on SCL, 9th clock acknowledge. The X24C01 (1 Kbit, 128 x 8) takes a single byte holding a 7-bit word address and the R/W bit ("Mode 1"); the 24C02 and larger take a device byte then a separate word address. | Genesis Plus GX `core/cart_hw/eeprom_i2c.c` header comment (8BitWizard's notes, gendev.spritesmind.net/forum/viewtopic.php?t=206); Xicor X24C01 / Atmel AT24C01 datasheets (1 Kbit, 4-byte page write) | hardware fact (protocol), emulator documentation |
| For the Sega cartridge boards the lines are: SDA (input and read-back) on D0, SCL on D1, at an odd address in the `$200000` window. For this title: SDA D0, SCL D1, 24C01 (size mask `$7F`, page mask `$03`), reported "works". | MAME `src/devices/bus/megadrive/eeprom.cpp` mapping table (derived from Eke-Eke's notes); Genesis Plus GX `mapper_i2c_sega_init`; MAME software list `hash/megadriv.xml` for this title: PCB `171-6584A`, `ic5 = AT24C01P`, slot `rom_eeprom_mode1` | independent emulator-community observation, corroborated by the board part list; per-title evidence |
| Other boards wire differently (EA: SCL D6 / SDA D7; Acclaim: 24C02 and larger; Codemasters J-cart). | same MAME table / GPGX database | per-title evidence |
| For a header-declared EEPROM that is in no database Genesis Plus GX assumes the Sega mapper with an X24C01. | GPGX `eeprom_i2c_init`, "assume SEGA mapper as default" | emulator compatibility assumption |

**Therefore:** the header proves *that* the cartridge has a serial EEPROM at one location; it proves neither capacity nor wiring. No
public document derives either from the header. The class selection below is a **project compatibility assumption**, labelled as such.

## 3. Decision

**Classification (no title or checksum list).** The header form `'RA',$E8,$40` whose range is exactly one odd byte location inside
`$200000-$3FFFFF` (`start == end`, odd) creates a serial-EEPROM device with the **Sega-board X24C01 profile**: 128 x 8 bits,
7-bit word address + R/W, 4-byte write page, SDA = D0 (input and read-back), SCL = D1. This is the GPGX default and is the only profile
this model implements. Every other EEPROM form (another type byte, a span, an even or out-of-window location) creates no device and
keeps the pre-existing fail-closed outcomes. A cartridge wired differently is misidentified by this rule and will see a device that
never acknowledges its lines; this residual is accepted and documented, not detectable from the header.

**Accesses.** The only EEPROM accesses are a BYTE read (bit 0 = SDA the device or bus presents; other bits 0) and a BYTE write
(bit 0 = SDA, bit 1 = SCL, others ignored) at exactly the declared location. Any WORD/LONG access covering the location stops with
`unsupported_device_access` / `unsupported_cartridge_sram_access` (the outcome class is shared with ADR 0098); an odd-address
WORD is rejected earlier by the unchanged odd-effective-address stage; a byte next to the location is not the device and keeps its
unchanged outcome. Nothing is decomposed.

**Overlay with the ROM.** The location is the device whenever the loaded ROM ends at or below it (the device sits over nothing). If
the ROM reaches the location the ROM/device overlay (mapper bank state) is not established: the layout is *declared but unsupported*
and any access stops with `unsupported_cartridge_sram_layout`. `$A130F1` belongs to the SRAM mapper only: an EEPROM cartridge keeps
the ADR 0097 idle policy for it.

**Device state machine.** START/STOP detection, MSB-first byte assembly sampled on SCL rising edges, a counter advanced on SCL falling
edges, acknowledge on the 9th clock (device pulls SDA low after the address byte and after each written byte; the master's ACK/NACK
after each read byte advances the address or ends the read), page-write wrap inside the 4-byte page, read address wrap at 128.
Between those events the read-back is the last SDA level written (open-drain idle). Write-cycle time is **not** modeled (writes are
instantaneous) and factory contents are `0xFF`: both are project assumptions, not hardware facts. In memory only, power-on fresh,
deterministic; persistence stays a non-goal.

## 4. Ownership and tests

Header descriptor: `parse_genesis_cartridge_sram_header` (machine library, `kind = serial_eeprom_x24c01`); shared address policy:
`address_space_contract.h` (same classifier as ADR 0098, kind constants); generated config + install:
`genesis_cartridge_sram_install` with a `kind` field; device logic: the runtime owner (`genesis_eeprom_update`, `genesis_route_access`).
Tests: `genesis_cartridge_sram_test` (descriptor classes, the single-location rule, overlay, residual forms),
`genesis_cartridge_eeprom_generated_test` (strict C11 bit-banged protocol: power-on contents, write/read-back, page wrap, sequential
read with wrap, NACK, all fail-closed forms, `$A130F1`), unchanged `genesis_cartridge_sram_generated_test`.
