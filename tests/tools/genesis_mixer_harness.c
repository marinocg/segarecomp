/*
 * SEG-032-T009 (ADR 0075): drives the REAL genesis_mixer.c with a scripted list of native-sample deliveries.
 *   genesis_mixer_harness <script> [frames]
 * script lines (decimal): `ym <start> <left> <right>`, `psg <start> <level>`, `range <first> <count>` (before any delivery),
 * `window <k>` (prints `W <k> <T_k>`), `read <max>` (drains the consumer ring, prints `R <n>`), `end` (prints the summary).
 * With `frames` as the second argument every produced frame is printed as `F <left> <right>` (observer sink, independent of the ring).
 * The summary: `MIX frames= clipped= changes= span= nonsilent= fault= dropped= ring= sha= range_frames= range_sha=` and, after a `read`, the
 * hex of every drained frame as `D <left> <right>`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_mixer.h"

static GenesisMixer mixer;
static int print_frames;

static void sink(void *context, uint64_t index, int16_t left, int16_t right) {
  (void)context;
  (void)index;
  if (print_frames) printf("F %d %d\n", (int)left, (int)right);
}

static void hex(const uint8_t d[32]) {
  unsigned i;
  for (i = 0; i < 32U; ++i) printf("%02x", (unsigned)d[i]);
}

int main(int argc, char **argv) {
  char line[128];
  FILE *script;
  if (argc < 2) return 2;
  script = fopen(argv[1], "r");
  if (script == NULL) return 2;
  print_frames = argc > 2 && strcmp(argv[2], "frames") == 0;
  genesis_mixer_init(&mixer);
  genesis_mixer_set_sink(&mixer, sink, NULL);
  while (fgets(line, sizeof line, script) != NULL) {
    char op[16];
    long long a = 0, b = 0, c = 0;
    if (sscanf(line, "%15s %lld %lld %lld", op, &a, &b, &c) < 1) continue;
    if (strcmp(op, "ym") == 0) genesis_mixer_push_ym(&mixer, (uint64_t)a, (int32_t)b, (int32_t)c);
    else if (strcmp(op, "psg") == 0) genesis_mixer_push_psg(&mixer, (uint64_t)a, (uint32_t)b);
    else if (strcmp(op, "range") == 0) genesis_mixer_set_range(&mixer, (uint64_t)a, (uint64_t)b);
    else if (strcmp(op, "window") == 0) printf("W %lld %llu\n", a, (unsigned long long)genesis_mixer_window_start((uint64_t)a));
    else if (strcmp(op, "read") == 0) {
      static int16_t out[2 * GENESIS_MIXER_RING_FRAMES];
      uint32_t n = genesis_mixer_ring_read(&mixer, out, (uint32_t)a), i;
      printf("R %u\n", (unsigned)n);
      for (i = 0; i < n; ++i) printf("D %d %d\n", (int)out[2U * i], (int)out[2U * i + 1U]);
    } else if (strcmp(op, "end") == 0) break;
  }
  {
    uint8_t digest[32], range[32];
    genesis_mixer_digest(&mixer, digest);
    genesis_mixer_range_digest(&mixer, range);
    printf("MIX frames=%llu clipped=%llu changes=%llu span=%d nonsilent=%d fault=%d dropped=%llu ring=%u sha=", (unsigned long long)mixer.frames,
           (unsigned long long)mixer.clipped, (unsigned long long)mixer.changes, (int)genesis_mixer_span(&mixer), genesis_mixer_non_silent(&mixer),
           (int)mixer.fault, (unsigned long long)mixer.ring_dropped, (unsigned)genesis_mixer_ring_available(&mixer));
    hex(digest);
    printf(" range_frames=%llu range_sha=", (unsigned long long)mixer.range_frames);
    hex(range);
    printf("\n");
  }
  fclose(script);
  return 0;
}
