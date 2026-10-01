/*
 * SEG-032-T009: replays a recorded PSG write list against the SN76489 library DIRECTLY (no runtime, no mixer) and writes the level of every
 * tick as little-endian u32. The reference side of the whole-machine mixer comparison.
 *   genesis_psg_levels_driver <script> <levels-out>
 * script lines (decimal): `w <byte> <master_ticks>`, `adv <master_ticks>`. A tick j starts at master tick j x 240 (16 PSG clocks of
 * 15 master ticks) and is executed once its end is at or before the target; a write first runs every tick that ends at or before it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "segarecomp/device/sega/psg/sn76489.h"

int main(int argc, char **argv) {
  Sn76489 chip;
  Sn76489Config config = sn76489_default_config();
  FILE *script, *out;
  char line[128];
  uint64_t done = 0;
  if (argc < 3) return 2;
  script = fopen(argv[1], "r");
  out = fopen(argv[2], "wb");
  if (script == NULL || out == NULL || !sn76489_init(&chip, &config)) return 2;
  sn76489_reset(&chip);
  while (fgets(line, sizeof line, script) != NULL) {
    char op[8];
    unsigned long long a = 0, b = 0;
    uint64_t target;
    if (sscanf(line, "%7s %llu %llu", op, &a, &b) < 2) continue;
    target = (strcmp(op, "w") == 0 ? b : a) / 240U;
    for (; done < target; ++done) {
      const uint32_t level = sn76489_tick(&chip, NULL);
      fwrite(&level, sizeof level, 1, out);
    }
    if (strcmp(op, "w") == 0) (void)sn76489_write(&chip, (uint8_t)a);
  }
  fclose(out);
  return 0;
}
