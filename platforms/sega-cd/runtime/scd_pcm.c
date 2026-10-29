/* SEG-064-T004 / SEG-102: the sub's PCM sound chip (RF5C164). SEG-064 modelled the register file and wave RAM; SEG-102 (ADR 0233) adds
 * the per-channel running wave address and the sample clock that advances it, so the wave-address-pointer registers can be read back.
 * Sound generation is still not modelled (no audio output exists in this runtime): only state the sub processor can observe is
 * exact. Every access not independently verified is the typed stop pcm_unsupported.
 *
 * Source: Genesis Plus GX core/cd_hw/pcm.c (consulted only) and the SEG-064-T001 reference's boot access inventory (write-only).
 * Layout (GPGX scd_write_byte/scd_write_word, scd.c): the PCM area $FF0000-$FF7FFF is decoded on /LDS only: a byte write at an odd
 * address, or a word write (its low byte), reaches the chip at (address >> 1) & $1FFF; a byte write at an even address is an
 * unused-area write (dropped). Chip offsets $0000-$0FFF are registers ($00 ENV, $01 PAN, $02/$03 FD low/high, $04/$05 LS low/high,
 * $06 ST, $07 CTRL, $08 ON/OFF, others illegal and ignored), $1000-$1FFF the selected 4 KiB wave RAM bank. CTRL with bit 6 set
 * selects the channel (bits 2-0), otherwise the bank (bits 3-0); bit 7 is the chip enable. The channel registers belong to the
 * selected channel at the time of the write.
 *
 * Sample clock (ADR 0233): the chip runs at one sample per 384 sub cycles (the stopwatch's unit; GPGX PCM_SCYCLES_RATIO 1536 of its
 * 4-per-sub-cycle clock; MAME rf5c68 clock/384), on the machine's profile-aware sub cycle. Sample events fall at origin + k x period
 * (k = 0, 1, ...), origin = the last chip reset. An access at tick t observes exactly the events strictly before t (an access on an
 * event instant sees the state before the event; the reference's ceil rule). Each event, for every running channel while the chip is
 * enabled: read the wave byte at address bits 26-11; a $FF loop marker replaces the address by the loop address << 11 (no increment
 * on that event), otherwise the address increases by the channel's frequency step, modulo 2^27. A state-changing write synchronizes
 * the chip to its own tick first. The pointer registers are chip offsets $10-$1F: an even offset is address bits 18-11, an odd one
 * bits 26-19 (channel = (offset >> 1) & 7). */
#include "scd_runtime.h"

#include <stdio.h>
#include <string.h>

/* GPGX scd_reset() ends with pcm_reset() for the power-on and the peripheral reset alike: the chip clears with the external wave RAM, the
 * default bank and channel 0 are selected and every channel's panning is $FF. */
void scd_pcm_reset(ScdMachine *m) {
  uint32_t c;
  const uint64_t syncs = m->pcm.syncs, advanced = m->pcm.advanced, steps = m->pcm.channel_steps, catchup = m->pcm.max_catchup;
  if (m->pcm_trace != NULL) fprintf((FILE *)m->pcm_trace, "Z %llu %llu\n", (unsigned long long)m->vdp_frames, (unsigned long long)m->now);
  memset(&m->pcm, 0, sizeof m->pcm);
  for (c = 0; c < 8U; ++c) m->pcm.pan[c] = 0xFFU;
  m->pcm.origin = m->now; /* the sample grid restarts at the reset instant (power-on: tick 0) */
  m->pcm.syncs = syncs;
  m->pcm.advanced = advanced;
  m->pcm.channel_steps = steps;
  m->pcm.max_catchup = catchup;
}

#define SCD_PCM_ADDR_MASK UINT32_C(0x7FFFFFF)

static void scd_pcm_step(ScdPcm *p, uint32_t c) {
  if (p->ram[(p->addr[c] >> 11) & 0xFFFFU] == 0xFFU) p->addr[c] = (uint32_t)p->ls[c] << 11;
  else p->addr[c] = (p->addr[c] + p->fd[c]) & SCD_PCM_ADDR_MASK;
}

void scd_pcm_sync(ScdMachine *m, uint64_t tick) {
  ScdPcm *p = &m->pcm;
  const uint64_t period = SCD_TIMER_UNIT_SUB_CYCLES * scd_sub_cycle_ticks(m);
  uint64_t due, n;
  ++p->syncs;
  if (tick <= p->origin) return;
  due = (tick - p->origin + period - 1U) / period; /* events at origin + k x period strictly before tick: k < (tick - origin) / period */
  if (due <= p->samples) return;
  n = due - p->samples;
  p->samples = due;
  p->advanced += n;
  if (n > p->max_catchup) p->max_catchup = n;
  if (!p->enabled || p->running == 0U) return; /* time passes; no address moves */
  for (; n != 0U; --n) {
    uint32_t c;
    for (c = 0; c < 8U; ++c)
      if ((p->running >> c) & 1U) {
        scd_pcm_step(p, c);
        ++p->channel_steps;
      }
  }
}

