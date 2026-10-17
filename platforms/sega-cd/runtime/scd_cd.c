/* SEG-062-T005: the CD drive (CDD) command/status port, the LC8951 CDC subset, CDC DMA into Word RAM, the peripheral reset and the
 * aggregation of timed device events (contract sections 7, 8). Behaviour follows Genesis Plus GX (core/cd_hw/cdc.c, cdd.c, scd.c;
 * consulted only, default configuration: "CD access latency" enabled) wherever the references disagree.
 *
 * Time is the integer tick domain (contract section 2). Things GPGX evaluates at its emulation-slice boundaries are evaluated here at
 * their exact tick: the 75 Hz drive clock (anchored at power-on and re-anchored by a peripheral reset), DMA progress (counted from
 * the transfer's base tick, 4 sub cycles per byte, rounded up to whole bytes and down to 8-byte groups) and DMA completion. Every
 * observation point (a Gate Array CDC/DMA register access, a Word RAM access, an ownership change) first brings an in-flight DMA up
 * to the current instruction's start tick, so a run's result never depends on how it is sliced. */
#include "scd_runtime.h"

#include <string.h>

#define SCD_IFSTAT_DTEI 0x40U
#define SCD_IFSTAT_DECI 0x20U
#define SCD_IFSTAT_DTBSY 0x08U
#define SCD_IFSTAT_DTEN 0x02U
#define SCD_IFCTRL_DTEIEN 0x40U
#define SCD_IFCTRL_DECIEN 0x20U
#define SCD_IFCTRL_DOUTEN 0x02U
#define SCD_CTRL0_DECEN 0x80U
#define SCD_CTRL0_AUTORQ 0x10U
#define SCD_CTRL0_WRRQ 0x04U
#define SCD_CTRL1_MODRQ 0x08U
#define SCD_CTRL1_FORMRQ 0x04U
#define SCD_CTRL1_SHDREN 0x01U
#define SCD_CDD_STOPPED 0x00U
#define SCD_CDD_PAUSE 0x04U
/* GPGX: decoder active period 267000 SCD clocks after a block is decoded (MCDV "CDC FLAGS" #40/#41) = 66750 sub cycles. */
#define SCD_DECODER_ACTIVE_SUB_CYCLES UINT64_C(66750)
/* GPGX: CD access latency (default on) - 2 + 10 ticks, plus a seek of 120 ticks per 270000 sectors of distance. */
#define SCD_CDD_BASE_LATENCY 12U

static uint64_t scd_dma_byte_ticks(void) { return SCD_DMA_SUB_CYCLES_PER_BYTE * SCD_SUB_CYCLE_TICKS; }

static void scd_cdc_level5(ScdMachine *m) {
  if (m->ga.interrupt_enable & 0x20U) scd_raise_sub_level(m, 5U); /* only with IEN5 (GPGX) */
}

/* ---- DMA ------------------------------------------------------------------------------------------------------------- */

static uint32_t scd_bank_offset(uint32_t bank, uint32_t offset) { return (offset >> 1) * 4U + bank * 2U + (offset & 1U); }

/* Move `length` bytes (whole words; an odd last byte is not transferred) from the CDC buffer to the latched destination. A
 * destination of the other Word RAM mode than the current one is not the live memory (GPGX keeps separate 2M and bank buffers and
 * copies only on a mode switch): those writes are lost. */
static void scd_cdc_dma_move(ScdMachine *m, uint32_t length) {
  const uint32_t words = length >> 1;
  const int to_2m = m->cdc.dma_target == SCD_DMA_TARGET_2M;
  const uint32_t mask = to_2m ? 0x3FFFEU : 0x1FFFEU;
  const int live = to_2m == !(m->ga.mode & SCD_MODE_1M);
  uint32_t src = m->cdc.dac & 0x3FFEU, dst = ((uint32_t)m->cdc.dma_address << 3) & mask, i;
  m->cdc.dma_address = (uint16_t)(m->cdc.dma_address + (words >> 2));
  m->cdc.dac = (uint16_t)(m->cdc.dac + (words << 1));
  for (i = 0; i < words; ++i) {
    if (live) {
      const uint32_t at = to_2m ? dst : scd_bank_offset(m->cdc.dma_target == SCD_DMA_TARGET_BANK0 ? 0U : 1U, dst);
      m->word_ram[at] = m->cdc.buffer[src];
      m->word_ram[at + 1U] = m->cdc.buffer[src + 1U];
      SCD_OVERLAY_NOTE_WRITE(m, SCD_MEMORY_WORD, at, 2U);
    }
    src = (src + 2U) & 0x3FFEU;
    dst = (dst + 2U) & mask;
  }
  if (!live && words != 0U) scd_log(m, 0x94U, words, m->cdc.dma_target);
  m->cdc.dma_done += words << 1;
}

