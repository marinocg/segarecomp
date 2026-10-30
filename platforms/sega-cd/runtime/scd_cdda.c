/* SEG-104 (ADR 0244): CD-DA output of the Sega CD drive. See ScdCdda in scd_runtime.h.
 *
 * The drive tick (scd_cd.c) already decides which sector is under the head and whether audio is playing; this file turns that sector into
 * samples. Guest time is the only clock: one tick, one sector, 588 frames - no queue depth, device clock or wall time is read.
 *
 * Fader (CD-DA volume, $FF8034): the 12-bit value in bits 15-4 is the target volume; 1024 is unity (power-on and peripheral reset). The current
 * volume moves one step per produced sample frame toward the target and the multiplier follows the LC7883KM stepping 0,1,2,3,4,8,12,...,1020,1024
 * (value & $7FC, or value & 3 below 4). These two conventions are the reference emulator's (not independently verified against hardware); targets
 * above 1024 are clamped to unity (no amplification exists). The product (sample x multiplier) / 1024 truncates toward zero. */
#include "scd_runtime.h"

#include <string.h>

#define SCD_FADER_UNITY 1024U

void scd_cdda_reset(ScdMachine *m) {
  scd_mix_advance(m, m->now); /* SEG-107: the frames before the reset keep the old fader (a no-op unless the mix is enabled) */
  scd_cdda_interrupt(m);
  m->cdda.fader_register = 0x4000U;
  m->cdda.fader_target = (uint16_t)SCD_FADER_UNITY;
  m->cdda.fader_volume = (uint16_t)SCD_FADER_UNITY;
  m->mix.fader_volume = (uint16_t)SCD_FADER_UNITY; /* SEG-107: the mix's own fader volume (the target is shared) */
  if (m->cdda.log != NULL) fprintf(m->cdda.log, "reset frame=%llu tick=%llu\n", (unsigned long long)m->vdp_frames, (unsigned long long)m->now);
}

void scd_cdda_interrupt(ScdMachine *m) {
  if (m->cdda.playing) {
    m->cdda.playing = 0U;
    m->cdda.interruptions += 1U;
    scd_mix_cdda_end(m); /* SEG-107: the mix stops refilling the run (a no-op unless the mix is enabled) */
    if (m->cdda.log != NULL) fprintf(m->cdda.log, "stop frame=%llu tick=%llu next_lba=%ld\n", (unsigned long long)m->vdp_frames, (unsigned long long)m->now, (long)m->cdda.next_lba);
  }
}

void scd_cdda_silent_tick(ScdMachine *m) {
  if (m->cdda.enabled) scd_cdda_interrupt(m);
}

/* offset 0x34 (high byte) / 0x35 (low byte) of the Gate Array, width 1 or 2 (a word write at 0x34). */
void scd_cdda_fader_write(ScdMachine *m, uint32_t offset, uint32_t width, uint32_t value) {
  uint32_t target;
  scd_mix_advance(m, m->now); /* SEG-107: frames before this write keep the old target (a no-op unless the mix is enabled) */
  if (width == 2U) m->cdda.fader_register = (uint16_t)value;
  else if (offset == 0x34U) m->cdda.fader_register = (uint16_t)((m->cdda.fader_register & 0x00FFU) | ((value & 0xFFU) << 8));
  else m->cdda.fader_register = (uint16_t)((m->cdda.fader_register & 0xFF00U) | (value & 0xFFU));
  target = (uint32_t)(m->cdda.fader_register >> 4);
  m->cdda.fader_target = (uint16_t)(target > SCD_FADER_UNITY ? SCD_FADER_UNITY : target);
  if (m->cdda.log != NULL) fprintf(m->cdda.log, "fader frame=%llu tick=%llu target=%u\n", (unsigned long long)m->vdp_frames, (unsigned long long)m->now, (unsigned)m->cdda.fader_target);
}

int32_t scd_cdda_scale(int32_t sample, uint32_t volume) {
  const uint32_t multiplier = (volume & 0x7FCU) != 0U ? (volume & 0x7FCU) : (volume & 0x03U);
  return (sample * (int32_t)multiplier) / 1024;
}

int scd_cdda_play(ScdMachine *m, int32_t lba, uint32_t track_index) {
  int16_t frames[2U * SCD_CDDA_SECTOR_FRAMES], raw[2U * SCD_CDDA_SECTOR_FRAMES];
  ScdCdda *const c = &m->cdda;
  uint32_t i;
  const int continues = c->playing && lba == c->next_lba; /* SEG-107: this sector continues the run in progress */
  if (!c->enabled) return 0;
  scd_mix_advance(m, m->now); /* SEG-107: everything before this sector's arrival is on the timeline before the run state changes */
  {
    const ScdDiscError error = scd_disc_read_cdda(m->disc, (uint32_t)lba, frames);
    if (error != SCD_DISC_OK) {
      scd_fail(m, SCD_SUB, SCD_STOP_CD_READ_FAILED, 0U, (uint32_t)lba);
      return 1;
    }
  }
  if (m->mix.enabled) memcpy(raw, frames, sizeof raw); /* the mix scales per output frame itself */
  for (i = 0; i < SCD_CDDA_SECTOR_FRAMES; ++i) {
    frames[2U * i] = (int16_t)scd_cdda_scale(frames[2U * i], c->fader_volume);
    frames[2U * i + 1U] = (int16_t)scd_cdda_scale(frames[2U * i + 1U], c->fader_volume);
    if (c->fader_volume < c->fader_target) c->fader_volume = (uint16_t)(c->fader_volume + 1U);
    else if (c->fader_volume > c->fader_target) c->fader_volume = (uint16_t)(c->fader_volume - 1U);
  }
  if (!c->playing) c->segments += 1U;
  else if (lba != c->next_lba) c->discontinuities += 1U;
  else if (track_index != c->last_index) c->track_changes += 1U;
  if (c->log != NULL && (!c->playing || lba != c->next_lba || track_index != c->last_index))
    fprintf(c->log, "%s frame=%llu tick=%llu lba=%ld track=%u\n", !c->playing ? "start" : lba != c->next_lba ? "jump" : "track", (unsigned long long)m->vdp_frames,
            (unsigned long long)m->now, (long)lba, track_index + 1U);
  if (c->sectors == 0U) c->first_frame = m->vdp_frames;
  c->last_frame = m->vdp_frames;
  c->playing = 1U;
  c->next_lba = lba + 1;
  c->last_index = track_index;
  c->sectors += 1U;
  c->frames += SCD_CDDA_SECTOR_FRAMES;
  if (c->sink != NULL && c->sink(c->sink_context, frames, SCD_CDDA_SECTOR_FRAMES) != 0) c->sink_refused += 1U;
  if (m->mix.enabled) scd_mix_cdda(m, raw, lba, track_index, !continues); /* SEG-107: the same sector joins the mixed timeline (a no-op unless the mix is enabled) */
  return 0;
}
