# ADR 0303: authored virtual sprites, the VDP's own decode, priority/clip/budget rules and positioned, layered overlays

- Status: accepted (combined delivery F14: SEG-140-T001, SEG-140-T002, SEG-140-T003, SEG-140-T004). Builds on [ADR 0267](0267-seg-124-native-widescreen-compatibility-provider.md) (the fixed HUD overlay and the provider seam), [ADR 0301](0301-widescreen-geometry-rational-aspect-planner-and-bounded-surfaces.md) / [ADR 0302](0302-honest-geometry-declarations-and-per-scene-coverage.md) (geometry and coverage) and [ADR 0296](0296-luau-only-mod-executable-integration.md) (the Luau overlay path).
- Presentation only: nothing here reaches guest state, VRAM, the sprite attribute table, VDP status, a save state or the native picture.
- Not in scope (SEG-140-T005..T007): scene rejection and per-component coverage of virtual sprites, the Sonic actor-art and object-loading study, per-scene 32:9 results. Also not in scope: attaching virtual sprites to the live viewer or exposing them to Luau/native providers (this delivery ships the command model, the compositor and the rules as a tested C11 module; the presenter hookup is the consumer's work and is named below), sprites drawn into the widescreen side columns, Master System / Sega CD (SEG-147-T006, SEG-148-T009), an emulation of sprite DMA or per-line timing.

## Decision 1 (T001): the virtual-sprite command, its two data origins and what drawing does NOT mean

A **virtual sprite** is one bounded per-frame command asking the presenter to draw a sprite-shaped pattern block over the finished native picture. A frame's commands are a `GenesisVirtualSpriteSet` (`platforms/genesis/runtime/virtual_sprite.{h,c}`, strict C11, library `segarecomp::runtime_genesis_vsprite`, linked only by a consumer that presents them; it adds no symbol to the runtime every generated program links).

| Field | Meaning and rule |
| --- | --- |
| `provenance` | `GAME_STATE_VERIFIED` (1) or `MOD_AUTHORED` (2); anything else is `PROVENANCE`. |
| `layer` | `BEHIND_PLANES` (0), `BETWEEN` (1), `ABOVE_ALL` (2); else `LAYER`. See decision 3. |
| `x`, `y` | picture pixel of the top-left corner, `|v| <= 1024`, may be negative (partly off the picture); else `POSITION`. |
| `width_cells`, `height_cells` | 1..4 each (the hardware shapes); else `SIZE`. |
| `tile_index` | first pattern; the block uses `width * height` consecutive patterns, column-major, exactly like a hardware sprite; `tile_index + cells` must stay inside the 64 KiB image without wrapping (2048 patterns, 1024 in interlace mode 2); else `TILE_RANGE`. |
| `palette` | 0..3; else `PALETTE`. |
| `h_flip`, `v_flip`, `has_clip` | 0 or 1; `reserved` bytes and the set's `reserved[3]` must be 0; else `RESERVED`. |
| `clip_x/y/w/h` | with `has_clip`: a non-empty rectangle wholly inside the picture; else `CLIP`. |
| set | at most 64 commands (`TOO_MANY`), `budget_mode` 0 or 1 (`BUDGET_MODE`); the command order is the priority order. |

**Lifetime: one frame.** Commands are consumed by the composition of the frame they were supplied for. Nothing persists, nothing is carried to the next frame, nothing is written back.

**Provenance classes are enforced, not advisory.** The class selects the art source and a gate; the host, not the author, applies both:

- `GAME_STATE_VERIFIED` - patterns and palette come from the frame's own VRAM / per-row CRAM (the game's art). It is accepted only when the host says the frame's guest-state snapshot is valid and exactly current (`GENESIS_VSPRITE_HOST_STATE_VERIFIED`, the same condition as `compat_ram_valid`); otherwise the whole set is rejected with `UNVERIFIED_STATE`. The host cannot check that the position the author computed really depicts the game object: that remains the author's claim, exactly like a provider's scene name (ADR 0302). What the host does guarantee is that a "verified" sprite can only show what the frame's VRAM really holds, and only in a frame with verified state.
- `MOD_AUTHORED` - patterns and palette come **only** from the set's own authored pool (`authored_vram`, `authored_cram`, the same layouts as VRAM and CRAM, so the same decoder reads them). Never from the frame, so authored art can neither impersonate nor depend on the game's art. It needs no verified frame state (it asserts nothing about the game).

