/* SEG-062-T004: table-driven checks of the Word RAM ownership state machine, the 1M bank views, the sub timer and the interrupt
 * enable/pending rules, driven through the runtime's own register paths (scd_ga_main_access / scd_ga_sub_access /
 * scd_word_ram_access / scd_devices_*). Every expected value comes from docs/architecture/sega-cd-machine-contract.md sections
 * 5, 6 and 10 (GPGX/MAME-derived, see the classifications there), not from the implementation.
 * Prints one line per row; exits 1 on the first failing row's summary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

static int failures;
static ScdMachine *m;

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

static void fresh(void) {
  memset(m, 0, sizeof *m);
  m->ga.ret = 1U; /* power-on: 2M, main owns (contract section 6) */
  m->ga.sub_reset = 1U;
  m->ga.sub_busreq = 1U;
}

static int main_write_byte(uint32_t offset, uint32_t value) {
  uint32_t v = value;
  return scd_ga_main_access(m, offset, 1U, 1, &v, 0xFF0000U);
}
static int sub_write_byte(uint32_t offset, uint32_t value) {
  uint32_t v = value;
  return scd_ga_sub_access(m, offset, 1U, 1, &v, 0x6000U);
}
static uint32_t main_read_byte(uint32_t offset) {
  uint32_t v = 0U;
  scd_ga_main_access(m, offset, 1U, 0, &v, 0xFF0000U);
  return v;
}
static uint32_t sub_read_byte(uint32_t offset) {
  uint32_t v = 0U;
  scd_ga_sub_access(m, offset, 1U, 0, &v, 0x6000U);
  return v;
}
/* 0 = access completed, else the stop class it ended with (the stop is cleared for the next row). */
static int access(uint32_t cpu, uint32_t address, int is_write, uint32_t *value) {
  int result;
  m->stop.stop_class = SCD_STOP_NONE;
  result = scd_word_ram_access(m, cpu, address, 2U, is_write, value, 0U);
  if (result == 0) return 0;
  result = (int)m->stop.stop_class;
  m->stop.stop_class = SCD_STOP_NONE;
  return result;
}
static int can(uint32_t cpu, uint32_t address) {
  uint32_t v = 0U;
  return access(cpu, address, 0, &v) == 0;
}
static int stop_of(int status) {
  const int s = status == 0 ? 0 : (int)m->stop.stop_class;
  m->stop.stop_class = SCD_STOP_NONE;
  return s;
}

