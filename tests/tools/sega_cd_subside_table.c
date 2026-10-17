/* SEG-064-T004: table-driven checks of the sub-side boot hardware the authentic BIOS reaches after the sub runnable epoch - the
 * stopwatch ($FF800C), the PCM write state ($FF0000-$FF7FFF), the graphics ASIC ($FF8058-$FF8066), the CDD TOC/seek/pause/resume/jump
 * commands, the CDC host-data transfer ($FF8008/$A12008) and the level-1 (graphics completion) interrupt request - driven through
 * the runtime's own register paths (scd_ga_sub_access / scd_ga_main_access / scd_pcm_access via scd_write / scd_devices_*).
 *
 * Every expected value is derived here from the documented register behaviour (docs/architecture/sega-cd-machine-contract.md sections
 * 5, 7, 8 and 17; Genesis Plus GX where the references disagree), never from the implementation: the graphics rows compare the
 * ASIC's Word RAM output with an image computed geometrically by the independent oracle below (a logical stamp bitmap, sampled at
 * the positions the trace vector defines, written into the cell-column image buffer layout), plus literal hand-derived pixels.
 * Prints one line per row; exits 1 when a row fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

static int failures;
static ScdMachine *m;
static const ScdDiscTrack tracks[2] = {{1U, 0U, 0U, 2048U, 0U, 0U, 40U}, {2U, 2U, 1U, 2352U, 0U, 190U, 75U}};
static ScdDisc disc;
#define UNIT SCD_STOPWATCH_UNIT_TICKS

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

static void fresh(void) {
  memset(m, 0, sizeof *m);
  disc.tracks = tracks;
  disc.track_count = 2U;
  m->disc = &disc;
  m->ga.ctl = 0x02U;
  m->ga.sub_halted = 1U;
  m->ga.prg_window = 1U;
  m->ga.mode = SCD_MODE_RET;
  m->cpu[SCD_SUB].cycle_ticks = SCD_SUB_CYCLE_TICKS;
  scd_devices_power_on(m);
}

static int clear_stop(int status) {
  const int s = status == 0 ? 0 : (int)m->stop.stop_class;
  m->stop.stop_class = SCD_STOP_NONE;
  return s;
}
static int sub_access(uint32_t offset, uint32_t width, int is_write, uint32_t *value) {
  return clear_stop(scd_ga_sub_access(m, offset, width, is_write, value, 0x6000U));
}
static int sub_write(uint32_t offset, uint32_t width, uint32_t value) {
  uint32_t v = value;
  return sub_access(offset, width, 1, &v);
}
static uint32_t sub_read(uint32_t offset, uint32_t width) {
  uint32_t v = 0xDEADBEEFU;
  sub_access(offset, width, 0, &v);
  return v;
}
static int main_read_word(uint32_t offset, uint32_t *v) { return clear_stop(scd_ga_main_access(m, offset, 2U, 0, v, 0xFF0000U)); }
/* A full bus write/read as the generated code issues it (address within the sub's 1 MiB view). */
static int bus_write(uint32_t address, uint32_t width, uint32_t value) { return clear_stop(scd_write(m, SCD_SUB, address, width, value, 0x6000U)); }
static int bus_read(uint32_t address, uint32_t width, uint32_t *value) { return clear_stop(scd_read(m, SCD_SUB, address, width, value, 0x6000U)); }

static void run_devices_until(uint64_t limit) {
  for (;;) {
    const uint64_t t = scd_devices_next_event(m);
    if (t > limit) break;
    m->now = t;
    scd_devices_advance(m, t);
  }
  m->now = limit;
}

static void next_cdd_tick(void) {
  const uint64_t t = m->cdd.next_tick;
  m->now = t;
  scd_devices_advance(m, t);
}

/* ---- the independent graphics oracle ------------------------------------------------------------------------------------- */

/* A logical 16x16 stamp bitmap; stored into Word RAM in the documented layout: stamp number n at pixel index n << 8, its four 8x8
 * cells in column-major order (cell = cy + 2 * cx), pixels row-major inside a cell, two 4-bit pixels per byte, the left pixel in the
 * high nibble. */
static void store_stamp16(uint32_t number, uint8_t bitmap[16][16]) {
  uint32_t x, y;
  for (y = 0; y < 16U; ++y)
    for (x = 0; x < 16U; ++x) {
      const uint32_t index = (number << 8) | (((y / 8U) + 2U * (x / 8U)) << 6) | ((y % 8U) * 8U + (x % 8U));
      uint8_t *byte = &m->word_ram[index >> 1];
      if (index & 1U) *byte = (uint8_t)((*byte & 0xF0U) | (bitmap[y][x] & 0x0FU));
      else *byte = (uint8_t)((*byte & 0x0FU) | ((bitmap[y][x] & 0x0FU) << 4));
    }
}

static uint8_t stamp16[16][16];

/* The map entry's flip/roll bits (bit 15 reserved? no: bits 15-13 = HFLIP, ROLL1, ROLL0) applied to a position inside the stamp. */
static void orient(uint32_t bits, uint32_t *u, uint32_t *v) {
  uint32_t t;
  if (bits & 4U) *u = 15U - *u;                       /* horizontal flip, always first */
  if (bits & 2U) { *u = 15U - *u; *v = 15U - *v; }    /* half turn */
  if (bits & 1U) { t = *u; *u = 15U - *v; *v = t; }   /* quarter turn */
}

/* Expected 4-bit pixel at fixed-point position (x, y) in dots * 2048 on a 16x16-stamp map of 16x16 stamps (256x256 dots), or 0
 * outside it when the map does not repeat. `map[sy][sx]` are the map entries: stamp number in bits 10-0, orientation in bits 15-13. */
static uint32_t oracle_pixel(uint16_t map[16][16], int64_t x, int64_t y, int repeat) {
  int64_t dx = x >> 11, dy = y >> 11;
  uint32_t entry, u, v;
  if (repeat) { dx &= 255; dy &= 255; }
  else if (dx < 0 || dy < 0 || dx > 255 || dy > 255) return 0U;
  entry = map[dy >> 4][dx >> 4];
  if ((entry & 0x7FFU) == 0U) return 0U; /* stamp 0 is not used */
  u = (uint32_t)(dx & 15);
  v = (uint32_t)(dy & 15);
  orient((entry >> 13) & 7U, &u, &v);
  /* These rows use stamp number 1 only (the logical bitmap held in stamp16). */
  return (entry & 0x7FFU) == 1U ? (uint32_t)stamp16[v][u] : 0U;
}

/* 32x32-dot stamps (control size 1): a stamp is 16 cells in column-major order (cell = cy + 4 * cx); bits 1-0 of the stamp number are
 * ignored, so a stamp number n holds its bitmap at n & ~3. The map is 8x8 stamps (256x256 dots). */
static uint8_t stamp32[32][32];

