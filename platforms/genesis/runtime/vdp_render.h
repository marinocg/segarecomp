#ifndef SEGARECOMP_RUNTIME_GENESIS_VDP_RENDER_H
#define SEGARECOMP_RUNTIME_GENESIS_VDP_RENDER_H

/*
 * SEG-007-T049 checkpoint C1: tile-pattern and palette decoding.
 *
 * This module is a self-contained, generic decode layer over the VDP tile
 * ("pattern") and CRAM (Color RAM) byte layouts already stored by
 * GenesisVdpState (runtime.h; SEG-007-T098/T101/T108/T110). It performs no
 * plane/nametable addressing, no scrolling, no sprite-table traversal, and no
 * priority/composition/frame-output work -- those are SEG-007-T049's later
 * checkpoints (C2-C5), not this one. It is purely: given raw tile-pattern
 * bytes, produce a per-pixel palette index (0-15); given a raw 16-bit CRAM
 * word, produce a concrete RGB color. Both are static, deterministic, and
 * side-effect-free.
 *
 * Public hardware sources (bit-exact, cross-corroborated):
 *
 * - Tile/pattern format (8x8 pixels, 4 bits per pixel, 32 bytes per tile,
 *   4 bytes per row, high nibble = left pixel of a byte pair, low nibble =
 *   right pixel): Sega Enterprises, *Genesis Technical Overview* v1.00
 *   (1991) ("GTO1" in docs/references), pp. 53-54, pattern-data bit
 *   diagram "D7 D6 D5 D4 D3 D2 D1 D0" / "COL3 COL2 COL1 COL0 COL3 COL2 COL1
 *   COL0" (two 4-bit color-index nibbles per byte, high nibble first);
 *   independently corroborated by Plutiedev, "Tiles and palettes"
 *   <https://plutiedev.com/tiles-and-palettes> ("PLUTIE-PAL" in
 *   docs/references/genesis-vdp-data-port-cpu-write-contract.md): "tiles are
 *   stored as 32 bytes, where every 4 byte (a longword) represents a row,
 *   and every four bits is a pixel."
 *
 * - CRAM color-word format (each 16-bit CRAM entry stores a 9-bit BGR color
 *   as `----BBB-GGG-RRR-`: bits 15-12 unused, bits 11-9 = Blue, bit 8
 *   unused, bits 7-5 = Green, bit 4 unused, bits 3-1 = Red, bit 0 unused):
 *   Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), p. 32 ("64
 *   x 9-bits of CRAM"; 3-bit R/G/B fields); independently and precisely
 *   corroborated bit-for-bit by the widely-used, actively-maintained
 *   open-source Genesis Plus GX VDP core (`core/vdp_ctrl.c`,
 *   `cram_write` / CRAM-write handling), which packs "16-bit bus data
 *   (BBB0GGG0RRR0) to 9-bit CRAM data (BBBGGGRRR)" -- i.e. bit 0 unused,
 *   bits 3-1 = R (R2 R1 R0, R2 = bit 3), bit 4 unused, bits 7-5 = G, bit 8
 *   unused, bits 11-9 = B, bits 15-12 unused. This project adopts that
 *   `----BBB-GGG-RRR-` layout as the implemented fact, not merely a
 *   compatibility guess: it is the same layout essentially every
 *   independent open-source Genesis VDP implementation (Genesis Plus GX,
 *   BlastEm, PicoDrive) agrees on, and it is consistent with GTO1's own
 *   documented "B2 B1 B0 / G2 G1 G0 / R2 R1 R0" three-bit-per-channel
 *   9-bit-color statement. GTO1's own OCR'd page-32 bit-position table is
 *   not itself trusted bit-for-bit here (its raster/column alignment is not
 *   reliably recoverable from a plain-text derivative of a scanned PDF);
 *   the exact bit *positions* used above come from the independently
 *   corroborated, widely-implemented layout, not from re-deriving them from
 *   the OCR text alone.
 *
 * Channel-to-8-bit scaling (each 3-bit channel value 0-7 scaled to an
 * 8-bit 0-255 value by `value * 255 / 7`, i.e. 0,36,73,109,146,182,219,255)
 * is an explicit, labeled **project compatibility policy**, not a
 * documented hardware fact: the real VDP outputs an analog composite/RGB
 * signal, not an 8-bit-per-channel digital value, so there is no single
 * "correct" 3-bit-to-8-bit expansion. `value * 255 / 7` is a standard,
 * widely-used linear expansion and is adopted here as this project's one
 * reference scaling for the `GenesisRgb888` output type -- a project
 * compatibility policy choice, not a claim that any specific upstream
 * implementation uses this exact formula. A consuming task
 * needing a different (e.g. gamma-aware) scaling states that need
 * explicitly as its own scoped decision; it never overrides this file's
 * documented default silently.
 */

#include <stdint.h>

#include "runtime.h" /* GENESIS_VDP_CRAM_BYTES */

#define GENESIS_VDP_TILE_BYTES 32U   /* 8 rows x 4 bytes/row (8x8 pixels, 4bpp). */
#define GENESIS_VDP_TILE_WIDTH 8U
#define GENESIS_VDP_TILE_HEIGHT 8U
#define GENESIS_VDP_CRAM_ENTRY_COUNT 64U /* GENESIS_VDP_CRAM_BYTES / 2. */

/* An 8-bit-per-channel RGB color, produced by the labeled project scaling
 * policy documented above. */
typedef struct GenesisRgb888 {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} GenesisRgb888;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decodes one pixel's palette index (0-15) from a raw 32-byte tile-pattern
 * buffer (`tile`, exactly GENESIS_VDP_TILE_BYTES bytes, e.g. a pointer into
 * GenesisVdpState.vram at a tile-aligned offset) at zero-based (row, col).
 *
 * Returns 0 and writes the decoded index through `index_out` on success.
 * Returns a negative value and leaves `*index_out` unmodified if `tile` or
 * `index_out` is NULL, or if `row >= GENESIS_VDP_TILE_HEIGHT` or
 * `col >= GENESIS_VDP_TILE_WIDTH` (untrusted-input bounds check per this
 * project's own input-handling policy; every read this function performs is
 * bounds-checked against the fixed 32-byte tile buffer before it happens).
 */
int genesis_vdp_decode_tile_pixel(const uint8_t tile[GENESIS_VDP_TILE_BYTES], unsigned row,
                                   unsigned col, uint8_t *index_out);

/*
 * Decodes every pixel of one 8x8 tile into `indices_out`
 * (GENESIS_VDP_TILE_WIDTH * GENESIS_VDP_TILE_HEIGHT bytes, row-major,
 * top-left origin, one palette-index byte 0-15 per pixel). Equivalent to
 * calling genesis_vdp_decode_tile_pixel for every (row, col) pair (the
 * implementation does exactly that internally); provided as a convenience
 * for callers that want the whole tile decoded in one call.
 *
 * Returns 0 on success. Returns a negative value and leaves `*indices_out`
 * unmodified if `tile` or `indices_out` is NULL.
 */
int genesis_vdp_decode_tile(const uint8_t tile[GENESIS_VDP_TILE_BYTES],
                             uint8_t indices_out[GENESIS_VDP_TILE_WIDTH * GENESIS_VDP_TILE_HEIGHT]);

/*
 * Reads CRAM entry `entry_index` (0-63) out of a raw CRAM byte buffer
 * (`cram`, exactly GENESIS_VDP_CRAM_BYTES bytes, big-endian halfword order
 * per GenesisVdpState.cram's own documented layout) and decodes it into a
 * concrete GenesisRgb888 color per this file's documented CRAM bit layout
 * and channel-scaling policy.
 *
 * Returns 0 and writes the decoded color through `color_out` on success.
 * Returns a negative value and leaves `*color_out` unmodified if `cram` or
 * `color_out` is NULL, or if `entry_index >= GENESIS_VDP_CRAM_ENTRY_COUNT`.
 */
int genesis_vdp_decode_cram_entry(const uint8_t cram[GENESIS_VDP_CRAM_BYTES], unsigned entry_index,
                                   GenesisRgb888 *color_out);

/*
 * Decodes a single raw 16-bit CRAM word (already extracted from big-endian
 * storage into host byte order by the caller) into a concrete GenesisRgb888
 * color per this file's documented `----BBB-GGG-RRR-` bit layout and
 * channel-scaling policy. Bits outside the documented R/G/B fields are
 * ignored (never rejected -- real hardware CRAM writes may set them; this
 * project's decode simply does not interpret them as color).
 */
