# ADR 0287: Sega CD mod sources

- Status: accepted (SEG-148-T001, combined delivery F01). Contract: [sega-cd-mod-sources.md](../architecture/sega-cd-mod-sources.md).
- Related: ADR 0129 (disc identity), ADR 0130 (BIOS profiles), ADR 0279 (Sega CD states), ADR 0281 (schema 3).

## Decision

1. A Sega CD package is a schema 3 package with `platform` `sega-cd`, `source.kind` `disc-image` carrying the existing `segarecomp.disc.v1` identity, and a BIOS requirement declared by digest or class, never packaged.
2. Disc payloads use the schema 3 discriminated, versioned envelope with the reserved kinds `disc-extent-edit` and `disc-track-replace`; unknown kinds and versions are rejected.
3. Edits are layout-preserving only: file count/order/sizes, track count/order/modes/sector sizes/lengths and pregap/`INDEX 00` data are unchanged; any other change is rejected, never rewritten; `INDEX 02` and later stay unsupported.
4. Disc payloads have caps independent of the cartridge (16 MiB) and archive (32 MiB) caps; no copyrighted content is stored (mod-owned bytes or BPS deltas over digest-verified ranges).
5. Patched builds embed the effective disc identity in the generated disc check; save states bind to it.
6. Ownership: SEG-148-T001 owns identity, layout rules and the envelope; SEG-145-T005 owns the CD-DA mechanism: `148-T001 -> 145-T005 -> 145-T006`.

## Consequences

SEG-148-T002 implements verified extent edits; SEG-145-T005 fills the track kind. Rejected alternatives: carrying a disc in the cartridge model (16 MiB cap, one image); re-layout or resizing of tracks (unverifiable identity); embedding BIOS or disc content.
