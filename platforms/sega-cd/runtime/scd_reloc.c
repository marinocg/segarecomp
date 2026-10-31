/* SEG-111 (ADR 0255): the build pass's bounded RAM write-stream observer.
 *
 * Attached only for a materialization pass (--materialize-dir); the final program, ordinary runs and every other tool never attach it, so
 * their only cost is a null-pointer test on the bus entry points. It is a pure observer: it reads the arguments of accesses that already
 * happened, never touches guest state, timing or ordering, and has no effect on the program identity.
 *
 * It records at most SCD_RELOC_STREAMS ascending, contiguous write streams (one writer processor and program counter, one RAM) of at most
 * SCD_RELOC_MAX_BYTES bytes each, and states for every stream the facts the build driver needs to explain a RAM executable frontier:
 *   - whether each written value was the value the SAME instruction instance read (a memory-to-memory move): the pair is identified by
 *     (processor, retired-instruction count), which is constant across the bus cycles of one instruction and different for every loop
 *     iteration, so a repeated writer PC is never mistaken for one instance;
 *   - the contiguous source range of such a paired stream;
 *   - whether any later write (bus, write-protect drop or exception frame) touched its bytes, whether a device register was read while it
 *     was being produced, whether it exceeded the bound, and how many streams the bounded table had to evict.
 * Nothing here is a claim: the driver decides, with the validated disc reader, whether the evidence proves a copy of delivered bytes. */
#include "scd_runtime.h"

#include <stdlib.h>
#include <string.h>

#define SCD_RELOC_STREAMS 8U
#define SCD_RELOC_MAX_BYTES 4096U

typedef struct ScdRelocStream {
  uint8_t used, writer_cpu, memory;           /* memory: SCD_MEMORY_WORK or SCD_MEMORY_PRG */
  uint8_t unpaired, source_gap, clobbered, dropped, too_large, volatile_input;
  uint8_t source_class;                        /* SCD_RELOC_CLASS_* of the first paired read */
  uint32_t destination;                        /* RAM offset of the first byte */
  uint32_t length;                             /* bytes written (counted past the bound) */
  uint32_t pc;                                 /* writer instruction address */
  uint32_t source, source_next;                /* paired: normalised address (writer's space) of the first byte / of the next expected one */
  uint64_t order;                              /* recency of the last write */
  uint64_t device_reads;                       /* device reads by the writer when the last write happened */
  uint8_t bytes[SCD_RELOC_MAX_BYTES];
} ScdRelocStream;

typedef struct ScdRelocRead {
  uint64_t instance;
  uint32_t count, address, width, value, klass;
  uint8_t valid;
} ScdRelocRead;

typedef struct ScdReloc {
  ScdRelocStream streams[SCD_RELOC_STREAMS];
  ScdRelocRead pending[2];
  uint64_t device_reads[2];
  uint64_t order, evicted, writes;
} ScdReloc;

int scd_reloc_attach(ScdMachine *m) {
  m->reloc = calloc(1U, sizeof(ScdReloc));
  return m->reloc == NULL;
}

void scd_reloc_release(ScdMachine *m) {
  free(m->reloc);
  m->reloc = NULL;
}

void scd_reloc_note_read(ScdMachine *m, uint32_t cpu, uint32_t klass, uint32_t address, uint32_t width, uint32_t value) {
  ScdReloc *r = m->reloc;
  ScdRelocRead *p;
  const uint64_t instance = m->cpu[cpu].retired;
  if (klass == SCD_RELOC_CLASS_DEVICE) ++r->device_reads[cpu];
  p = &r->pending[cpu];
  if (p->valid && p->instance == instance) {
    if (p->address == address && p->width == width) { /* the same read repeated (a retried instruction): not a second read */
      p->value = value;
      return;
    }
    ++p->count; /* several data reads in one instruction: never a pair */
    return;
  }
  p->valid = 1U;
  p->instance = instance;
  p->count = 1U;
  p->address = address;
  p->width = width;
  p->value = value;
  p->klass = klass;
}

static int scd_reloc_overlaps(const ScdRelocStream *s, uint32_t memory, uint32_t offset, uint32_t length) {
  return s->used && s->memory == memory && offset < s->destination + s->length && s->destination < offset + length;
}

void scd_reloc_note_foreign(ScdMachine *m, uint32_t memory, uint32_t offset, uint32_t length) {
  ScdReloc *r = m->reloc;
  uint32_t i;
  for (i = 0; i < SCD_RELOC_STREAMS; ++i)
    if (scd_reloc_overlaps(&r->streams[i], memory, offset, length)) r->streams[i].clobbered = 1U;
}

