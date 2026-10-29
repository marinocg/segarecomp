/* SEG-100 (ADR 0221, 0223): runtime binding of file-first dynamic images to the guest memory the original game loaded them into.
 *
 * What is compiled before launch: for every candidate disc file ("dynamic image"), one native body per source offset the CPU-owned decode
 * and lowering accepted. Their text reads the actual guest PC and verifies that instruction's bytes (scd_code_guard) before it runs.
 *
 * What this file does at run time, and nothing else: when a PC is owned by no static case and no overlay group, it looks for the one candidate
 * image the live bytes around that PC are an exact copy of, over a run of delivered disc blocks (the same rule as the build-time
 * resolve_source, ADR 0205: an anchor of 32 live bytes, extended both ways while live bytes equal the image and the blocks were delivered by
 * the machine's own CDC; the longest run wins; a tie between distinct candidates is ambiguous; a run shorter than SCD_DYN_MIN_RUN is refused). The
 * anchor is tried at the PC and, when the bytes after the PC were changed by the module (variables next to code), at the 32 and 64 bytes before it.
 * The result is a binding (image, base): from then on the body is picked from the image's table by source offset. It never decodes an opcode,
 * never lowers, never generates or compiles code, never guesses among candidates, and never serves an instruction whose bytes are not the image's.
 *
 * The exact-copy run is the evidence that identifies the image; it is not the extent of what may execute. The binding serves the image's whole extent
 * at that base, and each instruction is judged by its own bytes: a 64-byte granule is "clean" when the whole granule was seen equal to the image (the
 * run, or a comparison since the last write into it), and an instruction in a clean granule is served at the cost of a table lookup. A granule that
 * was written since (or lies outside the run) is not known clean: it is compared with the image once; if equal it becomes clean; if not (a variable the
 * module changes next to code), it stays "mutated" and each instruction fetched in it is compared with the image's bytes at that offset before the body
 * is chosen. An instruction that differs makes the binding skipped for that PC (it stays for the rest of its image) and, if no other binding explains the PC, the PC
 * is searched afresh (so a file replaced by another is re-identified, and an overwritten instruction is never executed through the old image). The body's own byte guard remains the last, unconditional
 * check of every instruction.
 *
 * Cost: a search reads the delivered candidate images (each at most SCD_DYN_MAX_IMAGE_BYTES) once per binding event; an instruction in a clean granule
 * costs one table lookup, one in a mutated granule at most ten byte comparisons. Nothing scans the disc or the RAM per instruction. */
#include "scd_runtime.h"

#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define SCD_DYN_GRANULE_BYTES (1U << SCD_DYN_GRANULE_SHIFT)
#define SCD_DYN_MAX_INSTRUCTION 10U

static ScdMachine *scd_dyn_machine; /* the one machine of this process (computed-control lookups have no machine parameter) */

static void scd_dyn_event(ScdMachine *m, uint32_t cpu, uint32_t kind, uint32_t pc, uint32_t image, uint32_t base) {
  ScdDynamicState *d = &m->dyn;
  if (d->events < SCD_DYN_LOG) {
    ScdDynamicEvent *e = &d->log[d->events];
    e->tick = m->now;
    e->cpu = cpu;
    e->kind = kind;
    e->pc = pc;
    e->image = image;
    e->base = base;
  }
  d->events += 1U;
}

static const ScdDynamicProgram *scd_dyn_registered;

void scd_dynamic_register(const ScdDynamicProgram *program) { scd_dyn_registered = program; }

void scd_dynamic_attach(ScdMachine *m) {
  memset(&m->dyn, 0, sizeof m->dyn);
  m->dyn.enabled = scd_dyn_registered != NULL ? 1U : 0U;
  scd_dyn_machine = scd_dyn_registered != NULL ? m : NULL;
}

static const ScdDynamicProgram *scd_dyn_program(const ScdMachine *m) { (void)m; return scd_dyn_registered; }

static uint32_t scd_dyn_sectors(const ScdDynamicImage *img) { return (img->length + 2047U) / 2048U; }

static int scd_dyn_delivered(const ScdMachine *m, uint32_t lba) {
  uint32_t i;
  for (i = 0; i < m->delivered_count; ++i)
    if (lba >= m->delivered[i].first_lba && lba - m->delivered[i].first_lba < m->delivered[i].count) return 1;
  return 0;
}

static uint32_t scd_dyn_slot(uint32_t index) { return index < SCD_DYN_MAX_IMAGES ? index : SCD_DYN_MAX_IMAGES - 1U; }

/* The physical region a processor's address belongs to, as canonical [lo, hi) guest addresses. A run, an anchor or a binding's evidence never crosses it:
 * main work RAM is mirrored every 64 KiB below $FF0000, Word RAM every 256 KiB, and the sub's windows are separate memories; bytes on the other side of a region
 * boundary are a different memory even when the address arithmetic is contiguous. Returns 0 for an address that is not executable RAM of the processor. */
