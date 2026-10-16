/* SEG-062-T004: the sub timer. The CD drive / CDC and the aggregation of all timed device events are in scd_cd.c. */
#include "scd_runtime.h"

uint64_t scd_timer_next_event(const ScdMachine *m);
void scd_timer_event(ScdMachine *m);
int scd_timer_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);

/* $FF8030/$FF8031 timer (contract section 5): writing N != 0 starts it with a period of (N + 1) * 384 sub cycles (rate V2, the "+ 1"
 * V1 from GPGX citing a hardware test; MAME uses N * 384 - section 13); writing 0 stops it; reads return the written value (V2).
 * At each expiry it raises level 3 only if IEN3 is set, then reloads. */
static uint64_t scd_timer_period(const ScdMachine *m) {
  return ((uint64_t)m->ga.timer_reload + 1U) * SCD_TIMER_UNIT_SUB_CYCLES * SCD_SUB_CYCLE_TICKS;
}

int scd_timer_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  (void)pc;
  if (!is_write) {
    *value = (width == 1U && offset == 0x30U) ? 0U : m->ga.timer_reload;
    return 0;
  }
  if (width == 1U && offset == 0x30U) return 0; /* the upper byte holds nothing */
  m->ga.timer_reload = (uint8_t)*value;
  m->ga.timer_next = m->ga.timer_reload != 0U ? m->now + scd_timer_period(m) : 0U;
  scd_log(m, 0x80U, m->ga.timer_reload, (uint32_t)(m->ga.timer_next >> 20));
  return 0;
}

void scd_timer_event(ScdMachine *m) {
  if (m->ga.interrupt_enable & 0x08U) scd_raise_sub_level(m, 3U);
  m->ga.timer_next += scd_timer_period(m);
}

uint64_t scd_timer_next_event(const ScdMachine *m) { return m->ga.timer_next != 0U ? m->ga.timer_next : UINT64_MAX; }
