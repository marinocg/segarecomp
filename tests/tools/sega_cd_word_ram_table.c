/* SEG-062-T004: table-driven checks of the Word RAM ownership state machine, the 1M bank views, the sub timer, the interrupt
 * enable/pending rules, the SRES/SBRQ register, the peripheral reset, CDC DMA halting and the CDD play sequence, driven through the
 * runtime's own register paths (scd_ga_main_access / scd_ga_sub_access / scd_word_ram_access / scd_devices_*). Every expected value
 * comes from docs/architecture/sega-cd-machine-contract.md sections 5-8 and 10 (Genesis Plus GX where the references disagree), not
 * from the implementation. Prints one line per row; exits 1 when a row fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

static int failures;
static ScdMachine *m;
static const ScdDiscTrack tracks[2] = {{1U, 0U, 0U, 2048U, 0U, 0U, 40U}, {2U, 2U, 1U, 2352U, 0U, 190U, 75U}};
static ScdDisc disc;

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

/* The power-on profile (contract sections 5, 6, 8): sub halted in reset (SBRQ reads 1), window mapped, 2M main-owned. */
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
static int main_write(uint32_t offset, uint32_t width, uint32_t value) {
  uint32_t v = value;
  return clear_stop(scd_ga_main_access(m, offset, width, 1, &v, 0xFF0000U));
}
static int sub_write(uint32_t offset, uint32_t width, uint32_t value) {
  uint32_t v = value;
  return clear_stop(scd_ga_sub_access(m, offset, width, 1, &v, 0x6000U));
}
static int main_write_byte(uint32_t offset, uint32_t value) { return main_write(offset, 1U, value); }
static int sub_write_byte(uint32_t offset, uint32_t value) { return sub_write(offset, 1U, value); }
static uint32_t main_read_byte(uint32_t offset) {
  uint32_t v = 0U;
  clear_stop(scd_ga_main_access(m, offset, 1U, 0, &v, 0xFF0000U));
  return v;
}
static uint32_t sub_read_byte(uint32_t offset) {
  uint32_t v = 0U;
  clear_stop(scd_ga_sub_access(m, offset, 1U, 0, &v, 0x6000U));
  return v;
}
/* 0 = access completed, -1 = the sub must wait for DTACK, else the stop class it ended with (cleared for the next row). */
static int access(uint32_t cpu, uint32_t address, int is_write, uint32_t *value) {
  int result;
  m->stop.stop_class = SCD_STOP_NONE;
  m->sub_wait_request = 0U;
  result = scd_word_ram_access(m, cpu, address, 2U, is_write, value, 0U);
  if (result == 0) return 0;
  if (m->stop.stop_class == SCD_STOP_NONE && m->sub_wait_request) {
    m->sub_wait_request = 0U;
    return -1;
  }
  return clear_stop(result);
}
static int can(uint32_t cpu, uint32_t address) {
  uint32_t v = 0U;
  return access(cpu, address, 0, &v) == 0;
}
static int bus_read(uint32_t cpu, uint32_t address, uint32_t *value) { return clear_stop(scd_read(m, cpu, address, 2U, value, 0U)); }
static int bus_write(uint32_t cpu, uint32_t address, uint32_t value) { return clear_stop(scd_write(m, cpu, address, 2U, value, 0U)); }
static void run_devices_until(uint64_t limit) {
  for (;;) {
    const uint64_t t = scd_devices_next_event(m);
    if (t > limit) break;
    m->now = t;
    scd_devices_advance(m, t);
  }
}
static void next_cdd_tick(void) {
  const uint64_t t = m->cdd.next_tick;
  m->now = t;
  scd_devices_advance(m, t);
}
static void cdd_send(uint32_t command, uint32_t lba) {
  const uint32_t msf = lba + 150U, mm = msf / 75U / 60U, ss = msf / 75U % 60U, ff = msf % 75U;
  sub_write(0x42U, 2U, command << 8);
  sub_write(0x44U, 2U, ((mm / 10U) << 8) | (mm % 10U));
  sub_write(0x46U, 2U, ((ss / 10U) << 8) | (ss % 10U));
  sub_write(0x48U, 2U, ((ff / 10U) << 8) | (ff % 10U));
  sub_write(0x4AU, 2U, 0x0505U); /* TX8/TX9 as written are irrelevant: the command write clears them */
}

