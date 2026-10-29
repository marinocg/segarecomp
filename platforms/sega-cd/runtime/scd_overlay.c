/* SEG-063-T001 (ADR 0127, ADR 0132): runtime activation of overlapping precompiled M68K images.
 *
 * The generated program carries a finite registry: per processor instance, groups of candidates (one mapping of one image each)
 * whose execution ranges overlap. A PC inside a group is never dispatched by PC alone. The selection key is (processor instance,
 * PC, live bytes): the candidates are the group's members covering PC, and a candidate is live only when every byte of its whole
 * execution range equals its known image (compared through 64-byte granule digests computed from live memory; nothing is decoded).
 * Exactly one live covering candidate, with no other live candidate overlapping it, is selected; its own dispatcher then runs the
 * compiled instruction, whose byte guard still compares that instruction's bytes exactly.
 *
 * A selection stays valid until a write (CPU store, CDC DMA, exception frame) dirties a granule of the group, or the processor's
 * view of the range changes (PRG-RAM bank or window, Word RAM owner or mode); it is then revalidated before the next instruction in
 * the group, at the cost of the dirty granules only. A selection that fails revalidation leaves the group stale: until a new
 * selection, the group may only be re-entered at a declared entry root of a live candidate - a return into, or a fall through
 * from, an image that was replaced underneath is the typed stop stale_image_selection, never execution of the replacement's code.
 *
 * Typed stops (detail = the candidate's image id, or 0): image_unknown, image_bytes_mismatch, image_partially_overwritten,
 * ambiguous_image, image_entry_not_compiled, stale_image_selection. A revalidation that keeps the selection also checks every
 * other overlapping candidate the dirty granules touch, so a write that makes a second image live is ambiguous_image. Cost bounds:
 * O(written bytes x groups) to mark, O(dirty granules) to revalidate (plus O(candidate bytes) for an overlapping candidate whose
 * dirty granules all match), O(group bytes) for a fresh selection (bounded by SCD_OVERLAY_MAX_CANDIDATES candidates). */
#include "scd_runtime.h"

#include <string.h>

#include "runtime.h"

#define SCD_GRANULE_BYTES (1U << SCD_OVERLAY_GRANULE_SHIFT)

static void scd_overlay_event(ScdMachine *m, uint32_t cpu, uint32_t group, int32_t candidate, uint32_t pc, uint32_t kind) {
  if (m->overlay_events < SCD_OVERLAY_LOG) {
    ScdOverlayEvent *e = &m->overlay_log[m->overlay_events];
    e->tick = m->now;
    e->cpu = cpu;
    e->group = group;
    e->candidate = candidate < 0 ? UINT32_MAX : (uint32_t)candidate;
    e->pc = pc;
    e->kind = kind;
  }
  m->overlay_events += 1U;
  scd_log(m, 0xA0U + kind, pc, (group << 8) | (candidate < 0 ? 0xFFU : (uint32_t)candidate));
}

static void scd_clear_dirty(ScdOverlayState *s) {
  if (s->any_dirty && !s->all_dirty) {
    uint32_t i;
    for (i = s->dirty_lo >> 3; i <= (s->dirty_hi >> 3); ++i) s->dirty[i] = 0U;
  } else if (s->all_dirty) {
    memset(s->dirty, 0, sizeof s->dirty);
  }
  s->any_dirty = 0U;
  s->all_dirty = 0U;
  s->dirty_lo = UINT32_MAX;
  s->dirty_hi = 0U;
}

void scd_overlay_attach(ScdMachine *m, const ScdOverlayProgram *overlays) {
  uint32_t g;
  m->overlays = overlays;
  m->overlay_events = 0U;
  for (g = 0; g < SCD_OVERLAY_MAX_GROUPS; ++g) {
    ScdOverlayState *s = &m->overlay[g];
    memset(s, 0, sizeof *s);
    s->selected = -1;
    s->stale_from = -1;
    s->dirty_lo = UINT32_MAX;
  }
}

