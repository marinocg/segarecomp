#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_VDP_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_VDP_H

#include <stddef.h>
#include <stdint.h>

#include "sms_machine.h"

/* Master System II VDP (315-5246) state, port protocol, status and frame/line interrupt logic (SEG-009-T004; machine
 * contract section 9, ADR 0067). Mode 4 only; every other mode combination fails closed (SMS_ERROR_VDP_MODE_UNSUPPORTED).
 * Independent of every other platform's VDP. No pixel rendering here: T005 consumes `SmsVdp` (and the per-line hook) for that.
 *
 * Time. The VDP owns no clock: it is driven by the machine's scanline events (line L of frame F starts at
 * T = (F x 262 + L) x 228; U2: every in-line event happens at offset 0 of its line) and by port accesses stamped with the
 * instruction-start T-state (U11). Nothing depends on how the host slices a run. */
#ifdef __cplusplus
extern "C" {
#endif

#define SMS_VDP_VRAM_SIZE 16384u
#define SMS_VDP_CRAM_SIZE 32u
#define SMS_VDP_REGISTERS 16u /* registers 0-10 are modelled; 11-15 have no effect */

typedef enum SmsVdpMode {
  SMS_VDP_MODE_UNSUPPORTED = 0, /* TMS9918 modes, invalid text, 240-line, R0 bit 0 (no sync): typed stop when observable */
  SMS_VDP_MODE4_192 = 192,
  SMS_VDP_MODE4_224 = 224
} SmsVdpMode;

typedef enum SmsVdpTraceKind {
  SMS_VDP_TRACE_REG_WRITE = 0,    /* arg = register (0-15), byte = value */
  SMS_VDP_TRACE_STATUS_READ = 1,  /* byte = value returned */
  SMS_VDP_TRACE_FLAG_FRAME = 2,   /* the frame interrupt flag was set by a line event */
  SMS_VDP_TRACE_FLAG_LINE = 3,    /* the line interrupt flag was set by a line event */
  SMS_VDP_TRACE_VRAM_SUMMARY = 4, /* arg = VRAM data writes since the last summary (saturating), data = first 8 bytes of SHA-256(VRAM), big endian */
  SMS_VDP_TRACE_CRAM_SUMMARY = 5  /* same for CRAM */
} SmsVdpTraceKind;

typedef struct SmsVdpTraceEntry {
  uint64_t cycles;
  uint64_t data;
  uint16_t arg;
  uint8_t kind; /* SmsVdpTraceKind */
  uint8_t byte;
} SmsVdpTraceEntry;

struct SmsVdp;
/* Optional raster hook (T005): called from the scanline event of every active line (line < active lines), after the mode
 * check and before the line counter / flag logic of that line, with the register file as the line sees it. */
typedef void (*SmsVdpLineHook)(void *context, const struct SmsVdp *vdp, uint32_t line, uint64_t cycles);

typedef struct SmsVdp {
  uint8_t vram[SMS_VDP_VRAM_SIZE];
  uint8_t cram[SMS_VDP_CRAM_SIZE]; /* --BBGGRR, only the low 6 bits are stored */
  uint8_t reg[SMS_VDP_REGISTERS];
  uint16_t address; /* 14 bits */
  uint8_t code;     /* 2 bits: 0 VRAM read, 1 VRAM write, 2 register write, 3 CRAM write */
  uint8_t latch_set; /* control port first byte received */
  uint8_t latch_byte;
  uint8_t read_buffer;
  uint8_t frame_pending, sprite_overflow, sprite_collision, line_pending;
  uint8_t line_counter;
  uint8_t hcounter_latch; /* upper 8 bits of the 9-bit H counter at the last TH rising edge (contract 9.7, U3) */
  uint8_t hcounter_valid; /* 0 until the first latch: a port $7F read before it stops typed */
  uint32_t line;  /* line of the last scanline event (0..261) */
  uint64_t frame; /* frame of the last scanline event */

  SmsMemory *error_sink; /* receives SMS_ERROR_VDP_MODE_UNSUPPORTED / SMS_ERROR_HCOUNTER_UNRESOLVED */
  SmsVdpLineHook line_hook;
  void *line_hook_context;

  SmsVdpTraceEntry *trace;
  uint32_t trace_capacity, trace_count, trace_dropped;
  uint32_t vram_writes, cram_writes; /* data writes since the last summary */
} SmsVdp;

/* Power-on / machine reset state (U9 project convention, contract section 9.1): registers as the reset column, address 0,
 * code 0, latch clear, buffer 0, status 0, line counter $FF, VRAM and CRAM zero. Trace buffer, error sink and hook kept. */
void sms_vdp_reset(SmsVdp *vdp);

/* Wires `vdp` into `machine->vdp` (port device, scanline hook, irq sources) and binds the error sink to the machine. */
void sms_vdp_install(SmsMachine *machine, SmsVdp *vdp);
/* The SmsVdp wired into `machine` by sms_vdp_install, or NULL when the VDP slot holds any other device (stubs, none). */
SmsVdp *sms_vdp_from_machine(SmsMachine *machine);
void sms_vdp_set_trace(SmsVdp *vdp, SmsVdpTraceEntry *entries, uint32_t capacity);
/* Emits the pending VRAM/CRAM write summaries (call once at the end of a run before reading the trace). */
void sms_vdp_flush_trace(SmsVdp *vdp, uint64_t cycles);

/* H counter (contract 9.7, U3). `sms_vdp_hcounter_value` maps a T offset within a 228-T line to the 8-bit port value; the
 * latch is taken by the pad device on a TH pin rising edge (sms_pad.h) and returned, frozen, by every port $7F read. */
uint8_t sms_vdp_hcounter_value(uint32_t t_in_line);
void sms_vdp_latch_hcounter(SmsVdp *vdp, uint64_t cycles);

/* Port protocol, exposed for the unit tests (the machine reaches these through `SmsVdpDevice`). */
uint8_t sms_vdp_read(SmsVdp *vdp, SmsPortClass cls, uint64_t cycles);
void sms_vdp_write(SmsVdp *vdp, SmsPortClass cls, uint8_t value, uint64_t cycles);
void sms_vdp_scanline(SmsVdp *vdp, uint64_t frame, uint32_t line, uint64_t cycles);
uint8_t sms_vdp_irq_sources(const SmsVdp *vdp);
void sms_vdp_digest(const SmsVdp *vdp, SmsSha256 *sha);

/* Mode and geometry. `sms_vdp_mode` classifies the current register bits (contract section 9.5). */
SmsVdpMode sms_vdp_mode(const SmsVdp *vdp);
/* Number of active lines of the current mode (192 or 224), 0 when unsupported. */
static inline uint32_t sms_vdp_active_lines(SmsVdpMode mode) { return mode == SMS_VDP_MODE_UNSUPPORTED ? 0u : (uint32_t)mode; }
/* V counter value of line 0..261 in a supported mode (contract section 9.6). */
uint8_t sms_vdp_v_counter_value(SmsVdpMode mode, uint32_t line);

/* Sprite evaluation (T005) reports its flags here; a status read returns and clears them. */
static inline void sms_vdp_set_sprite_flags(SmsVdp *vdp, int overflow, int collision) {
  if (overflow) vdp->sprite_overflow = 1u;
  if (collision) vdp->sprite_collision = 1u;
}

/* Palette: CRAM byte --BBGGRR to 0xRRGGBB, each 2-bit component x 85 (presentation mapping, contract section 9.9). Pure;
 * shared with the renderer. Independent of any other platform's helper. */
static inline uint32_t sms_vdp_cram_to_rgb888(uint8_t cram) {
  const uint32_t r = (uint32_t)(cram & 3u) * 85u, g = (uint32_t)((cram >> 2) & 3u) * 85u, b = (uint32_t)((cram >> 4) & 3u) * 85u;
  return (r << 16) | (g << 8) | b;
}

#ifdef __cplusplus
}
#endif
#endif