GenesisRgb888 genesis_vdp_decode_cram_word(uint16_t raw_word);

/*
 * SEG-007-T049 checkpoint C2: plane/nametable addressing and bound scrolling
 * behavior.
 *
 * Bound surface (see the task record's Notes for the bounded-diagnosis
 * provenance): standard Mode 5, H40 (320px) display, non-interlaced; Plane A
 * and Plane B only (no Window plane -- there is deliberately no selector
 * value for it below); full-screen (whole-plane) horizontal scroll mode
 * (register #11 bits 1-0 == 00), the SEG-007-T050-added per-scanline
 * ("line") horizontal scroll mode (register #11 bits 1-0 == 11 -- see the
 * dedicated citation below), and per-2-cell-column vertical scroll mode
 * (register #11 bit 2 == 1), or full-plane vertical scroll (bit 2 == 0).
 * Any other configuration (reserved plane
 * size, oversized plane, the "eight lines then repeat" horizontal-scroll
 * mode `01`, the "every tile" (per-8-line) horizontal-scroll mode `10`, or
 * any other unbound scroll mode) is rejected with a negative return -- this
 * module never guesses.
 *
 * Public sources (bit-exact where cited; a genuine two-source disagreement
 * is called out explicitly below and resolved):
 *
 * - Plane A nametable base address (register #2): bits 5-3 select address
 *   bits A15-A13 (address = (reg2 bits 5-3) << 13, i.e. `(reg2 << 10) &
 *   0xE000`): Plutiedev, "VDP register reference"
 *   <https://www.plutiedev.com/vdp-registers> ("register $82... Bit 5-3:
 *   SA15-13... divided by $2000"); independently corroborated bit-for-bit by
 *   the actively-maintained open-source Genesis Plus GX VDP core
 *   (`core/vdp_ctrl.c`, register-2 write handler: `ntab = (d << 10) &
 *   0xE000`).
 *
 * - Plane B nametable base address (register #4): bits 2-0 select address
 *   bits A15-A13 (address = (reg4 bits 2-0) << 13, i.e. `(reg4 << 13) &
 *   0xE000`): Genesis Plus GX (`core/vdp_ctrl.c`, register-4 write handler:
 *   `ntbb = (d << 13) & 0xE000`). **Discrepancy noted and resolved:**
 *   Plutiedev's own register-reference page names register #4's field as
 *   "Bit 4-2: SB15-13" rather than bits 2-0; that placement is inconsistent
 *   with Genesis Plus GX's independently-implemented, widely-relied-upon
 *   shift-by-13 decode (which only reads reg4 bits 2-0), and bits 2-0 is
 *   also the layout independently recalled from Charles MacDonald's
 *   long-standing "Genesis VDP" hardware notes (`genvdp.txt`, register #4
 *   "Bits 2-0 = Base Address bits A15-A13"), a third independent, widely
 *   cross-implemented source. This module implements bits 2-0 (matching two
 *   independent sources against one), and treats the Plutiedev page's "Bit
 *   4-2" wording as a probable transcription/formatting slip on that page
 *   rather than as a genuine, evenly-supported hardware disagreement.
 *
 * - Register #16 (plane size) decoding: bits 1-0 select plane width class,
 *   bits 5-4 select plane height class; each class value 0/1/3 decodes to
 *   32/64/128 cells respectively, value 2 (binary `10`) is reserved/invalid
 *   for either field, and the hardware additionally limits the *combined*
 *   nametable to at most 8KB (4096 cells, 2 bytes/cell) -- e.g. 128x64 or
 *   128x128 or 64x128 are each individually well-formed per-field but
 *   collectively exceed that combined limit and are therefore also
 *   rejected. Source: Plutiedev, "VDP register reference"
 *   (register `$90`, "Plane Size", enumerating exactly the valid combinations
 *   32x32, 64x32, 128x32, 32x64, 64x64, 32x128 -- precisely the six
 *   width/height pairs whose product does not exceed 4096 cells -- "other
 *   combinations don't function properly"); independently corroborated by
 *   Genesis Plus GX (`core/vdp_ctrl.c` register-16 write handler), whose
 *   `shift_table`/`col_mask_table`/`row_mask_table` likewise special-case
 *   the reserved value 2 for the width field. This module implements the
 *   general documented decode (field values -> cell counts, reserved value
 *   rejected, combined 4096-cell ceiling enforced) rather than hardcoding
 *   any one of the six valid combinations.
 *
 * - Mode-5 nametable entry word layout (`PPFF TIII IIII IIII` from bit 15
 *   down to bit 0: bit 15 = priority, bits 14-13 = palette select 0-3, bit
 *   12 = vertical flip, bit 11 = horizontal flip, bits 10-0 = tile index
 *   0-2047): Plutiedev, "Tile ID flags" <https://www.plutiedev.com/tile-id>
 *   ("Bits 10-0 are the tile number... Bit 11... horizontal flip... Bit
 *   12... vertical flip... Bits 14-13... palette... Bit 15... priority").
 *   This checkpoint only decodes/exposes these fields; composing palette
 *   colors or priority into a final pixel is SEG-007-T049 checkpoint C4, not
 *   this one.
 *
 * - Horizontal-scroll table base address (register #13, bits 5-0 select
 *   address bits A15-A10, address = (reg13 bits 5-0) << 10): Plutiedev,
 *   "VDP register reference" (register `$8D`, "H-Scroll Table", "Bit 5-0:
 *   HS15-10... divided by $400"). Full-screen mode uses exactly one row of
 *   that table (table_base + 0 = Plane A's horizontal-scroll word,
 *   table_base + 2 = Plane B's horizontal-scroll word): corroborated by
 *   Genesis Plus GX (`core/vdp_render.c`), whose big-endian 32-bit
 *   full-screen h-scroll fetch reads Plane A from the high (first, lower
 *   address) 16 bits and Plane B from the low (second, higher address) 16
 *   bits of that same table row.
 *
 * - SEG-007-T050: per-scanline ("line") horizontal-scroll mode (register #11
 *   bits 1-0 == `11`). Runtime-selected evidence (an authorized, ephemeral,
 *   non-persisted local inspection of the reached title-phase VDP register
 *   state; see this task's own backlog record for the sanitized normalized
 *   finding -- no raw address/byte/pixel content is reproduced here) showed
 *   the one non-full-screen horizontal-scroll mode this reached state uses
 *   is this "line" mode, not either of the other two non-full-screen
 *   encodings. Register #11 bits 1-0 select one of four documented
 *   horizontal-scroll granularities: Plutiedev, "VDP register reference"
 *   <https://www.plutiedev.com/vdp-registers> (register `$8B`, "Mode Set
 *   Register #3", field `H/LSCR`): `00`: "full scroll"; `01`: "scroll eight
 *   lines, then repeat"; `10`: "scroll every tile"; `11`: "scroll every
 *   line". This module implements exactly `00` (already documented above)
 *   and, as of SEG-007-T050, `11` ("scroll every line") -- `01` and `10`
 *   remain outside this module's bound surface and continue to fail closed
 *   (see `genesis_vdp_hscroll_mode_is_fullscreen`/
 *   `genesis_vdp_hscroll_mode_is_line` below), per this task's own Non-goals
 *   (no unobserved scroll mode is added speculatively).
 *
 *   Independently corroborated, bit-for-bit, by the actively-maintained
 *   open-source Genesis Plus GX VDP core: its register-11 write handler
 *   (`core/vdp_ctrl.c`) sets `hscroll_mask = hscroll_mask_table[d & 0x03]`
 *   from the table `{0x00, 0x07, 0xF8, 0xFF}`, and its per-line horizontal-
 *   scroll fetch (`core/vdp_render.c`) indexes the h-scroll table as
 *   `vram[hscb + ((line & hscroll_mask) << 2)]` -- i.e. each table "row" is
 *   4 bytes (Plane A word then Plane B word, exactly this module's existing
 *   full-screen row layout), and the active on-screen scanline (`line`)
 *   selects which row via `line & hscroll_mask`. For mode `11`
 *   (`hscroll_mask == 0xFF`), every one of this bound surface's 224 H40
 *   scanlines (0-223, all `<= 0xFF`) maps to its own distinct row -- `line &
 *   0xFF == line` -- so this module implements the mode-`11` row selection
 *   simply as `table_base + line * 4 + {0 for Plane A, 2 for Plane B}` (the
 *   general `line & hscroll_mask` rule specialized to this bound surface's
 *   always-`<=223` line range, where the mask has no visible effect). This
 *   module does not implement mode `01`'s `0x07` mask (cyclic repeat every 8
 *   lines) or mode `10`'s `0xF8` mask (new value every 8 lines, never
 *   repeating) -- both remain explicitly out of scope and fail closed.
 *
 * - Horizontal-scroll value interpretation and direction: the stored word's
 *   low 10 bits are a two's-complement signed value (bits above bit 9 are
 *   documented as unused); the value is *subtracted* from the on-screen X
 *   coordinate to obtain the plane-space X coordinate (`plane_x = screen_x -
 *   hscroll`), i.e. increasing the scroll register moves the visible window
 *   rightward across the plane. Source: Genesis Plus GX
 *   (`core/vdp_render.c` full-screen h-scroll path indexes tile columns as
 *   `pf_col_mask + 1 - ((xscroll >> shift) & pf_col_mask)`, a subtraction-
 *   shaped index derivation), consistent with the long-standing, widely
 *   cross-implemented community convention for Genesis/Mega Drive
 *   horizontal scrolling (also stated this way by Charles MacDonald's
 *   `genvdp.txt` notes and reflected identically in BlastEm/PicoDrive).
 *
 * - Vertical-scroll mode and VSRAM organization (H40): register #11 bit 2
 *   (`VSCR`) selects full-plane scroll when clear and 2-cell-column scroll
 *   when set (Plutiedev, "VDP register reference"
 *   <https://www.plutiedev.com/vdp-registers>; MegaDrive Wiki, "VDP"
 *   <https://md.railgun.works/index.php?title=VDP>). In full-plane mode,
 *   Plane A and Plane B use the first and second big-endian VSRAM words
 *   (offsets 0 and 2). SGDK's `src/vdp_bg.c`
 *   <https://raw.githubusercontent.com/Stephane-D/SGDK/master/src/vdp_bg.c>
 *   independently corroborates that Plane-A/Plane-B word ordering. In
 *   2-cell-column mode, VSRAM's
 *   documented 80-byte size (GTO1; already realized as
 *   `GENESIS_VDP_VSRAM_BYTES` in runtime.h) holds exactly 20 four-byte
 *   groups, one per on-screen 2-cell (16px) column group (40 H40 tile
 *   columns / 2); within each group, the first word (offset +0) is Plane
 *   A's vertical-scroll value and the second word (offset +2) is Plane B's,
 *   mirroring the horizontal-scroll table's same Plane-A-then-Plane-B word
 *   order. Source: GTO1 (VSRAM total size) plus Genesis Plus GX
 *   (`core/vdp_render.c`), whose 2-cell vertical-scroll fetch reads
 *   consecutive per-plane VSRAM words indexed by the screen column's 2-cell
 *   group.
 *
 * - Vertical-scroll value interpretation and direction: the stored word's
 *   low 10 bits are a two's-complement signed value (mirroring the
 *   horizontal case); the value is *added* to the on-screen Y coordinate to
 *   obtain the plane-space Y coordinate (`plane_y = screen_y + vscroll`).
 *   Source: Genesis Plus GX (`core/vdp_render.c`: `v_line = (line +
 *   yscroll) & pf_row_mask`), an explicit addition, unlike the horizontal
 *   case's subtraction -- this asymmetry is implemented deliberately, not
 *   normalized away.
 *
 * - Scroll-plus-wrapping interaction: scrolling is applied in plane-pixel
 *   space and then wrapped modulo the decoded plane's pixel width/height
 *   (`plane_width_px = width_cells * 8`, `plane_height_px = height_cells *
 *   8`); this module performs that wrap with a true (non-truncating,
 *   always-non-negative-result) modulo so a negative scrolled coordinate
 *   wraps to the plane's far edge rather than producing an out-of-range or
 *   incorrectly-signed result. This wrap rule is this project's own
 *   direct application of the plane-size fact above (register #16) and the
 *   scroll-direction facts above, not a separately cited hardware source.
 *
 * Scroll-value bit-width policy: this module treats bits above bit 9 of
 * each raw 16-bit scroll word as always zero on the bound title-phase
 * route this checkpoint targets (the documented fields above only assign
 * meaning to the low 10 bits); it does not claim a citable hardware fact
 * about what a real VDP does with software-set higher bits, and callers
 * must not rely on this module for that undocumented case.
 *
 * - Display-mode gating (Mode 5 selection, H40 vs H32 horizontal
 *   resolution, and interlace mode), enforced by the composed
 *   `genesis_vdp_resolve_plane_pixel()` entry point per this file's own
 *   documented bound surface (standard Mode 5, H40, non-interlaced only):
 *   Plutiedev, "VDP register reference"
 *   <https://www.plutiedev.com/vdp-registers>. Register #1 ("Mode Set
 *   Register #2", `$81xx`): bit 2 is documented as always `1` in that
 *   register's bit table for the standard configuration this project
 *   targets -- i.e. Mode 5 (Genesis-native display mode, as opposed to the
 *   legacy Master-System-compatible Mode 4) is selected by that bit being
 *   set. Register #12 ("Mode Set Register #4", `$8Cxx`): bit 7 = `RS0` and
 *   bit 0 = `RS1` jointly select horizontal resolution ("horizontal
 *   resolution in tiles", documented valid combinations `00` = H32 and
 *   `11` = H40 -- both bits must agree); bit 2 = `LSM1` and bit 1 = `LSM0`
 *   jointly select interlace mode ("interlaced mode", documented valid
 *   combinations `00` = none, `01` = interlace mode 1, `11` = interlace
 *   mode 2). This module implements exactly Mode 5 (register #1 bit 2 set),
 *   H40 (register #12 bits 7 and 0 both set), and non-interlaced (register
 *   #12 bits 2 and 1 both clear); any other combination is outside this
 *   checkpoint's documented bound surface and is rejected rather than
 *   guessed.
 */

