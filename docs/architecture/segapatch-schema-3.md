# `.segapatch` schema 3 (normative)

Status: accepted contract (SEG-133-T002, ADR [0281](../decisions/0281-segapatch-schema-3.md)). **Reader, canonical packer and `mod.toml` mapping are implemented** (F02, ADR [0288](../decisions/0288-modding-platform-host-implementation.md); `libs/media/src/mod_package.cpp`); identity/cache use (SEG-135-T005), typed edits and symbols (SEG-136-T001/T002, ADR 0290), Luau, geometry and disc kinds are not. Inputs: the
[gap inventory](modding-platform-gap-inventory.md) (existing limits) and [adapter vocabulary](mod-platform-adapters.md). Consumers: SEG-135-T001/T002/T003, SEG-136-T001,
SEG-139/SEG-133-T004 (geometry declarations), SEG-145-T002 (audio assets), SEG-148-T001 (disc payloads), SEG-141 (identities).
Section numbers below are stable citation targets ("schema 3 section N").

Terms follow RFC 2119 (MUST, MUST NOT, MAY). Every MUST is testable: section 11 lists the vectors and the rejection table.

## 1. Scope and compatibility

1. Schema 1 and 2 packages stay readable **forever** with unchanged semantics (section 8). Schema 3 is **opt-in**: a package is schema 3 only if its manifest says `"schema_version": 3`.
2. A reader MUST NOT reinterpret a schema 1/2 manifest through schema 3 rules, and MUST NOT accept a schema 3 manifest through schema 1/2 rules.
3. Unknown manifest fields, unknown payload kinds, unknown `kind_version`s, unknown capability names and unknown source kinds fail closed (section 9).
4. A schema 3 manifest MUST NOT be able to express a build script, compiler flag, linker flag, include path, fetch URL, prebuilt binary, environment variable or shell command.
   No field of that nature exists, and every string field is restricted to the character classes of section 3.
5. The platform is mandatory and exact (`genesis`, `master-system`, `sega-cd`). There is no platform guessing from an image.

## 2. Archive form

A package is a classic ZIP (no encryption, no ZIP64, no links, no directories, no trailing data, no overlapping entries) containing `manifest.json` plus exactly the entries the manifest names.

* **Entries.** `manifest.json` at the root; every other entry is named by a `payloads[].path`. An entry the manifest does not name, or a named entry that is absent, is `E_ARCHIVE_ENTRY`.
* **Paths.** Relative, `/`-separated, ASCII `[a-z0-9._-]` segments, no empty, `.` or `..` segment, no leading dot in a segment, at most 96 bytes, at most 3 segments. Case-insensitive duplicates are refused (case-folding collisions on Windows and macOS).
* **Canonical packing form** (what the pack tool MUST emit; the reader accepts any valid ZIP layout, because identity does not depend on layout): entries ordered by path bytes ascending with `manifest.json` first;
  every entry uses method `stored` (so output is independent of any compressor); DOS time `00:00:00`, date `1980-01-01`; no extra
  fields, no comment; "version made by" 20, "version needed" 20, external attributes 0, general-purpose flags 0.
* **Canonical manifest bytes:** UTF-8/ASCII JSON, keys in the order of section 3, no insignificant whitespace, no trailing newline. The reader hashes the exact bytes it received.
* **Limits.** Archive <= 32 MiB; manifest <= 64 KiB; entries <= 256 including `manifest.json` (hence at most 255 payload entries; this is the one effective entry limit for every package, disc kinds included); a `cartridge-image` package keeps the existing caps (`bps-image` payload <= 16 MiB, source/target image <= 16 MiB);
  per-kind caps in section 4. Payloads whose kind is a disc kind have per-payload caps independent of the cartridge and archive caps (section 7).
  Sizes are enforced while inflating; a declared size that differs from the actual size is `E_ARCHIVE_SIZE`.

## 3. Manifest

