#include "sms_vdp.h"

#include <string.h>

/* SMS II VDP: see sms_vdp.h and machine contract section 9. Every rule below cites MacDonald's VDP document (MD-VDP) or
 * SMS Power! through the contract; nothing is derived from a reference emulator's source. */

#define R0_NOSYNC 0x01u
#define R0_M2 0x02u
#define R0_M4 0x04u
#define R0_IE1 0x10u /* line interrupt enable */
#define R1_M3 0x08u
#define R1_M1 0x10u
#define R1_IE0 0x20u /* frame interrupt enable */
#define R1_DISPLAY 0x40u

static const uint8_t reset_registers[SMS_VDP_REGISTERS] = {0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00,
                                                           0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00};

/* ---- trace ---------------------------------------------------------------------------------------------------- */

static void trace_add(SmsVdp *v, uint64_t cycles, SmsVdpTraceKind kind, uint16_t arg, uint8_t byte, uint64_t data) {
  if (v->trace != NULL && v->trace_count < v->trace_capacity) {
    SmsVdpTraceEntry *e = &v->trace[v->trace_count++];
    e->cycles = cycles;
    e->data = data;
    e->arg = arg;
    e->kind = (uint8_t)kind;
    e->byte = byte;
  } else {
    ++v->trace_dropped;
  }
}

static uint64_t hash64(const uint8_t *bytes, size_t size) {
  SmsSha256 sha;
  uint8_t out[32];
  uint64_t value = 0;
  int i;
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, bytes, size);
  sms_sha256_final(&sha, out);
  for (i = 0; i < 8; ++i) value = (value << 8) | out[i];
  return value;
}

void sms_vdp_flush_trace(SmsVdp *v, uint64_t cycles) {
  if (v->vram_writes != 0u) {
    trace_add(v, cycles, SMS_VDP_TRACE_VRAM_SUMMARY, (uint16_t)(v->vram_writes > 0xFFFFu ? 0xFFFFu : v->vram_writes), 0,
              hash64(v->vram, sizeof v->vram));
    v->vram_writes = 0;
  }
  if (v->cram_writes != 0u) {
    trace_add(v, cycles, SMS_VDP_TRACE_CRAM_SUMMARY, (uint16_t)(v->cram_writes > 0xFFFFu ? 0xFFFFu : v->cram_writes), 0,
              hash64(v->cram, sizeof v->cram));
    v->cram_writes = 0;
  }
}

void sms_vdp_set_trace(SmsVdp *v, SmsVdpTraceEntry *entries, uint32_t capacity) {
  v->trace = entries;
  v->trace_capacity = capacity;
  v->trace_count = v->trace_dropped = 0;
}

/* ---- mode and geometry ---------------------------------------------------------------------------------------- */

SmsVdpMode sms_vdp_mode(const SmsVdp *v) {
  const unsigned r0 = v->reg[0], r1 = v->reg[1];
  const unsigned m2 = (r0 & R0_M2) != 0u, m3 = (r1 & R1_M3) != 0u, m1 = (r1 & R1_M1) != 0u;
  if ((r0 & R0_M4) == 0u || (r0 & R0_NOSYNC) != 0u) return SMS_VDP_MODE_UNSUPPORTED; /* TMS9918 modes, no-sync */
  if (!m1 && !m2) return SMS_VDP_MODE4_192;                 /* 1 x 0 0 */
  if (!m3 && m2 && !m1) return SMS_VDP_MODE4_192;           /* 1 0 1 0 */
  if (m3 && m2 && m1) return SMS_VDP_MODE4_192;             /* 1 1 1 1 */
  if (!m3 && m2 && m1) return SMS_VDP_MODE4_224;            /* 1 0 1 1 */
  return SMS_VDP_MODE_UNSUPPORTED;                          /* 1 1 1 0 (240 lines), 1 x 0 1 (invalid text) */
}

uint8_t sms_vdp_v_counter_value(SmsVdpMode mode, uint32_t line) {
  const uint32_t jump = mode == SMS_VDP_MODE4_224 ? 235u : 219u; /* first line whose counter steps back by 6 */
  return (uint8_t)(line >= jump ? line - 6u : line);
}

static void fail_mode(SmsVdp *v, uint64_t cycles) {
  if (v->error_sink != NULL)
    sms_memory_latch_error(v->error_sink, SMS_ERROR_VDP_MODE_UNSUPPORTED, (uint16_t)((v->reg[1] << 8) | v->reg[0]), 0,
                           cycles);
}

/* ---- reset ---------------------------------------------------------------------------------------------------- */

void sms_vdp_reset(SmsVdp *v) {
  memset(v->vram, 0, sizeof v->vram);
  memset(v->cram, 0, sizeof v->cram);
  memcpy(v->reg, reset_registers, sizeof v->reg);
  v->address = 0;
  v->code = 0;
  v->latch_set = 0;
  v->latch_byte = 0;
  v->read_buffer = 0;
  v->frame_pending = v->sprite_overflow = v->sprite_collision = v->line_pending = 0;
  v->line_counter = 0xFFu;
  v->hcounter_latch = 0;
  v->hcounter_valid = 0;
  v->line = 0;
  v->frame = 0;
  v->trace_count = v->trace_dropped = 0;
  v->vram_writes = v->cram_writes = 0;
}

