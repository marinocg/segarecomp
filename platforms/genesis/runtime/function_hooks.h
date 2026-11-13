#ifndef SEGARECOMP_GENESIS_FUNCTION_HOOKS_H
#define SEGARECOMP_GENESIS_FUNCTION_HOOKS_H

/*
 * SEG-138-T004 (ADR 0298): the dispatch-boundary half of static function hooks. Header-only strict C11, included only by generated programs that carry hook sites (the generated text
 * defines `genesis_dispatch` as a wrapper that calls `genesis_function_hook_step` and then the ordinary dispatch). Nothing here decodes an opcode, patches code or looks anything up by
 * name: it compares the dispatch PC with a const, build-resolved site table and keeps a bounded stack of call frames.
 *
 * Semantics (mod-execution-phases.md section 5):
 *  - entry: a dispatch at a site's entry whose return address (long at A7) is one of the site's statically known call continuations is a CALL. before: `enter`, then the original runs;
 *    replace: `enter`; when it returns 1 the function is not run - PC becomes the return address, A7 += 4, the declared cost is retired; after: a frame is pushed.
 *  - exit (after): at a dispatch boundary where PC == the frame's return address and A7 == entry_sp + 4 the function has returned through its original address: `leave`.
 *  - a frame whose stack was unwound without that return (A7 above entry_sp + 4) is abandoned and counted; a dispatch at a site from an unknown caller, with the stack outside work RAM or
 *    with the frame stack full is skipped and counted. In every skipped case the original code runs unchanged (the guest is never altered by a hook that did not fire).
 * The register view `genesis_hook_reg_read` is the only state a callback sees; `genesis_hook_reg_write` is what a committed replacement writes.
 */
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

/* Register-view slots (bit numbers of the declaration masks): 0..7 D0-D7, 8..15 A0-A7, 16 CCR, 17 deterministic M68K cycle counter, 24..31 stack slot k at entry. */
#define GENESIS_HOOK_REG_CCR 16U
#define GENESIS_HOOK_REG_CYCLES 17U
#define GENESIS_HOOK_REG_STACK0 24U
#define GENESIS_HOOK_READABLE_MASK UINT32_C(0xFF03FFFF)
#define GENESIS_HOOK_WRITABLE_MASK UINT32_C(0x00017FFF)

static inline int genesis_hook_stack_long(const GenesisRuntime *runtime, uint32_t address, uint32_t *value) {
  uint32_t offset;
  address &= UINT32_C(0x00FFFFFF);
  if (address < UINT32_C(0x00FF0000) || address > UINT32_C(0x00FFFFFC) || (address & 1U) != 0U) return 0;
  offset = address - UINT32_C(0x00FF0000);
  *value = ((uint32_t)runtime->work_ram[offset] << 24U) | ((uint32_t)runtime->work_ram[offset + 1U] << 16U) | ((uint32_t)runtime->work_ram[offset + 2U] << 8U) | (uint32_t)runtime->work_ram[offset + 3U];
  return 1;
}

/* Reads register-view slot `slot` (0..31); `entry_sp` is the frame's entry stack pointer. Returns 0 when the slot is not readable (only the slots in GENESIS_HOOK_READABLE_MASK are). */
static inline int genesis_hook_reg_read(const GenesisRuntime *runtime, uint32_t entry_sp, uint32_t slot, uint32_t *value) {
  if (slot > 31U || ((GENESIS_HOOK_READABLE_MASK >> slot) & 1U) == 0U) return 0;
  if (slot < 8U) { *value = runtime->d[slot]; return 1; }
  if (slot < 16U) { *value = runtime->a[slot - 8U]; return 1; }
  if (slot == GENESIS_HOOK_REG_CCR) { *value = (uint32_t)(runtime->sr & 0x1FU); return 1; }
  if (slot == GENESIS_HOOK_REG_CYCLES) { *value = (uint32_t)(runtime->scheduler.master_ticks / 7U); return 1; }
  return genesis_hook_stack_long(runtime, entry_sp + 4U + 4U * (slot - GENESIS_HOOK_REG_STACK0), value);
}

/* Writes slot `slot` (D0-D7, A0-A6, CCR only); returns 0 for any other slot. */
static inline int genesis_hook_reg_write(GenesisRuntime *runtime, uint32_t slot, uint32_t value) {
  if (slot > 31U || ((GENESIS_HOOK_WRITABLE_MASK >> slot) & 1U) == 0U) return 0;
  if (slot < 8U) runtime->d[slot] = value;
  else if (slot < 16U) runtime->a[slot - 8U] = value;
  else runtime->sr = (uint16_t)((runtime->sr & 0xFFE0U) | (value & 0x1FU));
  return 1;
}

