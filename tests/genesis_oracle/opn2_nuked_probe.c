/* SEG-032-T001 (ADR 0074): injection/observation shape of the independent YM2612 oracle (Nuked-OPN2, LGPL-2.1, test-only).
 * usage: opn2_nuked_probe <key-on 0|1>   -> "samples=N nonzero=N fnv=H busy_after_write=B status_idle=S" */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "ym3438.h"
#include "trace.h"

static void write_reg(ym3438_t *c, unsigned port, unsigned char reg, unsigned char value, int32_t *acc, long *clocks) {
  int16_t buf[2];
  OPN2_Write(c, port * 2 + 0, reg);
  for (int i = 0; i < 4; ++i) { OPN2_Clock(c, buf); acc[0] += buf[0]; acc[1] += buf[1]; ++*clocks; }
  OPN2_Write(c, port * 2 + 1, value);
  for (int i = 0; i < 64; ++i) { OPN2_Clock(c, buf); acc[0] += buf[0]; acc[1] += buf[1]; ++*clocks; }
}

int main(int argc, char **argv) {
  ym3438_t chip;
  int16_t buf[2];
  int32_t acc[2] = {0, 0};
  long clocks = 0, distinct = 0;
  int32_t last_l = 0, last_r = 0;
  uint64_t fnv = 0xcbf29ce484222325ULL;
  int key_on = argc > 1 && atoi(argv[1]) != 0;
  OPN2_Reset(&chip);
  unsigned status_idle = OPN2_Read(&chip, 0);
  for (unsigned i = 0; i < TRACE_COUNT; ++i) write_reg(&chip, trace_writes[i].port, trace_writes[i].reg, trace_writes[i].value, acc, &clocks);
  write_reg(&chip, 0, 0x28, key_on ? 0xF0 : 0x00, acc, &clocks);
  OPN2_Write(&chip, 0, 0x28);
  OPN2_Write(&chip, 1, key_on ? 0xF0 : 0x00);
  for (int i = 0; i < 4; ++i) OPN2_Clock(&chip, buf);
  unsigned busy = (OPN2_Read(&chip, 0) >> 7) & 1;
  for (unsigned s = 0; s < TRACE_SAMPLES; ++s) {
    int32_t l = 0, r = 0;
    for (int i = 0; i < 24; ++i) { OPN2_Clock(&chip, buf); l += buf[0]; r += buf[1]; }
    if (s == 0 || l != last_l || r != last_r) ++distinct;
    last_l = l; last_r = r;
    fnv = (fnv ^ (uint32_t)l) * 0x100000001b3ULL;
    fnv = (fnv ^ (uint32_t)r) * 0x100000001b3ULL;
  }
  printf("samples=%u distinct=%ld fnv=%016llx busy_after_write=%u status_idle=%u\n", TRACE_SAMPLES, distinct,
         (unsigned long long)fnv, busy, status_idle);
  return 0;
}