/* The (canonical) address at which processor `cpu` currently sees byte `offset` of `memory`; *visible = 0 when it does not see it. */
uint32_t scd_overlay_view_of(const ScdMachine *m, uint32_t cpu, uint32_t memory, uint32_t offset, int *visible) {
  *visible = 0;
  if (memory == SCD_MEMORY_PRG) {
    if (cpu == SCD_SUB) {
      *visible = 1;
      return offset;
    }
    if (m->ga.prg_window && (offset >> 17) == m->ga.prg_bank) {
      *visible = 1;
      return UINT32_C(0x20000) + (offset & UINT32_C(0x1FFFF));
    }
    return 0U;
  }
  if (memory == SCD_MEMORY_WORK) {
    if (cpu != SCD_MAIN) return 0U;
    *visible = 1;
    return UINT32_C(0xFF0000) + offset;
  }
  if (!(m->ga.mode & SCD_MODE_1M)) {
    const int main_owns = (m->ga.mode & SCD_MODE_RET) != 0;
    if ((cpu == SCD_MAIN) != main_owns) return 0U;
    *visible = 1;
    return (cpu == SCD_MAIN ? UINT32_C(0x200000) : UINT32_C(0x80000)) + offset;
  }
  {
    const uint32_t bank = (offset >> 1) & 1U;
    const uint32_t main_bank = (m->ga.mode & SCD_MODE_RET) ? 1U : 0U;
    const uint32_t local = ((offset >> 2) << 1) | (offset & 1U);
    if ((cpu == SCD_MAIN) != (bank == main_bank)) return 0U;
    *visible = 1;
    return (cpu == SCD_MAIN ? UINT32_C(0x200000) : UINT32_C(0xC0000)) + local;
  }
}

void scd_overlay_note_write(ScdMachine *m, uint32_t memory, uint32_t offset, uint32_t length) {
  const ScdOverlayProgram *o = m->overlays;
  uint32_t g, i;
  if (o == NULL) return;
  for (g = 0; g < o->group_count; ++g) {
    const ScdOverlayGroup *group = &o->groups[g];
    ScdOverlayState *s = &m->overlay[g];
    if (s->all_dirty) continue;
    for (i = 0; i < length; ++i) {
      int visible;
      const uint32_t address = scd_overlay_view_of(m, group->cpu, memory, offset + i, &visible);
      if (!visible || address < group->start || address >= group->end) continue;
      {
        const uint32_t bit = (address >> SCD_OVERLAY_GRANULE_SHIFT) - group->granule_first;
        s->dirty[bit >> 3] = (uint8_t)(s->dirty[bit >> 3] | (1U << (bit & 7U)));
        s->any_dirty = 1U;
        if (bit < s->dirty_lo) s->dirty_lo = bit;
        if (bit > s->dirty_hi) s->dirty_hi = bit;
      }
    }
  }
}

void scd_overlay_note_view_change(ScdMachine *m) {
  uint32_t g;
  if (m->program != NULL && m->program->dynamic != NULL) scd_dynamic_note_view_change(m); /* SEG-100 */
  if (m->overlays == NULL) return;
  for (g = 0; g < m->overlays->group_count; ++g) {
    m->overlay[g].all_dirty = 1U;
    m->overlay[g].any_dirty = 1U;
  }
}

/* SHA-256 of the live bytes of [lo, hi) as `cpu` sees them, first 8 bytes big-endian; 0 when a byte is not RAM of `cpu`. */
static int scd_live_digest(const ScdMachine *m, uint32_t cpu, uint32_t lo, uint32_t hi, uint64_t *out) {
  uint8_t bytes[SCD_GRANULE_BYTES], digest[32];
  GenesisSha256 sha;
  uint32_t i;
  for (i = 0; lo + i < hi; ++i) {
    const uint8_t *live = scd_code_byte(m, cpu, lo + i);
    if (live == NULL) return 0;
    bytes[i] = *live;
  }
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, bytes, hi - lo);
  genesis_sha256_final(&sha, digest);
  *out = 0U;
  for (i = 0; i < 8U; ++i) *out = (*out << 8) | digest[i];
  return 1;
}