Value set (closed JSON subset): objects, arrays, ASCII strings, non-negative decimal integers up to 2147483647. No booleans, no null, no floating point, no escapes other than `\"` and `\\`, no duplicate keys.
Field order below is the canonical order.

| Field | Type | Required | Rule |
|---|---|---|---|
| `format` | string | yes | exactly `segarecomp-patch` |
| `schema_version` | integer | yes | exactly `3` |
| `id` | string | yes | `[a-z0-9]+(-[a-z0-9]+)*`, at most 64 bytes (package id) |
| `version` | string | yes | `MAJOR.MINOR.PATCH`, each decimal, no leading zeros, at most 9 digits |
| `name` | string | yes | printable ASCII, 1 to 64 bytes |
| `platform` | string | yes | `genesis` \| `master-system` \| `sega-cd` |
| `author` | string | yes if a `native-c11` payload exists, else optional | printable ASCII, at most 64 bytes |
| `license` | string | optional | token `[A-Za-z0-9.+-]`, at most 32 bytes |
| `purpose` | string | optional | token; informational |
| `source` | object | yes | section 5 |
| `bios` | object | required for `disc-image`, forbidden for `cartridge-image` | section 5 |
| `capabilities` | array of strings | optional (default empty) | capability names (section 6); at most 32; sorted ascending, no duplicates |
| `capabilities_optional` | array of strings | optional (default empty) | capability names the package can run without; same rules as `capabilities`; disjoint from it; an optional name that is known but not supported on the platform is simply not granted, an unknown name is `E_CAPABILITY_UNKNOWN` |
| `abi` | object | required iff a `native-c11` payload exists, else forbidden | `{"native":2}` (the ABI major version; ABI 2 negotiation: [native-sdk-abi-2.md](native-sdk-abi-2.md)) |
| `state_schema` | integer | required iff `state.mod` is declared, else forbidden | 1 to 2147483647; the version of the mod's serialized state ([phases](mod-execution-phases.md) section 6.2) |
| `hooks` | array | optional; requires capability `hook.function` | **reserved key**: the content model (site, class, mode, replacement contract) is defined by SEG-138-T004 consistently with [phases](mod-execution-phases.md) section 5; until then any element is `E_FIELD_UNKNOWN` |
| `mappings` | array | optional | **reserved key**: audio declared mappings, content model defined by SEG-145-T002 within [modding-audio-architecture.md](modding-audio-architecture.md) section 5; until then any element is `E_FIELD_UNKNOWN` |
| `symbols` | array of objects | optional | per-revision guest symbols with address provenance (ADR [0290](../decisions/0290-typed-rom-edits-and-revision-symbols.md)): `{"name","revision","address","provenance"}`; at most 255, strictly sorted by (name, revision); `name` a token of at most 64 characters, `revision` declared, `address` an image offset below 16 MiB, `provenance` `manual` \| `verified` \| `imported-hint`; only `verified` entries may be an edit site |
| `header_repair` | string | optional | explicit header-repair declaration (ADR [0293](../decisions/0293-guest-injection-collisions-header-repair-bps.md)): `genesis-checksum` (strict: rewrite the Genesis ROM-end field and checksum; fail closed on an unsupported header) or `none` (opt-out). Absent: the checksum is repaired only when the original image verifies it and the build changed the image. `genesis-checksum` on another platform is `E_HEADER_UNSUPPORTED` |
| `limits` | object | optional | requested resource limits; keys `luau_memory_bytes`, `luau_budget_units` (integers); each MUST be <= the policy default of [luau-trust-policy.md](luau-trust-policy.md) section 4 (a package may request less, never more: `E_LIMIT_EXCEEDS_POLICY`); other keys `E_FIELD_UNKNOWN` |
| `geometry` | object | optional | geometry declaration: `{"scenes":[...]}`; the content model is SEG-139-T004's; schema 3 only reserves the key and requires `E_FIELD_UNKNOWN` for any other key inside until then |
| `presentation_defaults` | object | optional | as schema 1/2 (`widescreen`, `side_style`) |
| `payloads` | array of objects | yes | section 4; 1 to 255 entries (the archive limit of 256 entries includes `manifest.json`) |