static void scd_cdc_dma_finish(ScdMachine *m) {
  m->cdc.dma_state = SCD_DMA_NONE;
  m->cdc.dbc = 0xFFFFU;
  m->cdc.ifstat = (uint8_t)((m->cdc.ifstat | SCD_IFSTAT_DTBSY | SCD_IFSTAT_DTEN) & ~SCD_IFSTAT_DTEI);
  if (m->cdc.ifctrl & SCD_IFCTRL_DTEIEN) {
    if ((m->cdc.irq & SCD_IFSTAT_DECI) && m->cdc.dma_base > m->cdc.decoder_end) { /* the decoder period is over */
      m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DECI);
      m->cdc.irq = (uint8_t)(m->cdc.irq & ~SCD_IFSTAT_DECI);
    }
    if (!m->cdc.irq) scd_cdc_level5(m); /* /INT falling edge */
    m->cdc.irq = (uint8_t)(m->cdc.irq | SCD_IFSTAT_DTEI);
  }
  m->cdc.edt = 1U;
  m->cdc.dsr = 0U;
  scd_log(m, 0x92U, m->cdc.dma_done, m->cdc.dac);
}

/* Bring a running DMA up to tick `now` (GPGX cdc_dma_update). */
static void scd_cdc_dma_sync(ScdMachine *m, uint64_t now) {
  uint64_t elapsed, groups;
  const uint64_t bt = scd_dma_byte_ticks();
  if (m->cdc.dma_state != SCD_DMA_RUNNING || now <= m->cdc.dma_base) return;
  elapsed = (now - m->cdc.dma_base + bt - 1U) / bt;
  groups = elapsed & ~(uint64_t)7U;
  if ((uint64_t)m->cdc.dbc < groups) {
    const uint32_t remaining = (uint32_t)m->cdc.dbc + 1U;
    scd_cdc_dma_move(m, remaining);
    m->cdc.dma_base += (uint64_t)remaining * bt;
    scd_cdc_dma_finish(m);
  } else if (groups > 0U) {
    scd_cdc_dma_move(m, (uint32_t)groups);
    m->cdc.dbc = (uint16_t)(m->cdc.dbc - groups);
    m->cdc.dma_base += groups * bt;
  }
}

static uint64_t scd_cdc_dma_completion(const ScdMachine *m) {
  const uint64_t rounded = ((uint64_t)m->cdc.dbc + 1U + 7U) & ~(uint64_t)7U;
  return m->cdc.dma_base + (rounded - 1U) * scd_dma_byte_ticks() + 1U;
}

int scd_cd_observe(ScdMachine *m) {
  scd_cdc_dma_sync(m, m->now);
  return m->stop.stop_class != SCD_STOP_NONE;
}

/* Select the writer of a started transfer (GPGX cdc_dma_init): no effect unless a transfer is started (DTEN asserted). */
static int scd_cdc_dma_init(ScdMachine *m, uint32_t pc) {
  if (m->cdc.ifstat & SCD_IFSTAT_DTEN) return 0;
  m->cdc.dma_state = SCD_DMA_NONE;
  switch (m->cdc.destination) {
  case 2:
  case 3: /* main / sub host reads through $A12008 / $FF8008 */
  case 4: /* PCM RAM */
  case 5: /* PRG RAM */
    return scd_fail(m, SCD_SUB, SCD_STOP_CDC_DESTINATION_UNSUPPORTED, pc, m->cdc.destination);
  case 7:
    if (m->ga.mode & SCD_MODE_1M) {
      m->cdc.dma_target = (m->ga.mode & SCD_MODE_RET) ? SCD_DMA_TARGET_BANK0 : SCD_DMA_TARGET_BANK1; /* the sub's bank */
      m->cdc.dma_state = SCD_DMA_RUNNING;
    } else {
      m->cdc.dma_target = SCD_DMA_TARGET_2M;
      m->cdc.dma_state = (m->ga.mode & SCD_MODE_RET) ? SCD_DMA_HALTED : SCD_DMA_RUNNING; /* halted while main owns Word RAM */
    }
    break;
  default: break; /* invalid destination: the started transfer has no writer */
  }
  scd_log(m, 0x95U, m->cdc.destination, m->cdc.dma_state);
  return 0;
}

void scd_cdc_dma_halt_2m(ScdMachine *m) {
  if (m->cdc.dma_state != SCD_DMA_RUNNING || m->cdc.dma_target != SCD_DMA_TARGET_2M) return;
  scd_cdc_dma_sync(m, m->now);
  if (m->cdc.dma_state == SCD_DMA_RUNNING) {
    m->cdc.dma_state = SCD_DMA_HALTED;
    scd_log(m, 0x96U, m->cdc.dma_done, 0U);
  }
}

void scd_cdc_dma_resume_2m(ScdMachine *m) {
  if (m->cdc.dma_state != SCD_DMA_HALTED || m->cdc.dma_target != SCD_DMA_TARGET_2M) return;
  m->cdc.dma_state = SCD_DMA_RUNNING;
  if (m->cdc.dma_base < m->now) m->cdc.dma_base = m->now;
  scd_log(m, 0x97U, m->cdc.dma_done, 0U);
}