static uint32_t scd_end(const ScdOverlayCandidate *c) { return c->start + c->length; }

/* The candidate's byte range inside absolute granule `granule` ([lo, hi) empty when the granule is outside it). */
static void scd_granule_range(const ScdOverlayCandidate *c, uint32_t granule, uint32_t *lo, uint32_t *hi) {
  const uint32_t g_lo = granule << SCD_OVERLAY_GRANULE_SHIFT, g_hi = g_lo + SCD_GRANULE_BYTES;
  *lo = c->start > g_lo ? c->start : g_lo;
  *hi = scd_end(c) < g_hi ? scd_end(c) : g_hi;
}

/* 1 when the live bytes of the candidate's part of `granule` equal its image. */
static int scd_granule_live(const ScdMachine *m, uint32_t cpu, const ScdOverlayCandidate *c, uint32_t granule) {
  uint32_t lo, hi;
  uint64_t live;
  if (granule < c->granule_first || granule >= c->granule_first + c->granule_count) return 1;
  scd_granule_range(c, granule, &lo, &hi);
  return scd_live_digest(m, cpu, lo, hi, &live) && live == c->granule_digests[granule - c->granule_first];
}

static int scd_candidate_live(const ScdMachine *m, uint32_t cpu, const ScdOverlayCandidate *c) {
  uint32_t g;
  for (g = 0; g < c->granule_count; ++g)
    if (!scd_granule_live(m, cpu, c, c->granule_first + g)) return 0;
  return 1;
}

static int scd_covers(const ScdOverlayCandidate *c, uint32_t pc) { return pc >= c->start && pc < scd_end(c); }

static int scd_overlaps(const ScdOverlayCandidate *a, const ScdOverlayCandidate *b) {
  return a->start < scd_end(b) && b->start < scd_end(a);
}

static int32_t scd_find(const uint32_t *sorted, uint32_t count, uint32_t value) {
  uint32_t lo = 0U, hi = count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2U;
    if (sorted[mid] == value) return (int32_t)mid;
    if (sorted[mid] < value) lo = mid + 1U;
    else hi = mid;
  }
  return -1;
}

/* 1 when the candidate's compiled instruction at pc has the live bytes (comparison of its verification bytes only). */
static int scd_instruction_live(const ScdMachine *m, uint32_t cpu, const ScdOverlayCandidate *c, uint32_t pc) {
  const int32_t e = scd_find(c->entries, c->entry_count, pc);
  uint32_t i;
  if (e < 0 || c->entry_bytes[e] == NULL) return 0;
  for (i = 0; i < c->entry_lengths[e]; ++i) {
    const uint8_t *live = scd_code_byte(m, cpu, pc + i);
    if (live == NULL || *live != c->entry_bytes[e][i]) return 0;
  }
  return 1;
}

/* No candidate covering pc is live: classify. The instruction at pc is no candidate's -> image_unknown. Otherwise take the first
 * candidate (registry order; diagnostic only) whose instruction at pc matches: when every mismatching granule of it is explained by
 * another candidate of the group over exactly the same byte range, the live range is a mixture of admitted images ->
 * image_partially_overwritten; otherwise the known image holds bytes no admitted image explains -> image_bytes_mismatch. */
