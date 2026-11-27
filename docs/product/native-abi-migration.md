# Migrating a Genesis native provider from ABI 1 to ABI 2

ABI 1 (`segarecomp_genesis_patch.h`, schema 2 packages) is **deprecated, not removed**. Write new providers against ABI 2 (`segarecomp_mod_genesis.h`, schema 3 `native-c11` packages). Existing ABI 1 packages keep loading and running unchanged; see [compatibility policy](#compatibility-policy).

The complete worked example is [`sdk-sample/abi2`](sdk-sample/abi2/src/provider.c): the ABI 2 counterpart of the ABI 1 [`sdk-sample/provider.c`](sdk-sample/provider.c), using every facility below. CI compiles it as strict C11; it is not linked into a game or run. The behaviour of each call is specified in [native SDK ABI 2](../architecture/native-sdk-abi-2.md) and compared with ABI 1 in the [parity matrix](../architecture/abi2-abi1-parity.md).

## What changes in shape

* ABI 1 has three callbacks (`prepare`, `scenery_word`, `hud`). ABI 2 has one `phase` callback; all frame output is submitted in the `render` phase.
* Require `frame.vdp` to get the **exact-frame** `render`: it runs at the producer seam, once per frame produced for presentation, and sees the same work-RAM copy and VDP state as ABI 1 `prepare` for that frame. Without it, `render` keeps its earlier predicted timing.
* Declare the ABI minor you use in `mod.toml` (`[abi] minor`) and in the provider's `required_minor`: 1 for the frame view, helpers, bitmap and refusal; 2 for `scenery_generic_rows`; 3 for `scenery_row_origin`.
* The host never calls provider code per pixel or per cell.
* **Faults differ from refusals.** `decline` is a refusal and the provider stays enabled. A bad argument, wrong phase, missing capability or exceeded budget is a fault: the callback's output is discarded and the mod is disabled for the session.
* Check every table's `size` before reading appended members.
* The exact-frame callback exists in the played (viewer) program. A headless capture refuses a required `frame.vdp` at attach (`E_CAPABILITY_UNSUPPORTED`).

## One worked example per ABI 1 facility

`f` is `host->platform_features`, `r` is `f->render`, `v` is `f->frame`.

### Entry point and provider table

```c
/* ABI 1 */ const GenesisNativeCompat *segarecomp_patch_provider(void **state_out);   /* abi_version, name, implementation_version, components, prepare, scenery_word, hud */
/* ABI 2 */ const SegaModProvider    *segarecomp_mod_provider(void **state_out);      /* size, abi_major, required_minor, platform, name, implementation_version, init, phase, hook, save_state, load_state */
```

ABI 2 has no `components` field: capabilities in the manifest and the calls you make say what you serve. One source exports one entry symbol, and one build never mixes the two ABIs (`E_ABI_MIXED`).

### Capabilities and packaging

`frame.work_ram`, `frame.vdp`, `scenery.words` and `overlay.rgba` keep their names. In ABI 2 `frame.vdp` is **required-only**. A schema 2 package becomes a schema 3 project:

```toml
[[source]]
kind = "bps-image"            # the schema 2 `rom` BPS delta, unchanged
path = "bps/game.bps"
target_sha256 = "<effective image digest>"
[[source]]
kind = "native-c11"
path = "src/provider.c"
[abi]
native = 2
minor = 3
[capabilities]
required = ["frame.vdp", "frame.work_ram", "overlay.rgba", "scenery.words"]
```

`segarecomp mod check` validates, `segarecomp mod build` also compile-checks, `segarecomp mod pack` packages, and `segarecomp build --native-package P --native-identity ID` links it (ABI 1 used `--patch-package`). See [mod projects](mod-projects.md). The ROM digests must stay those of the same exact image; if your original relied on a particular header checksum result, set `[build] header_repair` accordingly.

### Per-frame accept, decline, reason and component subsets (`prepare`)

```c
/* ABI 1 */ out->accepted = 0; out->reason = GENESIS_COMPAT_UNSUPPORTED_SCENE;
/* ABI 2 */ r->decline(SEGA_GEN_COMPONENT_HUD | SEGA_GEN_COMPONENT_SCENERY | SEGA_GEN_COMPONENT_SPRITES, SEGA_GEN_REASON_UNSUPPORTED_SCENE);
```

To serve a subset, decline only the others and submit the rest. `v->requested` is the ABI 1 `frame->requested` mask. You may use `UNSUPPORTED_SCENE`, `BAD_STATE`, `BAD_SCROLL` and `MISMATCH`; `STALE_STATE` and `FAULT` belong to the host.

### Scene token (`decision->scene`)

```c
r->scene("main", 4);
```

### Placement (`left_px`, `has_left_px`)

```c
/* ABI 1 */ out->left_px = left; out->has_left_px = 1;
/* ABI 2 */ r->view_left(left, v->side);   /* the second argument is the margin the placement and windows were computed for */
```

`0 <= left <= 2 * margin`; `left == margin` is centred. See ADR [0315](../decisions/0315-asymmetric-widescreen-viewport.md). Build windows wide enough for the placement you declare. A provider with a predicted (not exact-frame) `render` learns the margin from `view_margin`.

### Frame data (`work_ram`, `vram`, `lines`, `line_count`, `width`, `height`, `side`)

```c
const SegaGenesisFrameView *v = f->frame;   /* non-NULL only inside the exact-frame render callback */
uint8_t mode = v->work_ram[0x0102];         /* the same previous-onset copy ABI 1 prepare received; needs frame.work_ram */
uint16_t reg1 = v->lines[0].registers[1];   /* per captured raster line: registers, CRAM, VSRAM */
if ((v->flags & SEGA_GEN_FRAME_VRAM_FRAME_START) == 0) { /* v->vram is NOT a frame-start copy: decline if your identity checks need one */ }
```

Pointers are valid only during the callback. In interlace mode 2 `height` is 448 and `line_count` is 224: output row `r` reads line `r / 2`.

### Host helpers (`frame->host`)

```c
f->vdp->read_hscroll(v->vram, v->lines[line].registers, plane, line, &h);
f->vdp->read_vscroll(v->lines[line].vsram, v->lines[line].registers, plane, column, &s);
f->vdp->vscroll_is_per_column(v->lines[line].registers);
f->vdp->cram_rgb(v->lines[line].cram, index, rgb);
```

They are the same host functions with the same arguments and return values.

### Authored scenery (`scenery_word`)

Submit windows of name-table words instead of answering one cell at a time. The host looks up plane column `floor((x - hscroll) / 8)` and plane row `floor((line + vscroll) / 8)`.

```c
r->scenery_window(plane, cell_x0, cell_y0, cols, rows, words);   /* words[row * cols + col]; at most 128 x 64 and 4096 words; two windows per plane */
```

* A blank cell stays word 0.
* A cell outside a plane's windows withdraws that frame's authored-scenery claim, as an unanswered `scenery_word` did.
* **`GENESIS_COMPAT_WORD_GENERIC`** (a plane left to the generic extension): submit no window for that plane.
* **Lines answered with the VDP's own word** (returning 1 with the word read from VRAM): mark those produced screen rows.

  ```c
  r->scenery_generic_rows(plane, mask, rows, (rows + 7) / 8);   /* bit r & 7 of byte r >> 3; abi.minor 2 */
  ```

* **A line-dependent world origin** (two lines ask the same plane cell and need different words): give each screen row a whole-cell delta.

  ```c
  r->scenery_row_origin(plane, delta_cells, rows, rows * 2);    /* int16 per row; abi.minor 3 */
  ```

A window holds one word per plane cell. If a provider answers one cell in two ways that neither call can express, decline the frame and report the class; do not approximate.

### HUD overlay and fallback position (`hud`, `rgba`, `fallback_x`, `fallback_y`)

```c
r->overlay_target(0);
r->overlay_bitmap(x, y, w, h, rgba, w * 4);   /* RGBA8, straight alpha; REPLACES destination bytes, alpha 0 included */
r->overlay_fallback(fallback_x, fallback_y);
```

The fixed overlay stays 128 x 256. At most 8 bitmap requests and 393216 bytes per callback.

### Positioned layers and clip (`layer_rgba`, `layers[]`)

```c
r->overlay_layer_bitmap(index /* 1..4 */, anchor, reference, dx, dy, w, h, z);   /* up to 256 x 256; the layers share 65536 pixels */
r->overlay_target(index);
r->overlay_bitmap(0, 0, w, h, rgba, w * 4);
r->overlay_layer_clip(index, clip_x, clip_y, clip_w, clip_h);
```

A bitmap layer takes no rect or text requests.

### Virtual sprites (`GENESIS_COMPAT_SPRITES`, `out->vsprites`)

```c
r->virtual_sprites(&set);   /* the same GenesisVirtualSpriteSet type, in your own storage; at most once per callback */
```

### State kept between frames (statics updated in `prepare`)

`prepare` and the exact-frame `render` run only for frames that are produced, so state written there can differ between runs. Classify each item with evidence (ADR [0320](../decisions/0320-abi-2-exact-frame-presentation-and-parity-contract.md) decision 3):

* a **pure cache** may stay in `render` only if clearing it before every presented frame changes no output;
* **essential state** is maintained in a phase that runs for every guest frame (`frame_end`) or serialized through `state.mod` (`save_state`, `load_state`), and must match an uninterrupted run after save/load and rewind.

`frame_end(N)` runs before `render` of frame N, whose RAM copy is from the end of N - 1.

## Verifying a port

Compare the old and new packages on the same inputs, from the same product revision: per-frame verdict and reason, accepted components, HUD pixels, authored-scenery accounting, geometry and placement, and the frames after save/load, rewind and aspect changes. Use no tolerance and omit no frames. Keep commercial-derived data out of the project and its reports.

## Production ports as references

Ports of the Sonic the Hedgehog, Sonic the Hedgehog 2 and Sonic 3 & Knuckles widescreen providers are developed in the [segarecomp-patches](https://github.com/marinocg/segarecomp-patches) repository (descriptive reference only; check its catalog for what is published). They illustrate, respectively: a line-dependent world origin and an overlay beyond the virtual-sprite command limit; per-row generic scenery with HUD state kept in `frame_end` and `state.mod`; and background registration whose search seed is essential state.

## Compatibility policy

* Schema 2 / ABI 1 packages load and run unchanged in every supported release until the [removal gate](../decisions/0322-abi-1-deprecation-and-removal-gate.md) is met and a separate removal decision is taken. Deprecation deletes no code, header, test, reader support or published package.
* The ABI 1 test suites listed in [native SDK ABI 2 section 6](../architecture/native-sdk-abi-2.md#6-coexistence-with-abi-1-compatibility-matrix) stay mandatory in CI.
* New packages, examples and recommended workflows use schema 3 / ABI 2.
* `E_ABI_MIXED` rules are unchanged: one ABI per source and per build.
* Building an ABI 1 package prints no new warning and needs no new flag; the SDK header adds no `deprecated` attribute.
