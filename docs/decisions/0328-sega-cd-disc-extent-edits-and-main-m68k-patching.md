# ADR 0328: Sega CD disc extent edits and main-M68K guest patching

- Status: accepted (SEG-148-T002, SEG-148-T003, combined delivery F21).
- Implements: [ADR 0287](0287-sega-cd-mod-sources.md) and [sega-cd-mod-sources.md](../architecture/sega-cd-mod-sources.md). Related: ADR 0129, 0130, 0142, 0205, 0255, 0257, 0292.

## Decisions

### Disc edits (SEG-148-T002)

1. `source.kind` `disc-image` (with the `bios` requirement) and the payload kind `disc-extent-edit` (`kind_version` 1) are implemented. `disc-track-replace` stays reserved. On `sega-cd` the only other consumed payload is `guest-asm`; every other kind is `E_PAYLOAD_KIND_RESERVED`: a payload with no consumer is never ignored.
2. Envelope fields of kind version 1, beyond the ADR 0287 minimum: `iso_file` (optional), `code_cpu` and `load_address` (optional, together). `target` is the **0-based CUE FILE index**. Without `iso_file`, `start`/`length` are a byte range of that file. With `iso_file` (an absolute ISO9660 path of track 1, compared byte for byte, version suffix included) they are a range of that file's **logical** data; the record is resolved on the verified original disc, and on a raw (2352-byte) track the range is mapped sector by sector to user bytes only. `original_sha256` covers the range in the same address space.
3. Raw sectors: the sync pattern and header are layout and cannot be changed (`E_DISC_SECTOR_HEADER`). **EDC/ECC bytes are not recomputed**: the product neither reads nor checks them (machine contract section 13). A patched raw image is an input of this product's build, not a mastering output.
4. The plan is complete before any byte exists: disc identity, ranges, data tracks only (no audio, no `INDEX 00` data), no overlap, every original digest, every delta. Materialization streams each FILE, re-checks the original's digest while reading, copies the CUE byte for byte into a new folder (never the original's folder or inside it), reloads the result, checks the layout rules and only then publishes it. A failure leaves nothing.
5. The layout rules are one function over two loaded discs. `mod verify-disc` applies it to any patched image and also proves that nothing outside the declared bytes differs (`E_DISC_UNDECLARED_CHANGE`).
6. **Layout metadata is protected** (`E_DISC_METADATA`): no write may touch the boot header's system identifier or IP/SP descriptors, an ISO9660 volume descriptor, a path table or a directory extent, however the range is addressed. A disc whose ISO9660 volume announces itself but cannot be walked completely, or that carries a supplementary (for example Joliet) volume, refuses every edit: its metadata cannot be told apart from file data. This is a deliberate fail-closed limit.
7. Caps: the contract maxima are kept (128 MiB per disc payload, 512 MiB per package; an archive over 32 MiB only when every payload is a disc kind). A BPS replacement is limited to a 16 MiB range, because the delta is applied in memory.

### Main-M68K patching (SEG-148-T003)