## 4. Payload envelope and kinds

Every payload is an object `{ "kind": <string>, "kind_version": <integer>, "path": <string>, "size": <integer>, "sha256": <hex64>, ...kind fields }`. `kind` and `kind_version` are both exact.
A package MAY contain any set of payloads that is legal for its platform, subject to the combination rules below.

| `kind` (v) | Platforms | Meaning | Trust class | Kind fields | Caps |
|---|---|---|---|---|---|
| `bps-image` (1) | genesis, master-system | standard BPS1 delta producing a patched cartridge image | data | `target_sha256` | <= 16 MiB; image <= 16 MiB |
| `rom-edit-set` (1) | genesis, master-system | typed ROM edits with expected-value assertions: ASCII text, one edit per line `<revision\|*> <type> <site> <expected> <value>` (ADR [0290](../decisions/0290-typed-rom-edits-and-revision-symbols.md)); checked by the reader | data | `edits` count (informational, verified) | <= 1 MiB; <= 4096 edits |
| `guest-code` (1) | genesis, master-system | pre-assembled guest machine code with placement and expected original bytes (assembly source is a different kind, `guest-asm`, ADR 0292) | data | `cpu` (`m68k` \| `z80`) | <= 1 MiB total |
| `native-c11` (1) | genesis (ABI 2), master-system and sega-cd (typed feature tables, SEG-146-T003) | portable C11 source files compiled by the product into the Patched program | **native** | `role` (`provider`) | <= 64 files, each <= 256 KiB, total <= 2 MiB |
| `luau-source` (1) | genesis, master-system, sega-cd | Luau source; bytecode is produced only by the pinned trusted compiler at build (Luau policy) | luau | `role` (`script`) | <= 64 files, each <= 128 KiB, total <= 1 MiB |
| `asset` (1) | all | declared binary asset | data | `asset_class` and class fields (below) | <= 128 assets, each <= 8 MiB |
| `disc-extent-edit` (reserved) | sega-cd | layout-preserving file/extent edit | data | defined by SEG-148-T001 | independent disc cap (section 7) |
| `disc-track-replace` (reserved) | sega-cd | track-content replacement | data | defined by SEG-145-T005 within SEG-148-T001 | independent disc cap |
| `guest-asm` (1) | genesis, master-system | guest assembly source text, assembled at build time by the in-tree bounded assembler (ADR 0292); at most one per package | data | `cpu` (`m68k` \| `z80`) | <= 1 MiB total with `guest-code` |

A reserved kind is **parseable** (its envelope is validated) and **rejected** with `E_PAYLOAD_KIND_RESERVED` until its adapter exists. A kind that is not in this table is `E_PAYLOAD_KIND_UNKNOWN`. A known kind with an unlisted
`kind_version` is `E_PAYLOAD_VERSION_UNKNOWN`. Platform-scoped means: a kind used on a platform not listed for it is `E_PAYLOAD_PLATFORM`. Examples: `guest-code` with `cpu` `z80` is legal on `genesis` and `master-system` only
for the CPUs those machines have; `m68k` guest code on `master-system` is `E_PAYLOAD_PLATFORM`. Per-CPU Sega CD payloads are reserved kinds (above), never `guest-code`.

**Combination rules.** (a) At most one `bps-image` and at most one `rom-edit-set` (`E_PAYLOAD_COMBINATION`; one edit file carries the edits of every revision, so overlap, assertions and the effective digest are judged over one set, ADR 0290); schema 3 v1 allows one image-producing mechanism per package: `bps-image` cannot coexist with `rom-edit-set`, `guest-code` or `guest-asm` (`E_PAYLOAD_COMBINATION`); at most one `guest-asm` per package; `rom-edit-set` and `guest-asm` may coexist (edits, then injected code, then header repair; ADR 0293). (b) `native-c11` requires `abi` and `author`; `abi` without `native-c11` is `E_FIELD_UNKNOWN`. (c) Duplicate `path` is `E_PAYLOAD_DUPLICATE`. (d) Every `capabilities` entry MUST be consistent with the payloads
(for example `memory.write.*` requires `native-c11` or `luau-source`); an inconsistent declaration is `E_CAPABILITY_INCONSISTENT`.

