/*
 * SEG-032-T006: the library-only PSG reference for the Genesis integration test. Reads `<cycles> <byte>` lines (arrival order, PSG
 * input-clock cycles), drives a fresh Sn76489 exactly as a host would (advance, then write) and prints the final state after running
 * to <final cycles>. With divider argument 15 the cycle numbers are master ticks / 15 (the integration); a mutation passes another.
 *   genesis_psg_reference <script> <final-cycles>
 */
#include <stdio.h>
#include <stdlib.h>

#include "segarecomp/device/sega/psg/sn76489.h"

int main(int argc, char **argv) {
  Sn76489 chip;
  Sn76489Config config = sn76489_default_config();
  unsigned long long cycles;
  unsigned byte;
  FILE *script;
  uint8_t state[SN76489_STATE_BYTES];
  unsigned i;
  if (argc < 3) return 2;
  script = fopen(argv[1], "r");
  if (script == NULL || !sn76489_init(&chip, &config)) return 2;
  sn76489_reset(&chip);
  while (fscanf(script, "%llu %x", &cycles, &byte) == 2) {
    sn76489_advance(&chip, NULL, cycles);
    (void)sn76489_write(&chip, (uint8_t)byte);
  }
  sn76489_advance(&chip, NULL, strtoull(argv[2], NULL, 10));
  sn76489_state_bytes(&chip, state);
  printf("PSG ");
  for (i = 0; i < SN76489_STATE_BYTES; ++i) printf("%02x", (unsigned)state[i]);
  printf("\n");
  return 0;
}
