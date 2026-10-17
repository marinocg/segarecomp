/* SEG-062 Gate Array registers (contract section 5). Offsets are relative to $A12000 (main side, A1-A5 decode: $A12000-$A1203F
 * mirrored up to $A120FF) and $FF8000 (sub side, A1-A8 decode). Register semantics follow Genesis Plus GX (core/mem68k.c ctrl_io_*,
 * core/cd_hw/scd.c scd_read_* / scd_write_*), consulted only, where it and MAME disagree:
 *   - writes to registers without a writable function are ignored (main: COMSTAT and $04-$0D; sub: COMCMD, the memory-mode word);
 *   - main reads of $A12030-$A1203F, like other unused areas, return open bus: the typed stop open_bus_read;
 *   - "!LDS/!UDS ignored" registers ($FF8002/3, $FF800E/F, $FF8030/1, $A1200E/F) act on a byte write to either half;
 *   - word-only sub registers ($FF8008-$FF800D) take a byte write as the byte duplicated into both halves;
 *   - RES0 = 0 is the peripheral reset (scd_peripheral_reset); SBRQ reads 0 when the sub is halted in STOP.
 * Devices this profile does not model (stopwatch, fader output, font, graphics, subcode, CDC host data, the BIOS H-INT vector)
 * remain typed stops carrying the side, offset, width and direction. */
#include "scd_runtime.h"

int scd_timer_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_cdd_register_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);
int scd_cdc_status_read(ScdMachine *m, uint32_t *status);
int scd_cdc_dma_address_access(ScdMachine *m, int is_write, uint32_t *value);

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

static int scd_ga_stop(ScdMachine *m, uint32_t cpu, ScdStopClass stop_class, uint32_t address, uint32_t width, int is_write,
                       uint32_t pc) {
  if (m->stop.stop_class == SCD_STOP_NONE) {
    m->stop.stop_class = stop_class;
    m->stop.cpu = cpu;
    m->stop.pc = pc;
    m->stop.address = address;
    m->stop.width = width;
    m->stop.is_write = is_write ? 1U : 0U;
    m->stop.tick = m->now;
  }
  return 1;
}

static void scd_ga_ignored(ScdMachine *m, uint32_t address, uint32_t value) {
  ++m->ignored_writes;
  scd_log(m, 0x51U, address, value);
}

static uint32_t scd_byte_of(uint32_t word, uint32_t offset) { return (offset & 1U) ? (word & 0xFFU) : (word >> 8); }

static void scd_word_put(uint16_t *word, uint32_t offset, uint32_t width, uint32_t value) {
  if (width == 2U) *word = (uint16_t)value;
  else if (offset & 1U) *word = (uint16_t)((*word & 0xFF00U) | (value & 0xFFU));
  else *word = (uint16_t)((*word & 0x00FFU) | ((value & 0xFFU) << 8));
}

