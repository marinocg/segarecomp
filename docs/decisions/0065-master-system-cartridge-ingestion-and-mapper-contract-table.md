# ADR 0065: Master System Cartridge Ingestion, Single Mapper Contract Table and Memory-Map Error Semantics

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T002
- Related: ADR 0058 (image identity), ADR 0061 (mapper identity), ADR 0063 (placement), ADR 0064 (execution architecture),
  `docs/architecture/master-system-machine-contract.md` sections 3-5 and 13.

## Decision

1. **One mapper contract table.** `platforms/master-system/runtime/sms_mapper_contract.h` is plain C11 (valid C++). It holds
   the constants (bank size, fixed 1 KiB, register base, reset values, identities) and the slot table `{window_base,
   first_offset, length, bank_register}`, plus the pure functions `sms_sega_rom_offset` and `sms_mapper_code_image`.
   The C11 memory map (`sms_memory.c`) and the generation-time ImageSet builder (`machine/src/image_set.cpp`) both consume
   it, so the declared windows and the run-time `code_image` cannot drift apart by construction. A test additionally
   enumerates (register state, address) classes and checks the two sides against each other, with mutation controls on both.
   This follows the Genesis `address_space_contract.h` precedent; it is not a generic bus abstraction.
2. **Identities.** Image 1 is the invariant first 1 KiB (Sega) or the whole 32 KiB ROM (`rom_only`); bank *n* is image
   *2 + n*. Every bank image has the three slot windows, so its owners are window-relative (ADR 0058 reference shape). The
   SMS route emits exactly the image set measured by `z80_static_budget.sms_spec`; a test proves the emitted files are
   byte-identical to the generic emitter over that spec, so the SEG-008 measured costs apply unchanged.
3. **Ingestion order and explicit selection.** Profile (header platform/region), then ROM size, then the declared mapper,
   then the family-specific size. The header identifies platform/region only. Explicit profile selection admits a ROM with
   no or ambiguous/invalid header, and never overrides a recognized Game Gear, Japanese or other-console header. Declarations
   are resolved by source precedence; differing names are `SMS_ERROR_MAPPER_UNDECLARED`, as are a manifest without a mapper,
   without the input SHA-256 or with a different one. The header checksum and size code are recorded (match/mismatch/not
   evaluated) and never decide acceptance.
4. **Sticky run-time errors.** The Z80 ABI cannot abort a run from a callback. The memory map latches the first `SmsError`
   (address, value, T-state), then returns `$FF` for reads, ignores writes and answers `code_image` 0, so the state observed at
   the stop is not advanced. The platform checks `error` after every `z80_run` and never resumes. The rejected `$FFFC` write
   (bit 4 or non-zero bank shift) does not update the register or the RAM copy.
5. **ROM embedding.** The generation route writes `<stem>_rom.c` (`sms_rom_data`, `sms_rom_size`, `sms_rom_mapper_family`)
   and `<stem>_cartridge.json` (identity provenance; no paths) next to the sharded image C and appends the ROM unit to
   `<stem>.units`. The executable never opens the ROM (ADR 0064 section 2).
6. **Typed error surface.** `sms_error.h` defines the whole T001 section 13 error enum once, with stable values; classes owned by
   later tasks are defined but unused until their owner lands.

## Consequences

- T003 binds `sms_memory_host_*` into `Z80Host`, calls `sms_memory_reset`, routes port `$3E` writes to
  `sms_memory_control_write`, and reads `sms_memory_io_disabled` for the `$C0-$FF` read decision.
- T010 routes `segarecomp build` for SMS through `emit_cartridge` and adds `--mapper`/`--manifest` handling that builds
  `MapperDeclaration` values (`parse_mapper_manifest` already validates the manifest shape and digest).
- No change in `libs/cpu/z80`, `codegen_c11_z80` or `z80_runtime.h`, and no Genesis dependency (checked by
  `sms_machine_dependency_test`).
