/* SEG-064-T002 (ADR 0142): the BIOS-image profile's sub runnable epoch.
 *
 * The epoch is the sub's first runnable transition after a reset (SRES 0 -> 1 with SBRQ = 0, or SBRQ 1 -> 0 after such a reset):
 * the hardware event after which the sub fetches its vectors from PRG RAM. The live PRG RAM [0, E) - E the end of its last non-zero
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
      m->epochs += 1U;
      m->epoch_image = (int32_t)i;
      m->epoch_tick = m->now;
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