**Asset classes** (`asset_class`): `image` (fields: `width`, `height`, `format` `rgba8`), `data` (opaque; `format` token), `audio` (fields below). Assets are declared and size/digest checked; the product never decodes game formats.
`audio` assets: `audio_format` `wav-pcm-s16le`, `sample_rate` integer (8000..48000), `channels` 1 or 2, `frames` integer, `loop_start`/`loop_end` optional integers within `frames`. The mapping that makes an audio asset
audible (the *declared mapping*) is declared in a `mappings` array of the manifest of SEG-145-T002 and consumes only the capability names in the audio contract ([modding-audio-architecture.md](modding-audio-architecture.md) section 5). Schema 3
therefore needs **no revision** for SEG-145: the audio class, its fields and the platform-scoped payload table exist here. No audio asset may embed commercial audio (section 10).

**Trust class of a package** = the highest class among its payloads: `data` < `luau` < `native`. Names used in user-facing text: **BPS-only** (data), **Luau-only**, **native**. **Any `native-c11` payload makes the package native**,
including a mixed package (native + Luau + data). Details: [luau-trust-policy.md](luau-trust-policy.md) section 1.

## 5. Source identity and BIOS

`source` is a discriminated object:

* `{"kind":"cartridge-image","revisions":[{"label":<token>,"sha256":<hex64>[,"mapper":<token>]}, ...]}` for `genesis` and `master-system`. 1 to 16 revisions; each `sha256` is the digest of one exact
  original image; labels are unique tokens; `mapper` is required for `master-system` (`sega` \| `rom_only`, as declared, never inferred, `master-system-machine-contract.md` section 4.1) and forbidden for `genesis`.
  Exact-digest verification is retained: the build requires the supplied image's SHA-256 to equal exactly one declared revision; a title, region or header is never consulted.
* `{"kind":"disc-image","disc_identity":<hex64>}` for `sega-cd`: **reserved in schema 3**. The disc identity is the SHA-256 normalized text of `segarecomp.disc.v1` (`sega-cd-machine-contract.md` section 9a). The
  kind is parsed and rejected with `E_SOURCE_KIND_RESERVED` until the Sega CD adapter exists (SEG-148-T001 resolves it: [sega-cd-mod-sources.md](sega-cd-mod-sources.md)).
* `bios` (disc platforms only): `{"requirement":"digest"|"class","sha256s":[...]}` or `{"requirement":"class","class":<token>}`. The BIOS is **declared by digest or class, never packaged**; a package that carries a BIOS
  payload is rejected (`E_PAYLOAD_KIND_UNKNOWN`: no BIOS kind exists).

A platform/source mismatch (`genesis` with `disc-image`, `sega-cd` with `cartridge-image`) is `E_SOURCE_PLATFORM`.

## 6. Capability declaration (consumed by ABI 2 and geometry)

`capabilities` and `capabilities_optional` are optional fields; **an absent field means the empty set**, and the two forms are equivalent. They are sets of names from the [adapter vocabulary](mod-platform-adapters.md) section 2 that the package requires. The product grants exactly the declared set for the platform, and refuses the package at
`mod check` if any name is unknown (`E_CAPABILITY_UNKNOWN`) or unsupported on the platform (`E_CAPABILITY_UNSUPPORTED`). Schema 3 stores **names only**; legality by phase and trust implications are in
[native-sdk-abi-2.md](native-sdk-abi-2.md) section 4. The schema 1/2 `requires` array is not valid in schema 3 (`E_FIELD_UNKNOWN`); schema 2's native capabilities map 1:1 to schema 3 names (section 8).
Geometry declarations live in the reserved `geometry` object; SEG-139-T004 defines their content, which MUST be additive to this schema (a reader of this section rejects unknown keys inside it until then).

