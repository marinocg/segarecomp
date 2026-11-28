#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MOD_PRESENT_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MOD_PRESENT_H

/*
 * SEG-147-T006 (ADR 0325): the presentation-only output of a Master System mod and its ONE compositor. This module has no machine, memory or VDP argument anywhere: it reads a finished framebuffer
 * (256 x height CRAM colour values) and writes host RGB, so no presentation request can change guest state, a guest digest, a save state or a replay. Integer only; identical on every host.
 *
 * Each request function validates its arguments and answers SEGA_MOD_OK, SEGA_MOD_E_RANGE or SEGA_MOD_E_BUDGET; phase and capability are the scheduler's checks.
 */
#include <stdint.h>

#include <segarecomp_mod_sms.h>

#include "sms_render.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_MOD_OVERLAY_BYTES (SEGA_SMS_SCREEN_WIDTH * SEGA_SMS_SCREEN_MAX_HEIGHT * 4U)
#define SMS_MOD_RGB_MAX_BYTES (SEGA_SMS_SCREEN_WIDTH * SEGA_SMS_SCREEN_MAX_HEIGHT * 3U)
#define SMS_MOD_COORD_MAX 4096

typedef struct SmsModSprite {
  int32_t x, y;
  uint8_t w, h, flags;
  uint16_t tile;
} SmsModSprite;

typedef struct SmsModPresentation {
  uint32_t draws, sprite_count;          /* accepted requests */
  uint8_t has_overlay, has_palette, has_scroll;
  int32_t dx, dy;
  uint8_t mapped[64];                    /* palette: 1 = `colour[v]` replaces the CRAM colour v */
  uint32_t colour[64];                   /* 0xRRGGBB */
  uint8_t patterns[SEGA_SMS_SPRITE_PATTERNS * 32U];
  uint8_t sprite_colours[16];
  SmsModSprite sprites[SEGA_SMS_SPRITES_MAX];
  uint8_t overlay[SMS_MOD_OVERLAY_BYTES]; /* RGBA; meaningful only while has_overlay */
} SmsModPresentation;

/* Empties the presentation (the overlay pixels are cleared lazily by the first draw). */
void sms_mod_present_clear(SmsModPresentation *p);
/* 1 when nothing was requested: composing it is the Vanilla picture. */
int sms_mod_present_is_empty(const SmsModPresentation *p);

int sms_mod_present_rect(SmsModPresentation *p, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgba);
int sms_mod_present_text(SmsModPresentation *p, int32_t x, int32_t y, const char *ascii, uint32_t len, uint32_t rgba);
int sms_mod_present_palette(SmsModPresentation *p, uint32_t cram_value, uint32_t rgb);
int sms_mod_present_scroll(SmsModPresentation *p, int32_t dx, int32_t dy);
int sms_mod_present_sprite_pattern(SmsModPresentation *p, uint32_t index, const uint8_t rows[32]);
int sms_mod_present_sprite_colors(SmsModPresentation *p, const uint8_t cram[16]);
int sms_mod_present_sprite_add(SmsModPresentation *p, int32_t x, int32_t y, uint32_t width_cells, uint32_t height_cells, uint32_t tile, uint32_t flags);

/* Writes 256 x height RGB888 into `rgb` from the framebuffer (CRAM colour values, row-major) and `p` (NULL = nothing: the Vanilla expansion). `height` <= 224. */
void sms_mod_present_compose(const SmsModPresentation *p, const uint8_t *framebuffer, uint32_t height, uint8_t *rgb);

#ifdef __cplusplus
}
#endif
#endif
