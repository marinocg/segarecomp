# ADR 0132: Sega CD overlay activation and digest-checked module lists

- Status: Accepted (SEG-063-T001). Implements ADR 0127; refines it where the design left the rule open.
- Related: ADR 0126 (images per processor instance), ADR 0127 (overlay dispatch contract), ADR 0129 (disc boundary),
  ADR 0049/0122 (byte guards), `docs/architecture/sega-cd-machine-contract.md` section 15.

## Context

ADR 0127 fixed the key `(processor instance, PC, live bytes)`, a finite build-time registry, comparison of known images (never
decoding), 64-byte dirty granules and six typed failures. Implementing it exposed four questions the design did not settle:
what the compiled unit is when one image has several mappings; what "live" means when two images share instructions but differ
elsewhere; what stops execution of a replacement image through a return address or fall-through belonging to the replaced one;
and how a module is admitted without trusting a filename.

## Decision

1. **Compilation unit = one mapping of one image.** Each (image, mapping) pair is traversed from its declared roots and lowered
   with the unchanged CPU-owned decode/lift/lowering; functions are named per image (`scd_p<k>_m<id>[_map<j>]_i_<pc>`). The same
   bytes at two bases are two units (every absolute PC differs). The generic `ExecutableImageSet` header is unchanged; image
   names and entry roots are platform roles (`ImageRole`) parallel to the set.
2. **Overlay groups.** Units whose ranges overlap form connected components; a component that contains a disc-module unit is an
   overlay group (at most 16 groups, 16 candidates each, 512 KiB each). Boot-only components keep the SEG-062 per-PC switch, so a
   program without modules is byte-identical to SEG-062 output. A PC inside a group is **absent from the per-PC switch**: its
   default calls the platform selector, which calls the selected candidate's own dispatcher. The computed-control table
   (`scd_p<k>_entries`, used by RTS/JMP/vector checks) is the union of all units: it authorises an address, never an image; every
   computed transfer only sets the PC, and the next dispatch selects.
3. **Coherent liveness.** A candidate is live only when **every byte of its whole execution range** equals its image, compared
   through per-granule digests (first 8 bytes of SHA-256 of the image bytes in each 64-byte granule of the absolute grid)
   computed from live memory through the processor's current bus view. Instruction-level matches never select an image, so a
   mixture of A and B is never executed even where individual instructions coincide. The per-instruction byte guard (contract
   section 11) still compares each executed instruction's bytes exactly and remains the authority for instruction semantics;
   the digest is used only to choose between known images. Module bytes are not embedded in generated C (ADR 0129): only the
   instruction verification bytes (as in SEG-062) and the granule digests.
4. **Unambiguous choice.** Exactly one live candidate covering PC, and no other live candidate overlapping it, is selected.
   Otherwise `ambiguous_image`. ADR 0127's "same bytes resolve in registry order" case is closed at build time instead: two
   modules with identical bytes over the same addresses are a `duplicate_module` list error, so two simultaneously live candidates
   always differ in range or entry set and are never resolved by order.
5. **Invalidation and revalidation.** CPU stores (both processors, PRG-RAM window included), CDC DMA words and exception frames
   mark the granule they change in every group whose processor currently sees that byte; a change of what a processor sees
   (PRG-RAM bank or window, Word RAM owner or mode) marks whole groups. A selection whose group is dirty is revalidated, over the
   dirty granules only, before the group's next instruction; every observation first brings an in-flight CDC DMA up to the
   instruction tick. A revalidation that keeps the selection also re-applies point 4: every other candidate overlapping it
   that a dirty granule touches is checked, first on its dirty granules and in full only if those all match; a newly live one is
   `ambiguous_image`. Cost: O(written bytes x groups) to mark, O(dirty granules) to revalidate (plus O(candidate bytes) for an
   overlapping candidate whose dirty granules all match), O(group bytes) for a fresh selection.