static int scd_dyn_region(uint32_t cpu, uint32_t address, uint32_t *lo, uint32_t *hi) {
  address &= UINT32_C(0x00FFFFFF);
  if (cpu == SCD_MAIN) {
    if (address >= UINT32_C(0x00E00000)) { *lo = UINT32_C(0x00FF0000); *hi = UINT32_C(0x01000000); return 1; }
    if (address >= UINT32_C(0x00200000) && address < UINT32_C(0x00400000)) { *lo = address & ~UINT32_C(0x3FFFF); *hi = *lo + UINT32_C(0x40000); return 1; }
    if (address < UINT32_C(0x00200000) && (address & UINT32_C(0x3FFFF)) >= UINT32_C(0x20000)) { *lo = address & ~UINT32_C(0x1FFFF); *hi = *lo + UINT32_C(0x20000); return 1; }
    return 0;
  }
  {
    const uint32_t a = address & UINT32_C(0xFFFFF), window = address & ~UINT32_C(0xFFFFF);
    if (a < SCD_PRG_RAM_BYTES) { *lo = window; *hi = window + SCD_PRG_RAM_BYTES; return 1; }
    if (a < UINT32_C(0xC0000)) { *lo = window + UINT32_C(0x80000); *hi = window + UINT32_C(0xC0000); return 1; }
    if (a < UINT32_C(0xE0000)) { *lo = window + UINT32_C(0xC0000); *hi = window + UINT32_C(0xE0000); return 1; }
  }
  return 0;
}

static void scd_dyn_bit_set(uint8_t *bits, uint32_t bit) { bits[bit >> 3] = (uint8_t)(bits[bit >> 3] | (1U << (bit & 7U))); }
static void scd_dyn_bit_clear(uint8_t *bits, uint32_t bit) { bits[bit >> 3] = (uint8_t)(bits[bit >> 3] & ~(1U << (bit & 7U))); }
static int scd_dyn_bit_get(const uint8_t *bits, uint32_t bit) { return (bits[bit >> 3] >> (bit & 7U)) & 1U; }

/* SEG-101: count one retired instruction against the image that served it and note its source offset (observer-only). */
static void scd_dyn_account(ScdMachine *m, const ScdDynamicBinding *b, uint32_t pc) {
  ScdDynamicState *d = &m->dyn;
  const uint32_t slot = scd_dyn_slot(b->image);
  const ScdDynamicImage *img = &scd_dyn_registered->images[b->image];
  const uint32_t offset = (pc - b->base) >> 1;
  if (b->image >= SCD_DYN_MAX_IMAGES) return; /* the emitter refuses more images (too_many_images); never alias a slot */
  d->image_retired[slot] += 1U;
  if (d->image_seen[slot] == NULL) d->image_seen[slot] = (uint8_t *)calloc((img->length >> 4) + 2U, 1U);
  if (d->image_seen[slot] != NULL && (offset >> 3) < (img->length >> 4) + 2U && !scd_dyn_bit_get(d->image_seen[slot], offset)) {
    scd_dyn_bit_set(d->image_seen[slot], offset);
    d->image_offsets[slot] += 1U;
  }
}

void scd_dynamic_release(ScdMachine *m) {
  uint32_t i;
  for (i = 0; i < SCD_DYN_MAX_IMAGES; ++i) {
    free(m->dyn.image_seen[i]);
    m->dyn.image_seen[i] = NULL;
  }
}

/* The image bytes [offset, offset + length) into `out`, or 0 when a sector cannot be read. */
static int scd_dyn_image_read(ScdMachine *m, const ScdDynamicImage *img, uint32_t offset, uint32_t length, uint8_t *out) {
  uint32_t done = 0U;
  ScdDynamicState *d = &m->dyn;
  while (done < length) {
    const uint32_t at = offset + done, lba = img->first_lba + at / 2048U, in = at % 2048U, slot = lba % SCD_DYN_CACHE_SECTORS;
    uint32_t n = 2048U - in;
    if (n > length - done) n = length - done;
    if (!d->cache_valid[slot] || d->cache_lba[slot] != lba) {
      if (scd_disc_read_user_data(m->disc, lba, d->cache[slot]) != SCD_DISC_OK) {
        d->cache_valid[slot] = 0U;
        return 0;
      }
      d->cache_lba[slot] = lba;
      d->cache_valid[slot] = 1U;
    }
    memcpy(out + done, d->cache[slot] + in, n);
    done += n;
  }
  return 1;
}

static uint32_t scd_dyn_body_index(const ScdDynamicImage *img, uint32_t offset) {
  return (offset & 1U) == 0U && offset < img->length ? img->body_map[offset / 2U] : 0U;
}

static ScdDispatchFunction scd_dyn_body(const ScdDynamicProgram *p, uint32_t index) {
  if (index == 0U || index > p->body_count) return NULL;
  return p->body_slices[(index - 1U) / p->body_slice_size][(index - 1U) % p->body_slice_size];
}