## 7. Disc payload rules (reserved kinds)

Disc payload kinds MUST obey the common rules of [sega-cd-mod-sources.md](sega-cd-mod-sources.md) section 4 and have **per-payload caps independent** of the 16 MiB cartridge image cap and the 32 MiB archive cap.
Schema 3 fixes only the envelope: `kind`, `kind_version`, `path`, `size`, `sha256` are mandatory and validated; the remaining fields are kind-owned. An unknown `kind_version` of a disc kind is
`E_PAYLOAD_VERSION_UNKNOWN`, never a best-effort parse.

## 8. Identity

* **Package id** and **version**: the manifest fields.
* **Package identity** = SHA-256 of the exact manifest bytes (unchanged definition; the manifest names every payload digest, so the identity covers all inputs).
* **Content digest** = SHA-256 of the UTF-8 text
  ```text
  segarecomp.mod.content.v1\n
  platform <platform>\n
  payload <kind> <kind_version> <path> size <size> sha256 <hex>\n      (one per payload, ordered by path bytes ascending)
  ```
  It is independent of manifest metadata (name, author, license, geometry), so two packages with the same payloads and platform share a content digest.
* **Target revision digests**: `source.revisions[].sha256`.
* **Relation to the Vanilla/Patched build identity** (ADR 0266 decision 5): the library game identity, the effective compiled image digest (`SEGARECOMP_ROM_SHA256`) and the selected variant (package identity)
  keep their meanings. For schema 3 the Patched cache key appends: package identity, content digest, the selected revision digest, the effective image digest and the native ABI major (`abi.native`, or `0`);
  a Vanilla key is unchanged. A change of any of them cannot share a cache entry.
* **Mod identity in a set** (SEG-141) is the ordered list of package identities; defined there.

## 9. Rejection table (stable diagnostic classes)

| Class | Condition |
|---|---|
| `E_ARCHIVE_ENTRY`, `E_ARCHIVE_SIZE`, `E_ARCHIVE_PATH` | structure, declared size, path rules (section 2) |
| `E_MANIFEST_SYNTAX` | not the closed JSON subset, duplicate key, bad escape, over 64 KiB |
| `E_SCHEMA_VERSION` | `schema_version` is not 3 (when read as schema 3) |
| `E_FIELD_UNKNOWN`, `E_FIELD_MISSING`, `E_FIELD_VALUE` | unknown/missing/malformed field |
| `E_PLATFORM_UNKNOWN` | platform string not one of the three |
| `E_SOURCE_PLATFORM`, `E_SOURCE_KIND_RESERVED`, `E_SOURCE_DIGEST` | section 5 |
| `E_PAYLOAD_KIND_UNKNOWN`, `E_PAYLOAD_KIND_RESERVED`, `E_PAYLOAD_VERSION_UNKNOWN`, `E_PAYLOAD_PLATFORM`, `E_PAYLOAD_COMBINATION`, `E_PAYLOAD_DUPLICATE`, `E_PAYLOAD_DIGEST`, `E_PAYLOAD_CAP` | section 4 |
| `E_CAPABILITY_UNKNOWN`, `E_CAPABILITY_UNSUPPORTED`, `E_CAPABILITY_INCONSISTENT` | section 6 |
| `E_EDIT_SYNTAX`, `E_EDIT_REVISION`, `E_EDIT_COUNT`, `E_EDIT_OVERLAP`, `E_EDIT_RANGE`, `E_EDIT_EXPECTED_MISMATCH` | a `rom-edit-set` payload (ADR 0290); the last two are raised when edits are applied to an image |
| `E_SYMBOL_UNKNOWN`, `E_SYMBOL_NOT_VERIFIED`, `E_SYMBOL_DUPLICATE`, `E_SYMBOL_REVISION` | the `symbols` key and symbol edit sites (ADR 0290) |
| `E_GUEST_SYNTAX`, `E_GUEST_REVISION`, `E_GUEST_OPERAND`, `E_GUEST_LABEL_UNKNOWN`, `E_GUEST_LABEL_DUPLICATE`, `E_GUEST_VALUE_RANGE`, `E_GUEST_ALIGN`, `E_GUEST_ADDRESS` | `guest-asm` source and placement (ADR 0292) |
| `E_GUEST_BRANCH_RANGE`, `E_GUEST_BRANCH_TARGET`, `E_GUEST_FORBIDDEN_INSTRUCTION`, `E_GUEST_WRITE_OUTSIDE_RAM` | static validation of the emitted code (ADR 0292) |
| `E_GUEST_OUTSIDE_FREE_RANGE`, `E_GUEST_COLLISION`, `E_GUEST_RANGE`, `E_GUEST_RANGE_NOT_FREE`, `E_MAPPER_UNSUPPORTED`, `E_HEADER_UNSUPPORTED` | free ranges, collisions, image-dependent range checks, mappers and header repair (ADR 0293) |
| `E_BPS_BUILD`, `E_BPS_INVALID`, `E_BPS_STALE`, `E_HEADER_FIELD_COLLISION`, `E_OUTPUT_IS_INPUT` | `mod bps`, `mod verify-bps` (ADR 0293) |
| `E_TRUST_UNDECLARED` | a `native-c11` payload without `author`/`abi` |
| `E_LIMIT_EXCEEDS_POLICY`, `E_LUAU_BYTECODE` | requested limit above policy; a `luau-source` payload that is not Luau text (section 4, Luau policy section 2) |

