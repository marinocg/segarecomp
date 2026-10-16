/* SEG-062 Word RAM (contract section 6). SEG-062-T003 subset: only the main-side PRG-RAM bank select of $A12003 is live; every
 * Word RAM access and ownership change is the typed stop of the register or region until SEG-062-T004. */
#include "scd_runtime.h"

int scd_word_ram_main_mode_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  m->ga.prg_bank = (uint8_t)((value >> 6) & 3U);
  if (value & 0x02U) return scd_fail(m, SCD_MAIN, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xA12003U);
  return 0;
}

int scd_word_ram_sub_mode_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  (void)value;
  return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8003U);
}

int scd_word_ram_access(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  (void)width;
  (void)is_write;
  (void)value;
  return scd_fail(m, cpu, SCD_STOP_UNMAPPED, pc, address);
}

int scd_word_ram_dma_store(ScdMachine *m, uint32_t offset, uint16_t word) {
  (void)m;
  (void)offset;
  (void)word;
  return 1;
}