The two classes are accounted separately (`pixels_drawn_verified`, `pixels_drawn_authored`) so a coverage report can later distinguish them.

**Rejection is whole-set and fail-closed.** One invalid command rejects every command of the frame: nothing is drawn, nothing is approximated, and the report carries the stable reason and the first offending index. (Layered overlays, decision 4, are independent images and are rejected per layer.) An absent, empty or rejected set yields the native picture byte for byte.

**Drawing does not imply object activation or art residency.** A virtual sprite is a picture, not an object. Drawing one does not create, wake, move or despawn a game object, does not allocate, load or keep any art resident, does not touch the game's sprite table (`row_hw_sprites` is the VDP's own selection, unaffected), and does not set VDP sprite-collision or overflow flags. A verified sprite whose tiles the game has not loaded simply shows whatever those VRAM bytes are (often nothing); the host neither detects nor repairs that. Whether an object exists, or its art is resident, is established only by the game state the author reads through the existing read-only services, never by the act of drawing.

**Relationship to the hardware sprite table.** The VDP is not changed or emulated further: the hardware sprites are evaluated exactly as before (`genesis_vdp_build_sprite_row`, 20 / 16 per line, 40 / 32 cells, link walk and overflow stop). Virtual sprites are evaluated afterwards, against the finished native classes and the budget the VDP actually used (decision 3).

## Decision 2 (T002): one decoder, exposed read-only

The decode is the VDP's own. `vdp_render.c` gains read-only helpers and no table: `genesis_vdp_decode_sprite_rgba` (a whole sprite-shaped block to RGBA8 through `genesis_vdp_resolve_sprite_pixel` and `genesis_vdp_decode_cram_entry`: flips, column-major order, palette line, interlace-2 cell height, transparent index 0), `genesis_vdp_frame_row_cram` (the palette a row used) and `genesis_vdp_produce_frame_aux` / `_raster_aux` (the normal render plus side data, below). The compositor resolves every pixel through `genesis_vdp_resolve_sprite_pixel`, the function the VDP's row composer shares its pattern read with. `tests/genesis_vsprite_ownership_test.py` fails if `virtual_sprite.c` ever grows its own nibble/palette decode.

**Zero change to normal output.** The row composer is split, not rewritten: the SAT walk became `genesis_vdp_select_row_sprites` (same code, used by the composer and by the budget accounting) and the pixel loop became an inlined `genesis_vdp_render_row_impl` taking an optional layer output that is `NULL` for every existing caller (the optimiser folds it away). Evidence: all existing `genesis_vdp_render_*` and extension tests pass unchanged; `genesis_vsprite_decode_tests` pins a golden frame digest recorded from `origin/main` before the change and requires the aux render to be byte-identical to `genesis_vdp_produce_frame`.

**Differential evidence.** For 96 combinations (six shapes x four palettes x four flips) on randomised synthetic VRAM/CRAM, a virtual sprite composed over a frame without the hardware sprite equals, pixel for pixel, the frame the same VDP renders with that hardware sprite in its SAT (verified art), also with authored art in the private pool and the frame's own VRAM wiped; the standalone decode agrees with the hardware frame. A negative control (a wrongly flipped sprite) proves the comparison can fail.

## Decision 3 (T003): layers against the native priority classes, clip rectangles, the original budget

**Native classes.** The aux render classifies every native pixel by what produced it: `BACKDROP`, `LOW` (a low-priority plane or sprite pixel) or `HIGH` (a high-priority one), using the same winner selection as the VDP. Layers are defined against those classes:

