/* SEG-062 Gate Array registers (contract section 5). Offsets are relative to $A12000 (main side) and $FF8000 (sub side).
 * Byte and word accesses follow the register semantics both references agree on; anything else is the typed stop
 * gate_array_register_unsupported carrying the side, offset, width and direction. */
#include "scd_runtime.h"

int scd_word_ram_main_mode_write(ScdMachine *m, uint32_t value, uint32_t pc);
int scd_word_ram_sub_mode_write(ScdMachine *m, uint32_t value, uint32_t pc);
int scd_timer_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_cdd_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);

static int scd_ga_unsupported(ScdMachine *m, uint32_t cpu, uint32_t base, uint32_t offset, uint32_t width, int is_write, uint32_t pc) {
  if (m->stop.stop_class == SCD_STOP_NONE) {
    m->stop.stop_class = SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED;
    m->stop.cpu = cpu;
    m->stop.pc = pc;
    m->stop.address = base + offset;
    m->stop.width = width;
    m->stop.is_write = is_write ? 1U : 0U;
    m->stop.tick = m->now;
  }
  return 1;
}

/* $A12001 written with `value` (bit 0 SRES, bit 1 SBRQ). */
static int scd_ga_reset_control(ScdMachine *m, uint32_t value, uint32_t pc) {
  ScdCpu *sub = &m->cpu[SCD_SUB];
  const int was_reset = m->ga.sub_reset;
  if (value & 1U) {
    const uint32_t busreq = (value >> 1) & 1U;
    if (busreq && !m->ga.sub_busreq && !m->ga.sub_reset && sub->irq.stopped)
      return scd_fail(m, SCD_MAIN, SCD_STOP_BUSREQ_WHILE_SUB_STOPPED_UNSUPPORTED, pc, value);
    m->ga.sub_reset = 0U;
    m->ga.sub_busreq = (uint8_t)busreq;
    if (was_reset) {
      /* SRES 0 -> 1: the sub takes its reset exception from PRG RAM $000000 (SSP) and $000004 (PC) (contract section 4). */
      uint32_t i, ssp = 0U, entry = 0U;
      for (i = 0; i < 4U; ++i) {
        ssp = (ssp << 8U) | m->prg_ram[i];
        entry = (entry << 8U) | m->prg_ram[4U + i];
      }
      sub->a[7] = ssp;
      sub->pc = entry & UINT32_C(0x00FFFFFF);
      sub->sr = UINT16_C(0x2700);
      sub->irq.stopped = 0U;
      sub->irq.sampled_level = 0U;
      sub->irq.level7_edge_pending = 0U;
      if (sub->next < m->now) sub->next = m->now;
      sub->next += UINT64_C(40) * sub->cycle_ticks; /* MC68000 reset exception processing: 40 clocks (UM68K) */
      scd_log(m, 0x10U, ssp, entry);
    } else if (!busreq && sub->next < m->now) {
      sub->next = m->now; /* released from a bus request */
    }
  } else {
    /* Writing SRES = 0 asserts reset and halt; the register reads back $02 (V1, GPGX "verified on real hardware"). */
    m->ga.sub_reset = 1U;
    m->ga.sub_busreq = 1U;
  }
  scd_log(m, 0x11U, value, (uint32_t)(m->ga.sub_reset | (m->ga.sub_busreq << 1)));
  return 0;
}

static void scd_ga_ifl2(ScdMachine *m) {
  if (m->ga.interrupt_enable & 0x04U) {
    m->ga.ifl2 = 1U;
    scd_raise_sub_level(m, 2U);
  } else {
    scd_log(m, 0x12U, 0U, 0U); /* IEN2 = 0: the request is dropped (V2) */
  }
}

static uint32_t scd_ga_main_status_byte(const ScdMachine *m, uint32_t offset) {
  switch (offset) {
  case 0x00: return (uint32_t)(((m->ga.interrupt_enable & 0x04U) ? 0x80U : 0U) | m->ga.ifl2);
  case 0x01: return (uint32_t)((m->ga.sub_busreq << 1) | (m->ga.sub_reset ? 0U : 1U));
  case 0x02: return m->ga.write_protect;
  case 0x03: return (uint32_t)((m->ga.prg_bank << 6) | (m->ga.mode_1m << 2) | (m->ga.dmna << 1) | m->ga.ret);
  case 0x0E: return m->ga.main_flag;
  case 0x0F: return m->ga.sub_flag;
  default: break;
  }
  return 0U;
}