/* The SRES/SBRQ write ($A12001, or the low byte of a word write to $A12000), GPGX ctrl_io_write_byte case 0x01. */
static void scd_ga_reset_control(ScdMachine *m, uint32_t value, uint32_t pc) {
  ScdCpu *sub = &m->cpu[SCD_SUB];
  const uint32_t before = (uint32_t)(sub->irq.stopped | (m->ga.sub_halted << 1) | (m->ga.sub_wait << 2));
  uint32_t after;
  if (value & 1U) {
    if (!(m->ga.ctl & 1U) && m->bios != NULL) {
      /* SEG-064-T002 (ADR 0142 decision 3): in the BIOS-image profile the reset is armed here and the sub's reset exception - its
       * vector fetch is a bus cycle the sub cannot make while SBRQ grants the bus to main - is taken at the first runnable
       * transition, from the PRG RAM verified there (scd_sub_runnable_epoch). */
      m->sub_pristine = 1U;
      sub->irq.stopped = 0U;
      m->ga.sub_wait = 0U;
      scd_log(m, 0x10U, 0U, 0U);
    } else if (!(m->ga.ctl & 1U)) {
      /* SRES 0 -> 1: the sub takes its reset exception from PRG RAM $000000 (SSP) and $000004 (PC) (contract section 4); every
       * stop level (STOP, halt, DTACK wait) clears. */
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
      m->ga.sub_wait = 0U;
      if (sub->next < m->now) sub->next = m->now;
      sub->next += UINT64_C(40) * sub->cycle_ticks; /* MC68000 reset exception processing: 40 clocks (UM68K) */
      scd_log(m, 0x10U, ssp, entry);
    }
    if (value & 2U) {
      m->ga.sub_halted = 1U;
    } else {
      if (m->ga.sub_halted && sub->next < m->now) sub->next = m->now; /* released from the bus request */
      m->ga.sub_halted = 0U;
    }
    m->ga.ctl = (uint8_t)(value & 3U);
  } else {
    m->ga.sub_halted = 1U; /* SRES = 0: halt (and reset) asserted; the register reads back $02 (V1, GPGX "verified on real hardware") */
    m->ga.ctl = 0x02U;
  }
  if (sub->irq.stopped) m->ga.ctl = (uint8_t)(m->ga.ctl & ~2U); /* SBRQ reads 0 when the sub is halted in STOP (GPGX, V1) */
  after = (uint32_t)(sub->irq.stopped | (m->ga.sub_halted << 1) | (m->ga.sub_wait << 2));
  if (after != before) { /* the window follows SBRQ when the halt state changes */
    const uint8_t window = (uint8_t)((m->ga.ctl >> 1) & 1U);
    if (window != m->ga.prg_window) {
      m->ga.prg_window = window;
      scd_overlay_note_view_change(m); /* SEG-063: main's view of $020000-$03FFFF changed */
    }
  }
  scd_log(m, 0x11U, value, (uint32_t)(m->ga.ctl | (m->ga.prg_window << 4)));
  /* SEG-064-T002 (ADR 0142 decision 3/4): the sub's first runnable transition after its reset is the materialization epoch. The
   * BIOS-image profile only; it may record a typed stop (the caller fails the writing instruction). */
  if (m->bios != NULL && m->sub_pristine && !m->ga.sub_halted) (void)scd_sub_runnable_epoch(m, pc);
}

static void scd_ga_ifl2(ScdMachine *m) {
  if (m->ga.interrupt_enable & 0x04U) {
    m->ga.ifl2 = 1U;
    scd_raise_sub_level(m, 2U);
  } else {
    scd_log(m, 0x12U, 0U, 0U); /* IEN2 = 0: the request is dropped (V2) */
  }
}

static void scd_ga_interrupt_enable(ScdMachine *m, uint32_t value) {
  m->ga.interrupt_enable = (uint8_t)value;
  m->ga.ien2_mirror = (uint8_t)((value >> 2) & 1U);
  if (!(value & 0x02U)) m->ga.pending = (uint8_t)(m->ga.pending & ~0x02U); /* V2: level 1 dropped when disabled */
}

/* ---- main side ------------------------------------------------------------------------------------------------------------ */