- `BEHIND_PLANES` - visible only where the native pixel is `BACKDROP` (behind every plane and sprite pixel).
- `BETWEEN` - visible where the native pixel is `BACKDROP` or `LOW`; hidden by `HIGH` content ("above low-priority tiles, below high-priority tiles").
- `ABOVE_ALL` - visible everywhere.

A pixel takes the first opaque candidate in (layer high to low, then command order); a candidate hidden by the native class does not block a lower one that is visible. Transparent pattern index 0 never wins. This is a deliberate three-class model, not a six-layer sprite/plane split: it is what a presentation layer needs and what the VDP's own selection can report exactly.

**Clipping.** Each sprite may carry a clip rectangle (picture pixels); the picture bounds always clip. Pixels outside the picture are never written (tested with a canary). Sprites are composed inside the native picture only; the widescreen side columns are not a target in this delivery (a `Limitation`).

**Original budget.** Per output row the report accounts the hardware sprites and cells the VDP selected plus the virtual sprites that touch the row (a sprite clipped away on a row does not occupy it): `row_sprites`, `row_cells`, `rows_over_sprite_limit`, `rows_over_cell_budget`, against the frame width's original limits (20 / 40 in H40, 16 / 32 in H32). The default mode is `LIMIT`: virtual sprites obey what is left, in command order, with the hardware's own rules - no free slot drops the sprite for that row (`sprite_rows_dropped`), too few cells truncates it to the remaining cells (`sprite_rows_truncated`). `EXTENDED` is an **explicit mod opt-in** (`budget_mode = 1`): no per-line limit, over-budget rows are still reported. A line already full of hardware sprites takes no virtual sprite in `LIMIT`. No cycle accuracy or DMA timing is modelled.

**Defaults leave vanilla output unchanged:** no set, an empty set and a rejected set are the native picture exactly; the module is never linked unless a consumer presents sprites.

## Decision 4 (T004): bounded, positioned, layered overlays

`sv_layout.c` gains a pure planner (`sv_overlay_plan`) and blend (`sv_overlay_compose`), and `SvExtendedSurface` gains additive `layers` / `layer_count` (zero-initialised = none = presented exactly as before):

- **Position model.** Each layer names an `anchor` (nine points: top/middle/bottom x left/center/right) of a **reference rectangle** of the presentation geometry - `picture` (the native picture) or `presentation` (the whole visible surface: picture plus the visible side on both sides) - plus a signed `dx`, `dy`. At 4:3 both references are the picture; at 32:9 a top-right presentation anchor is the far edge of the extended surface (x = 796 - w) while a top-right picture anchor stays at the picture's own corner.
- **Order.** `z` 0..3 ascending, equal `z` in submission order. The fixed HUD overlay is **not** a layer: it is drawn first, exactly as before, and the layers go above it.
- **Bounds and budget.** At most 4 layers, each 1..256 per side, at most 65,536 submitted image pixels in total (every submitted image of valid size counts), `|dx|, |dy| <= 2048`, a clip rectangle wholly inside its image. A layer whose placed rectangle is not **wholly inside** its reference is **rejected** (`out_of_bounds`), never moved, clipped or resized. Each rejection is per layer with a stable reason (`count`, `no_image`, `size`, `budget`, `anchor`, `reference`, `z`, `offset`, `out_of_bounds`, `clip`); the valid layers are still drawn. The host counts rejections, remembers the last (`sv_host_overlay_diag`) and shows a toast once on entering the state.
- **Backward compatibility.** `sv_overlay_test` (the HUD golden: exact positions, side-style independence, lifetime) passes unchanged, `sv_overlay_layers_test` shows a frame with a HUD and a zero layer count is byte-identical to the HUD alone, and the Luau golden frames of `mod_luau_executable_test` are unchanged.
- **API.** C11 providers: `GenesisCompatOverlay` gains appended, zero-defaulted fields (`layer_rgba`, `layer_count`, `layers[4]` of `GenesisCompatLayer`); the host zero-initialises the struct and the layer buffer is optional, so an ABI 1 provider that predates the fields is presented exactly as before (additive, no version bump; the runtime validates the transport PER LAYER: a descriptor that is empty or whose image would leave the host buffer is flagged (`layer_bad`), the presenter never forms a pointer to it and rejects exactly that layer (`no_image` / `size`), and its valid siblings keep their order and z; descriptors beyond the four the host holds are counted and rejected as `count`, never read). Luau: `sega.overlay.layer(index, anchor, reference, dx, dy, w, h, z)` and `sega.overlay.target(index)` under the existing `overlay.rgba` capability (so no new catalogue entry); layers are at most 128 x 128 (four of them are exactly the host budget); every malformed call is a `range` script error that disables the mod, a layer the geometry cannot hold is a host diagnostic, not a script error. The generated API list and declarations are regenerated.

