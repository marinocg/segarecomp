/*
 * SEG-032-T007: scripted C driver for the Genesis YM2612 device (libs/device/sega/ym2612), linked with a C driver and the C++-runtime
 * shim only (the production link shape).
 *   ym2612_driver <script> <samples-out>
 * script lines (decimal): `w <port> <value> <master_ticks>`, `r <port> <master_ticks>` (prints `R <ticks> <value>`), `adv <master_ticks>`,
 * `reset <master_ticks>`, `digest` (prints `DIGEST <hex> samples=<n>`). Native samples are written to <samples-out> as little-endian
 * int32 pairs. Prints `SAMPLES <count> <fnv64>` last.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "segarecomp/device/sega/ym2612/ym2612.h"

static FILE *out;
static uint64_t fnv = UINT64_C(0xcbf29ce484222325);
static uint64_t count;

static void sink(void *context, uint64_t start, int32_t left, int32_t right) {
  int32_t pair[2];
  unsigned i;
  (void)context; (void)start;
  pair[0] = left; pair[1] = right;
  if (out != NULL) fwrite(pair, sizeof pair[0], 2, out);
  for (i = 0; i < 8; ++i) fnv = (fnv ^ ((const unsigned char *)pair)[i]) * UINT64_C(0x100000001b3);
  ++count;
}

int main(int argc, char **argv) {
  Ym2612 *chip;
  FILE *script;
  char line[128];
  if (argc < 3) return 2;
  script = fopen(argv[1], "r");
  out = fopen(argv[2], "wb");
  if (script == NULL || out == NULL) return 2;
  chip = ym2612_create();
  if (chip == NULL) return 3;
  ym2612_set_sink(chip, sink, NULL);
  while (fgets(line, sizeof line, script) != NULL) {
    char op[16];
    unsigned long port = 0, value = 0;
    unsigned long long ticks = 0;
    if (sscanf(line, "%15s", op) != 1) continue;
    if (strcmp(op, "w") == 0 && sscanf(line, "%*s %lu %lu %llu", &port, &value, &ticks) == 3) ym2612_write(chip, (uint32_t)port, (uint8_t)value, ticks);
    else if (strcmp(op, "r") == 0 && sscanf(line, "%*s %lu %llu", &port, &ticks) == 2) printf("R %llu %u\n", ticks, (unsigned)ym2612_read(chip, (uint32_t)port, ticks));
    else if (strcmp(op, "adv") == 0 && sscanf(line, "%*s %llu", &ticks) == 1) ym2612_advance(chip, ticks);
    else if (strcmp(op, "reset") == 0 && sscanf(line, "%*s %llu", &ticks) == 1) ym2612_reset(chip, ticks);
    else if (strcmp(op, "digest") == 0) printf("DIGEST %016llx samples=%llu\n", (unsigned long long)ym2612_state_digest(chip), (unsigned long long)ym2612_samples_generated(chip));
  }
  printf("SAMPLES %llu %016llx\n", (unsigned long long)count, (unsigned long long)fnv);
  fclose(out);
  ym2612_destroy(chip);
  return 0;
}