/* Main byte reads; returns 0 and the value, or 1 after a stop. */
static int scd_ga_main_read_byte(ScdMachine *m, uint32_t index, uint32_t *value, uint32_t pc) {
  switch (index) {
  case 0x00: *value = (uint32_t)((m->ga.ien2_mirror << 7) | m->ga.ifl2); return 0;
  case 0x01: *value = m->ga.ctl; return 0;
  case 0x02: *value = m->ga.write_protect; return 0;
  case 0x03: *value = ((uint32_t)m->ga.prg_bank << 6) | (m->ga.mode & 0x07U); return 0; /* PM1/PM0 masked on the main side */
  case 0x04: {
    uint32_t status;
    if (scd_cdc_status_read(m, &status) != 0) return 1;
    *value = status & 0xC7U;
    return 0;
  }
  case 0x05: *value = 0U; return 0; /* the CDC register address is not visible from main */
  case 0x06: *value = m->ga.led; return 0;
  case 0x07: *value = 1U; return 0;
  case 0x08:
  case 0x09: *value = 0xFFU; return 0; /* the CDC host-data register keeps its reset value: the host path is not modelled */
  case 0x0A:
  case 0x0B: {
    uint32_t word;
    if (scd_cdc_dma_address_access(m, 0, &word) != 0) return 1;
    *value = scd_byte_of(word, index);
    return 0;
  }
  case 0x0E: *value = m->ga.main_flag; return 0;
  case 0x0F: *value = m->ga.sub_flag; return 0;
  default: break;
  }
  if (index >= 0x10U && index < 0x20U) {
    *value = scd_byte_of(m->ga.comcmd[(index - 0x10U) >> 1], index);
    return 0;
  }
  if (index >= 0x20U && index < 0x30U) {
    *value = scd_byte_of(m->ga.comstat[(index - 0x20U) >> 1], index);
    return 0;
  }
  if (index >= 0x30U) return scd_ga_stop(m, SCD_MAIN, SCD_STOP_OPEN_BUS_READ, 0xA12000U + index, 1U, 0, pc);
  return scd_ga_unsupported(m, SCD_MAIN, 0xA12000U, index, 1U, 0, pc); /* $0C/$0D: stopwatch */
}

int scd_ga_main_access(ScdMachine *m, uint32_t index, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  scd_log(m, is_write ? 0x21U : 0x20U, index, is_write ? *value : 0U);
  if (!is_write) {
    uint32_t high = 0U, low = 0U;
    if (width == 1U) return scd_ga_main_read_byte(m, index, value, pc);
    switch (index) {
    case 0x06: return scd_ga_stop(m, SCD_MAIN, SCD_STOP_BIOS_ABSENT, 0xA12006U, 2U, 0, pc); /* H-INT vector: BIOS ROM word */
    case 0x08: return scd_ga_stop(m, SCD_MAIN, SCD_STOP_CDC_DESTINATION_UNSUPPORTED, 0xA12008U, 2U, 0, pc); /* host data */
    case 0x04:
      if (scd_ga_main_read_byte(m, 0x04U, &high, pc) != 0) return 1;
      *value = high << 8; /* the low byte (CDC register address) reads 0 */
      return 0;
    case 0x0C: return scd_ga_unsupported(m, SCD_MAIN, 0xA12000U, index, 2U, 0, pc);
    default: break;
    }
    if (index >= 0x30U) return scd_ga_stop(m, SCD_MAIN, SCD_STOP_OPEN_BUS_READ, 0xA12000U + index, 2U, 0, pc);
    if (index == 0x02U) high = m->ga.write_protect;
    else if (scd_ga_main_read_byte(m, index, &high, pc) != 0) return 1;
    if (scd_ga_main_read_byte(m, index + 1U, &low, pc) != 0) return 1;
    *value = (high << 8) | low;
    return 0;
  }
  switch (index) {
  case 0x00:
    if (width == 2U) {
      scd_ga_reset_control(m, *value & 0x03U, pc); /* reset/halt first, then IFL2 (GPGX order) */
      if (m->stop.stop_class != SCD_STOP_NONE) return 1;
      if (*value & 0x0100U) scd_ga_ifl2(m);
    } else if (*value & 0x01U) {
      scd_ga_ifl2(m); /* writing 0 does nothing */
    }
    return 0;
  case 0x01:
    if (width == 1U) scd_ga_reset_control(m, *value & 0x03U, pc);
    return m->stop.stop_class != SCD_STOP_NONE ? 1 : 0;
  case 0x02:
    if (width == 2U) {
      m->ga.write_protect = (uint8_t)(*value >> 8);
      return scd_word_ram_main_mode_write(m, *value & 0xFFU, pc);
    }
    m->ga.write_protect = (uint8_t)*value;
    return 0;
  case 0x03: return width == 1U ? scd_word_ram_main_mode_write(m, *value & 0xFFU, pc) : 0;
  case 0x06:
    if (width == 2U) return scd_ga_stop(m, SCD_MAIN, SCD_STOP_BIOS_ABSENT, 0xA12006U, 2U, 1, pc); /* H-INT vector */
    break;
  case 0x08:
    if (width == 2U) return scd_ga_stop(m, SCD_MAIN, SCD_STOP_CDC_DESTINATION_UNSUPPORTED, 0xA12008U, 2U, 1, pc);
    break;
  case 0x0E:
  case 0x0F: /* main flag: either byte, or the low byte of a word write (V2) */
    m->ga.main_flag = (uint8_t)*value;
    return 0;
  default: break;
  }
  if (index >= 0x10U && index < 0x20U) { /* COMCMD0-7: main read/write */
    scd_word_put(&m->ga.comcmd[(index - 0x10U) >> 1], index, width, *value);
    return 0;
  }
  scd_ga_ignored(m, 0xA12000U + index, *value); /* COMSTAT, CDC mirrors, stopwatch, unused: no effect from main */
  return 0;
}