static int scd_overlay_classify(ScdMachine *m, uint32_t cpu, const ScdOverlayGroup *group, uint32_t pc) {
  uint32_t k, g, j;
  for (k = 0; k < group->candidate_count; ++k) {
    const ScdOverlayCandidate *c = &group->candidates[k];
    int explained = 1;
    if (!scd_covers(c, pc) || !scd_instruction_live(m, cpu, c, pc)) continue;
    for (g = c->granule_first; g < c->granule_first + c->granule_count && explained; ++g) {
      uint32_t lo, hi;
      int other = 0;
      if (scd_granule_live(m, cpu, c, g)) continue;
      scd_granule_range(c, g, &lo, &hi);
      for (j = 0; j < group->candidate_count && !other; ++j) {
        const ScdOverlayCandidate *d = &group->candidates[j];
        uint32_t dlo, dhi;
        if (j == k || g < d->granule_first || g >= d->granule_first + d->granule_count) continue;
        scd_granule_range(d, g, &dlo, &dhi);
        other = dlo == lo && dhi == hi && scd_granule_live(m, cpu, d, g);
      }
      explained = other;
    }
    return scd_fail(m, cpu, explained ? SCD_STOP_IMAGE_PARTIALLY_OVERWRITTEN : SCD_STOP_IMAGE_BYTES_MISMATCH, pc, c->image_id);
  }
  return scd_fail(m, cpu, SCD_STOP_IMAGE_UNKNOWN, pc, 0U);
}

/* 1 when a dirty granule of the group (any granule after a view change) lies inside candidate `c`'s range. */
static int scd_dirty_touches(const ScdOverlayGroup *group, const ScdOverlayState *s, const ScdOverlayCandidate *c) {
  uint32_t g;
  if (s->all_dirty) return 1;
  for (g = c->granule_first; g < c->granule_first + c->granule_count; ++g) {
    const uint32_t bit = g - group->granule_first;
    if (bit >= s->dirty_lo && bit <= s->dirty_hi && ((s->dirty[bit >> 3] >> (bit & 7U)) & 1U)) return 1;
  }
  return 0;
}

/* 1 when every dirty granule of `c` matches (all granules after a view change): the cheap test before a full verification. */
static int scd_dirty_granules_live(const ScdMachine *m, const ScdOverlayGroup *group, const ScdOverlayState *s,
                                   const ScdOverlayCandidate *c) {
  uint32_t g;
  if (s->all_dirty) return scd_candidate_live(m, group->cpu, c);
  for (g = c->granule_first; g < c->granule_first + c->granule_count; ++g) {
    const uint32_t bit = g - group->granule_first;
    if (bit >= s->dirty_lo && bit <= s->dirty_hi && ((s->dirty[bit >> 3] >> (bit & 7U)) & 1U) &&
        !scd_granule_live(m, group->cpu, c, g))
      return 0;
  }
  return 1;
}

/* Revalidate the selection over the dirty granules only (all of them after a view change). Returns 1 when the selection stays valid
 * and unambiguous, 0 when it no longer matches, -1 when it matches but the writes made another candidate overlapping it live
 * (ADR 0132 point 4; *other = that candidate). Only candidates the dirty granules touch are examined, and a candidate is verified
 * in full only when all of its dirty granules already match. */
static int scd_overlay_revalidate(const ScdMachine *m, const ScdOverlayGroup *group, const ScdOverlayState *s, int32_t *other) {
  const ScdOverlayCandidate *c = &group->candidates[s->selected];
  uint32_t bit, k;
  if (s->all_dirty) {
    if (!scd_candidate_live(m, group->cpu, c)) return 0;
  } else {
    for (bit = s->dirty_lo; bit <= s->dirty_hi; ++bit)
      if (((s->dirty[bit >> 3] >> (bit & 7U)) & 1U) && !scd_granule_live(m, group->cpu, c, group->granule_first + bit)) return 0;
  }
  for (k = 0; k < group->candidate_count; ++k) {
    const ScdOverlayCandidate *d = &group->candidates[k];
    if ((int32_t)k == s->selected || !scd_overlaps(d, c) || !scd_dirty_touches(group, s, d)) continue;
    if (scd_dirty_granules_live(m, group, s, d) && scd_candidate_live(m, group->cpu, d)) {
      *other = (int32_t)k;
      return -1;
    }
  }
  return 1;
}

