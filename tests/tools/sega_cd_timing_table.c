/* SEG-071-T002 (ADR 0177): the timing conventions of the BIOS-image profile, driven through the runtime's own interfaces with values
 * derived by hand (the arithmetic is shown in each label). Nothing here reads a game byte: the BIOS image is a zero-filled array with a
 * few project-authored opcode words.
 *   1. the sub cycle (line-locked in the BIOS-image profile, exact 12.5 MHz in the synthetic profile);
 *   2. the 75 Hz drive grid: tick k at the end of the first line n with acc0 + 9,552 n >= 2,000,000 k (BIOS-image), the exact grid otherwise;
 *   3. the main bus refresh delay and the long-instruction rule;
 *   4. the Z80-window access latency (one cycle per bus access);
 *   5. the VDP data-port FIFO (stall and the status bits) and the VRAM-fill busy bit;
 *   6. the one-instruction interrupt latency after IE0 is set while a vertical interrupt is pending;
 *   7. the video phase: first onset, the vertical interrupt a fixed delay after each onset;
 *   8. the autovector interrupt exception cycles (the E-clock alignment table).
 * usage: sega_cd_timing_table */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"
#include "runtime.h"

static int failures;
static ScdMachine *m;
static uint8_t bios[0x20000];

#define MASTER UINT64_C(500000)
#define LINE UINT64_C(3420)
#define LINE_TICKS (LINE * MASTER)
#define FRAME (LINE * 262U * MASTER)

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

static int vdp(uint32_t width, uint32_t value) {
  const int s = scd_write(m, SCD_MAIN, 0xC00004U, width, value, 0xFF0100U);
  if (s != 0) {
    printf("      stop %s\n", scd_stop_class_name(m->stop.stop_class));
    m->stop.stop_class = SCD_STOP_NONE;
  }
  return s;
}

static int vdp_data(uint32_t width, uint32_t value) {
  const int s = scd_write(m, SCD_MAIN, 0xC00000U, width, value, 0xFF0100U);
  if (s != 0) {
    printf("      stop %s\n", scd_stop_class_name(m->stop.stop_class));
    m->stop.stop_class = SCD_STOP_NONE;
  }
  return s;
}

static uint32_t status_read(void) {
  uint32_t v = 0U;
  if (scd_read(m, SCD_MAIN, 0xC00004U, 2U, &v, 0xFF0100U) != 0) {
    printf("      stop %s\n", scd_stop_class_name(m->stop.stop_class));
    m->stop.stop_class = SCD_STOP_NONE;
  }
  return v;
}

/* A BIOS-image machine (the profile is selected by `bios`); `onset` is the last onset (the frame origin is onset - FRAME). */
static void fresh(int bios_profile, uint32_t reg1, uint32_t reg12, uint64_t onset) { /* onset UINT64_MAX: keep the power-on first onset */
  if (m->vdp_host != NULL) scd_vdp_release(m);
  memset(m, 0, sizeof *m);
  if (bios_profile) {
    m->bios = bios;
    m->bios_size = sizeof bios;
  }
  m->cpu[SCD_MAIN].cycle_ticks = SCD_MAIN_CYCLE_TICKS;
  m->cpu[SCD_SUB].cycle_ticks = scd_sub_cycle_ticks(m);
  if (scd_vdp_init(m) != 0) exit(2);
  if (bios_profile && onset != UINT64_MAX) m->vdp_onset = onset;
  if (bios_profile) {
    vdp(2U, 0x8100U | reg1);
    vdp(2U, 0x8C00U | reg12);
    vdp(2U, 0x8F02U);
  }
}

static uint64_t line_of(uint64_t tick) { return tick / LINE_TICKS; }

