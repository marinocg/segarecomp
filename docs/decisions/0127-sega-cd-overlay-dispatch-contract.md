# ADR 0127: Sega CD overlay dispatch contract (design only)

- Status: Accepted as design (SEG-062-T001). Implemented by SEG-063-T001. Nothing here is implemented in SEG-062.
- Related: ADR 0126, ADR 0049/0122 (byte guards), `docs/architecture/sega-cd-machine-contract.md` sections 4, 7, 11.

## Context

Sega CD titles load modules from disc into PRG RAM and Word RAM by CDC DMA or CPU copies, frequently at the same
addresses. Generated-native execution must pick, at run time, which precompiled image occupies a PC, without
decoding guest bytes and without trusting a "loader finished" event that the hardware does not provide.

## Decision

1. **Finite registry.** Per processor instance, a build-time-known, ordered registry of candidate compiled images
   (the processor's `ExecutableImageSet` with `platform_selector = true`). No image is added at run time.
2. **Platform-owned selection.** The key is `(processor instance, PC)`; the candidates are the images whose mapping
   covers PC. The runtime owns mapping and selection; the codegen emits one compiled-entry table per image.
3. **Validation by comparison.** A candidate is live only if the bytes it executes equal the live memory bytes per its
   verification contract (`byte_identity`: every byte of every instruction executed; `structural`: the CPU-defined
   structural bytes). Runtime **comparison against known precompiled images** is allowed; runtime decoding of guest
   opcodes and runtime compilation are not.
4. **Unambiguous choice.** Several live candidates at one PC resolve in the registry's fixed deterministic order only if
   their compiled behaviour for the executed span is identical (same bytes); otherwise the typed stop
   `ambiguous_image`.
5. **Invalidation.** Any write to executable memory - CPU store, CDC DMA, PRG-RAM window copy - marks the written
   64-byte granules dirty; a selection covering a dirty granule is revalidated before its next instruction. Cost bound:
   O(written bytes) to mark, O(instruction length) per revalidation, O(candidates at PC) per selection.
6. **Typed failures**: unknown image (no candidate), ambiguous candidates, partially overwritten image (some bytes
   match), mismatched live bytes, missing precompiled entry, stale selection after overwrite.
7. **No privileged loader event** is required; DMA and CPU copies are ordinary writes.

Required negative tests for SEG-063-T001: wrong module loaded, partial overwrite, mismatched live bytes, ambiguous
candidates, missing precompiled entry, stale selection after overwrite. No generic loader framework.

## Go/no-go criterion

GO for SEG-063 if a fixture with two modules sharing a load address runs both, selected only by live-byte comparison,
and all six negative tests stop with distinct typed errors. NO-GO (refine) if any title-observed module requires a key
other than `(instance, PC, live bytes)`.

## SEG-062 position

SEG-062 uses the degenerate case (one image per instance, `platform_selector = false`), with the per-instruction
`byte_identity` check of contract section 11 - the same comparison rule, without selection.