6. **Re-entry after replacement.** When revalidation fails the group becomes stale; until the next successful selection it can
   be re-entered only at a **declared entry root** of the newly live candidate. While the group is stale, a return address,
   fall-through or computed jump into the replaced image is the typed stop `stale_image_selection`, and no instruction of the
   replacement runs from it. The rule is bounded by that selection: once the replacement has been entered at a root, it is the
   selected image, and a later transfer to any of its compiled entries runs it (the runtime keeps no per-return-address
   provenance). What runs is then always the one whole-range-verified live image with its byte guards, never a mixture; a stale
   return address that lands on a compiled entry of that image executes it, as the hardware would. No loader completion event is
   assumed: DMA and copies are ordinary writes.
7. **Typed failures** (stop detail = image id): `image_unknown` (the instruction at PC is no candidate's), `image_bytes_mismatch`
   (a known image's instruction is at PC but its range holds bytes no admitted image explains), `image_partially_overwritten`
   (every mismatching granule of that image is explained by another candidate over the same byte range: a mixture),
   `ambiguous_image`, `image_entry_not_compiled` (the selected image has no compiled entry at PC), `stale_image_selection`;
   PCs outside every image keep `entry_not_compiled`.
8. **Module lists are the only admission authority.** A module is admitted only by an explicit list entry
   (`segarecomp.sega_cd.modules.v1`) naming the disc identity, an ISO9660 path and/or a data-track extent (which must agree), the
   SHA-256 of those bytes, the owning processor, execution bases inside one executable RAM region of that processor, entry roots
   and `byte_identity` verification. Nothing is inferred from names, extensions or contents. The canonical admitted list has an
   identity that enters the program identity. ISO9660 is resolved at build time only (bounded subset, `libs/media/iso9660.hpp`);
   the runtime never parses it.
9. **Producers.** Supported for the Sega CD: the boot IP/SP and disc modules; one module at several addresses is several
   mappings of one image (ADR 0126 row 3). The ADR 0049 static-proof copy and ADR 0073/0122 bounded materialization producers are
   cartridge-ROM authorities with no disc-to-RAM proof in the existing contracts; the list has no directive for them. No
   decompression or relocation producer exists.

## Review: where generated code could assume a PC identifies a function

Every Sega CD instruction function ends by storing the next PC and returning to the scheduler; no generated function calls
another instruction function. Each path therefore reaches `scd_p<k>_dispatch` again, and inside a group the selector:

| path | what names the next address | what selects the function |
| --- | --- | --- |
| fall-through, Bcc/BRA/DBcc, BSR/JSR absolute | constant successor PC from the CPU-owned lowering | dispatch: per-PC switch outside groups, selector inside |
| JSR/JMP `(An)` and other indirect forms | runtime EA; `scd_p<k>_entries` (union) must contain it, else `entry_not_compiled` | dispatch / selector |
| RTS (return guard) | popped address; union lookup, else `return_target_not_compiled` | dispatch / selector |
| RTE, RTR | `scd_rte`/`scd_rtr` restored PC (CPU-owned exception core) | dispatch / selector |
| exception vectors, sub interrupts | vector long from PRG RAM; `scd_compiled` (union), else `exception_vector_not_compiled` | dispatch / selector |
| reset (SRES 0 -> 1) | PRG `$000004` | dispatch / selector |

The union lookup only refuses addresses that no image compiled; it never chooses one. Instruction lowering is unchanged; only
the binding of compiled entries to dispatch changed.

## Consequences

- Checkpoint D holds on project-authored discs: module A loaded by CDC DMA and run at `$080000`, copied by CPU stores to
  `$020000` and run there, then B loaded over it and run at both addresses through B's own compiled entries; six negative loaders
  end in six distinct typed stops with no instruction retired by the wrong image.
- Mutable data inside a module's verified range invalidates it. A module whose image contains writable data must be listed with
  a range that covers only its code (a later list refinement); the synthetic fixtures do not need it.
- A sub-CPU code fetch from 2M Word RAM it does not own fails closed (no candidate is live, or `code_bytes_mismatch`), where the
  hardware would withhold DTACK; no fixture executes there.
- Open: whether title-observed modules need a key beyond `(instance, PC, live bytes)` (ADR 0127's NO-GO criterion) can only be
  answered with user-supplied discs; nothing in the synthetic evidence requires one.