static void store_stamp32(uint32_t number, uint8_t bitmap[32][32]) {
  uint32_t x, y;
  for (y = 0; y < 32U; ++y)
    for (x = 0; x < 32U; ++x) {
      const uint32_t index = (number << 8) | (((y / 8U) + 4U * (x / 8U)) << 6) | ((y % 8U) * 8U + (x % 8U));
      uint8_t *byte = &m->word_ram[index >> 1];
      if (index & 1U) *byte = (uint8_t)((*byte & 0xF0U) | (bitmap[y][x] & 0x0FU));
      else *byte = (uint8_t)((*byte & 0x0FU) | ((bitmap[y][x] & 0x0FU) << 4));
    }
}

static uint32_t oracle_pixel32(uint16_t map[8][8], int64_t x, int64_t y, int repeat) {
  int64_t dx = x >> 11, dy = y >> 11;
  uint32_t entry, u, v, t, bits;
  if (repeat) { dx &= 255; dy &= 255; }
  else if (dx < 0 || dy < 0 || dx > 255 || dy > 255) return 0U;
  entry = map[dy >> 5][dx >> 5];
  if ((entry & 0x7FCU) == 0U) return 0U; /* bits 1-0 of the number are ignored; the number 0 draws nothing */
  u = (uint32_t)(dx & 31);
  v = (uint32_t)(dy & 31);
  bits = (entry >> 13) & 7U;
  if (bits & 4U) u = 31U - u;
  if (bits & 2U) { u = 31U - u; v = 31U - v; }
  if (bits & 1U) { t = u; u = 31U - v; v = t; }
  return (entry & 0x7FCU) == 4U ? (uint32_t)stamp32[v][u] : 0U;
}

static uint32_t buffer_pixel(uint32_t start_dots, uint32_t cells_tall, uint32_t x, uint32_t y) {
  const uint32_t index = start_dots + (x / 8U) * cells_tall * 64U + (y / 8U) * 64U + (y % 8U) * 8U + (x % 8U);
  const uint8_t byte = m->word_ram[(index >> 1) & 0x3FFFFU];
  return (index & 1U) ? (byte & 0x0FU) : (uint32_t)(byte >> 4);
}

static void put_map_entries(uint32_t byte_base, uint16_t map[16][16]) {
  uint32_t i;
  for (i = 0; i < 256U; ++i) {
    m->word_ram[byte_base + 2U * i] = (uint8_t)(map[i / 16U][i % 16U] >> 8);
    m->word_ram[byte_base + 2U * i + 1U] = (uint8_t)map[i / 16U][i % 16U];
  }
}

static void put_trace_line(uint32_t byte_base, uint32_t line, uint32_t xs, uint32_t ys, int32_t dxs, int32_t dys) {
  const uint32_t at = byte_base + 8U * line;
  m->word_ram[at] = (uint8_t)(xs >> 8); m->word_ram[at + 1U] = (uint8_t)xs;
  m->word_ram[at + 2U] = (uint8_t)(ys >> 8); m->word_ram[at + 3U] = (uint8_t)ys;
  m->word_ram[at + 4U] = (uint8_t)((uint32_t)dxs >> 8); m->word_ram[at + 5U] = (uint8_t)dxs;
  m->word_ram[at + 6U] = (uint8_t)((uint32_t)dys >> 8); m->word_ram[at + 7U] = (uint8_t)dys;
}

static void gfx_registers(uint32_t ctl, uint32_t map_base, uint32_t cell_v, uint32_t origin, uint32_t offset, uint32_t h, uint32_t v) {
  sub_write(0x58U, 2U, ctl);
  sub_write(0x5AU, 2U, map_base);
  sub_write(0x5CU, 2U, cell_v);
  sub_write(0x5EU, 2U, origin);
  sub_write(0x60U, 2U, offset);
  sub_write(0x62U, 2U, h);
  sub_write(0x64U, 2U, v);
}