/* ---- CDC registers ----------------------------------------------------------------------------------------------------- */

static void scd_cdc_reset(ScdMachine *m) {
  static const uint8_t head0[4] = {0U, 0U, 0U, 1U};
  m->cdc.address = 0U;
  m->cdc.ifstat = 0xFFU;
  m->cdc.ifctrl = 0U;
  m->cdc.ctrl0 = 0U;
  m->cdc.ctrl1 = 0U;
  m->cdc.stat0 = 0U;
  m->cdc.stat2 = 0U;
  m->cdc.stat3 = 0x80U;
  memcpy(m->cdc.head[0], head0, 4U);
  memset(m->cdc.head[1], 0, 4U);
  m->cdc.dma_base = 0U;
  m->cdc.decoder_end = 0U;
  m->cdc.dma_state = SCD_DMA_NONE;
  m->cdc.irq = 0U;
  m->ga.pending = (uint8_t)(m->ga.pending & ~0x20U);
}

static void scd_cdc_decoder_period_check(ScdMachine *m) {
  if (m->now > m->cdc.decoder_end) {
    m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DECI);
    m->cdc.irq = (uint8_t)(m->cdc.irq & ~SCD_IFSTAT_DECI);
  }
}

static void scd_cdc_stat2(ScdMachine *m) {
  if (m->cdc.ctrl0 & SCD_CTRL0_AUTORQ)
    m->cdc.stat2 = (uint8_t)((m->cdc.ctrl1 & SCD_CTRL1_MODRQ) | ((m->cdc.head[1][2] & 0x20U) >> 3));
  else
    m->cdc.stat2 = (uint8_t)(m->cdc.ctrl1 & (SCD_CTRL1_MODRQ | SCD_CTRL1_FORMRQ));
}

static int scd_cdc_register_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  const uint32_t reg = m->cdc.address;
  value &= 0xFFU;
  switch (reg) {
  case 0x01: { /* IFCTRL */
    uint8_t previous = m->cdc.irq;
    if (m->now > m->cdc.decoder_end) {
      m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DECI);
      previous = (uint8_t)(previous & ~SCD_IFSTAT_DECI);
    }
    m->cdc.irq = (uint8_t)(~(uint32_t)m->cdc.ifstat & value & (SCD_IFCTRL_DTEIEN | SCD_IFCTRL_DECIEN));
    if (m->cdc.irq && !previous) scd_cdc_level5(m);
    if (!(value & SCD_IFCTRL_DOUTEN)) { /* data output disabled: any transfer is aborted; bytes already moved stay (V1) */
      if (m->cdc.dma_state != SCD_DMA_NONE || !(m->cdc.ifstat & SCD_IFSTAT_DTEN)) scd_log(m, 0x93U, m->cdc.dma_done, m->cdc.dbc);
      m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DTBSY | SCD_IFSTAT_DTEN);
      m->cdc.dma_state = SCD_DMA_NONE;
    }
    m->cdc.ifctrl = (uint8_t)value;
    break;
  }
  case 0x02: m->cdc.dbc = (uint16_t)((m->cdc.dbc & 0xFF00U) | value); break;
  case 0x03: m->cdc.dbc = (uint16_t)((m->cdc.dbc & 0x00FFU) | ((value & 0x0FU) << 8)); break;
  case 0x04: m->cdc.dac = (uint16_t)((m->cdc.dac & 0xFF00U) | value); break;
  case 0x05: m->cdc.dac = (uint16_t)((m->cdc.dac & 0x00FFU) | (value << 8)); break;
  case 0x06: /* DTRG: start a transfer if data output is enabled */
    if (m->cdc.ifctrl & SCD_IFCTRL_DOUTEN) {
      m->cdc.ifstat = (uint8_t)(m->cdc.ifstat & ~(SCD_IFSTAT_DTBSY | SCD_IFSTAT_DTEN));
      m->cdc.edt = 0U;
      m->cdc.dsr = 0U;
      m->cdc.dma_total = (uint32_t)m->cdc.dbc + 1U;
      m->cdc.dma_done = 0U;
      if (scd_cdc_dma_init(m, pc) != 0) return 1;
      m->cdc.dma_base = m->now;
      scd_log(m, 0x91U, m->cdc.dma_total, m->cdc.dac);
    }
    break;
  case 0x07: /* DTACK */
    m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DTEI);
    m->cdc.irq = (uint8_t)(m->cdc.irq & ~SCD_IFSTAT_DTEI);
    break;
  case 0x08: m->cdc.wa = (uint16_t)((m->cdc.wa & 0xFF00U) | value); break;
  case 0x09: m->cdc.wa = (uint16_t)((m->cdc.wa & 0x00FFU) | (value << 8)); break;
  case 0x0A: /* CTRL0 */
    m->cdc.stat0 = (uint8_t)(value & SCD_CTRL0_DECEN);
    if (!(value & SCD_CTRL0_DECEN)) {
      m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DECI);
      m->cdc.irq = (uint8_t)(m->cdc.irq & ~SCD_IFSTAT_DECI);
    }
    m->cdc.ctrl0 = (uint8_t)value;
    scd_cdc_stat2(m);
    break;
  case 0x0B: /* CTRL1 */
    m->cdc.ctrl1 = (uint8_t)value;
    scd_cdc_stat2(m);
    break;
  case 0x0C: m->cdc.pt = (uint16_t)((m->cdc.pt & 0xFF00U) | value); break;
  case 0x0D: m->cdc.pt = (uint16_t)((m->cdc.pt & 0x00FFU) | (value << 8)); break;
  case 0x0F: scd_cdc_reset(m); break; /* the register address is 0 afterwards: no increment */
  default: scd_log(m, 0x98U, reg, value); break; /* registers 0 and 14 are not emulated: no effect (GPGX) */
  }
  if (m->cdc.address != 0U) m->cdc.address = (uint8_t)((m->cdc.address + 1U) & 0x0FU);
  return 0;
}

