# Master System

Documentation-only platform goal; no code yet (see [../README.md](../README.md)).

**Purpose:** Static recompilation of Z80 + SMS VDP software.

**Profile:** Master System II (315-5246 VDP), NTSC, export region, documented post-BIOS state, declared mapper
identity. See [the machine contract](../../docs/architecture/master-system-machine-contract.md) and ADR 0061-0064.

**First checkpoint:** First deterministic frame of a project-authored fixture.

**Reuse:** SEG-008 Z80 CPU library, `codegen_c11_z80` and the Z80 runtime ABI; a platform-neutral PSG device
library `libs/device/sega/psg` (ADR 0063).

**Milestone:** SEG-009.
