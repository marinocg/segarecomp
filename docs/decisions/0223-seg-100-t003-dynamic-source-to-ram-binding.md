# ADR 0223: SEG-100-T003 - dynamic source-to-RAM binding of precompiled images

- Status: Accepted. Milestone SEG-100 (M4). Refines ADR 0127/0132 (overlay selection among *known* candidates) and ADR 0205 (the build-time `resolve_source`). Related: ADR 0221, 0222, 0224, 0225.
- Privacy: project-authored fixtures; for the authentic discs, classes and counts only.
- Gate: `DYNAMIC_MAPPING_GO` (mechanism, deterministic, adversarially tested; the authentic effect is ADR 0225).

## Question

Connect a precompiled source image to an actual guest RAM location **only after** the original game's loading behaviour has established the corresponding live bytes, with no predefined guest-base list, keeping *mapping selection* (which compiled image and source offset can serve this PC) separate from *execution verification* (the instruction's original bytes still match the live bytes).

## Decision: option A's evidence, option B's matching, no taint tracking

**Evidence (option A, device-owned).** The machine's delivery ledger (ADR 0205: the disc blocks its CDC decoder read) says which immutable bytes reached the machine. A candidate image is considered only if one of its sectors was delivered, and an identifying run never extends across an undelivered sector. A ledger that overflowed (256 ranges) is incomplete evidence and is **never used** (typed refusal `ledger_overflow`). Option A alone cannot bind: 7 of the 12 observed Sonic CD sources are main-processor destinations the sub's CDC never writes (the main CPU reads the data through the host port and copies it), so the ledger names *what* arrived, not *where* it went.

**Matching (option B, live-image).** When the dispatcher reaches a PC that no static `case` and no overlay group owns, `scd_dynamic_dispatch` (platforms/sega-cd/runtime/scd_dynamic.c) looks for the one candidate image the live bytes are an exact copy of, with the rule of the build-time resolver:

1. an anchor of 32 live bytes at the PC (shifted back only where the memory ends); if no delivered image has it (the module changed bytes after the PC - variables sit next to code), the anchor 32 and then 64 bytes **before** the PC;
2. every occurrence of the anchor in a delivered candidate is extended in both directions while the live byte equals the image byte and the block was delivered; the occurrence must put the PC on an even source offset and inside the run;
3. a run needs at least 64 bytes (`too_short` otherwise: a short coincidence is not an identity); more than 64 anchor occurrences is `ambiguous`;
4. the longest run wins; **a tie between distinct candidates is the typed stop `ambiguous_image`** - never decided by order, size or confidence.

The exact-copy run is the *evidence that identifies the image*; it is not the extent of what may execute. The binding is `(processor, image, base)` and serves the image's whole extent at that base (that is how a header the loader patched, or a variable the module changes, does not defeat identity - measured: 6 of Sonic CD's 9 module extents are payloads after a patched header).