#define GENESIS_VDP_REG1_MODE5_BIT 0x04U           /* Register #1 bit 2: Mode 5 select. */
#define GENESIS_VDP_REG12_RS0_BIT 0x80U            /* Register #12 bit 7: RS0 (H-res). */
#define GENESIS_VDP_REG12_RS1_BIT 0x01U            /* Register #12 bit 0: RS1 (H-res). */
#define GENESIS_VDP_REG12_LSM1_BIT 0x04U           /* Register #12 bit 2: LSM1 (interlace). */
#define GENESIS_VDP_REG12_LSM0_BIT 0x02U           /* Register #12 bit 1: LSM0 (interlace). */
#define GENESIS_VDP_PLANE_SIZE_RESERVED_FIELD 2U   /* Register #16 field value `10`. */
#define GENESIS_VDP_PLANE_MAX_CELLS 4096U          /* 8KB nametable / 2 bytes-per-cell ceiling. */
#define GENESIS_VDP_TILE_INDEX_MASK 0x07FFU        /* Bits 10-0 of a nametable entry word. */
#define GENESIS_VDP_HSCROLL_VSCROLL_SIGN_BITS 10U  /* Documented low-bit width of h/v scroll values. */
#define GENESIS_VDP_VSRAM_GROUP_BYTES 4U           /* One 2-cell-column group: Plane A word + Plane B word. */
#define GENESIS_VDP_VSRAM_GROUP_COUNT (GENESIS_VDP_VSRAM_BYTES / GENESIS_VDP_VSRAM_GROUP_BYTES)
#define GENESIS_VDP_HSCROLL_MODE_MASK 0x03U         /* Register #11 bits 1-0: H/LSCR field. */
#define GENESIS_VDP_HSCROLL_MODE_FULLSCREEN 0x00U   /* `00`: full scroll. */
#define GENESIS_VDP_HSCROLL_MODE_LINE 0x03U         /* SEG-007-T050: `11`: scroll every line. */
#define GENESIS_VDP_HSCROLL_TABLE_ROW_BYTES 4U      /* One h-scroll table row: Plane A word + Plane B word. */

/* Selects Plane A or Plane B. The Window plane is deliberately not a member
 * of this enum -- it is out of this checkpoint's bound scope (see the file
 * header comment above); a caller cannot even name it through this API. */
typedef enum GenesisVdpPlaneSelector {
  GENESIS_VDP_PLANE_A = 0,
  GENESIS_VDP_PLANE_B = 1
} GenesisVdpPlaneSelector;

