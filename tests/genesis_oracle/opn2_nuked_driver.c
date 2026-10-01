/* SEG-032-T007: the independent oracle (Nuked-OPN2, test-only) driven by the same script as tests/tools/ym2612_driver.c.
 * One Nuked clock = 6 YM2612 input clocks = 42 master ticks; one native sample = 24 Nuked clocks. Writes are issued after clocking to
 * the access time; the sample is the SUM of the 24 multiplexed outputs of its window. Prints status reads (`R <ticks> <value>`) and
 * `SAMPLES <count>`; samples go to <samples-out> as int32 pairs. usage: opn2_nuked_driver <script> <samples-out> */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ym3438.h"

static ym3438_t chip;
static uint64_t clocks_done;
static int32_t acc[2];
static unsigned in_sample;
static FILE *out;
static uint64_t count;

static void clock_to(uint64_t target) {
  while (clocks_done < target) {
    int16_t buf[2];
    OPN2_Clock(&chip, buf);
    acc[0] += buf[0]; acc[1] += buf[1];
    ++clocks_done;
    if (++in_sample == 24u) {
      fwrite(acc, sizeof acc[0], 2, out);
      ++count;
      acc[0] = acc[1] = 0; in_sample = 0;
    }
  }
}

int main(int argc, char **argv) {
  FILE *script;
  char line[128];
  if (argc < 3) return 2;
  script = fopen(argv[1], "r");
  out = fopen(argv[2], "wb");
  if (script == NULL || out == NULL) return 2;
  OPN2_Reset(&chip);
  while (fgets(line, sizeof line, script) != NULL) {
    char op[16];
    unsigned long port = 0, value = 0;
    unsigned long long ticks = 0;
    if (sscanf(line, "%15s", op) != 1) continue;
    if (strcmp(op, "w") == 0 && sscanf(line, "%*s %lu %lu %llu", &port, &value, &ticks) == 3) {
      clock_to(ticks / 42u);
      OPN2_Write(&chip, (uint32_t)port, (uint8_t)value);
      clock_to(ticks / 42u + 1u);  /* let the write register */
    } else if (strcmp(op, "r") == 0 && sscanf(line, "%*s %lu %llu", &port, &ticks) == 2) {
      clock_to(ticks / 42u);
      printf("R %llu %u\n", ticks, (unsigned)OPN2_Read(&chip, (uint32_t)port));
    } else if (strcmp(op, "adv") == 0 && sscanf(line, "%*s %llu", &ticks) == 1) {
      clock_to(ticks / 42u);
    }
  }
  printf("SAMPLES %llu\n", (unsigned long long)count);
  fclose(out);
  return 0;
}
