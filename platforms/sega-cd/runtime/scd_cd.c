/* SEG-062-T005: the CD drive (CDD) command/status port and the LC8951 CDC subset of the one transfer chain the synthetic profile
 * supports: 75 Hz sector delivery into the 16 KiB CDC buffer, then CDC DMA (destination 7) into Word RAM. Contract sections 7, 8.
 *
 * DMA is not atomic (V1, GPGX cdc_dma_update): it moves 16-bit words at 4 sub cycles per byte in 8-byte groups from its start
 * time; any observation point (a CDC register access, a Word RAM access, a Word RAM ownership change) first brings it up to the
 * current instruction's start tick, and an abort keeps every byte already moved. */
#include "scd_runtime.h"

#include <string.h>

#define SCD_IFSTAT_DTEI 0x40U
#define SCD_IFSTAT_DECI 0x20U
#define SCD_IFSTAT_DTBSY 0x08U
#define SCD_IFSTAT_DTEN 0x02U
#define SCD_IFCTRL_DOUTEN 0x02U
#define SCD_CTRL0_DECEN 0x80U
#define SCD_CTRL0_WRRQ 0x04U
#define SCD_CDD_STOP 0x00U
#define SCD_CDD_PLAY 0x01U
#define SCD_CDD_SEEK 0x02U

static uint64_t scd_dma_byte_ticks(void) { return SCD_DMA_SUB_CYCLES_PER_BYTE * SCD_SUB_CYCLE_TICKS; }

/* The CDC /INT line: level 5 is requested on its falling edge (a newly asserted source) with IEN5 (V1, GPGX). */
static void scd_cdc_irq_update(ScdMachine *m) {
  const uint8_t line = (uint8_t)(~m->cdc.ifstat & m->cdc.ifctrl & (SCD_IFSTAT_DTEI | SCD_IFSTAT_DECI));
  if ((line & ~m->cdc.irq_line) != 0U && (m->ga.interrupt_enable & 0x20U)) scd_raise_sub_level(m, 5U);
  m->cdc.irq_line = line;
}

static int scd_cdc_dma_move(ScdMachine *m, uint32_t bytes) {
  uint32_t i;
  for (i = 0; i < bytes; i += 2U) {
    const uint32_t source = m->cdc.dac & 0x3FFEU;
    const uint16_t word = (uint16_t)((m->cdc.buffer[source] << 8) | m->cdc.buffer[source + 1U]);
    const uint32_t offset = ((uint32_t)m->cdc.dma_address << 3) + ((m->cdc.dma_done & 7U));
    if (scd_word_ram_dma_store(m, offset, word) != 0) return scd_fail(m, SCD_SUB, SCD_STOP_CDC_DMA_DESTINATION_NOT_OWNED, 0U, offset);
    m->cdc.dac = (uint16_t)(m->cdc.dac + 2U);
    m->cdc.dma_done += 2U;
    if ((m->cdc.dma_done & 7U) == 0U) m->cdc.dma_address = (uint16_t)(m->cdc.dma_address + 1U);
  }
  m->cdc.dbc = (uint16_t)(m->cdc.dbc - bytes);
  return 0;
}

static void scd_cdc_dma_finish(ScdMachine *m) {
  m->cdc.dma_active = 0U;
  m->cdc.dbc = 0xFFFFU;
  m->cdc.ifstat = (uint8_t)((m->cdc.ifstat | SCD_IFSTAT_DTBSY | SCD_IFSTAT_DTEN) & ~SCD_IFSTAT_DTEI);
  m->cdc.edt = 1U;
  m->cdc.dsr = 0U;
  scd_cdc_irq_update(m);
  scd_log(m, 0x92U, m->cdc.dma_total, m->cdc.dac);
}

