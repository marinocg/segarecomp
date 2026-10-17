/* SEG-064-T002 (ADR 0142): the BIOS-image profile's sub runnable epoch.
 *
 * The epoch is the sub's first runnable transition after a reset (SRES 0 -> 1 with SBRQ = 0, or SBRQ 1 -> 0 after such a reset).
 * In this profile the sub's reset exception (its vector fetch, a bus cycle it cannot make while SBRQ grants the bus to main) is taken
 * here, after verification, from the verified bytes (the synthetic profile keeps taking it at the SRES write, contract section 4). The live PRG RAM [0, E) - E the end of its last non-zero
 * byte rounded up to 256 bytes - must equal one admitted materialized image (length and SHA-256), otherwise the sub never retires an
 * instruction: the typed stop materialized_image_unverified. In a build pass (m->materialize) an unknown image is instead captured
 * for admission (materialization_captured; the driver writes the snapshot). Comparison only: nothing is decoded or compiled. */
#include "scd_runtime.h"

#include <stdio.h>
#include <string.h>

#include "runtime.h"

uint32_t scd_prg_snapshot_length(const ScdMachine *m) {
  uint32_t end = SCD_PRG_RAM_BYTES;
  while (end > 0U && m->prg_ram[end - 1U] == 0U) --end;
  return (end + 0xFFU) & ~UINT32_C(0xFF);
}

static void scd_sha_hex_of(const uint8_t *data, uint32_t length, char out[65]) {
  GenesisSha256 sha;
  uint8_t digest[32];
  unsigned i;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, data, length);
  genesis_sha256_final(&sha, digest);
  for (i = 0; i < 32U; ++i) snprintf(out + 2U * i, 3U, "%02x", digest[i]);
}

int scd_sub_runnable_epoch(ScdMachine *m, uint32_t pc) {
  const ScdAuthentic *a = m->authentic;
  const uint32_t length = scd_prg_snapshot_length(m);
  char digest[65];
  uint32_t i;
  m->sub_pristine = 0U;
  scd_sha_hex_of(m->prg_ram, length, digest);
  scd_log(m, 0x70U, length, (uint32_t)digest[0]);
  for (i = 0; a != NULL && i < a->sub_image_count; ++i) {
    if (a->sub_images[i].length == length && strcmp(a->sub_images[i].sha256, digest) == 0) {
      ScdCpu *sub = &m->cpu[SCD_SUB];
      uint32_t k, ssp = 0U, entry = 0U;
      m->epochs += 1U;
      m->epoch_image = (int32_t)i;
      m->epoch_tick = m->now;
      /* The reset exception, from the verified image: SSP at $000000, PC at $000004 (contract section 4), 40 clocks (UM68K). */
      for (k = 0; k < 4U; ++k) {
        ssp = (ssp << 8U) | m->prg_ram[k];
        entry = (entry << 8U) | m->prg_ram[4U + k];
      }
      sub->a[7] = ssp;
      sub->pc = entry & UINT32_C(0x00FFFFFF);
      sub->sr = UINT16_C(0x2700);
      sub->irq.stopped = 0U;
      sub->irq.sampled_level = 0U;
      sub->irq.level7_edge_pending = 0U;
      if (sub->next < m->now) sub->next = m->now;
      sub->next += UINT64_C(40) * sub->cycle_ticks;
      scd_log(m, 0x71U, ssp, entry);
      return 0;
    }
  }
  m->epoch_image = -1;
  if (m->materialize) {
    m->capture_kind = 1U;
    m->capture_length = length;
    return scd_fail(m, SCD_SUB, SCD_STOP_MATERIALIZATION_CAPTURED, pc, length);
  }
  return scd_fail(m, SCD_SUB, SCD_STOP_MATERIALIZED_IMAGE_UNVERIFIED, pc, length);
}