Every ambiguity is a rejection: an alternate spelling, case variation, duplicate or unknown key, extra archive entry, or unsorted `capabilities`.

## 10. Content rules

No commercial ROM, disc, BIOS, firmware, extracted asset or audio may be placed in a package that is committed to a product or public repository; packages of exact-digest deltas stay in the user's local store.
Synthetic fixtures only in product tests.

## 11. Schema 1/2 field mapping and worked examples

Behavior under a schema 3-capable reader: **unchanged** (a schema 1/2 package is parsed by the schema 1/2 reader exactly as today), **read-only** (the field is kept for display), **superseded** (schema 3 uses a different field when the package is authored as schema 3).

| Schema 1/2 field | Schema 3 behavior |
|---|---|
| `format` `segarecomp-patch` | unchanged (same value) |
| `schema_version` 1 / 2 | unchanged: routes to the schema 1/2 reader; value 3 routes to schema 3 |
| `id`, `version`, `name` | unchanged rules |
| `platform` (`genesis` only) | unchanged in 1/2; schema 3 adds `master-system`, `sega-cd` |
| `purpose` (`widescreen` only) | schema 1/2: unchanged and still required; schema 3: optional informational token |
| `source_sha256` | superseded by `source.revisions[]` in schema 3 (single digest = one revision labelled by the author); unchanged in 1/2 |
| `requires` (always empty) | unchanged in 1/2 (non-empty refused); not valid in schema 3 (`capabilities` replaces it) |
| `widescreen_width_max` | read-only in 1/2; **not valid** in schema 3 - geometry declarations (SEG-139-T004) supersede it |
| `presentation_defaults` | unchanged (valid in both) |
| `author`, `license` (schema 2) | unchanged |
| `payloads.rom` (`format: bps`, `file`, `sha256`, `target_sha256`) | unchanged in 1/2; schema 3: `bps-image` v1 payload with `path`, `size`, `sha256`, `target_sha256` |
| `payloads.native` (`format c11-source`, `file provider.c`, `sha256`, `abi` 1, `capabilities`, `executable yes`) | unchanged in 1/2 (ABI 1 provider, one source); schema 3: `native-c11` v1 payload(s) + manifest `abi` (`native`: 2) + manifest `capabilities`; the `executable: yes` declaration is implied by the kind (trust class native) |