static int scd_overlay_run(ScdMachine *m, uint32_t cpu, uint32_t g, const ScdOverlayCandidate *c, int32_t index, uint32_t pc) {
  const int result = c->dispatch(m);
  if (result == SCD_IMAGE_NO_ENTRY) return scd_fail(m, cpu, SCD_STOP_IMAGE_ENTRY_NOT_COMPILED, pc, c->image_id);
  if (result == 0) m->overlay[g].retired[index] += 1U;
  return result;
}

int scd_overlay_dispatch(ScdMachine *m, uint32_t cpu) {
  const ScdOverlayProgram *o = m->overlays;
  const uint32_t pc = m->cpu[cpu].pc;
  const ScdOverlayGroup *group = NULL;
  ScdOverlayState *s;
  uint32_t g, k;
  int32_t chosen = -1;
  uint32_t live_covering = 0U;
  uint8_t live[SCD_OVERLAY_MAX_CANDIDATES];
  if (o != NULL)
    for (g = 0; g < o->group_count; ++g)
      if (o->groups[g].cpu == cpu && pc >= o->groups[g].start && pc < o->groups[g].end) {
        group = &o->groups[g];
        break;
      }
  if (group == NULL) return scd_fail(m, cpu, SCD_STOP_ENTRY_NOT_COMPILED, pc, pc); /* the SEG-062 stop */
  s = &m->overlay[g];
  if (scd_cd_observe(m) != 0) return 1; /* bring an in-flight CDC DMA up to this instruction before comparing Word RAM */
  if (s->selected >= 0 && s->any_dirty) {
    int32_t other = -1;
    int valid;
    s->revalidations += 1U;
    valid = scd_overlay_revalidate(m, group, s, &other);
    if (valid < 0) return scd_fail(m, cpu, SCD_STOP_AMBIGUOUS_IMAGE, pc, group->candidates[other].image_id);
    if (!valid) {
      s->invalidations += 1U;
      scd_overlay_event(m, cpu, g, s->selected, pc, 2U);
      s->stale_from = s->selected;
      s->selected = -1;
    }
    scd_clear_dirty(s);
  }
  if (s->selected >= 0 && scd_covers(&group->candidates[s->selected], pc))
    return scd_overlay_run(m, cpu, g, &group->candidates[s->selected], s->selected, pc);

  /* Fresh selection among the candidates covering pc. */
  for (k = 0; k < group->candidate_count; ++k) {
    live[k] = (uint8_t)scd_candidate_live(m, cpu, &group->candidates[k]);
    if (live[k] && scd_covers(&group->candidates[k], pc)) {
      ++live_covering;
      if (chosen < 0) chosen = (int32_t)k;
    }
  }
  if (live_covering > 1U) return scd_fail(m, cpu, SCD_STOP_AMBIGUOUS_IMAGE, pc, group->candidates[chosen].image_id);
  if (live_covering == 0U) return scd_overlay_classify(m, cpu, group, pc);
  for (k = 0; k < group->candidate_count; ++k) /* another live image over the chosen one: the bytes cannot tell them apart */
    if ((int32_t)k != chosen && live[k] && scd_overlaps(&group->candidates[k], &group->candidates[chosen]))
      return scd_fail(m, cpu, SCD_STOP_AMBIGUOUS_IMAGE, pc, group->candidates[k].image_id);
  {
    const ScdOverlayCandidate *c = &group->candidates[chosen];
    if (s->stale_from >= 0 && scd_find(c->roots, c->root_count, pc) < 0)
      return scd_fail(m, cpu, SCD_STOP_STALE_IMAGE_SELECTION, pc, group->candidates[s->stale_from].image_id);
    if (scd_find(c->entries, c->entry_count, pc) < 0) return scd_fail(m, cpu, SCD_STOP_IMAGE_ENTRY_NOT_COMPILED, pc, c->image_id);
    s->selected = chosen;
    s->stale_from = -1;
    s->selections += 1U;
    scd_clear_dirty(s);
    scd_overlay_event(m, cpu, g, chosen, pc, 1U);
    return scd_overlay_run(m, cpu, g, c, chosen, pc);
  }
}