## Tests that SEG-140-T002..T005 must keep passing

- T002: `genesis_vsprite_decode_tests` (differential decode, golden normal output, helper argument checks), `genesis_vsprite_ownership_test`, every `genesis_vdp_render_*` test and `genesis_extension_tests`.
- T003: `genesis_vsprite_layers_tests` (each priority case at each native class, ordering, clip edges, picture edges, every rejection reason, read-only behaviour, hand-counted budget in LIMIT / EXTENDED, H40 / H32 limits).
- T004: `sv_overlay_layers_test` (anchors at 4:3 and 32:9, rejection diagnostics, HUD golden), `sv_overlay_test`, `luau_api_test`, `mod_luau_api_test`, `genesis_native_compat_tests`, `mod_luau_executable_test`.
- T005 (scene rejection, per-component coverage): its own tests must reuse the `GenesisVirtualReason` classes of decision 1 as the stable rejection vocabulary, keep `rejection_rules` in `genesis_vsprite_layers_tests` green, and must not count a virtual sprite as delivered `actors` coverage unless the sprite set was composed (`GENESIS_VSPRITE_STATUS_COMPOSED`) for a verified scene. T006/T007 must not describe a result as gameplay support from virtual sprites: they are presentation.

## Limitations

- Virtual sprites are not attached to the live viewer, to Luau or to native providers in this delivery; the viewer does not yet call the compositor, and a mod cannot author them yet. The compositor, the rules and the evidence exist so that consumer is a thin presenter. (Layered overlays, by contrast, are wired end to end: Luau, native provider seam, runtime transport, host.)
- Verified sprites read the frame-end VRAM (the same limit as the extension producer: mid-frame VRAM writes are not replayed for them).
- Virtual sprites do not reach the widescreen side columns; positions are picture pixels.
- Layered overlays are not counted as `hud` coverage delivered (they are not a declared component yet; T005 decides); a mod that draws only into layers is reported `missing` for a declared `hud`.
- A verified sprite's position depicts the author's reading of the game state; the host verifies the gate (frame state valid) and the art source, not the semantics.
- The additive provider fields are compatible in one direction: a provider built before them runs unchanged on a new host. A provider built against the new header must run on a host of the same build (providers are compiled into the program with the host today); there is no struct-size guard for the reverse pairing.

## ABI scope decision (T004: "through ABI 2 / Luau capability list")

ABI 2's catalogue already names the capability `overlay.rgba` (id 6, `render`) and the Luau API list is its Luau face; F14 extends that one capability, so the Luau route is complete. The runnable native route today is the ABI 1 provider seam (`GenesisCompatOverlay`, extended additively), the only native provider path a built program links. ABI 2 has no typed overlay feature, and no native ABI 2 provider can run in a built program until SEG-138-T011. Therefore F14 does not add a second native overlay path. The ABI 2 native overlay route (a typed feature mapped onto the same host planner and rejection vocabulary, with the F14 anchor/rejection tests held through it) is owned by **SEG-138-T011** (harness refinement PR marinocg/segarecomp-harness#133).
