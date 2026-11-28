# ADR 0326: Ordered mod sets, verified guest-edit composition, composite identities and declared mod relations

- Status: accepted (combined delivery F19: SEG-141-T001, SEG-141-T002, SEG-141-T004).
- Builds on: ADR 0280 (adapters), 0281/0288 (schema 3), 0289 (single-mod variant identity), 0290 (typed edits, verified symbols), 0293 (injection, header repair, BPS), 0298 (hooks), 0324 (Master System admission).

## Engine

1. One engine (`libs/media`, `segarecomp/mod_set.hpp`), pure functions over verified packages. What differs per platform comes from the adapter table and the conflict claims' address spaces. There is no second engine per platform and no dependency solver.

## Composite identity and persistence (SEG-141-T001)

2. A **definition** is versioned canonical text: `segarecomp-modset=1`, `platform=<p>`, then 1 to 16 `member=<package identity>` lines in set order. Another format number is `E_SET_VERSION`; anything else is refused. The CLI stores it at `<library>/mod-sets/<name>.modset`; the launcher library at `<library>/mod-sets/<game sha256>/<name>.modset`. It names exact package identities only.
3. A **one-member set is that mod**: `variant_identity` of ADR 0289, component for component, and the same cache entry.
4. A larger set's components are, in order: `set_format`, `platform`, `source_kind`, `source_identity` (cartridge: image digest and declared mapper; disc: disc identity and BIOS requirement), `adapter_capabilities` (digest of the platform adapter's whole capability/support table), `membership` (order independent), `effective_image` (the composed image), then `member.<i>` = `<package identity>:<that mod's single-mod key>`. The single-mod key binds sources, ABI, capabilities, hooks and compiled Luau. The key is the SHA-256 of the `name=value` lines. The set name is not part of it, so identical sets reuse one build.
5. `mod set variant` extends the ADR 0289 **variant record cache** (`<library>/cache/variants/<key>/`): a hit requires the exact record and every stored member package and compiled Luau output to verify. Reasons name changed components (`membership`, `member.<i>`, `effective_image`, ...). Rebuild-reason history is per set name and image (`index/sets/`). A failed build publishes nothing. Each installed archive is read **once** and verified against the expected identity (`verify_package_bytes`); only those bytes are compiled, compared and published, here and in the single-mod `mod variant` route. A single-mod hit likewise requires the cached `package.segapatch` to be those bytes (this tightens ADR 0289, which only required the file to exist); a missing, unreadable or different cached archive is a miss and is rebuilt.

## Composition (SEG-141-T002)

6. Every member is first built **alone against the unmodified original**. Therefore every expected-value assertion, free-range fill and admission is judged on pre-composition bytes: a mod that expects another mod's bytes is `E_EDIT_EXPECTED_MISMATCH` in any order.
7. Conflicts are **claims** per address space. A readable package claims `cartridge-image` (image offsets; reported bank-qualified on Master System) and, for Genesis injected Z80 code, `z80.sound_ram` (the addresses the blocks are assembled for: two mods using one address collide even with disjoint cartridge bytes, although a product cannot know when each driver is loaded). Overlaps are reported once per colliding pair of members and space, with the number of ranges, so no pair is ever omitted. Disc spaces (one per file/extent) are keyed by the same overlap rule, which is tested directly; disc payload kinds stay reserved. Writes of different members must be disjoint (`E_SET_EDIT_OVERLAP`, naming both; identical bytes still overlap). Adjacent ranges are disjoint. Nothing is merged or reordered.
8. **BPS is conservative**: a `bps-image` declares no write ranges, may copy from anywhere in its source and was made from the original only. It composes solely with members that change no image byte and declare no hook (`E_SET_BPS_CONFLICT`), even if a byte diff would look disjoint. No three-way BPS merge exists. Authors who want composition use typed edits.
9. A write over the first instruction word of another member's hooked function is `E_SET_HOOK_SITE`.
10. Master System: besides each member's own admission, every **pair** of members with injected code is admitted together by the SMS owner, so a control target of one cannot land in the padding of, or in the middle of, the other's code (`E_SET_GUEST_ADMISSION`).
11. **One header policy.** The repair runs once over the composed image. `none` together with another image-changing member that does not say `none` is `E_SET_HEADER_POLICY`. An explicit repair declared by any member applies to the whole image; a member write that the combined repair would discard is `E_HEADER_FIELD_COLLISION`.

## Relations (SEG-141-T004)

12. Schema 3 gains optional `dependencies` and `incompatibilities`: `{"id", "version"?}` entries, strictly sorted by id, 1 to 16, present only when non-empty (`[[dependency]]`, `[[incompatibility]]` in `mod.toml`). The schema 1/2 spelling `requires` stays refused. They are part of the package identity, not of the content digest. Older products refuse such a package with `E_FIELD_UNKNOWN` (fail closed), and every manifest without them keeps its canonical bytes.
13. A version range is 1 to 4 comparators (`=`, `>=`, `>`, `<=`, `<` + `MAJOR.MINOR.PATCH`) separated by one space, all of which must hold, compared numerically. No `||`, caret, tilde or wildcard.
14. Verification is **check only**: every violated rule is listed in a deterministic order. A requirement is satisfied by presence anywhere in the set; the order never adds or moves a mod. The same mod id twice is refused.
15. **A mod used alone is a one-member set**: `mod variant`, every `segarecomp build` mod route, `mod diff-run`, `mod test` and `mod session` refuse a package with an unmet requirement. `install` stays per package and does not judge relations; `mod apply` and `mod bps` export one mod's image or patch (authoring artifacts that run nothing and carry no relations) and do not judge them either.

## Limits

* Free ranges are not claims across members: members may place disjoint blocks in a shared padding range, and one member's ROM edit may write bytes inside another's declared free range that its blocks do not use. This is weaker than the rule inside one package (`E_GUEST_COLLISION`); Master System joint admission still refuses a control target in padding.
* No multi-mod program is built or run. Callback dispatch, hook ownership between members (two members hooking one function are not judged), namespaced settings/saves, runtime memory-write conflicts and composition UI are SEG-141-T003/T005/T006. The support matrix therefore keeps multiple-mod composition `unsupported`.
* Disjoint bytes do not prove that two mods work together: a mod may depend on the behaviour of code or data another mod changes (including `m68k.rom` ranges a provider reads, and cartridge code named through `@symbol`). That is not verified.
* The launcher library can store and verify sets; it cannot select, build or show them.

## Rejected

A dependency solver or automatic ordering; merging BPS patches or computing their ranges by byte diff; letting a later mod's write win; assertions against a partially composed image; a new identity for one-member sets.

## Evidence

`tests/mod_set_test.py` (CLI; composed bytes, checksums and canonical manifests computed independently; synthetic data only), `tests/mod_set_engine_test.cpp`, `apps/segarecomp-launcher/launcher_mod_set_test.cpp`.
