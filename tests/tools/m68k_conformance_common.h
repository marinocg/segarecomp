/* SEG-021-T003: memory model, vector parsing and result printing shared by the generated-native runner and
 * the pinned-Musashi oracle runner of the conformance harness, so both sides start from bit-identical state
 * and print in the SEG-020 boundary schema (tools/m68k_first_divergence.py).
 *
 * Vector line (space separated, produced by tools/m68k_conformance.py):
 *   ID CODEHEX SR USP D0..D7 A0..A7 NMEM ADDR:HEXBYTES...        (all numbers hexadecimal)
 * Common initial memory: 1 MiB at address 0 = deterministic byte pattern, vector table entries 2..31
 * pointing at distinct handler addresses, the instruction bytes at CF_CODE_BASE, then the per-vector seeds.
 * Result: one JSON object per vector; `effects` are byte-granular memory differences against the initial
 * image (a write of an unchanged value is intentionally invisible) plus a k=2 exception entry when the
 * final PC is a vector handler address. */
#ifndef M68K_CONFORMANCE_COMMON_H
#define M68K_CONFORMANCE_COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CF_MEM_SIZE 0x100000U
#define CF_CODE_BASE 0x2000U
#define CF_HANDLER(v) (0x8000U + (unsigned)(v) * 0x20U)
#define CF_MAX_EFFECTS 256U
#define CF_MAX_MEM 8U

typedef struct { unsigned addr, len; uint8_t bytes[16]; } cf_seed;
typedef struct {
  char id[128], code[64];
  unsigned sr, usp, d[8], a[8], nmem;
  cf_seed mem[CF_MAX_MEM];
} cf_vector;

static int cf_parse(char *line, cf_vector *v) {
  char *tok[64]; unsigned n = 0, i;
  for (char *p = strtok(line, " \t\r\n"); p && n < 64U; p = strtok(NULL, " \t\r\n")) tok[n++] = p;
  if (n < 21U) return 0;
  if (strlen(tok[0]) >= sizeof v->id || strlen(tok[1]) >= sizeof v->code) return 0;
  strcpy(v->id, tok[0]); strcpy(v->code, tok[1]);
  v->sr = (unsigned)strtoul(tok[2], NULL, 16); v->usp = (unsigned)strtoul(tok[3], NULL, 16);
  for (i = 0; i < 8U; ++i) v->d[i] = (unsigned)strtoul(tok[4 + i], NULL, 16);
  for (i = 0; i < 8U; ++i) v->a[i] = (unsigned)strtoul(tok[12 + i], NULL, 16);
  v->nmem = (unsigned)strtoul(tok[20], NULL, 10);
  if (v->nmem > CF_MAX_MEM || n != 21U + v->nmem) return 0;
  for (i = 0; i < v->nmem; ++i) {
    char *colon = strchr(tok[21 + i], ':'); size_t k, digits;
    if (!colon) return 0;
    *colon = 0; v->mem[i].addr = (unsigned)strtoul(tok[21 + i], NULL, 16);
    digits = strlen(colon + 1); if (digits == 0U || digits % 2U || digits / 2U > 16U) return 0;
    v->mem[i].len = (unsigned)(digits / 2U);
    for (k = 0; k < digits / 2U; ++k) { char h[3] = {colon[1 + 2 * k], colon[2 + 2 * k], 0}; v->mem[i].bytes[k] = (uint8_t)strtoul(h, NULL, 16); }
    if ((uint64_t)v->mem[i].addr + v->mem[i].len > CF_MEM_SIZE) return 0;
  }
  return 1;
}

static int cf_code_bytes(const char *hex, uint8_t *out, unsigned *len) {
  size_t n = strlen(hex), i;
  if (n == 0U || n % 4U || n / 2U > 16U) return 0;
  for (i = 0; i < n / 2U; ++i) { char h[3] = {hex[2 * i], hex[2 * i + 1], 0}; out[i] = (uint8_t)strtoul(h, NULL, 16); }
  *len = (unsigned)(n / 2U); return 1;
}

static void cf_init_memory(uint8_t *ram, const cf_vector *v) {
  static uint8_t base[CF_MEM_SIZE]; static int ready;
  unsigned i; uint8_t code[16]; unsigned len = 0;
  if (!ready) { /* the pattern + vector table are built once; every vector starts from a copy */
    for (i = 0; i < CF_MEM_SIZE; ++i) base[i] = (uint8_t)((i * 7U + 3U) & 0xFFU);
    for (i = 2; i < 32U; ++i) { unsigned h = CF_HANDLER(i); base[i * 4U] = (uint8_t)(h >> 24); base[i * 4U + 1U] = (uint8_t)(h >> 16); base[i * 4U + 2U] = (uint8_t)(h >> 8); base[i * 4U + 3U] = (uint8_t)h; }
    ready = 1;
  }
  memcpy(ram, base, CF_MEM_SIZE);
  if (cf_code_bytes(v->code, code, &len)) memcpy(ram + CF_CODE_BASE, code, len);
  for (i = 0; i < v->nmem; ++i) memcpy(ram + v->mem[i].addr, v->mem[i].bytes, v->mem[i].len);
}

static void cf_print(const cf_vector *v, const uint8_t *before, const uint8_t *after, unsigned pc, unsigned sr,
                     unsigned usp, const unsigned *d, const unsigned *a) {
  unsigned k, count = 0, overflow = 0, vec = 0; size_t block, i;
  printf("{\"id\":\"%s\",\"boundary\":1,\"pc\":%u,\"sr\":%u,\"usp\":%u,\"d\":[", v->id, pc, sr & 0xFFFFU, usp);
  for (k = 0; k < 8U; ++k) printf("%s%u", k ? "," : "", d[k]);
  printf("],\"a\":[");
  for (k = 0; k < 8U; ++k) printf("%s%u", k ? "," : "", a[k]);
  printf("],\"effects\":[");
  for (block = 0; block < CF_MEM_SIZE; block += 4096U) {
    if (memcmp(before + block, after + block, 4096U) == 0) continue;
    for (i = block; i < block + 4096U; ++i) {
      if (before[i] == after[i]) continue;
      if (count >= CF_MAX_EFFECTS) { overflow = 1; continue; }
      printf("%s{\"k\":1,\"w\":1,\"a\":%zu,\"v\":%u}", count ? "," : "", i, after[i]); ++count;
    }
  }
  for (k = 2; k < 32U; ++k) if (pc == CF_HANDLER(k)) vec = k;
  if (vec) printf("%s{\"k\":2,\"w\":0,\"a\":%u,\"v\":%u}", count ? "," : "", pc, vec);
  printf("],\"unsupported\":%u}\n", overflow);
}
#endif
