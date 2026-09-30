# Master System

Code lives in `machine/` (generation-time C++: cartridge ingestion, mapper identity, ImageSet, SMS generation route
`emit_cartridge`), `runtime/` (strict C11 compiled with generated programs: memory map, Sega mapper, typed error
surface, port decode, machine composition, deterministic scheduler, run API, state digest, scripted input) and `headless/`
(the generated program's `main`, artifacts, and the no-device wiring). See ADR 0065 and ADR 0066. Device seams for the VDP,
PSG and controllers are typed and fail closed until T004/T006/T007 attach them; the viewer lands in T009.

**Purpose:** Static recompilation of Z80 + SMS VDP software.

**Profile:** Master System II (315-5246 VDP), NTSC, export region, documented post-BIOS state, declared mapper
identity. See [the machine contract](../../docs/architecture/master-system-machine-contract.md) and ADR 0061-0064.

**First checkpoint:** First deterministic frame of a project-authored fixture.

**Reuse:** SEG-008 Z80 CPU library, `codegen_c11_z80` and the Z80 runtime ABI; a platform-neutral PSG device
library `libs/device/sega/psg` (ADR 0063).

**Milestone:** SEG-009.
