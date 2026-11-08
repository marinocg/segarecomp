# ADR 0280: Mod platform adapters and address-space ownership

- Status: accepted (SEG-146-T001, delivered in combined delivery F01).
- Related: ADR 0266, 0267 (patch packages, native SDK ABI 1), ADR 0278, 0279 (save states), [mod-platform-adapters.md](../architecture/mod-platform-adapters.md),
  [modding-platform-gap-inventory.md](../architecture/modding-platform-gap-inventory.md).

## Context

Mod support must cover Genesis, Master System and Sega CD without a universal machine abstraction. The platforms differ in CPUs, address spaces, ownership (Sega CD Word RAM),
source identity (cartridge digest vs disc) and mapper behaviour.

## Decision

1. An adapter is an existing platform runtime plus a capability table. No new abstraction layer, bus interface or CPU interface is created.
2. The shared vocabulary is exactly: platform identity, source-identity kind, capability names, named address spaces, phase names and support levels.
3. Address spaces are named `<owner>.<region>`, passed explicitly on every access, and carry owner, phases, read/write/hook eligibility, endianness and width rules.
4. Only declared work-RAM spaces (`m68k.work_ram`, `z80.work_ram` on Master System, `main.work_ram` on Sega CD) are mod-writable. ROM is never mutated at runtime.
   Genesis/Sega CD Z80 RAM, cartridge RAM, backup RAM and device registers are not writable.
5. Site resolution is build-time, per verified source identity; mappers and disc modules are never decoded at runtime.
6. Any further sharing needs the four-point review gate in the architecture document.

## Consequences

Later contracts (schema 3, ABI 2, phases) cite the tables by space name. A platform added later supplies its own table; it does not extend a shared interface.
Rejected alternatives: a universal bus/CPU interface (rejected: no two platforms share behaviour that a test could assert identically); numeric space ids (rejected: ambiguity).
