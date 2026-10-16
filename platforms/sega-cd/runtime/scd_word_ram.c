/* SEG-062-T004: Word RAM ownership and views (contract section 6).
 *
 * Storage is always the 256 KiB 2M layout. In 1M mode bank b's byte offset o (0..$1FFFF) is the 2M byte offset 4 * (o / 2) +
 * 2 * b + (o & 1): the banks are the word interleave of the 2M space (V2: GPGX word_ram_switch, MAME segacd_1meg_mode_word_read),
 * so switching modes needs no copy. Ownership: 2M has one owner (RET = 1: main, RET = 0: sub); 1M gives the main processor bank
 * RET and the sub the other bank (which physical bank is called 0 is U; no fixture observes it). */
#include "scd_runtime.h"

static uint32_t scd_bank_offset(uint32_t bank, uint32_t offset) { return (offset >> 1) * 4U + bank * 2U + (offset & 1U); }
static uint32_t scd_main_bank(const ScdMachine *m) { return m->ga.ret ? 1U : 0U; }

int scd_word_ram_main_mode_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  if (scd_cd_observe(m) != 0) return 1;
  m->ga.prg_bank = (uint8_t)((value >> 6) & 3U);
  if (m->ga.mode_1m) return scd_fail(m, SCD_MAIN, SCD_STOP_WORDRAM_1M_DMNA_UNSUPPORTED, pc, value); /* U: 1M swap requests */
  if (value & 0x02U) { /* DMNA = 1 in 2M mode: Word RAM passes to the sub at once (V2); repeated requests change nothing */
    m->ga.ret = 0U;
    m->ga.dmna = 1U;
    scd_log(m, 0x70U, value, 0U);
  }
  return 0; /* DMNA = 0 in 2M mode: no effect (V1) */
}

int scd_word_ram_sub_mode_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  const uint32_t ret = value & 1U;
  if (scd_cd_observe(m) != 0) return 1;
  if (value & 0x18U) return scd_fail(m, SCD_SUB, SCD_STOP_WORDRAM_PRIORITY_MODE_UNSUPPORTED, pc, value);
  if (value & 0x04U) { /* 1M mode: RET selects the banks; the switch and every swap complete at once (V2) */
    m->ga.mode_1m = 1U;
    m->ga.ret = (uint8_t)ret;
    m->ga.dmna = 0U;
    scd_log(m, 0x71U, value, 1U);
    return 0;
  }
  if (m->ga.mode_1m) { /* 1M -> 2M */
    if (!ret) return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8003U); /* U: lands with the sub owning */
    m->ga.mode_1m = 0U;
    m->ga.ret = 1U;
    m->ga.dmna = 0U;
    scd_log(m, 0x72U, value, 0U);
    return 0;
  }
  if (ret && !m->ga.ret) { /* RET = 1 in 2M mode: Word RAM returns to main (V2) */
    if (m->cdc.dma_active && m->cdc.destination == 7U)
      return scd_fail(m, SCD_SUB, SCD_STOP_CDC_DMA_DESTINATION_NOT_OWNED, pc, value); /* U: GPGX halts the DMA, MAME continues */
    m->ga.ret = 1U;
    m->ga.dmna = 0U;
    scd_log(m, 0x73U, value, 0U);
  }
  return 0; /* RET = 0, or RET = 1 while main already owns: no effect (V2) */
}

int scd_word_ram_access(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  uint32_t offset;
  if (scd_cd_observe(m) != 0) return 1; /* an in-flight CDC DMA is brought up to this instruction first */
  if (cpu == SCD_MAIN) {
    if (!m->ga.mode_1m) {
      if (!m->ga.ret) return scd_fail(m, cpu, SCD_STOP_WORDRAM_NOT_OWNED, pc, address);
      offset = address - UINT32_C(0x00200000);
    } else {
      if (address >= UINT32_C(0x00220000)) return scd_fail(m, cpu, SCD_STOP_WORDRAM_CELL_IMAGE_UNSUPPORTED, pc, address);
      offset = scd_bank_offset(scd_main_bank(m), address - UINT32_C(0x00200000));
    }
  } else if (address < UINT32_C(0x000C0000)) {
    if (m->ga.mode_1m) return scd_fail(m, cpu, SCD_STOP_WORDRAM_DOT_IMAGE_UNSUPPORTED, pc, address);
    if (m->ga.ret) return scd_fail(m, cpu, SCD_STOP_WORDRAM_NOT_OWNED, pc, address); /* the real sub CPU would hang (V2) */
    offset = address - UINT32_C(0x00080000);
  } else {
    if (!m->ga.mode_1m) return scd_fail(m, cpu, SCD_STOP_UNMAPPED, pc, address);
    offset = scd_bank_offset(scd_main_bank(m) ^ 1U, address - UINT32_C(0x000C0000));
  }
  if (is_write) {
    if (width == 1U) {
      m->word_ram[offset] = (uint8_t)*value;
    } else {
      m->word_ram[offset] = (uint8_t)(*value >> 8);
      m->word_ram[offset + 1U] = (uint8_t)*value;
    }
  } else {
    *value = width == 1U ? m->word_ram[offset] : (uint32_t)((m->word_ram[offset] << 8) | m->word_ram[offset + 1U]);
  }
  return 0;
}

/* CDC DMA destination 7 (contract section 7): `offset` is the byte offset in the sub-owned view. Returns 1 when the sub does not
 * own the destination (2M Word RAM owned by main: U). */
int scd_word_ram_dma_store(ScdMachine *m, uint32_t offset, uint16_t word) {
  uint32_t at;
  if (!m->ga.mode_1m) {
    if (m->ga.ret) return 1;
    at = offset & 0x3FFFEU;
  } else {
    at = scd_bank_offset(scd_main_bank(m) ^ 1U, offset & 0x1FFFEU);
  }
  m->word_ram[at] = (uint8_t)(word >> 8);
  m->word_ram[at + 1U] = (uint8_t)word;
  return 0;
}