static uint32_t scd_cdc_register_read(ScdMachine *m) {
  const uint32_t reg = m->cdc.address;
  const uint32_t shdren = m->cdc.ctrl1 & SCD_CTRL1_SHDREN;
  uint32_t value;
  switch (reg) {
  case 0x01:
    scd_cdc_decoder_period_check(m);
    value = m->cdc.ifstat;
    break;
  case 0x02: value = m->cdc.dbc & 0xFFU; break;
  case 0x03: value = (uint32_t)(m->cdc.dbc >> 8) & 0xFFU; break;
  case 0x04:
  case 0x05:
  case 0x06:
  case 0x07: value = m->cdc.head[shdren][reg - 4U]; break;
  case 0x08: value = m->cdc.pt & 0xFFU; break;
  case 0x09: value = (uint32_t)m->cdc.pt >> 8; break;
  case 0x0A: value = m->cdc.wa & 0xFFU; break;
  case 0x0B: value = (uint32_t)m->cdc.wa >> 8; break;
  case 0x0C: value = m->cdc.stat0; break;
  case 0x0D: value = 0U; break; /* STAT1 */
  case 0x0E: value = m->cdc.stat2; break;
  case 0x0F: /* STAT3: reading sets VALST back and clears the decoder interrupt (V1) */
    value = m->cdc.stat3;
    m->cdc.stat3 = 0x80U;
    m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DECI);
    m->cdc.irq = (uint8_t)(m->cdc.irq & ~SCD_IFSTAT_DECI);
    break;
  default: value = 0xFFU; break; /* register 0 (COMIN) is not emulated */
  }
  if (m->cdc.address != 0U) m->cdc.address = (uint8_t)((m->cdc.address + 1U) & 0x0FU);
  return value;
}

int scd_cdc_status_read(ScdMachine *m, uint32_t *status) {
  scd_cdc_dma_sync(m, m->now);
  *status = (uint32_t)((m->cdc.edt << 7) | (m->cdc.dsr << 6) | m->cdc.destination);
  return 0;
}

int scd_cdc_dma_address_access(ScdMachine *m, int is_write, uint32_t *value) {
  scd_cdc_dma_sync(m, m->now);
  if (is_write) m->cdc.dma_address = (uint16_t)*value;
  else *value = m->cdc.dma_address;
  return 0;
}

/* $FF8004-$FF800B except the $FF8006 byte (the Gate Array keeps the LED there). */
static int scd_cdc_mode_write(ScdMachine *m, uint32_t destination, uint32_t pc) {
  m->cdc.destination = (uint8_t)(destination & 7U);
  m->cdc.edt = 0U;
  m->cdc.dsr = 0U;
  scd_cdc_dma_sync(m, m->now); /* with the previous writer and DMA address */
  if (scd_cdc_dma_init(m, pc) != 0) return 1;
  m->cdc.dma_address = 0U; /* reset by any mode write (V1, GPGX citing MCDV) */
  return 0;
}

