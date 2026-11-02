# ADR 0267: SEG-124 - verified native widescreen compatibility provider (Sonic the Hedgehog 1), package schema 2

## Status

Accepted (SEG-124-T001, one product PR with three internal checkpoints). Builds on ADR 0262-0266. It does not replace BPS patches (ADR 0266) and adds no generic widescreen
feature; it adds one title-specific, statically compiled component behind a small generic interface.

## Context

ADR 0266 classified game problems as A (renderer defect), B (game logic limitation: BPS), C (original hardware capacity limit: a statically compiled, game-aware presentation
extension may be considered) and D (unavailable information). The Sonic 1 streaming patch (class B) widens what the game submits to the VDP, but it cannot make the VDP show
world the game never prepared (the initial fill and a respawn leave one side of the 512-pixel plane unprepared, and every plane ring buffer stays a ring buffer), and it cannot
add a HUD beyond the original viewport. A generated-native program can present authentic information the hardware never displayed because the game's own level data is
resident in guest RAM. That is the capability this ADR admits, for one game, without turning the runtime into a scripting or plugin host.

## Decision

1. **Three sources stay separate.**
   * *Original guest execution*: the recompiled 68000/Z80 code of the original or BPS-patched image, guest RAM/VRAM/CRAM and normal device semantics. Unchanged by this ADR.
   * *Generic presentation*: the existing VDP-backed Level 2/3 extended surface (ADR 0262-0265). It reads VRAM, CRAM, scroll state and the SAT only.
   * *Verified native compatibility provider*: a statically compiled component, selected by a verified patch, that reads a bounded, frame-synchronized, read-only snapshot of guest
     work RAM and reports presentation data (extra name-table words, an overlay). It never writes guest state, never runs guest code, never changes CPU/VDP/Z80 semantics and never
     supplies hardware state; the generic VDP still decodes patterns, palettes, flips, priorities and sprites.
2. **Interface (generic, game-independent).** `platforms/genesis/runtime/native_compat.h` defines ABI version 1: a provider has a capability id, an implementation version, a component
   mask, `prepare(frame)` (validates ALL inputs for one frame and fails closed), `scenery_word(plane,row,x,hscroll,vscroll)` (the authentic name-table word of the world cell under an
   off-screen column) and `hud(overlay)`. The generic extension producer calls the hook only for a requested frame whose provider accepted the frame. The generic M68K/Z80/VDP code contains
   no Sonic knowledge. The Sonic 1 provider lives in `platforms/genesis/native_compat/` and is linked into a program only when a verified package names its capability.
3. **Frame synchronization.** The frame is published at the VBlank onset, before the VBlank handler runs. The state the game used to prepare the displayed frame is therefore the work RAM
   at the PREVIOUS onset (before that handler). The runtime copies the 64 KiB work RAM into an adapter-owned buffer right after each publication and stamps it with the virtual frame counter;
   the provider receives it only if it was taken exactly one frame earlier, otherwise the frame is served by the generic path. The camera is additionally cross-checked against the VDP's
   own per-line scroll values, which are the authority for what is displayed.
4. **No second renderer.** Scenery is delivered as name-table words, never as pixels. The generic code fetches patterns from the frame's VRAM and colours from the line's CRAM, applies flips, priority
   and the sprite layer exactly as for visible columns. The provider derives a word from verified level data (layout, chunk, block tables the game decompressed into RAM), not from the wrapped nametable.
5. **Package schema 2 (`docs/product/patch-packages.md`).** `schema_version` 2 adds the typed payload `native` (a closed JSON descriptor, `widescreen.json`) next to the optional `rom` (BPS). Schema 1 is
   read unchanged. Packages contain data only; the descriptor names a capability, the exact original image, the minimum implementation version, the enabled components and the validated width.
   Unknown capabilities, other revisions, malformed or oversized descriptors and a `requires` capability this release lacks are refused at install and build, never ignored. A native-only package leaves the image
   bytes unchanged (effective image == original) and is still a distinct variant.
6. **Identities.** The variant cache key additionally contains the capability id, its implementation version and the descriptor digest. The package identity (digest of the manifest, which names
   every payload digest) is already part of the key, so a native-only package can never share a build with Vanilla, with a BPS-only package or with another descriptor. No new meaning is given to existing digests.
7. **Provenance.** Only a frame whose scenery came from a provider that accepted that frame is labelled `SV_EXTSRC_AUTHORED`; a frame served by the generic path (unsupported scene, rejected state, no provider)
   stays `GENERIC` and receives the normal Comfort treatment. The HUD is a separate overlay (RGBA, drawn by the host after side treatment, only into visible side space, never over the native picture) so
   it neither forces nor depends on the provenance of the scenery.
8. **Failure.** Wrong revision, a scene or zone the provider does not support, a bad state pointer, inconsistent camera/scroll, a stale snapshot or an out-of-range world coordinate produce the generic
   result with an accurate status; the provider never invents scenery for a world coordinate outside the level (those cells are blank, showing the backdrop), never reads outside the snapshot and never crashes.
   Vanilla and BPS-only builds contain no provider code.
9. **Non-goals.** No interpreter, JIT, expression language, plugin loader, runtime-loaded executable payload, generic introspection of guest RAM, second VDP, scene engine, or claim for any game, zone or
   scene that is not individually validated.

## Amendments (SEG-124, same PR)

* **HUD replacement.** The patch removes the game's own HUD sprites (BPS; counters untouched) and the `hud` component replaces them: in the left side space while the visible side holds it
  (`SV_OVERLAY_MARGIN`), otherwise at the replaced HUD's place inside the picture (also at 4:3 and in Original, where the producer runs for the HUD only). The native centre therefore
  intentionally differs from Vanilla's HUD area; every other native pixel is unchanged (tested). The HUD is offered in any running level, scenery only in the verified zone.
* **Background bands.** A parallax band's position is only defined modulo the 512-pixel plane; the provider uses the alias nearest the background camera and leaves cells left of world x 0 blank.
* **Fixture capability.** `genesis.fixture.native-widescreen.v1` exists only under `SEGARECOMP_ALLOW_FIXTURE_CAPABILITIES=1` (synthetic tests), is never buildable and has no provider.

## Consequences

The first native-compat title adds title-specific code to the product, kept in one directory with a capability table and tests. Each further title needs its own investigation and provider. Authored
provenance stays a per-frame claim of a verified provider, never a mode setting.