void scd_dynamic_note_write(ScdMachine *m, uint32_t memory, uint32_t offset, uint32_t length) {
  uint32_t cpu, k;
  if (scd_dyn_machine != m || length == 0U) return;
  for (cpu = 0; cpu < 2U; ++cpu) {
    int any = 0;
    for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) any |= m->dyn.binding[cpu][k].active;
    if (!any) continue;
    for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) {
      ScdDynamicBinding *b = &m->dyn.binding[cpu][k];
      int v0, v1;
      uint32_t a0, a1, i;
      if (!b->active) continue;
      a0 = scd_overlay_view_of(m, cpu, memory, offset, &v0);
      a1 = scd_overlay_view_of(m, cpu, memory, offset + length - 1U, &v1);
      if (v0 && v1 && a1 >= a0 && a1 - a0 == length - 1U) { /* contiguous in this processor's view: one range */
        uint32_t lo = a0 > b->lo ? a0 : b->lo, hi = a1 + 1U < b->hi ? a1 + 1U : b->hi, g;
        if (lo >= hi) continue;
        for (g = (lo >> SCD_DYN_GRANULE_SHIFT); g <= ((hi - 1U) >> SCD_DYN_GRANULE_SHIFT); ++g) {
          scd_dyn_bit_set(b->dirty, g - b->granule_first);
          scd_dyn_bit_clear(b->mutated, g - b->granule_first);
        }
        continue;
      }
      for (i = 0; i < length; ++i) { /* interleaved or partly invisible: byte by byte */
        int visible;
        const uint32_t address = scd_overlay_view_of(m, cpu, memory, offset + i, &visible);
        if (!visible || address < b->lo || address >= b->hi) continue;
        scd_dyn_bit_set(b->dirty, (address >> SCD_DYN_GRANULE_SHIFT) - b->granule_first);
        scd_dyn_bit_clear(b->mutated, (address >> SCD_DYN_GRANULE_SHIFT) - b->granule_first);
      }
    }
  }
}

void scd_dynamic_note_view_change(ScdMachine *m) {
  uint32_t cpu, k;
  if (scd_dyn_machine != m) return;
  for (cpu = 0; cpu < 2U; ++cpu)
    for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) {
      ScdDynamicBinding *b = &m->dyn.binding[cpu][k];
      if (!b->active) continue;
      memset(b->dirty, 0xFF, sizeof b->dirty);   /* what a processor sees at these addresses may be other memory: nothing is known clean */
      memset(b->mutated, 0, sizeof b->mutated);
    }
}

/* Compare the live bytes [lo, hi) with the image's bytes at the same offsets. 1 when equal (and visible). */
static int scd_dyn_live_equals_image(ScdMachine *m, uint32_t cpu, const ScdDynamicBinding *b, uint32_t lo, uint32_t hi) {
  const ScdDynamicImage *img = &scd_dyn_program(m)->images[b->image];
  uint8_t want[SCD_DYN_GRANULE_BYTES];
  uint32_t i;
  if (lo >= hi) return 1;
  if (hi - lo > sizeof want || !scd_dyn_image_read(m, img, lo - b->base, hi - lo, want)) return 0;
  for (i = 0; i < hi - lo; ++i) {
    const uint8_t *live = scd_code_byte(m, cpu, lo + i);
    if (live == NULL || *live != want[i]) return 0;
  }
  return 1;
}

/* Is the instruction at `pc` (`length` bytes, the length of the body that would run, or ten when the image has none; inside the binding's extent) the image's? Clean granules are trusted; an unknown granule is compared once and either
 * becomes clean or is marked mutated; an instruction in a mutated granule is compared byte for byte with the image. */
static int scd_dyn_instruction_is_image(ScdMachine *m, uint32_t cpu, ScdDynamicBinding *b, uint32_t pc, uint32_t length) {
  uint32_t end = pc + length, g, need = 0U;
  if (end > b->hi) end = b->hi;
  for (g = pc >> SCD_DYN_GRANULE_SHIFT; g <= ((end - 1U) >> SCD_DYN_GRANULE_SHIFT); ++g) {
    const uint32_t bit = g - b->granule_first;
    if (!scd_dyn_bit_get(b->dirty, bit)) continue;
    if (!scd_dyn_bit_get(b->mutated, bit)) { /* unknown: one whole-granule comparison decides whether it can be trusted again */
      uint32_t lo = g << SCD_DYN_GRANULE_SHIFT, hi = lo + SCD_DYN_GRANULE_BYTES;
      if (lo < b->lo) lo = b->lo;
      if (hi > b->hi) hi = b->hi;
      m->dyn.granule_compares += 1U;
      if (scd_dyn_live_equals_image(m, cpu, b, lo, hi)) {
        scd_dyn_bit_clear(b->dirty, bit);
        m->dyn.granule_upgrades += 1U;
        continue;
      }
      scd_dyn_bit_set(b->mutated, bit);
    }
    need = 1U;
  }
  if (!need) return 1;
  m->dyn.rechecks += 1U;
  if (scd_dyn_live_equals_image(m, cpu, b, pc, end)) return 1;
  m->dyn.recheck_failures += 1U;
  return 0;
}