void scd_reloc_note_write(ScdMachine *m, uint32_t cpu, uint32_t memory, uint32_t offset, uint32_t destination_address, uint32_t width,
                          uint32_t value, int dropped, uint32_t pc) {
  ScdReloc *r = m->reloc;
  const ScdRelocRead *p = &r->pending[cpu];
  ScdRelocStream *target = NULL, *slot = NULL;
  uint32_t i, k;
  int paired;
  const uint32_t same_ram_class = memory == SCD_MEMORY_WORK ? SCD_RELOC_CLASS_WORK : SCD_RELOC_CLASS_PRG;
  ++r->writes;
  /* a pair: the one data read of this very instruction, same width and value, from a cell that is not the written one */
  paired = p->valid && p->instance == m->cpu[cpu].retired && p->count == 1U && p->width == width && p->value == value &&
           p->klass != SCD_RELOC_CLASS_DEVICE &&
           !(p->klass == same_ram_class && p->address < destination_address + width && destination_address < p->address + width);
  for (i = 0; i < SCD_RELOC_STREAMS; ++i) {
    ScdRelocStream *s = &r->streams[i];
    if (s->used && s->writer_cpu == cpu && s->memory == memory && s->pc == pc && s->destination + s->length == offset) {
      target = s;
      break;
    }
  }
  for (i = 0; i < SCD_RELOC_STREAMS; ++i) /* every other stream that this write touches is no longer the owner of those bytes */
    if (&r->streams[i] != target && scd_reloc_overlaps(&r->streams[i], memory, offset, width)) r->streams[i].clobbered = 1U;
  if (target == NULL) {
    for (i = 0; i < SCD_RELOC_STREAMS && slot == NULL; ++i)
      if (!r->streams[i].used) slot = &r->streams[i];
    for (i = 0; i < SCD_RELOC_STREAMS && slot == NULL; ++i)
      if (r->streams[i].clobbered) slot = &r->streams[i];
    if (slot == NULL) {
      slot = &r->streams[0];
      for (i = 1; i < SCD_RELOC_STREAMS; ++i)
        if (r->streams[i].order < slot->order) slot = &r->streams[i];
      ++r->evicted;
    }
    memset(slot, 0, sizeof *slot);
    slot->used = 1U;
    slot->writer_cpu = (uint8_t)cpu;
    slot->memory = (uint8_t)memory;
    slot->destination = offset;
    slot->pc = pc;
    slot->device_reads = r->device_reads[cpu];
    if (paired) {
      slot->source = slot->source_next = p->address;
      slot->source_class = (uint8_t)p->klass;
    } else {
      slot->unpaired = 1U;
    }
    target = slot;
  }
  if (!paired) target->unpaired = 1U;
  else if (!target->unpaired && target->source_next != p->address) target->source_gap = 1U;
  else if (paired) target->source_next = p->address + width;
  if (dropped) target->dropped = 1U;
  if (target->device_reads != r->device_reads[cpu]) {
    target->volatile_input = 1U;
    target->device_reads = r->device_reads[cpu];
  }
  for (k = 0; k < width; ++k) {
    if (target->length + k < SCD_RELOC_MAX_BYTES) target->bytes[target->length + k] = (uint8_t)(value >> (8U * (width - 1U - k)));
    else target->too_large = 1U;
  }
  target->length += width;
  target->order = ++r->order;
}

static const char *scd_reloc_memory_name(uint32_t memory) { return memory == SCD_MEMORY_WORK ? "work" : "prg"; }

/* The versioned `streams` member of the frontier capture. Local ignored build artifact; the driver validates the version and every field. */
void scd_reloc_report(const ScdMachine *m, FILE *out) {
  const ScdReloc *r = m->reloc;
  uint32_t i, first = 1U;
  fprintf(out, ",\"streams\":{\"version\":1,\"slots\":%u,\"max_bytes\":%u,\"writes\":%llu,\"evicted\":%llu,\"items\":[", SCD_RELOC_STREAMS,
          SCD_RELOC_MAX_BYTES, (unsigned long long)r->writes, (unsigned long long)r->evicted);
  for (i = 0; i < SCD_RELOC_STREAMS; ++i) {
    const ScdRelocStream *s = &r->streams[i];
    uint32_t stored, k;
    if (!s->used) continue;
    stored = s->length < SCD_RELOC_MAX_BYTES ? s->length : SCD_RELOC_MAX_BYTES;
    fprintf(out, "%s{\"writer_cpu\":%u,\"memory\":\"%s\",\"destination\":%u,\"length\":%u,\"pc\":%u,\"paired\":%u,\"source\":%u,\"source_class\":%u,"
                 "\"source_gap\":%u,\"clobbered\":%u,\"dropped\":%u,\"too_large\":%u,\"volatile\":%u,\"bytes\":\"",
            first ? "" : ",", s->writer_cpu, scd_reloc_memory_name(s->memory), s->destination, s->length, s->pc, s->unpaired ? 0U : 1U,
            s->source, s->source_class, s->source_gap, s->clobbered, s->dropped, s->too_large, s->volatile_input);
    for (k = 0; k < stored; ++k) fprintf(out, "%02x", s->bytes[k]);
    fprintf(out, "\"}");
    first = 0U;
  }
  fprintf(out, "]}");
}
