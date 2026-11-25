# ADR 0321: Provider read of declared ROM ranges (`m68k.rom`)

- Status: accepted (SEG-152-T018). Builds on [ADR 0300](0300-scheduled-mods-in-built-programs-diff-run-test-and-gameplay-example.md) (declared memory ranges), [ADR 0305](0305-live-scheduled-mods-and-native-abi2-providers-in-built-programs.md) (the native scheduled provider) and [ADR 0320](0320-abi-2-exact-frame-presentation-and-parity-contract.md).
- Scope: ABI 2 stays at minor 3. No header, struct, enum or capability-id change; no write access; no other space; no live-phase read; no game-specific product code.

## Context

A generated program holds no cartridge buffer at run time (only proven-immutable owned regions). A provider that must reconstruct presentation data keyed by data the guest does not hold in RAM (static tables of the cartridge) therefore had no source.
The build already derives the EFFECTIVE image (original verified by digest, typed edits and `bps-image` payloads applied and checked against their declared target digests) and already emits the declared work-RAM ranges as generated C.

## Decision

1. `[[memory]]` accepts `space = "m68k.rom"`: `access = "read"` only (`E_MEMORY_ACCESS`), a non-empty `revision` (`E_MEMORY_REVISION`), a non-empty length (`E_MEMORY_RANGE`), backed by the existing capability name `memory.read.m68k.rom` (id 7).
   The sum of one revision's ROM range lengths is at most 4096 bytes (`E_MEMORY_ROM_BUDGET`), a generic bound, not a game rule. The existing 16-range cap, canonical order and duplicate rules apply.
2. Offsets are byte offsets into the revision's effective image. At build, a range outside the image is `E_MEMORY_RANGE`; otherwise the build emits exactly the declared bytes (`segarecomp_mod_rom_data`, concatenated in range order, next to the
   `segarecomp_mod_ranges` array which gains entries with the ROM space id). A byte outside every range does not exist in the program's mod tables. Nothing enters the package, repository, catalog or release; the bytes live only in the user's own local build.
3. The viewer hands the bytes to the scheduler through the host-internal `GenesisModConfig::rom_data` / `rom_data_bytes` (not SDK/ABI). The scheduler refuses a ROM range with a write mode, without `memory.read`, empty, or whose bytes do not match the lengths.
4. `service_read` for the ROM space: render only; width 1/2/4 big-endian; even offset for 2/4; inside ONE declared range; at most 4096 reads and 8192 bytes per callback. Check order capability, phase, width, alignment, range, budget. Every violation faults the
   callback like the other spaces. Writes to the ROM space stay `E_READONLY`. Other spaces and phases are unchanged.
5. Identity: the bytes are immutable and a pure function of (effective image, declared ranges). The ranges array and the materialised bytes are part of the scheduler configuration digest, and the effective digest is part of the variant identity, so a
   different range set or image refuses a state (`E_STATE_MOD_MISMATCH`). Reads happen in `render`, outside guest time; restore, rewind and replay are unaffected and no `save_state` data is added.
6. Old hosts: a host that does not list `memory.read.m68k.rom` refuses the package at the reader. Providers that do not name the capability are untouched.

## Consequences

Native providers are trusted host code (not sandboxed): the declaration, capability and budget rules make accesses explicit, bounded and reproducible. A later record may widen the phases. The changed existing expectation: `mod_package_test.py` previously used
`m68k.rom` as its example of a space the platform does not expose; it now uses `z80.ram`.