/* ---- H counter (contract 9.7, U3) ----------------------------------------------------------------------------------- */

/* The counter has 342 pixel positions per line, 2 pixels per count, running 0x00-0x93 then 0xE9-0xFF (MacDonald 342-pixel
 * breakdown). Pixel position = floor(3 x T / 2) + origin (exact integer arithmetic, never accumulated). Line T = 0 is
 * pixel 266 (count 0x85), the origin of the GPGX table; Gearsystem's is 30 pixels (20 T) earlier, a constant
 * shift of the same sequence (the U2 in-line offset tolerance, tests/sms_hcounter_reference_test.py). */
#define SMS_HCOUNTER_ORIGIN_PIXEL 266u
#define SMS_HCOUNTER_LINE_PIXELS 342u

uint8_t sms_vdp_hcounter_value(uint32_t t_in_line) {
  const uint32_t pixel = (3u * t_in_line / 2u + SMS_HCOUNTER_ORIGIN_PIXEL) % SMS_HCOUNTER_LINE_PIXELS;
  const uint32_t count = pixel / 2u; /* 0..170 */
  return (uint8_t)(count < 0x94u ? count : 0xE9u + (count - 0x94u));
}

void sms_vdp_latch_hcounter(SmsVdp *v, uint64_t cycles) {
  v->hcounter_latch = sms_vdp_hcounter_value((uint32_t)(cycles % 228u));
  v->hcounter_valid = 1u;
}

/* ---- ports ---------------------------------------------------------------------------------------------------- */

static void control_write(SmsVdp *v, uint8_t value, uint64_t cycles) {
  if (!v->latch_set) {
    v->latch_set = 1u;
    v->latch_byte = value;
    v->address = (uint16_t)((v->address & 0x3F00u) | value); /* the first byte updates the address low byte at once */
    return;
  }
  v->latch_set = 0u;
  v->code = (uint8_t)(value >> 6);
  v->address = (uint16_t)(((value & 0x3Fu) << 8) | (v->address & 0x00FFu));
  if (v->code == 0u) {
    v->read_buffer = v->vram[v->address];
    v->address = (uint16_t)((v->address + 1u) & 0x3FFFu);
  } else if (v->code == 2u) {
    const unsigned r = value & 0x0Fu;
    trace_add(v, cycles, SMS_VDP_TRACE_REG_WRITE, (uint16_t)r, v->latch_byte, 0);
    if (r <= 10u) v->reg[r] = v->latch_byte; /* registers 11-15 have no effect */
  }
}

static void data_write(SmsVdp *v, uint8_t value) {
  v->latch_set = 0u;
  if (v->code == 3u) {
    v->cram[v->address & 0x1Fu] = (uint8_t)(value & 0x3Fu);
    ++v->cram_writes;
  } else {
    v->vram[v->address] = value;
    ++v->vram_writes;
  }
  v->read_buffer = value;
  v->address = (uint16_t)((v->address + 1u) & 0x3FFFu);
}

void sms_vdp_write(SmsVdp *v, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  if (cls == SMS_PORT_VDP_CONTROL) control_write(v, value, cycles);
  else if (cls == SMS_PORT_VDP_DATA) data_write(v, value);
}

uint8_t sms_vdp_read(SmsVdp *v, SmsPortClass cls, uint64_t cycles) {
  uint8_t value = 0xFFu;
  switch (cls) {
    case SMS_PORT_VDP_DATA:
      v->latch_set = 0u;
      value = v->read_buffer;
      v->read_buffer = v->vram[v->address];
      v->address = (uint16_t)((v->address + 1u) & 0x3FFFu);
      break;
    case SMS_PORT_VDP_STATUS:
      if (sms_vdp_mode(v) == SMS_VDP_MODE_UNSUPPORTED) fail_mode(v, cycles); /* a flag it would produce is read */
      value = (uint8_t)((v->frame_pending << 7) | (v->sprite_overflow << 6) | (v->sprite_collision << 5) | 0x1Fu);
      v->frame_pending = v->sprite_overflow = v->sprite_collision = v->line_pending = 0;
      v->latch_set = 0u;
      trace_add(v, cycles, SMS_VDP_TRACE_STATUS_READ, 0, value, 0);
      break;
    case SMS_PORT_V_COUNTER: {
      const SmsVdpMode mode = sms_vdp_mode(v);
      if (mode == SMS_VDP_MODE_UNSUPPORTED) {
        fail_mode(v, cycles);
        value = 0xFFu;
      } else {
        value = sms_vdp_v_counter_value(mode, v->line);
      }
      break;
    }
    case SMS_PORT_H_COUNTER: /* the value latched by the last TH rising edge; unlatched, no value exists: typed stop */
      if (v->hcounter_valid) {
        value = v->hcounter_latch;
      } else if (v->error_sink != NULL) {
        sms_memory_latch_error(v->error_sink, SMS_ERROR_HCOUNTER_UNRESOLVED, 0x7Fu, 0, cycles);
      }
      break;
    default: break;
  }
  return value;
}