int scd_cdc_sub_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  scd_cdc_dma_sync(m, m->now); /* observation point: an abort keeps every byte moved up to this instruction */
  switch (offset) {
  case 0x04:
  case 0x05:
    if (!is_write) {
      uint32_t status;
      scd_cdc_status_read(m, &status);
      *value = width == 2U ? ((status << 8) | m->cdc.address) : (offset == 0x04U ? status : m->cdc.address);
      return 0;
    }
    if (width == 2U) {
      m->cdc.address = (uint8_t)(*value & 0x0FU);
      return scd_cdc_mode_write(m, *value >> 8, pc);
    }
    if (offset == 0x04U) return scd_cdc_mode_write(m, *value, pc);
    m->cdc.address = (uint8_t)(*value & 0x0FU);
    return 0;
  case 0x06:
  case 0x07:
    if (is_write) return scd_cdc_register_write(m, *value, pc);
    *value = scd_cdc_register_read(m);
    return 0;
  case 0x08:
  case 0x09:
    if (!is_write && width == 1U) {
      *value = 0xFFU; /* the host-data register keeps its reset value */
      return 0;
    }
    return scd_fail(m, SCD_SUB, SCD_STOP_CDC_DESTINATION_UNSUPPORTED, pc, 0xFF8008U); /* host data path */
  case 0x0A:
  case 0x0B:
    if (is_write) {
      uint32_t word = width == 2U ? *value : (((*value & 0xFFU) << 8) | (*value & 0xFFU)); /* word-only register */
      return scd_cdc_dma_address_access(m, 1, &word);
    }
    {
      uint32_t word;
      scd_cdc_dma_address_access(m, 0, &word);
      *value = width == 2U ? word : (offset == 0x0AU ? word >> 8 : word & 0xFFU);
    }
    return 0;
  default: break;
  }
  return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset);
}

/* ---- CD decoder ---------------------------------------------------------------------------------------------------------- */

/* The user data of `lba` from the first (data) track as GPGX cdd_read_data reads it: 2048 bytes, only for lba >= 0 inside a data
 * track. A non-data block (header 0) reads the same bytes; GPGX's raw-image Mode 2 path (sub-header and 2328 bytes) differs from
 * its cooked path there, and this profile takes the cooked behaviour for both so that the image format never changes machine
 * state (contract section 7). Returns 1 on a stop. */
static int scd_cdd_read_data(ScdMachine *m, uint8_t *destination) {
  const uint32_t index = m->cdd.index;
  if (index >= m->disc->track_count || m->disc->tracks[index].type == 2U || m->cdd.lba < 0) return 0;
  if (scd_disc_read_user_data(m->disc, (uint32_t)m->cdd.lba, destination) != SCD_DISC_OK)
    return scd_fail(m, SCD_SUB, SCD_STOP_CD_READ_FAILED, 0U, (uint32_t)m->cdd.lba);
  return 0;
}

/* One decoded block (GPGX cdc_decoder_update). Returns 1 on a stop. */
static int scd_cdc_decode(ScdMachine *m, const uint8_t header[4]) {
  uint32_t offset;
  if (!(m->cdc.ctrl0 & SCD_CTRL0_DECEN)) return 0;
  memcpy(m->cdc.head[0], header, 4U);
  m->cdc.stat3 = 0x00U;
  m->cdc.ifstat = (uint8_t)(m->cdc.ifstat & ~SCD_IFSTAT_DECI);
  m->cdc.decoder_end = m->now + SCD_DECODER_ACTIVE_SUB_CYCLES * SCD_SUB_CYCLE_TICKS;
  if (m->cdc.ifctrl & SCD_IFCTRL_DECIEN) {
    if (!(m->cdc.irq & SCD_IFSTAT_DTEI)) scd_cdc_level5(m);
    m->cdc.irq = (uint8_t)(m->cdc.irq | SCD_IFSTAT_DECI);
  }
  if (!(m->cdc.ctrl0 & SCD_CTRL0_WRRQ)) return 0;
  m->cdc.pt = (uint16_t)(m->cdc.pt + 2352U);
  m->cdc.wa = (uint16_t)(m->cdc.wa + 2352U);
  offset = m->cdc.pt & 0x3FFFU;
  memcpy(m->cdc.buffer + offset, header, 4U);
  offset += 4U;
  if (m->cdc.head[0][3] == 0x01U) { /* Mode 1 */
    if (scd_cdd_read_data(m, m->cdc.buffer + offset) != 0) return 1;
    offset += 2048U;
  } else {
    if (m->cdc.ctrl1 & SCD_CTRL1_MODRQ) {
      if (scd_cdd_read_data(m, m->cdc.buffer + offset + 8U) != 0) return 1;
      memcpy(m->cdc.buffer + offset, m->cdc.head[1], 4U);
      memcpy(m->cdc.buffer + offset + 4U, m->cdc.head[1], 4U);
      offset += 2336U;
    } else {
      if (scd_cdd_read_data(m, m->cdc.buffer + offset) != 0) return 1;
      offset += 2328U;
    }
    if (m->cdc.ctrl0 & SCD_CTRL0_AUTORQ)
      m->cdc.stat2 = (uint8_t)((m->cdc.ctrl1 & SCD_CTRL1_MODRQ) | ((m->cdc.head[1][2] & 0x20U) >> 3));
  }
  if (offset > SCD_CDC_BUFFER_BYTES) memcpy(m->cdc.buffer, m->cdc.buffer + SCD_CDC_BUFFER_BYTES, offset - SCD_CDC_BUFFER_BYTES);
  return 0;
}

/* ---- CDD ---------------------------------------------------------------------------------------------------------------- */

static uint8_t scd_bcd(uint32_t v) { return (uint8_t)(((v / 10U) << 4) | (v % 10U)); }

