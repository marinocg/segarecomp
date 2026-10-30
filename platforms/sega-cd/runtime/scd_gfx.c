/* SEG-064-T004: the Sega CD graphics ASIC (stamp rotation/scaling, contract section 17) as the authentic BIOS drives it: 2M-mode Word
 * RAM, a stamp map, a trace vector table and an image buffer, started by the write of the trace vector base ($FF8066) and completed
 * with the level-1 interrupt. The algorithm and the timing follow Genesis Plus GX core/cd_hw/gfx.c (consulted only); the synthetic tests
 * derive expected images by hand (tests/sega_cd_bios_subside_test.py).
 *
 * Registers (word access only; sub $FF8058-$FF8066):
 *   $58  stamp size control: bit 0 map repeat (1: repeat the map, 0: pixels outside it are 0), bits 2-1 stamp/map size
 *        (0: 16x16 stamps, 16x16-stamp map; 1: 32x32 stamps, 8x8 map; 2: 16x16, 256x256; 3: 32x32, 128x128); read: bit 15 = busy.
 *        Only bits 2-0 are stored (bit 15 is the read-only busy flag).
 *   $5A  stamp map base address (word address)       $5C  image buffer vertical size in cells - 1 (bits 4-0)
 *   $5E  image buffer start address                  $60  image buffer offset (bits 5-0 horizontal dot offset)
 *   $62  image buffer horizontal dot size (bits 8-0) $64  image buffer vertical dot size (lines, bits 7-0; the live counter: counts down while
 *        running, 0 when an operation has finished; the ONE copy is ScdGfx::lines_left; the byte at $65 is readable, ADR 0249)
 *   $66  trace vector base address; writing it starts the operation.
 * Each line reads four trace words (start x, start y, x step, y step; the start in 13.3, steps in 5.11 fixed point), then renders
 * `$62` dots into the image buffer through the stamp map and the stamp data, in the priority mode of the memory-mode register
 * (PM1/PM0: 0 normal, 1 underwrite, 2 overwrite, 3 invalid = keep). The operation runs only while the sub owns 2M Word RAM; it takes
 * 3 * (4 + 2 * W + ((W + (offset & 3) + 3) >> 2)) sub cycles per line (3 sub cycles per Word RAM access: 4 initial reads, 2 per dot, 1 write per 4 dots) (W = horizontal dot size), a line being rendered as soon as it
 * starts (GPGX: ceil), and ends with level 1 pending when IEN1 is set. Everything else about the ASIC (1M Word RAM, byte accesses,
 * register reads other than $58 and the byte $65, the font generator) is a typed stop. */
#include "scd_runtime.h"

#include <string.h>

#define SCD_GFX_CYCLES_PER_ACCESS 3U /* sub cycles per Word RAM access (GPGX: 4 * 3 in its sub clock units of 4 per cycle) */

static int scd_gfx_stop(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t pc) {
  return scd_fail_access(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset, width, is_write);
}

static uint32_t scd_gfx_read16(const ScdMachine *m, uint32_t offset) {
  offset &= 0x3FFFEU;
  return ((uint32_t)m->word_ram[offset] << 8) | m->word_ram[offset + 1U];
}

static uint8_t scd_gfx_read8(const ScdMachine *m, uint32_t offset) { return m->word_ram[offset & 0x3FFFFU]; }

static void scd_gfx_write8(ScdMachine *m, uint32_t offset, uint8_t value) {
  offset &= 0x3FFFFU;
  m->word_ram[offset] = value;
  SCD_OVERLAY_NOTE_WRITE(m, SCD_MEMORY_WORD, offset, 1U);
}

/* GPGX gfx_init lookup tables, as functions. */
static uint32_t scd_gfx_cell(uint32_t i) { /* table entry yyxxshrr -> cell offset */
  const uint32_t mask = (i & 8U) ? 3U : 1U;
  uint32_t row = (i >> 6) & mask, col = (i >> 4) & mask, temp;
  if (i & 4U) col ^= mask;
  if (i & 2U) { col ^= mask; row ^= mask; }
  if (i & 1U) { temp = col; col = row ^ mask; row = temp; }
  return row + col * (mask + 1U);
}

static uint32_t scd_gfx_pixel(uint32_t i) { /* table entry yyyxxxhrr -> pixel offset (0-63) */
  uint32_t row = (i >> 6) & 7U, col = (i >> 3) & 7U, temp;
  if (i & 4U) col ^= 7U;
  if (i & 2U) { col ^= 7U; row ^= 7U; }
  if (i & 1U) { temp = col; col = row ^ 7U; row = temp; }
  return col + row * 8U;
}