/* Bring an in-flight DMA up to tick `now`. Returns 1 on a stop. */
static int scd_cdc_sync(ScdMachine *m, uint64_t now) {
  uint64_t elapsed, target;
  if (!m->cdc.dma_active) return 0;
  elapsed = now > m->cdc.dma_start ? now - m->cdc.dma_start : 0U;
  target = elapsed / scd_dma_byte_ticks();
  if (target >= m->cdc.dma_total) target = m->cdc.dma_total;
  else target &= ~(uint64_t)7U;
  if (target > m->cdc.dma_done && scd_cdc_dma_move(m, (uint32_t)target - m->cdc.dma_done) != 0) return 1;
  if (m->cdc.dma_done >= m->cdc.dma_total) scd_cdc_dma_finish(m);
  return 0;
}

int scd_cd_observe(ScdMachine *m) { return scd_cdc_sync(m, m->now); }

static void scd_cdc_reset(ScdMachine *m) {
  m->cdc.ifstat = 0xFFU;
  m->cdc.ifctrl = 0U;
  m->cdc.ctrl0 = 0U;
  m->cdc.ctrl1 = 0U;
  m->cdc.stat0 = 0U;
  m->cdc.stat2 = 0U;
  m->cdc.stat3 = 0x80U;
  m->cdc.head[0] = m->cdc.head[1] = m->cdc.head[2] = 0U;
  m->cdc.head[3] = 1U;
  m->cdc.dma_active = 0U;
  m->cdc.irq_line = 0U;
  m->cdc.address = 0U;
  m->ga.pending = (uint8_t)(m->ga.pending & ~0x20U);
}

static int scd_cdc_unsupported(ScdMachine *m, uint32_t reg, uint32_t pc) {
  return scd_fail(m, SCD_SUB, SCD_STOP_CDC_REGISTER_UNSUPPORTED, pc, reg);
}

static int scd_cdc_start_transfer(ScdMachine *m, uint32_t pc) {
  if (!(m->cdc.ifctrl & SCD_IFCTRL_DOUTEN)) return 0; /* DTRG without DOUTEN: no transfer (V1) */
  if (m->cdc.destination != 7U) return scd_fail(m, SCD_SUB, SCD_STOP_CDC_DESTINATION_UNSUPPORTED, pc, m->cdc.destination);
  if (!m->ga.mode_1m && m->ga.ret) return scd_fail(m, SCD_SUB, SCD_STOP_CDC_DMA_DESTINATION_NOT_OWNED, pc, 7U);
  if (((uint32_t)(m->cdc.dbc & 0x0FFFU) + 1U) & 1U) return scd_cdc_unsupported(m, 0x102U, pc); /* odd byte count: U */
  m->cdc.ifstat = (uint8_t)(m->cdc.ifstat & ~(SCD_IFSTAT_DTBSY | SCD_IFSTAT_DTEN));
  m->cdc.edt = 0U;
  m->cdc.dsr = 0U;
  m->cdc.dma_active = 1U;
  m->cdc.dma_total = (uint32_t)(m->cdc.dbc & 0x0FFFU) + 1U;
  m->cdc.dma_done = 0U;
  m->cdc.dma_start = m->now;
  scd_log(m, 0x91U, m->cdc.dma_total, m->cdc.dac);
  return 0;
}