/* Returns 1 with *out set when this dispatch was fully handled (a committed replacement); 0 = run the ordinary dispatch. */
static inline int genesis_function_hook_step(GenesisRuntime *runtime, const GenesisHookSiteInfo *sites, size_t site_count, const uint32_t *continuations, GenesisControlTransfer *out) {
  GenesisFunctionHooks *hooks = runtime->function_hooks;
  size_t low = 0U, high = site_count;
  if (hooks == 0) return 0;
  while (hooks->depth != 0U) {   /* exits first: a return and the next call may share one boundary */
    const GenesisHookFrame *frame = &hooks->frames[hooks->depth - 1U];
    if (runtime->pc == frame->return_pc && runtime->a[7] == frame->entry_sp + 4U) {
      const GenesisHookFrame done = *frame;
      --hooks->depth;
      if (hooks->leave != 0) hooks->leave(hooks->ctx, runtime, done.site, done.entry_sp);
      continue;
    }
    if (runtime->a[7] > frame->entry_sp + 4U) {   /* unwound without returning through the original address */
      const GenesisHookSiteInfo *site = frame->site;
      --hooks->depth;
      ++hooks->abandoned;
      if (hooks->skipped != 0) hooks->skipped(hooks->ctx, runtime, site, 1U);
      continue;
    }
    break;
  }
  while (low < high) {
    const size_t middle = low + (high - low) / 2U;
    if (sites[middle].entry < runtime->pc) low = middle + 1U; else high = middle;
  }
  if (low < site_count && sites[low].entry == runtime->pc) {
    const GenesisHookSiteInfo *site = &sites[low];
    uint32_t return_pc = 0U, index, known = 0U;
    if (!genesis_hook_stack_long(runtime, runtime->a[7], &return_pc)) { ++hooks->unproven_callers; if (hooks->skipped != 0) hooks->skipped(hooks->ctx, runtime, site, 2U); return 0; }
    return_pc &= UINT32_C(0x00FFFFFF);
    for (index = 0U; index < site->continuation_count; ++index) if (continuations[site->continuation_begin + index] == return_pc) known = 1U;
    if (!known) { ++hooks->unproven_callers; if (hooks->skipped != 0) hooks->skipped(hooks->ctx, runtime, site, 2U); return 0; }
    while (hooks->depth != 0U && hooks->frames[hooks->depth - 1U].entry_sp <= runtime->a[7]) {   /* a new call at an SP at or above a pending frame: that function cannot still be running (it left without its original return) */
      const GenesisHookSiteInfo *stale = hooks->frames[hooks->depth - 1U].site;
      --hooks->depth;
      ++hooks->abandoned;
      if (hooks->skipped != 0) hooks->skipped(hooks->ctx, runtime, stale, 1U);
    }
    if ((site->mode & GENESIS_HOOK_MODE_AFTER) != 0U && hooks->depth >= GENESIS_HOOK_FRAMES_MAX) { ++hooks->overflowed; if (hooks->skipped != 0) hooks->skipped(hooks->ctx, runtime, site, 3U); return 0; }   /* skipped as a whole: no entry callback without its exit */
    if ((site->mode & GENESIS_HOOK_MODE_BEFORE) != 0U && hooks->enter != 0) (void)hooks->enter(hooks->ctx, runtime, site, runtime->a[7]);
    if ((site->mode & GENESIS_HOOK_MODE_REPLACE) != 0U && hooks->enter != 0) {
      const uint32_t entry_sp = runtime->a[7];
      if (hooks->enter(hooks->ctx, runtime, site, entry_sp) != 0) {
        runtime->pc = return_pc;
        runtime->a[7] = entry_sp + 4U;
        *out = genesis_runtime_retire_m68k_instruction(runtime, site->cost, runtime->pc);
        return 1;
      }
    }
    if ((site->mode & GENESIS_HOOK_MODE_AFTER) != 0U) {
      hooks->frames[hooks->depth].site = site;
      hooks->frames[hooks->depth].return_pc = return_pc;
      hooks->frames[hooks->depth].entry_sp = runtime->a[7];
      ++hooks->depth;
    }
  }
  return 0;
}

#endif