int main(void) {
  uint32_t v, w;
  m = calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  /* ================================= stopwatch ($FF800C) ================================= */
  fresh();
  m->now = 0U;
  row(sub_read(0x0CU, 2U) == 0U, "stopwatch: reset value 0 at power-on");
  m->now = UNIT - 1U;
  row(sub_read(0x0CU, 2U) == 0U && sub_read(0x0CU, 2U) == 0U, "stopwatch: one tick before the first unit it still reads 0, repeated reads agree");
  m->now = UNIT;
  row(sub_read(0x0CU, 2U) == 1U, "stopwatch: 1 after 384 sub cycles (30.72 us)");
  m->now = 5U * UNIT + 7U;
  row(sub_read(0x0CU, 2U) == 5U && sub_read(0x0CU, 2U) == 5U, "stopwatch: 5 after five units; repeated reads inside one unit are equal");
  m->now = 4095U * UNIT;
  row(sub_read(0x0CU, 2U) == 4095U, "stopwatch: 4095 is the largest value");
  m->now = 4096U * UNIT;
  row(sub_read(0x0CU, 2U) == 0U, "stopwatch: rolls over from 4095 to 0 (12 bits)");
  m->now = 4097U * UNIT + 3U;
  row(sub_read(0x0CU, 2U) == 1U, "stopwatch: ... and continues counting after the rollover");
  m->now = 10U * UNIT + UNIT / 2U;
  row(sub_write(0x0CU, 2U, 0x1234U) == 0 && sub_read(0x0CU, 2U) == 0U, "stopwatch: a word write of any value clears the count");
  m->now = 11U * UNIT - 1U;
  row(sub_read(0x0CU, 2U) == 0U, "stopwatch: after a write the grid phase is kept (the count is still 0 inside the unit)");
  m->now = 11U * UNIT;
  row(sub_read(0x0CU, 2U) == 1U, "stopwatch: ... and becomes 1 at the next unit boundary of the free-running grid");
  {
    const uint64_t ignored = m->ignored_writes;
    const uint32_t base = m->ga.stopwatch_base;
    row(sub_write(0x0CU, 1U, 0U) == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED && m->ga.stopwatch_base == base && m->ignored_writes == ignored,
        "stopwatch: a byte write is the typed stop gate_array_register_unsupported and changes nothing");
    m->stop.stop_class = SCD_STOP_NONE;
    scd_ga_sub_access(m, 0x0DU, 1U, 0, &v, 0x6A00U);
    row(m->stop.stop_class == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED && m->stop.cpu == SCD_SUB && m->stop.pc == 0x6A00U &&
            m->stop.address == 0xFF800DU && m->stop.width == 1U && m->stop.is_write == 0U,
        "stopwatch: a byte read stops typed with the sub, the pc, the address, the width and the direction");
    m->stop.stop_class = SCD_STOP_NONE;
    scd_ga_sub_access(m, 0x0DU, 2U, 1, &w, 0x6A00U);
    row(m->stop.stop_class == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED && m->stop.address == 0xFF800DU && m->stop.width == 2U,
        "stopwatch: a word access at the odd offset is not the register either");
    m->stop.stop_class = SCD_STOP_NONE;
  }
  m->now = 100U * UNIT + 5U;
  row(sub_write(0x01U, 1U, 0x00U) == 0, "stopwatch: RES0 = 0 (peripheral reset) ...");
  row(sub_read(0x0CU, 2U) == 0U && m->ga.stopwatch_origin == m->now, "... clears the count and restarts the grid at that instant");
  m->now += UNIT - 1U;
  row(sub_read(0x0CU, 2U) == 0U, "... the first count comes one unit after the reset");
  m->now += 1U;
  row(sub_read(0x0CU, 2U) == 1U, "... exactly one unit after it");
  /* deadline partitioning: the value is a pure function of the machine time, never of how the run was sliced */
  {
    uint32_t a = 0U, b = 0U;
    m->now = 4242U * UNIT + 11U;
    a = sub_read(0x0CU, 2U);
    run_devices_until(m->now + 123456789U);
    m->now = 4242U * UNIT + 11U;
    b = sub_read(0x0CU, 2U);
    row(a == b, "stopwatch: device events processed in between do not change what a read at the same instant returns");
  }

  /* ================================= PCM write state ================================= */
  fresh();
  row(bus_write(0xFF000FU, 1U, 0xC3U) == 0 && m->pcm.index == 3U && m->pcm.enabled == 1U,
      "PCM: CTRL ($FF000F, offset 7) with bit 6 selects channel 3; bit 7 enables the chip");
  row(bus_write(0xFF0001U, 1U, 0x7FU) == 0 && bus_write(0xFF0003U, 1U, 0x55U) == 0 && m->pcm.env[3] == 0x7FU && m->pcm.pan[3] == 0x55U &&
          m->pcm.env[0] == 0U && m->pcm.pan[0] == 0U,
      "PCM: ENV and PAN go to the selected channel only");
  row(bus_write(0xFF0005U, 1U, 0x34U) == 0 && bus_write(0xFF0007U, 1U, 0x12U) == 0 && m->pcm.fd[3] == 0x1234U, "PCM: FD low then high form 0x1234");
  row(bus_write(0xFF0009U, 1U, 0xCDU) == 0 && bus_write(0xFF000BU, 1U, 0xABU) == 0 && m->pcm.ls[3] == 0xABCDU, "PCM: LS low then high form 0xABCD");
  row(bus_write(0xFF000DU, 1U, 0x20U) == 0 && m->pcm.st[3] == 0x20U, "PCM: ST holds the start page");
  row(bus_write(0xFF0011U, 1U, 0xF7U) == 0 && m->pcm.onoff == 0xF7U, "PCM: ON/OFF ($FF0011) is stored as written");
  row(bus_write(0xFF0013U, 1U, 0x99U) == 0 && m->pcm.onoff == 0xF7U && m->pcm.env[3] == 0x7FU, "PCM: an illegal register offset has no effect");
  row(bus_write(0xFF000FU, 1U, 0x05U) == 0 && m->pcm.bank == 5U && m->pcm.index == 3U && m->pcm.enabled == 0U,
      "PCM: CTRL with bit 6 clear selects the wave RAM bank (5), keeps the channel, clears the enable bit");
  row(bus_write(0xFF2001U, 1U, 0xA1U) == 0 && m->pcm.ram[5U * 0x1000U] == 0xA1U && bus_write(0xFF3FFFU, 1U, 0xB2U) == 0 &&
          m->pcm.ram[5U * 0x1000U + 0xFFFU] == 0xB2U,
      "PCM: wave RAM writes land in the selected 4 KiB bank (first and last byte of the window)");
  row(bus_write(0xFF000FU, 1U, 0x0AU) == 0 && bus_write(0xFF2003U, 1U, 0xC3U) == 0 && m->pcm.ram[10U * 0x1000U + 1U] == 0xC3U &&
          m->pcm.ram[5U * 0x1000U + 1U] == 0U,
      "PCM: another bank (10) receives the next byte; bank 5 is untouched");
  {
    const uint64_t ignored = m->ignored_writes;
    row(bus_write(0xFF0000U, 1U, 0x66U) == 0 && m->ignored_writes == ignored + 1U && m->pcm.env[3] == 0x7FU,
        "PCM: a byte write at an even address (/LDS not asserted) is an unused-area write, dropped and counted");
  }
  row(bus_write(0xFF2000U, 2U, 0x1234U) == 0 && m->pcm.ram[10U * 0x1000U] == 0x34U, "PCM: a word write stores its low byte (GPGX scd_write_word)");
  {
    const ScdPcm before = m->pcm;
    int stop;
    stop = bus_read(0xFF0021U, 1U, &v);
    row(stop == SCD_STOP_PCM_UNSUPPORTED && memcmp(&before, &m->pcm, sizeof before) == 0,
        "PCM: a register read (channel address pointers need running channels) is the typed stop pcm_unsupported and mutates nothing");
    scd_read(m, SCD_SUB, 0xFF2001U, 1U, &v, 0x6B00U);
    row(m->stop.stop_class == SCD_STOP_PCM_UNSUPPORTED && m->stop.cpu == SCD_SUB && m->stop.pc == 0x6B00U && m->stop.address == 0xFF2001U &&
            m->stop.width == 1U && m->stop.is_write == 0U,
        "PCM: a wave RAM read stops typed with the sub, pc, address, width and direction");
    m->stop.stop_class = SCD_STOP_NONE;
  }

  /* ================================= graphics ASIC ================================= */
  {
    uint16_t map[16][16];
    uint32_t x, y, i;
#define BUFFER_ORIGIN_REG 0x1000U /* $FF805E: image buffer start (dots >> 3); the dots start at 0x8000 */
#define BUFFER_DOTS 0x8000U
    for (y = 0; y < 16U; ++y)
      for (x = 0; x < 16U; ++x) stamp16[y][x] = (uint8_t)(((x * 3U + y * 5U) % 15U) + 1U); /* asymmetric, never 0 */
    for (y = 0; y < 16U; ++y)
      for (x = 0; x < 16U; ++x) map[y][x] = 1U;

    /* -- identity: 16 dots x 16 lines at one dot per dot -- */
    fresh();
    m->ga.mode = 0U; /* 2M, the sub owns Word RAM */
    store_stamp16(1U, stamp16);
    put_map_entries(0x20000U, map);
    for (i = 0; i < 16U; ++i) put_trace_line(0x30000U, i, 0U, i * 8U, 0x0800, 0);
    m->now = 1000U;
    gfx_registers(0x0000U, 0x8000U, 1U, BUFFER_ORIGIN_REG, 0U, 16U, 16U); /* map base 0x20000, 2 cells tall, W = 16, 16 lines */
    row(sub_read(0x58U, 2U) == 0x0000U, "graphics: idle, $FF8058 reads busy = 0 with the control bits");
    row(sub_write(0x66U, 2U, 0xC000U) == 0 && sub_read(0x58U, 2U) == 0x8000U && m->gfx.busy == 1U,
        "graphics: writing the trace base starts the operation (busy bit 15)");
    {
      /* cost per line: 3 sub cycles * (4 + 2 * 16 + ((16 + 0 + 3) >> 2)) = 3 * 40 = 120; the operation ends after 16 lines */
      const uint64_t line = 120U * SCD_SUB_CYCLE_TICKS;
      const uint64_t done = 1000U + 15U * line + 1U;
      row(scd_gfx_next_event(m) == done, "graphics: the end is 15 lines + 1 tick after the start (a line renders as soon as it starts)");
      m->now = done - 1U;
      scd_gfx_sync(m, m->now);
      row(m->gfx.busy == 1U && m->gfx.lines_left == 1U, "graphics: one tick before the end the last line is outstanding");
      run_devices_until(done);
      row(m->gfx.busy == 0U && m->gfx.lines_left == 0U && sub_read(0x58U, 2U) == 0x0000U, "graphics: at the end the busy bit clears and no line remains");
    }
    {
      int same = 1;
      for (y = 0; y < 16U && same; ++y)
        for (x = 0; x < 16U; ++x)
          if (buffer_pixel(BUFFER_DOTS, 2U, x, y) != stamp16[y][x]) { same = 0; break; }
      row(same, "graphics: identity transform reproduces the stamp bitmap in the cell-column image buffer");
      row(buffer_pixel(BUFFER_DOTS, 2U, 0U, 0U) == 1U && buffer_pixel(BUFFER_DOTS, 2U, 1U, 0U) == 4U && buffer_pixel(BUFFER_DOTS, 2U, 0U, 1U) == 6U &&
              buffer_pixel(BUFFER_DOTS, 2U, 14U, 15U) == 13U && m->word_ram[BUFFER_DOTS >> 1] == 0x14U,
          "graphics: literal pixels (hand-derived (3x + 5y) mod 15 + 1: 1, 4, 6, 13; two dots per byte, the left dot in the high nibble: $14)");
    }

    /* -- the oracle against several transforms: scale, rotation through the trace, flips and rolls through the map, repeat/clipping -- */
    {
      static const struct {
        const char *label;
        int32_t dx, dy;           /* per-dot steps (5.11) */
        int32_t line_dx, line_dy; /* per-line start steps, in dots * 8 (13.3) */
        uint32_t entry_bits;      /* orientation bits of every map entry (bit 2 HFLIP, bit 1 ROLL1, bit 0 ROLL0) */
        int repeat;
        int32_t start_x, start_y; /* start position in dots * 8 */
        uint32_t h, lines;
      } cases[] = {
          {"2x magnification (half a dot per dot)", 0x0400, 0, 0, 4, 0, 0, 0, 0, 32U, 32U},
          {"rotation by 90 degrees through the trace (dot step along y, line step along x)", 0, 0x0800, 8, 0, 0, 0, 0, 0, 16U, 16U},
          {"rotation by 45 degrees (both steps)", 0x05A8, 0x05A8, -0x2D, 0x2D, 0, 0, 128 * 8, 0, 20U, 20U},
          {"horizontal flip of every stamp", 0x0800, 0, 0, 8, 4, 0, 0, 0, 16U, 16U},
          {"half turn of every stamp", 0x0800, 0, 0, 8, 2, 0, 0, 0, 16U, 16U},
          {"quarter turn of every stamp", 0x0800, 0, 0, 8, 1, 0, 0, 0, 16U, 16U},
          {"flip then quarter turn", 0x0800, 0, 0, 8, 5, 0, 0, 0, 16U, 16U},
          {"all three orientation bits", 0x0800, 0, 0, 8, 7, 0, 0, 0, 16U, 16U},
          {"clipping: a start left of and above the map, no repeat (dots outside are 0)", 0x0800, 0, 0, 8, 0, 0, -64, -64, 24U, 24U},
          {"repeat: the same start wraps around the 256-dot map", 0x0800, 0, 0, 8, 0, 1, -64, -64, 24U, 24U},
          {"clipping at the far edge: nothing is drawn past 256 dots when the map does not repeat", 0x0800, 0, 0, 8, 0, 0, 248 * 8, 250 * 8, 24U, 16U},
      };
      uint32_t c;
      for (c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        int same = 1;
        uint32_t px, py, mismatches = 0U, nonzero = 0U;
        fresh();
        m->ga.mode = 0U;
        store_stamp16(1U, stamp16);
        for (y = 0; y < 16U; ++y)
          for (x = 0; x < 16U; ++x) map[y][x] = (uint16_t)(1U | (cases[c].entry_bits << 13));
        put_map_entries(0x20000U, map);
        for (i = 0; i < cases[c].lines; ++i)
          put_trace_line(0x30000U, i, (uint32_t)(cases[c].start_x + (int32_t)i * cases[c].line_dx) & 0xFFFFU,
                         (uint32_t)(cases[c].start_y + (int32_t)i * cases[c].line_dy) & 0xFFFFU, cases[c].dx, cases[c].dy);
        m->now = 5000U;
        gfx_registers(cases[c].repeat ? 0x0001U : 0x0000U, 0x8000U, 3U, BUFFER_ORIGIN_REG, 0U, cases[c].h, cases[c].lines); /* 4 cells tall */
        sub_write(0x66U, 2U, 0xC000U);
        run_devices_until(5000U + (uint64_t)cases[c].lines * 3U * (4U + 2U * cases[c].h + ((cases[c].h + 3U) >> 2)) * SCD_SUB_CYCLE_TICKS + 10U);
        for (py = 0; py < cases[c].lines; ++py)
          for (px = 0; px < cases[c].h; ++px) {
            const int64_t xpos = ((int64_t)(cases[c].start_x + (int32_t)py * cases[c].line_dx) * 256) + (int64_t)px * cases[c].dx;
            const int64_t ypos = ((int64_t)(cases[c].start_y + (int32_t)py * cases[c].line_dy) * 256) + (int64_t)px * cases[c].dy;
            const uint32_t expected = oracle_pixel(map, xpos, ypos, cases[c].repeat);
            if (buffer_pixel(BUFFER_DOTS, 4U, px, py) != expected) { same = 0; ++mismatches; }
            if (expected) ++nonzero;
          }
        {
          char label[240];
          snprintf(label, sizeof label, "graphics oracle: %s (%u of %u dots drawn, %u mismatching)", cases[c].label, nonzero,
                   cases[c].h * cases[c].lines, mismatches);
          row(same && m->gfx.busy == 0U && m->gfx.operations == 1U && nonzero != 0U, label);
        }
      }
    }

    /* -- 32x32-dot stamps against the oracle: the stamp number's bits 1-0 are ignored (an entry of 5 draws stamp 4) -- */
    {
      static const struct { const char *label; uint32_t entry_bits; int repeat; int32_t start; } cases32[] = {
          {"32x32 stamps: identity, entry number 5 reads stamp 4 (bits 1-0 ignored)", 0U, 0, 0},
          {"32x32 stamps: half turn and flip (entry bits 6)", 6U, 0, 0},
          {"32x32 stamps: quarter turn, repeated map, start left of it", 1U, 1, -48 * 8},
      };
      uint32_t c;
      for (c = 0; c < sizeof cases32 / sizeof cases32[0]; ++c) {
        uint16_t map8[8][8];
        int same = 1;
        uint32_t px, py, bad = 0U, nonzero = 0U;
        for (y = 0; y < 32U; ++y)
          for (x = 0; x < 32U; ++x) stamp32[y][x] = (uint8_t)(((x * 7U + y * 3U) % 15U) + 1U);
        for (y = 0; y < 8U; ++y)
          for (x = 0; x < 8U; ++x) map8[y][x] = (uint16_t)(5U | (cases32[c].entry_bits << 13));
        fresh();
        m->ga.mode = 0U;
        store_stamp32(4U, stamp32);
        for (y = 0; y < 8U; ++y)
          for (x = 0; x < 8U; ++x) {
            m->word_ram[0x20000U + 2U * (y * 8U + x)] = (uint8_t)(map8[y][x] >> 8);
            m->word_ram[0x20000U + 2U * (y * 8U + x) + 1U] = (uint8_t)map8[y][x];
          }
        for (i = 0; i < 40U; ++i) put_trace_line(0x30000U, i, (uint32_t)cases32[c].start & 0xFFFFU, (uint32_t)(cases32[c].start + (int32_t)i * 8) & 0xFFFFU, 0x0800, 0);
        m->now = 5000U;
        gfx_registers((cases32[c].repeat ? 0x0001U : 0x0000U) | 0x0002U, 0x8000U, 4U, BUFFER_ORIGIN_REG, 0U, 40U, 40U); /* 5 cells tall */
        sub_write(0x66U, 2U, 0xC000U);
        run_devices_until(5000U + 40U * 3U * (4U + 80U + ((40U + 3U) >> 2)) * SCD_SUB_CYCLE_TICKS + 10U);
        for (py = 0; py < 40U; ++py)
          for (px = 0; px < 40U; ++px) {
            const int64_t xpos = (int64_t)cases32[c].start * 256 + (int64_t)px * 0x0800;
            const int64_t ypos = (int64_t)(cases32[c].start + (int32_t)py * 8) * 256;
            const uint32_t expected = oracle_pixel32(map8, xpos, ypos, cases32[c].repeat);
            if (buffer_pixel(BUFFER_DOTS, 5U, px, py) != expected) { same = 0; ++bad; }
            if (expected) ++nonzero;
          }
        {
          char label[240];
          snprintf(label, sizeof label, "graphics oracle: %s (%u of 1600 dots drawn, %u mismatching)", cases32[c].label, nonzero, bad);
          row(same && nonzero != 0U && m->gfx.operations == 1U, label);
        }
      }
      for (y = 0; y < 16U; ++y)
        for (x = 0; x < 16U; ++x) stamp16[y][x] = (uint8_t)(((x * 3U + y * 5U) % 15U) + 1U);
    }

    /* -- literal hand-derived: a half-turn map entry puts the stamp's opposite corner first; stamp 0 draws nothing -- */
    fresh();
    m->ga.mode = 0U;
    store_stamp16(1U, stamp16);
    for (y = 0; y < 16U; ++y)
      for (x = 0; x < 16U; ++x) map[y][x] = (uint16_t)(1U | (2U << 13));
    put_map_entries(0x20000U, map);
    for (i = 0; i < 16U; ++i) put_trace_line(0x30000U, i, 16U * 8U, (16U + i) * 8U, 0x0800, 0); /* start at dot (16, 16): the second stamp */
    m->now = 100U;
    gfx_registers(0x0000U, 0x8000U, 1U, BUFFER_ORIGIN_REG, 0U, 16U, 16U);
    sub_write(0x66U, 2U, 0xC000U);
    run_devices_until(100U + 16U * 120U * SCD_SUB_CYCLE_TICKS);
    row(buffer_pixel(BUFFER_DOTS, 2U, 0U, 0U) == stamp16[15][15] && buffer_pixel(BUFFER_DOTS, 2U, 15U, 15U) == stamp16[0][0],
        "graphics: with a half-turn map entry the first dot is the stamp's opposite corner (literal)");
    fresh();
    m->ga.mode = 0U;
    store_stamp16(1U, stamp16);
    memset(map, 0, sizeof map);
    put_map_entries(0x20000U, map);
    memset(&m->word_ram[BUFFER_DOTS >> 1], 0xEE, 64U);
    for (i = 0; i < 8U; ++i) put_trace_line(0x30000U, i, 0U, i * 8U, 0x0800, 0);
    m->now = 100U;
    gfx_registers(0x0000U, 0x8000U, 1U, BUFFER_ORIGIN_REG, 0U, 8U, 8U);
    sub_write(0x66U, 2U, 0xC000U);
    run_devices_until(100U + 8U * 120U * SCD_SUB_CYCLE_TICKS);
    row(buffer_pixel(BUFFER_DOTS, 2U, 3U, 3U) == 0U && m->word_ram[BUFFER_DOTS >> 1] == 0x00U,
        "graphics: stamp 0 is not used, the image buffer dots become 0 (normal priority)");

    /* -- priority modes (PM1:PM0 of the memory-mode register): a buffer dot E, an incoming stamp dot D -- */
    {
      static const struct { uint32_t pm, existing, incoming, expected; const char *label; } rows[] = {
          {0U, 0x5U, 0x9U, 0x9U, "normal: the new nibble replaces the old one"},
          {0U, 0x5U, 0x0U, 0x0U, "normal: a zero new nibble replaces it too"},
          {1U, 0x5U, 0x9U, 0x5U, "underwrite: a nonzero existing nibble stays"},
          {1U, 0x0U, 0x9U, 0x9U, "underwrite: a zero existing nibble takes the new one"},
          {2U, 0x5U, 0x9U, 0x9U, "overwrite: a nonzero new nibble wins"},
          {2U, 0x5U, 0x0U, 0x5U, "overwrite: a zero new nibble leaves the old one"},
          {3U, 0x5U, 0x9U, 0x5U, "mode 3 (invalid) keeps the buffer"},
      };
      uint32_t r;
      for (r = 0; r < sizeof rows / sizeof rows[0]; ++r) {
        uint8_t one[16][16];
        uint16_t one_map[16][16];
        memset(one, 0, sizeof one);
        one[0][0] = (uint8_t)rows[r].incoming;
        fresh();
        m->ga.mode = (uint8_t)(rows[r].pm << 3); /* 2M, sub-owned, priority mode PM1:PM0 */
        store_stamp16(1U, one);
        memset(one_map, 0, sizeof one_map);
        one_map[0][0] = 1U;
        put_map_entries(0x20000U, one_map);
        m->word_ram[BUFFER_DOTS >> 1] = (uint8_t)(rows[r].existing << 4);
        put_trace_line(0x30000U, 0U, 0U, 0U, 0x0800, 0);
        m->now = 100U;
        gfx_registers(0x0000U, 0x8000U, 1U, BUFFER_ORIGIN_REG, 0U, 1U, 1U);
        sub_write(0x66U, 2U, 0xC000U);
        run_devices_until(100U + 30U * SCD_SUB_CYCLE_TICKS);
        {
          char label[160];
          snprintf(label, sizeof label, "graphics priority: %s (got %X)", rows[r].label, (unsigned)(m->word_ram[BUFFER_DOTS >> 1] >> 4));
          row((uint32_t)(m->word_ram[BUFFER_DOTS >> 1] >> 4) == rows[r].expected, label);
        }
      }
    }

    /* -- control bits -- */
    fresh();
    m->ga.mode = 0U;
    sub_write(0x58U, 2U, 0x0002U);
    row(m->gfx.ctl == 2U, "graphics: $FF8058 stores the three control bits (32x32 stamps)");
    sub_write(0x58U, 2U, 0x8007U);
    row(m->gfx.ctl == 7U && sub_read(0x58U, 2U) == 0x0007U, "graphics: bit 15 of a $FF8058 write is the read-only busy flag, not stored");

    /* -- ownership: the operation is halted while main owns 2M Word RAM, and resumes from the hand-over -- */
    fresh();
    store_stamp16(1U, stamp16);
    m->ga.mode = 0U;
    put_map_entries(0x20000U, map);
    for (y = 0; y < 16U; ++y)
      for (x = 0; x < 16U; ++x) map[y][x] = 1U;
    put_map_entries(0x20000U, map);
    for (i = 0; i < 16U; ++i) put_trace_line(0x30000U, i, 0U, i * 8U, 0x0800, 0);
    m->now = 1000U;
    gfx_registers(0x0000U, 0x8000U, 1U, BUFFER_ORIGIN_REG, 0U, 16U, 16U);
    sub_write(0x66U, 2U, 0xC000U);
    m->now = 1000U + 2U * 120U * SCD_SUB_CYCLE_TICKS;
    row(sub_write(0x03U, 1U, 0x01U) == 0 && m->gfx.lines_left == 14U,
        "graphics: handing 2M Word RAM back (RET = 1) first brings the operation up to date (2 of 16 lines done)");
    row(scd_gfx_next_event(m) == UINT64_MAX, "graphics: while main owns Word RAM there is no completion event");
    m->now = 9000000U;
    scd_gfx_sync(m, m->now);
    row(m->gfx.lines_left == 14U && m->gfx.busy == 1U, "graphics: no line is processed while halted, however much time passes");
    v = 0x02U;
    row(clear_stop(scd_ga_main_access(m, 0x03U, 1U, 1, &v, 0xFF0000U)) == 0 && (m->ga.mode & SCD_MODE_RET) == 0U, "graphics: main writes DMNA = 1 (the sub owns Word RAM again)");
    row(scd_gfx_next_event(m) == 9000000U + 13U * 120U * SCD_SUB_CYCLE_TICKS + 1U, "graphics: ... the remaining 14 lines are counted from the hand-over");

    /* -- the level-1 request: raised at the end only with IEN1; cleared when disabled -- */
    for (i = 0; i < 2U; ++i) {
      fresh();
      m->ga.mode = 0U;
      store_stamp16(1U, stamp16);
      put_map_entries(0x20000U, map);
      put_trace_line(0x30000U, 0U, 0U, 0U, 0x0800, 0);
      sub_write(0x33U, 1U, i ? 0x02U : 0x00U); /* IEN1 */
      m->now = 10U;
      gfx_registers(0x0000U, 0x8000U, 1U, BUFFER_ORIGIN_REG, 0U, 8U, 1U);
      sub_write(0x66U, 2U, 0xC000U);
      row((m->ga.pending & 0x02U) == 0U && scd_devices_can_raise(m, 1U) == (int)i, i ? "graphics level 1: nothing is pending while it runs; it can still be raised (IEN1 = 1)" : "graphics level 1: nothing is pending at the start; it cannot be raised (IEN1 = 0)");
      run_devices_until(10U + 100U * SCD_SUB_CYCLE_TICKS);
      row(((m->ga.pending & 0x02U) != 0U) == (i == 1U) && m->gfx.operations == 1U,
          i ? "graphics level 1: IEN1 = 1 -> level 1 pending at the end" : "graphics level 1: IEN1 = 0 -> never pending, the operation still completes");
    }
    row(scd_devices_can_raise(m, 1U) == 0, "graphics level 1: an idle ASIC cannot raise it (the STOP wake rule)");
    sub_write(0x33U, 1U, 0x00U);
    row((m->ga.pending & 0x02U) == 0U, "graphics level 1: disabling IEN1 drops a pending request");

    /* -- unsupported shapes: typed, nothing mutated -- */
    fresh();
    m->ga.mode = SCD_MODE_1M;
    row(sub_write(0x66U, 2U, 0x1000U) == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED && m->gfx.busy == 0U,
        "graphics: starting an operation in 1M Word RAM mode is the typed stop gate_array_register_unsupported and does not start");
    fresh();
    row(sub_write(0x5AU, 1U, 0x12U) == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED && m->gfx.map_base == 0U,
        "graphics: a byte write to a graphics register is a typed stop and changes nothing");
    v = 0U;
    row(sub_access(0x5AU, 2U, 0, &v) == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED && sub_access(0x66U, 2U, 0, &v) == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED,
        "graphics: reads of the write-only parameter registers are typed stops");
    row(sub_read(0x58U, 1U) == 0U, "graphics: the busy byte reads 0 when idle");
    fresh();
    m->ga.mode = 0U;
    m->now = 50U;
    gfx_registers(0, 0x8000U, 1U, 0, 0, 64U, 100U);
    sub_write(0x66U, 2U, 0xC000U);
    row(m->gfx.busy == 1U, "graphics: a long operation is running ...");
    m->now = 60U;
    row(sub_write(0x01U, 1U, 0x00U) == 0 && m->gfx.busy == 0U && scd_gfx_next_event(m) == UINT64_MAX,
        "graphics: the peripheral reset abandons a running operation (the register file above $FF8030 clears)");
  }

  /* ================================= CDD TOC / seek / pause / resume / jump ================================= */
  fresh();
  m->now = 0U;
#define CDD_TX(c, d1, d2, d3, d4, d5, d6, d7) \
  do { \
    sub_write(0x42U, 2U, ((c) << 8) | (d1)); \
    sub_write(0x44U, 2U, ((d2) << 8) | (d3)); \
    sub_write(0x46U, 2U, ((d4) << 8) | (d5)); \
    sub_write(0x48U, 2U, ((d6) << 8) | (d7)); \
    w = (uint32_t)sub_write(0x4AU, 2U, 0U); \
  } while (0)
  {
    uint32_t sum;
    int i;
    CDD_TX(2U, 0U, 0U, 3U, 0U, 0U, 0U, 0U);
    row(w == 0U && m->cdd.rx[0] == SCD_CDD_TOC_STATUS && m->cdd.rx[1] == 3U && m->cdd.rx[2] == 0U && m->cdd.rx[3] == 0U && m->cdd.rx[4] == 0U &&
            m->cdd.rx[5] == 5U && m->cdd.rx[6] == 4U && m->cdd.rx[7] == 0U && m->cdd.rx[8] == 0U,
        "CDD TOC 3: total length 0:05:40 (end of the last track + 150) in RS2-RS7, RS0 the drive status, RS1 the request");
    for (sum = 0U, i = 0; i < 9; ++i) sum += m->cdd.rx[i];
    row(m->cdd.rx[9] == (uint8_t)(~sum & 0x0FU), "CDD TOC 3: RS9 is the complemented sum of RS0-RS8");
    CDD_TX(2U, 0U, 0U, 4U, 0U, 0U, 0U, 0U);
    row(m->cdd.rx[1] == 4U && m->cdd.rx[2] == 0U && m->cdd.rx[3] == 1U && m->cdd.rx[4] == 0U && m->cdd.rx[5] == 2U && m->cdd.rx[6] == 0U &&
            m->cdd.rx[7] == 0U && m->cdd.rx[8] == 0U,
        "CDD TOC 4: first track 1 and last track 2 (BCD digits in RS2-RS3 and RS4-RS5)");
    CDD_TX(2U, 0U, 0U, 5U, 0U, 1U, 0U, 0U);
    row(m->cdd.rx[1] == 5U && m->cdd.rx[2] == 0U && m->cdd.rx[3] == 0U && m->cdd.rx[4] == 0U && m->cdd.rx[5] == 2U && m->cdd.rx[6] == 8U &&
            m->cdd.rx[7] == 0U && m->cdd.rx[8] == 1U,
        "CDD TOC 5: track 1 starts at 0:02:00 (LBA 0 + 150); RS6 bit 3 marks the data track; RS8 the track number");
    CDD_TX(2U, 0U, 0U, 5U, 0U, 2U, 0U, 0U);
    row(m->cdd.rx[2] == 0U && m->cdd.rx[3] == 0U && m->cdd.rx[4] == 0U && m->cdd.rx[5] == 4U && m->cdd.rx[6] == 4U && m->cdd.rx[7] == 0U &&
            m->cdd.rx[8] == 2U,
        "CDD TOC 5: audio track 2 starts at 0:04:40 (LBA 190 + 150 = 340 blocks), no data-track flag");
    CDD_TX(2U, 0U, 0U, 5U, 0U, 3U, 0U, 0U);
    row(m->cdd.rx[4] == 0U && m->cdd.rx[5] == 5U && m->cdd.rx[6] == 4U && m->cdd.rx[7] == 0U && m->cdd.rx[8] == 3U,
        "CDD TOC 5: the next track number is the lead-out, at the end of the disc (0:05:40)");
    {
      uint8_t before[10];
      memcpy(before, m->cdd.rx, 10U);
      CDD_TX(2U, 0U, 0U, 5U, 0U, 0U, 0U, 0U);
      row(w == SCD_STOP_CDD_COMMAND_UNSUPPORTED && memcmp(before, m->cdd.rx, 10U) == 0,
          "CDD TOC 5: track 0 is the typed stop cdd_command_unsupported; the status registers are unchanged");
      CDD_TX(2U, 0U, 0U, 5U, 0U, 4U, 0U, 0U);
      row(w == SCD_STOP_CDD_COMMAND_UNSUPPORTED && memcmp(before, m->cdd.rx, 10U) == 0, "CDD TOC 5: a track beyond the lead-out is the typed stop too");
      CDD_TX(2U, 0U, 0U, 7U, 0U, 0U, 0U, 0U);
      row(w == 0U && memcmp(before, m->cdd.rx, 10U) == 0, "CDD TOC 7 (invalid request code): nothing is updated and no checksum is computed");
    }
    CDD_TX(2U, 0U, 0U, 6U, 0U, 0U, 0U, 0U);
    row(m->cdd.rx[1] == 6U && m->cdd.rx[2] == 0U && m->cdd.rx[3] == 0U && m->cdd.rx[4] == 0U && m->cdd.rx[8] == 0U, "CDD TOC 6: latest error information is empty");
    CDD_TX(2U, 0U, 0U, 2U, 0U, 0U, 0U, 0U);
    row(m->cdd.rx[1] == 2U && m->cdd.rx[2] == 0U && m->cdd.rx[3] == 1U, "CDD TOC 2: the current track number (track 1 at LBA 0)");
    CDD_TX(2U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
    row(m->cdd.rx[1] == 0U && m->cdd.rx[2] == 0U && m->cdd.rx[3] == 0U && m->cdd.rx[4] == 0U && m->cdd.rx[5] == 2U && m->cdd.rx[6] == 0U &&
            m->cdd.rx[7] == 0U && m->cdd.rx[8] == 4U,
        "CDD TOC 0: the current absolute time 0:02:00 (LBA 0 + 150), the data-track flag in RS8");
    CDD_TX(2U, 0U, 0U, 1U, 0U, 0U, 0U, 0U);
    row(m->cdd.rx[1] == 1U && m->cdd.rx[5] == 0U && m->cdd.rx[8] == 4U, "CDD TOC 1: the track-relative time 0:00:00 at the start of track 1");
  }

  /* seek (command 4, MSF 00:03:25 -> LBA 100 in the audio track), pause (6), resume (7), N-track jump (A): the status sequence */
  fresh();
  m->now = 0U;
  sub_write(0x37U, 1U, 0x04U); /* HOCK: the drive ticks every 1/75 s and requests level 4 */
  CDD_TX(4U, 0U, 0U, 0U, 0U, 3U, 2U, 5U);
  row(w == 0U && m->cdd.rx[0] == SCD_CDD_SEEK && m->cdd.rx[1] == 0x0FU && m->cdd.pending == 0x04U && m->cdd.rx[9] == (uint8_t)(~(SCD_CDD_SEEK + 0x0FU) & 0x0FU),
      "CDD seek: RS0 = SEEK, RS1 = $F, a pause is pending (RS9 the checksum)");
  next_cdd_tick();
  row(m->cdd.status == 0x04U && m->cdd.pending == 0U && m->cdd.lba == 100 && m->cdd.index == 1U && m->cdd.latency == 12U &&
          (m->ga.pending & 0x10U) != 0U,
      "CDD seek: the next 75 Hz tick moves to LBA 100 (track 2), status PAUSE, latency 12 ticks, level 4 pending");
  CDD_TX(0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
  row(m->cdd.rx[0] == SCD_CDD_SEEK && m->cdd.rx[1] == 0x0FU, "CDD status: while the latency exceeds 3 ticks the report still says SEEK");
  {
    int n;
    for (n = 0; n < 9; ++n) next_cdd_tick();
  }
  row(m->cdd.latency == 3U, "CDD seek: nine ticks later the latency is 3");
  CDD_TX(0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
  row(m->cdd.rx[0] == 0x04U && m->cdd.rx[1] == 0U && m->cdd.rx[2] == 0U && m->cdd.rx[3] == 0U && m->cdd.rx[4] == 0U && m->cdd.rx[5] == 3U &&
          m->cdd.rx[6] == 2U && m->cdd.rx[7] == 5U && m->cdd.rx[8] == 0U,
      "CDD status: PAUSE with the absolute time 0:03:25 (LBA 100 + 150 = 250 blocks), audio track flag 0");
  CDD_TX(7U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
  row(m->cdd.status == SCD_CDD_PLAY && m->cdd.rx[0] == SCD_CDD_PLAY && m->cdd.rx[5] == 3U, "CDD resume: PLAY, RS1-RS8 unchanged");
  CDD_TX(6U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
  row(m->cdd.status == 0x04U && m->cdd.rx[0] == 0x04U && m->ga.cdd_ctrl_hi == 1U && m->cdd.rx[5] == 3U, "CDD pause: PAUSE, no audio playing (control high byte 1), RS1-RS8 unchanged");
  CDD_TX(7U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
  CDD_TX(10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
  row(m->cdd.status == 0x04U && m->cdd.rx[0] == 0x04U && m->ga.cdd_ctrl_hi == 1U, "CDD N-track jump: the drive pauses, RS1-RS8 unchanged");
  {
    uint8_t before[10];
    memcpy(before, m->cdd.rx, 10U);
    CDD_TX(8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
    row(w == SCD_STOP_CDD_COMMAND_UNSUPPORTED && memcmp(before, m->cdd.rx, 10U) == 0, "CDD forward scan (not on the boot path): typed stop, nothing changes");
  }

  /* ================================= CDC host-data transfer ================================= */
  {
    static const uint8_t data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint32_t dest;
#define CDC_REG(reg, value) do { sub_write(0x05U, 1U, (reg)); sub_write(0x07U, 1U, (value)); } while (0)
    for (dest = 2U; dest <= 3U; ++dest) {
      uint32_t r, mw;
      fresh();
      m->now = 77U;
      sub_write(0x33U, 1U, 0x20U); /* IEN5 */
      memcpy(&m->cdc.buffer[0x100], data, 6U);
      CDC_REG(1U, 0x42U);          /* IFCTRL: DOUTEN + DTEIEN */
      CDC_REG(2U, 5U); CDC_REG(3U, 0U); /* DBC = 5: six bytes */
      CDC_REG(4U, 0x00U); CDC_REG(5U, 0x01U); /* DAC = $0100 */
      sub_write(0x04U, 1U, dest);  /* destination: the host read of main (2) or sub (3) */
      row((sub_read(0x04U, 1U) & 0xC0U) == 0U && m->cdc.dsr == 0U, dest == 2U ? "CDC host (main): before DTRG nothing is started (EDT = 0, DSR = 0)" : "CDC host (sub): before DTRG nothing is started (EDT = 0, DSR = 0)");
      CDC_REG(6U, 0U);             /* DTRG */
      row(sub_read(0x04U, 1U) == (0x40U | dest) && m->cdc.host_word == 0xAABBU && m->cdc.dac == 0x0102U && m->cdc.dbc == 3U,
          dest == 2U ? "CDC host (main): DTRG makes the first word available at once: DSR = 1, word $AABB, DAC +2, DBC -2" : "CDC host (sub): DTRG makes the first word available at once: DSR = 1, word $AABB, DAC +2, DBC -2");
      /* a byte read of the data register has no side effect */
      row(dest == 3U ? (sub_read(0x08U, 1U) == 0xAAU && m->cdc.dac == 0x0102U) : 1, "CDC host: a byte read of $FF8008 returns the high byte and does not advance");
      mw = 0U;
      r = 0U;
      if (dest == 3U) {
        r = sub_read(0x08U, 2U);
        mw = 0U;
      } else {
        main_read_word(0x08U, &r);
      }
      row(r == 0xAABBU && m->cdc.host_word == 0xCCDDU && m->cdc.dac == 0x0104U && m->cdc.dbc == 1U,
          "CDC host: the first read returns $AABB and loads the next word $CCDD (DAC +2, DBC -2)");
      r = dest == 3U ? sub_read(0x08U, 2U) : (main_read_word(0x08U, &mw), mw);
      row(r == 0xCCDDU && m->cdc.host_word == 0xEEFFU && m->cdc.dac == 0x0106U && m->cdc.dbc == 0xFFFFU && m->cdc.edt == 1U &&
              (m->cdc.ifstat & 0x4AU) == 0x0AU && (m->ga.pending & 0x20U) != 0U,
          "CDC host: the second read returns $CCDD; the third word ends the transfer: EDT = 1, DBC reset, DTEI active (low), !DTBSY/!DTEN high, level 5 pending (IEN5 and DTEIEN)");
      row(sub_read(0x04U, 1U) == (0xC0U | dest), "CDC host: $FF8004 now reads EDT = 1 and DSR = 1");
      r = dest == 3U ? sub_read(0x08U, 2U) : (main_read_word(0x08U, &mw), mw);
      row(r == 0xEEFFU && m->cdc.dsr == 0U && sub_read(0x04U, 1U) == (0x80U | dest), "CDC host: the third read returns $EEFF and, the transfer being finished, clears DSR");
      r = dest == 3U ? sub_read(0x08U, 2U) : (main_read_word(0x08U, &mw), mw);
      row(r == 0xEEFFU && m->cdc.dac == 0x0106U, "CDC host: later reads return the last word and move nothing");
    }
    /* the wrong processor does not advance a transfer */
    fresh();
    m->now = 5U;
    memcpy(&m->cdc.buffer[0x100], data, 6U);
    CDC_REG(1U, 0x42U); CDC_REG(2U, 5U); CDC_REG(3U, 0U); CDC_REG(4U, 0x00U); CDC_REG(5U, 0x01U);
    sub_write(0x04U, 1U, 3U);
    CDC_REG(6U, 0U);
    {
      uint32_t main_value = 0U;
      main_read_word(0x08U, &main_value);
      row(main_value == 0xAABBU && m->cdc.dac == 0x0102U && m->cdc.dbc == 3U, "CDC host: a main read while the sub is the destination returns the register and does not advance");
    }
    row(sub_write(0x08U, 2U, 0U) == 0 && m->cdc.host_word == 0xCCDDU, "CDC host: a word write to $FF8008 performs the same read (GPGX), its data discarded");
    {
      const uint16_t word = m->cdc.host_word;
      row(sub_write(0x08U, 1U, 0x12U) == SCD_STOP_CDC_DESTINATION_UNSUPPORTED && m->cdc.host_word == word, "CDC host: a byte write to the data register is a typed stop and changes nothing");
    }
    /* a PCM or PRG RAM destination is still not modelled */
    fresh();
    CDC_REG(1U, 0x42U);
    sub_write(0x04U, 1U, 4U);
    row(sub_write(0x05U, 1U, 6U) == 0 && sub_write(0x07U, 1U, 0U) == SCD_STOP_CDC_DESTINATION_UNSUPPORTED && m->cdc.dsr == 0U,
        "CDC: the PCM RAM destination (4) stays a typed stop at DTRG and starts nothing");
  }

  free(m);
  printf("%d failure(s)\n", failures);
  return failures != 0;
}
