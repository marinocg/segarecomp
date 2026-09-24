#ifndef SEGARECOMP_CPU_M68K_EXCEPTION_CORE_H
#define SEGARECOMP_CPU_M68K_EXCEPTION_CORE_H

#include <stdint.h>

/*
 * SEG-021-T018 / ADR 0043 §2, §5, §6, §7: the reusable, machine-independent
 * MC68000 Group 1/2 exception-entry and RTE core for generated-native C11
 * programs.
 *
 * This is a header-only strict-C11 support unit owned by the M68K CPU module.
 * It holds no file-scope mutable state and names no machine: every machine
 * interaction goes through an explicitly bound hook set whose `context`
 * pointer is supplied per CPU instance, so two MC68000 instances can never
 * share state through this unit (ADR 0043 §9).
 *
 * CPU state is referenced, not owned: the caller binds the SR, the ACTIVE
 * stack pointer (A7, the one SR.S selects), the INACTIVE stack-pointer slot
 * (USP while S = 1, SSP while S = 0) and the PC of its own per-instance CPU
 * record.  The frame is the original MC68000 six-byte frame (ADR 0043 §2):
 * saved SR word at the new supervisor SP, saved PC long word at SP+2, no
 * format/vector-offset word.  Nothing here fetches, decodes or interprets
 * program bytes; the handler entry comes from the machine's resolve_vector
 * hook, which selects among entries compiled from permitted build-time inputs.
 */

#define SEGARECOMP_M68K_SR_TRACE UINT16_C(0x8000)
#define SEGARECOMP_M68K_SR_SUPERVISOR UINT16_C(0x2000)
#define SEGARECOMP_M68K_SR_INTERRUPT_MASK UINT16_C(0x0700)
/* The SR bits the MC68000 implements (T, S, I2-I0, X, N, Z, V, C); every other
   bit reads as zero, so every SR write is masked with this value. */
#define SEGARECOMP_M68K_SR_IMPLEMENTED UINT16_C(0xA71F)
#define SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES UINT32_C(6)

typedef enum SegarecompM68kExceptionStatus {
  SEGARECOMP_M68K_EXCEPTION_OK = 0,
  /* The supervisor stack pointer cannot hold the frame (odd, underflow, or
     the machine rejected the extent).  Nothing was written or committed. */
  SEGARECOMP_M68K_EXCEPTION_STACK_INVALID = 1,
  /* resolve_vector reported no installed / representable handler. */
  SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE = 2,
  /* A stack access failed after validation (entry) or a frame read failed
     (RTE).  Nothing was committed. */
  SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED = 3,
  /* RTE would leave SR.T = 1: trace is deferred and fails closed (§6). */
  SEGARECOMP_M68K_EXCEPTION_TRACE_DEFERRED = 4,
  SEGARECOMP_M68K_EXCEPTION_BAD_BINDING = 5
} SegarecompM68kExceptionStatus;

typedef enum SegarecompM68kVectorResolution {
  SEGARECOMP_M68K_VECTOR_HANDLER = 0,
  SEGARECOMP_M68K_VECTOR_NOT_INSTALLED = 1,
  SEGARECOMP_M68K_VECTOR_UNREPRESENTABLE = 2
} SegarecompM68kVectorResolution;

typedef enum SegarecompM68kStackDirection {
  SEGARECOMP_M68K_STACK_READ = 0,
  SEGARECOMP_M68K_STACK_WRITE = 1
} SegarecompM68kStackDirection;

/* ADR 0043 §7 machine hooks.  Every hook receives the bound `context`.
   Boolean hooks return 1 on success and 0 on a fail-closed refusal; a refusal
   must leave the machine unchanged.  `size` is 2 (word) or 4 (long). */
typedef struct SegarecompM68kMachineHooks {
  void *context;
  int (*validate_stack_extent)(void *context, uint32_t base, uint32_t length, SegarecompM68kStackDirection direction);
  int (*stack_read)(void *context, uint32_t address, uint32_t size, uint32_t *value);
  int (*stack_write)(void *context, uint32_t address, uint32_t size, uint32_t value);
  SegarecompM68kVectorResolution (*resolve_vector)(void *context, uint32_t vector, uint32_t *handler_entry);
  /* Optional provenance notifications (may be null). */
  void (*on_exception_entry)(void *context, uint32_t vector, uint32_t frame_base, uint32_t handler_entry);
  void (*on_exception_return)(void *context, uint32_t frame_base);
} SegarecompM68kMachineHooks;

/* The per-instance CPU fields the core reads and commits. */
typedef struct SegarecompM68kCpuBinding {
  uint16_t *sr;
  uint32_t *active_sp;
  uint32_t *inactive_sp;
  uint32_t *pc;
} SegarecompM68kCpuBinding;

static inline int segarecomp_m68k_binding_valid(const SegarecompM68kMachineHooks *hooks,
                                                const SegarecompM68kCpuBinding *cpu) {
  return hooks != 0 && cpu != 0 && cpu->sr != 0 && cpu->active_sp != 0 && cpu->inactive_sp != 0 &&
         cpu->pc != 0 && hooks->validate_stack_extent != 0 && hooks->stack_read != 0 && hooks->stack_write != 0 &&
         hooks->resolve_vector != 0;
}

