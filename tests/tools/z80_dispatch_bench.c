/* SEG-008-T009: dispatch micro-benchmark linked with a generated image (measurement only, never a test oracle).
 * usage: z80_dispatch_bench <mode> <iterations> <seed>
 *   mode 0: one invariant 64 KiB image (identity 1); mode 1: SMS-shaped map (0x0000-0x03FF identity 1; slots 0-2 map bank
 *   identities 2..33 by (pc >> 14), window-relative owners; 0xC000-0xFFFF non-code).
 * Each iteration seeds a random PC in the code space and calls z80_run with a one-instruction deadline, i.e. a full
 * dispatcher round trip (image query, exact entry lookup, one owner step). The reported ns/step is therefore an upper
 * bound of the exact lookup alone. It also prints the sequential-PC figure. Prints "bench mode= random_ns= sequential_ns=".
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef Z80_BENCH_MAIN_TU /* exact-lookup build: include the generated main TU so its static z80_entry_lookup is callable */
#include Z80_BENCH_MAIN_TU
#else
#include "segarecomp/codegen/c11/runtime/z80_runtime.h"
#endif

static int bench_mode;
static uint8_t mem[65536];
static uint8_t host_read(void *c, uint16_t a, uint64_t t) { (void)c; (void)t; return mem[a]; }
static void host_write(void *c, uint16_t a, uint8_t v, uint64_t t) { (void)c; (void)t; mem[a] = v; }
static uint8_t host_in(void *c, uint16_t p, uint64_t t) { (void)c; (void)p; (void)t; return 0xFF; }
static void host_out(void *c, uint16_t p, uint8_t v, uint64_t t) { (void)c; (void)p; (void)v; (void)t; }
static uint8_t host_ack(void *c, uint64_t t) { (void)c; (void)t; return 0xFF; }
static int host_image(void *c, uint16_t a, Z80CodeImage *image) {
  (void)c;
  if (bench_mode == 0) { image->identity = 1; image->window_base = 0; return 1; }
  if (a < 0x0400u) { image->identity = 1; image->window_base = 0; return 1; }
  if (a >= 0xC000u) return 0;
  image->identity = 2u + (((unsigned)a * 2654435761u) >> 27);  /* a pseudo-random bank of 32 */
  image->window_base = (uint16_t)(a & 0xC000u);
  return 1;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e9 + (double)t.tv_nsec; }

#ifdef Z80_BENCH_MAIN_TU
/* Exact lookup alone: random keys, then keys in ascending order (sequential). */
static unsigned lookup_key(int mode, unsigned r) {
  if (mode == 0) return (1u << 16) | (r & 0xFFFFu);
  if ((r & 0xFu) == 0) return (1u << 16) | ((r >> 4) & 0x3FFu);
  return ((2u + ((r >> 4) & 31u)) << 16) | ((r >> 9) & 0x3FFFu);
}
static int bench_lookup(int mode, long iterations, unsigned seed) {
  srand(seed);
  unsigned *keys = malloc(sizeof(unsigned) * (size_t)iterations);
  for (long i = 0; i < iterations; ++i) keys[i] = lookup_key(mode, ((unsigned)rand() << 8) ^ (unsigned)rand());
  double result[2];
  for (int pass = 0; pass < 2; ++pass) {
    uintptr_t sink = 0;
    const double start = now();
    for (long i = 0; i < iterations; ++i) {
      const unsigned key = pass == 0 ? keys[i] : (mode == 0 ? (1u << 16) | ((unsigned)i & 0xFFFFu) : lookup_key(mode, (unsigned)i * 16u));
      sink += (uintptr_t)z80_entry_lookup(key);
    }
    result[pass] = (now() - start) / (double)iterations;
    if (sink == 1) puts("");
  }
  printf("bench mode=%d random_ns=%.1f sequential_ns=%.1f exact_lookup=1\n", mode, result[0], result[1]);
  free(keys);
  return 0;
}
#endif

int main(int argc, char **argv) {
  if (argc < 4) return 2;
#ifdef Z80_BENCH_MAIN_TU
  return bench_lookup(atoi(argv[1]), atol(argv[2]), (unsigned)atoi(argv[3]));
#endif
  bench_mode = atoi(argv[1]);
  const long iterations = atol(argv[2]);
  srand((unsigned)atoi(argv[3]));
  static Z80Runtime rt;
  rt.host = (Z80Host){NULL, host_read, host_write, host_in, host_out, host_ack, host_image, NULL};
  const unsigned limit = bench_mode == 0 ? 0x10000u : 0xC000u;
  unsigned *pcs = malloc(sizeof(unsigned) * (size_t)iterations);
  for (long i = 0; i < iterations; ++i) pcs[i] = (unsigned)(((unsigned)rand() << 8 ^ (unsigned)rand()) % limit);
  double result[2];
  for (int pass = 0; pass < 2; ++pass) {
    unsigned checksum = 0;
    const double start = now();
    for (long i = 0; i < iterations; ++i) {
      rt.state.pc = (uint16_t)(pass == 0 ? pcs[i] : (unsigned)i % limit);
      rt.state.halted = 0; rt.state.int_deferral = 0; rt.state.in_prefix_run = 0; rt.state.int_line = 0;
      checksum += z80_run(&rt, rt.state.cycles + 1u);
    }
    result[pass] = (now() - start) / (double)iterations;
    if (checksum == 0xFFFFFFFFu) puts("");
  }
  printf("bench mode=%d random_ns=%.1f sequential_ns=%.1f\n", bench_mode, result[0], result[1]);
  free(pcs);
  return 0;
}