static uint32_t scd_track_end(const ScdMachine *m, uint32_t index) {
  return m->disc->tracks[index].first_lba + m->disc->tracks[index].sector_count;
}

int scd_cdd_active(const ScdMachine *m) {
  return (m->ga.cdd_ctrl_lo & 0x04U) || m->cdd.pending || m->cdd.latency > 0U || m->cdd.status == SCD_CDD_PLAY ||
         (m->cdc.ctrl0 & SCD_CTRL0_DECEN);
}

/* While the drive has nothing to do its 75 Hz ticks are no-ops and are not scheduled; when it becomes active again the next tick is
 * the first grid point after the current instruction. */
void scd_cdd_refresh(ScdMachine *m, int was_active) {
  if (!was_active && scd_cdd_active(m))
    m->cdd.next_tick = m->cdd.anchor + ((m->now - m->cdd.anchor) / SCD_CDD_TICK_TICKS + 1U) * SCD_CDD_TICK_TICKS;
}

static void scd_cdd_reset(ScdMachine *m) {
  m->cdd.latency = 0U;
  m->cdd.index = 0U;
  m->cdd.lba = 0;
  m->cdd.status = SCD_CDD_TOC_STATUS;
  m->cdd.pending = 0U;
}

static void scd_cdd_register_defaults(ScdMachine *m) {
  memset(m->cdd.rx, 0, sizeof m->cdd.rx);
  m->cdd.rx[9] = 0x0FU;
  memset(m->cdd.tx, 0xFF, sizeof m->cdd.tx);
  m->ga.cdd_ctrl_hi = 0x01U;
  m->ga.cdd_ctrl_lo = 0x00U;
  m->cdc.dma_address = 0xFFFFU;
}

void scd_devices_power_on(ScdMachine *m) {
  scd_cdd_register_defaults(m);
  scd_cdd_reset(m);
  scd_cdc_reset(m);
  m->cdd.anchor = 0U;
  m->cdd.next_tick = SCD_CDD_TICK_TICKS;
}

/* RES0 = 0 from the sub (GPGX scd_reset(0)): the sub-side registers from $FF8030 up and $FF8004 clear and take their defaults,
 * the timer stops, pending interrupts clear, the drive and the CDC reset and the 75 Hz clock restarts from now. Communication
 * registers, the memory mode, the LED, the flags and the main-side IFL2/IEN2 view are kept. */
void scd_peripheral_reset(ScdMachine *m) {
  m->cdc.destination = 0U;
  m->cdc.edt = 0U;
  m->cdc.dsr = 0U;
  m->ga.timer_reload = 0U;
  m->ga.timer_next = 0U;
  m->ga.reg32_hi = 0U;
  m->ga.interrupt_enable = 0U;
  memset(m->ga.scratch, 0, sizeof m->ga.scratch);
  scd_cdd_register_defaults(m);
  m->ga.pending = 0U;
  m->cdd.anchor = m->now;
  m->cdd.next_tick = m->now + SCD_CDD_TICK_TICKS;
  scd_cdd_reset(m);
  scd_cdc_reset(m);
  scd_log(m, 0x13U, 0U, 0U);
}

static void scd_cdd_checksum(ScdMachine *m) {
  uint32_t i, sum = 0U;
  for (i = 0; i < 9U; ++i) sum += m->cdd.rx[i];
  m->cdd.rx[9] = (uint8_t)(~sum & 0x0FU);
}

/* RS2..RS7 as six BCD digits of a block count; returns 1 when the minutes do not fit two digits (GPGX's table would overflow). */
static int scd_cdd_put_time(ScdMachine *m, uint32_t blocks) {
  const uint32_t mm = blocks / 75U / 60U, ss = blocks / 75U % 60U, ff = blocks % 75U;
  if (mm > 99U) return 1;
  m->cdd.rx[2] = (uint8_t)(mm / 10U);
  m->cdd.rx[3] = (uint8_t)(mm % 10U);
  m->cdd.rx[4] = (uint8_t)(ss / 10U);
  m->cdd.rx[5] = (uint8_t)(ss % 10U);
  m->cdd.rx[6] = (uint8_t)(ff / 10U);
  m->cdd.rx[7] = (uint8_t)(ff % 10U);
  return 0;
}

static uint8_t scd_cdd_track_flags(const ScdMachine *m) {
  return (m->cdd.index < m->disc->track_count && m->disc->tracks[m->cdd.index].type != 2U) ? 0x04U : 0x00U;
}