static uint32_t scd_gfx_priority(uint32_t mode, uint32_t in, uint32_t out) {
  switch (mode & 3U) {
  case 0: return out;                                                                                /* normal */
  case 1: return ((in & 0x0FU) ? (in & 0x0FU) : (out & 0x0FU)) | ((in & 0xF0U) ? (in & 0xF0U) : (out & 0xF0U)); /* underwrite */
  case 2: return ((out & 0x0FU) ? (out & 0x0FU) : (in & 0x0FU)) | ((out & 0xF0U) ? (out & 0xF0U) : (in & 0xF0U)); /* overwrite */
  default: return in;                                                                                /* invalid */
  }
}

/* One image buffer line (GPGX gfx_render). */
static void scd_gfx_render_line(ScdMachine *m, uint32_t buffer_index, uint32_t width) {
  ScdGfx *g = &m->gfx;
  const uint32_t stamp_mask = (g->ctl & 0x02U) ? 0x7FCU : 0x7FFU;
  const uint32_t priority = (m->ga.mode >> 3) & 3U;
  uint32_t xpos = scd_gfx_read16(m, g->trace_ptr) << 8, ypos = scd_gfx_read16(m, g->trace_ptr + 2U) << 8;
  const uint32_t xoffset = (uint32_t)(int32_t)(int16_t)scd_gfx_read16(m, g->trace_ptr + 4U);
  const uint32_t yoffset = (uint32_t)(int32_t)(int16_t)scd_gfx_read16(m, g->trace_ptr + 6U);
  g->trace_ptr += 8U;
  if (g->trace_ptr == 0x40000U) g->trace_ptr = 0U; /* trace vector address overflow */
  while (width--) {
    uint32_t pixel_out, pixel_in;
    if (g->ctl & 0x01U) { /* the stamp map is repeated */
      xpos &= g->dot_mask;
      ypos &= g->dot_mask;
    } else {
      xpos &= 0xFFFFFFU;
      ypos &= 0xFFFFFFU;
    }
    if ((xpos | ypos) & ~g->dot_mask) {
      pixel_out = 0U; /* outside the stamp map */
    } else {
      uint32_t stamp_data = scd_gfx_read16(m, g->map_ptr + 2U * ((xpos >> g->stamp_shift) | ((ypos >> g->stamp_shift) << g->map_shift)));
      uint32_t stamp_index = (stamp_data & stamp_mask) << 8;
      if (stamp_index) {
        stamp_data = (stamp_data >> 13) & 7U;
        stamp_index |= scd_gfx_cell(stamp_data | ((g->ctl & 0x02U) << 2) | ((ypos >> 8) & 0xC0U) | ((xpos >> 10) & 0x30U)) << 6;
        stamp_index |= scd_gfx_pixel(stamp_data | ((xpos >> 8) & 0x38U) | ((ypos >> 5) & 0x1C0U));
        pixel_out = scd_gfx_read8(m, stamp_index >> 1);
        pixel_out = (stamp_index & 1U) ? (pixel_out & 0x0FU) : (pixel_out >> 4);
      } else {
        pixel_out = 0U; /* stamp 0 is not used */
      }
    }
    pixel_in = scd_gfx_read8(m, buffer_index >> 1);
    pixel_out = (buffer_index & 1U) ? (pixel_out | (pixel_in & 0xF0U)) : ((pixel_out << 4) | (pixel_in & 0x0FU));
    pixel_out = scd_gfx_priority(priority, pixel_in, pixel_out & 0xFFU);
    scd_gfx_write8(m, buffer_index >> 1, (uint8_t)pixel_out);
    buffer_index = ((buffer_index & 7U) != 7U) ? buffer_index + 1U : buffer_index + g->buffer_offset;
    xpos += xoffset;
    ypos += yoffset;
  }
}

static int scd_gfx_halted(const ScdMachine *m) { return (m->ga.mode & (SCD_MODE_1M | SCD_MODE_RET)) == SCD_MODE_RET; }

static void scd_gfx_finish(ScdMachine *m) {
  m->gfx.busy = 0U;
  m->gfx.operations += 1U;
  if (m->ga.interrupt_enable & 0x02U) scd_raise_sub_level(m, 1U);
  scd_log(m, 0xB2U, (uint32_t)m->gfx.operations, (uint32_t)m->gfx.lines_rendered);
}

/* Bring a running operation up to tick `now` (GPGX gfx_update). */
void scd_gfx_sync(ScdMachine *m, uint64_t now) {
  ScdGfx *g = &m->gfx;
  uint64_t lines, finished_lines;
  int done = 0;
  if (!g->busy) return;
  if (scd_gfx_halted(m)) { /* Word RAM is main's: the processing is halted and its clock follows the machine */
    g->base = now;
    return;
  }
  if (now <= g->base) return;
  lines = (now - g->base + g->line_ticks - 1U) / g->line_ticks;
  if (lines < g->lines_left) {
    g->lines_left = (uint8_t)(g->lines_left - lines);
    g->base += lines * g->line_ticks;
    finished_lines = lines;
  } else {
    finished_lines = g->lines_left;
    g->base += finished_lines * g->line_ticks;
    g->lines_left = 0U;
    done = 1;
  }
  while (finished_lines--) {
    scd_gfx_render_line(m, g->buffer_start, (uint32_t)(g->h_dots & 0x1FFU));
    g->buffer_start += 8U;
    g->lines_rendered += 1U;
  }
  if (done) scd_gfx_finish(m);
}

