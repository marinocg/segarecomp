# ADR 0282: Deterministic mod execution phases, guest writes, input interception and static hooks

- Status: accepted (SEG-133-T004, combined delivery F01). Normative text: [mod-execution-phases.md](../architecture/mod-execution-phases.md).
- Related: ADR 0278, 0279 (save states and rewind), ADR 0280 (adapters), ADR 0281 (schema 3), ADR 0283 (ABI 2).

## Decision

1. Mod callbacks run only on dispatch boundaries, take zero guest time, and never run inside an instruction, DMA step or interrupt acknowledgement.
2. Phases: `reset`, `frame-begin`, `input`, `post-input`, `hook-entry`, `hook-exit`, `frame-end`, `render` (read-only, outside guest time, from copies).
3. Guest memory writes: declared work RAM only, staged, bounded (1024 operations, 16 KiB per callback), typed width, endian-aware, committed atomically at callback end; ROM is never mutated at runtime.
4. Static hooks are resolved at build time against the exact image digest; unsupported site classes fail at build time with stable classes; there is no runtime opcode decoding.
5. A callback fault discards its effects, disables the mod for the session and surfaces a diagnostic; the guest continues unmodified.
6. Mod state is a versioned `MODSTATE` section in the existing save-state and rewind containers; restore is refused on any identity or fingerprint mismatch.
7. Determinism is a testable guarantee (replay, restore, rewind, no-op-hook equivalence). No hook promise exists for a CPU until its feasibility spike returns GO or PARTIAL.

## Consequences

SEG-138 implements phases and hooks against this contract; a NO-GO spike leaves hooks unsupported without changing any other rule. Rejected alternatives: runtime hook patching or decoding (violates the generated-native principle);
mod writes to any ROM or device space (unprovable determinism); immediate (unstaged) writes (no transactional rollback).
