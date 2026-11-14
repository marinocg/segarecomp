/* segarecomp_mod_genesis.h -- ABI 2 Genesis feature table (docs/architecture/native-sdk-abi-2.md section 3). ABI 1's segarecomp_genesis_patch.h is separate and unchanged. */
#ifndef SEGARECOMP_MOD_GENESIS_H
#define SEGARECOMP_MOD_GENESIS_H

#include <segarecomp_mod.h>

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
  void *reserved[8];                          /* NULL */
} SegaGenesisRenderApi;

typedef struct SegaGenesisFeatures {
  uint32_t size; /* the host's sizeof */
  uint32_t work_ram_bytes, vram_bytes, vdp_registers, cram_bytes, vsram_bytes, visible_lines; /* 65536, 65536, 24, 128, 80, 224 */
  uint32_t reserved[8];
  /* Appended by SEG-138-T011 (append-only: a provider reads this field only when `size` covers it). Non-NULL exactly when overlay.rgba was granted. */
  const SegaGenesisRenderApi *render;
} SegaGenesisFeatures;

#endif /* SEGARECOMP_MOD_GENESIS_H */
