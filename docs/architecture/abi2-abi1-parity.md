# ABI 2 / ABI 1 parity matrix

Status: verified 2026-11-22 against product `main` `da82cf0` and `segarecomp-patches` `32eb7d4` (SEG-152-T002). The decisions that close every open row are in [ADR 0320](../decisions/0320-abi-2-exact-frame-presentation-and-parity-contract.md); the scenery rows rest on [widescreen scenery window sufficiency](widescreen-scenery-window-sufficiency.md). "Decided, implemented by SEG-152-Txxx" marks what no code does yet. Nothing in this document changes behaviour.

## 1. What is being compared

**ABI 1** is the native patch provider the three production widescreen packages use (`segarecomp_patch_provider`, a `GenesisNativeCompat` table: `prepare`, `scenery_word`, `hud`; declared in `platforms/genesis/runtime/vdp_render.h`). The extension producer (`genesis_vdp_produce_extension`, `vdp_render.c`) calls it for the frame it is producing, with that frame's own raster state and the work-RAM copy of the previous onset.

**ABI 2** is the scheduled mod ABI (`platforms/sdk/segarecomp_mod.h`, `platforms/genesis/sdk/segarecomp_mod_genesis.h`). Its host, `platforms/genesis/viewer/mod_capture.cpp`, *is itself an ABI 1 provider* to the producer (`kTable`, line 505: `prepare`, `scenery_word`, `hud`): ABI 2 render output reaches the screen through the same producer, compositor and virtual-sprite transport. There is no second renderer; parity is an adapter and contract question.

## 2. Verified timeline and the critical gap

`runtime.c` 3050-3070, `mod_scheduler.c` `hook_after_snapshot` (423), `mod_capture.cpp` `prepare` (421): at the onset that ends guest frame `N`, `frame-end(N)` runs, the frame counter becomes `N + 1`, the producer runs (ABI 1 callbacks for **raster N**, RAM copy of the end of `N - 1`; `compat_ram_valid` requires `stamp + 1 == count`, `runtime.c` 2796), the snapshot is taken (end of N, `runtime.c` 2808), and ABI 2 `render(N)` runs from that copy. Its output is served only when `visible_frame + 2 == count`, that is, for **raster N + 1**. The full timeline table, with the RAM and VDP data each step sees, is in ADR 0320.

Consequences, each tied to a matrix row:

1. `render(N)` has no raster N + 1 to verify against its RAM snapshot or to draw its HUD from; the three ABI 1 providers do exactly that (rows 2, 3, 8, 11).
2. `genesis_mod_scheduler_supported()` omits `SEGA_MOD_CAP_FRAME_VDP` (`mod_scheduler.c` 21-27); a required `frame.vdp` is refused at attach, VDP spaces return `SEGA_MOD_E_SPACE` in `render`, and `SegaGenesisFeatures` (`segarecomp_mod_genesis.h` 65-71) has no frame pointer. The ABI 2 documents promise `frame.vdp` copies in `render` (`mod-execution-phases.md` section 2): the contract is ahead of the implementation.
3. The scheduled-mod presenter is viewer-only (`SEGARECOMP_MOD_VIEWER`); a headless program has no extension producer. "Headless versus SDL consistency" therefore means the shared frame artifact plus the SDL viewer under the dummy video driver; T008 states it that way.

## 3. The matrix (classes: 1 direct, 2 different but equivalent, 3 partial, 4 missing, 5 obsolete)

Columns: evidence (file:line), the ABI 1 users, the owner of the change (SDK = header, host = scheduler/adapter, renderer = producer, package = provider, patches), the test that proves it, and the risk.

