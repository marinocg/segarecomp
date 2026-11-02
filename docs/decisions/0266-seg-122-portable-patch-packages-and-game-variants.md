# ADR 0266: SEG-122 - portable patch packages (`.segapatch`), separate Vanilla / Patched variants, and the first two game compatibility patches

## Status

Accepted (SEG-122-T001, one combined product PR). Builds on ADRs 0261-0265 (widescreen mode, extended surface, Level 2/3, side presentation). Sonic CD, Master System, Sonic 2/3 and
Streets of Rage 1/3 are deferred successor work (SEG-122-T002, SEG-122-T004) and are not claimed.

## Context

SEG-123 left two honest limits of the presentation-only widescreen levels: where a game prepares only the scenery of its native viewport the extension shows plane ring-buffer content from
the far side (visible wrap), and where a game submits only the sprites of its own viewport the Level 3 layer has nothing to show beside the picture. Both are properties of the game, not of
the renderer, and neither can be fixed by presentation code without inventing content. A compatibility change therefore has to be an explicit, optional, verified modification of the game that
the user chooses, that never touches Vanilla, and that stays inside the project's rules (no interpreter, JIT, runtime opcode decoding, game-specific heuristics in generic CPU/VDP code,
invented hardware modes or raised hardware limits; deterministic, fail-closed; no commercial data in either repository).

## Decision

1. **A patch is a verified, optional modification of the generated-native game's behaviour or presentation; BPS is one payload format, not the definition.** Each problem is classified before a
   mechanism is chosen: **A** generic renderer defect (fix the renderer, with hardware-backed tests; never hide it with a game patch); **B** game logic limitation (a narrowly scoped change of the
   game's own code or data is appropriate, and a BPS delta is a suitable portable carrier); **C** original hardware capacity limitation (the original VDP interface cannot represent the content:
   consider a narrowly scoped, statically compiled, game-aware presentation extension that reconstructs content from verified game state, never presented as native VDP output); **D**
   fundamentally unavailable information (stay Partial, keep the Comfort/Borders fallback, never fabricate gameplay visibility). Only the payload types proven necessary are implemented.
2. **Classification of the two games (real local evidence, class level).**
   * **Sonic the Hedgehog (Genesis, USA/Europe REV00): B.** The level streaming routines keep one 16-pixel block of scenery beyond each screen edge of Plane A and Plane B, redraw rows 22
     blocks wide, and the initial level fill starts at the camera origin. The patch widens the streamed margins and row redraws only; the initial fill is shared with the title and ending screens (which scroll the plane without streaming) and is deliberately left unchanged, so a spawn/respawn left margin becomes valid only as the camera moves (documented limitation). The 512-pixel planes hold 416 pixels of valid scenery at three blocks per side, so the VDP can represent the 16:9
     extent (39 px per side) without any hardware extension: this is not class C. The unproven alternative, a presentation-side reconstruction of scenery from level data, is therefore not needed and not built.
   * **Streets of Rage 2 (Genesis, USA): B.** The sprite attribute table of a real play session contains no sprite piece whose x origin is below -32 or at/above 320 (across a long real
     play session), while the game keeps those objects alive and updated; three sprite-piece submission routines drop every piece outside exactly that window before it reaches the table. The Level 3
     compositor shows what the table holds (it uses the native sprite evaluation, per-line limits and priority), so this is not class A. No hardware limit prevents the 39 px beside the picture: this is
     not class C. SoR2's identity was verified from the local image (Streets of Rage 1 is a different image and is not patched).
   Neither game needed a native widescreen descriptor or a native compatibility component, so none is implemented (the manifest's `payloads` object is typed so one can be added when a game proves
   a need; v1 refuses every payload type except `rom`, and refuses any `requires` capability, rather than half-applying a future package).
3. **Package v1 (`docs/product/patch-packages.md`).** A ZIP with `manifest.json` and the declared payload. Strict closed-subset manifest (schema 1), exact platform + full source SHA-256, payload and
   patched-image SHA-256, optional `requires` (refused when unknown) and `widescreen_width_max` (informational, never drives capability). Standard BPS1 with source, target and patch CRC-32, range-checked copy
   offsets, exact command-stream end and a bounded output. The ZIP reader is bounded and conservative (entry count, names, sizes enforced while inflating, CRC, overlap, no links, no encryption, no ZIP64,
   no trailing data). The package identity is the SHA-256 of the manifest bytes (it names every payload and target digest). One implementation (`libs/media/src/patch_*.cpp`) serves the CLI seam
   (`segarecomp patch inspect|verify|apply`) and the launcher; nothing in a package is ever executed; there is no scripting, plugin, hook or dependency mechanism.
