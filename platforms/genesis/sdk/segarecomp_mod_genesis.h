/* segarecomp_mod_genesis.h -- ABI 2 Genesis feature table (docs/architecture/native-sdk-abi-2.md section 3). ABI 1's segarecomp_genesis_patch.h is separate and unchanged. */
#ifndef SEGARECOMP_MOD_GENESIS_H
#define SEGARECOMP_MOD_GENESIS_H

#include <segarecomp_mod.h>

#include "segarecomp_genesis_vsprite.h" /* SEG-140-T010 (ADR 0306): the per-frame virtual-sprite set, one transport shared by the ABI 1 seam, Luau and this feature */

#define SEGA_MOD_PLATFORM SEGA_MOD_PLATFORM_GENESIS
#if defined(SEGA_MOD_TARGET_PLATFORM) && (SEGA_MOD_TARGET_PLATFORM != SEGA_MOD_PLATFORM_ID_GENESIS)
#error "E_ABI_PLATFORM: this provider includes the Genesis header but the target platform is different"
#endif

/* Space identifiers are named constants of this table (never a numeric literal shared across platforms); append-only. */
typedef enum SegaGenesisSpace {
  SEGA_GEN_SPACE_M68K_WORK_RAM = 1, /* the only mod-writable Genesis space */
  SEGA_GEN_SPACE_M68K_ROM,          /* read at build time only */
  SEGA_GEN_SPACE_Z80_RAM,           /* limited read */
  SEGA_GEN_SPACE_VDP_VRAM,
  SEGA_GEN_SPACE_VDP_CRAM,
  SEGA_GEN_SPACE_VDP_VSRAM,
  SEGA_GEN_SPACE_M68K_HOOK_REGS /* the register view of the function hook in flight: offset = slot (0..7 D0-D7, 8..15 A0-A7, 16 CCR, 17 cycle counter, 24..31 stack slot 0..7), width 4; mod-execution-phases.md 5.3 */
} SegaGenesisSpace;

/* SEG-138-T011 (ADR 0305): the render-phase output of a native provider, the ABI 2 counterpart of the Luau `sega.overlay.*` / `sega.scene` bindings (same rules, same implementation in the host).
 * Reached through SegaGenesisFeatures::render, which is non-NULL exactly when `overlay.rgba` was granted. Calls are legal only inside a `render` callback; every call returns a SegaModStatus and a non-zero
 * result is also a fault of the callback (its staged output is discarded and the mod is disabled, mod-execution-phases.md section 6). Coordinates are integers; rgba is 0xRRGGBBAA; text is printable ASCII
 * (at most 64 bytes, other bytes become '?'); at most 64 rect/text requests per callback. A rect or text is clamped to its target (a note, never a fault); a layer outside the limits is a range fault.
 * `anchor` is the viewer's order top_left=0, top_center, top_right, middle_left, center, middle_right, bottom_left, bottom_center, bottom_right=8; `reference` is 0 = picture, 1 = presentation. */