| # | ABI 1 facility | ABI 2 today | Class | Evidence | Used by | Owner / change | Test | Risk |
|---|---|---|---|---|---|---|---|---|
| 1 | `work_ram`: 64 KiB copy of the previous onset | `read(M68K_WORK_RAM)` in `render`: copy of the end of N (`service_read`, `hook_after_snapshot` 423-435) | 2 (data) / 3 (bulk) | `mod_scheduler.c` 423; `runtime.c` 2796/2808 | S1, S2, S3K | exact-frame `render` serves the previous-onset copy (the ABI 1 bytes); pointer in the frame view (T003; ADR 0320 D1, D4) | `genesis_mod_exact_frame_tests` byte-equality with an ABI 1 twin | thousands of 1/2/4-byte reads if the view is not used; none with the pointer |
| 2 | frame VRAM at the frame's first line | none; `frame.vdp` not grantable; VDP spaces `E_SPACE` | 4 | `mod_scheduler.c` 21-27, `service_read` | S1 (identity, HUD tiles, special stage wrapped plane), S2, S3K | frame view `vram` (always a host-owned copy; `flags` marks frame-start vs the producer's end-of-frame VRAM for a non-journaled frame; the callback is not declined) (T003; D4) | same | ABI 1 still calls the provider and draws its HUD on non-journaled frames (pinned by `genesis_native_compat_tests`); declining would regress it, so the view marks the state honestly |
| 3 | per-line registers, CRAM, VSRAM (`GenesisCompatLine`) | none | 4 | as row 2; `vdp_render.c` 1432 | all three | frame view `lines`/`line_count` (T003; D4) | same, incl. a mid-frame change | layout equality asserted (`sizeof(GenesisCompatLine) == sizeof(GenesisRasterLine)`, already tested) |
| 4 | picture `width/height`, planned `side`, `requested` | `view_margin` (margin of the last *presented* extension, `mod_capture.cpp` 239); no width/height | 3 | `mod_capture.cpp` `current_margin` | S2/S3K (`width == 320`), S3K `bg_ext_agree` (side) | frame view geometry + `side`, the margin of the frame being produced (T003; D4, D9) | `genesis_mod_exact_frame_tests` | none (one frame older today) |
| 5 | host helpers `read_hscroll`, `read_vscroll`, `vscroll_is_per_column`, `cram_rgb` | none | 4 | `vdp_render.c` 826-830 (`genesis_native_compat_host`), pure | all three | typed table members re-exporting them (T004; D5) | `genesis_mod_vdp_helper_tests` vs `GenesisCompatHost` | none (rejected alternative: native re-decoding) |
| 6 | per-cell `scenery_word(plane,row,x,hscroll,vscroll)` | `scenery_window`: <= 2 windows per plane, <= 128 x 64 cells, <= 4096 words, host lookup with the line's real scroll (`mod_scenery_window.hpp`) | 3 / 2 | `mod_capture.cpp` 221, 486; `vdp_render.c` 1523-1542 | S1, S2, S3K | geometry sufficient; **answer representability fails for the per-line fallback class (real Sonic 2 conflict): `T007_REQUIRED`, per-row generic flag** ([sufficiency](widescreen-scenery-window-sufficiency.md); D8) | `abi_parity_scenery_tests` (T001), the T008 gate | Sonic 1 and other zones unmeasured (gate) |
| 7 | `GENESIS_COMPAT_WORD_GENERIC` (ADR 0314) | a plane without a window | 2 | ADR 0314; `vdp_render.c` 1528 | S1 special stage, S3K | none | existing `mod_scenery_abi2_test.py` | none |
| 8 | per-frame accept/decline, component subsets, `reason`, `scene` | submit nothing = not served (non-fault); `scene()` token; no reason; the verification itself impossible (row 2/3) | 3 | `mod_capture.cpp` `prepare` 421-485 | all three | exact-frame seam (T003) + `decline(components, reason)`: provider-selectable reasons `UNSUPPORTED_SCENE`, `BAD_SCROLL`, `MISMATCH`, `BAD_STATE`; `STALE_STATE`, `FAULT` host-only (T006; D7) | `genesis_mod_refusal_tests`, `mod_refusal_abi2_test.py` | reason vocabulary kept to ABI 1's |
| 9 | fail-closed on an unanswered cell; a `FAULT` reason fails only the authored claim | the same accounting (ADR 0313 decision 2) | 1 | `vdp_render.c` `compat_faults` 1528/1542/1563 | S1-S3K | none | `genesis_native_compat_tests` (existing) | none |
| 10 | per-frame placement `left_px/has_left_px` (ADR 0315) | `view_left` / `view_margin`, epoch- and margin-bound | 2 | `mod_capture.cpp` `api_view_left`, `prepare` `p.left_ok` | (Castle; Sonic centred today) | none; exact-frame providers learn `view->side` and the binding is trivial (D9) | `mod_view_abi2_test.py`, `mod_view_transitions_abi2_test.py` unmodified | Castle's pattern must stay bit-for-bit |
| 11 | fixed HUD overlay: raw RGBA 128 x 256, `fallback_x/y` | `overlay_target(0)` + rect/text (<= 64 ops), same 128 x 256; `fallback_x/y` hard-wired 0 | 4 (art) / 3 (fallback) | `luau_hud_pack.hpp` 26; `luau_api.hpp` 41 | all three (HUD from game tiles and CRAM per line) | `overlay_bitmap` + `overlay_fallback` (T005; D6) | `genesis_mod_bitmap_tests` pixel equality with `GenesisCompatOverlay` | blend vs replace rule fixed in D6 |
| 12 | up to 4 positioned layers, <= 256 x 256 each inside a 65536-pixel transport, `anchor/reference/dx/dy/z`, `has_clip` | `overlay_layer` index 1-4, <= 128 x 128 (`kOverlayLayerSide`), no clip | 3 | `luau_api.hpp` 43; `luau_api.cpp` 268; `luau_hud_pack.hpp` 68 | none of the three today | bitmap-specific layer declaration + clip, shared constants unchanged (T005; D6) | same | a global constant change is rejected (silently accepts the previously rejected) |
| 13 | virtual sprites (ADR 0306) | `virtual_sprites(set)`; verified commands bound to a frame epoch by digest | 2 | `mod_capture.cpp` `api_virtual_sprites` 208 | none of the three today | none; same-frame by construction for an exact-frame provider (T003) | existing vsprite suites | none |
| 14 | save state with a provider | ABI 2 `state.mod` / `MODSTATE` | 5 | ADR 0299 | - | none | existing `genesis_mod_state_tests` | the S2/S3K cross-frame state, see D3 |
| 15 | capability grant and refusal | ABI 2 negotiation, `E_CAPABILITY_UNSUPPORTED`, optional capabilities | 5 | `sega_mod_negotiate` (`segarecomp_mod.h` 124) | - | + the version mechanism (D10) | `segapatch_abi_minor_tests` | none |

