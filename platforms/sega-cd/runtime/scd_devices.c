/* SEG-062 timed devices: the sub timer (SEG-062-T004) and the CD drive / CDC (SEG-062-T005). SEG-062-T003 subset: none is
 * implemented; their registers are typed stops and they never raise an event. */
#include "scd_runtime.h"

int scd_timer_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  (void)width;
  (void)is_write;
  (void)value;
  return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset);
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
  (void)m;
  return UINT64_MAX;
}

void scd_devices_advance(ScdMachine *m, uint64_t until) {
  (void)m;
  (void)until;
}

int scd_devices_can_wake_sub(const ScdMachine *m) {
  (void)m;
  return 0;
}