/* ---- sub side ------------------------------------------------------------------------------------------------------------- */

/* Offsets of devices the profile does not model: stopwatch $0C, font $4C-$57, graphics $58-$67, subcode $100-$1FF. */
static int scd_ga_sub_unmodelled(uint32_t offset) {
  return offset == 0x0CU || offset == 0x0DU || (offset >= 0x4CU && offset < 0x68U) || offset >= 0x100U;
}

static int scd_ga_sub_read_byte(ScdMachine *m, uint32_t offset, uint32_t *value, uint32_t pc) {
  switch (offset) {
  case 0x00:
  case 0x06: *value = m->ga.led; return 0;
  case 0x01: *value = 1U; return 0;
  case 0x02: *value = m->ga.write_protect; return 0;
  case 0x03: *value = m->ga.mode & 0x1FU; return 0; /* BK1/BK0 masked on the sub side */
  case 0x08:
  case 0x09: *value = 0xFFU; return 0;
  case 0x0E: *value = m->ga.main_flag; return 0;
  case 0x0F: *value = m->ga.sub_flag; return 0;
  case 0x32: *value = m->ga.reg32_hi; return 0;
  case 0x33: *value = m->ga.interrupt_enable; return 0;
  case 0x34:
  case 0x35: *value = 0U; return 0; /* the fader register is write-only */
  default: break;
  }
  if (offset >= 0x10U && offset < 0x20U) {
    *value = scd_byte_of(m->ga.comcmd[(offset - 0x10U) >> 1], offset);
    return 0;
  }
  if (offset >= 0x20U && offset < 0x30U) {
    *value = scd_byte_of(m->ga.comstat[(offset - 0x20U) >> 1], offset);
    return 0;
  }
  if (offset >= 0x68U && offset < 0x100U) {
    *value = m->ga.scratch[offset - 0x68U];
    return 0;
  }
  (void)pc;
  return -1; /* not a plain byte register */
}

static int scd_ga_sub_write_byte_plain(ScdMachine *m, uint32_t offset, uint32_t value) {
  switch (offset) {
  case 0x00:
  case 0x06: m->ga.led = (uint8_t)value; return 0;
  case 0x0E:
  case 0x0F: m->ga.sub_flag = (uint8_t)value; return 0;
  case 0x32: m->ga.reg32_hi = (uint8_t)value; return 0;
  case 0x33: scd_ga_interrupt_enable(m, value); return 0;
  case 0x34:
  case 0x35: return 0; /* fader: acts on CD-DA output only, which is not modelled */
  default: break;
  }
  if (offset >= 0x10U && offset < 0x20U) {
    scd_ga_ignored(m, 0xFF8000U + offset, value); /* COMCMD is read-only for the sub */
    return 0;
  }
  if (offset >= 0x20U && offset < 0x30U) {
    scd_word_put(&m->ga.comstat[(offset - 0x20U) >> 1], offset, 1U, value);
    return 0;
  }
  if (offset >= 0x68U && offset < 0x100U) {
    m->ga.scratch[offset - 0x68U] = (uint8_t)value;
    return 0;
  }
  return -1;
}

