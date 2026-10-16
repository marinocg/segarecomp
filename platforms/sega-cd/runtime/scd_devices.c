/* SEG-062 timed devices: the sub timer (SEG-062-T004) and the CD drive / CDC (SEG-062-T005, still typed stops here). */
#include "scd_runtime.h"

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

static void scd_timer_event(ScdMachine *m) {
  if (m->ga.interrupt_enable & 0x08U) scd_raise_sub_level(m, 3U);
  m->ga.timer_next += scd_timer_period(m);
}

int scd_cdd_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  (void)width;
  (void)is_write;
  (void)value;
  return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset);
}

int scd_cdc_sub_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  (void)width;
  (void)is_write;
  (void)value;
  return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset);
}

uint64_t scd_devices_next_event(const ScdMachine *m) {
  return m->ga.timer_next != 0U ? m->ga.timer_next : UINT64_MAX;
}

/* Process every device event at exactly tick `until` (the scheduler calls this with the earliest pending event time). */
void scd_devices_advance(ScdMachine *m, uint64_t until) {
  if (m->ga.timer_next != 0U && m->ga.timer_next <= until) scd_timer_event(m);
}

int scd_devices_can_raise(const ScdMachine *m, uint32_t level) {
  if (level == 3U) return m->ga.timer_next != 0U;
  return 0;
}