Capability name mapping (schema 2 `native.capabilities` to schema 3 `capabilities`): `frame.work_ram`, `frame.vdp`, `scenery.words`, `overlay.rgba` keep their names.
A package that needs ABI 1 semantics stays schema 2; a schema 3 package declares ABI 2 ([ABI 2 coexistence](native-sdk-abi-2.md) section 6).

### Worked example, schema 1 (BPS only)

The documented schema 1 manifest (`docs/product/patch-packages.md`, `"schema_version": 1`, `payloads.rom`) continues to parse to the same `PackageInfo` and the same identity (SHA-256 of its manifest bytes).
Authored as schema 3 the same patch becomes `{"format":"segarecomp-patch","schema_version":3,"id":"x","version":"1.0.0","name":"X","platform":"genesis","source":{"kind":"cartridge-image","revisions":[{"label":"rev-a","sha256":"<source_sha256>"}]},"payloads":[{"kind":"bps-image","kind_version":1,"path":"game.bps","size":<n>,"sha256":"<sha>","target_sha256":"<target>"}]}`.

### Worked example, schema 2 (native provider)

The schema 2 `payloads.native` object (`file` `provider.c`, `abi` 1, four capabilities, `executable` `yes`) continues to build with ABI 1. Authored as schema 3 with ABI 2:
`"abi":{"native":2},"capabilities":["frame.vdp","frame.work_ram","overlay.rgba","scenery.words"],"author":"A","payloads":[{"kind":"native-c11","kind_version":1,"path":"src/provider.c","size":<n>,"sha256":"<sha>","role":"provider"}]`.

## 12. Test vectors

All digests are SHA-256 of the stated bytes; reproduce with any SHA-256 tool.

| Input | SHA-256 |
|---|---|
| ASCII `synthetic-source-image-A` (declared source image) | `a86d60429e75e5f792f5dcc5436ade92d21d19f9979a5114b11e437a13b7d1dd` |
| ASCII `-- hello` (8 bytes, `scripts/main.luau`) | `a49bb5fe7c01bb8575e59200abef7b4c873292154b8434616942764a0d76763c` |
| ASCII `int x;` + LF (7 bytes, `src/main.c`) | `7c725f30854a46033dd94f728ac6b08caf10845993cd3ed48e40079cdb0a76a6` |

**Vector A, content digest.** Text (each line ends with LF):

```text
segarecomp.mod.content.v1
platform genesis
payload luau-source 1 scripts/main.luau size 8 sha256 a49bb5fe7c01bb8575e59200abef7b4c873292154b8434616942764a0d76763c
payload native-c11 1 src/main.c size 7 sha256 7c725f30854a46033dd94f728ac6b08caf10845993cd3ed48e40079cdb0a76a6
```

SHA-256 = `15668889b22e0bcc650c597871b0819dc5a35ac3cabbe063acaf43dc0e03f0ef`. The same text with `platform master-system` = `25fdf0085aa085f112923452bb836f1a7c600c71cc3628a6d1a4cfb614c51d6f` (platform changes the digest).

**Vector B, package identity.** The manifest bytes (one line, no trailing newline):

```text
{"format":"segarecomp-patch","schema_version":3,"id":"example-mod","version":"1.0.0","name":"Example","platform":"genesis","source":{"kind":"cartridge-image","revisions":[{"label":"rev-a","sha256":"a86d60429e75e5f792f5dcc5436ade92d21d19f9979a5114b11e437a13b7d1dd"}]},"capabilities":["host.log"],"payloads":[{"kind":"luau-source","kind_version":1,"path":"scripts/main.luau","size":8,"sha256":"a49bb5fe7c01bb8575e59200abef7b4c873292154b8434616942764a0d76763c"},{"kind":"native-c11","kind_version":1,"path":"src/main.c","size":7,"sha256":"7c725f30854a46033dd94f728ac6b08caf10845993cd3ed48e40079cdb0a76a6"}]}
```

