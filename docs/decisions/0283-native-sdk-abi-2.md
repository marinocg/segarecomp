# ADR 0283: Native SDK ABI 2 and capability negotiation

- Status: accepted (SEG-133-T003, combined delivery F01). Normative text: [native-sdk-abi-2.md](../architecture/native-sdk-abi-2.md).
- Related: ADR 0267 (ABI 1), ADR 0280, 0281, 0282, 0284.

## Decision

1. ABI 2 is a versioned provider table plus a host service table, negotiated by manifest-declared capabilities. It coexists with ABI 1, which is unchanged; a package declares exactly one ABI and mixed ABIs in one build are rejected.
2. The capability catalogue (ids 1-15, append-only) is the single model for native C11 and Luau; Luau bindings exist only for granted capabilities.
3. There is one typed feature table per platform; no universal struct. Spaces are named constants, passed explicitly.
4. No raw runtime pointer is exposed; memory goes through host services, render views are call-scoped copies.
5. Missing required capability, platform mismatch, minor too new and every other negotiation failure is a build-time rejection with a stable diagnostic class.
6. Evolution is append-only within a major; the ABI 2 header is documented here and is not added to the ABI 1 header.
7. Guest-affecting native mods must declare `state.mod` or save states and rewind are disabled with a reason.

## Consequences

SEG-135-T003 and SEG-138 implement against this document. Rejected alternatives: extending ABI 1 in place (would change the meaning of installed packages); a single universal feature struct (rejected by ADR 0280); capability bits chosen per release (rejected: ids are append-only forever).