static int scd_cdc_register_write(ScdMachine *m, uint32_t value, uint32_t pc) {
  const uint32_t reg = m->cdc.address;
  value &= 0xFFU;
  switch (reg) {
  case 0x01:
    if (!(value & SCD_IFCTRL_DOUTEN) && m->cdc.dma_active) { /* abort: the bytes already moved stay (V1) */
      m->cdc.dma_active = 0U;
      m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DTBSY | SCD_IFSTAT_DTEN);
      scd_log(m, 0x93U, m->cdc.dma_done, m->cdc.dma_total);
    }
    m->cdc.ifctrl = (uint8_t)value;
    scd_cdc_irq_update(m);
    break;
  case 0x02: m->cdc.dbc = (uint16_t)((m->cdc.dbc & 0xFF00U) | value); break;
  case 0x03: m->cdc.dbc = (uint16_t)((m->cdc.dbc & 0x00FFU) | ((value & 0x0FU) << 8)); break;
  case 0x04: m->cdc.dac = (uint16_t)((m->cdc.dac & 0xFF00U) | value); break;
  case 0x05: m->cdc.dac = (uint16_t)((m->cdc.dac & 0x00FFU) | (value << 8)); break;
  case 0x06:
    if (scd_cdc_start_transfer(m, pc) != 0) return 1;
    break;
  case 0x07:
    m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DTEI);
    scd_cdc_irq_update(m);
    break;
  case 0x08: m->cdc.wa = (uint16_t)((m->cdc.wa & 0xFF00U) | value); break;
  case 0x09: m->cdc.wa = (uint16_t)((m->cdc.wa & 0x00FFU) | (value << 8)); break;
  case 0x0A:
    m->cdc.ctrl0 = (uint8_t)value;
    m->cdc.stat0 = (uint8_t)(value & SCD_CTRL0_DECEN);
    if (!(value & SCD_CTRL0_DECEN)) m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DECI);
    scd_cdc_irq_update(m);
    break;
  case 0x0B:
    if (value != 0U) return scd_cdc_unsupported(m, reg, pc); /* only Mode-1 data without subheaders (C) */
    m->cdc.ctrl1 = 0U;
    break;
  case 0x0C: m->cdc.pt = (uint16_t)((m->cdc.pt & 0xFF00U) | value); break;
  case 0x0D: m->cdc.pt = (uint16_t)((m->cdc.pt & 0x00FFU) | (value << 8)); break;
  case 0x0F:
    scd_cdc_reset(m);
    return 0; /* the register address is 0 after a reset: no increment */
  default: return scd_cdc_unsupported(m, reg, pc);
  }
  m->cdc.address = (uint8_t)((m->cdc.address + 1U) & 0x0FU);
  return 0;
}

static int scd_cdc_register_read(ScdMachine *m, uint32_t *value, uint32_t pc) {
  const uint32_t reg = m->cdc.address;
  switch (reg) {
  case 0x01: *value = m->cdc.ifstat; break;
  case 0x02: *value = m->cdc.dbc & 0xFFU; break;
  case 0x03: *value = m->cdc.dbc >> 8; break;
  case 0x04:
  case 0x05:
  case 0x06:
  case 0x07: *value = m->cdc.head[reg - 4U]; break;
  case 0x08: *value = m->cdc.pt & 0xFFU; break;
  case 0x09: *value = m->cdc.pt >> 8; break;
  case 0x0A: *value = m->cdc.wa & 0xFFU; break;
  case 0x0B: *value = m->cdc.wa >> 8; break;
  case 0x0C: *value = m->cdc.stat0; break;
  case 0x0D: *value = 0U; break;
  case 0x0E: *value = m->cdc.stat2; break;
  case 0x0F:
    *value = m->cdc.stat3;
    m->cdc.stat3 = 0x80U; /* reading STAT3 sets VALST back and clears the decoder interrupt (V1) */
    m->cdc.ifstat = (uint8_t)(m->cdc.ifstat | SCD_IFSTAT_DECI);
    scd_cdc_irq_update(m);
    break;
  default: return scd_cdc_unsupported(m, reg, pc);
  }
  m->cdc.address = (uint8_t)((m->cdc.address + 1U) & 0x0FU);
  return 0;
}