4. **Mechanism boundary: an input image modification before the normal static pipeline.** A Patched build materializes the verified patched image inside the build attempt's own partial folder (removed
   before publishing; never in a published build; the original file is only read) and the unchanged recompiler processes it. There is no runtime memory injection, no host code from packages, no change to
   the generic M68K, Z80 or VDP, and no title string heuristic: selection is by full source SHA-256. The patched game may execute different guest logic from Vanilla; guest equivalence is required
   between presentation modes of the same variant (verified), not between variants.
5. **Variants and identities.** Three concepts stay separate: the *library game identity* (platform + original SHA-256: the catalog key, the build metadata's `rom_sha256`, the per-game widescreen
   profile), the *effective compiled image* (`SEGARECOMP_ROM_SHA256` = the digest of what the program was built from, the verified patched image for a Patched program) and the *selected variant*
   (package identity). No new field is needed: the existing explicit `SEGARECOMP_PROFILE_ID` carries the original identity to the viewer for the per-game profile of a Patched program (ADR 0265's seam, also used by Sega CD); the meaning of every existing SHA-256 field is unchanged. The cache key
   of a Patched build appends the package identity and patched-image digest to the existing key (platform, build policy and model, toolchain, host, CLI identity are already in it), so a native-only or
   changed package can never share an entry; a Vanilla key is byte-for-byte unchanged. Build metadata records the variant, `entry_ready_for` and the older-build check require the recorded variant to
   equal the requested one, and a Patched build is never adopted as a game of its own. Cancellation and failure never touch a published build (the existing publish-by-rename discipline).
6. **Launcher.** A Patches / Variants card on the Genesis Details page: Install patch... (file chooser, full verification, confirmation sheet; never selects or builds), a Vanilla / Patched selection,
   Build patched version / Rebuild patched, Play Vanilla / Play Patched, Remove patch (optionally deleting only that patch's build through the validated storage executor). A selected-but-unbuilt Patched
   variant is not playable and never falls back to Vanilla. The catalog becomes schema 3 only when a patch is installed (older releases open it read-only).
7. **Authored provenance is not claimed.** Installing or running a patch never marks a frame `SV_EXTSRC_AUTHORED`: the evidence below covers sampled scenes, and the project does not classify frames
   automatically. The extension of both variants stays `SV_EXTSRC_GENERIC` with the producer's own Available/Partial status, the width slider, Comfort/Borders/Raw and the per-game profile apply unchanged,
   and Vanilla keeps the generic behaviour. A future ADR may grant authored provenance to scenes with explicit compatibility evidence.
8. **Hardware limits are never raised.** The SoR2 patch only changes which of the game's own pieces reach the table; the VDP's 80-sprite table, 20 sprites / 40 cells per line and the game's own 80-sprite cap
   still decide what is drawn. Validation counted the per-line budget before and after (below).
9. **Authoring stays private.** Byte-level knowledge lives in the private harness (`tools/seg122_authoring/`), exported deterministically; the product repository contains the format, the importer,
   the launcher and synthetic tests only.

## Consequences

Users can install a small portable file, get an additional separately built version of one game, and keep Vanilla untouched. Two real titles gain real, measurable improvements within stated scene
coverage. The cost is that a Patched program differs in guest behaviour from Vanilla by design, that any new game needs its own investigation and classification, and that coverage claims stay
limited to the scenes that were tested.

## Evidence

Synthetic: `tests/segapatch_package_test.py` (valid, deterministic authoring, independent Python reference agreement, hand-assembled BPS with all four commands, source/target/payload/CRC/size/offset
failures, traversal/absolute/drive/nested/symlink/duplicate/extra entries, bombs and oversize, unsupported schema/payload/capability, bounded mutation fuzz, atomic failure),
`library_catalog_test` (schema 3 round trip, variant records, selection fallback, damaged records, no adoption of Patched builds), `launcher_patch_gui_test.py` (real window: choose package, refusal of another
game's package, preview and install, Patched unbuilt is not playable, separate patched build from the verified image, distinct identities, Vanilla untouched, launches with the right identities,
persistence across restarts, cancel and failure keep both builds, second package gets its own build, removal with and without deleting the build, missing package, tampered build records).
Real games (local, class-level; no frames, bytes or addresses are retained) are recorded in the SEG-122-T001 backlog record.