int main(void) {
  uint32_t v;
  uint64_t t0, period;
  const uint64_t bt = SCD_DMA_SUB_CYCLES_PER_BYTE * SCD_SUB_CYCLE_TICKS;
  m = calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  /* ---- 2M ownership ---- */
  fresh();
  row(main_read_byte(0x03) == 0x01U && sub_read_byte(0x03) == 0x01U, "2M power-on: RET=1 DMNA=0 read on both sides");
  row(can(SCD_MAIN, 0x200000U) && access(SCD_SUB, 0x080000U, 0, &v) == -1,
      "2M main-owned: main accesses; a sub access withholds DTACK (the sub waits)");
  row(main_write_byte(0x03, 0x00) == 0 && main_read_byte(0x03) == 0x01U, "2M main writes DMNA=0: no effect");
  row(sub_write_byte(0x03, 0x00) == 0 && main_read_byte(0x03) == 0x01U, "2M sub writes RET=0 while main owns: no effect");
  row(sub_write_byte(0x03, 0x01) == 0 && main_read_byte(0x03) == 0x01U, "2M sub writes RET=1 while main owns: no effect");
  m->ga.sub_wait = 1U;
  m->now = 5000U;
  row(main_write_byte(0x03, 0x02) == 0 && main_read_byte(0x03) == 0x02U && sub_read_byte(0x03) == 0x02U && m->ga.dmna_int == 1U &&
          m->ga.sub_wait == 0U && m->cpu[SCD_SUB].next == 5000U,
      "2M main writes DMNA=1: sub owns (RET=0 DMNA=1), a waiting sub resumes at the write");
  v = 0x7777U;
  row(can(SCD_SUB, 0x080000U) && access(SCD_MAIN, 0x200000U, 0, &v) == SCD_STOP_OPEN_BUS_READ,
      "2M sub-owned: sub accesses; a main read is open bus (typed stop open_bus_read)");
  {
    const uint64_t before = m->ignored_writes;
    v = 0x7777U;
    row(access(SCD_MAIN, 0x200020U, 1, &v) == 0 && m->ignored_writes == before + 1U && m->word_ram[0x20] == 0U,
        "2M sub-owned: a main write is ignored");
  }
  row(main_write_byte(0x03, 0x02) == 0 && main_read_byte(0x03) == 0x02U, "2M repeated DMNA=1: no further change");
  row(sub_write_byte(0x03, 0x00) == 0 && main_read_byte(0x03) == 0x02U, "2M sub writes RET=0 while it owns: no effect");
  v = 0xA55AU;
  row(access(SCD_SUB, 0x080010U, 1, &v) == 0, "2M sub stores a word while it owns");
  row(sub_write_byte(0x03, 0x01) == 0 && main_read_byte(0x03) == 0x01U && m->ga.dmna_int == 0U,
      "2M sub writes RET=1: main owns, RET=1 DMNA=0, the request clears");
  v = 0U;
  row(access(SCD_MAIN, 0x200010U, 0, &v) == 0 && v == 0xA55AU, "2M handoff preserves the sub's word for main");
  row(main_write_byte(0x03, 0xC2) == 0 && m->ga.prg_bank == 3U && main_read_byte(0x03) == 0xC2U,
      "conflicting order: main BK=3 with DMNA=1 after the return gives the sub ownership again");
  row(sub_write_byte(0x03, 0x01) == 0 && main_read_byte(0x03) == 0xC1U, "... and the sub's RET=1 returns it (BK kept)");
  row(access(SCD_SUB, 0x0C0000U, 0, &v) == SCD_STOP_OPEN_BUS_READ, "2M: a sub read of $0C0000 (unused in 2M) is open bus");
  row(sub_write_byte(0x03, 0x19) == 0 && sub_read_byte(0x03) == 0x19U && main_read_byte(0x03) == 0xC1U,
      "PM1/PM0 are stored and read by the sub only (no RET/MODE change)");
  row(sub_write_byte(0x02, 0x01) == 0 && sub_read_byte(0x03) == 0x01U, "the $FF8002 byte reaches the memory-mode register (PM cleared)");
  row(sub_write(0x02U, 2U, 0x0004U) == 0 && sub_read_byte(0x03) == 0x01U, "a word write to $FF8002 has no effect");

  /* ---- 1M banks ---- */
  fresh();
  for (v = 0; v < 8U; ++v) m->word_ram[v] = (uint8_t)(0x10U + v); /* 2M words 0..3 = 1011 1213 1415 1617 */
  row(sub_write_byte(0x03, 0x04) == 0 && sub_read_byte(0x03) == 0x04U && main_read_byte(0x03) == 0x04U,
      "sub writes MODE=1 RET=0: 1M mode (MODE reads 1 on both sides)");
  row(can(SCD_MAIN, 0x200000U) && can(SCD_SUB, 0x0C0000U), "1M: both processors access their banks at once");
  v = 0U;
  access(SCD_MAIN, 0x200000U, 0, &v);
  {
    uint32_t s = 0U, v2 = 0U;
    access(SCD_SUB, 0x0C0000U, 0, &s);
    access(SCD_MAIN, 0x200002U, 0, &v2);
    row(v == 0x1011U && s == 0x1213U && v2 == 0x1415U, "1M banks are the word interleave of the 2M space (RET=0: main bank 0)");
  }
  row(access(SCD_SUB, 0x080000U, 0, &v) == SCD_STOP_WORDRAM_DOT_IMAGE_UNSUPPORTED, "1M: the sub dot-image view is a typed stop");
  row(access(SCD_MAIN, 0x220000U, 0, &v) == SCD_STOP_WORDRAM_CELL_IMAGE_UNSUPPORTED, "1M: the main cell-image view is a typed stop");
  v = 0xBEEFU;
  access(SCD_MAIN, 0x200100U, 1, &v);
  v = 0x1234U;
  access(SCD_SUB, 0x0C0100U, 1, &v);
  row(sub_write_byte(0x03, 0x05) == 0 && sub_read_byte(0x03) == 0x05U, "sub writes RET=1 in 1M: swap");
  {
    uint32_t a = 0U, b = 0U;
    access(SCD_MAIN, 0x200100U, 0, &a);
    access(SCD_SUB, 0x0C0100U, 0, &b);
    row(a == 0x1234U && b == 0xBEEFU, "1M swap exchanges the banks between the processors");
  }
  row(main_write_byte(0x03, 0x00) == 0 && main_read_byte(0x03) == 0x07U && sub_read_byte(0x03) == 0x07U,
      "main writes DMNA=0 in 1M: the DMNA bit sets (swap requested)");
  row(sub_write_byte(0x03, 0x05) == 0 && sub_read_byte(0x03) == 0x07U, "the sub rewriting the same RET leaves DMNA set");
  row(sub_write_byte(0x03, 0x04) == 0 && sub_read_byte(0x03) == 0x04U, "the sub's next RET change swaps and clears DMNA");
  row(main_write_byte(0x03, 0x02) == 0 && main_read_byte(0x03) == 0x04U && m->ga.dmna_int == 1U,
      "main writes DMNA=1 in 1M: only the request latches");
  row(sub_write_byte(0x03, 0x00) == 0 && main_read_byte(0x03) == 0x02U && can(SCD_SUB, 0x080000U),
      "1M -> 2M with RET=0 and a latched request: the sub keeps 2M Word RAM (RET=0 DMNA=1)");
  row(sub_write_byte(0x03, 0x01) == 0 && main_read_byte(0x03) == 0x01U, "... RET=1 returns it to main");
  row(sub_write_byte(0x03, 0x04) == 0 && sub_write_byte(0x03, 0x00) == 0 && main_read_byte(0x03) == 0x01U && can(SCD_MAIN, 0x200000U),
      "1M -> 2M with RET=0 and no request: RET is forced to 1, main owns");
  {
    uint32_t a = 0U, b = 0U;
    access(SCD_MAIN, 0x200000U + 4U * 0x80U, 0, &a);      /* bank 0 offset $100 = 2M offset 4 * $80 */
    access(SCD_MAIN, 0x200000U + 4U * 0x80U + 2U, 0, &b); /* bank 1 offset $100 = 2M offset 4 * $80 + 2 */
    row(a == 0xBEEFU && b == 0x1234U, "1M -> 2M keeps every byte in place (the interleave is the storage)");
  }

  /* ---- CDC DMA into 2M Word RAM: halted while main owns it ---- */
  fresh();
  for (v = 0; v < 32U; ++v) m->cdc.buffer[v] = (uint8_t)(0xA0U + v);
  m->now = 1000U;
  sub_write_byte(0x04, 0x07);              /* destination 7 (resets $FF800A) */
  sub_write_byte(0x05, 0x01);              /* IFCTRL: DOUTEN */
  sub_write_byte(0x07, 0x02);
  sub_write_byte(0x07, 0x0F);              /* DBCL: 16 bytes */
  sub_write_byte(0x07, 0x00);              /* DBCH */
  sub_write_byte(0x07, 0x00);              /* DACL */
  sub_write_byte(0x07, 0x00);              /* DACH */
  sub_write_byte(0x07, 0x00);              /* DTRG */
  row(m->cdc.dma_state == SCD_DMA_HALTED && scd_devices_next_event(m) == UINT64_MAX,
      "DTRG into 2M Word RAM owned by main: the transfer is started but halted (no stop, no event)");
  m->now = 2000U;
  main_write_byte(0x03, 0x02);
  row(m->cdc.dma_state == SCD_DMA_RUNNING && m->cdc.dma_base == 2000U, "main DMNA=1 resumes it from the handover");
  m->now = 2000U + 8U * bt;
  scd_cd_observe(m);
  row(m->cdc.dma_done == 8U && m->word_ram[0] == 0xA0U && m->word_ram[7] == 0xA7U && m->word_ram[8] == 0U,
      "8 bytes after the handover: exactly one 8-byte group moved");
  sub_write_byte(0x03, 0x01);
  row(m->cdc.dma_state == SCD_DMA_HALTED && m->cdc.dma_done == 8U && main_read_byte(0x03) == 0x01U,
      "the sub's RET=1 halts the transfer with its progress kept");
  m->now = 2000U + 100U * bt;
  main_write_byte(0x03, 0x02);
  run_devices_until(m->now + 64U * bt);
  row(m->cdc.dma_state == SCD_DMA_NONE && m->cdc.edt == 1U && m->cdc.dma_done == 16U && m->word_ram[15] == 0xAFU &&
          (m->cdc.ifstat & 0x0AU) == 0x0AU,
      "resumed again, it completes: 16 bytes, EDT set, DTBSY/DTEN released");

  /* ---- SRES / SBRQ and the PRG-RAM window ---- */
  fresh();
  row(main_read_byte(0x01) == 0x02U && bus_write(SCD_MAIN, 0x020000U, 0x1234U) == 0 && m->prg_ram[0] == 0x12U,
      "power-on: SRES=0 SBRQ=1, the window is mapped");
  m->prg_ram[0] = 0U;
  m->prg_ram[1] = 0U;
  row(main_write_byte(0x01, 0x01) == 0 && main_read_byte(0x01) == 0x01U && m->ga.sub_halted == 0U && m->ga.prg_window == 0U,
      "SRES=1 SBRQ=0: the sub runs and the window unmaps");
  row(bus_read(SCD_MAIN, 0x020000U, &v) == SCD_STOP_OPEN_BUS_READ && bus_write(SCD_MAIN, 0x020000U, 0x5555U) == 0 &&
          m->prg_ram[0] == 0U,
      "unmapped window: reads are open bus, writes are ignored");
  m->cpu[SCD_SUB].irq.stopped = 1U;
  row(main_write_byte(0x01, 0x03) == 0 && main_read_byte(0x01) == 0x01U && m->ga.sub_halted == 1U && m->ga.prg_window == 0U,
      "SBRQ=1 while the sub is in STOP: halted, SBRQ reads 0, the window stays unmapped");
  row(main_write_byte(0x01, 0x01) == 0 && m->ga.sub_halted == 0U && main_read_byte(0x01) == 0x01U, "SBRQ=0 releases it");
  m->cpu[SCD_SUB].irq.stopped = 0U;
  row(main_write_byte(0x01, 0x03) == 0 && main_read_byte(0x01) == 0x03U && m->ga.prg_window == 1U &&
          bus_read(SCD_MAIN, 0x060000U, &v) == 0,
      "SBRQ=1 while the sub runs: SBRQ reads 1, the window (and its $060000 mirror) maps");
  row(main_write_byte(0x01, 0x00) == 0 && main_read_byte(0x01) == 0x02U, "SRES=0 reads back $02");

  /* ---- timer ---- */
  fresh();
  m->now = 1000U;
  period = (uint64_t)(9U + 1U) * SCD_TIMER_UNIT_SUB_CYCLES * SCD_SUB_CYCLE_TICKS;
  row(sub_write_byte(0x31, 9U) == 0 && scd_devices_next_event(m) == 1000U + period && sub_read_byte(0x31) == 9U &&
          sub_read_byte(0x30) == 0U,
      "timer N=9: first expiry after (N + 1) * 384 sub cycles; $FF8031 reads N, $FF8030 reads 0");
  t0 = scd_devices_next_event(m);
  m->now = t0;
  scd_devices_advance(m, t0);
  row((m->ga.pending & 0x08U) == 0U && scd_devices_next_event(m) == t0 + period, "expiry with IEN3 = 0 raises nothing and reloads");
  sub_write_byte(0x33, 0x08U);
  m->now = t0 + period;
  scd_devices_advance(m, m->now);
  row((m->ga.pending & 0x08U) != 0U && scd_devices_can_raise(m, 3U), "expiry with IEN3 = 1 raises level 3");
  row(sub_write_byte(0x31, 0U) == 0 && scd_devices_next_event(m) == UINT64_MAX && !scd_devices_can_raise(m, 3U),
      "writing 0 stops the timer");
  m->now = 50000U;
  row(sub_write_byte(0x30, 4U) == 0 && sub_read_byte(0x31) == 4U && scd_devices_next_event(m) == 50000U + period / 2U,
      "a byte write to $FF8030 also programs the timer (!LDS/!UDS ignored)");

  /* ---- interrupt enables, unused registers, peripheral reset ---- */
  fresh();
  row(main_write_byte(0x00, 0x01) == 0 && m->ga.ifl2 == 0U && (m->ga.pending & 0x04U) == 0U, "IFL2 with IEN2 = 0 is dropped");
  sub_write_byte(0x33, 0x04U);
  row(main_read_byte(0x00) == 0x80U, "IEN2 mirrors to main $A12000 bit 7");
  row(main_write_byte(0x00, 0x01) == 0 && m->ga.ifl2 == 1U && (m->ga.pending & 0x04U) != 0U && main_read_byte(0x00) == 0x81U,
      "IFL2 with IEN2 = 1 sets the flag and the level-2 request");
  m->ga.pending |= 0x02U;
  sub_write_byte(0x33, 0x06U);
  row((m->ga.pending & 0x02U) != 0U, "level 1 stays pending while IEN1 is set");
  sub_write_byte(0x33, 0x04U);
  row((m->ga.pending & 0x02U) == 0U && (m->ga.pending & 0x04U) != 0U, "clearing IEN1 drops a pending level 1 only");
  row(sub_write(0x10U, 2U, 0x1111U) == 0 && m->ga.comcmd[0] == 0U && m->ignored_writes == 1U, "a sub write to COMCMD is ignored");
  row(main_write(0x20U, 2U, 0x2222U) == 0 && m->ga.comstat[0] == 0U && m->ignored_writes == 2U, "a main write to COMSTAT is ignored");
  row(main_write(0x30U, 2U, 1U) == 0 && m->ignored_writes == 3U, "a main write to $A12030 is ignored");
  {
    uint32_t r = 0U;
    row(clear_stop(scd_ga_main_access(m, 0x30U, 2U, 0, &r, 0U)) == SCD_STOP_OPEN_BUS_READ, "a main read of $A12030 is open bus");
    row(clear_stop(scd_ga_sub_access(m, 0x4CU, 2U, 1, &r, 0U)) == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED,
        "font registers ($FF804C): typed stop gate_array_register_unsupported");
  }
  row(sub_write_byte(0x20, 0x77) == 0 && sub_write_byte(0x31, 5U) == 0 && sub_write_byte(0x37, 0x04U) == 0 && m->ga.timer_next != 0U,
      "set up COMSTAT, the timer and HOCK");
  m->now = 777777U;
  row(sub_write_byte(0x01, 0x00) == 0 && m->ga.timer_next == 0U && m->ga.interrupt_enable == 0U && m->ga.pending == 0U &&
          m->ga.cdd_ctrl_lo == 0U && m->ga.cdd_ctrl_hi == 1U && m->cdc.dma_address == 0xFFFFU && m->cdd.tx[0] == 0xFFU &&
          m->cdd.rx[9] == 0x0FU && m->cdd.status == SCD_CDD_TOC_STATUS && m->cdd.anchor == 777777U,
      "RES0 = 0: peripheral reset (timer, enables, pending, CDD/CDC registers to defaults, 75 Hz clock restarted)");
  row(m->ga.comstat[0] == 0x7700U && m->ga.ien2_mirror == 1U && m->ga.ifl2 == 1U && main_read_byte(0x00) == 0x81U,
      "... communication registers and the main-side IFL2/IEN2 view are kept");

  /* ---- CDD play sequence (GPGX default latency) ---- */
  fresh();
  m->now = 100U;
  cdd_send(3U, 24U);
  row(m->cdd.pending == SCD_CDD_PLAY && m->cdd.rx[0] == SCD_CDD_SEEK && m->cdd.rx[1] == 0x0FU && m->cdd.tx[8] == 0U && m->cdd.tx[9] == 0U,
      "play: RS0 = seek, RS1 = $F; TX8/TX9 cleared (the checksum is not inspected)");
  next_cdd_tick();
  row(m->cdd.status == SCD_CDD_PLAY && m->cdd.latency == 12U && m->cdd.lba == 24 && m->cdd.pending == 0U,
      "first tick: the play starts with 12 ticks of latency (seek distance 24 adds none)");
  cdd_send(0U, 0U);
  row(m->cdd.rx[0] == SCD_CDD_SEEK, "status request while latency > 3: RS0 still reports seek");
  for (v = 0; v < 12U; ++v) next_cdd_tick();
  row(m->cdd.latency == 0U && m->cdd.delivered == 0U, "12 more ticks: latency spent, nothing read yet");
  next_cdd_tick();
  row(m->cdd.delivered == 1U && m->cdd.lba == 25, "next tick: block 24 read");
  cdd_send(0U, 0U);
  row(m->cdd.rx[0] == SCD_CDD_PLAY && m->cdd.rx[1] == 0U && m->cdd.rx[3] == 0U && m->cdd.rx[5] == 2U && m->cdd.rx[6] == 2U &&
          m->cdd.rx[7] == 5U && m->cdd.rx[8] == 0x04U,
      "status: playing, absolute time 00:02:25, data track");
  {
    uint32_t i, sum = 0U;
    for (i = 0; i < 9U; ++i) sum += m->cdd.rx[i];
    row(m->cdd.rx[9] == ((~sum) & 0x0FU), "RS9 is the status checksum");
  }
  cdd_send(1U, 0U);
  row(m->cdd.status == SCD_CDD_TOC_STATUS && m->cdd.rx[0] == 0U && m->cdd.rx[1] == 0x0FU && m->cdd.lba == 0,
      "stop: drive back to the TOC state, RS0 = 0, RS1 = $F");
  cdd_send(3U, 300U);
  for (v = 0; v < 14U; ++v) next_cdd_tick();
  row(m->cdd.status == SCD_CDD_END && m->cdd.delivered == 1U, "play past the last track: status END, nothing read");

  free(m);
  printf(failures ? "%d failing row(s)\n" : "all rows passed\n", failures);
  return failures ? 1 : 0;
}