/* The most recently used binding of `cpu` whose extent contains `pc` and whose order is below `below` (UINT64_MAX: any); NULL when there is none. */
static ScdDynamicBinding *scd_dyn_find_below(ScdMachine *m, uint32_t cpu, uint32_t pc, uint64_t below) {
  ScdDynamicBinding *best = NULL;
  uint32_t k;
  for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) {
    ScdDynamicBinding *b = &m->dyn.binding[cpu][k];
    if (b->active && pc >= b->lo && pc < b->hi && b->order < below && (best == NULL || b->order > best->order)) best = b;
  }
  return best;
}

typedef struct ScdDynamicCandidate {
  uint32_t image, base, run_lo, run_hi;
} ScdDynamicCandidate;

/* One anchor position: search the delivered candidate images for the exact copy of the 32 live bytes at `anchor_at`, extend it, and keep the longest run that
 * contains `pc`. Returns 0 (*out filled), or a SCD_DYN_REFUSE_* class; *short_run is set when a run existed but was shorter than the minimum. */
static uint32_t scd_dyn_search_anchor(ScdMachine *m, uint32_t cpu, uint32_t pc, uint32_t anchor_at, ScdDynamicCandidate *out, uint32_t *ambiguous_image, int *short_run,
                                      int *any_delivered) {
  const ScdDynamicProgram *p = scd_dyn_program(m);
  uint8_t anchor[SCD_DYN_ANCHOR_BYTES];
  uint32_t i, img_index, occurrences = 0U, best_len = 0U, ties = 0U, tied_images[8], rlo, rhi;
  ScdDynamicCandidate best;
  memset(&best, 0, sizeof best);
  if (!scd_dyn_region(cpu, pc, &rlo, &rhi) || anchor_at < rlo || anchor_at + SCD_DYN_ANCHOR_BYTES > rhi) return SCD_DYN_REFUSE_NO_SOURCE;   /* the anchor lies wholly inside PC's memory */
  for (i = 0; i < SCD_DYN_ANCHOR_BYTES; ++i) {
    const uint8_t *live = scd_code_byte(m, cpu, anchor_at + i);
    if (live == NULL) return SCD_DYN_REFUSE_NO_SOURCE;
    anchor[i] = *live;
  }
  for (img_index = 0; img_index < p->image_count; ++img_index) {
    const ScdDynamicImage *img = &p->images[img_index];
    uint8_t *bytes;
    uint32_t sectors, s, delivered = 0U, at;
    sectors = scd_dyn_sectors(img);
    for (s = 0; s < sectors && !delivered; ++s) delivered = (uint32_t)scd_dyn_delivered(m, img->first_lba + s);
    if (!delivered) continue;
    *any_delivered = 1;
    bytes = (uint8_t *)malloc(img->length != 0U ? img->length : 1U);
    if (bytes == NULL || !scd_dyn_image_read(m, img, 0U, img->length, bytes)) {
      free(bytes);
      return SCD_DYN_REFUSE_DISC_UNREADABLE;
    }
    m->dyn.scans += 1U;
    m->dyn.scan_bytes += img->length;
    for (at = 0U; at + SCD_DYN_ANCHOR_BYTES <= img->length; ++at) {
      const uint8_t *hit = (const uint8_t *)memchr(bytes + at, anchor[0], img->length - at - (SCD_DYN_ANCHOR_BYTES - 1U));
      uint32_t lo_i, hi_i, pc_off, run;
      if (hit == NULL) break;
      at = (uint32_t)(hit - bytes);
      if (memcmp(hit, anchor, SCD_DYN_ANCHOR_BYTES) != 0) continue;
      if (++occurrences > SCD_DYN_MAX_CANDIDATES) {
        free(bytes);
        m->dyn.ambiguity_run = 0U;
        m->dyn.ambiguity_count = occurrences;
        m->dyn.ambiguity_images[0] = img->id;
        *ambiguous_image = img->id;
        return SCD_DYN_REFUSE_AMBIGUOUS;
      }
      /* the source offset of PC under this alignment must be an even instruction start inside the image */
      pc_off = at + (pc - anchor_at);
      if (pc_off >= img->length || (pc_off & 1U) != 0U || pc_off > pc) continue;   /* the image's byte 0 would lie below address 0 */
      lo_i = at;
      hi_i = at + SCD_DYN_ANCHOR_BYTES;
      while (lo_i > 0U && anchor_at - (at - lo_i) > 0U) { /* extend back while the live bytes equal the image and the block was delivered */
        const uint8_t *live = anchor_at - (at - lo_i) - 1U < rlo ? NULL : scd_code_byte(m, cpu, anchor_at - (at - lo_i) - 1U);
        if (live == NULL || *live != bytes[lo_i - 1U] || !scd_dyn_delivered(m, img->first_lba + (lo_i - 1U) / 2048U)) break;
        --lo_i;
      }
      while (hi_i < img->length) {
        const uint8_t *live = anchor_at + (hi_i - at) >= rhi ? NULL : scd_code_byte(m, cpu, anchor_at + (hi_i - at));
        if (live == NULL || *live != bytes[hi_i] || !scd_dyn_delivered(m, img->first_lba + hi_i / 2048U)) break;
        ++hi_i;
      }
      run = hi_i - lo_i;
      if (pc_off < lo_i || pc_off >= hi_i) continue; /* the run must contain PC's source offset (an anchor before PC may stop short of it) */
      if (run > best_len) {
        best_len = run;
        ties = 1U;
        tied_images[0] = img->id;
        best.image = img_index;
        best.base = pc - pc_off;
        best.run_lo = pc - (pc_off - lo_i);
        best.run_hi = pc + (hi_i - pc_off);
      } else if (run == best_len) {
        if (ties < 8U) tied_images[ties] = img->id;
        ++ties;
      }
    }
    free(bytes);
  }
  if (best_len == 0U) return SCD_DYN_REFUSE_NO_SOURCE;
  if (best_len < SCD_DYN_MIN_RUN) { /* a coincidence of fewer than the minimum bytes is no identity - tied or not */
    *short_run = 1;
    return SCD_DYN_REFUSE_TOO_SHORT;
  }
  if (ties > 1U) {
    m->dyn.ambiguity_run = best_len;
    m->dyn.ambiguity_count = ties;
    for (i = 0; i < 8U && i < ties; ++i) m->dyn.ambiguity_images[i] = tied_images[i];
    *ambiguous_image = p->images[best.image].id;
    return SCD_DYN_REFUSE_AMBIGUOUS;
  }
  *out = best;
  return SCD_DYN_REFUSE_NONE;
}