8. **Eligibility comes from the product's own build of the original disc, never from the author.** `mod disc-build` runs `sega-cd build` on the original disc and reads its `disc_sources` evidence: the admitted extents that are exact copies of track-1 bytes (verified boot images, static loader proofs, resolved and relocated copies of delivered blocks), and separately the images that are not disc bytes (produced, statically evaluated, operand templates, epochs). Boot images that the same build contradicted are marked and never authorize a patch (they still count for refusals). Hand-admitted `--emit-arg` extents are never evidence, and `disc-build` does not accept the option. No new discovery heuristic exists.
9. A code patch is **declared**: a `disc-extent-edit` with `code_cpu` `main` and `load_address`, or an m68k `guest-asm` payload. In `guest-asm`, `org` and `free` offsets are **track-1 user-data offsets** (the offset space of the build's disc extents), the only revision is `*`, and `at <address>` is mandatory. The ADR 0292 static admission (forbidden instructions, writes only to work RAM, free ranges proven to hold their fill) is unchanged. A disc symbol has no revision and carries `cpu` (`main` | `sub` | `z80`) and `module` (`boot` = the 32 KiB system area, or an ISO9660 path).
10. Rules over every write that lands in track-1 user data, in this order:
   - overlapping a proven **sub-CPU** extent: refused, even when a main extent covers the same disc bytes;
   - a data edit overlapping a proven main extent: refused (it must be declared as code);
   - a code patch needs one proven main extent that holds all of its bytes **and** places its first byte at `load_address`.
11. The writes are judged twice: against the original disc's evidence before a patched disc exists, and against the Patched build's own evidence before the build is reported. A refusal keeps no patched disc and no Patched program.
12. `mod apply --cue` materializes **data** edits only and refuses a package that patches code (`E_DISC_CODE_EVIDENCE`). Without a build it cannot know which bytes are code and says so (`"code_checked":false`).
13. Rejection classes (exact strings):

| Class | Condition |
|---|---|
| `unsupported_code_class:sub_m68k` | declared sub code; a sub symbol in injected code; any write over a proven sub extent |
| `unsupported_code_class:z80` | declared Z80 code or Z80 `guest-asm`; a load or `at` address in the Z80 area; a Z80 symbol |
| `unsupported_code_class:dynamic` | the target address is reached only by an image whose bytes are not disc bytes |
| `unsupported_code_class:not_disc_proven` | no proven main extent holds the bytes at that address (RAM-only code, unexecuted or unrecognized bytes, a range that is not contiguous track-1 user data) |
| `unsupported_code_class:unrecognized_module` | a symbol's module context (including a module or overlay the evidence does not contain) is not proven |
| `unsupported_code_class:layout_changing` | a code patch or injection that would change a size, grow a file or leave track 1 |
| `unsupported_code_class:undeclared_code` | a data edit over proven main code |

14. A Patched build binds its variant: `sega-cd build --variant-identity <key>`, where the key covers the original and effective disc identities, the BIOS digest, the package identity, the content digest and the payload digests. It enters the program identity (so states are bound to the variant), and the program verifies the **effective** disc. A Vanilla build's identity is unchanged.
15. `mod disc-diff-run` runs the two programs of a build with the same explicit instruction budget and wall-clock timeout, each on its own disc, and reports the report fields that differ (the two identities are excluded: they differ by construction).

## Limits (not claimed)

- Eligibility reflects what one bounded build of the original proved. Code that build did not reach is `not_disc_proven`; raising the build budget is the only way to extend it. `--file-first` images are bound at run time and are not evidence.
- A proven extent is executable authority, **not** a claim that every byte in it is code. Data inside a proven main module is therefore patched with a main code declaration. Data inside a proven sub extent cannot be patched by `disc-build`.
- **A data edit is judged only against proven extents.** Bytes no extent covers are written as data even if they are code: sub-CPU code the bounded build did not reach, the encoded source of code that is later decoded or generated in RAM (the `dynamic` class applies only to a declared patch whose address such an image reaches), and **Z80 driver bytes**. The build evidence holds no Z80 extents, so Z80 data on disc cannot be identified: only declared Z80 patches (and Z80 symbols and addresses) are refused, and bytes inside a proven main extent can be declared as main code whatever the program later does with them. `mod apply` applies data edits without any code check.
- Sub-M68K and Z80 patching (SEG-148-T013), runtime code in Sega CD mods (SEG-148-T005/T006), CD-DA replacement (SEG-145-T005) and the launcher flow are not part of this decision. No comprehensive multi-CPU modification is claimed. Evidence is synthetic fixtures only.

## Evidence

`tests/mod_disc_edit_test.py` (cooked and raw fixture discs, independent disc and ISO9660 oracles, the patched disc through the existing emit/compile/run path, layout vectors), `tests/mod_disc_code_test.py` (project-authored BIOS and disc: Vanilla and Patched `sega-cd build`, differential run, one negative vector per class).
