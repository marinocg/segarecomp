# ADR 0318: a window-plane HUD band stays centred and bordered by the backdrop in the widescreen view

- Status: accepted (W04). Extends [ADR 0262](0262-seg-118-m118-presentation-only-extended-surface-and-genesis-level-2.md) (window rows) and [ADR 0315](0315-asymmetric-widescreen-viewport.md) (the placed picture). Product-only; no game knowledge, ABI 2 unchanged.

## Context

Since ADR 0315 a provider may place the native picture anywhere inside the planned total (`left = min(camera, margin)` for a side-scroller). A game that draws its status bar / HUD with the VDP **window plane** on the
bottom rows (Castle of Illusion: the power meter and indicators on the bottom four cell rows) has that HUD *inside the native picture*, so the HUD followed the placement: at a level's first column it sat at the left
edge of the widescreen view and slid towards the centre as the camera moved, then stayed there. Beside it, the producer filled the columns from Plane B (Plane A is already omitted where the window owns an edge),
so unrelated background tiles appeared next to the HUD and, with vertical scrolling, looked like they floated.

This is not a tilemap-wrapping or background-coverage defect: the window plane does not exist off-screen, so there is nothing valid to show beside a window-plane band.

## Decision

1. **The HUD band is detected from the VDP state, not from a game.** The *band* is the run of rows at the bottom of the picture on which the window plane owns **both** picture edges (the vertical-boundary window
   below row N, with or without a horizontal part). A window that owns one edge only, or a window at the top, is not a band and keeps the ADR 0262 behaviour. The producer reports it as
   `GenesisExtensionResult::hud_rows` (0 = none; zero-initialised = the previous behaviour for every other producer and platform).
2. **Producer (the extension's own composition).** On band rows the extension columns beside the picture are the backdrop: the producer draws neither Plane A (already omitted) nor Plane B there. Sprites
   keep their normal priority (a sprite in the margin is still the VDP's own). Nothing is reconstructed or extended; the generic tilemap sampling, wrapping and scrolling are untouched.
3. **Presenter (the picture's placement).** `SvExtendedSurface::hud_rows` (validated `0 <= hud_rows <= height`) tells the shared host to draw the band rows, **whenever the extension is shown**, as the native-width
   picture row at the **centre of the planned total (texture column `side`)** with black on every other visible column, after the picture and the side treatment, so Raw, Comfort and Borders show the same band.
   The rows above the band are exactly as before (the picture where its placement puts it). `hud_origin_x = (W - 320) / 2` falls out of `side = (W - 320) / 2`; no 320 is hard-coded.
   When the extension is not shown (4:3, Original, or an asymmetric surface under a narrower plan) the picture, band included, is presented exactly as produced.
4. **Black, not the per-row VDP backdrop, in the presenter.** The producer's margin rows are the VDP backdrop (the surface); the presenter's relocated band uses black, the same neutral the Borders treatment uses for
   absent content. A game whose backdrop colour is not black behind its HUD rows would show black margins there: the established presentation behaviour of an extension with no content.
5. **Overlay contract: authored overlays stay visible over the backdrop.** The band is drawn **before** the legacy HUD overlay and the positioned layers, which are the same order as for every other presentation
   (picture, side treatment, then overlays). An explicitly authored layer (a package's indicator, its relocated HUD) is deliberate UI, not background, so it is not masked: it may sit over the black margins beside the band.
   The black-margin guarantee therefore concerns **plane/background content and the extension's own columns**, never an authored overlay. No further masking stage was added; their background lookup follows the band so
   alpha blending reads the pixel actually shown. Tested: a layer anchored in a band margin is fully visible while the rest of the margin stays black.
5b. **Virtual-sprite accounting (review of #247).** The compositor reports side pixels per frame, not per row, so the host cannot tell an actor pixel above the band (shown) from one inside it (removed by the band).
   While a band is applied the host therefore credits **no side pixel** (left/right counters stay 0; a set that has only side pixels is `virtual_sprite_unpresented`), and keeps crediting pixels in the native picture,
   which the band keeps whole. This is a deliberate **under-claim**: side actors above the HUD are shown but not counted while a HUD band exists, the same conservative rule Comfort and Borders already follow. Without a
   band nothing changes. No per-row count and no compositor interface were added.
5c. **Notice.** `virtual_sprite_unpresented` is a diagnostic only (coverage JSON, `reason_frames`, `last_reason`): it occurs in ordinary play and on mode changes and no longer raises a toast. The transition bookkeeping
   still advances, so a later genuine reason is announced; `virtual_sprite_rejected` and every other reason still raise their notice.
5d. **Thumbnails.** The menu thumbnail composes the part above the band and the band separately; a band as tall as the picture (`hud_rows == height`) has no upper part and is handled.
6. **Out of scope, deliberately:** a camera model, any per-game offset, the generic VDP renderer's native output, vertical placement, hiding sprites outside the band rows, a non-bottom band.

## Consequences

- The HUD position is independent of the camera and of the picture's placement, for every planned total; both margins are the same width (`side`) and symmetric.
- Gameplay rows (above the band), the native viewport (320 columns, no extension shown) and ABI 2 (`SegaGenesisRenderApi`, `view_left`, scenery windows) are unchanged.
- A virtual actor in the extension columns of a band row is not shown (the margins of those rows are the backdrop by design); actors above the band are unaffected, but their side pixels are not counted while a band exists (decision 5b).
- The menu thumbnail composes its two parts (above/inside the band) so it still shows the native picture; the small CRT preview crop is not recomposed.
- Tests added in the review round: `sv_vsprite_admission_test` (side credit with/without a band, centred and asymmetric, Raw/Comfort/Borders; the unpresented diagnostic is silent but intact; genuine reasons still notify), `sv_hud_band_test` (full-height band, thumbnails for 0/32/224-row bands, the overlay contract).
- Tests: `sv_hud_band_test` (the real presenter at 32:9, 21:9 and 16:9, four placements at 32:9, Raw/Comfort/Borders: HUD column constant, margins black and symmetric, above-the-band picture at its placement, no band = the previous
  output, an invalid band refused, the extension not shown leaves the native picture untouched, an aspect change); `genesis_extension_tests` (the producer against the independent reference with the band rule, a bottom-window
  scene whose margins are flat backdrop while the rows above keep their scenery).
