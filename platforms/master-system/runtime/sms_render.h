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

/* ---- SEG-120: presentation-only Level 2 background extension (ADR 0263) -------------------------------------------------
 * An optional second output of the line hook. For every active line the hook, while that line's authentic VDP state is available, also writes
 * `side` background columns left of x = 0 and right of x = 255 (6-bit CRAM values like `pixels`) into renderer-owned preallocated arrays. Nothing is
 * ever reconstructed from the finished framebuffer, no VDP/line state is stored, and the extension never enters SmsFrameRecord, the SHA-256, the
 * VDP/machine digests or any serialization. With no extension attached (`SmsRenderer::ext == NULL`) or a zero request, the renderer is the unchanged
 * native renderer.
 *
 * The Mode 4 name table is 32 columns = 256 pixels and wraps horizontally, so the sample at screen x outside [0, 256) is, by construction of
 * `(x - hscroll) & 255`, the very tile pixel the native viewport shows at x & 255: the extension REPEATS scenery that is already on screen and reveals
 * no further part of the game world. Every extended line is therefore classified WRAPPED (reported Partial). DISTINCT is part of the typed vocabulary
 * but the Mode 4 model has no case that produces it (it would need a name table wider than the viewport); nothing upgrades WRAPPED to DISTINCT.
 * Lines whose extension would need hardware behaviour the model leaves undefined outside the physical screen (left-column mask, the right-hand
 * vertical-scroll lock of columns 24-31) are not extended: the frame is UNAVAILABLE with a typed reason and the host presents the Original picture.
 * Sprites are not extended (SEG-121). The 240-line mode stays unsupported (the renderer renders nothing in it, extension included). */
#define SMS_EXT_MAX_SIDE 80u /* source pixels per side; the widest 16:9 need is 71 (224 lines, square pixels) */

typedef enum SmsExtClass {
  SMS_EXT_NONE = 0,    /* no extension was requested/produced for the frame */
  SMS_EXT_WRAPPED = 1, /* real tilemap data, but the 256-pixel wrap repeating the native picture: Partial */
  SMS_EXT_DISTINCT = 2,/* reserved: provably additional scenery (no Mode 4 case exists) */
  SMS_EXT_UNAVAILABLE = 3
} SmsExtClass;

typedef enum SmsExtReason {
  SMS_EXT_REASON_NONE = 0,
  SMS_EXT_REASON_LEFT_MASK,    /* R0 bit 5: the blanked left column makes the left edge undefined */
  SMS_EXT_REASON_VSCROLL_LOCK, /* R0 bit 7: columns 24-31 are fixed by physical screen position, undefined off screen */
  SMS_EXT_REASON_GEOMETRY,     /* the requested side exceeds SMS_EXT_MAX_SIDE */
  SMS_EXT_REASON_INCOMPLETE    /* not every active line was produced */
} SmsExtReason;

typedef struct SmsExtResult {
  uint64_t sequence; /* SmsRenderer::frames_completed after this frame (the identity of the native frame it extends) */
  uint32_t height;   /* active lines */
  uint32_t side;     /* columns per side */
  SmsExtClass cls;
  SmsExtReason reason;
} SmsExtResult;

typedef struct SmsExtension {
  /* host request, written between guest frames only; 0 = no extension for that frame height. Latched at line 0 of each frame. */
  uint32_t request_side_192, request_side_224;
  /* producer state of the frame in progress */
  uint32_t side;
  uint32_t ok_lines;
  uint8_t geometry_fail;
  SmsExtReason first_reason;
  uint8_t work_left[SMS_FB_MAX_HEIGHT][SMS_EXT_MAX_SIDE], work_right[SMS_FB_MAX_HEIGHT][SMS_EXT_MAX_SIDE];
  /* published at frame completion, alongside SmsRenderer::last */
  SmsExtResult result;
  uint8_t left[SMS_FB_MAX_HEIGHT][SMS_EXT_MAX_SIDE], right[SMS_FB_MAX_HEIGHT][SMS_EXT_MAX_SIDE];
  uint64_t unsupported_frames; /* frames that began in an unsupported display mode while an extension was requested */
} SmsExtension;

/* Clears producer state and the published result; keeps the host request. */
void sms_extension_reset(SmsExtension *ext);
const char *sms_ext_class_name(SmsExtClass cls);
const char *sms_ext_reason_name(SmsExtReason reason);

typedef struct SmsRenderer {
  SmsVdp *vdp; /* receives the sprite flags */
  SmsExtension *ext; /* optional presentation-only extension target (NULL = none); never serialized */
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