uint64_t scd_gfx_next_event(const ScdMachine *m) {
  const ScdGfx *g = &m->gfx;
  if (!g->busy || scd_gfx_halted(m)) return UINT64_MAX;
  return g->base + (g->lines_left > 0U ? (uint64_t)(g->lines_left - 1U) * g->line_ticks : 0U) + 1U;
}

int scd_gfx_can_raise(const ScdMachine *m) { return m->gfx.busy && !scd_gfx_halted(m) && (m->ga.interrupt_enable & 0x02U); }

void scd_gfx_reset(ScdMachine *m) { memset(&m->gfx, 0, sizeof m->gfx); }

static int scd_gfx_start(ScdMachine *m, uint32_t trace_base, uint32_t pc) {
  ScdGfx *g = &m->gfx;
  uint32_t mask, h, offset;
  if (m->ga.mode & SCD_MODE_1M) return scd_gfx_stop(m, 0x66U, 2U, 1, pc); /* the ASIC's 1M Word RAM behaviour is not modelled */
  g->trace_ptr = (trace_base << 2) & 0x3FFF8U;
  switch ((g->ctl >> 1) & 3U) {
  case 0: g->dot_mask = 0x07FFFFU; g->stamp_shift = 15U; g->map_shift = 4U; mask = 0x3FE00U; break;
  case 1: g->dot_mask = 0x07FFFFU; g->stamp_shift = 16U; g->map_shift = 3U; mask = 0x3FF80U; break;
  case 2: g->dot_mask = 0x7FFFFFU; g->stamp_shift = 15U; g->map_shift = 8U; mask = 0x20000U; break;
  default: g->dot_mask = 0x7FFFFFU; g->stamp_shift = 16U; g->map_shift = 7U; mask = 0x38000U; break;
  }
  g->map_ptr = ((uint32_t)g->map_base << 2) & mask;
  g->buffer_offset = (((uint32_t)(g->cell_v & 0x1FU) + 1U) << 6) - 7U;
  g->buffer_start = (((uint32_t)g->buffer_origin << 3) & 0x7FFC0U) + (g->offset & 0x3FU);
  g->base = m->now;
  h = (uint32_t)(g->h_dots & 0x1FFU);
  offset = (uint32_t)(g->offset & 3U);
  g->line_ticks = (uint64_t)SCD_GFX_CYCLES_PER_ACCESS * (4U + 2U * h + ((h + offset + 3U) >> 2)) * scd_sub_cycle_ticks(m);
  g->busy = 1U;
  scd_log(m, 0xB1U, trace_base, g->lines_left);
  if (m->event_log != NULL) scd_event(m, "gfx_start", g->lines_left);
  return 0;
}

/* $FF8058-$FF8067 (the Gate Array has dispatched here). */
int scd_gfx_register_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  ScdGfx *g = &m->gfx;
  scd_gfx_sync(m, m->now);
  if (width != 2U) {
    if (!is_write && offset == 0x58U && width == 1U) { /* the busy byte (GPGX scd_read_byte case 0x58) */
      *value = g->busy ? 0x80U : 0U;
      return 0;
    }
    if (!is_write && offset == 0x65U && width == 1U) { /* SEG-105: the live vertical-dot counter (low byte of $64; ADR 0249) */
      *value = g->lines_left;
      g->vdot_reads += 1U;
      return 0;
    }
    return scd_gfx_stop(m, offset, width, is_write, pc);
  }
  if (!is_write) {
    if (offset != 0x58U) return scd_gfx_stop(m, offset, width, 0, pc);
    *value = ((uint32_t)g->busy << 15) | g->ctl;
    return 0;
  }
  switch (offset) {
  case 0x58: g->ctl = (uint8_t)(*value & 7U); return 0;
  case 0x5A: g->map_base = (uint16_t)*value; return 0;
  case 0x5C: g->cell_v = (uint16_t)*value; return 0;
  case 0x5E: g->buffer_origin = (uint16_t)*value; return 0;
  case 0x60: g->offset = (uint16_t)*value; return 0;
  case 0x62: g->h_dots = (uint16_t)*value; return 0;
  case 0x64: g->lines_left = (uint8_t)(*value & 0xFFU); return 0; /* the live counter, also while idle and while running (GPGX regs[0x32].byte.l) */
  case 0x66: g->trace_base = (uint16_t)*value; return scd_gfx_start(m, *value & 0xFFFFU, pc);
  default: break;
  }
  return scd_gfx_stop(m, offset, width, 1, pc);
}