int scd_cdc_sub_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  if (scd_cdc_sync(m, m->now) != 0) return 1;
  switch (offset) {
  case 0x04:
  case 0x05:
    if (!is_write) {
      const uint32_t status = (uint32_t)((m->cdc.edt << 7) | (m->cdc.dsr << 6) | m->cdc.destination);
      *value = width == 2U ? ((status << 8) | m->cdc.address) : (offset == 0x04U ? status : m->cdc.address);
      return 0;
    }
    if (width == 2U || offset == 0x04U) {
      if (m->cdc.dma_active) return scd_cdc_unsupported(m, 0x104U, pc); /* destination change during a transfer: U */
      m->cdc.destination = (uint8_t)((width == 2U ? (*value >> 8) : *value) & 7U);
      m->cdc.dma_address = 0U; /* writing the mode register resets the DMA address (V1, GPGX citing MCDV) */
    }
    if (width == 2U || offset == 0x05U) m->cdc.address = (uint8_t)(*value & 0x0FU);
    return 0;
  case 0x06:
  case 0x07:
    if (width == 1U && offset == 0x06U) return scd_cdc_unsupported(m, 0x106U, pc);
    return is_write ? scd_cdc_register_write(m, *value, pc) : scd_cdc_register_read(m, value, pc);
  case 0x08:
  case 0x09: return scd_fail(m, SCD_SUB, SCD_STOP_CDC_DESTINATION_UNSUPPORTED, pc, 0xFF8008U); /* host data path */
  case 0x0A:
    if (width != 2U) return scd_cdc_unsupported(m, 0x10AU, pc); /* word-only register */
    if (is_write) {
      if (m->cdc.dma_active) return scd_cdc_unsupported(m, 0x10AU, pc);
      m->cdc.dma_address = (uint16_t)*value;
    } else {
      *value = m->cdc.dma_address;
    }
    return 0;
  default: break;
  }
  return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset);
}

/* ---- CDD -------------------------------------------------------------------------------------------------------------- */

/* The 75 Hz grid is anchored at tick 0; while the drive is inactive (not reading, HOCK clear) no tick is scheduled, and activation
 * re-anchors `next_tick` on the first grid point after the activating instruction. */
static int scd_cdd_active(const ScdMachine *m) { return m->cdd.reading || m->ga.cdd_hock; }

static void scd_cdd_activate(ScdMachine *m, int was_active) {
  if (!was_active && scd_cdd_active(m)) m->cdd.next_tick = (m->now / SCD_CDD_TICK_TICKS + 1U) * SCD_CDD_TICK_TICKS;
}

static uint64_t scd_cdd_next(const ScdMachine *m) { return scd_cdd_active(m) ? m->cdd.next_tick : UINT64_MAX; }


static void scd_cdd_status(ScdMachine *m, uint8_t rs0, uint8_t rs1) {
  uint32_t i, sum = 0U;
  memset(m->cdd.rx, 0, sizeof m->cdd.rx);
  m->cdd.rx[0] = rs0;
  m->cdd.rx[1] = rs1;
  for (i = 0; i < 9U; ++i) sum += m->cdd.rx[i];
  m->cdd.rx[9] = (uint8_t)(~sum & 0x0FU);
}

