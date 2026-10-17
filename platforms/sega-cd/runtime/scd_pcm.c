/* SEG-064-T004: the sub's PCM sound chip (RF5C164) as the authentic BIOS drives it before the application handoff: a write-only
 * register file and wave RAM. Sound generation is not modelled (no audio output exists in this runtime), so only state that does not
 * depend on a running channel is observable; every other access is the typed stop pcm_unsupported.
 *
 * Source: Genesis Plus GX core/cd_hw/pcm.c (consulted only) and the SEG-064-T001 reference's boot access inventory (write-only).
 * Layout (GPGX scd_write_byte/scd_write_word, scd.c): the PCM area $FF0000-$FF7FFF is decoded on /LDS only: a byte write at an odd
 * address, or a word write (its low byte), reaches the chip at (address >> 1) & $1FFF; a byte write at an even address is an
 * unused-area write (dropped). Chip offsets $0000-$0FFF are registers ($00 ENV, $01 PAN, $02/$03 FD low/high, $04/$05 LS low/high,
 * $06 ST, $07 CTRL, $08 ON/OFF, others illegal and ignored), $1000-$1FFF the selected 4 KiB wave RAM bank. CTRL with bit 6 set
 * selects the channel (bits 2-0), otherwise the bank (bits 3-0); bit 7 is the chip enable. The channel registers belong to the
 * selected channel at the time of the write. */
#include "scd_runtime.h"

static int scd_pcm_unsupported(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t pc) {
  return scd_fail_access(m, SCD_SUB, SCD_STOP_PCM_UNSUPPORTED, pc, 0xFF0000U + offset, width, is_write);
}

static void scd_pcm_write_chip(ScdMachine *m, uint32_t address, uint32_t data) {
  ScdPcm *p = &m->pcm;
  const uint32_t c = p->index;
  if (address >= 0x1000U) {
    p->ram[((uint32_t)p->bank << 12) | (address & 0xFFFU)] = (uint8_t)data;
    return;
  }
  switch (address) {
  case 0x00: p->env[c] = (uint8_t)data; break;
  case 0x01: p->pan[c] = (uint8_t)data; break;
  case 0x02: p->fd[c] = (uint16_t)((p->fd[c] & 0xFF00U) | data); break;
  case 0x03: p->fd[c] = (uint16_t)((p->fd[c] & 0x00FFU) | (data << 8)); break;
  case 0x04: p->ls[c] = (uint16_t)((p->ls[c] & 0xFF00U) | data); break;
  case 0x05: p->ls[c] = (uint16_t)((p->ls[c] & 0x00FFU) | (data << 8)); break;
  case 0x06: p->st[c] = (uint8_t)data; break;
  case 0x07:
    if (data & 0x40U) p->index = (uint8_t)(data & 7U);
    else p->bank = (uint8_t)(data & 15U);
    p->enabled = (uint8_t)((data >> 7) & 1U);
    break;
  case 0x08: p->onoff = (uint8_t)data; break;
  default: break; /* illegal registers: no effect (GPGX) */
  }
}

int scd_pcm_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  if (!is_write) return scd_pcm_unsupported(m, offset, width, 0, pc); /* running-channel state and the RAM read path are not modelled */
  if (width == 1U && !(offset & 1U)) { /* /LDS not asserted: an unused-area write */
    ++m->ignored_writes;
    scd_log(m, 0x51U, 0xFF0000U + offset, *value);
    return 0;
  }
  scd_log(m, 0x39U, offset, *value);
  scd_pcm_write_chip(m, (offset >> 1) & 0x1FFFU, *value & 0xFFU);
  return 0;
}
