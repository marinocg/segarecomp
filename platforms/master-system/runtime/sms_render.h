#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_RENDER_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "sms_sha256.h"
#include "sms_vdp.h"

/* Deterministic Mode 4 renderer (SEG-009-T005; machine contract section 9.8-9.9, ADR 0068). No SDL, no host dependency.
 *
 * Raster model. The renderer is driven by the VDP's per-line hook (`sms_renderer_attach`): line L (0 .. active-1) is
 * rendered at the scanline event of line L from the register file, VRAM and CRAM as that event sees them (the U2 convention:
 * every event at offset 0 of its line, so a register write executed during line L first affects line L + 1). Latching:
 *   - R0, R1, R2, R5, R6, R7, R8 and the pattern data are read at every line;
 *   - R9 (vertical scroll) and the frame height (mode) are latched at line 0 (a change during the active display takes
 *     effect at the next frame; contract 9.8).
 * Sprite overflow / collision are reported to the VDP status flags (`sms_vdp_set_sprite_flags`) from the same line.
 *
 * Output. `pixels` is the active area only: 256 x height bytes, row-major, each byte the 6-bit CRAM value (--BBGGRR) of the
 * pixel. A frame completes after its last active line; the completed frame is copied to `last` and its SHA-256 and the VDP
 * trace count at completion are appended to the frame record list. */
#ifdef __cplusplus
extern "C" {
#endif

#define SMS_FB_WIDTH 256u
#define SMS_FB_MAX_HEIGHT 224u
#define SMS_FB_MAX_BYTES (SMS_FB_WIDTH * SMS_FB_MAX_HEIGHT)
#define SMS_SPRITES_PER_LINE 8u

typedef struct SmsFrameRecord {
  uint64_t frame;       /* frame number of the last active line */
  uint32_t height;      /* 192 or 224 */
  uint32_t trace_count; /* VDP trace entries recorded when the frame completed (per-frame VDP trace linkage) */
  uint8_t sha256[32];   /* SHA-256 of the 256 x height byte framebuffer */
} SmsFrameRecord;

typedef struct SmsRenderer {
  SmsVdp *vdp; /* receives the sprite flags */
  uint8_t pixels[SMS_FB_MAX_BYTES];
  uint8_t last[SMS_FB_MAX_BYTES];
  uint32_t height;      /* height of the frame in progress (latched at line 0) */
  uint32_t last_height; /* 0 until a frame completed */
  uint64_t last_frame;
  uint8_t vscroll;      /* R9 latched at line 0 */
  uint8_t in_frame;     /* line 0 of the frame in progress was rendered */
  uint64_t frames_completed;
  SmsFrameRecord *records;
  uint32_t record_capacity, record_count, record_dropped;
} SmsRenderer;

void sms_renderer_init(SmsRenderer *r, SmsVdp *vdp, SmsFrameRecord *records, uint32_t capacity);
/* Installs the line hook on the VDP (replaces any previous hook). */
void sms_renderer_attach(SmsRenderer *r);
/* Renders one active line from the VDP state (the hook body, also the state-injection entry point). Line 0 starts a frame.
 * `frame` labels the frame record. An unsupported mode renders nothing. */
void sms_render_line(SmsRenderer *r, const SmsVdp *vdp, uint32_t line, uint64_t frame);
/* Hashes the last completed frame (canonical serialization: the raw bytes, row-major). */
void sms_renderer_last_sha256(const SmsRenderer *r, uint8_t out[32]);
/* RGB888 expansion of a framebuffer byte (presentation mapping, contract 9.9): 0xRRGGBB. */
static inline uint32_t sms_render_rgb888(uint8_t cram) { return sms_vdp_cram_to_rgb888(cram); }

#ifdef __cplusplus
}
#endif
#endif