typedef struct SegaGenesisRenderApi {
  uint32_t size; /* the host's sizeof */
  int (*overlay_layer)(uint32_t index, uint32_t anchor, uint32_t reference, int32_t dx, int32_t dy, uint32_t w, uint32_t h, uint32_t z); /* declares positioned layer 1-4 (at most 128 x 128), z 0-3 */
  int (*overlay_target)(uint32_t index);                                                                                              /* 0 = the fixed HUD overlay (default each callback), 1-4 = a layer declared earlier */
  int (*overlay_rect)(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgba);
  int (*overlay_text)(int32_t x, int32_t y, const char *ascii, uint32_t len, uint32_t rgba);
  int (*scene)(const char *id, uint32_t len); /* the frame's scene token [a-z0-9_-], at most 32 characters (a claim; grants nothing) */
  /* SEG-140-T010 (ADR 0306): submits this frame's virtual sprites (authored pattern pool included) as ONE GenesisVirtualSpriteSet, at most once per render callback (a second call is a range fault); the host copies
   * it. It needs only overlay.rgba (output only: it grants no read access). The host validates the whole set against the frame with the same rules and stable reasons as every other route (a rejected set
   * draws nothing and is reported); GAME_STATE_VERIFIED commands (the host decodes the art from the frame's own VRAM/CRAM) are accepted only when the host holds a current verified snapshot. A scheduled provider granted
   * `frame.work_ram` has one: its set is bound to the guest frame whose end-of-frame work-RAM copy the render callback ran against, and is served only for the frame produced two guest frames later whose snapshot is
   * byte-identical to that copy (a stale submission, restore, rewind or skipped callback never draws game art; the whole set is then dropped). Without `frame.work_ram` there is no snapshot and a verified
   * command rejects the whole set (UNVERIFIED_STATE); MOD_AUTHORED commands need none. A set lives for one frame. `set` must not be NULL (a range fault). Replaces the first reserved slot of the table: its size and the others are unchanged. */
  int (*virtual_sprites)(const GenesisVirtualSpriteSet *set);
  /* SEG-140-T011 (ADR 0309): authored offscreen scenery, available only when `scenery.words` was granted (otherwise SEGA_MOD_E_CAPABILITY, a fault of the callback). Submits, for one VDP plane (0 = A, 1 = B), a WINDOW
   * of name-table words in unwrapped plane-cell coordinates: word (c, r) of `words` (row-major, `cols` x `rows`, 1..128 x 1..64, at most 4096 words) is the cell at plane x = (cell_x0 + c) * 8, plane y = (cell_y0 + r) * 8,
   * where the host's producer looks a column outside the picture up at plane x = screen x - hscroll, plane y = line + vscroll (the horizontal/vertical scroll the VDP applies to that line, floor-divided by 8).
   * Words are standard VDP name-table words (priority, palette line, flips, pattern); the host's own VDP code decodes patterns and palettes, so art must already be resident in VRAM. A cell outside the window fails the
   * frame's authored scenery claim (the host then reports scenery not delivered and presents generically); it never invents a tile. At most TWO windows per plane per render callback (a third is a range fault; ADR 0313: a split-scroll band can need cells disjoint from the playfield's): a cell is answered by the first window of its plane that contains it, and a frame is served when at least one plane has a window, and a plane with none is left to the VDP-derived generic extension (ADR 0314: it is not a failure, so a provider whose plane is exactly what the generic extension already shows, such as a tiling background, need not submit it); a cell of a plane that has a window and falls outside all of them is a failed answer; the
   * window is committed with the callback's other output, belongs to exactly that one frame and is never carried to another. `words` is copied; NULL or out-of-bounds arguments are range faults.
   * Replaces the second reserved slot: size and the other slots are unchanged. */
  int (*scenery_window)(uint32_t plane, int32_t cell_x0, int32_t cell_y0, uint32_t cols, uint32_t rows, const uint16_t *words);
  /* ADR 0315: the asymmetric widescreen viewport. The host plans one TOTAL number of extra source pixels, 2 * margin (`margin` on each side when centred), from the user's display aspect; the provider only chooses
   * where the native picture sits inside that total. `view_margin` writes the per-side margin of the host's most recent extension that is PRESENTED (0 in Original, at 4:3, after a failed production or before the first one: keep the
   * picture centred then; a verified-HUD package makes the producer run in Original but nothing of it is presented, so the margin is 0 there). `view_left` declares, for the frame this callback describes, that `left_px` of the planned columns lie LEFT of the picture and the rest, 2 * margin_px - left_px, right of it (0 = the
   * picture at the left edge of the view, margin_px = centred). `margin_px` is the per-side margin the placement AND the scenery windows built for it were computed for (the provider may have computed from an earlier read of
   * `view_margin`): the host serves the placement and those windows only when it is the margin the frame is produced with, and otherwise neither (the HUD and sprites are independent of the view and stay). The provider supplies a placement, never dimensions: the host validates it and falls back to the centred layout when it is
   * missing, out of range, from a callback that is not the frame's own (a skipped callback, restore or rewind), or computed for a different margin than the one the frame is produced with (an aspect change); it never
   * reinterprets columns made for another view. A placement lives for one frame and needs only the render API. `left_px` above 1344 or `margin_px` above 672 is a range fault (the absolute bounds of two maximum margins and of one). Replaces the third and
   * fourth reserved slots: size and the others are unchanged. */
  int (*view_margin)(uint32_t *margin_px);
  int (*view_left)(uint32_t left_px, uint32_t margin_px);
  void *reserved[4];                          /* NULL */
  /* Appended by SEG-152-T005/T006 (ABI minor 1, ADR 0320 decisions 6 and 7; a provider reads these only when `size` covers them and its manifest declares abi.minor >= 1). */
#if defined(SEGA_MOD_TARGET_ABI_MINOR) && (SEGA_MOD_TARGET_ABI_MINOR < 1)
  void *e_abi_minor_1_required_render[7]; /* a source that declares no abi.minor does not see overlay_bitmap, overlay_layer_bitmap, overlay_layer_clip, overlay_fallback, decline, scenery_generic_rows or scenery_row_origin */
#else
  /* Bounded RGBA bitmap (decision 6). `overlay_bitmap` copies w x h RGBA8 pixels (byte order R,G,B,A, straight alpha, `stride_bytes` >= 4 * w, rows top to bottom; the buffer may be reused at once) into the current target
   * (overlay_target) at (x, y). The pixels REPLACE the destination bytes (alpha 0 and its RGB included, as the ABI 1 transport carries raw bytes); rect and text keep blending, in submission order. 1 <= w,h <= the target's size,
   * an origin that puts the bitmap partly outside is clamped (a note, never a fault); NULL, a zero size, a short stride or an oversized bitmap is a range fault. At most 8 bitmap requests and 393216 submitted bytes per callback
   * (budget fault); a bitmap does not consume the 64 rect/text requests. `overlay_layer_bitmap` declares positioned layer 1-4 of up to 256 x 256 pixels (the declared layers share 65536 pixels) that accepts bitmaps only (rect/text
   * on it is a range fault); the existing overlay_layer limits (128 x 128) are unchanged. `overlay_layer_clip` sets GenesisCompatLayer::has_clip for a declared layer (the last call wins); `overlay_fallback` sets the fixed overlay's
   * fallback_x/fallback_y (0..65535 picture pixels; the last call wins). */
  int (*overlay_bitmap)(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint8_t *rgba, uint32_t stride_bytes);
  int (*overlay_layer_bitmap)(uint32_t index, uint32_t anchor, uint32_t reference, int32_t dx, int32_t dy, uint32_t w, uint32_t h, uint32_t z);
  int (*overlay_layer_clip)(uint32_t index, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  int (*overlay_fallback)(uint32_t x, uint32_t y);
  /* Refusal (decision 7), NOT a fault: this frame's `components` (SEGA_GEN_COMPONENT_*, the mask of SegaGenesisFrameView::requested) are declined with a diagnostic `reason` (SEGA_GEN_REASON_*): their staged output is
   * discarded (never half-served) and the host serves them generically with its own accurate status; the mod stays enabled and the next frame can be accepted. Only the provider-selectable reasons are accepted;
   * STALE_STATE, FAULT, OK, an unknown reason, or an empty/unknown component mask is a range fault (a provider must not impersonate a failure only the host observes). A second call declares the union of the components
   * and the first reason wins. Returns SEGA_MOD_OK for a valid argument. */
  int (*decline)(uint32_t components, uint32_t reason);
  /* Appended by SEG-152-T007 (ABI minor 2, ADR 0320 decision 8 addendum; declare abi.minor = 2 and required_minor = 2, and read it only when `size` covers it). */
#if defined(SEGA_MOD_TARGET_ABI_MINOR) && (SEGA_MOD_TARGET_ABI_MINOR < 2)
  void *e_abi_minor_2_required_render[2]; /* a source that declares abi.minor < 2 does not see scenery_generic_rows or scenery_row_origin */
#else
  /* Per-row generic scenery fallback (the ABI 1 `own_plane` lines). Available only when `scenery.words` was granted (otherwise SEGA_MOD_E_CAPABILITY, a fault of the callback); legal only inside a `render` callback.
   * For plane 0 (A) or 1 (B), marks produced SCREEN rows as answered by the VDP's own plane: the host answers every off-picture cell of a marked row with the plane's own name-table word read from the frame-start VRAM and
   * counts it as a provider-answered word (exactly what an ABI 1 provider returning 1 with the VDP's word does); authored windows of that plane and row are ignored. `mask` is indexed by produced screen row r (the row
   * the picture shows: line 0 is the first row of the native picture, not a world or plane cell row and not relative to `left_px`), bit (r & 7) of byte (r >> 3), LSB first; a set bit = the VDP's own word for that row.
   * `rows` is the number of rows the mask covers, 1..448 (448 = the interlace-mode-2 frame; marks beyond the current frame height are harmless) and `mask_bytes` must equal (rows + 7) / 8 (at most 56); the host reads exactly
   * `mask_bytes` bytes and copies them. Padding bits at row >= `rows` must be zero. NULL, a bad plane, rows 0 or above 448, a wrong `mask_bytes` or non-zero padding bits are range faults, as is a second submission for
   * the same plane in one callback. A mask is independent of windows (before or after scenery_window, on a plane with or without windows); a mask with a set bit counts toward the frame being served, one without has no effect.
   * Committed with the callback's other output and bound to exactly the frame it described, like the windows. */
  int (*scenery_generic_rows)(uint32_t plane, const uint8_t *mask, uint32_t rows, uint32_t mask_bytes);
  /* Appended by SEG-152-T017 (ABI minor 3, ADR 0320 decision 8 addendum 2; declare abi.minor = 3 and required_minor = 3, and read it only when `size` covers it). */
#if defined(SEGA_MOD_TARGET_ABI_MINOR) && (SEGA_MOD_TARGET_ABI_MINOR < 3)
  void *e_abi_minor_3_required_render; /* a source that declares abi.minor < 3 does not see scenery_row_origin */
#else
  /* Per-row scenery origin delta. Available only when `scenery.words` was granted (otherwise SEGA_MOD_E_CAPABILITY); legal only inside a `render` callback. For plane 0 (A) or 1 (B), `delta_cells[r]` is the signed number of
   * WHOLE PLANE CELLS by which the world differs from the plane cell the host looks up on produced SCREEN row r (line 0 = first row of the native picture): the host looks the window up at
   * floor((x - hscroll) / 8) + delta_cells[r] (the plane cell row is unchanged). This lets two screen lines that ask the same plane cell be answered with different words (a line-dependent world origin, e.g. a wrapped
   * 512 px plane). `rows` is 1..448 and `bytes` must equal rows * 2 (at most 896); rows >= `rows` have delta 0; the host copies the values. NULL, a bad plane, a wrong `bytes` or `rows` outside 1..448 are range faults, as is
   * a second submission for the same plane in one callback. The per-row generic mask (scenery_generic_rows) keeps precedence on its rows. Independent of window order; committed with the callback's other output and bound
   * to exactly the frame it described. A provider that never calls it behaves exactly as before. */
  int (*scenery_row_origin)(uint32_t plane, const int16_t *delta_cells, uint32_t rows, uint32_t bytes);
#endif
#endif
#endif
} SegaGenesisRenderApi;

/* ADR 0320 decisions 6/7 (the values of the host's GENESIS_COMPAT_* so `requested` and the reasons of an ABI 1 provider carry over unchanged). */
#define SEGA_GEN_COMPONENT_HUD 1U      /* the fixed overlay, layers and virtual sprites' overlay carrier: output of overlay_* */
#define SEGA_GEN_COMPONENT_SCENERY 2U  /* scenery windows */
#define SEGA_GEN_COMPONENT_SPRITES 4U  /* virtual sprites */
#define SEGA_GEN_REASON_UNSUPPORTED_SCENE 2U
#define SEGA_GEN_REASON_BAD_STATE 3U
#define SEGA_GEN_REASON_BAD_SCROLL 4U
#define SEGA_GEN_REASON_MISMATCH 6U

/* ADR 0320 (SEG-152-T003, ABI minor 1): the exact-frame view. Present only for a provider that REQUIRES `frame.vdp` (an exact-frame provider): the host runs its `render` callback once per produced frame at the producer
 * seam, and `SegaGenesisFeatures::frame` is non-NULL only inside that callback (NULL in every other phase and for every other provider). Everything is a host-owned copy valid only during the callback: there is no
 * pointer to live guest memory. `work_ram` is the copy taken at the previous onset (the one ABI 1 `prepare` receives for the same frame) and is NULL unless `frame.work_ram` was granted; `vram`, `lines` need `frame.vdp`.
 * `flags` bit SEGA_GEN_FRAME_VRAM_FRAME_START: `vram` is the frame-start state (a journaled frame); clear: it is the producer's own end-of-frame VRAM and NOT a frame-start snapshot (a frame without a VRAM journal is still
 * delivered, as for ABI 1). `requested` is the component mask the host asked for (SEGA_GEN_COMPONENT_HUD 1, SCENERY 2, SPRITES 4: the host's own GENESIS_COMPAT_* bits). `side` is the per-side extension margin planned for this frame (0 = none: Original, exact 4:3). */
#define SEGA_GEN_FRAME_VRAM_FRAME_START 1U
#define SEGA_GEN_FRAME_MAX_LINES 224U
typedef struct SegaGenesisFrameLine {
  uint16_t registers[24];
  uint8_t cram[128];
  uint8_t vsram[80];
} SegaGenesisFrameLine; /* layout equal to the host's GenesisCompatLine (asserted by the host) */
typedef struct SegaGenesisFrameView {
  uint32_t size; /* the host's sizeof: read fields only below it */
  uint32_t width, height;
  uint32_t side;
  uint32_t requested;
  const uint8_t *work_ram; /* 65536 bytes or NULL */
  uint32_t flags;
  const uint8_t *vram;                 /* 65536 bytes, host-owned copy */
  const SegaGenesisFrameLine *lines;   /* per output line registers, CRAM, VSRAM */
  uint32_t line_count;                 /* <= SEGA_GEN_FRAME_MAX_LINES: the number of captured RASTER (field) lines, NOT the output pixel rows: in interlace mode 2 `height` is 448 and line_count is 224; output row r reads line r / 2 */
  uint32_t reserved[7];
} SegaGenesisFrameView;

/* ADR 0320 decision 5 (SEG-152-T004): the host's own pure VDP helpers (the ABI 1 GenesisCompatHost functions, same semantics, 0 = success). Non-NULL exactly when `frame.vdp` was granted. */
typedef struct SegaGenesisVdpHelpers {
  uint32_t size; /* the host's sizeof */
  int (*read_hscroll)(const uint8_t vram[65536], const uint16_t registers[24], unsigned plane, unsigned line, int32_t *value);   /* plane 0 = A, 1 = B; plane_x = screen_x - hscroll */
  int (*read_vscroll)(const uint8_t vsram[80], const uint16_t registers[24], unsigned plane, unsigned cell_column, int32_t *value);
  int (*vscroll_is_per_column)(const uint16_t registers[24]);   /* non-zero when per-2-cell-column vertical scroll is selected */
  int (*cram_rgb)(const uint8_t cram[128], unsigned index, uint8_t rgb[3]);   /* index 0-63: palette line * 16 + colour */
} SegaGenesisVdpHelpers;

typedef struct SegaGenesisFeatures {
  uint32_t size; /* the host's sizeof */
  uint32_t work_ram_bytes, vram_bytes, vdp_registers, cram_bytes, vsram_bytes, visible_lines; /* 65536, 65536, 24, 128, 80, 224 */
  uint32_t reserved[8];
  /* Appended by SEG-138-T011 (append-only: a provider reads this field only when `size` covers it). Non-NULL exactly when overlay.rgba was granted. */
  const SegaGenesisRenderApi *render;
  /* Appended by SEG-152-T003/T004 (ABI minor 1; a provider reads these only when `size` covers them and its manifest declares abi.minor >= 1). */
#if defined(SEGA_MOD_TARGET_ABI_MINOR) && (SEGA_MOD_TARGET_ABI_MINOR < 1)
  /* ADR 0320 decision 10: a source whose manifest declares no abi.minor (0) does not see the minor-1 members; using one fails to compile ("no member named frame/vdp": declare "abi": {"native": 2, "minor": 1}). */
  const void *e_abi_minor_1_required_frame;
  const void *e_abi_minor_1_required_vdp;
#else
  const SegaGenesisFrameView *frame;   /* non-NULL only inside the exact-frame render callback of a provider granted frame.vdp */
  const SegaGenesisVdpHelpers *vdp;    /* non-NULL exactly when frame.vdp was granted */
#endif
} SegaGenesisFeatures;

#endif /* SEGARECOMP_MOD_GENESIS_H */