/* A decoded Mode-5 nametable entry word (see the "nametable entry word
 * layout" citation above). `tile_index` is 0-2047; `h_flip`/`v_flip`/
 * `priority` are 0 or 1; `palette` is 0-3. This checkpoint only exposes
 * these fields -- composing them into a colored/prioritized pixel is a
 * later checkpoint's job. */
typedef struct GenesisVdpNametableEntry {
  uint16_t tile_index;
  uint8_t h_flip;
  uint8_t v_flip;
  uint8_t palette;
  uint8_t priority;
} GenesisVdpNametableEntry;

/* The fully-resolved location of one screen pixel within one plane: which
 * nametable cell it falls in, the pixel's position local to that cell's
 * 8x8 tile, and the decoded nametable entry occupying that cell. */
typedef struct GenesisVdpPlanePixelLocation {
  unsigned cell_col;
  unsigned cell_row;
  unsigned tile_x;
  unsigned tile_y;
  GenesisVdpNametableEntry entry;
} GenesisVdpPlanePixelLocation;

/*
 * Decodes VDP register #16 (plane size) into a plane's width/height in
 * cells (tiles). Returns 0 and writes `*width_cells_out`/`*height_cells_out`
 * (each 32, 64, or 128) on success. Returns a negative value and leaves the
 * outputs unmodified if either output pointer is NULL, if either field
 * selects the reserved encoding `GENESIS_VDP_PLANE_SIZE_RESERVED_FIELD`, or
 * if the resulting cell count exceeds `GENESIS_VDP_PLANE_MAX_CELLS` -- see
 * the register #16 citation above for why each of those is rejected rather
 * than guessed.
 */
int genesis_vdp_decode_plane_size(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                   unsigned *width_cells_out, unsigned *height_cells_out);

/*
 * Decodes the Plane A (register #2) or Plane B (register #4) nametable base
 * address per the citations above. Returns 0 and writes `*base_addr_out`
 * (always < GENESIS_VDP_VRAM_BYTES, one of the 8 possible `0xE000`-masked
 * multiples of `0x2000`) on success. Returns a negative value and leaves
 * `*base_addr_out` unmodified if `base_addr_out` is NULL or `plane` is not
 * `GENESIS_VDP_PLANE_A`/`GENESIS_VDP_PLANE_B`.
 */
int genesis_vdp_plane_base_address(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                    GenesisVdpPlaneSelector plane, uint32_t *base_addr_out);

/*
 * Decodes one raw 16-bit Mode-5 nametable entry word (already extracted
 * from big-endian VRAM storage into host byte order by the caller) per the
 * documented bit layout cited above.
 */
GenesisVdpNametableEntry genesis_vdp_decode_nametable_word(uint16_t raw_word);

/*
 * Resolves the byte address (into GenesisVdpState.vram) of the nametable
 * entry at zero-based (`cell_col`, `cell_row`) for `plane`, per the
 * documented plane-size and base-address facts above. Returns 0 and writes
 * `*addr_out` on success. Returns a negative value and leaves `*addr_out`
 * unmodified if `addr_out` is NULL, if the plane size or base address
 * cannot be decoded (see the two functions above), if `cell_col` or
 * `cell_row` is out of range for the decoded plane size, or if the computed
 * two-byte entry would read past `GENESIS_VDP_VRAM_BYTES` (untrusted-input
 * bounds check per this project's own input-handling policy).
 */
int genesis_vdp_nametable_cell_address(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                        GenesisVdpPlaneSelector plane, unsigned cell_col,
                                        unsigned cell_row, uint32_t *addr_out);

/*
 * Reads and decodes the nametable entry at zero-based (`cell_col`,
 * `cell_row`) for `plane` out of `vram` (exactly GENESIS_VDP_VRAM_BYTES
 * bytes, big-endian halfword order, matching every other GenesisVdpState
 * buffer's own documented byte order). Returns 0 and writes `*entry_out` on
 * success. Returns a negative value and leaves `*entry_out` unmodified on
 * any of `genesis_vdp_nametable_cell_address`'s own failure conditions, or
 * if `vram` or `entry_out` is NULL.
 */
int genesis_vdp_read_nametable_entry(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                      const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                      GenesisVdpPlaneSelector plane, unsigned cell_col,
                                      unsigned cell_row, GenesisVdpNametableEntry *entry_out);

/*
 * Decomposed low-level register-state checks for this checkpoint's bound
 * display-mode surface (see the "Display-mode gating" citation above). Each
 * function is a pure predicate over `registers[]` and may be called
 * independently; `genesis_vdp_resolve_plane_pixel()` calls the equivalent of
 * all three together so its composed entry point enforces the full bound
 * surface. Each returns nonzero (true) or 0 (false); all return 0 if
 * `registers` is NULL.
 */
int genesis_vdp_mode_is_mode5(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]);
int genesis_vdp_hres_is_h40(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]);
int genesis_vdp_interlace_is_none(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]);

/*
 * Returns nonzero iff register #11's horizontal-scroll-mode bits (1-0)
 * select full-screen mode (`00`) -- one of the two horizontal-scroll modes
 * this checkpoint implements (see the file header comment above).
 */
int genesis_vdp_hscroll_mode_is_fullscreen(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]);

/*
 * SEG-007-T050: returns nonzero iff register #11's horizontal-scroll-mode
 * bits (1-0) select per-scanline ("line") mode (`11`) -- the other of the
 * two horizontal-scroll modes this checkpoint implements (see the file
 * header comment above and its dedicated "line" horizontal-scroll-mode
 * citation for the public sources). Modes `01` ("scroll eight lines, then
 * repeat") and `10` ("scroll every tile") are deliberately NOT recognized by
 * either this predicate or `genesis_vdp_hscroll_mode_is_fullscreen` above --
 * both remain out of this checkpoint's bound surface and fail closed.
 */
int genesis_vdp_hscroll_mode_is_line(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]);

/*
 * Returns nonzero iff register #11's vertical-scroll-mode bit (2) selects
 * per-2-cell-column mode (`1`). A clear bit selects the supported full-plane
 * mode (see the file header comment above).
 */
int genesis_vdp_vscroll_mode_is_2cell(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT]);

/*
 * Reads and sign-extends (per the documented low-10-bit two's-complement
 * layout cited above) the horizontal-scroll value for `plane` at on-screen
 * scanline `screen_y` (0-based) out of `vram`'s horizontal-scroll table
 * (register #13). In full-screen mode (`genesis_vdp_hscroll_mode_is_
 * fullscreen`), `screen_y` is accepted but not used to select the table row
 * -- row 0 (table_base + 0/+2) is always used, exactly as before
 * SEG-007-T050. In line mode (SEG-007-T050,
 * `genesis_vdp_hscroll_mode_is_line`), the table row is selected by
 * `screen_y` itself (`table_base + screen_y * 4 + {0 for Plane A, 2 for
 * Plane B}`, this checkpoint's bound-surface specialization of Genesis Plus
 * GX's general `line & hscroll_mask` rule -- see the dedicated "line"
 * horizontal-scroll-mode citation above). Returns 0 and writes `*value_out`
 * (range -512..511) on success. Returns a negative value and leaves
 * `*value_out` unmodified if `vram` or `value_out` is NULL, if `plane` is
 * not `GENESIS_VDP_PLANE_A`/`GENESIS_VDP_PLANE_B`, if neither
 * `genesis_vdp_hscroll_mode_is_fullscreen` nor `genesis_vdp_hscroll_mode_
 * is_line` is true (unsupported scroll mode -- fails closed rather than
 * guessing), if line mode's `screen_y` is outside this renderer's display
 * range `[0, GENESIS_FRAME_HEIGHT)`, or if the relevant table entry would read past
 * `GENESIS_VDP_VRAM_BYTES`.
 */
int genesis_vdp_read_hscroll(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                              const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                              GenesisVdpPlaneSelector plane, unsigned screen_y,
                              int32_t *value_out);

/*
 * Reads and sign-extends the vertical-scroll value for `plane` out of `vsram`.
 * Full-plane mode (register #11 bit 2 clear) reads Plane A/B from offsets 0/2
 * and ignores `screen_cell_col`. Per-2-cell-column mode (bit set) reads the
 * group containing the on-screen tile column (group = screen_cell_col / 2).
 * Returns 0 and writes `*value_out` (range -512..511) on success. Returns a
 * negative value and leaves `*value_out` unmodified if `vsram` or
 * `value_out` is NULL, if `plane` is not `GENESIS_VDP_PLANE_A`/
 * `GENESIS_VDP_PLANE_B`, or if per-2-cell mode's resulting group index is >=
 * GENESIS_VDP_VSRAM_GROUP_COUNT (this
 * checkpoint's bound H40 surface has exactly
 * GENESIS_VDP_VSRAM_GROUP_COUNT == 20 column groups; a wider request fails
 * closed rather than guessing a hardware VSRAM-address-wrap behavior this
 * checkpoint does not implement).
 */
