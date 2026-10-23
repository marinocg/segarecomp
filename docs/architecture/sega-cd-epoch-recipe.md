# Sega CD epoch-image recipe (SEG-079, ADR 0178/0179)

- Status: **Research candidate, opt-in.** Decision and evidence: ADR 0179. Plan: ADR 0178.
- Scope: the *sub runnable-epoch image* only. Mutable `JMP` operand templates are **not** part of this schema (see "Template record").
- Privacy: this document contains no BIOS or disc byte, hash, offset or address. The example below is project-authored.

## What a recipe is

A recipe lets the emitter obtain the sub PRG RAM image that the main BIOS builds before it releases the sub processor, **without
running the BIOS**. It names the immutable BIOS image it applies to and describes the image as:

1. a zero-initialised image of `image_length` bytes (the BIOS clears PRG RAM before it builds anything; a recorded producer, not a copy);
2. then, in order, `steps`: each decodes one bounded Kosinski stream found at `source_offset` of the BIOS image into the image at `dest`.

There is no other operation. There is no scripting, conditional, loop, patch list or arithmetic. The only transformation is the public
Kosinski LZ decoder (a published format); it has three independent implementations in this repository's evidence: the BIOS's own
MC68000 routine (observed, never copied), `tools/sega_cd_epoch_recipe.py` and `platforms/sega-cd/machine/src/epoch_recipe.cpp`.

## Schema `segarecomp.sega_cd.epoch_recipe.v1` (JSON, canonical form = sorted keys, no whitespace)

| field | type | rule |
| --- | --- | --- |
| `schema` | string | exactly `segarecomp.sega_cd.epoch_recipe.v1` |
| `bios_sha256` | string | 64 lowercase hex; must equal the SHA-256 of the BIOS image the recipe is applied to |
| `bios_size` | integer | 1 .. 131072; must equal the BIOS image size |
| `image_length` | integer | 256 .. 524288, a multiple of 256 (the epoch granule); the last 256 bytes must hold a non-zero byte (the snapshot rule) |
| `steps` | array | 1 .. 8 objects, in application order |
| `steps[].decoder` | string | `kosinski` (the only decoder) |
| `steps[].source_offset` | integer | `< bios_size` |
| `steps[].dest` | integer | `< image_length` |

Rejected, fail closed, with a typed error: unknown or missing fields (top level or step), a different schema version, non-integer or
boolean numbers, negative numbers, a non-hex or upper-case SHA, a wrong BIOS identity or size, out-of-range offsets or lengths,
decoded output past `image_length`, overlapping decoded ranges, a stream that is truncated by the BIOS image, a back-reference before
the stream's first byte, an unterminated stream (token bound), an image whose last granule is all zero. The recipe never embeds
executable bytes or a reconstructed image, and contains no host path.

Emitter form: `segarecomp sega-cd emit ... --admit epoch-recipe:<image_length>:<source>@<dest>[,<source>@<dest>]*` (canonical decimal
only; the BIOS identity is the `--bios` image the program is bound to). `tools/sega_cd_epoch_recipe.py spec` prints it.

## Authority and what is still verified

`ImageAuthority::static_proof` ("static reasoning over immutable input, no guest execution", ADR 0076) is the smallest correct
authority: the image is a deterministic function of the immutable BIOS image and the recipe. The extension of ADR 0049's copy rule is
one producer evidence kind: *a declared decode of an immutable span* (`epoch.recipe`). The emitter derives the bytes itself; nothing
stored is trusted.

The generated program is unchanged in what it enforces: at the sub runnable epoch it compares the live PRG RAM with the reconstructed
image (length and SHA-256) and stops `materialized_image_unverified` before the sub retires an instruction when they differ
(ADR 0142 decision 4). Per-instruction byte guards (decision 5) are unchanged. A recipe is therefore a *claim about what the BIOS
produces*, checked by the CPU-owned execution path on every run; it cannot make unverified code run. No pass identity is fabricated:
the admission chain records `epochrecipe <canonical recipe>` (the BIOS identity is already part of the chain text), and a pass-observed
admission after a recipe still has to carry the true identity of the program before it.

## Producing a recipe

- `sega_cd_epoch_recipe.py scan --bios FILE`: a structural scan of the BIOS image (no execution, no lifting) for the call shape
  `LEA abs.l,A0 / LEA abs.l,A1 / BSR.W target`, keeping sites whose source decodes to a terminated stream and requiring one target.
  It is a *candidate generator*; the run-time epoch verification is the authority. Ambiguity fails closed.
- `sega_cd_epoch_recipe.py derive`: from a private writer log of one generated-program run (`--ram-write-log`); evidence tooling.

## Distributability gate (task section 6)

The recipe for a real BIOS is a list of three `(source_offset, dest)` scalars plus a length and the BIOS SHA-256. Against the five
questions:

1. *Reconstructable commercial content?* Not by itself: without the user's BIOS image it reconstructs nothing. With it, it reconstructs the
   sub BIOS program, which the user's own BIOS already contains in compressed form.
2. *Independently justified structural facts?* Yes: each step corresponds to one static call site in the BIOS and decodes to a terminated stream.
3. *Substantial commercial-derived address inventory?* Per BIOS: no (three records). Across the BIOS variants a user may hold: tens of
   records, and the **template** facts (below) would add table bases; the shape is an address inventory of firmware.
4. *Does `docs/testing/commercial-games.md` require an amendment?* **Yes.** Its only carve-out is `platforms/genesis/compat/<rom-sha256>.json`
   with the exact scalar `logical_table_descriptor` shape; it states it does not loosen the prohibition on raw addresses/offsets in
   committed evidence. A Sega CD record is neither Genesis nor that kind.
5. *Can the records be regenerated locally instead?* **Yes**, from the user's BIOS alone, by the scan above (27 of the 31 distinct
   128 KiB BIOS images in the authorized local pack yield a candidate; 4 fail closed; only the one BIOS used throughout this project is
   verified against an independent snapshot).

**Recommendation:** do not commit any real recipe and do not widen the policy. Keep recipes private and regenerate them locally
(`scan`), optionally storing them in the ignored build cache. A committed, schema-valid *synthetic* example is the whole public
surface (its BIOS image is the project-authored 8 KiB pattern `bytes((i*37+11)&0xFF for i in range(0x2000))`; `sega_cd_epoch_recipe_test.py` validates this block):

```json
{"bios_sha256":"1f5a16c4456f34c5459e4e66d8650d4dbb6b298810d0c21cef6571db21d69c81","bios_size":8192,"image_length":512,"schema":"segarecomp.sega_cd.epoch_recipe.v1","steps":[{"decoder":"kosinski","dest":0,"source_offset":4096}]}
```

## Template record (not a schema yet)

A `mutable_operand_template` fact would need: processor instance, a base and slot count of a table of 6-byte `JMP (xxx).L` slots, the
permitted shapes, the writer evidence, and "opcode verified at run time". ADR 0179 shows these tables are initialised by immutable or
epoch-reconstructed code, but no implemented analysis derives them without executing guest code. A record without a derivation is just a
list of previously observed addresses, which is exactly what the task says is not executable authority. **No template schema is
proposed until a derivation exists** (successor).
