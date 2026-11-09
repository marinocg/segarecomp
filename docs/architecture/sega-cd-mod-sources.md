# Sega CD mod sources: disc identity, BIOS requirement, payload envelope and layout rules

Status: accepted contract (SEG-148-T001, ADR [0287](../decisions/0287-sega-cd-mod-sources.md)). **This document defines the Sega CD source-identity and disc-payload contract only; it does not implement the Sega CD adapter.** Reserved kinds (`disc-extent-edit`, `disc-track-replace`) and the `disc-image` source kind continue to fail closed (`E_PAYLOAD_KIND_RESERVED`, `E_SOURCE_KIND_RESERVED`) until the implementing task lands: verified extent edits are SEG-148-T002 are SEG-148-T002; the CD-DA mechanism is SEG-145-T005. It amends [schema 3](segapatch-schema-3.md) only through the discriminator
and the reserved payload kinds that schema 3 already defines. Inputs: `docs/architecture/sega-cd-machine-contract.md` sections 9a and 15, ADR 0129, ADR 0130, [adapters](mod-platform-adapters.md), [gap inventory](modding-platform-gap-inventory.md).

**Ownership (explicit).** SEG-148-T001 (this document) owns **disc identity, the layout-preservation rules and the versioned disc payload envelope**. SEG-145-T005 owns the **CD-DA replacement mechanism** and fills the reserved kind `disc-track-replace` within this contract.
Dependency direction: `SEG-148-T001 -> SEG-145-T005 -> SEG-145-T006`. SEG-148-T002 fills `disc-extent-edit`.

## 1. Why schema 1/2 cannot carry Sega CD, and how schema 3 does

| Schema 1/2 fact (source) | Consequence for a disc | Schema 3 answer |
|---|---|---|
| one `source_sha256` of one cartridge image (`libs/media/src/patch_package.cpp`) | a disc is several FILEs plus a track table; one digest cannot express the layout | `source.kind` `disc-image` with the disc identity ([schema 3](segapatch-schema-3.md) section 5) |
| `kMaxImageBytes` 16 MiB source and target (`libs/media/include/segarecomp/patch.hpp`) | track data is orders of magnitude larger | per-payload disc caps independent of the cartridge cap (section 5) |
| `kMaxPackageBytes` 32 MiB archive | cannot hold a disc payload | an independent disc archive cap (section 5) |
| single BPS `rom` payload producing one image | a disc patch edits extents inside files and tracks, preserving layout | discriminated payload envelope with disc kinds (section 3) |
| `platform` must be `genesis` | Sega CD is refused | `platform` `sega-cd` |
| Genesis-only native SDK (ABI 1) | no Sega CD feature table | typed `SegaCdFeatures` ([ABI 2](native-sdk-abi-2.md) section 3) |

## 2. Source identity and BIOS requirement

* **Disc identity** is the existing normalized-text SHA-256 (`segarecomp.disc.v1`: one line per FILE in CUE order with size, SHA-256 and name, one line per track with mode, file, offset, sector size, pregap, LBA, count and `index0`, and the total sector count; ADR 0129, machine contract section 9a).
  It is reused unchanged. Changing any byte of any file, any name or the layout changes it.
* **Source kind** `disc-image`: `{"kind":"disc-image","disc_identity":"<hex64>"}`. Platform must be `sega-cd`. Reserved in schema 3; resolved here.
* **BIOS requirement**: declared, never packaged: `"bios":{"requirement":"digest","sha256s":[...]}` or `{"requirement":"class","class":"<token>"}` where the class names a BIOS profile known to the runtime (ADR 0130). The build verifies the user-supplied BIOS satisfies the declaration (`E_DISC_BIOS` otherwise).
  A package that embeds a BIOS or any disc content is rejected.
* The existing save-state binding to the exact disc and BIOS identity (ADR 0279) continues to apply, with the **effective** disc identity (section 6).

## 3. Payload envelope for disc kinds

Every disc payload is a schema 3 envelope: `{"kind","kind_version","path","size","sha256", ...kind fields}` with `kind` in {`disc-extent-edit`, `disc-track-replace`} (both reserved in schema 3; parseable, rejected until their adapter exists, `E_PAYLOAD_KIND_RESERVED`).
`kind_version` is exact; an unknown kind or version is rejected, never best-effort parsed. A future validated disc-specific payload is added as a **new kind or a new kind_version**; old readers reject it by class, so no breaking schema revision is required.