**Every instruction is judged by its own bytes.** A 64-byte granule is *clean* when the whole granule was seen equal to the image (inside the identifying run, or by a comparison since the last write into it); an instruction in a clean granule costs a table lookup. A granule that was written since, or lies outside the run, is compared with the image once: equal -> clean; different -> *mutated* (data) and every instruction fetched in it is compared with the image's bytes at that offset - exactly the length of the body that would run - before the body is chosen. An instruction that differs is not served by that binding (the binding is skipped for it and kept for the rest of its image); if no other binding explains it a fresh search runs; a search that cannot explain it, or an identifying run that ends inside the instruction, is a typed refusal. The body's own `scd_code_guard` remains the last, unconditional check of every instruction. This was changed after the first authentic Sonic CD run (a module's flag byte, 12 bytes after intact code, made a whole-granule check drop a correct mapping): **identity needs a contiguous exact copy; executability is per instruction**.

**No CPU-wide taint tracking.** Following every `MOVE` from a CDC buffer to its destination would be a second, parallel machine model. The ledger plus byte equality establishes the same fact where it matters (the bytes about to execute are a copy of delivered immutable bytes) with a bounded search that runs once per binding event, not per instruction.

**What the runtime may and may not do.** It may select a precompiled body by `(image, source offset)`. It never decodes the opcode at the PC, lowers an instruction, produces or compiles a body, guesses a base, chooses among ambiguous images, or promotes a RAM snapshot into source authority: the image bytes are read from the verified disc (digest-checked at start-up), the live bytes are only compared. An unidentified candidate stops with the typed stop the static dispatch would have produced - `entry_not_compiled` at a PC no static case owns, `code_bytes_mismatch` at a PC whose static variants all disagree (the existing vocabulary of the build driver, so a frontier is resolved exactly as before, including the execution-derived extent fallback) - and `ambiguous_image` for a tie. The precise reason (`not_executable`, `ledger_overflow`, `no_delivery`, `no_source`, `too_short`, `ambiguous`, `body_not_compiled`, `disc_unreadable`, `instruction_differs`) is counted in the report.

## Mapping lifecycle

At most 16 bindings per processor, ordered by recency of use: the most recently used binding that explains an instruction wins; the least recently used is evicted and simply re-identified when fetched again. Bindings of overlapping extents (one image over another) coexist as hypotheses; the newer is tried first and a failing one is skipped.

- **Writes** (CPU stores, CDC DMA, exception frames - the existing `SCD_OVERLAY_NOTE_WRITE` points) mark the granules dirty and clear their *mutated* mark. **View changes** (PRG-RAM bank or window, Word RAM owner or mode - `scd_overlay_note_view_change`) mark every granule unknown.
- **Main and sub** have separate tables and the image is processor-neutral (ADR 0222): the same file may be bound by either processor, or by both at different bases. Word RAM visibility (1M/2M, ownership) is whatever `scd_code_byte` says the processor sees; a byte it cannot see never matches. A fetch from Word RAM first brings the CDC DMA and the graphics ASIC up to the instruction (as `scd_code_guard` does for a static body); a fetch from PRG RAM or work RAM compares the bytes as they stand, so a dynamic flow there is state-identical to a static one.
- **Computed control and returns.** `scd_pK_lookup` falls back to `scd_dynamic_lookup(K, pc)`, which resolves through the same binder (a jump into freshly loaded code is a compiled entry once its bytes are identified; an unidentified target is the unchanged `entry_not_compiled`). The processor's vector/handler check (`scd_compiled`) does the same.
- **Static variants first.** With dynamic images, a RAM PC that has static variants is a *chain*: the static variants whose bytes match run; if none does, the PC is offered to the bindings before it fails with `code_bytes_mismatch`. (Found by the first authentic run: an extent compiled for one module shadowed a different module later loaded at the same addresses.)

Mapping selection (`scd_dyn_resolve`/`scd_dyn_search`) and execution verification (the body's first statement) stay separate. A stale mapping can therefore never execute the wrong variant: at worst the guard stops `code_bytes_mismatch`.

## Evidence

- `tests/sega_cd_dynamic_binding_test.c` (a real `ScdMachine`, a cooked scratch track and stub bodies; 40+ checks): bind at a base given nowhere; the same image at two bases and on both processors; the longest run wins and a tie is `ambiguous_image`; no delivery, an incomplete ledger, unknown bytes, a short run, more anchor occurrences than the bound, an odd PC, a PC outside executable memory or beyond RAM, a hole in the body map; replacement of a file at the same base; a data write never fetched (no re-verification); **a variable changed next to intact code still runs, the changed instruction does not**; an unchanged-by-value write (compared once, trusted again); an identifying run shorter than the image (the rest is served once seen equal); a partial overwrite (each part runs only through the image its bytes copy; the first mapping is skipped, not lost); a bank change (nothing trusted, re-identified when the memory returns); the bounded table with least-recently-used eviction; the 24-bit mask; determinism (same events, order and search cost); the report.
- End to end (`tests/sega_cd_file_first_test.py`): modules are bound at the bases the game's loader chose (A at the Word RAM base, B over it, C at two bases) and the run reports equal the static reference's; a static extent that shadows an address no longer blocks a module loaded over it; `overwritten` is a typed stop, `header_patched` still runs.
- Authentic: ADR 0225.

## Not claimed

That a binding survives a Word RAM ownership swap without re-verification (it does not: it is re-verified); behaviour for files larger than 512 KiB (not candidates, ADR 0224); that two byte-identical segments in two different files can be told apart (they execute the same bodies; only a provenance name could differ, and a tie is refused rather than named); anything about audio or PCM.

## Gate

`DYNAMIC_MAPPING_GO` for the mechanism. Production adoption (a default) is not proposed: ADR 0226.