int scd_cdc_sub_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc);

static int scd_ga_sub_access_inner(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  int plain;
  if (scd_ga_sub_unmodelled(offset)) return scd_ga_unsupported(m, SCD_SUB, 0xFF8000U, offset, width, is_write, pc);
  if (offset == 0x06U && width == 1U) { /* $FF8006 byte: the LED register as GPGX keeps it ($FF8007 is the CDC data port) */
    if (is_write) m->ga.led = (uint8_t)*value;
    else *value = m->ga.led;
    return 0;
  }
  if (offset >= 0x04U && offset < 0x0CU) return scd_cdc_sub_access(m, offset, width, is_write, value, pc);
  if (offset == 0x30U || offset == 0x31U) return scd_timer_access(m, offset, width, is_write, value, pc);
  if (offset >= 0x36U && offset < 0x4CU) return scd_cdd_register_access(m, offset, width, is_write, value, pc);
  if (!is_write) {
    if (width == 1U) {
      plain = scd_ga_sub_read_byte(m, offset, value, pc);
    } else {
      uint32_t high = 0U, low = 0U;
      plain = scd_ga_sub_read_byte(m, offset, &high, pc);
      if (plain == 0) plain = scd_ga_sub_read_byte(m, offset + 1U, &low, pc);
      *value = (high << 8) | low;
    }
    if (plain == 0) return 0;
    return scd_ga_unsupported(m, SCD_SUB, 0xFF8000U, offset, width, 0, pc);
  }
  switch (offset) {
  case 0x00:
  case 0x01:
    if (width == 2U) m->ga.led = (uint8_t)(*value >> 8);
    else if (offset == 0x00U) {
      m->ga.led = (uint8_t)*value;
      return 0;
    }
    if (!(*value & 1U)) scd_peripheral_reset(m); /* RES0 = 0: peripheral reset (GPGX scd_reset(0)) */
    return 0;
  case 0x02:
  case 0x03:
    if (width == 2U) { /* the memory-mode word write has no effect (GPGX scd_write_word case 0x02) */
      scd_ga_ignored(m, 0xFF8002U, *value);
      return 0;
    }
    return scd_word_ram_sub_mode_write(m, *value & 0xFFU, pc); /* !LDS/!UDS ignored: either byte */
  case 0x0E:
    if (width == 2U) { /* only the sub flag (low byte) */
      m->ga.sub_flag = (uint8_t)*value;
      return 0;
    }
    break;
  default: break;
  }
  if (width == 1U) {
    plain = scd_ga_sub_write_byte_plain(m, offset, *value);
  } else {
    plain = 0;
    if (offset == 0x32U) scd_ga_interrupt_enable(m, *value & 0xFFU); /* LSB only */
    else if (offset >= 0x20U && offset < 0x30U) m->ga.comstat[(offset - 0x20U) >> 1] = (uint16_t)*value;
    else if (offset >= 0x10U && offset < 0x20U) scd_ga_ignored(m, 0xFF8000U + offset, *value);
    else if (offset == 0x34U) plain = 0; /* fader */
    else if (offset >= 0x68U && offset < 0x100U) {
      m->ga.scratch[offset - 0x68U] = (uint8_t)(*value >> 8);
      m->ga.scratch[offset + 1U - 0x68U] = (uint8_t)*value;
    } else plain = -1;
  }
  if (plain == 0) return 0;
  return scd_ga_unsupported(m, SCD_SUB, 0xFF8000U, offset, width, 1, pc);
}

int scd_ga_sub_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  const int was_active = scd_cdd_active(m);
  int result;
  scd_log(m, is_write ? 0x31U : 0x30U, offset, is_write ? *value : 0U);
  result = scd_ga_sub_access_inner(m, offset, width, is_write, value, pc);
  scd_cdd_refresh(m, was_active);
  return result;
}