static int scd_pcm_unsupported(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t pc) {
  return scd_fail_access(m, SCD_SUB, SCD_STOP_PCM_UNSUPPORTED, pc, 0xFF0000U + offset, width, is_write);
}

static void scd_pcm_write_chip(ScdMachine *m, uint32_t address, uint32_t data) {
  ScdPcm *p = &m->pcm;
  const uint32_t c = p->index;
  uint32_t k;
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
  case 0x06:
    p->st[c] = (uint8_t)data;
    if (!((p->running >> c) & 1U)) p->addr[c] = (uint32_t)data << 19; /* an OFF channel's address follows its start address */
    break;
  case 0x07:
    if (data & 0x40U) p->index = (uint8_t)(data & 7U);
    else p->bank = (uint8_t)(data & 15U);
    p->enabled = (uint8_t)((data >> 7) & 1U);
    break;
  case 0x08:
    p->onoff = (uint8_t)data;
    p->running = (uint8_t)~data;
    for (k = 0; k < 8U; ++k)
      if ((data >> k) & 1U) p->addr[k] = (uint32_t)p->st[k] << 19; /* every channel turned (or kept) OFF reloads from its start address */
    break;
  default: break; /* illegal registers: no effect (GPGX) */
  }
}

/* The pointer registers (chip offsets $10-$1F) are the only independently verified reads (ADR 0233). */
static int scd_pcm_is_pointer(uint32_t chip_offset) { return chip_offset >= 0x10U && chip_offset < 0x20U; }

int scd_pcm_access(ScdMachine *m, uint32_t offset, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  if (!is_write) {
    /* Only /LDS carries the chip (contract section 17): a byte read at an even address is the unused lane, whose open-bus value is not
     * modelled. A word read returns the chip byte in its low lane; the upper lane is the reference's zero (not hardware-verified). A refused
     * read changes nothing (no synchronization). */
    const uint32_t chip = (offset >> 1) & 0x1FFFU;
    uint32_t byte;
    if ((width == 1U && !(offset & 1U)) || !scd_pcm_is_pointer(chip)) return scd_pcm_unsupported(m, offset, width, 0, pc);
    scd_pcm_sync(m, m->now);
    byte = (m->pcm.addr[(chip >> 1) & 7U] >> ((chip & 1U) ? 19 : 11)) & 0xFFU;
    *value = byte;
    scd_log(m, 0x3AU, offset, byte);
    if (m->pcm_trace != NULL) /* SEG-102 diagnostic: onsets so far, machine tick, bus offset, width, value, channel address, samples applied */
      fprintf((FILE *)m->pcm_trace, "R %llu %llu %X %u %X %X %llu\n", (unsigned long long)m->vdp_frames, (unsigned long long)m->now, offset, width, byte,
              m->pcm.addr[(chip >> 1) & 7U], (unsigned long long)m->pcm.samples);
    return 0;
  }
  if (width == 1U && !(offset & 1U)) { /* /LDS not asserted: an unused-area write */
    ++m->ignored_writes;
    scd_log(m, 0x51U, 0xFF0000U + offset, *value);
    return 0;
  }
  scd_log(m, 0x39U, offset, *value);
  scd_pcm_sync(m, m->now);
  if (m->pcm_trace != NULL) /* SEG-102 diagnostic: onsets so far, machine tick, chip offset, value, samples applied (before the write) */
    fprintf((FILE *)m->pcm_trace, "W %llu %llu %X %X %llu\n", (unsigned long long)m->vdp_frames, (unsigned long long)m->now, (offset >> 1) & 0x1FFFU,
            (unsigned)(*value & 0xFFU), (unsigned long long)m->pcm.samples);
  scd_pcm_write_chip(m, (offset >> 1) & 0x1FFFU, *value & 0xFFU);
  return 0;
}

int scd_pcm_trace_open(ScdMachine *m, const char *path) {
  m->pcm_trace = fopen(path, "w");
  return m->pcm_trace == NULL;
}

void scd_pcm_trace_close(ScdMachine *m) {
  if (m->pcm_trace != NULL) fclose((FILE *)m->pcm_trace);
  m->pcm_trace = NULL;
}