int scd_ga_main_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  scd_log(m, is_write ? 0x21U : 0x20U, offset, is_write ? *value : 0U);
  if (offset >= 0x10U && offset < 0x20U) { /* COMCMD0-7: main read/write */
    uint16_t *word = &m->ga.comcmd[(offset - 0x10U) >> 1];
    if (!is_write) *value = width == 2U ? *word : ((offset & 1U) ? (uint32_t)(*word & 0xFFU) : (uint32_t)(*word >> 8));
    else if (width == 2U) *word = (uint16_t)*value;
    else if (offset & 1U) *word = (uint16_t)((*word & 0xFF00U) | (*value & 0xFFU));
    else *word = (uint16_t)((*word & 0x00FFU) | ((*value & 0xFFU) << 8));
    return 0;
  }
  if (offset >= 0x20U && offset < 0x30U) { /* COMSTAT0-7: main read-only (a write is U) */
    const uint16_t word = m->ga.comstat[(offset - 0x20U) >> 1];
    if (is_write) return scd_ga_unsupported(m, SCD_MAIN, 0xA12000U, offset, width, is_write, pc);
    *value = width == 2U ? word : ((offset & 1U) ? (uint32_t)(word & 0xFFU) : (uint32_t)(word >> 8));
    return 0;
  }
  if (offset <= 0x03U || offset == 0x0EU || offset == 0x0FU) {
    if (!is_write) {
      *value = width == 2U ? ((scd_ga_main_status_byte(m, offset) << 8) | scd_ga_main_status_byte(m, offset + 1U))
                           : scd_ga_main_status_byte(m, offset);
      return 0;
    }
    switch (offset) {
    case 0x00:
      if (width == 2U) {
        if (scd_ga_reset_control(m, *value & 0x03U, pc) != 0) return 1; /* reset/halt first, then IFL2 (GPGX order) */
        if (*value & 0x0100U) scd_ga_ifl2(m);
      } else if (*value & 0x01U) {
        scd_ga_ifl2(m);
      }
      return 0;
    case 0x01: return scd_ga_reset_control(m, *value & 0x03U, pc);
    case 0x02:
      if (width == 2U) {
        m->ga.write_protect = (uint8_t)(*value >> 8);
        return scd_word_ram_main_mode_write(m, *value & 0xFFU, pc);
      }
      m->ga.write_protect = (uint8_t)*value;
      return 0;
    case 0x03: return scd_word_ram_main_mode_write(m, *value & 0xFFU, pc);
    case 0x0E:
    case 0x0F: /* main flag; a word write stores its low byte (V2) */
      m->ga.main_flag = (uint8_t)*value;
      return 0;
    default: break;
    }
  }
  return scd_ga_unsupported(m, SCD_MAIN, 0xA12000U, offset, width, is_write, pc);
}

int scd_ga_sub_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  scd_log(m, is_write ? 0x31U : 0x30U, offset, is_write ? *value : 0U);
  if (offset >= 0x04U && offset < 0x0EU) return scd_cdc_sub_access(m, offset, width, is_write, value, pc);
  if (offset == 0x30U || offset == 0x31U) return scd_timer_access(m, offset, width, is_write, value, pc);
  if (offset >= 0x36U && offset < 0x4CU) return scd_cdd_access(m, offset, width, is_write, value, pc);
  if (offset >= 0x10U && offset < 0x20U) { /* COMCMD0-7: sub read-only (a write is U) */
    const uint16_t word = m->ga.comcmd[(offset - 0x10U) >> 1];
    if (is_write) return scd_ga_unsupported(m, SCD_SUB, 0xFF8000U, offset, width, is_write, pc);
    *value = width == 2U ? word : ((offset & 1U) ? (uint32_t)(word & 0xFFU) : (uint32_t)(word >> 8));
    return 0;
  }
  if (offset >= 0x20U && offset < 0x30U) { /* COMSTAT0-7: sub read/write */
    uint16_t *word = &m->ga.comstat[(offset - 0x20U) >> 1];
    if (!is_write) *value = width == 2U ? *word : ((offset & 1U) ? (uint32_t)(*word & 0xFFU) : (uint32_t)(*word >> 8));
    else if (width == 2U) *word = (uint16_t)*value;
    else if (offset & 1U) *word = (uint16_t)((*word & 0xFF00U) | (*value & 0xFFU));
    else *word = (uint16_t)((*word & 0x00FFU) | ((*value & 0xFFU) << 8));
    return 0;
  }
  switch (offset) {
  case 0x00:
  case 0x01:
    if (!is_write) {
      *value = width == 2U ? (uint32_t)((m->ga.led << 8) | 1U) : (offset == 0x00U ? m->ga.led : 1U);
      return 0;
    }
    if (width == 2U || offset == 0x00U) m->ga.led = (uint8_t)(width == 2U ? (*value >> 8) : *value);
    if ((width == 2U || offset == 0x01U) && !(*value & 1U))
      return scd_fail(m, SCD_SUB, SCD_STOP_PERIPHERAL_RESET_UNSUPPORTED, pc, *value);
    return 0;
  case 0x02:
  case 0x03:
    if (!is_write) {
      const uint32_t low = (uint32_t)((m->ga.mode_1m << 2) | (m->ga.dmna << 1) | m->ga.ret);
      *value = width == 2U ? (uint32_t)((m->ga.write_protect << 8) | low) : (offset == 0x02U ? m->ga.write_protect : low);
      return 0;
    }
    if (width == 1U && offset == 0x02U) return 0; /* WP is not writable from the sub (V2) */
    return scd_word_ram_sub_mode_write(m, *value & 0xFFU, pc);
  case 0x0E:
  case 0x0F:
    if (!is_write) {
      *value = width == 2U ? (uint32_t)((m->ga.main_flag << 8) | m->ga.sub_flag) : (offset == 0x0EU ? m->ga.main_flag : m->ga.sub_flag);
      return 0;
    }
    m->ga.sub_flag = (uint8_t)*value; /* the sub flag; a word write stores its low byte (V2) */
    return 0;
  case 0x32:
  case 0x33:
    if (!is_write) {
      *value = m->ga.interrupt_enable;
      return 0;
    }
    if (width == 1U && offset == 0x32U) return 0; /* the upper byte holds nothing */
    m->ga.interrupt_enable = (uint8_t)(*value & 0x7EU);
    if (!(m->ga.interrupt_enable & 0x02U)) m->ga.pending = (uint8_t)(m->ga.pending & ~0x02U); /* V2: level 1 dropped when disabled */
    return 0;
  default: break;
  }
  return scd_ga_unsupported(m, SCD_SUB, 0xFF8000U, offset, width, is_write, pc);
}
