/* SEG-064-T004 diagnostic: which instruction starts a run executed, per processor, with the execution count and the global order of the
 * first execution (retired instructions of both processors at that time). Off unless the program driver's --coverage flag enables it;
 * nothing here affects the machine. The output is local diagnostic data for comparison with a reference, never durable evidence. */
#include "scd_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ScdCoverageEntry {
  uint32_t key;    /* (cpu << 24) | pc, plus 1 (0 = empty) */
  uint32_t count;
  uint64_t first;
} ScdCoverageEntry;

#define SCD_COVERAGE_SLOTS (1U << 21)

int scd_coverage_enable(ScdMachine *m) {
  m->coverage = calloc(SCD_COVERAGE_SLOTS, sizeof(ScdCoverageEntry));
  return m->coverage == NULL;
}

void scd_coverage_note(ScdMachine *m, uint32_t cpu, uint32_t pc) {
  ScdCoverageEntry *table = (ScdCoverageEntry *)m->coverage;
  const uint32_t key = ((cpu << 24) | (pc & UINT32_C(0x00FFFFFF))) + 1U;
  uint32_t slot = (key * UINT32_C(0x9E3779B1)) >> 11;
  for (;;) {
    ScdCoverageEntry *e = &table[slot & (SCD_COVERAGE_SLOTS - 1U)];
    if (e->key == key) {
      ++e->count;
      return;
    }
    if (e->key == 0U) {
      e->key = key;
      e->count = 1U;
      e->first = m->cpu[0].retired + m->cpu[1].retired;
      return;
    }
    ++slot;
  }
}

static int scd_coverage_compare(const void *a, const void *b) {
  const ScdCoverageEntry *x = (const ScdCoverageEntry *)a, *y = (const ScdCoverageEntry *)b;
  return x->key < y->key ? -1 : x->key > y->key;
}

/* One line per executed start, sorted: "<cpu> <pc hex> <count> <first order>". */
int scd_coverage_write(const ScdMachine *m, const char *path) {
  const ScdCoverageEntry *table = (const ScdCoverageEntry *)m->coverage;
  ScdCoverageEntry *list;
  size_t n = 0U, i;
  FILE *f;
  if (table == NULL) return 0;
  list = (ScdCoverageEntry *)malloc(SCD_COVERAGE_SLOTS * sizeof(ScdCoverageEntry));
  if (list == NULL) return 1;
  for (i = 0U; i < SCD_COVERAGE_SLOTS; ++i)
    if (table[i].key != 0U) list[n++] = table[i];
  qsort(list, n, sizeof *list, scd_coverage_compare);
  f = fopen(path, "w");
  if (f == NULL) {
    free(list);
    return 1;
  }
  for (i = 0U; i < n; ++i)
    fprintf(f, "%u %06X %u %llu\n", (list[i].key - 1U) >> 24, (list[i].key - 1U) & UINT32_C(0x00FFFFFF), list[i].count,
            (unsigned long long)list[i].first);
  free(list);
  return fclose(f) != 0;
}

void scd_coverage_release(ScdMachine *m) {
  free(m->coverage);
  m->coverage = NULL;
}

/* The sub Gate Array access trace: "<R|W><width> <offset hex> <value hex>" with consecutive identical accesses coded as " xN". */
typedef struct ScdTrace {
  FILE *file;
  char last[64];
  unsigned long run;
} ScdTrace;

static void scd_trace_flush(ScdTrace *t) {
  if (t->run != 0UL) fprintf(t->file, "%s x%lu\n", t->last, t->run);
  t->run = 0UL;
}

int scd_trace_open(ScdMachine *m, const char *path) {
  ScdTrace *t = (ScdTrace *)calloc(1U, sizeof *t);
  if (t == NULL) return 1;
  t->file = fopen(path, "w");
  if (t->file == NULL) {
    free(t);
    return 1;
  }
  m->access_trace = t;
  return 0;
}

void scd_trace_ga(ScdMachine *m, uint32_t cpu, uint32_t offset, uint32_t width, int is_write, uint32_t value) {
  ScdTrace *t = (ScdTrace *)m->access_trace;
  char line[64];
  snprintf(line, sizeof line, "%s%c%u %03X %X", cpu == SCD_MAIN ? "M" : "", is_write ? 'W' : 'R', width, offset, width == 1U ? (value & 0xFFU) : (value & 0xFFFFU));
  if (t->run != 0UL && strcmp(line, t->last) == 0) {
    ++t->run;
    return;
  }
  scd_trace_flush(t);
  memcpy(t->last, line, sizeof t->last);
  t->run = 1UL;
}

void scd_trace_close(ScdMachine *m) {
  ScdTrace *t = (ScdTrace *)m->access_trace;
  if (t == NULL) return;
  scd_trace_flush(t);
  fclose(t->file);
  free(t);
  m->access_trace = NULL;
}

/* Event log: "<vertical-blank onsets so far> <event> <detail hex>" per event. */
int scd_event_open(ScdMachine *m, const char *path) {
  m->event_log = fopen(path, "w");
  return m->event_log == NULL;
}

void scd_event(ScdMachine *m, const char *kind, uint32_t detail) {
  fprintf((FILE *)m->event_log, "%llu %s %X\n", (unsigned long long)m->vdp_frames, kind, detail);
}

void scd_event_close(ScdMachine *m) {
  if (m->event_log != NULL) fclose((FILE *)m->event_log);
  m->event_log = NULL;
}
