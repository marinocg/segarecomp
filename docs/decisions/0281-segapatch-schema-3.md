# ADR 0281: `.segapatch` schema 3

- Status: accepted (SEG-133-T002, combined delivery F01). Normative text: [segapatch-schema-3.md](../architecture/segapatch-schema-3.md).
- Related: ADR 0266 (schema 1), ADR 0267 (schema 2, native ABI 1), ADR 0280 (adapters), ADR 0282 (phases), ADR 0283 (ABI 2), ADR 0284 (Luau), ADR 0286 (audio), ADR 0287 (Sega CD sources).

## Context

Schema 1/2 carry one BPS image and one C11 provider for Genesis only, with caps (16 MiB image, one source, 8 entries) that cannot express multi-file mods, Luau, assets, other platforms or discs.

## Decision

1. Schema 3 is opt-in and additive. Schema 1/2 packages are read by the existing reader with unchanged semantics forever.
2. A closed, versioned, discriminated payload envelope (`kind`, `kind_version`) replaces the schema 2 `payloads` object; unknown kinds, versions, fields and sources fail closed.
3. Payload kinds: `bps-image`, `rom-edit-set`, `guest-code`, `native-c11`, `luau-source`, `asset`; reserved: `disc-extent-edit`, `disc-track-replace`, `guest-asm`. Reserved kinds parse and are rejected.
4. Trust class is the highest payload class (`data` < `luau` < `native`); any native source makes the package native.
5. `platform` is mandatory and exact; source identity is discriminated (`cartridge-image` with exact revision digests, `disc-image` reserved); a BIOS is declared by digest or class and never packaged.
6. Identity: package identity stays SHA-256 of the manifest bytes; a content digest over the payload listing is added; the Patched cache key appends package identity, content digest, revision digest, effective image digest and ABI major.
7. No build script, flag, include path, fetch URL or binary is representable.
8. Canonical packing form is specified for deterministic output. Audio assets and platform-scoped payloads are included so SEG-145 needs no schema revision.

## Consequences

SEG-135 implements the parser against the normative sections and vectors. Rejected alternatives: extending schema 2 in place (would change the meaning of existing packages); a manifest-level dependency solver (out of scope, M08 is minimal by design);
embedding BIOS or disc data (forbidden by content rules).