int main(void) {
  uint32_t v;
  uint64_t t0, period;
  m = calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  /* ---- 2M ownership ---- */
  fresh();
  row(main_read_byte(0x03) == 0x01U && sub_read_byte(0x03) == 0x01U, "2M power-on: RET=1 DMNA=0 read on both sides");
  row(can(SCD_MAIN, 0x200000U) && access(SCD_SUB, 0x080000U, 0, &v) == SCD_STOP_WORDRAM_NOT_OWNED,
      "2M main-owned: main accesses, sub is refused (wordram_not_owned: the real sub would hang)");
  row(stop_of(main_write_byte(0x03, 0x00)) == 0 && main_read_byte(0x03) == 0x01U, "2M main writes DMNA=0: no effect");
  row(stop_of(sub_write_byte(0x03, 0x00)) == 0 && main_read_byte(0x03) == 0x01U, "2M sub writes RET=0 while main owns: no effect");
  row(stop_of(sub_write_byte(0x03, 0x01)) == 0 && main_read_byte(0x03) == 0x01U, "2M sub writes RET=1 while main owns: no effect");
  row(stop_of(main_write_byte(0x03, 0x02)) == 0 && main_read_byte(0x03) == 0x02U && sub_read_byte(0x03) == 0x02U,
      "2M main writes DMNA=1: sub owns, RET=0 DMNA=1");
  row(can(SCD_SUB, 0x080000U) && access(SCD_MAIN, 0x200000U, 0, &v) == SCD_STOP_WORDRAM_NOT_OWNED,
      "2M sub-owned: sub accesses, main is refused (wordram_not_owned)");
  row(stop_of(main_write_byte(0x03, 0x02)) == 0 && main_read_byte(0x03) == 0x02U, "2M repeated DMNA=1: no further change");
  row(stop_of(sub_write_byte(0x03, 0x00)) == 0 && main_read_byte(0x03) == 0x02U, "2M sub writes RET=0 while it owns: no effect");
  v = 0xA55AU;
  row(access(SCD_SUB, 0x080010U, 1, &v) == 0, "2M sub stores a word while it owns");
  row(stop_of(sub_write_byte(0x03, 0x01)) == 0 && main_read_byte(0x03) == 0x01U, "2M sub writes RET=1: main owns, RET=1 DMNA=0");
  v = 0U;
  row(access(SCD_MAIN, 0x200010U, 0, &v) == 0 && v == 0xA55AU, "2M handoff preserves the sub's word for main");
  row(stop_of(main_write_byte(0x03, 0xC2)) == 0 && m->ga.prg_bank == 3U && main_read_byte(0x03) == 0xC2U,
      "conflicting order: main BK=3 with DMNA=1 after the return gives the sub ownership again");
  row(stop_of(sub_write_byte(0x03, 0x01)) == 0 && main_read_byte(0x03) == 0xC1U, "... and the sub's RET=1 returns it (BK kept)");
  row(access(SCD_SUB, 0x0C0000U, 0, &v) == SCD_STOP_UNMAPPED, "2M: the sub's 1M bank window is unmapped");
  row(stop_of(sub_write_byte(0x03, 0x09)) == SCD_STOP_WORDRAM_PRIORITY_MODE_UNSUPPORTED, "priority mode PM != 0: typed stop");

  /* ---- 1M banks ---- */
  fresh();
  for (v = 0; v < 8U; ++v) m->word_ram[v] = (uint8_t)(0x10U + v); /* 2M words 0..3 = 1011 1213 1415 1617 */
  row(stop_of(sub_write_byte(0x03, 0x04)) == 0 && sub_read_byte(0x03) == 0x04U && main_read_byte(0x03) == 0x04U,
      "sub writes MODE=1 RET=0: 1M mode (MODE reads 1 on both sides)");
  row(can(SCD_MAIN, 0x200000U) && can(SCD_SUB, 0x0C0000U), "1M: both processors access their banks at once");
  v = 0U;
  access(SCD_MAIN, 0x200000U, 0, &v);
  {
    uint32_t s = 0U, v2 = 0U;
    access(SCD_SUB, 0x0C0000U, 0, &s);
    access(SCD_MAIN, 0x200002U, 0, &v2);
    row(v == 0x1011U && s == 0x1213U && v2 == 0x1415U, "1M banks are the word interleave of the 2M space");
  }
  row(access(SCD_SUB, 0x080000U, 0, &v) == SCD_STOP_WORDRAM_DOT_IMAGE_UNSUPPORTED, "1M: the sub dot-image view is a typed stop");
  row(access(SCD_MAIN, 0x220000U, 0, &v) == SCD_STOP_WORDRAM_CELL_IMAGE_UNSUPPORTED, "1M: the main cell-image view is a typed stop");
  v = 0xBEEFU;
  access(SCD_MAIN, 0x200100U, 1, &v);
  v = 0x1234U;
  access(SCD_SUB, 0x0C0100U, 1, &v);
  row(stop_of(sub_write_byte(0x03, 0x05)) == 0 && sub_read_byte(0x03) == 0x05U, "sub writes RET=1 in 1M: swap");
  {
    uint32_t a = 0U, b = 0U;
    access(SCD_MAIN, 0x200100U, 0, &a);
    access(SCD_SUB, 0x0C0100U, 0, &b);
    row(a == 0x1234U && b == 0xBEEFU, "1M swap exchanges the banks between the processors");
  }
  row(stop_of(sub_write_byte(0x03, 0x05)) == 0 && can(SCD_MAIN, 0x200000U), "repeated RET=1 in 1M: no change");
  row(stop_of(main_write_byte(0x03, 0x02)) == SCD_STOP_WORDRAM_1M_DMNA_UNSUPPORTED, "main DMNA write in 1M mode: typed stop (U)");
  row(stop_of(sub_write_byte(0x03, 0x00)) == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, "1M -> 2M with RET=0: typed stop (U)");
  row(stop_of(sub_write_byte(0x03, 0x01)) == 0 && main_read_byte(0x03) == 0x01U && can(SCD_MAIN, 0x200000U),
      "1M -> 2M with RET=1: main owns 2M Word RAM");
  {
    uint32_t a = 0U, b = 0U;
    access(SCD_MAIN, 0x200000U + 4U * 0x80U, 0, &a);      /* bank 0 offset $100 = 2M offset 4 * $80 */
    access(SCD_MAIN, 0x200000U + 4U * 0x80U + 2U, 0, &b); /* bank 1 offset $100 = 2M offset 4 * $80 + 2 */
    row(a == 0xBEEFU && b == 0x1234U, "1M -> 2M keeps every byte in place (the interleave is the storage)");
  }

  /* ---- timer ---- */
  fresh();
  m->now = 1000U;
  period = (uint64_t)(9U + 1U) * SCD_TIMER_UNIT_SUB_CYCLES * SCD_SUB_CYCLE_TICKS;
  row(stop_of(sub_write_byte(0x31, 9U)) == 0 && scd_devices_next_event(m) == 1000U + period && sub_read_byte(0x31) == 9U,
      "timer N=9: first expiry after (N + 1) * 384 sub cycles; reads return N");
  t0 = scd_devices_next_event(m);
  m->now = t0;
  scd_devices_advance(m, t0);
  row((m->ga.pending & 0x08U) == 0U && scd_devices_next_event(m) == t0 + period, "expiry with IEN3 = 0 raises nothing and reloads");
  sub_write_byte(0x33, 0x08U);
  m->now = t0 + period;
  scd_devices_advance(m, m->now);
  row((m->ga.pending & 0x08U) != 0U && scd_devices_can_raise(m, 3U), "expiry with IEN3 = 1 raises level 3");
  row(stop_of(sub_write_byte(0x31, 0U)) == 0 && scd_devices_next_event(m) == UINT64_MAX && !scd_devices_can_raise(m, 3U),
      "writing 0 stops the timer");

  /* ---- interrupt enables ---- */
  fresh();
  row(stop_of(main_write_byte(0x00, 0x01)) == 0 && m->ga.ifl2 == 0U && (m->ga.pending & 0x04U) == 0U, "IFL2 with IEN2 = 0 is dropped");
  sub_write_byte(0x33, 0x04U);
  row(main_read_byte(0x00) == 0x80U, "IEN2 mirrors to main $A12000 bit 7");
  row(stop_of(main_write_byte(0x00, 0x01)) == 0 && m->ga.ifl2 == 1U && (m->ga.pending & 0x04U) != 0U && main_read_byte(0x00) == 0x81U,
      "IFL2 with IEN2 = 1 sets the flag and the level-2 request");
  m->ga.pending |= 0x02U;
  sub_write_byte(0x33, 0x06U);
  row((m->ga.pending & 0x02U) != 0U, "level 1 stays pending while IEN1 is set");
  sub_write_byte(0x33, 0x04U);
  row((m->ga.pending & 0x02U) == 0U && (m->ga.pending & 0x04U) != 0U, "clearing IEN1 drops a pending level 1 only");
  row(stop_of(sub_write_byte(0x01, 0x00)) == SCD_STOP_PERIPHERAL_RESET_UNSUPPORTED, "sub RESET=0 (peripheral reset): typed stop");
  {
    uint32_t w = 1U;
    row(scd_ga_sub_access(m, 0x10U, 2U, 1, &w, 0U) == 1 && m->stop.stop_class == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED,
        "sub write to COMCMD: typed stop (U)");
    m->stop.stop_class = SCD_STOP_NONE;
    row(scd_ga_main_access(m, 0x20U, 2U, 1, &w, 0U) == 1 && m->stop.stop_class == SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED,
        "main write to COMSTAT: typed stop (U)");
  }
  free(m);
  printf(failures ? "%d failing row(s)\n" : "all rows passed\n", failures);
  return failures ? 1 : 0;
}