/* Search the delivered candidate images for the one the live bytes at `pc` copy. The anchor is tried at the PC and, when the module changed bytes after the PC, at
 * the 32 and the 64 bytes before it (a run that contains the PC identifies the image just as well). Returns 0 (*out filled) or a SCD_DYN_REFUSE_* class. */
static uint32_t scd_dyn_search(ScdMachine *m, uint32_t cpu, uint32_t pc, ScdDynamicCandidate *out, uint32_t *ambiguous_image) {
  static const uint32_t back[3] = {0U, SCD_DYN_ANCHOR_BYTES, 2U * SCD_DYN_ANCHOR_BYTES};
  int short_run = 0, any_delivered = 0;
  uint32_t k;
  if ((pc & 1U) != 0U || scd_code_byte(m, cpu, pc) == NULL) return SCD_DYN_REFUSE_NOT_EXECUTABLE;
  if (m->delivered_overflow) return SCD_DYN_REFUSE_LEDGER_OVERFLOW;
  for (k = 0; k < 3U; ++k) {
    uint32_t anchor_at = pc, reason, i;
    int ok = 1;
    if (back[k] > pc) break;
    anchor_at = pc - back[k];
    if (k == 0U) { /* near the end of the visible memory the window shifts back so that it ends inside it */
      for (;;) {
        ok = 1;
        for (i = 0; i < SCD_DYN_ANCHOR_BYTES; ++i)
          if (scd_code_byte(m, cpu, anchor_at + i) == NULL) { ok = 0; break; }
        if (ok || anchor_at < 2U || pc - anchor_at >= SCD_DYN_ANCHOR_BYTES) break;
        anchor_at -= 2U;
      }
      if (!ok) continue;
    }
    reason = scd_dyn_search_anchor(m, cpu, pc, anchor_at, out, ambiguous_image, &short_run, &any_delivered);
    if (reason == SCD_DYN_REFUSE_NONE || reason == SCD_DYN_REFUSE_DISC_UNREADABLE) return reason;
    if (reason == SCD_DYN_REFUSE_AMBIGUOUS && k == 0U) return reason;   /* an anchor before the PC that is too common to identify anything is just not used */
  }
  if (!any_delivered) return SCD_DYN_REFUSE_NO_DELIVERY;
  return short_run ? SCD_DYN_REFUSE_TOO_SHORT : SCD_DYN_REFUSE_NO_SOURCE;
}