static int scd_cdd_command(ScdMachine *m, uint32_t pc) {
  uint32_t i, sum = 0U;
  for (i = 0; i < 9U; ++i) sum += m->cdd.tx[i];
  scd_log(m, 0xA0U, m->cdd.tx[0], m->cdd.tx[9]);
  if ((uint8_t)(~sum & 0x0FU) != m->cdd.tx[9]) return scd_fail(m, SCD_SUB, SCD_STOP_CDD_BAD_CHECKSUM, pc, m->cdd.tx[9]);
  switch (m->cdd.tx[0]) {
  case 0x0:
    scd_cdd_status(m, m->cdd.status, 0U);
    return 0;
  case 0x1:
    m->cdd.reading = 0U;
    m->cdd.status = SCD_CDD_STOP;
    scd_cdd_status(m, SCD_CDD_STOP, 0x0FU);
    return 0;
  case 0x3: {
    const uint8_t *t = m->cdd.tx;
    uint32_t msf, lba;
    const ScdDiscTrack *track;
    if (t[2] > 9U || t[3] > 9U || t[4] > 5U || t[5] > 9U || t[6] > 7U || t[7] > 9U || t[6] * 10U + t[7] >= 75U)
      return scd_fail(m, SCD_SUB, SCD_STOP_CDD_COMMAND_UNSUPPORTED, pc, 0x300U);
    msf = ((t[2] * 10U + t[3]) * 60U + t[4] * 10U + t[5]) * 75U + t[6] * 10U + t[7];
    if (msf < 150U) return scd_fail(m, SCD_SUB, SCD_STOP_CD_READ_OUT_OF_RANGE, pc, msf);
    lba = msf - 150U;
    track = scd_disc_track_of(m->disc, lba);
    if (track == NULL) return scd_fail(m, SCD_SUB, SCD_STOP_CD_READ_OUT_OF_RANGE, pc, lba);
    if (track->type == 2U) return scd_fail(m, SCD_SUB, SCD_STOP_CD_READ_AUDIO_TRACK, pc, lba);
    {
      const int was_active = scd_cdd_active(m);
      m->cdd.reading = 1U;
      scd_cdd_activate(m, was_active);
    }
    m->cdd.lba = lba;
    m->cdd.status = SCD_CDD_SEEK;
    /* The first sector arrives on the third 75 Hz tick after the command (two ticks of latency, contract section 8). */
    m->cdd.latency = 2U;
    scd_cdd_status(m, SCD_CDD_SEEK, 0x0FU);
    return 0;
  }
  default: break;
  }
  return scd_fail(m, SCD_SUB, SCD_STOP_CDD_COMMAND_UNSUPPORTED, pc, m->cdd.tx[0]);
}

int scd_cdd_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  if (offset == 0x36U || offset == 0x37U) {
    if (!is_write) {
      *value = width == 2U ? (uint32_t)(0x0100U | (m->ga.cdd_hock << 2)) : (offset == 0x36U ? 0x01U : (uint32_t)(m->ga.cdd_hock << 2));
      return 0;
    }
    if (width == 2U || offset == 0x37U) {
      const int was_active = scd_cdd_active(m);
      m->ga.cdd_hock = (uint8_t)((*value >> 2) & 1U);
      scd_cdd_activate(m, was_active);
    }
    return 0;
  }
  if (offset >= 0x38U && offset < 0x42U) { /* RS0..RS9 */
    const uint32_t i = offset - 0x38U;
    if (is_write) return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset);
    *value = width == 2U ? (uint32_t)((m->cdd.rx[i] << 8) | m->cdd.rx[i + 1U]) : m->cdd.rx[i];
    return 0;
  }
  if (offset >= 0x42U && offset < 0x4CU) { /* TX0..TX9 */
    const uint32_t i = offset - 0x42U;
    if (!is_write) {
      *value = width == 2U ? (uint32_t)((m->cdd.tx[i] << 8) | m->cdd.tx[i + 1U]) : m->cdd.tx[i];
      return 0;
    }
    if (width == 2U) {
      m->cdd.tx[i] = (uint8_t)((*value >> 8) & 0x0FU);
      m->cdd.tx[i + 1U] = (uint8_t)(*value & 0x0FU);
      if (offset == 0x4AU) return scd_cdd_command(m, pc); /* the word write to $FF804A sends the command (V1) */
    } else {
      m->cdd.tx[i] = (uint8_t)(*value & 0x0FU);
    }
    return 0;
  }
  return scd_fail(m, SCD_SUB, SCD_STOP_GATE_ARRAY_REGISTER_UNSUPPORTED, pc, 0xFF8000U + offset);
}

static uint8_t scd_bcd(uint32_t v) { return (uint8_t)(((v / 10U) << 4) | (v % 10U)); }