/* The word write to $FF804A (GPGX cdd_process): TX8/TX9 are cleared first - the command checksum is never inspected. */
static int scd_cdd_command(ScdMachine *m, uint32_t pc) {
  const uint32_t command = m->cdd.tx[0] & 0x0FU;
  m->cdd.tx[8] = 0U;
  m->cdd.tx[9] = 0U;
  scd_log(m, 0xA0U, command, (uint32_t)m->cdd.lba);
  switch (command) {
  case 0x0: /* drive status */
    if (m->cdd.latency <= 3U) {
      m->cdd.rx[0] = m->cdd.status;
      if (m->cdd.status == SCD_CDD_STOPPED || m->cdd.status > SCD_CDD_PAUSE) break;
      if (m->cdd.rx[1] == 0x0FU || m->cdd.rx[1] == 0x00U) { /* absolute time (RS1 = $F: seeking has ended) */
        m->cdd.rx[1] = 0x00U;
        if (scd_cdd_put_time(m, (uint32_t)(m->cdd.lba + 150))) return scd_fail(m, SCD_SUB, SCD_STOP_CDD_COMMAND_UNSUPPORTED, pc, 0x100U);
        m->cdd.rx[8] = scd_cdd_track_flags(m);
      } else if (m->cdd.rx[1] == 0x01U) { /* track-relative time */
        const int64_t start = m->cdd.index < m->disc->track_count ? (int64_t)m->disc->tracks[m->cdd.index].first_lba : 0;
        const int64_t distance = (int64_t)m->cdd.lba - start;
        if (scd_cdd_put_time(m, (uint32_t)(distance < 0 ? -distance : distance)))
          return scd_fail(m, SCD_SUB, SCD_STOP_CDD_COMMAND_UNSUPPORTED, pc, 0x101U);
        m->cdd.rx[8] = scd_cdd_track_flags(m);
      } else if (m->cdd.rx[1] == 0x02U) { /* current track number */
        const uint32_t number = m->cdd.index + 1U;
        m->cdd.rx[2] = (uint8_t)(m->cdd.index < m->disc->track_count ? number / 10U : 0x0AU);
        m->cdd.rx[3] = (uint8_t)(m->cdd.index < m->disc->track_count ? number % 10U : 0x0AU);
      }
    }
    break;
  case 0x1: /* stop */
    m->cdd.status = SCD_CDD_TOC_STATUS;
    m->ga.cdd_ctrl_hi = 0x01U;
    memset(m->cdd.rx, 0, sizeof m->cdd.rx);
    m->cdd.rx[1] = 0x0FU;
    m->cdd.rx[9] = 0x00U; /* ~(0 + $F) & $F */
    m->cdd.index = 0U;
    m->cdd.lba = 0;
    return 0;
  case 0x3: /* play: seeking is reported until the drive is ready; the play starts on the next tick */
    memset(m->cdd.rx, 0, sizeof m->cdd.rx);
    m->cdd.rx[0] = SCD_CDD_SEEK;
    m->cdd.rx[1] = 0x0FU;
    m->cdd.rx[9] = (uint8_t)(~(SCD_CDD_SEEK + 0x0FU) & 0x0FU);
    m->cdd.pending = SCD_CDD_PLAY;
    return 0;
  case 0x5:
  case 0xB:
  case 0xE:
  case 0xF: /* commands the drive does not know: only RS0 is refreshed */
    m->cdd.rx[0] = m->cdd.status;
    break;
  default: /* TOC, seek, pause, resume, scan, tray: real commands this profile does not model */
    return scd_fail(m, SCD_SUB, SCD_STOP_CDD_COMMAND_UNSUPPORTED, pc, command);
  }
  scd_cdd_checksum(m);
  return 0;
}

int scd_cdd_register_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  uint8_t *reg;
  if (offset == 0x36U || offset == 0x37U) {
    if (!is_write) {
      *value = width == 2U ? (uint32_t)((m->ga.cdd_ctrl_hi << 8) | m->ga.cdd_ctrl_lo)
                           : (offset == 0x36U ? m->ga.cdd_ctrl_hi : m->ga.cdd_ctrl_lo);
      return 0;
    }
    if (width == 2U) m->ga.cdd_ctrl_lo = (uint8_t)(*value & 0x04U); /* word: only HOCK is writable */
    else if (offset == 0x36U) m->ga.cdd_ctrl_hi = (uint8_t)*value;
    else m->ga.cdd_ctrl_lo = (uint8_t)*value;
    return 0;
  }
  reg = offset < 0x42U ? &m->cdd.rx[offset - 0x38U] : &m->cdd.tx[offset - 0x42U]; /* RS0..RS9, TX0..TX9 */
  if (!is_write) {
    *value = width == 2U ? (uint32_t)((reg[0] << 8) | reg[1]) : reg[0];
    return 0;
  }
  if (width == 2U) {
    if (offset == 0x4AU) return scd_cdd_command(m, pc); /* the word written to TX8/TX9 is discarded */
    reg[0] = (uint8_t)(*value >> 8);
    reg[1] = (uint8_t)*value;
  } else {
    reg[0] = (uint8_t)*value;
  }
  return 0;
}

