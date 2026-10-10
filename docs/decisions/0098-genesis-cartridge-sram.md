# ADR 0098: Bounded Genesis cartridge SRAM

- Status: Accepted.
- Predecessors: ADR 0006 (cartridge-data region ownership), ADR 0049 (immutable copy aliases), ADR 0097 (compatibility repairs; its non-goal "cartridge SRAM" and its idle-state `$A130F1` policy).

## 1. Question and scope

Two independent locally held images reach the same missing hardware class: a byte read or write inside the `$200000-$3FFFFF`
cartridge window that is declared by the ROM header as battery-backed SRAM (one failed closed as `rom_write_prohibited`, the other
ended through the missing cartridge-memory owner). This ADR adds the smallest sound owner of that class. Only non-reconstructable
classifications are recorded; nothing from any commercial image appears in this repository.

Non-goals (deliberately not decided here): persistence (`.srm` load/save), EEPROM/FRAM, S&K lock-on, the SSF2 / any bank-switching mapper,
a mapper plugin API, save states, SRAM-resident executable code.

## 2. Reference basis

- ROM header extra-memory field at `$1B0`: `"RA"`, a type byte, a second byte, start and end (Plutiedev "ROM header reference"). The documented
  type bytes are `$A0`/`$E0` (16-bit), `$B0`/`$F0` (8-bit, even addresses), `$B8`/`$F8` (8-bit, odd addresses); bit 6 is "saves on power-off".
  The second byte is `$20` for SRAM and `$40` for the EEPROM form.
- Plutiedev "Saving progress with SRAM": SRAM is reached through `$A130F1` (a BYTE register; 1 maps the SRAM into the upper 2 MiB of the
  cartridge window, 0 maps the ROM back) and uses every other byte of the declared range (`$200001`, `$200003`, ...).
- Genesis Plus GX `mapper_sega_w` ($A130F1): bit 0 maps the SRAM over `$200000-$20FFFF`, bit 1 (with bit 0) disables writes to it. Genesis Plus GX
  `sram_init` / MAME `md_slot`: SRAM is detected from the `"RA"` marker and the declared start/end, a span of `0x10000` or more is clamped to 64 KiB,
  and a cartridge whose ROM ends at or below the SRAM start has the SRAM mapped from power on. Genesis Plus GX fills the buffer with `$FF`.

## 3. Decision

**Ownership.** The descriptor is derived only from the immutable ROM header (`parse_genesis_cartridge_sram_header`, machine library) and
never from a title or checksum. The generic executable-image layer knows nothing about it. The shared address policy lives in
`address_space_contract.h` (one source of truth for the translation-time gate and the generated runtime).

**Supported layouts.** 8-bit SRAM on the odd lane (`$B8`/`$F8`) or on the even lane (`$B0`/`$F0`); start and end must lie on the lane's parity inside
`$200000-$3FFFFF` with `end - start < 0x10000`. Dense storage is one byte per lane address, at most 32 KiB, `(address - start) / 2`. 16-bit
SRAM (`$A0`/`$E0`) is *declared but unsupported*: its extent is recorded so any access to it stops with `unsupported_cartridge_sram_layout`.
Extra memory whose second byte is `$40` (EEPROM) creates no device; a malformed descriptor (unknown type or second byte, reversed or out-of-window or
oversized range, wrong parity) creates no device. Both leave the machine exactly as it was before this ADR (accesses in the window fail closed
with the existing diagnostics), so a title that never touches the window still builds.

**Accesses.** BYTE accesses on the lane's own addresses inside `[start, end]`, read and write. Every other access that touches the extent
(WORD/LONG, the opposite lane, an access straddling an edge) stops with `unsupported_device_access` / `unsupported_cartridge_sram_access`;
nothing is decomposed into bytes. Addresses outside the extent are unchanged.

**`$A130F1`.** A cartridge with supported SRAM owns the register: a BYTE write whose only set bits are bit 0 (map SRAM) and bit 1 (write protect)
updates two bits of state (zero at power on: ROM visible, writable). Any other value or width, and any read, stops with
`unsupported_cartridge_sram_access`. A protected store is ignored (hardware behaviour), not a fault. Cartridges without supported SRAM keep
the ADR 0097 idle policy unchanged (BYTE write with bit 0 clear). The register is *not* generalised to lock-on: the other bits remain undefined.

**ROM/SRAM overlay.** The extent is visible when bit 0 is set, or always when the loaded ROM ends at or below `start` (nothing lies beneath it).
Otherwise the extent is plain cartridge ROM (every width, immutable; a store is the ordinary `rom_write_prohibited`). Statically, any absolute
operand that touches the extent is a value-free `routed_device` fact (never an immutable ROM fold, never `rom_write_prohibited`), and the C4 fact
validators accept a fact naming the extent only in that form; register-indirect, indexed and other runtime-computed accesses already
reach `genesis_route_access`. The runtime checks the extent before the owned-ROM-region paths (ADR 0006) and before the ROM mirror, so nothing
outside the declared extent becomes mutable. Instruction fetch from the extent is unchanged (SRAM-resident code remains unsupported).

**Storage.** In memory only: a generated, bounded `uint8_t[storage_bytes]` array plus a generated `const` config, installed by `main`
(`genesis_cartridge_sram_install`), filled with `$FF` (deterministic, never random). No file is read or written and the ROM is never mutated.
The storage is a plain byte array so a later persistence feature can load/save it without redesign.

## 4. Evidence

`genesis_cartridge_sram_test` (descriptor parsing, malformed/unsupported forms, the shared access classifier) and
`genesis_cartridge_sram_generated_test` (synthetic ROMs through the production emit route, strict C11, deterministic runs: static absolute,
register-indirect and indexed accesses; first/last declared address; overlay at power on / enable / disable / re-enable with the stored byte intact;
write protect; every fail-closed form; 16-bit and EEPROM headers; a cartridge without SRAM keeps the ADR 0097 behaviour). Commercial images were
used only for ephemeral validation; results are recorded as classifications in the pull request.

## 5. Consequences

Images whose header declares standard 8-bit SRAM now run past their first SRAM access with in-memory, power-on-fresh contents. Titles that need
persistence, EEPROM, lock-on or bank-switched mappers still stop at a typed frontier.