int genesis_vdp_read_vscroll(const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                              const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                              GenesisVdpPlaneSelector plane, unsigned screen_cell_col,
                              int32_t *value_out);

/*
 * Applies a signed horizontal-scroll value to an on-screen X coordinate and
 * wraps the result into plane-pixel space, per the documented subtraction
 * direction and wrap rule cited above. `plane_width_px` must be > 0
 * (guaranteed by any value `genesis_vdp_decode_plane_size` can produce).
 */
unsigned genesis_vdp_plane_x(unsigned screen_x, int32_t hscroll, unsigned plane_width_px);

/*
 * Applies a signed vertical-scroll value to an on-screen Y coordinate and
 * wraps the result into plane-pixel space, per the documented addition
 * direction and wrap rule cited above. `plane_height_px` must be > 0
 * (guaranteed by any value `genesis_vdp_decode_plane_size` can produce).
 */
unsigned genesis_vdp_plane_y(unsigned screen_y, int32_t vscroll, unsigned plane_height_px);

/*
 * Resolves the complete plane-space pixel location (nametable cell,
 * tile-local pixel position, and decoded nametable entry) for one on-screen
 * (`screen_x`, `screen_y`) pixel on `plane`, applying this checkpoint's bound
 * full-screen-or-line horizontal / full-plane-or-per-2-cell-column vertical scrolling and
 * wrapping per every fact cited above (`screen_y` also selects the
 * horizontal-scroll table row in line mode -- see
 * `genesis_vdp_read_hscroll` above). This is the composed entry point;
 * it performs no work `genesis_vdp_decode_plane_size` /
 * `genesis_vdp_plane_base_address` / `genesis_vdp_read_hscroll` /
 * `genesis_vdp_read_vscroll` / `genesis_vdp_plane_x` / `genesis_vdp_plane_y`
 * / `genesis_vdp_read_nametable_entry` do not already do individually.
 *
 * Returns 0 and writes `*location_out` on success. Returns a negative value
 * and leaves `*location_out` unmodified if any pointer argument is NULL, if
 * `plane` is not `GENESIS_VDP_PLANE_A`/`GENESIS_VDP_PLANE_B`, if the
 * display-mode state does not select this checkpoint's bound surface (Mode
 * 5, H40, non-interlaced -- see `genesis_vdp_mode_is_mode5` /
 * `genesis_vdp_hres_is_h40` / `genesis_vdp_interlace_is_none` above), or if
 * any of the above helper calls this function makes internally fails (an
 * unsupported/reserved plane size, an unsupported scroll mode, or an
 * out-of-range VSRAM column group) -- this function never falls back to a
 * guessed default for a failure it detects.
 */
int genesis_vdp_resolve_plane_pixel(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                     const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                                     const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                     GenesisVdpPlaneSelector plane, unsigned screen_x,
                                     unsigned screen_y, GenesisVdpPlanePixelLocation *location_out);

/*
 * SEG-007-T049 checkpoint C3: sprite-attribute-table decoding and placement.
 *
 * This checkpoint stays inside the same bound surface C2 already established
 * (standard Mode 5, H40 (320px) display, non-interlaced; see the
 * "Display-mode gating" citation above -- `genesis_vdp_resolve_plane_pixel`'s
 * own callers are expected to have already checked
 * `genesis_vdp_mode_is_mode5` / `genesis_vdp_hres_is_h40` /
 * `genesis_vdp_interlace_is_none` before using any C3 function below; C3's
 * own functions do not re-check display mode themselves because, unlike C2's
 * plane addressing, nothing about SAT decoding/placement below actually
 * depends on H32-vs-H40 *except* the SAT base-address multiplier and the
 * 80-sprite H40 traversal cap, both called out explicitly where used).
 *
 * Public sources (bit-exact where cited; a genuine two-source disagreement
 * is called out explicitly below and resolved, matching this file's own C2
 * citation style):
 *
 * - Sprite Attribute Table (SAT) base address (register #5): in H40 mode,
 *   the low address bit (register #5 bit 0, "AT9") is IGNORED -- the H40 SAT
 *   base has 0x400-byte (1024-byte) granularity, not 0x200-byte, computed as
 *   `(reg5 << 9) & 0xFC00` (bit 0's contribution at result bit 9 is masked
 *   off). This checkpoint implements only the H40 case per its bound
 *   surface, and fails closed (see `genesis_vdp_sat_base_address` below) when
 *   H40 is not selected, rather than guessing an H32 1024-byte-vs-512-byte
 *   distinction that is out of scope here. Source (direct, unambiguous): the
 *   actively-maintained open-source Genesis Plus GX VDP core
 *   (`core/vdp_ctrl.c`, register-5 write handler), whose H40-mode base-
 *   address mask discards the low address bit contributed by register #5
 *   bit 0, yielding the implemented `(reg5 << 9) & 0xFC00` 1024-byte
 *   SAT-base granularity in H40. **Discrepancy noted:** Plutiedev's "VDP
 *   register reference" <https://www.plutiedev.com/vdp-registers> (register
 *   `$85`, "Sprite Table") states "Bit 6-0: ST15-9... divided by $200... In
 *   32-cell mode, bit 0 is ignored" -- that wording documents bit 0 being
 *   ignored only for the 32-cell (H32) case and does not itself state
 *   anything about H40's coarser alignment; it does not unambiguously
 *   corroborate this exact H40 masking rule, so it is not relied on for that
 *   specific claim here. It remains a useful source for the SAT's other
 *   documented facts below (entry layout, H40 sprite/byte capacity), where
 *   its wording is not in question. In H40, the SAT occupies exactly
 *   `GENESIS_VDP_SAT_H40_MAX_SPRITES * GENESIS_VDP_SPRITE_ENTRY_BYTES` = 80 *
 *   8 = 640 bytes starting at that base address: Plutiedev, "Sprites"
 *   <https://www.plutiedev.com/sprites> ("the VDP can display up to 80
 *   sprites on the H40 mode... Each sprite uses 8 bytes in the table").
 *
 * - Sprite attribute entry layout (8 bytes per sprite, 4 big-endian 16-bit
 *   words): Plutiedev, "Sprites" <https://www.plutiedev.com/sprites>,
 *   corroborated bit-for-bit by Genesis Plus GX's sprite-attribute read code
 *   (`core/vdp_render.c`, `object_info_t` population from raw SAT bytes):
 *     - Word 0 (offset +0): bits 8-0 = Y position (raw, unsigned, 0-511);
 *       bits 15-9 unused/not interpreted for this bounded non-interlaced
 *       Mode-5 surface (interlace mode 2's different, wider Y-coordinate
 *       semantics are out of scope here -- see this checkpoint's Non-goals).
 *     - Byte 2 (offset +2): bits 3-2 = width-in-cells-minus-1 (0-3 -> 1-4
 *       cells); bits 1-0 = height-in-cells-minus-1 (0-3 -> 1-4 cells); bits
 *       7-4 unused/reserved.
 *     - Byte 3 (offset +3): bits 6-0 = link field (the *index*, 0-127, of
 *       the next sprite to process in SAT traversal order; 0 terminates the
 *       list); bit 7 unused/reserved. **Note on field width:** this field is
 *       implemented here as 7 bits (mask `0x7F`), matching both Plutiedev's
 *       "Sprites" page and Genesis Plus GX's own `link = temp[3] & 0x7F`
 *       decode -- 7 bits is also the only width that can represent every
 *       valid H40 sprite index 0-79, since 6 bits could only reach 0-63.
 *     - Word 4 (offset +4): bit 15 = priority; bits 14-13 = palette select
 *       (0-3); bit 12 = vertical flip; bit 11 = horizontal flip; bits 10-0 =
 *       tile index (0-2047) -- the identical Mode-5 "tile ID" word layout
 *       C2 already documents and decodes via
 *       `genesis_vdp_decode_nametable_word` above (Plutiedev, "Tile ID
 *       flags"); this checkpoint reuses that exact bit layout for a
 *       sprite's own pattern-tile word.
 *     - Word 6 (offset +6): bits 8-0 = X position (raw, unsigned, 0-511);
 *       bits 15-9 unused/reserved.
 *
 * - The +128 X/Y coordinate offset convention: both the raw Y (word 0) and
 *   raw X (word 6) values above are stored with a documented +128 bias --
 *   the real (screen-relative, signed) coordinate is `raw_value - 128` --
 *   so that a sprite may be positioned partially or fully off the top-left
 *   edge of the visible display (a real negative on-screen coordinate) while
 *   still being stored as an unsigned SAT field. Source: Plutiedev,
 *   "Sprites" ("Sprite position... As with tiles, the position is measured
 *   from a point 128 pixels to the up-left of the display, meaning that a
 *   sprite exactly at the top-left of the screen has a position of
 *   (128, 128)"); independently corroborated by Charles MacDonald's
 *   long-standing "Genesis VDP" hardware notes (`genvdp.txt`, sprite table
 *   section, documenting the same "subtract 128" rule for both X and Y) and
 *   by Genesis Plus GX (`core/vdp_render.c`), which computes each sprite's
 *   working X/Y as the raw SAT field minus 128 before any further
 *   processing. This project treats this as a well-established, precisely
 *   cited hardware fact, not a compatibility guess -- and explicitly calls
 *   it out here because an off-by-N error against this exact constant is a
 *   commonly documented pitfall.
 *
 * - Multi-cell sprite tile traversal order (column-major): for a sprite
 *   wider or taller than one cell, its `tile_index` names only the
 *   *first* (top-left, pre-flip) cell's tile; consecutive VRAM tile indices
 *   fill the remaining cells column-by-column (all cells of the first
 *   column, top-to-bottom, then all cells of the second column, and so on),
 *   not row-by-row. Source: Plutiedev, "Sprites"
 *   <https://www.plutiedev.com/sprites> (worked example: a 2-cell-wide,
 *   3-cell-tall sprite's six tiles are laid out in VRAM as columns `[0,1,2]`
 *   then `[3,4,5]`, i.e. tile-at-cell(col, row) = `tile_index + col *
 *   height_cells + row`); independently corroborated by Genesis Plus GX
 *   (`core/vdp_render.c` sprite-tile-address computation, which indexes
 *   consecutive tiles by `column * height_in_cells + row` for an unflipped
 *   sprite). H/V flip is applied by mirroring the sprite's whole on-screen
 *   pixel bounding box *before* this column-major cell/tile-local-pixel
 *   split (see `genesis_vdp_resolve_sprite_pixel` below) -- flip changes
 *   which on-screen pixel maps to which stored cell/row/column, but the
 *   underlying column-major storage order itself never changes.
 *
 * - SAT linked-list traversal order: sprite index 0 is always the first
 *   sprite processed; each processed sprite's `link` field (see above)
 *   names the *index* (not a byte address) of the next sprite to process;
 *   `link == 0` documents list termination. Source: Plutiedev, "Sprites"
 *   ("sprites are ordered in a linked list... starting with sprite 0...
 *   Each sprite has a link to the next one... A link value of 0 signifies
 *   the end of the list"); independently corroborated by Genesis Plus GX's
 *   own SAT-traversal loop (`core/vdp_render.c`, sprite parsing loop:
 *   `link = object_info[i].link; if (!link) break;`).
 *
 *   **Project-added defensive bound (not itself a hardware citation):**
 *   the cited sources describe well-formed SAT content; they do not
 *   describe traversal behavior for adversarial/corrupted SAT content that
 *   forms a `link` cycle never revisiting index 0 (e.g. sprite A links to
 *   B, B links back to A). Per this project's own untrusted-input policy
 *   (the project charter: "Treat input files as untrusted... reject ambiguity
 *   explicitly"), `genesis_vdp_sat_traverse` below caps total visited
 *   sprites at `GENESIS_VDP_SAT_H40_MAX_SPRITES` (80, the maximum possible
 *   in H40 per the SAT-size citation above) *and* independently stops the
 *   moment an already-visited index would be revisited, so a
 *   never-quite-hits-zero cycle in untrusted VRAM content cannot cause an
 *   unbounded traversal. This bound is this project's own compatibility
 *   policy layered on top of the cited hardware traversal rule, not a
 *   claim about real hardware's own behavior on cyclic SAT content (real
 *   hardware traversal timing/behavior on such content is not addressed by
 *   any source located here and is out of this checkpoint's scope).
 */