int main(void) {
  char label[240];
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  /* ---- 1. the sub cycle ---- */
  fresh(1, 0x04U, 0x00U, 0U);
  row(scd_sub_cycle_ticks(m) == 2148241U && 796U * scd_sub_cycle_ticks(m) == 1709999836U,
      "BIOS-image profile: one sub cycle = 2,148,241 ticks (796 per 1,710,000,000-tick line = 1,709,999,836)");
  fresh(0, 0U, 0U, 0U);
  row(scd_sub_cycle_ticks(m) == 2147727U, "synthetic profile: one sub cycle = 2,147,727 ticks (exact 12.5 MHz)");

  /* ---- 2. the drive grid ---- */
  {
    static const uint32_t want[] = {210U, 419U, 629U, 838U, 1047U, 1257U, 1466U, 1676U}; /* ceil(k * 2,000,000 / 9,552) */
    uint64_t t = 0U;
    unsigned k;
    int ok = 1;
    fresh(1, 0x04U, 0x00U, 0U);
    scd_cdd_anchor(m, 0U);
    for (k = 0; k < 8U; ++k) {
      t = scd_cdd_grid_after(m, t);
      if (line_of(t) != want[k] || t % LINE_TICKS != 0U) ok = 0;
    }
    row(ok, "BIOS-image grid from power-on: ticks at the ends of lines 210, 419, 629, 838, 1047, 1257, 1466, 1676 (ceil(k * 209.38))");
    row(scd_cdd_grid_after(m, 210U * LINE_TICKS - 1U) == 210U * LINE_TICKS && scd_cdd_grid_after(m, 210U * LINE_TICKS) == 419U * LINE_TICKS,
        "a grid point is strictly after its argument (one tick before line 210's end: that end; at it: line 419's)");
    scd_cdd_anchor(m, 10U * LINE_TICKS + 400U * scd_sub_cycle_ticks(m)); /* line 10, 400 sub cycles in: acc0 = -4,800 */
    t = scd_cdd_grid_after(m, m->cdd.anchor);
    row(line_of(t) == 220U && line_of(scd_cdd_grid_after(m, t)) == 430U && m->cdd.grid_acc0 == -4800,
        "a peripheral reset 400 sub cycles into line 10: acc0 = -4,800, ticks at the ends of lines 10 + 210 and 10 + 420 (ceil(209.88), ceil(419.26))");
    fresh(0, 0U, 0U, 0U);
    scd_cdd_anchor(m, 123456789U);
    row(scd_cdd_grid_after(m, m->cdd.anchor) == 123456789U + SCD_CDD_TICK_TICKS &&
            scd_cdd_grid_after(m, m->cdd.anchor + 2U * SCD_CDD_TICK_TICKS) == 123456789U + 3U * SCD_CDD_TICK_TICKS,
        "synthetic profile: the exact 75 Hz grid (357,954,500,000 ticks) from the anchor");
  }

  /* ---- 3. the main bus refresh delay ---- */
  fresh(1, 0x04U, 0x00U, 0U);
  m->main_refresh_due = 5000U * MASTER;
  row(scd_main_refresh_gate(m, 4999U * MASTER) == 0 && m->main_refresh_due == 5000U * MASTER, "refresh: an instruction before the deadline runs");
  {
    const int charged = scd_main_refresh_gate(m, 5000U * MASTER);
    row(charged == 1 && m->cpu[SCD_MAIN].next == 5014U * MASTER && m->main_refresh_due == 5896U * MASTER,
        "refresh: at the deadline the instruction restarts 14 master clocks (2 cycles) later and the next deadline is 896 (128 cycles) after");
  }
  row(scd_main_refresh_gate(m, 5014U * MASTER) == 0, "refresh: the restarted instruction runs");
  {
    /* project-authored opcode words: LSR.W D7,D1 ($EE69), ASL.W #1,D0 ($E340), MOVEM.L (A7)+,D0-D7/A0-A6 ($4CDF $7FFF), MOVEM.W ($4C9F $7FFF), NOP */
    bios[0x200] = 0xEEU; bios[0x201] = 0x69U;
    bios[0x210] = 0xE3U; bios[0x211] = 0x40U;
    bios[0x220] = 0x4CU; bios[0x221] = 0xDFU; bios[0x222] = 0x7FU; bios[0x223] = 0xFFU;
    bios[0x230] = 0x4CU; bios[0x231] = 0x9FU; bios[0x232] = 0x7FU; bios[0x233] = 0xFFU;
    bios[0x240] = 0x4EU; bios[0x241] = 0x71U;
    m->cpu[SCD_MAIN].d[7] = 10U;
    m->cpu[SCD_MAIN].pc = 0x200U;
    row(scd_main_refresh_skip_cycles(m) == 20U, "long instruction: LSR.W D7,D1 with D7 = 10 follows 2 x 10 = 20 cycles");
    m->cpu[SCD_MAIN].d[7] = 64U + 3U;
    row(scd_main_refresh_skip_cycles(m) == 6U, "long instruction: the count is D7 modulo 64 (67 -> 3 -> 6 cycles)");
    m->cpu[SCD_MAIN].d[7] = 64U;
    row(scd_main_refresh_skip_cycles(m) == 0U, "long instruction: a zero count follows nothing");
    m->cpu[SCD_MAIN].pc = 0x210U;
    row(scd_main_refresh_skip_cycles(m) == 0U, "long instruction: an immediate-count shift is not of the class");
    m->cpu[SCD_MAIN].pc = 0x220U;
    row(scd_main_refresh_skip_cycles(m) == 120U, "long instruction: MOVEM.L of 15 registers follows 15 x 8 = 120 cycles");
    m->cpu[SCD_MAIN].pc = 0x230U;
    row(scd_main_refresh_skip_cycles(m) == 0U, "long instruction: MOVEM.W is not of the class");
    bios[0x250] = 0x48U; bios[0x251] = 0xC0U; bios[0x252] = 0xFFU; bios[0x253] = 0xFFU; /* EXT.L D0 followed by a word that looks like a mask */
    m->cpu[SCD_MAIN].pc = 0x250U;
    row(scd_main_refresh_skip_cycles(m) == 0U, "long instruction: EXT.L D0 (opcode $48C0) is not MOVEM.L");
    m->cpu[SCD_MAIN].pc = 0x240U;
    row(scd_main_refresh_skip_cycles(m) == 0U, "long instruction: NOP is not of the class");
    m->main_refresh_due = 1000U * MASTER;
    m->cpu[SCD_MAIN].pc = 0x220U; /* 120 cycles = 840 master clocks */
    scd_main_refresh_note_instruction(m, 1000U * MASTER - 840U * MASTER);
    row(m->main_refresh_due == 1896U * MASTER, "refresh skipped: a MOVEM.L starting 120 cycles before the deadline reaches it -> the deadline moves on by 896");
    scd_main_refresh_note_instruction(m, 1896U * MASTER - 841U * MASTER);
    row(m->main_refresh_due == 1896U * MASTER, "refresh kept: one master clock short of the deadline it is not skipped");
  }

  /* ---- 4. the Z80-window latency ---- */
  fresh(1, 0x04U, 0x00U, 0U);
  scd_write(m, SCD_MAIN, 0xA11100U, 2U, 0x0100U, 0xFF0100U); /* request the Z80 bus */
  m->main_extra_ticks = 0U;
  scd_write(m, SCD_MAIN, 0xA00010U, 1U, 0x12U, 0xFF0100U);
  row(m->main_extra_ticks == 7U * MASTER, "Z80 window: a byte write takes one more cycle (7 master clocks)");
  m->stop.stop_class = SCD_STOP_NONE;
  m->main_extra_ticks = 0U;
  scd_write(m, SCD_MAIN, 0xA00010U, 2U, 0x1234U, 0xFF0100U); /* the host refuses a word to the Z80 RAM; the cycle is taken first */
  row(m->main_extra_ticks == 7U * MASTER, "Z80 window: a word access (one bus cycle) takes one more cycle too (7 master clocks)");
  m->stop.stop_class = SCD_STOP_NONE;
  m->main_extra_ticks = 0U;
  {
    uint32_t v = 0U;
    scd_read(m, SCD_MAIN, 0xA10003U, 1U, &v, 0xFF0100U);
    row(m->main_extra_ticks == 0U, "an I/O port access outside the Z80 window takes no extra cycle");
  }
  m->stop.stop_class = SCD_STOP_NONE;

  /* ---- 5. the VDP FIFO ---- */
  {
    const uint64_t onset = 5U * FRAME;
    uint64_t origin;
    uint32_t i, st;
    fresh(1, 0x54U, 0x00U, onset); /* Mode 5, DMA enable, display on; H32 */
    origin = m->vdp_onset - FRAME;
    vdp(4U, 0xC0000000U); /* CRAM write at 0 (two slots would be a VRAM write) */
    m->now = origin + 100U * LINE_TICKS;
    for (i = 0; i < 4U; ++i) {
      m->main_extra_ticks = 0U;
      vdp_data(2U, 0x0100U + i);
      snprintf(label, sizeof label, "FIFO write %u at the start of active line 100 (H32, CRAM): no stall (got %llu)", i + 1U,
               (unsigned long long)(m->main_extra_ticks / MASTER));
      row(m->main_extra_ticks == 0U, label);
    }
    m->main_extra_ticks = 0U;
    vdp_data(2U, 0x0104U);
    snprintf(label, sizeof label,
             "FIFO write 5: all four entries in use (read-out slots 230, 510, 810, 970): wait for the oldest, line 100 + 230 = 342,230 = 7 x 48,890 -> 230 master clocks (got %llu)",
             (unsigned long long)(m->main_extra_ticks / MASTER));
    row(m->main_extra_ticks == 230U * MASTER, label);
    m->now = origin + 100U * LINE_TICKS + 0U;
    st = status_read();
    snprintf(label, sizeof label, "status read right after: FIFO full (bit 8) and not empty (bit 9) - got $%03X", st & 0x300U);
    row((st & 0x300U) == 0x100U, label);
    m->main_extra_ticks = 0U;
    m->now = origin + 100U * LINE_TICKS + 2000U * MASTER;
    st = status_read();
    snprintf(label, sizeof label, "status read after the last read-out slot (1130): FIFO empty (bit 9), not full - got $%03X", st & 0x300U);
    row((st & 0x300U) == 0x200U, label);
    /* the same write sequence in the vertical blank is unrestricted */
    fresh(1, 0x54U, 0x00U, onset);
    origin = m->vdp_onset - FRAME;
    vdp(4U, 0xC0000000U);
    m->now = origin + 10U * LINE_TICKS;
    m->main_extra_ticks = 0U;
    for (i = 0; i < 6U; ++i) vdp_data(2U, 0x0100U + i);
    row(m->main_extra_ticks == 0U, "FIFO: six writes in the vertical blank (line 10) do not stall");
    /* the FIFO is empty at the start of each frame (the onset event) */
    fresh(1, 0x54U, 0x00U, onset);
    origin = m->vdp_onset - FRAME;
    vdp(4U, 0xC0000000U);
    m->now = origin + 100U * LINE_TICKS;
    for (i = 0; i < 4U; ++i) vdp_data(2U, 0x0100U + i);
    m->now = m->vdp_onset;
    scd_devices_advance(m, m->vdp_onset);
    m->now = m->vdp_onset - FRAME + 100U * LINE_TICKS;
    m->main_extra_ticks = 0U;
    for (i = 0; i < 4U; ++i) vdp_data(2U, 0x0100U + i);
    row(m->main_extra_ticks == 0U, "FIFO: emptied at the next frame's start (four more writes at line 100 do not stall)");
  }

  /* ---- 5b. the VRAM fill's busy bit ---- */
  {
    uint64_t origin, start, end;
    uint32_t st;
    fresh(1, 0x14U, 0x00U, 5U * FRAME); /* Mode 5, DMA enable, display off: blank slot rates; H32: 166 per line */
    origin = m->vdp_onset - FRAME;
    vdp(2U, 0x8F01U); /* auto-increment 1 */
    vdp(2U, 0x934CU); /* length $014C = 332 bytes */
    vdp(2U, 0x9401U);
    vdp(2U, 0x9500U);
    vdp(2U, 0x9600U);
    vdp(2U, 0x9780U); /* fill */
    vdp(4U, 0x40000080U); /* VRAM write, DMA (CD5) */
    start = origin + 5U * LINE_TICKS; /* frame line 5: inside the 37-line blank window */
    m->now = start;
    m->main_extra_ticks = 0U;
    vdp_data(2U, 0x00FFU);
    /* start: 5 * 3420 = 17,100; initial data-port write: + 2 * (3420 / 166) = 2 * 20 = 40; 332 bytes at 166 per line: 332 * 3420 / 166 = 6,840 */
    end = start + (40U + 6840U) * MASTER;
    m->now = end - 57U * MASTER; /* the read is taken 8 cycles (56 master clocks) later: 1 master clock before the end */
    st = status_read();
    snprintf(label, sizeof label, "VRAM fill of 332 bytes in the blank (166 per line): busy 1 master clock before its end (status $%04X)", st & 0xFFFFU);
    row((st & 0x02U) != 0U, label);
    m->now = end - 56U * MASTER;
    st = status_read();
    row((st & 0x02U) == 0U, "VRAM fill: not busy when the read reaches the end (17,100 + 40 + 6,840 master clocks)");
  }

  /* ---- 6. the interrupt latency ---- */
  fresh(1, 0x04U, 0x00U, 5U * FRAME);
  m->vint_pending = 1U;
  m->main_irq_delay = 0U;
  vdp(2U, 0x8164U); /* IE0 set while a vertical interrupt is pending */
  row(m->main_irq_delay == 1U, "IE0 set from a word write while a vertical interrupt is pending: one instruction executes before it is recognised");
  m->main_irq_delay = 0U;
  vdp(2U, 0x8164U);
  row(m->main_irq_delay == 0U, "IE0 already set: no latency");
  fresh(1, 0x04U, 0x00U, 5U * FRAME);
  m->vint_pending = 1U;
  vdp(4U, 0x81648F02U); /* a MOVE.L: the interrupt line is set at once */
  row(m->main_irq_delay == 0U, "IE0 set by a long write: no latency");
  fresh(1, 0x04U, 0x00U, 5U * FRAME);
  m->vint_pending = 0U;
  vdp(2U, 0x8164U);
  row(m->main_irq_delay == 0U, "IE0 set with no vertical interrupt pending: no latency");

  /* ---- 7. the video phase ---- */
  fresh(1, 0x04U, 0x00U, UINT64_MAX);
  {
    uint64_t t;
    row(m->vdp_onset == 111858U * MASTER && 111858U + 784182U == 896040U,
        "the first onset is 111,858 master clocks after power-on (896,040 - 784,182: the main processor leaves reset 229 lines + 1,004 clocks into the frame, rounded down to a multiple of 7)");
    t = scd_devices_next_event(m);
    m->now = t;
    scd_devices_advance(m, t);
    row(m->vdp_frames == 1U && m->vint_pending == 0U && m->vdp_onset == 111858U * MASTER + FRAME, "at the onset the vertical interrupt is not yet requested");
    t = scd_devices_next_event(m);
    row(t == 111858U * MASTER + 770U * MASTER, "H32: the vertical interrupt is requested 770 master clocks after the onset");
    m->now = t;
    scd_devices_advance(m, t);
    row(m->vint_pending == 1U && (((GenesisRuntime *)m->vdp_host)->devices.vdp.status_register & 0x80U) != 0U, "...and the F flag is set with it");
    fresh(1, 0x04U, 0x01U, UINT64_MAX); /* H40 */
    t = scd_devices_next_event(m);
    m->now = t;
    scd_devices_advance(m, t);
    row(scd_devices_next_event(m) == 111858U * MASTER + 788U * MASTER, "H40: 788 master clocks after the onset");
  }

  /* ---- 8. the interrupt exception cycles ---- */
  {
    static const uint32_t table[10] = {50U, 59U, 58U, 57U, 56U, 55U, 54U, 53U, 52U, 51U};
    uint32_t c;
    int ok = 1;
    fresh(1, 0x04U, 0x00U, 3U * FRAME);
    for (c = 0; c < 23U; ++c) {
      m->cpu[SCD_MAIN].next = (m->vdp_onset - FRAME) + (uint64_t)c * SCD_MAIN_CYCLE_TICKS;
      m->cpu[SCD_SUB].next = (m->vdp_onset - FRAME) + (uint64_t)c * scd_sub_cycle_ticks(m);
      if (scd_interrupt_entry_cycles(m, SCD_MAIN, 6U) != table[c % 10U] || scd_interrupt_entry_cycles(m, SCD_SUB, 4U) != table[c % 10U]) ok = 0;
    }
    row(ok, "interrupt exception cycles: 50, 59, 58, 57, 56, 55, 54, 53, 52, 51 by the processor's cycle count within the frame, mod 10 (both processors)");
    m->cpu[SCD_MAIN].next = (m->vdp_onset - FRAME) + 12345U * SCD_MAIN_CYCLE_TICKS;
    row(scd_interrupt_entry_cycles(m, SCD_MAIN, 6U) == 55U, "...the main processor at cycle 12,345 of the frame: index 5 -> 55 cycles");
    fresh(0, 0U, 0U, 0U);
    row(scd_interrupt_entry_cycles(m, SCD_MAIN, 6U) == 44U && scd_interrupt_entry_cycles(m, SCD_SUB, 4U) == 44U,
        "synthetic profile: the flat 44 cycles");
  }

  scd_vdp_release(m);
  free(m);
  printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
