/* SEG-102 (ADR 0233, 0236): local diagnostic. Replay a recorded sequence of PCM chip operations through the product's chip model and compare
 * every pointer read with the value the recording carries. It is not a registered test: the recordings of authentic runs are private and
 * never committed; only the aggregate counts printed here may leave the machine.
 *
 * Input (text, one operation per line, ordered by time): "<abs> <kind> <chip offset hex> <value hex>" where <abs> is the recording's time in
 * 50 MHz-domain cycles (four per sub cycle), kind is W (write), R (pointer read, value expected) or Z (chip reset; offset and value ignored).
 * usage: sega_cd_pcm_replay FILE [line_locked=1] */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

int main(int argc, char **argv) {
  static ScdProgram program;
  static ScdAuthentic authentic;
  static uint8_t bios[0x200];
  ScdMachine *m;
  FILE *f;
  char line[256];
  uint64_t reads = 0U, equal = 0U, writes = 0U, resets = 0U, shown = 0U, last = 0U, unit;
  const int line_locked = argc > 2 ? atoi(argv[2]) != 0 : 1;
  if (argc < 2) return 2;
  f = fopen(argv[1], "r");
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (f == NULL || m == NULL) return 2;
  bios[1] = 0xFFU;
  bios[5] = 0x02U;
  if (scd_machine_init_bios(m, &program, &authentic, NULL, bios, sizeof bios, 1000000U) != 0) return 2;
  if (!line_locked) m->bios = NULL;
  unit = scd_sub_cycle_ticks(m);
  while (fgets(line, sizeof line, f) != NULL) {
    uint64_t abs;
    char kind;
    unsigned off, val;
    uint64_t tick;
    if (sscanf(line, "%" SCNu64 " %c %x %x", &abs, &kind, &off, &val) != 4) continue;
    if (abs < last) {
      printf("out-of-order operation at %" PRIu64 "\n", abs);
      return 3;
    }
    last = abs;
    tick = abs * unit / 4U; /* exact on the multiples of four that carry the sample grid */
    m->now = tick;
    if (kind == 'Z') {
      scd_pcm_reset(m);
      ++resets;
    } else if (kind == 'W') {
      if (scd_write(m, SCD_SUB, 0xFF0001U + 2U * off, 1U, val, 0x100U) != 0) {
        printf("write stopped at %" PRIu64 "\n", abs);
        return 3;
      }
      ++writes;
    } else if (kind == 'R') {
      uint32_t got = 0U;
      ++reads;
      if (scd_read(m, SCD_SUB, 0xFF0001U + 2U * off, 1U, &got, 0x100U) != 0) {
        printf("read stopped at %" PRIu64 " (offset %X)\n", abs, off);
        return 3;
      }
      if (got == val) ++equal;
      else if (shown++ < 5U) printf("  mismatch at %" PRIu64 ": offset %X expected %X got %X\n", abs, off, val, got);
    }
  }
  printf("replayed %" PRIu64 " writes, %" PRIu64 " resets, %" PRIu64 " pointer reads: %" PRIu64 " equal, %" PRIu64 " different (syncs %" PRIu64
         ", sample events %" PRIu64 ", channel steps %" PRIu64 ", largest catch-up %" PRIu64 ")\n",
         writes, resets, reads, equal, reads - equal, m->pcm.syncs, m->pcm.advanced, m->pcm.channel_steps, m->pcm.max_catchup);
  fclose(f);
  scd_machine_release(m);
  free(m);
  return reads == equal ? 0 : 1;
}
