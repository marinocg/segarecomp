# ADR 0002: Static Translation and Fail-Closed Execution

- Status: Accepted
- Date: 2026-08-04
- Amends: ADR 0001

## Context

A generated program could appear to support difficult control flow by embedding the source image
and decoding opcodes at runtime. That is an interpreter, does not prove static recompilation, and
can conceal unknown instructions or hardware behavior. A prematurely universal IR can likewise
move CPU semantics into emitter or runtime special cases.

## Decision

Generated C contains statically decoded and lifted instruction semantics. Runtime dispatch may
select only among statically emitted block identities; it must not fetch and decode target opcodes.
Unsupported instructions, unresolved targets, code writes, and unsupported bus/device operations
either reject translation or produce a deterministic fail-closed trap with source provenance.

IR operations use explicit widths and portable semantics. Shared primitives may be target-neutral,
but CPU-specific operations remain typed and explicit until both 68000 and Z80 frontends demonstrate
a sound common lowering.

## Consequences

Early fixtures intentionally support narrow instruction and hardware envelopes. Compatibility
reports are part of generated artifacts, and a successful exit means no reachable behavior escaped
the declared envelope. Dynamic translation fallback is excluded from the initial architecture.
