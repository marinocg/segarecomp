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

/* SEG-064-T005: the distinct control-flow edges (a processor's consecutive instruction starts) - the same relation the private
 * reference records - so the BIOS-service call/return classes of an interval can be compared (written to "<coverage path>.edges"). */
#define SCD_EDGE_SLOTS (1U << 22)
static uint64_t *scd_edges;
static uint32_t scd_edge_count, scd_edge_last[2] = {UINT32_MAX, UINT32_MAX};

int scd_coverage_enable(ScdMachine *m) {
  m->coverage = calloc(SCD_COVERAGE_SLOTS, sizeof(ScdCoverageEntry));
  scd_edges = calloc(SCD_EDGE_SLOTS, sizeof *scd_edges);
  scd_edge_count = 0U;
  scd_edge_last[0] = scd_edge_last[1] = UINT32_MAX;
  return m->coverage == NULL || scd_edges == NULL;
}

void scd_coverage_note(ScdMachine *m, uint32_t cpu, uint32_t pc) {
  ScdCoverageEntry *table = (ScdCoverageEntry *)m->coverage;
  const uint32_t key = ((cpu << 24) | (pc & UINT32_C(0x00FFFFFF))) + 1U;
  uint32_t slot = (key * UINT32_C(0x9E3779B1)) >> 11, probes;
  if (scd_edges != NULL) {
    const uint32_t to = pc & UINT32_C(0x00FFFFFF);
    if (scd_edge_last[cpu] != UINT32_MAX && scd_edge_count < SCD_EDGE_SLOTS / 2U) {
      const uint64_t edge = ((uint64_t)(cpu + 1U) << 56) | ((uint64_t)scd_edge_last[cpu] << 28) | to;
      uint32_t at = (uint32_t)((edge * UINT64_C(0x9E3779B97F4A7C15)) >> 42) & (SCD_EDGE_SLOTS - 1U);
      while (scd_edges[at] != 0U && scd_edges[at] != edge) at = (at + 1U) & (SCD_EDGE_SLOTS - 1U);
      if (scd_edges[at] == 0U) {
        scd_edges[at] = edge;
        ++scd_edge_count;
      }
    }
    scd_edge_last[cpu] = to;
  }
  for (probes = 0U; probes < SCD_COVERAGE_SLOTS; ++probes) { /* bounded: a full table drops the note (diagnostic only) */
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
  if (fclose(f) != 0) return 1;
  if (scd_edges != NULL) {
    char edge_path[1024];
    uint32_t at;
    snprintf(edge_path, sizeof edge_path, "%s.edges", path);
    f = fopen(edge_path, "w");
    if (f == NULL) return 1;
    for (at = 0U; at < SCD_EDGE_SLOTS; ++at)
      if (scd_edges[at] != 0U)
        fprintf(f, "%u %06X %06X\n", (unsigned)(scd_edges[at] >> 56) - 1U, (unsigned)(scd_edges[at] >> 28) & 0xFFFFFFU, (unsigned)(scd_edges[at] & 0xFFFFFFFU));
    return fclose(f) != 0;
  }
  return 0;
}

void scd_coverage_release(ScdMachine *m) {
  free(m->coverage);
  m->coverage = NULL;
  free(scd_edges);
  scd_edges = NULL;
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

/* SEG-079 diagnostic: binary RAM write / BIOS read log (see scd_runtime.h). */
int scd_wlog_open(ScdMachine *m, const char *path) {
  m->write_log = fopen(path, "wb");
  return m->write_log == NULL;
}

void scd_wlog(ScdMachine *m, uint32_t kind, uint32_t cpu, uint32_t offset, uint32_t width, uint32_t value, uint32_t pc) {
  uint8_t r[16];
  uint32_t k;
  r[0] = (uint8_t)kind;
  r[1] = (uint8_t)width;
  r[2] = (uint8_t)cpu;
  r[3] = 0U;
  for (k = 0; k < 4U; ++k) {
    r[4U + k] = (uint8_t)(offset >> (8U * k));
    r[8U + k] = (uint8_t)(value >> (8U * k));
    r[12U + k] = (uint8_t)(pc >> (8U * k));
  }
  (void)fwrite(r, 1U, sizeof r, (FILE *)m->write_log);
}

void scd_wlog_close(ScdMachine *m) {
  if (m->write_log != NULL) fclose((FILE *)m->write_log);
  m->write_log = NULL;
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
