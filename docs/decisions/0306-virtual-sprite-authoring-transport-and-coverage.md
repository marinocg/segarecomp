# ADR 0306: virtual-sprite authoring, one transport, and truthful live coverage

- Status: accepted (combined delivery F28: SEG-140-T008, SEG-140-T010). Builds on [ADR 0303](0303-authored-virtual-sprites-vdp-decode-priority-budget-and-layered-overlays.md) (the compositor and its rules, unchanged) and [ADR 0304](0304-scene-rejection-reasons-and-visual-region-coverage.md) (coverage accounting). Side columns are [ADR 0307](0307-virtual-sprites-in-the-widescreen-side-columns.md).
- Presentation only: nothing here reaches guest state, VRAM, the sprite table, VDP flags or a save state. No game-specific behavior; drawing a sprite never proves a game object exists.

## Decision 1: ONE per-frame submission model, three authoring routes

`GenesisVirtualSpriteSet` (commands, authored pool, budget mode, coordinate space) is defined **once**, in the public SDK header `segarecomp_genesis_vsprite.h`; the compositor's `virtual_sprite.h` includes it (compile-time asserts tie its pool sizes to the VDP's). Every route ends in that structure and in the same compositor, validation and rejection vocabulary:

| Route | Submission | Capability |
| --- | --- | --- |
| ABI 1 native provider | `GenesisCompatOverlay::vsprites` (zeroed, host-owned), declared by `GenesisNativeCompat::components` |  `overlay.rgba` (and `frame.work_ram` only for verified art) |
| Luau | `sega.sprite.space/budget/tile/palette/add/clip`, staged in the render callback (`SpriteSubmission`, one shared recorder) and packed into the set | `overlay.rgba` |
| ABI 2 native provider | `SegaGenesisRenderApi::virtual_sprites(const GenesisVirtualSpriteSet *)`, once per render callback | `overlay.rgba` |

No ABI version changes; every field is appended or takes a reserved slot. **Authorization.** Virtual sprites are output only: the script or provider supplies commands and its OWN pattern/palette pool and never reads guest memory or VDP state through this API, so the existing `overlay.rgba` output capability is the right boundary (no new capability identifier: the choice is made on the authorization boundary, not on naming). `GAME_STATE_VERIFIED` selects the frame's own VRAM/CRAM and is therefore gated by the host: the set is accepted only when the host established a valid, exactly current snapshot (`compat_ram_valid`, which exists only for an ABI 1 provider holding `frame.work_ram`). On the Luau and ABI 2 routes (the scheduled path grants no `frame.work_ram`) a verified command rejects the whole set as `UNVERIFIED_STATE`; the gate is never weakened.

## Decision 2: where the compositor runs

F14's ownership rule stands: `virtual_sprite.c` is not part of the runtime library every generated program links, and the headless program contains none of it. The producer (`genesis_vdp_produce_extension`) asks the PROVIDER for the set while the frame's raster state and VRAM are valid, zeroes the set before every request (a set never survives into the next frame) and, only for a set with at least one command, calls an adapter-supplied compose hook; the SDL3 adapter then runs the VDP's own auxiliary render and the F14 compositor into static buffers. Zero commands: neither runs and nothing is allocated. Strict C11, bounded, deterministic.

## Decision 3: what the viewer presents, and what coverage may claim

The host shows the composed picture instead of the native one **only when this frame's contribution is credited**: the surface is current, the planner admitted the geometry and the frame is inside the declared range. Every refusal presents the native picture (byte-identical to a run with no set). The original guest framebuffer artifact is never modified; the composed picture is a presentation copy.

Coverage (`SEGARECOMP_COVERAGE_REPORT`, schema unchanged) gains one bounded block, absent until a set was ever submitted so every earlier report is byte-identical: `virtual_sprites{sets, composed, rejected, composed_without_pixels, presented, pixels_verified, pixels_authored, pixels_picture, pixels_left, pixels_right}`. Rules:

- A set the compositor composed but that drew **no pixel** (fully transparent artwork, fully clipped, fully occluded by native priority, fully dropped by the LIMIT budget) is reported as stage `validated` (`composed_without_pixels`); it credits **no** `actors` coverage. Only a composed set with at least one pixel that reached the presented frame is `composed` and credited.
- Authored and verified pixels are counted separately; authored pixels never support a statement that a game actor was shown (they only say the mod's own art was drawn).
- A rejected set carries its `GenesisVirtualReason` verbatim (`last_vsprite_reason`) and draws nothing; a stale surface is never shown or counted.
- Pixels count only in a region that was really presented inside an eligible declared range ([ADR 0307](0307-virtual-sprites-in-the-widescreen-side-columns.md) for the side columns).

The host also says why, once per change, as a visible notice (ADR 0304).

## Limits (stated, not hidden)

- The verified route exists end to end only through an ABI 1 provider with `frame.work_ram`; a verified command from Luau or ABI 2 is rejected (`UNVERIFIED_STATE`) until a route can establish a current snapshot. The tests prove the gate in both directions.
- The scheduled routes' output appears one frame after the render callback ran (ADR 0305); a set is one frame long on every route.
- Nothing changes what the real Sonic packages deliver: they submit no virtual sprites, so their declared `actors` component still reports `missing`, and no scene-class feasibility of the F15 classification changes.

## Rejected alternatives

A new capability for sprites (no authorization boundary differs from `overlay.rgba`); a second per-route compositor or validator; building the compositor into the runtime library; presenting the composed picture unconditionally (a refusal would then show sprites the report says were not presented); counting composed-but-empty sets as delivered.

## Verification

`mod_vsprite_viewer_test` (ABI 1 and both Luau routes against an independent golden model: authored/verified art, the gate, every rejection, layers, flips, clips, both budgets, lifetime, coverage), `mod_vsprite_abi2_test` (three-route pixel and accounting equality, the ABI 2 contract), `luau_api_test`, `mod_luau_api_test` (the generated API list and declarations), `genesis_vsprite_decode_tests`/`genesis_vsprite_layers_tests` (unchanged), `sv_coverage_test`, `sv_scene_rejection_test`.