#define GENESIS_VDP_SPRITE_ENTRY_BYTES 8U       /* 4 big-endian 16-bit words per sprite. */
#define GENESIS_VDP_SAT_H40_MAX_SPRITES 80U     /* H40 SAT capacity (640 / 8). */
#define GENESIS_VDP_SAT_H40_BYTES \
  (GENESIS_VDP_SAT_H40_MAX_SPRITES * GENESIS_VDP_SPRITE_ENTRY_BYTES) /* 640. */
#define GENESIS_VDP_SPRITE_COORD_BIAS 128       /* Documented +128 X/Y coordinate offset. */
#define GENESIS_VDP_SPRITE_LINK_MASK 0x7FU      /* Byte +3 bits 6-0: link field. */
#define GENESIS_VDP_SPRITE_Y_MASK 0x01FFU       /* Word +0 bits 8-0: raw Y (9 bits). */
#define GENESIS_VDP_SPRITE_X_MASK 0x01FFU       /* Word +6 bits 8-0: raw X. */
#define GENESIS_VDP_REG5_SAT_BASE_H40_MASK 0xFC00U /* H40 SAT base address mask (1024-byte granularity). */

/* One fully decoded Mode-5 sprite-attribute-table entry. `x`/`y` are already
 * offset-corrected (raw SAT field minus GENESIS_VDP_SPRITE_COORD_BIAS) signed
 * screen-relative coordinates -- they may be negative for a sprite placed
 * off the top/left edge of the visible display. `width_cells`/`height_cells`
 * are each 1-4. `link` is the raw 0-127 next-sprite index (0 = end of list).
 * `tile_index` (0-2047), `h_flip`/`v_flip`/`priority` (0 or 1), and `palette`
 * (0-3) mirror `GenesisVdpNametableEntry`'s own field meanings and naming --
 * see the nametable-entry citation above; this checkpoint only exposes
 * `priority`, it does not yet use it for composition (that is checkpoint
 * C4). */
typedef struct GenesisVdpSpriteEntry {
  int32_t x;
  int32_t y;
  unsigned width_cells;
  unsigned height_cells;
  unsigned link;
  uint16_t tile_index;
  uint8_t h_flip;
  uint8_t v_flip;
  uint8_t palette;
  uint8_t priority;
} GenesisVdpSpriteEntry;

/*
 * Decodes the H40 Sprite Attribute Table base address from register #5, per
 * the citation above. Returns 0 and writes `*base_addr_out` (always <
 * GENESIS_VDP_VRAM_BYTES, a multiple of 1024) on success. Returns a negative
 * value and leaves `*base_addr_out` unmodified if `registers` or
 * `base_addr_out` is NULL, or if `genesis_vdp_hres_is_h40(registers)` is
 * false (this checkpoint's SAT layout/traversal assumes the 80-entry H40
 * SAT; a non-H40 display-mode state fails closed here rather than guessing
 * an H32 SAT base/size this checkpoint does not implement).
 */
int genesis_vdp_sat_base_address(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                  uint32_t *base_addr_out);

/*
 * Decodes one raw 8-byte Mode-5 sprite-attribute-table entry (`raw`, exactly
 * GENESIS_VDP_SPRITE_ENTRY_BYTES bytes, big-endian halfword order matching
 * every other GenesisVdpState buffer) per the documented byte/bit layout and
 * +128 coordinate-offset convention cited above.
 */
GenesisVdpSpriteEntry genesis_vdp_decode_sprite_entry(
    const uint8_t raw[GENESIS_VDP_SPRITE_ENTRY_BYTES]);

/*
 * Reads and decodes sprite `sprite_index` (0-based, must be <
 * GENESIS_VDP_SAT_H40_MAX_SPRITES) directly out of `vram` and `registers`.
 * Returns 0 and writes `*entry_out` on success. Returns a negative value and
 * leaves `*entry_out` unmodified if `vram`, `registers`, or `entry_out` is
 * NULL, if `sprite_index >= GENESIS_VDP_SAT_H40_MAX_SPRITES`, if the SAT base
 * address cannot be decoded (see `genesis_vdp_sat_base_address`, which also
 * fails closed when H40 is not selected), or if the
 * computed 8-byte entry would read past `GENESIS_VDP_VRAM_BYTES`
 * (untrusted-input bounds check per this project's own input-handling
 * policy).
 */
