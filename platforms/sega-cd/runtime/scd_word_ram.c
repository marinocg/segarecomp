/* SEG-062-T004: Word RAM ownership and views (contract section 6). Where the references disagree this follows Genesis Plus GX
 * (core/cd_hw/scd.c memory-mode handlers, core/mem68k.c $A12003), consulted only.
 *
 * Storage is always the 256 KiB 2M layout. In 1M mode bank b's byte offset o (0..$1FFFF) is the 2M byte offset 4 * (o / 2) +
 * 2 * b + (o & 1): the banks are the word interleave of the 2M space (V2: GPGX word_ram_switch, MAME segacd_1meg_mode_word_read),
 * so switching modes needs no copy. Ownership: in 2M mode RET = 1 maps Word RAM to main, RET = 0 to the sub; in 1M mode main sees
 * bank RET and the sub the other bank.
 *
 * The memory-mode register: DMNA and RET as the processors read them, plus GPGX's internal request latch `dmna_int` (scd.dmna):
 *   main, 2M, DMNA = 1   Word RAM goes to the sub at once (RET = 0, DMNA = 1, request latched); a sub waiting for it resumes and a
 *                        CDC DMA halted on main ownership continues
 *   main, 1M, DMNA = 1   only latches the request (a later 1M -> 2M switch hands 2M Word RAM to the sub)
 *   main, 1M, DMNA = 0   sets the DMNA bit (swap pending) until the sub next changes RET or MODE
 *   sub, RET/MODE change 1M: RET selects the banks, DMNA clears, RET = 1 drops the request; 1M -> 2M: RET is forced to 1 unless a
 *                        request is latched (then the sub keeps 2M Word RAM with DMNA = 1); 2M RET = 1: Word RAM returns to main
 *                        (a CDC DMA into 2M Word RAM halts), the request drops
 *   sub, no RET/MODE change   only PM1/PM0 are stored (they act on graphics and dot-image writes, which are not modelled) */
#include "scd_runtime.h"

static uint32_t scd_bank_offset(uint32_t bank, uint32_t offset) { return (offset >> 1) * 4U + bank * 2U + (offset & 1U); }
static uint32_t scd_main_bank(const ScdMachine *m) { return (m->ga.mode & SCD_MODE_RET) ? 1U : 0U; }

static void scd_word_ram_resume_sub(ScdMachine *m) {
  if (m->ga.sub_wait) {
    ScdCpu *sub = &m->cpu[SCD_SUB];
    m->ga.sub_wait = 0U;
    if (sub->next < m->now) sub->next = m->now;
    scd_log(m, 0x15U, sub->pc, 0U);
  }
}

int scd_word_ram_main_mode_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  (void)pc;
  if (scd_cd_observe(m) != 0) return 1;
  m->ga.prg_bank = (uint8_t)((value >> 6) & 3U);
  if (m->ga.mode & SCD_MODE_1M) {
    if (value & 0x02U) m->ga.dmna_int = 1U; /* 1M: DMNA = 1 only latches the request */
    else m->ga.mode = (uint8_t)(m->ga.mode | SCD_MODE_DMNA); /* 1M: DMNA = 0 sets the DMNA bit */
    scd_log(m, 0x74U, value, m->ga.mode);
    return 0;
  }
  if (value & 0x02U) { /* 2M, DMNA = 1: Word RAM to the sub */
    m->ga.dmna_int = 1U;
    m->ga.mode = (uint8_t)((m->ga.mode & ~SCD_MODE_RET) | SCD_MODE_DMNA);
    scd_log(m, 0x70U, value, m->ga.mode);
    scd_word_ram_resume_sub(m);
    scd_cdc_dma_resume_2m(m);
  }
  return 0; /* 2M, DMNA = 0: no effect */
}

int scd_word_ram_sub_mode_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  const uint32_t current = m->ga.mode;
  (void)pc;
  if (scd_cd_observe(m) != 0) return 1;
  value &= 0x1FU;
  if ((value ^ current) & (SCD_MODE_1M | SCD_MODE_RET)) {
    if (value & SCD_MODE_1M) { /* 1M mode (from 2M, or a RET change in 1M): RET selects the banks, the swap completes at once */
      if (value & SCD_MODE_RET) m->ga.dmna_int = 0U;
      m->ga.mode = (uint8_t)(value & 0x1DU);
      scd_log(m, 0x71U, value, m->ga.mode);
      return 0;
    }
    if (current & SCD_MODE_1M) { /* 1M -> 2M */
      value |= ~(uint32_t)m->ga.dmna_int & 1U;
      if (!(value & SCD_MODE_RET)) { /* a latched request: the sub keeps 2M Word RAM */
        m->ga.mode = (uint8_t)((value | SCD_MODE_DMNA) & 0x1FU);
        scd_log(m, 0x72U, value, m->ga.mode);
        return 0;
      }
    }
    if (value & SCD_MODE_RET) { /* 2M, RET = 1: Word RAM returns to main */
      scd_cdc_dma_halt_2m(m);
      m->ga.dmna_int = 0U;
      m->ga.mode = (uint8_t)(value & 0x1DU);
      scd_log(m, 0x73U, value, m->ga.mode);
      return 0;
    }
  }
  m->ga.mode = (uint8_t)((m->ga.mode & ~0x1CU) | (value & 0x1CU)); /* PM1, PM0 (and an unchanged MODE) */
  return 0;
}

/* `address` is the main view folded to $200000-$23FFFF or the sub view folded to $080000-$0DFFFF. */
int scd_word_ram_access(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  uint32_t offset;
  if (scd_cd_observe(m) != 0) return 1; /* an in-flight CDC DMA is brought up to this instruction first */
  if (cpu == SCD_MAIN) {
    if (!(m->ga.mode & SCD_MODE_1M)) {
      if (!(m->ga.mode & SCD_MODE_RET)) { /* 2M owned by the sub: unmapped for main (GPGX m68k_read_bus / m68k_unused_w) */
        if (!is_write) return scd_fail(m, cpu, SCD_STOP_OPEN_BUS_READ, pc, address);
        ++m->ignored_writes;
        scd_log(m, 0x51U, address, *value);
        return 0;
      }
      offset = address - UINT32_C(0x00200000);
    } else {
      if (address >= UINT32_C(0x00220000)) return scd_fail(m, cpu, SCD_STOP_WORDRAM_CELL_IMAGE_UNSUPPORTED, pc, address);
      offset = scd_bank_offset(scd_main_bank(m), address - UINT32_C(0x00200000));
    }
  } else if (address < UINT32_C(0x000C0000)) {
    if (m->ga.mode & SCD_MODE_1M) return scd_fail(m, cpu, SCD_STOP_WORDRAM_DOT_IMAGE_UNSUPPORTED, pc, address);
    if (m->ga.mode & SCD_MODE_RET) { /* 2M owned by main: DTACK is withheld, the sub waits (scheduler) */
      m->sub_wait_request = 1U;
      scd_log(m, 0x52U, address, (uint32_t)is_write);
      return 1;
    }
    offset = address - UINT32_C(0x00080000);
  } else {
    if (!(m->ga.mode & SCD_MODE_1M)) { /* $0C0000-$0DFFFF is unused in 2M mode */
      if (!is_write) return scd_fail(m, cpu, SCD_STOP_OPEN_BUS_READ, pc, address);
      ++m->ignored_writes;
      scd_log(m, 0x51U, address, *value);
      return 0;
    }
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