/* ---- scanline events ------------------------------------------------------------------------------------------ */

void sms_vdp_scanline(SmsVdp *v, uint64_t frame, uint32_t line, uint64_t cycles) {
  const SmsVdpMode mode = sms_vdp_mode(v);
  uint32_t active;
  v->frame = frame;
  v->line = line;
  if (line == 0u) sms_vdp_flush_trace(v, cycles); /* per-frame write summaries */
  if (mode == SMS_VDP_MODE_UNSUPPORTED) {
    /* The mode is observable here only through the display or an interrupt it would time: stop typed (never approximate). */
    if ((v->reg[1] & (R1_DISPLAY | R1_IE0)) != 0u || (v->reg[0] & R0_IE1) != 0u) fail_mode(v, cycles);
    return;
  }
  active = sms_vdp_active_lines(mode);
  if (line < active && v->line_hook != NULL) v->line_hook(v->line_hook_context, v, line, cycles);
  /* Line counter: decremented on lines 0..active (inclusive); an underflow reloads from R10 and sets the line flag; it is
   * reloaded every line after that (MD-VDP section 12). */
  if (line <= active) {
    if (v->line_counter == 0u) {
      v->line_counter = v->reg[10];
      v->line_pending = 1u;
      trace_add(v, cycles, SMS_VDP_TRACE_FLAG_LINE, (uint16_t)line, 0, 0);
    } else {
      --v->line_counter;
    }
  } else {
    v->line_counter = v->reg[10];
  }
  if (line == active + 1u) {
    v->frame_pending = 1u;
    trace_add(v, cycles, SMS_VDP_TRACE_FLAG_FRAME, (uint16_t)line, 0, 0);
  }
}

uint8_t sms_vdp_irq_sources(const SmsVdp *v) {
  return (uint8_t)(((v->frame_pending != 0u && (v->reg[1] & R1_IE0) != 0u) ? SMS_IRQ_FRAME : 0u) |
                   ((v->line_pending != 0u && (v->reg[0] & R0_IE1) != 0u) ? SMS_IRQ_LINE : 0u));
}

void sms_vdp_digest(const SmsVdp *v, SmsSha256 *sha) {
  const uint8_t scalars[9] = {v->code, v->latch_set, v->latch_byte, v->read_buffer, v->frame_pending, v->sprite_overflow,
                               v->sprite_collision, v->line_pending, v->line_counter};
  sms_sha256_update(sha, v->vram, sizeof v->vram);
  sms_sha256_update(sha, v->cram, sizeof v->cram);
  sms_sha256_update(sha, v->reg, sizeof v->reg);
  sms_sha256_update_u16(sha, v->address);
  sms_sha256_update(sha, scalars, sizeof scalars);
  sms_sha256_update_u8(sha, v->hcounter_latch);
  sms_sha256_update_u8(sha, v->hcounter_valid);
  sms_sha256_update_u32(sha, v->line);
  sms_sha256_update_u64(sha, v->frame);
  sms_sha256_update_u32(sha, v->vram_writes);
  sms_sha256_update_u32(sha, v->cram_writes);
  sms_sha256_update_u32(sha, v->trace_count);
}

/* ---- machine seam --------------------------------------------------------------------------------------------- */

static void seam_reset(void *context) { sms_vdp_reset((SmsVdp *)context); }
static uint8_t seam_read(void *context, SmsPortClass cls, uint64_t cycles) {
  return sms_vdp_read((SmsVdp *)context, cls, cycles);
}
static void seam_write(void *context, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  sms_vdp_write((SmsVdp *)context, cls, value, cycles);
}
static void seam_digest(void *context, SmsSha256 *sha) { sms_vdp_digest((const SmsVdp *)context, sha); }
static void seam_scanline(void *context, uint64_t frame, uint32_t line, uint64_t cycles) {
  sms_vdp_scanline((SmsVdp *)context, frame, line, cycles);
}
static uint8_t seam_irq(void *context) { return sms_vdp_irq_sources((const SmsVdp *)context); }

void sms_vdp_install(SmsMachine *m, SmsVdp *v) {
  v->error_sink = &m->mem;
  m->vdp.port.context = v;
  m->vdp.port.reset = seam_reset;
  m->vdp.port.read = seam_read;
  m->vdp.port.write = seam_write;
  m->vdp.port.digest = seam_digest;
  m->vdp.scanline = seam_scanline;
  m->vdp.irq_sources = seam_irq;
}

SmsVdp *sms_vdp_from_machine(SmsMachine *m) { return m->vdp.scanline == seam_scanline ? (SmsVdp *)m->vdp.port.context : NULL; }
