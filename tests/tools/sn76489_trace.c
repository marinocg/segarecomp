/* SEG-009-T007 test tool: drives the Sega PSG device library from a timestamped write script and prints observations.
 *   sn76489_trace [--taps N] [--bits N] [--divider N] [--bad-table] trace|pcm < script
 * Script lines: `w <master clock> <byte>` (non-decreasing) and a final `end <master clock>`.
 * trace: before tick j (j = 0..) every write with floor(clock / divider) == j is applied, then the tick runs and one line
 *        `j o0 o1 o2 c0 c1 c2 p0 p1 p2 noise_counter flip lfsr a0 a1 a2 a3 noise_control level` is printed (the state after the
 *        tick, this device's conventions) - the tick-level differential against the pinned references.
 * pcm:   the real catch-up path (advance to each write's clock, apply it), then every completed PCM sample, one per line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "segarecomp/device/sega/psg/sn76489.h"

typedef struct Write { uint64_t cycles; unsigned value; } Write;
static Write writes[1 << 16];
static unsigned write_count;
static uint64_t end_cycles;

static void collect(void *context, uint64_t first, const int16_t *samples, uint32_t count) {
  uint32_t i;
  (void)context;
  (void)first;
  for (i = 0; i < count; ++i) printf("%d\n", (int)samples[i]);
}

int main(int argc, char **argv) {
  Sn76489Config config = sn76489_default_config();
  static uint16_t bad_table[16];
  const char *mode = "trace";
  char kind[8];
  unsigned long long a;
  unsigned b;
  int i;
  Sn76489 chip;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--taps") == 0 && i + 1 < argc) config.noise_taps = (uint16_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--bits") == 0 && i + 1 < argc) config.lfsr_bits = (uint8_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--divider") == 0 && i + 1 < argc) config.divider = (uint8_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--bad-table") == 0) {
      memcpy(bad_table, sn76489_default_levels, sizeof bad_table);
      bad_table[6] += 1u;
      config.levels = bad_table;
    } else mode = argv[i];
  }
  while (scanf("%7s %llu", kind, &a) == 2) {
    if (strcmp(kind, "end") == 0) {
      end_cycles = a;
      break;
    }
    if (scanf("%u", &b) != 1 || write_count >= sizeof writes / sizeof writes[0]) return 2;
    writes[write_count].cycles = a;
    writes[write_count++].value = b;
  }
  if (!sn76489_init(&chip, &config)) return 2;
  if (strcmp(mode, "pcm") == 0) {
    const Sn76489PcmConfig pc = sn76489_pcm_config_44100_sms();
    Sn76489Pcm pcm;
    unsigned w;
    sn76489_pcm_init(&pcm, &pc, collect, NULL);
    for (w = 0; w < write_count; ++w) {
      sn76489_advance(&chip, &pcm, writes[w].cycles);
      sn76489_write(&chip, (uint8_t)writes[w].value);
    }
    sn76489_advance(&chip, &pcm, end_cycles);
    sn76489_pcm_flush(&pcm);
    return 0;
  } else {
    const uint64_t ticks = end_cycles / config.divider;
    unsigned w = 0;
    uint64_t j;
    for (j = 0; j < ticks; ++j) {
      uint32_t level;
      while (w < write_count && writes[w].cycles / config.divider <= j) sn76489_write(&chip, (uint8_t)writes[w++].value);
      level = sn76489_tick(&chip, NULL);
      printf("%llu %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n", (unsigned long long)j, chip.tone_output[0],
             chip.tone_output[1], chip.tone_output[2], chip.tone_counter[0], chip.tone_counter[1], chip.tone_counter[2],
             chip.tone_period[0], chip.tone_period[1], chip.tone_period[2], chip.noise_counter, chip.noise_flip, chip.lfsr,
             chip.attenuation[0], chip.attenuation[1], chip.attenuation[2], chip.attenuation[3], chip.noise_control, level);
    }
  }
  return 0;
}
