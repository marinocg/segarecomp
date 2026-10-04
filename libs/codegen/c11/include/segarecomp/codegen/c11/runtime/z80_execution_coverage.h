#ifndef SEGARECOMP_Z80_EXECUTION_COVERAGE_H
#define SEGARECOMP_Z80_EXECUTION_COVERAGE_H

/* SEG-031 (ADR 0080): the machine-neutral Z80 execution-PC collector (measurement only; plain C11, header only).
 *
 * A host compiles its program with SEGARECOMP_Z80_EXECUTION_COVERAGE and attaches a collector with
 * `z80_execution_coverage_attach`. Every instruction that begins executing is then keyed by (code-image identity, logical PC): the
 * identity is the host's own `code_image` answer at that PC (banked ROM images and materialized RAM images never conflate). A PC
 * whose identity query fails is counted in `unknown_identity` (it cannot happen for an instruction that began through a generated
 * entry). At most Z80_COVERAGE_MAX_IMAGES distinct identities are kept; further ones are counted in `identity_overflow`.
 *
 * Observational only: nothing is written to guest state, the observer is never part of a report or digest, and an ordinary program
 * (compiled without the macro) never calls it. The bitmaps hold exact PCs: a host writes them only to private, ignored output and
 * persists sanitized aggregates (identity count, distinct PCs, retirements) only. */

#include <stddef.h>
#include <stdint.h>

#include "segarecomp/codegen/c11/runtime/z80_runtime.h"

#define Z80_COVERAGE_MAX_IMAGES 256u
#define Z80_COVERAGE_BITMAP_BYTES 8192u /* one bit per logical PC */

typedef struct Z80ExecutionCoverage {
  Z80Runtime *rt;
  uint32_t image_count;
  uint32_t identities[Z80_COVERAGE_MAX_IMAGES];
  uint8_t bitmaps[Z80_COVERAGE_MAX_IMAGES][Z80_COVERAGE_BITMAP_BYTES];
  uint64_t retirements;
  uint64_t unknown_identity;
  uint64_t identity_overflow;
} Z80ExecutionCoverage;

static inline void z80_execution_coverage_observe(void *observer, uint16_t pc) {
  Z80ExecutionCoverage *coverage = (Z80ExecutionCoverage *)observer;
  Z80CodeImage image;
  uint32_t slot;
  ++coverage->retirements;
  if (coverage->rt->host.code_image == NULL || !coverage->rt->host.code_image(coverage->rt->host.context, pc, &image)) {
    ++coverage->unknown_identity;
    return;
  }
  for (slot = 0; slot < coverage->image_count && coverage->identities[slot] != image.identity; ++slot) {
  }
  if (slot == coverage->image_count) {
    if (slot == Z80_COVERAGE_MAX_IMAGES) {
      ++coverage->identity_overflow;
      return;
    }
    coverage->identities[slot] = image.identity;
    ++coverage->image_count;
  }
  coverage->bitmaps[slot][pc >> 3] = (uint8_t)(coverage->bitmaps[slot][pc >> 3] | (1u << (pc & 7u)));
}

/* `coverage` must be zero-initialized (it is large: allocate it statically or on the heap). */
static inline void z80_execution_coverage_attach(Z80ExecutionCoverage *coverage, Z80Runtime *rt) {
  coverage->rt = rt;
  rt->observer = coverage;
  rt->observe_pc = z80_execution_coverage_observe;
}

static inline int z80_execution_coverage_contains(const Z80ExecutionCoverage *coverage, uint32_t slot, uint16_t pc) {
  return slot < coverage->image_count && ((coverage->bitmaps[slot][pc >> 3] >> (pc & 7u)) & 1u) != 0u;
}

/* Distinct PCs of one identity slot (all slots when slot == Z80_COVERAGE_MAX_IMAGES). */
static inline uint64_t z80_execution_coverage_distinct(const Z80ExecutionCoverage *coverage, uint32_t slot) {
  uint64_t count = 0;
  uint32_t first = slot == Z80_COVERAGE_MAX_IMAGES ? 0u : slot, last = slot == Z80_COVERAGE_MAX_IMAGES ? coverage->image_count : slot + 1u;
  for (uint32_t s = first; s < last && s < coverage->image_count; ++s)
    for (uint32_t i = 0; i < Z80_COVERAGE_BITMAP_BYTES; ++i)
      for (uint8_t byte = coverage->bitmaps[s][i]; byte != 0u; byte = (uint8_t)(byte & (byte - 1u))) ++count;
  return count;
}

#endif /* SEGARECOMP_Z80_EXECUTION_COVERAGE_H */