Package identity = `1c2ad1354fe66a47ee00336af9b82375859025b8552c22187e24bcf1e24aec30`. (This vector is a *syntactic* identity vector; as a package it is rejected `E_TRUST_UNDECLARED` because it contains a `native-c11` payload without `author` and `abi`, which is also a required negative vector.)

**Negative vectors (each MUST be rejected with the named class):** a duplicate key; `"schema_version":3` with `requires`; `"platform":"saturn"`; `source.kind` `disc-image` on `genesis`; `source.kind` `disc-image` on `sega-cd` (`E_SOURCE_KIND_RESERVED`);
payload `kind` `disc-extent-edit` (`E_PAYLOAD_KIND_RESERVED`); payload `kind_version` 2 of `native-c11` (`E_PAYLOAD_VERSION_UNKNOWN`); `bps-image` plus `guest-code` (`E_PAYLOAD_COMBINATION`);
a payload path with `..` (`E_ARCHIVE_PATH`); an unsorted `capabilities` array; an archive entry not named in `payloads`; a BIOS payload (`E_PAYLOAD_KIND_UNKNOWN`); `native-c11` with `abi.native` 1.

**Regression vectors for schema 1/2.** Every schema 1/2 fixture in `tests/segapatch_package_test.py` and `tests/segapatch_native_package_test.py` MUST keep passing unchanged after schema 3 lands. A schema-1/2-only reader meets a
schema 3 manifest and fails with its existing `unsupported patch package schema version 3`; it meets an SMS or Sega CD schema 3 platform and (independently) refuses `the manifest platform is not supported`; therefore a Genesis-only
reader never mistakes SMS or Sega CD packages for Genesis (both behaviors exist today in `libs/media/src/patch_package.cpp`).

## 13. Named later consumers

SEG-135-T001 (parser), SEG-135-T002 (`mod.toml` model, revisions), SEG-135-T003 (multi-file sources and assets), SEG-135-T005 (identity/cache), SEG-136-T001/T002 (edits, symbols),
SEG-139-T004 (geometry key), SEG-141-T001 (set identity), SEG-145-T002 (audio assets), SEG-146-T002 (adapter plumbing), SEG-148-T001 (disc kinds).

## 14. Implementation notes (F02, ADR 0288)

* **Entry points.** `segarecomp::mod::read_package` (reader), `pack_package` (canonical packer), `serialize_manifest`, `content_digest` in `libs/media/include/segarecomp/mod.hpp`; the CLI is `segarecomp mod inspect <package>`. Schema 1/2 packages stay with `segarecomp::patch::read_package`.
* **Strictness fixed by the implementation** (these resolve latitude in the text above): the reader refuses insignificant whitespace in the manifest; key order is not enforced (the packer always emits the canonical order, and identity is the hash of the exact bytes received); the payload `role` field is required; a `native-c11` payload without `author`/`abi` is `E_TRUST_UNDECLARED` before any other payload field is judged; archive and payload paths are lower-case, so a case-folded duplicate can only come from a name the path rule already refuses.
* **Zip-bomb ratio.** A deflated entry may expand to at most 256 times its compressed size plus 4 KiB (`E_ARCHIVE_SIZE`); the uncompressed total is capped at 48 MiB and each entry at 16 MiB before the per-kind caps of section 4.
* **Native source set.** `native-c11` payloads are `.c` and `.h` files (a package needs at least one `.c`); Luau payloads end in `.luau`. Include policy and host compile: ADR 0288 decision 8.
* **Audio mappings (SEG-145-T002).** The reserved `mappings` array now has its content model: see [modding-audio-architecture.md](modding-audio-architecture.md) section 5. A package with an `audio` asset must carry `license`. The content model of `hooks` and `geometry` is still reserved (empty only).
* **Added classes** beyond section 9: `E_INCLUDE_FORBIDDEN`, `E_COMPILE_FAILED`, `E_LINK_FAILED`, and the adapter/space classes `E_SPACE_*`, `E_ACCESS_*` for audio mailboxes.