static ScdDynamicBinding *scd_dyn_install(ScdMachine *m, uint32_t cpu, const ScdDynamicCandidate *c, uint32_t pc) {
  const ScdDynamicProgram *p = scd_dyn_program(m);
  const ScdDynamicImage *img = &p->images[c->image];
  ScdDynamicBinding *slot = NULL;
  uint32_t k, g;
  uint32_t lo = c->base, hi = c->base + img->length, rlo = 0U, rhi = 0U;
  if (scd_dyn_region(cpu, pc, &rlo, &rhi)) {   /* what the binding serves never leaves the memory it was found in (the image's un-loaded head may lie before it) */
    if (lo < rlo) lo = rlo;
    if (hi > rhi) hi = rhi;
  }
  int overlaps_other = 0;
  for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) {
    ScdDynamicBinding *b = &m->dyn.binding[cpu][k];
    if (!b->active) continue;
    if (b->image == c->image && b->base == c->base) slot = b;   /* the same image at the same base is identified again: refreshed in place */
    else if (b->lo < hi && lo < b->hi) overlaps_other = 1;      /* another image over the same memory: both stay as hypotheses, the newer is tried first */
  }
  if (overlaps_other) {
    m->dyn.replacements += 1U;
    scd_dyn_event(m, cpu, 3U, pc, img->id, c->base);
  }
  if (slot == NULL)
    for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k)
      if (!m->dyn.binding[cpu][k].active) { slot = &m->dyn.binding[cpu][k]; break; }
  if (slot == NULL) { /* the least recently used binding is evicted */
    slot = &m->dyn.binding[cpu][0];
    for (k = 1U; k < SCD_DYN_MAX_BINDINGS; ++k)
      if (m->dyn.binding[cpu][k].order < slot->order) slot = &m->dyn.binding[cpu][k];
    scd_dyn_event(m, cpu, 3U, pc, p->images[slot->image].id, slot->base);
  }
  memset(slot, 0, sizeof *slot);
  slot->active = 1U;
  slot->image = c->image;
  slot->base = c->base;
  slot->lo = lo;
  slot->hi = hi;
  slot->run_lo = c->run_lo;
  slot->run_hi = c->run_hi;
  slot->granule_first = lo >> SCD_DYN_GRANULE_SHIFT;
  slot->order = ++m->dyn.order;
  memset(slot->dirty, 0xFF, sizeof slot->dirty);   /* nothing is known clean ... */
  for (g = (c->run_lo + SCD_DYN_GRANULE_BYTES - 1U) >> SCD_DYN_GRANULE_SHIFT; ((g + 1U) << SCD_DYN_GRANULE_SHIFT) <= c->run_hi; ++g)
    scd_dyn_bit_clear(slot->dirty, g - slot->granule_first);   /* ... except the granules wholly inside the exact-copy run */
  m->dyn.binds += 1U;
  m->dyn.binds_by_cpu[cpu] += 1U;
  scd_dyn_bit_set(m->dyn.bound_ever, scd_dyn_slot(c->image));
  scd_dyn_event(m, cpu, 1U, pc, img->id, c->base);
  return slot;
}

/* The byte length of the instruction the image's body at `pc` verifies; ten (the longest MC68000 instruction) when the image has no body there. */
static uint32_t scd_dyn_instruction_bytes(const ScdDynamicProgram *p, const ScdDynamicBinding *b, uint32_t pc) {
  const uint32_t index = scd_dyn_body_index(&p->images[b->image], pc - b->base);
  const uint32_t length = index != 0U && index <= p->body_count && p->body_lengths != NULL ? p->body_lengths[index - 1U] : 0U;
  return length != 0U ? length : SCD_DYN_MAX_INSTRUCTION;
}

/* Resolve `pc` to a precompiled body: through the most recently used binding whose image the instruction's bytes still equal, or a fresh one. Returns the body
 * (non-NULL) or NULL with *refusal set. A binding whose image the instruction does not equal is skipped for it, not dropped. */
static ScdDispatchFunction scd_dyn_resolve(ScdMachine *m, uint32_t cpu, uint32_t pc, uint32_t *refusal, uint32_t *detail, ScdDynamicBinding **bound) {
  const ScdDynamicProgram *p = scd_dyn_program(m);
  ScdDynamicBinding *b = NULL, *candidate;
  uint64_t below = UINT64_MAX;
  *refusal = SCD_DYN_REFUSE_NONE;
  *detail = 0U;
  while ((candidate = scd_dyn_find_below(m, cpu, pc, below)) != NULL) {
    if (scd_dyn_instruction_is_image(m, cpu, candidate, pc, scd_dyn_instruction_bytes(p, candidate, pc))) {
      b = candidate;
      b->stale = 0U;
      break;
    }
    if (!candidate->stale) { /* the transition is the event: a binding does not report every instruction it skips */
      candidate->stale = 1U;
      m->dyn.invalidations += 1U;
      scd_dyn_event(m, cpu, 2U, pc, p->images[candidate->image].id, candidate->base);
    }
    below = candidate->order;
  }
  if (b == NULL) {
    ScdDynamicCandidate c;
    uint32_t reason = scd_dyn_search(m, cpu, pc, &c, detail);
    if (reason != SCD_DYN_REFUSE_NONE) {
      *refusal = reason;
      return NULL;
    }
    b = scd_dyn_install(m, cpu, &c, pc);
    if (!scd_dyn_instruction_is_image(m, cpu, b, pc, scd_dyn_instruction_bytes(p, b, pc))) { /* the run that identified the image ends inside this very instruction */
      *refusal = SCD_DYN_REFUSE_INSTRUCTION_DIFFERS;
      *detail = p->images[b->image].id;
      return NULL;
    }
  }
  {
    const ScdDynamicImage *img = &p->images[b->image];
    const uint32_t index = scd_dyn_body_index(img, pc - b->base);
    ScdDispatchFunction fn = scd_dyn_body(p, index);
    *detail = img->id;
    if (fn == NULL) {
      *refusal = SCD_DYN_REFUSE_BODY_NOT_COMPILED;
      return NULL;
    }
    b->order = ++m->dyn.order;
    *bound = b;
    return fn;
  }
}