**Minimum kind fields every disc kind MUST carry** (their remainder is owned by the kind's task):

| Field | Meaning |
|---|---|
| `target` | what is edited: a CUE FILE index (and optionally a track number inside it) |
| `start`, `length` | byte range inside the target's file, `length` equals the replacement size exactly |
| `original_sha256` | digest of the original bytes of that range (a digest only; no original content is stored) |
| `replacement` | how the new bytes are carried: `inline` (the payload entry holds exactly `length` mod-owned bytes) or `bps` (a BPS delta whose source is the original range and whose target size equals `length`) |

Worked example of a valid synthetic envelope (digests are placeholders): `{"kind":"disc-extent-edit","kind_version":1,"path":"disc/f1-0100.bin","size":4096,"sha256":"<hex64>","target":1,"start":65536,"length":4096,"original_sha256":"<hex64>","replacement":"inline"}`.
**Worked example of a rejection:** the same envelope with `"kind_version":2` is `E_PAYLOAD_VERSION_UNKNOWN`; with `"length":4097` and a 4096-byte payload it is `E_DISC_PAYLOAD_SIZE`; with `"kind":"disc-rewrite"` it is `E_PAYLOAD_KIND_UNKNOWN`.

## 4. Common rules every disc kind obeys

1. **Original-identity verification.** Before anything is applied the build computes the supplied disc's identity and requires it to equal the declared `disc_identity` (`E_DISC_IDENTITY`), and each range's original digest to equal `original_sha256` (`E_DISC_ORIGINAL_MISMATCH`).
2. **Layout preservation (objective, testable).** The effective disc MUST have, relative to the original: the same number of FILEs, in the same order, with the same names and **sizes** (`E_DISC_FILE_SIZE`); the same number of tracks (`E_DISC_TRACK_COUNT`), in the same order (`E_DISC_TRACK_ORDER`), with the same modes and sector sizes (`E_DISC_SECTOR_SIZE`),
   the same lengths in sectors (`E_DISC_TRACK_LENGTH`) and the same pregap and `INDEX 00` data (`E_DISC_PREGAP`); the same LBAs. Only the **bytes within declared ranges** differ. A change in any of these is **rejected, never rewritten**: there is no resizing, no re-layout, no reordering and no metadata rewriting.
3. **`INDEX 02` and later** are unsupported as today (`cue_unsupported`; `E_DISC_INDEX_UNSUPPORTED`).
4. **Ranges** lie inside the target file (`E_DISC_RANGE`) and no two payloads overlap (`E_DISC_OVERLAP`).
5. **Caps independent of cartridge limits** (section 5).
6. **No copyrighted content stored.** A payload carries only mod-owned bytes (for example a synthetic or author-owned recording) or a BPS delta over a digest-verified original range; the original range content is never embedded; product tests use synthetic discs only.
7. **Determinism.** The effective disc and its identity are a pure function of the original disc and the payloads.

## 5. Size model and caps

Disc payloads have caps independent of the 16 MiB cartridge image cap and the 32 MiB archive cap of schema 1/2: each disc payload at most 128 MiB, a package containing disc payloads at most 512 MiB in total, and the schema 3 entry limit applies unchanged (256 archive entries including `manifest.json`, so at most 255 payloads; see [schema 3](segapatch-schema-3.md) section 2). The size caps are contract maxima; SEG-148-T002 and SEG-145-T005 may lower them
with evidence. `size` is enforced while inflating as for all payloads. Large payloads are stored as `stored` ZIP entries (canonical form) of mod-owned bytes or BPS deltas; an archive over 32 MiB is legal only when every payload other than `manifest.json` is a disc kind.

## 6. Build identity and the generated program's disc check

Build-identity inputs for a patched Sega CD build: **original disc identity**, **effective disc identity** (the `segarecomp.disc.v1` text computed over the effective files), the BIOS identity, the **package identity** and **content digest** ([schema 3](segapatch-schema-3.md) section 8) and the schema 3 payload digests. They are appended to the existing build key; a Vanilla key is unchanged.

The generated Sega CD program already stores each FILE's size and SHA-256 and the track table and refuses a disc whose files do not match at start-up (machine contract section 9a). For a Patched build the program embeds the **effective** disc identity: it verifies the effective (patched) disc, never the original, and a Vanilla program keeps verifying the original.
The patched disc is a derived local artifact materialized inside the build attempt's own partial area from the user's original files (originals are only read, never modified) and is never redistributed or published. Save states bind to the effective disc identity plus the variant, so a state of a patched disc is not loadable on the original and vice versa (ADR 0279).
Module and overlay admission lists (ADR 0132) are keyed by the disc identity they name; a patched build admits modules against the effective identity (and may only do so for ranges declared by the patch; `byte_identity` verification of the resulting module bytes is unchanged).

## 7. Testable statements

* A synthetic disc with one `disc-extent-edit` of exactly `length` bytes yields an effective disc whose identity differs from the original and whose layout (files, tracks, LBAs, pregaps) is identical.
* Each of the nine layout rejections in section 4 has a negative vector (changed file size, added/removed/reordered track, changed sector size, changed track length, changed pregap, `INDEX 02`, out-of-range, overlap).
* An unknown `kind` or `kind_version` is rejected.
* The effective identity is reproduced bit-for-bit by two runs.

## 8. Not decided here

The CD-DA replacement mechanism (verified deltas versus mod-owned replacement assets versus a track-override manifest) is chosen by SEG-145-T005 within sections 3 to 5. Sub-M68K and Z80 patching of disc code is SEG-148-T013 (post-beta); main-M68K patching is SEG-148-T003.