/*
 * ADR 0043 §5 entry, in order: saved SR <- SR; the frame goes on the SSP (the
 * active SP when S = 1, the inactive slot when S = 0); validate the complete
 * extent [SSP-6, SSP); resolve the handler; write SR word at SSP-6 and PC long
 * at SSP-4; then commit together SR <- (saved & keep) | forced (callers always
 * force S and clear T), active SP <- SSP-6, the USP into the inactive slot when
 * the mode changed, and PC <- handler.  Every fallible step precedes the first
 * frame write, so any failure returns with nothing written or committed.
 */
static inline SegarecompM68kExceptionStatus segarecomp_m68k_exception_enter(
    const SegarecompM68kMachineHooks *hooks, const SegarecompM68kCpuBinding *cpu, uint32_t vector,
    uint32_t stacked_pc, uint16_t sr_keep_mask, uint16_t sr_forced_bits, uint32_t *handler_entry_out) {
  uint16_t saved_sr;
  int was_supervisor;
  uint32_t ssp;
  uint32_t frame_base;
  uint32_t handler_entry = 0U;
  if (!segarecomp_m68k_binding_valid(hooks, cpu)) return SEGARECOMP_M68K_EXCEPTION_BAD_BINDING;
  saved_sr = *cpu->sr;
  was_supervisor = (saved_sr & SEGARECOMP_M68K_SR_SUPERVISOR) != 0U;
  ssp = was_supervisor ? *cpu->active_sp : *cpu->inactive_sp;
  if (ssp < SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES || (ssp & 1U) != 0U) return SEGARECOMP_M68K_EXCEPTION_STACK_INVALID;
  frame_base = ssp - SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES;
  if (!hooks->validate_stack_extent(hooks->context, frame_base, SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES,
                                    SEGARECOMP_M68K_STACK_WRITE))
    return SEGARECOMP_M68K_EXCEPTION_STACK_INVALID;
  if (hooks->resolve_vector(hooks->context, vector, &handler_entry) != SEGARECOMP_M68K_VECTOR_HANDLER)
    return SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE;
  /* Frame (§2): saved SR word at frame_base, saved PC long at frame_base + 2. */
  if (!hooks->stack_write(hooks->context, frame_base, 2U, (uint32_t)saved_sr))
    return SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED;
  if (!hooks->stack_write(hooks->context, frame_base + 2U, 4U, stacked_pc))
    return SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED;
  /* Commit. */
  if (!was_supervisor) *cpu->inactive_sp = *cpu->active_sp; /* the USP moves to the inactive slot */
  *cpu->active_sp = frame_base;
  *cpu->sr = (uint16_t)(((saved_sr & sr_keep_mask) | sr_forced_bits) & SEGARECOMP_M68K_SR_IMPLEMENTED);
  *cpu->pc = handler_entry;
  if (handler_entry_out != 0) *handler_entry_out = handler_entry;
  if (hooks->on_exception_entry != 0) hooks->on_exception_entry(hooks->context, vector, frame_base, handler_entry);
  return SEGARECOMP_M68K_EXCEPTION_OK;
}

/*
 * ADR 0043 §5 RTE (the caller has already performed the privilege check, so
 * S = 1 and the active SP is the SSP): read the SR word at SSP and the PC long
 * at SSP+2, then commit together SR <- read SR (masked to the implemented
 * bits), PC <- read PC, SSP <- SSP+6; when the restored S is 0 the USP (the
 * inactive slot) becomes active and the incremented SSP moves to the inactive
 * slot.  A failed read, or a restored T = 1 (trace deferred, §6), commits
 * nothing.
 */
static inline SegarecompM68kExceptionStatus segarecomp_m68k_exception_return(
    const SegarecompM68kMachineHooks *hooks, const SegarecompM68kCpuBinding *cpu, uint32_t *restored_pc_out) {
  uint32_t sp;
  uint32_t saved_sr = 0U;
  uint32_t saved_pc = 0U;
  uint16_t restored_sr;
  if (!segarecomp_m68k_binding_valid(hooks, cpu)) return SEGARECOMP_M68K_EXCEPTION_BAD_BINDING;
  sp = *cpu->active_sp;
  if ((sp & 1U) != 0U) return SEGARECOMP_M68K_EXCEPTION_STACK_INVALID;
  if (!hooks->stack_read(hooks->context, sp, 2U, &saved_sr)) return SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED;
  if (!hooks->stack_read(hooks->context, sp + 2U, 4U, &saved_pc)) return SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED;
  restored_sr = (uint16_t)(saved_sr & SEGARECOMP_M68K_SR_IMPLEMENTED);
  if ((restored_sr & SEGARECOMP_M68K_SR_TRACE) != 0U) return SEGARECOMP_M68K_EXCEPTION_TRACE_DEFERRED;
  if (hooks->on_exception_return != 0) hooks->on_exception_return(hooks->context, sp);
  if ((restored_sr & SEGARECOMP_M68K_SR_SUPERVISOR) != 0U) {
    *cpu->active_sp = sp + SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES;
  } else {
    const uint32_t user_sp = *cpu->inactive_sp;
    *cpu->inactive_sp = sp + SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES;
    *cpu->active_sp = user_sp;
  }
  *cpu->sr = restored_sr;
  *cpu->pc = saved_pc;
  if (restored_pc_out != 0) *restored_pc_out = saved_pc;
  return SEGARECOMP_M68K_EXCEPTION_OK;
}

#endif