No row is "unknown". Rows 1-5, 8, 11, 12 are the new ABI 2 surface (T003-T006); row 6 is answered by T001; rows 7, 9, 10, 13-15 need no change.

### Row 6 in one paragraph

A window is addressed by the **plane cell the host derives from the line's real scroll**: `floor((x - hscroll) / 8)`, `floor((line + vscroll) / 8)`. Because the VDP reports `hscroll` as a 10-bit signed value, every scroll lies in -512..511, so the plane columns one plane needs are bounded by `(picture + 2 * side) / 8 + 128` cells; two 128-column windows therefore cover every scene up to a total width of 1024 pixels (32:9 is 796): **geometry is sufficient** (measured on Sonic 1, Sonic 2 and Sonic 3 & Knuckles scenes: never more than two windows per plane). **Answer representability is a different question**: a window holds one word per plane cell, so a coordinate that ABI 1 answers two ways inside a frame (a fallback line sharing a cell with an authored line, a registered-multiple change or a raster condition inside one plane cell row) cannot be expressed. The real Sonic 2 provider does this in Emerald Hill Zone 1 (136 of 269 accepted frames, plane A, up to 3 lines, an ideal window still differs in up to 2 pixel rows); the real Sonic 3 & Knuckles provider does not in Angel Island Zone 1 (0 of 1032); Sonic 1 is unmeasured. Hence `T007_REQUIRED` for the per-line fallback class only.

## 4. Intended differences (stated, not accidental)

| Difference | Why it is intended |
|---|---|
| ABI 2 has no per-pixel provider call: the host looks up bounded windows | the renderer never calls guest-influenced code in the per-pixel loop (ADR 0309); ABI 1's per-pixel calls cost 34,944 / 90,496 / 213,248 provider calls per produced frame at 16:9 / 21:9 / 32:9 against at most 4 x 4096 window words |
| An exact-frame provider cannot also use the predicted `render` timing | ADR 0320 D1; no consumer needs both |
| Refusal is a call with a bounded reason vocabulary, not a `prepare` return value | an ABI 2 callback returns nothing; the mapping onto `GenesisCompatDecision` is one to one (D7) |
| `frame.vdp` is required-only | a capability that selects callback timing cannot be optional (D2) |
| ABI 2 save state is the provider's `state.mod`, not an ABI 1 side channel | superseded (row 14) |

## 5. What remains open and who owns it

| Open item | Owner | Gate |
|---|---|---|
| Sonic 2 `g_prev_level`, `g_hud_frame` and Sonic 3 & Knuckles `g_prev_kx/g_prev_ky` redesigned under ADR 0320 R2/R4 | SEG-152-T010, T011 | zero differences from ABI 1 with the state cleared every frame |
| per-row generic flag (the Sonic 2 per-line fallback conflict) | SEG-152-T007, promoted by the resolution step | the T008 differential passes for Emerald Hill Zone 1 |
| answer-representability gate: Sonic 1, Sonic 2 revision 1, Sonic 3 & Knuckles zones other than Angel Island Zone 1 | SEG-152-T009, T010, T011 (real-provider comparison) | a conflict beyond the fallback class widens T007 with that evidence |
| exact-frame mechanism implementation | SEG-152-T003 | ADR 0320 D1-D4, D9, D10 |