static void scd_dyn_refuse(ScdMachine *m, uint32_t cpu, uint32_t pc, uint32_t reason, uint32_t detail) {
  m->dyn.refused[reason] += 1U;
  m->dyn.last_refusal = reason;
  scd_dyn_event(m, cpu, 4U, pc, reason, detail);
}

/* `refusal_stop` is the typed stop when no verified binding explains the PC: entry_not_compiled for a PC no static case owns, code_bytes_mismatch for a PC whose
 * static variants all disagree with the live bytes (the existing vocabulary of the build driver, so a frontier is resolved exactly as before). */
int scd_dynamic_dispatch(ScdMachine *m, uint32_t cpu, ScdStopClass refusal_stop) {
  const uint32_t pc = m->cpu[cpu].pc & UINT32_C(0x00FFFFFF);
  const ScdDynamicProgram *p = scd_dyn_program(m);
  uint32_t reason, detail;
  ScdDynamicBinding *b = NULL;
  ScdDispatchFunction fn;
  int result;
  if (p == NULL) return scd_fail(m, cpu, refusal_stop, pc, pc);
  m->cpu[cpu].pc = pc; /* the 24-bit address bus (the static dispatch masks the PC the same way) */
  if (m->overlays != NULL) { /* a static overlay group keeps its authority over its range */
    uint32_t g;
    for (g = 0; g < m->overlays->group_count; ++g)
      if (m->overlays->groups[g].cpu == cpu && pc >= m->overlays->groups[g].start && pc < m->overlays->groups[g].end)
        return scd_overlay_dispatch(m, cpu);
  }
  /* A fetch from Word RAM is an observation point exactly as for a static body (scd_code_guard): the CDC DMA and the graphics ASIC are brought up to this
   * instruction before the bytes are compared. RAM that no device writes behind the CPU's back is compared as it stands, so a dynamic flow in PRG RAM or work
   * RAM is state-identical to a static one. */
  if (scd_is_word_ram(cpu, pc) && scd_cd_observe(m) != 0) return 1;
  fn = scd_dyn_resolve(m, cpu, pc, &reason, &detail, &b);
  if (fn == NULL) {
    scd_dyn_refuse(m, cpu, pc, reason, detail);
    if (reason == SCD_DYN_REFUSE_AMBIGUOUS) return scd_fail(m, cpu, SCD_STOP_AMBIGUOUS_IMAGE, pc, detail);
    return scd_fail(m, cpu, refusal_stop, pc, pc); /* the unchanged typed stop of the static dispatch: the build driver's frontier */
  }
  m->dyn.cpu = cpu;
  result = fn(m);
  if (result == 0) {
    b->retired += 1U;
    m->dyn.retired += 1U;
    scd_dyn_bit_set(m->dyn.exercised, scd_dyn_slot(b->image));
    scd_dyn_account(m, b, pc);
  }
  return result;
}

uint32_t scd_dynamic_exec_cpu(void) { return scd_dyn_machine != NULL ? scd_dyn_machine->dyn.cpu : 0U; }

int scd_dynamic_known(ScdMachine *m, uint32_t cpu, uint32_t pc) {
  uint32_t reason, detail;
  ScdDynamicBinding *b = NULL;
  if (scd_dyn_machine != m || scd_dyn_program(m) == NULL) return 0;
  pc &= UINT32_C(0x00FFFFFF);
  m->dyn.lookups += 1U;
  if (m->overlays != NULL) {
    uint32_t g;
    for (g = 0; g < m->overlays->group_count; ++g)
      if (m->overlays->groups[g].cpu == cpu && pc >= m->overlays->groups[g].start && pc < m->overlays->groups[g].end) return 0;
  }
  if (scd_is_word_ram(cpu, pc) && scd_cd_observe(m) != 0) return 0; /* as the dispatch does, and only where a device may have written behind the CPU's back */
  if (scd_dyn_resolve(m, cpu, pc, &reason, &detail, &b) == NULL) {
    scd_dyn_refuse(m, cpu, pc, reason, detail);
    return 0;
  }
  m->dyn.lookup_hits += 1U;
  return 1;
}

const uint32_t *scd_dynamic_lookup(uint32_t cpu, uint32_t pc) {
  static const uint32_t known = 1U;
  if (scd_dyn_machine == NULL || cpu > 1U) return NULL;
  return scd_dynamic_known(scd_dyn_machine, cpu, pc) ? &known : NULL;
}

static uint32_t scd_dyn_popcount(const uint8_t *bits, uint32_t bytes) {
  uint32_t i, n = 0U;
  for (i = 0; i < bytes; ++i) {
    uint8_t v = bits[i];
    while (v != 0U) { n += v & 1U; v = (uint8_t)(v >> 1); }
  }
  return n;
}

