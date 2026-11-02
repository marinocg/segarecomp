#ifndef SEGARECOMP_GENESIS_SONIC1_WIDESCREEN_H
#define SEGARECOMP_GENESIS_SONIC1_WIDESCREEN_H

/*
 * SEG-124 (ADR 0267): the verified native widescreen compatibility provider for Sonic the Hedgehog (Genesis, REV00, original image SHA-256
 * 46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6), capability "genesis.sonic1.native-widescreen.v1".
 *
 * It reads a bounded, read-only work-RAM snapshot (see native_compat.h) and:
 *   scenery: derives the authentic name-table word of the world cell beneath an off-screen column from the level layout, the 256x256 chunk table and the
 *            16x16 block table the game itself decompressed into work RAM (never from the wrapped plane nametable);
 *   hud:     draws a compact ring/lives panel.
 * Verified scope is stated in docs/product/native-widescreen-sonic1.md; any scene outside it is declined (generic result).
 */
#include "native_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SONIC1_NATIVE_CAPABILITY "genesis.sonic1.native-widescreen.v1"
#define SONIC1_NATIVE_IMPLEMENTATION 1U

/* The provider for the capability and its (static) state; NULL for any other id. */
const GenesisNativeCompat *sonic1_widescreen_provider(void **state_out);

/* Test seam: the pure level lookup used by `scenery_word` (layout plane 0 = foreground, 1 = background). Returns the name-table word of 8x8 cell (cx, cy) or
 * -1 when the RAM image is not a valid level image; a coordinate outside the layout is a blank cell (0). */
int sonic1_world_cell(const uint8_t *ram, unsigned plane, int cx, int cy);

#ifdef __cplusplus
}
#endif
#endif
