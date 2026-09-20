# Recompiler Pipeline

## Goal

Produce deterministic C11 and a small runtime from a console image without depending on the
original CPU at execution time. Preserve observable CPU, memory, and device behavior within an
explicit compatibility envelope.

## Stages

1. **Image ingestion** validates container/header data and constructs target memory regions.
2. **Decode** turns bytes into typed target instructions without assigning high-level meaning.
3. **Lift** maps instructions to typed IR with explicit state and source provenance. Shared primitive
   operations remain target-neutral; CPU-specific semantics remain explicit until reuse is proven.
4. **Discovery** starts from vectors and known entry points, recursively recovers direct control
   flow, and records unresolved indirect edges rather than guessing.
5. **Analysis** resolves code/data, indirect targets, bank mappings, and safe transformations.
6. **Emission** lowers IR to deterministic C11 translation units and metadata.
7. **Runtime** implements memory access, scheduling, devices, and deterministic traps.
8. **Validation** compares state transitions with independent emulators and legal fixtures.

## Contracts

- Addresses are typed by address space; host pointers never stand in for target addresses.
- Integer width, signedness, wrapping, shifts, flags, and target endianness are explicit.
- Each instruction and IR operation retains image offset, target address, and byte length.
- Unknown decode or control flow is represented as an error or unresolved edge, never a no-op.
- Generated code contains statically lifted semantics. The runtime never fetches and decodes target
  opcodes; dispatch is limited to statically emitted block identities.
- Reached unsupported behavior fails closed with a stable stop reason and source-address diagnostic.
- Target-specific memory and device policy stays behind backend interfaces.
- Generated output order and symbol names are stable for identical inputs and options.

## Initial Shape

The present `RomInfo` and C manifest are a header-detection prototype, not validated image ingestion
or recompilation. Add contracts in backlog order: honest ingestion, executable mapping, then one
68000 instruction through decode, lift, emission, compiled C, and differential execution.

## Target Families

| Family | Systems | CPUs | Initial status |
| --- | --- | --- | --- |
| 8/16-bit | Genesis, Master System, Game Gear | 68000, Z80 | Active |
| 32-bit | Saturn | dual SH-2 plus auxiliaries | Deferred |
| 128-bit era | Dreamcast | SH-4 plus ARM7 | Deferred |