int genesis_vdp_read_sprite_entry(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                   const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                   unsigned sprite_index, GenesisVdpSpriteEntry *entry_out);

/*
 * Traverses the SAT's documented linked list starting at sprite index 0
 * (per the traversal-order citation above), writing each visited sprite
 * index, in visitation order, into `visited_out` (an array of at least
 * GENESIS_VDP_SAT_H40_MAX_SPRITES elements) and the number of indices
 * written into `*visited_count_out`.
 *
 * Traversal stops (successfully) when a visited sprite's `link` field is 0
 * (documented end-of-list), when the next `link` value would name an index
 * already present in `visited_out` (project-added cycle defense, see the
 * traversal-order citation above -- this includes a `link` that points back
 * to index 0, since index 0 is always visited first), when the next `link`
 * value is `>= GENESIS_VDP_SAT_H40_MAX_SPRITES` (out-of-range for this H40
 * checkpoint's SAT; treated as an implicit end-of-list rather than followed),
 * or when `GENESIS_VDP_SAT_H40_MAX_SPRITES` sprites have been visited
 * (defensive cap, matching H40's own maximum sprite count).
 *
 * Returns 0 and writes `*visited_out`/`*visited_count_out` on success (this
 * includes the cycle-defense and out-of-range-link stopping cases above --
 * neither is treated as an error, since both terminate the traversal safely
 * with a well-defined, bounded result). Returns a negative value and leaves
 * the outputs unmodified if `vram`, `registers`, `visited_out`, or
 * `visited_count_out` is NULL, or if reading sprite index 0 itself fails
 * (see `genesis_vdp_read_sprite_entry`).
 */
int genesis_vdp_sat_traverse(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                              const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                              unsigned visited_out[GENESIS_VDP_SAT_H40_MAX_SPRITES],
                              unsigned *visited_count_out);

/*
 * Returns nonzero (true) iff on-screen pixel (`screen_x`, `screen_y`) falls
 * within `sprite`'s placed rectangle: `sprite->x <= screen_x <
 * sprite->x + sprite->width_cells * 8` and the equivalent for Y. Basic
 * screen-bounds clipping per this checkpoint's scope: a caller-supplied
 * (`screen_x`, `screen_y`) is compared against the sprite's own signed,
 * offset-corrected rectangle, so a sprite placed off-screen (negative
 * `x`/`y`, or extending past whatever visible-area width/height the caller
 * uses) simply yields 0 (not covered) for a query outside that rectangle --
 * no hardware sprite-per-line/overflow limit is applied (see this
 * checkpoint's own Non-goals). Returns 0 if `sprite` is NULL.
 */
int genesis_vdp_sprite_covers_pixel(const GenesisVdpSpriteEntry *sprite, unsigned screen_x,
                                     unsigned screen_y);

/*
 * Resolves the decoded palette index (0-15) of `sprite`'s pixel at on-screen
 * (`screen_x`, `screen_y`), applying H/V flip (by mirroring the sprite's
 * whole pixel bounding box before splitting into cell/tile-local-pixel
 * coordinates) and the documented column-major multi-cell tile order (see
 * the citation above), then delegating to this file's existing C1
 * `genesis_vdp_decode_tile_pixel` for the final per-pixel nibble decode.
 *
 * Returns 0 and writes `*index_out` on success. Returns a negative value and
 * leaves `*index_out` unmodified if `sprite`, `vram`, or `index_out` is
 * NULL, if `genesis_vdp_sprite_covers_pixel` is false for this
 * (`screen_x`, `screen_y`) (screen-bounds clipping -- this function never
 * reads VRAM for an uncovered query), or if the resolved tile's 32-byte
 * pattern data would read past `GENESIS_VDP_VRAM_BYTES` (untrusted-input
 * bounds check per this project's own input-handling policy). Note: the
 * effective sprite tile number (`tile_index` plus the column-major cell
 * offset) is masked to the 11-bit Mode-5 tile-index range
 * (`GENESIS_VDP_TILE_INDEX_MASK`) before this bounds check, so an
 * overflowing sum wraps modulo 2048 rather than being rejected -- see the
 * column-major-order citation above for the cited source of this wrap
 * behavior.
 */
int genesis_vdp_resolve_sprite_pixel(const GenesisVdpSpriteEntry *sprite,
                                      const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                                      unsigned screen_x, unsigned screen_y, uint8_t *index_out);

/*
 * SEG-007-T049 checkpoint C4: priority/transparency/composition.
 *
 * This checkpoint composes C1 (tile/palette decode), C2 (Plane A/B pixel
 * resolution), and C3 (sprite decoding/placement/traversal) into one final
 * per-pixel color, per this checkpoint's own bound surface (the same Mode-5/
 * H40/non-interlaced/Plane-A-or-B/no-Window surface C2 and C3 already
 * establish -- see their own header comments above). It reimplements no
 * plane/sprite addressing or tile/CRAM decode logic; it only calls the
 * existing C1/C2/C3 functions and combines their results.
 *
 * Public sources (bit-exact where cited; matching this file's own C1-C3
 * citation style):
 *
 * - Palette index 0 is never opaque: for both a plane pattern pixel and a
 *   sprite pattern pixel, a decoded palette index of 0 is transparent
 *   (it never contributes a color to the composed result), regardless of
 *   which of the 4 palettes (0-3) is selected. Source: Plutiedev, "Layers"
 *   <https://www.plutiedev.com/backgrounds> / "Sprites"
 *   <https://www.plutiedev.com/sprites> ("Color 0 in each palette line is
 *   always transparent" -- stated identically for both plane and sprite
 *   pattern data); independently corroborated by the actively-maintained
 *   open-source Genesis Plus GX VDP core (`core/vdp_render.c`), whose plane
 *   and sprite pixel-write paths both special-case a decoded pattern value
 *   of 0 as "no pixel drawn" before any priority/palette lookup.
 *
 * - Mode-5 priority/compositing order (six distinct precedence layers,
 *   highest to lowest): (1) a sprite pixel whose entry's priority bit is
 *   set, (2) a Plane A pixel whose entry's priority bit is set, (3) a
 *   Plane B pixel whose entry's priority bit is set, (4) a sprite pixel
 *   whose priority bit is clear, (5) a Plane A pixel whose priority bit is
 *   clear, (6) a Plane B pixel whose priority bit is clear; the backdrop
 *   color is used only when no plane or sprite contributes an opaque pixel
 *   at any of these six layers. Source: Plutiedev, "Layers"
 *   <https://www.plutiedev.com/backgrounds> (documenting exactly this
 *   "S H, A H, B H, S L, A L, B L" six-layer high/low interleaving between
 *   sprites and the two planes, sprite ahead of both planes within each
 *   priority level); independently corroborated by the actively-maintained
 *   open-source Genesis Plus GX VDP core (`core/vdp_render.c`, the
 *   `LINEBUF`/priority-compositing path that blends the previously-rendered
 *   sprite line buffer over each plane's own high/low-priority tile output
 *   in this same S>A>B ordering per priority level, and re-applies the same
 *   ordering for the opposite priority level beneath it). This module
 *   implements exactly this six-layer order; it does not implement
 *   shadow/highlight mode (see this checkpoint's Non-goals).
 *
 * - Sprite-overlap precedence: when two or more sprites' opaque pixels both
 *   cover the same on-screen coordinate, the sprite visited earlier in SAT
 *   linked-list traversal order (see C3's `genesis_vdp_sat_traverse`) wins,
 *   regardless of either sprite's own priority bit -- sprite priority does
 *   NOT reorder sprites relative to each other; only the single surviving
 *   (earliest-traversal-order, opaque) sprite candidate's own priority bit
 *   is later compared against Plane A/B (see the six-layer order above). A
 *   later sprite's opaque pixel at that coordinate is never drawn over an
 *   earlier sprite's already-opaque pixel, even if the later sprite's
 *   priority bit is set and the earlier sprite's is clear. Source: Plutiedev,
 *   "Sprites" <https://www.plutiedev.com/sprites>
 *   ("sprites are drawn in link order, and the ones drawn first will appear
 *   above the ones drawn later" -- i.e. earlier-drawn/earlier-traversed
 *   sprites occlude later ones); independently corroborated by Genesis Plus
 *   GX (`core/vdp_render.c` sprite-rendering loop), which iterates the SAT
 *   link chain in traversal order and only writes a sprite pixel into its
 *   line buffer when that buffer position has not already been written by
 *   an earlier sprite in the same pass.
 *
 * - Palette-plus-index-within-palette CRAM addressing: a decoded (palette
 *   0-3, index-within-palette 0-15) pair addresses CRAM entry
 *   `palette * 16 + index` (0-63), the same 64-entry space C1's
 *   `genesis_vdp_decode_cram_entry` already indexes. Source: Sega
 *   Enterprises, *Genesis Technical Overview* v1.00 (1991) p. 32 ("64 x
 *   9-bits of CRAM" organized as 4 palette lines of 16 colors each);
 *   independently corroborated by Plutiedev, "Tile ID flags"
 *   <https://www.plutiedev.com/tile-id> (documenting the nametable entry's
 *   2-bit palette-select field choosing "one of 4 palette lines" of 16
 *   colors) and by Genesis Plus GX (`core/vdp_render.c`), whose pattern
 *   lookup indexes `pixel_data | (palette << 4)` directly into the 64-entry
 *   CRAM-derived color table -- i.e. `palette << 4` is exactly
 *   `palette * 16`.
 *
 * - Backdrop/background color (register #7): bits 5-4 select one of the 4
 *   palette lines and bits 3-0 select the index (0-15) within that palette
 *   line; combined into a CRAM entry index via the same
 *   `palette * 16 + index` formula above. Used only when no plane or
 *   sprite contributes an opaque pixel at the queried coordinate. Source:
 *   Plutiedev, "VDP register reference" <https://www.plutiedev.com/vdp-registers>
 *   (register `$87`, "Background Color", "Bit 5-4: Palette... Bit 3-0:
 *   Color"); independently corroborated by Genesis Plus GX
 *   (`core/vdp_ctrl.c`/`core/vdp_render.c` register-7 handling, which reads
 *   the same two bit-fields to select the backdrop's CRAM entry via
 *   `(reg7 & 0x3F)` -- already exactly `palette * 16 + index` for a 2-bit
 *   palette and 4-bit index packed into one byte).
 *
 * Non-goals (see this checkpoint's own Scope/Non-goals in the task record):
 * the Window plane, shadow/highlight mode, sprite-per-line/overflow
 * emulation, and any frame-artifact/output-schema concern (that is
 * checkpoint C5, not this one).
 */