/* One 75 Hz tick (GPGX cdd_update, then the HOCK interrupt request). Returns 1 on a stop. */
static int scd_cdd_tick(ScdMachine *m) {
  static const uint8_t no_header[4] = {0U, 0U, 0U, 0U};
  if (m->cdd.latency > 0U) {
    m->cdd.latency -= 1U;
    if (scd_cdc_decode(m, no_header) != 0) return 1; /* the decoder keeps running while the drive seeks */
  } else if (m->cdd.status == SCD_CDD_PLAY) {
    if (m->cdd.index >= m->disc->track_count) {
      m->cdd.status = SCD_CDD_END; /* past the last track; a pending command waits for the next tick */
      goto hock;
    }
    if (m->disc->tracks[m->cdd.index].type == 2U) /* CD-DA playback (audio output) is not modelled */
      return scd_fail(m, SCD_SUB, SCD_STOP_CD_READ_AUDIO_TRACK, 0U, (uint32_t)m->cdd.lba);
    {
      const uint32_t msf = (uint32_t)(m->cdd.lba + 150);
      uint8_t header[4];
      header[0] = scd_bcd(msf / 75U / 60U);
      header[1] = scd_bcd(msf / 75U % 60U);
      header[2] = scd_bcd(msf % 75U);
      header[3] = 0x01U; /* Mode 1 */
      scd_log(m, 0xA1U, (uint32_t)m->cdd.lba, msf);
      if (scd_cdc_decode(m, header) != 0) return 1;
    }
    m->cdd.delivered += 1U;
    m->cdd.lba += 1;
    if (m->cdd.lba >= (int32_t)scd_track_end(m, m->cdd.index)) {
      m->cdd.index += 1U; /* on into the next track */
      m->ga.cdd_ctrl_hi = 0x01U;
    }
  } else {
    if (scd_cdc_decode(m, no_header) != 0) return 1; /* the decoder keeps running while the disc is not read */
  }
  if (m->cdd.pending) { /* a play command starts: seek to TX2..TX7 (MSF digits as written) */
    const uint8_t *t = m->cdd.tx;
    const int32_t lba = (((int32_t)t[2] * 10 + t[3]) * 60 + ((int32_t)t[4] * 10 + t[5])) * 75 + ((int32_t)t[6] * 10 + t[7]) - 150;
    const int64_t distance = (int64_t)lba - m->cdd.lba;
    if (m->cdd.latency == 0U) m->cdd.latency = SCD_CDD_BASE_LATENCY;
    m->cdd.latency += (uint32_t)(((distance < 0 ? -distance : distance) * 120) / 270000);
    m->cdd.lba = lba;
    m->cdd.index = 0U;
    while (m->cdd.index < m->disc->track_count && (int64_t)scd_track_end(m, m->cdd.index) <= (int64_t)lba) m->cdd.index += 1U;
    m->ga.cdd_ctrl_hi = 0x01U;
    m->cdd.status = m->cdd.pending;
    m->cdd.pending = 0U;
    scd_log(m, 0xA2U, (uint32_t)lba, m->cdd.latency);
  }
hock:
  if (m->ga.cdd_ctrl_lo & 0x04U) scd_raise_sub_level(m, 4U); /* pending even while IEN4 = 0 (V1, GPGX) */
  return 0;
}

/* ---- timed device aggregation ----------------------------------------------------------------------------------------- */

uint64_t scd_timer_next_event(const ScdMachine *m);
void scd_timer_event(ScdMachine *m);

uint64_t scd_devices_next_event(const ScdMachine *m) {
  uint64_t next = scd_timer_next_event(m);
  if (scd_vdp_next_event(m) < next) next = scd_vdp_next_event(m); /* SEG-064-T003: vertical-blank onset (BIOS-image profile) */
  if (scd_cdd_active(m) && m->cdd.next_tick < next) next = m->cdd.next_tick;
  if (m->cdc.dma_state == SCD_DMA_RUNNING) {
    const uint64_t done = scd_cdc_dma_completion(m);
    if (done < next) next = done;
  }
  return next;
}

/* Process every device event due at exactly `until`, in the fixed order: CDD tick, timer, CDC DMA, vertical-blank onset. */
void scd_devices_advance(ScdMachine *m, uint64_t until) {
  if (scd_cdd_active(m) && m->cdd.next_tick <= until) {
    m->cdd.next_tick += SCD_CDD_TICK_TICKS;
    if (scd_cdd_tick(m) != 0) return;
  }
  if (scd_timer_next_event(m) <= until) scd_timer_event(m);
  scd_cdc_dma_sync(m, until);
  if (scd_vdp_next_event(m) <= until) scd_vdp_event(m);
}

int scd_devices_can_raise(const ScdMachine *m, uint32_t level) {
  if (level == 3U) return scd_timer_next_event(m) != UINT64_MAX;
  if (level == 4U) return (m->ga.cdd_ctrl_lo & 0x04U) != 0U;
  if (level == 5U)
    return (m->cdc.dma_state != SCD_DMA_NONE && (m->cdc.ifctrl & SCD_IFCTRL_DTEIEN)) ||
           ((m->cdc.ctrl0 & SCD_CTRL0_DECEN) && (m->cdc.ifctrl & SCD_IFCTRL_DECIEN));
  return 0;
}