/* One 75 Hz tick: the HOCK interrupt request and, while reading, the delivery of one sector into the CDC (contract section 8). */
static int scd_cdd_tick(ScdMachine *m) {
  if (m->ga.cdd_hock) scd_raise_sub_level(m, 4U); /* pending even while IEN4 = 0 (V1, GPGX) */
  if (!m->cdd.reading) return 0;
  if (m->cdd.latency > 0U) {
    m->cdd.latency -= 1U;
    return 0;
  }
  {
    uint8_t user[2048];
    const uint32_t lba = m->cdd.lba;
    const ScdDiscTrack *track = scd_disc_track_of(m->disc, lba);
    const uint32_t msf = lba + 150U;
    uint32_t i;
    ScdDiscError error;
    if (track == NULL || track->type == 2U)
      return scd_fail(m, SCD_SUB, track == NULL ? SCD_STOP_CD_READ_OUT_OF_RANGE : SCD_STOP_CD_READ_AUDIO_TRACK, 0U, lba);
    error = scd_disc_read_user_data(m->disc, lba, user);
    if (error != SCD_DISC_OK) return scd_fail(m, SCD_SUB, SCD_STOP_CD_READ_FAILED, 0U, lba);
    m->cdd.status = SCD_CDD_PLAY;
    m->cdd.lba = lba + 1U;
    m->cdd.delivered += 1U;
    scd_log(m, 0xA1U, lba, user[0]);
    if (!(m->cdc.ctrl0 & SCD_CTRL0_DECEN)) return 0;
    m->cdc.head[0] = scd_bcd(msf / 75U / 60U);
    m->cdc.head[1] = scd_bcd(msf / 75U % 60U);
    m->cdc.head[2] = scd_bcd(msf % 75U);
    m->cdc.head[3] = 0x01U;
    m->cdc.stat3 = 0x00U;
    m->cdc.ifstat = (uint8_t)(m->cdc.ifstat & ~SCD_IFSTAT_DECI);
    scd_cdc_irq_update(m);
    if (m->cdc.ctrl0 & SCD_CTRL0_WRRQ) {
      uint32_t offset;
      m->cdc.pt = (uint16_t)(m->cdc.pt + 2352U);
      m->cdc.wa = (uint16_t)(m->cdc.wa + 2352U);
      offset = m->cdc.pt & 0x3FFFU;
      for (i = 0; i < 4U; ++i) m->cdc.buffer[(offset + i) & 0x3FFFU] = m->cdc.head[i];
      for (i = 0; i < 2048U; ++i) m->cdc.buffer[(offset + 4U + i) & 0x3FFFU] = user[i];
    }
  }
  return 0;
}

/* ---- timed device aggregation ----------------------------------------------------------------------------------------- */

uint64_t scd_timer_next_event(const ScdMachine *m);
void scd_timer_event(ScdMachine *m);

uint64_t scd_devices_next_event(const ScdMachine *m) {
  uint64_t next = scd_timer_next_event(m);
  const uint64_t cdd = scd_cdd_next(m);
  if (cdd < next) next = cdd;
  if (m->cdc.dma_active) {
    const uint64_t done = m->cdc.dma_start + (uint64_t)m->cdc.dma_total * scd_dma_byte_ticks();
    if (done < next) next = done;
  }
  return next;
}

/* Process every device event due at exactly `until`, in the fixed order: CDD tick, timer, CDC DMA. */
void scd_devices_advance(ScdMachine *m, uint64_t until) {
  if (scd_cdd_active(m) && m->cdd.next_tick <= until) {
    m->cdd.next_tick += SCD_CDD_TICK_TICKS;
    if (scd_cdd_tick(m) != 0) return;
  }
  if (scd_timer_next_event(m) <= until) scd_timer_event(m);
  if (m->cdc.dma_active) scd_cdc_sync(m, until);
}

int scd_devices_can_raise(const ScdMachine *m, uint32_t level) {
  if (level == 3U) return scd_timer_next_event(m) != UINT64_MAX;
  if (level == 4U) return m->ga.cdd_hock != 0U;
  if (level == 5U) return m->cdc.dma_active || (m->cdd.reading && (m->cdc.ctrl0 & SCD_CTRL0_DECEN));
  return 0;
}