#define GENESIS_VDP_TRANSPARENT_PALETTE_INDEX 0U /* Palette index 0 is always transparent. */
#define GENESIS_VDP_PALETTE_LINE_COLORS 16U      /* Colors per palette line; CRAM index = palette*16+index. */
#define GENESIS_VDP_REG7_BACKDROP_PALETTE_SHIFT 4U /* Register #7 bits 5-4: backdrop palette select. */
#define GENESIS_VDP_REG7_BACKDROP_MASK 0x3FU       /* Register #7 bits 5-0: palette(2) + index(4). */

/* One resolved, not-yet-composited candidate pixel: whether it is opaque,
 * its priority bit, and its (palette, index) pair (only meaningful when
 * opaque). Used internally by `genesis_vdp_compose_pixel` for each of Plane
 * A, Plane B, and the winning sprite (if any) at one screen coordinate. */
typedef struct GenesisVdpCandidatePixel {
  uint8_t opaque;
  uint8_t priority;
  uint8_t palette;
  uint8_t index;
} GenesisVdpCandidatePixel;

/*
 * Combines a decoded (palette 0-3, index-within-palette 0-15) pair into a
 * single CRAM entry index (0-63) per the documented `palette * 16 + index`
 * addressing cited above. Behavior is undefined by contract only in the
 * sense that this is a pure arithmetic combination; callers are expected to
 * pass already-range-checked `palette`/`index` values (both fields are
 * always in range when they come from `genesis_vdp_decode_nametable_word`,
 * `genesis_vdp_decode_sprite_entry`, or `genesis_vdp_decode_tile_pixel`).
 */
unsigned genesis_vdp_cram_index(unsigned palette, unsigned index);

/*
 * Decodes register #7's backdrop/background color selection into a CRAM
 * entry index (0-63) per the documented bit layout cited above. Returns 0
 * and writes `*cram_index_out` on success. Returns a negative value and
 * leaves `*cram_index_out` unmodified if `registers` or `cram_index_out` is
 * NULL.
 */
int genesis_vdp_backdrop_cram_index(const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                                     unsigned *cram_index_out);

/*
 * Resolves and composes the final visible pixel at on-screen (`screen_x`,
 * `screen_y`) from Plane A, Plane B, and every SAT-traversed sprite, per the
 * six-layer priority order and palette-index-0-transparency rule cited
 * above, then decodes the winning (palette, index) pair (or the register #7
 * backdrop when nothing is opaque) into a concrete color via C1's
 * `genesis_vdp_decode_cram_entry`.
 *
 * This function performs no plane/sprite addressing or tile/CRAM decode
 * work itself: it calls `genesis_vdp_resolve_plane_pixel` (C2),
 * `genesis_vdp_decode_tile_pixel` (C1) against the resolved plane entry's
 * tile, `genesis_vdp_sat_traverse` + `genesis_vdp_resolve_sprite_pixel` (C3),
 * and `genesis_vdp_decode_cram_entry` (C1) for the final color lookup.
 *
 * Returns 0 and writes `*color_out` on success. Returns a negative value and
 * leaves `*color_out` unmodified if any pointer argument is NULL, if the
 * display-mode/plane-size/scroll-mode state does not satisfy this
 * checkpoint's bound surface (any failure `genesis_vdp_resolve_plane_pixel`
 * itself would report for either plane), or if SAT traversal itself fails
 * (see `genesis_vdp_sat_traverse`) -- this function never falls back to a
 * guessed default for a failure it detects.
 */
int genesis_vdp_compose_pixel(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                               const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                               const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                               const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                               unsigned screen_x, unsigned screen_y, GenesisRgb888 *color_out);

/*
 * SEG-007-T049 checkpoint C5: deterministic frame output.
 *
 * This checkpoint applies -- and never redefines -- the frame-artifact
 * schema already committed by SEG-007-T042
 * (docs/architecture/genesis-persistent-device-state-and-checkpoint-evidence-contract.md
 * section 12.5); its exact C type is `GenesisFrameArtifact`
 * (platforms/genesis/runtime/checkpoint_evidence.h). It reimplements no plane/sprite
 * addressing, tile/CRAM decode, or priority/composition logic of its own: it
 * only calls C4's `genesis_vdp_compose_pixel` for every one of the schema's
 * 320x224 screen pixels (row-major, top-left origin) and C1's
 * `genesis_vdp_cram_index` to convert the winning (palette, index) pair into
 * the schema's required single palette-index byte (0-63), then reuses this
 * runtime's existing shared SHA-256 implementation
 * (`genesis_sha256_init`/`_update`/`_final`, runtime.h) to compute
 * `frame_digest` over `pixels` then `palette_snapshot`, in that exact field
 * order (matching the schema's own documented digest-order requirement).
 *
 * This function is fully generic over `GenesisVdpState`-shaped inputs -- no
 * ROM-specific addresses, title-screen constants, or screenshot-specific
 * special cases. It respects the same bound renderer surface C2/C3/C4 already
 * enforce (Mode 5, H40, non-interlaced, Plane A/B, sprites, no Window, no
 * shadow/highlight, no sprite-overflow/per-line emulation): if
 * `genesis_vdp_compose_pixel` fails for any one of the 320x224 pixels (e.g.
 * because register state is outside that bound surface), this function fails
 * closed for the WHOLE frame rather than emitting a partial/misleading
 * artifact.
 *
 * Returns 0 and writes the fully populated `*frame_out` (pixels,
 * palette_snapshot, and frame_digest all set) on success. Returns a negative
 * value and leaves `*frame_out` completely unmodified -- not partially
 * filled -- if `vram`, `vsram`, `cram`, `registers`, or `frame_out` is NULL,
 * or if `genesis_vdp_compose_pixel` fails for any queried pixel.
 */
int genesis_vdp_produce_frame(const uint8_t vram[GENESIS_VDP_VRAM_BYTES],
                               const uint8_t vsram[GENESIS_VDP_VSRAM_BYTES],
                               const uint8_t cram[GENESIS_VDP_CRAM_BYTES],
                               const uint16_t registers[GENESIS_VDP_REGISTER_COUNT],
                               GenesisFrameArtifact *frame_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SEGARECOMP_RUNTIME_GENESIS_VDP_RENDER_H */
