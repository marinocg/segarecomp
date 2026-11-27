/* segarecomp_mod_sms.h -- ABI 2 Master System feature table (docs/architecture/native-sdk-abi-2.md sections 3 and 12; ADR 0325). The table holds Master System fields only.
 *
 * Phases reset, frame-begin, input, post-input, frame-end and render run at the machine's frame start (T = N x 59,736 Z80 T-states). Guest memory: `host->read` / `host->write` with a SegaSmsSpace, width 1 or 2
 * BYTES, LITTLE-ENDIAN (the Z80), no alignment rule, inside a manifest-declared range. Input: `host->input_get` / `host->input_set`, ports 1 and 2, the six pad bits SEGA_SMS_BUTTON_*; the console Pause button
 * is not part of the pad and cannot be read or changed. Everything under `render` is PRESENTATION ONLY: nothing in this header can write VRAM, CRAM, a VDP register or any other guest state. */
#ifndef SEGARECOMP_MOD_SMS_H
#define SEGARECOMP_MOD_SMS_H

#include <segarecomp_mod.h>

#define SEGA_MOD_PLATFORM SEGA_MOD_PLATFORM_MASTER_SYSTEM
#if defined(SEGA_MOD_TARGET_PLATFORM) && (SEGA_MOD_TARGET_PLATFORM != SEGA_MOD_PLATFORM_ID_MASTER_SYSTEM)
#error "E_ABI_PLATFORM: this provider includes the Master System header but the target platform is different"
#endif

typedef enum SegaSmsSpace {
  SEGA_SMS_SPACE_Z80_WORK_RAM = 1, /* 8 KiB; the $E000 mirror is the same space; the only mod-writable space */
  SEGA_SMS_SPACE_Z80_CART_RAM      /* 32 KiB, read only */
} SegaSmsSpace;

typedef enum SegaSmsMapper { SEGA_SMS_MAPPER_SEGA = 1, SEGA_SMS_MAPPER_ROM_ONLY = 2 } SegaSmsMapper; /* declared in the manifest, never inferred */

#define SEGA_SMS_BUTTON_UP 0x01U
#define SEGA_SMS_BUTTON_DOWN 0x02U
#define SEGA_SMS_BUTTON_LEFT 0x04U
#define SEGA_SMS_BUTTON_RIGHT 0x08U
#define SEGA_SMS_BUTTON_1 0x10U
#define SEGA_SMS_BUTTON_2 0x20U

#define SEGA_SMS_SCREEN_WIDTH 256U
#define SEGA_SMS_SCREEN_MAX_HEIGHT 224U
#define SEGA_SMS_OVERLAY_DRAWS_MAX 64U    /* rect + text requests per render callback */
#define SEGA_SMS_OVERLAY_TEXT_BYTES 64U
#define SEGA_SMS_SCROLL_MAX 255           /* |dx|, |dy| */
#define SEGA_SMS_SPRITES_MAX 64U          /* authored sprites per frame */
#define SEGA_SMS_SPRITE_PATTERNS 256U     /* the mod's own pattern pool, one frame long */
#define SEGA_SMS_SPRITE_HFLIP 1U
#define SEGA_SMS_SPRITE_VFLIP 2U

/* The presentation of ONE frame (ABI minor 5), composed by the host over the finished picture, in this order: the picture shifted by `scroll` (vacated pixels are black) with each CRAM colour value mapped
 * through `palette_color`; the authored sprites in submission order (later over earlier); the overlay. Coordinates are presented picture pixels, top left origin. Valid only inside `render`; submissions are
 * staged and dropped if the callback faults; nothing carries to the next frame. Every call returns a SegaModStatus; a non-zero result is also a fault of the callback. */
typedef struct SegaSmsRenderApi {
  uint32_t size; /* the host's sizeof */
  /* overlay.rgba. `rgba` is 0xRRGGBBAA, straight alpha. A rectangle is clamped to 256 x 224; a text origin is clamped and the text cut to 64 bytes, non-printable bytes drawn as '?'. */
  int (*overlay_rect)(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgba);
  int (*overlay_text)(int32_t x, int32_t y, const char *ascii, uint32_t len, uint32_t rgba);
  /* present.transform. Presents every pixel whose guest colour is `cram_value` (0..63, --BBGGRR) as `rgb` (0xRRGGBB). The guest CRAM is not touched. */
  int (*palette_color)(uint32_t cram_value, uint32_t rgb);
  /* present.transform. Moves the produced picture by (dx, dy) presented pixels; the picture size does not change. The guest scroll registers are not touched. */
  int (*scroll)(int32_t dx, int32_t dy);
  /* overlay.rgba. AUTHORED sprites only (the provenance rule of SEG-140-T001): the pixels come from the mod's own pool below; there is no way to draw from guest VRAM, so nothing can be presented as
   * verified game art. Pattern: 8 x 8, 4 bits per pixel, 4 bytes per row, high nibble = left pixel; colour index 0 is transparent. Colours: 16 SMS --BBGGRR values (0..63). A sprite is 1..4 x 1..4
   * cells, patterns row-major from `tile`; flags are SEGA_SMS_SPRITE_*. */
  int (*sprite_pattern)(uint32_t index, const uint8_t rows[32]);
  int (*sprite_colors)(const uint8_t cram[16]);
  int (*sprite_add)(int32_t x, int32_t y, uint32_t width_cells, uint32_t height_cells, uint32_t tile, uint32_t flags);
  void *reserved[4]; /* NULL */
} SegaSmsRenderApi;

/* A read-only copy of the VDP taken after frame-end of the rendered frame (capability frame.sms_vdp, ABI minor 5). The sprite fields are derived from the registers: the sprite attribute table starts at
 * `sprite_table` in `vram` (64 Y bytes, then at +128 the 64 X / pattern pairs). */
typedef struct SegaSmsVdpView {
  uint32_t size, active_lines; /* 192 or 224; 0 for an unsupported mode */
  const uint8_t *vram;         /* 16384 bytes */
  const uint8_t *cram;         /* 32 bytes, --BBGGRR */
  const uint8_t *registers;    /* 16 bytes */
  uint32_t sprite_table, sprite_pattern_base, sprite_height; /* height 8 or 16 (before zoom) */
} SegaSmsVdpView;

typedef struct SegaMasterSystemFeatures {
  uint32_t size; /* the host's sizeof */
  uint32_t work_ram_bytes, cart_ram_bytes, vram_bytes, cram_bytes; /* 8192, 32768, 16384, 32 */
  uint32_t mapper;                                                 /* SegaSmsMapper */
  uint32_t reserved[8];
  const SegaSmsRenderApi *render; /* minor 5: non-NULL exactly when overlay.rgba or present.transform was granted */
  const SegaSmsVdpView *vdp;      /* minor 5: non-NULL only inside render and only with frame.sms_vdp */
} SegaMasterSystemFeatures;

#endif /* SEGARECOMP_MOD_SMS_H */