void scd_dynamic_report(const ScdMachine *m, FILE *out) {
  static const char *const names[SCD_DYN_REFUSE_COUNT] = {"none", "not_executable", "ledger_overflow", "no_delivery", "no_source", "too_short",
                                                          "ambiguous", "body_not_compiled", "disc_unreadable", "instruction_differs"};
  const ScdDynamicProgram *p = scd_dyn_program(m);
  const ScdDynamicState *d = &m->dyn;
  uint32_t cpu, k, i, first;
  if (p == NULL) return;
  fprintf(out, ",\"dynamic\":{\"policy\":\"%s\",\"images\":%u,\"candidate_bytes\":%u,\"compiled_offsets\":%u,\"unique_bodies\":%u,"
               "\"binds\":%llu,\"binds_main\":%llu,\"binds_sub\":%llu,\"distinct_images_bound\":%u,\"distinct_images_exercised\":%u,"
               "\"invalidations\":%llu,\"replacements\":%llu,\"rechecks\":%llu,\"recheck_failures\":%llu,\"granule_compares\":%llu,\"granule_upgrades\":%llu,"
               "\"lookups\":%llu,\"lookup_hits\":%llu,\"scans\":%llu,\"scan_bytes\":%llu,\"retired\":%llu,\"last_refusal\":\"%s\",\"refusals\":{",
          p->policy != NULL ? p->policy : "", p->image_count, p->candidate_bytes, p->compiled_offsets, p->unique_bodies, (unsigned long long)d->binds,
          (unsigned long long)d->binds_by_cpu[0], (unsigned long long)d->binds_by_cpu[1], scd_dyn_popcount(d->bound_ever, sizeof d->bound_ever),
          scd_dyn_popcount(d->exercised, sizeof d->exercised), (unsigned long long)d->invalidations, (unsigned long long)d->replacements,
          (unsigned long long)d->rechecks, (unsigned long long)d->recheck_failures, (unsigned long long)d->granule_compares,
          (unsigned long long)d->granule_upgrades, (unsigned long long)d->lookups, (unsigned long long)d->lookup_hits, (unsigned long long)d->scans,
          (unsigned long long)d->scan_bytes, (unsigned long long)d->retired, names[d->last_refusal]);
  first = 1U;
  for (i = 1U; i < SCD_DYN_REFUSE_COUNT; ++i)
    if (d->refused[i] != 0U) {
      fprintf(out, "%s\"%s\":%llu", first ? "" : ",", names[i], (unsigned long long)d->refused[i]);
      first = 0U;
    }
  fprintf(out, "}");
  if (d->ambiguity_count != 0U) {
    fprintf(out, ",\"last_ambiguity\":{\"run\":%u,\"candidates\":%u,\"images\":[", d->ambiguity_run, d->ambiguity_count);
    for (i = 0; i < 8U && i < d->ambiguity_count; ++i) fprintf(out, "%s%u", i ? "," : "", d->ambiguity_images[i]);
    fprintf(out, "]}");
  }
  fprintf(out, ",\"bindings\":[");
  first = 1U;
  for (cpu = 0; cpu < 2U; ++cpu)
    for (k = 0; k < SCD_DYN_MAX_BINDINGS; ++k) {
      const ScdDynamicBinding *b = &d->binding[cpu][k];
      if (!b->active) continue;
      fprintf(out, "%s{\"cpu\":%u,\"image\":%u,\"base\":%u,\"lo\":%u,\"hi\":%u,\"run_lo\":%u,\"run_hi\":%u,\"retired\":%llu}", first ? "" : ",", cpu, p->images[b->image].id,
              b->base, b->lo, b->hi, b->run_lo, b->run_hi, (unsigned long long)b->retired);
      first = 0U;
    }
  fprintf(out, "],\"exercised\":[");
  first = 1U;
  for (i = 0; i < p->image_count && i < SCD_DYN_MAX_IMAGES; ++i)
    if (d->image_retired[i] != 0U) {
      fprintf(out, "%s{\"image\":%u,\"retired\":%llu,\"offsets\":%u}", first ? "" : ",", p->images[i].id, (unsigned long long)d->image_retired[i], d->image_offsets[i]);
      first = 0U;
    }
  fprintf(out, "],\"bound_images\":[");
  first = 1U;
  for (i = 0; i < p->image_count && i < SCD_DYN_MAX_IMAGES; ++i)
    if (scd_dyn_bit_get(d->bound_ever, i)) {
      fprintf(out, "%s%u", first ? "" : ",", p->images[i].id);
      first = 0U;
    }
  fprintf(out, "],\"event_count\":%u,\"events\":[", d->events);
  for (i = 0; i < d->events && i < SCD_DYN_LOG; ++i) {
    const ScdDynamicEvent *e = &d->log[i];
    static const char *const kinds[5] = {"", "bound", "invalidated", "replaced", "refused"};
    fprintf(out, "%s{\"tick\":%llu,\"cpu\":%u,\"kind\":\"%s\",\"pc\":%u,\"%s\":%u,\"base\":%u}", i ? "," : "", (unsigned long long)e->tick, e->cpu,
            kinds[e->kind], e->pc, e->kind == 4U ? "reason" : "image", e->image, e->base);
  }
  fprintf(out, "]}");
}
