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
  /* An RTE frame read failed.  Nothing was committed.  Exception entry never
     returns this: after a successful validate_stack_extent its frame writes
     cannot fail (see SegarecompM68kMachineHooks.frame_write). */
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
   must leave the machine unchanged.  `size` is 2 (word) or 4 (long).

   ADR 0043 §5 validate-then-commit contract: a successful
   `validate_stack_extent(context, base, length, SEGARECOMP_M68K_STACK_WRITE)`
   GUARANTEES that every subsequent even-aligned word/long `frame_write`
   entirely inside [base, base + length) succeeds for the bound machine.  The
   frame write is therefore non-fallible (`void`): the core has no failure path
   between the first frame byte and the commit.  A machine whose routed write
   could still fail must refuse such an extent in validate_stack_extent; an
   impossible post-validation failure is a machine-side invariant violation
   that the binding records and turns into its own terminal stop; it is never
   a recoverable core outcome and the core never rolls frame bytes back.
   `stack_read` (RTE) stays fallible. */
typedef struct SegarecompM68kMachineHooks {
  void *context;
  int (*validate_stack_extent)(void *context, uint32_t base, uint32_t length, SegarecompM68kStackDirection direction);
  int (*stack_read)(void *context, uint32_t address, uint32_t size, uint32_t *value);
  void (*frame_write)(void *context, uint32_t address, uint32_t size, uint32_t value);
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
         cpu->pc != 0 && hooks->validate_stack_extent != 0 && hooks->stack_read != 0 && hooks->frame_write != 0 &&
         hooks->resolve_vector != 0;
}

/*
 * ADR 0043 §5 entry, in order: saved SR <- SR; the frame goes on the SSP (the
 * active SP when S = 1, the inactive slot when S = 0); validate the complete
 * extent [SSP-6, SSP); resolve the handler; write SR word at SSP-6 and PC long
 * at SSP-4; then commit together SR <- (saved & keep) | forced (callers always
 * force S and clear T), active SP <- SSP-6, the USP into the inactive slot when
 * the mode changed, and PC <- handler.  Every fallible step (binding, SSP
 * alignment/underflow, extent validation, vector resolution) precedes the first
 * frame write, and the frame writes themselves are non-fallible by the hook
 * contract, so any failure returns with nothing written or committed and a
 * started frame always completes and commits.
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
  hooks->frame_write(hooks->context, frame_base, 2U, (uint32_t)saved_sr);
  hooks->frame_write(hooks->context, frame_base + 2U, 4U, stacked_pc);
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

/*
 * SEG-021-T019 / ADR 0043 §5 RTR (unprivileged; no mode requirement): read the
 * word at the active SP and the PC long at SP+2, then commit together CCR <-
 * the read word's X/N/Z/V/C (the system byte of SR is unchanged), PC <- the
 * read PC, active SP <- SP+6. A failed read commits nothing. RTR is not an
 * exception return, so no exception-return notification is made.
 */
static inline SegarecompM68kExceptionStatus segarecomp_m68k_return_restore_ccr(
    const SegarecompM68kMachineHooks *hooks, const SegarecompM68kCpuBinding *cpu, uint32_t *restored_pc_out) {
  uint32_t sp;
  uint32_t saved_ccr = 0U;
  uint32_t saved_pc = 0U;
  if (!segarecomp_m68k_binding_valid(hooks, cpu)) return SEGARECOMP_M68K_EXCEPTION_BAD_BINDING;
  sp = *cpu->active_sp;
  if ((sp & 1U) != 0U) return SEGARECOMP_M68K_EXCEPTION_STACK_INVALID;
  if (!hooks->stack_read(hooks->context, sp, 2U, &saved_ccr)) return SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED;
  if (!hooks->stack_read(hooks->context, sp + 2U, 4U, &saved_pc)) return SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED;
  *cpu->sr = (uint16_t)((*cpu->sr & UINT16_C(0xFF00)) | (saved_ccr & UINT32_C(0x1F)));
  *cpu->active_sp = sp + SEGARECOMP_M68K_EXCEPTION_FRAME_BYTES;
  *cpu->pc = saved_pc;
  if (restored_pc_out != 0) *restored_pc_out = saved_pc;
  return SEGARECOMP_M68K_EXCEPTION_OK;
}

/*
 * SEG-021-T020 / ADR 0043 §3, §7: the MC68000 interrupt acceptance contract and
 * the STOP halt state (MC68000 User's Manual §6.3.2 "Interrupts" and the
 * interrupt-acknowledge entries of §6.3; M68000 Family Programmer's Reference
 * Manual STOP entry).
 *
 * The request level (0 = none, 1-7), its sources, the device wiring and the
 * acknowledge answer stay machine-owned: the machine samples its request level
 * into this per-instance CPU record at instruction boundaries and answers the
 * acknowledge.  The CPU owns only the recognition rule, the vector mapping and
 * the entry SR:
 *
 *   - levels 1-6 are recognized iff the request level exceeds the SR interrupt
 *     mask (I2-I0);
 *   - level 7 is not maskable and is transition-sensitive: a change of the
 *     request level from a lower level to 7 latches one level-7 recognition
 *     regardless of the mask (serviced at the next boundary, even at mask 7).
 *     A level 7 that stays asserted while the mask is 7 is not recognized
 *     again; when an instruction lowers the mask below 7 while level 7 is still
 *     asserted, the level comparator recognizes it like any level above the
 *     mask (UM §6.3.2: "A level 7 interrupt may still be caused by the level
 *     comparator if the request level is a 7 and the processor priority is set
 *     to a lower level by an instruction").  Level 7 is therefore never "level
 *     > mask" alone.  A latched transition stays pending until serviced, as in
 *     the pinned Musashi core (`nmi_pending`), even if the request drops first.
 *   - acknowledge: autovector -> vector 24 + level (25-31); a supplied vector
 *     must be a user interrupt vector (64-255) -- any other number is refused
 *     (ADR 0043 §3: reserved vectors are never delivered); spurious -> 24;
 *     uninitialized -> 15.
 *   - entry: the shared Group 1/2 entry with the next instruction stacked, then
 *     SR <- S = 1, T = 0, I = accepted level; a stopped CPU resumes.
 *
 * STOP (privileged; the lowering owns the privilege check and the SR load) ends
 * with `stopped = 1` at the boundary of the next instruction.  Waking is a
 * machine scheduler decision (ADR 0041): the machine advances virtual time
 * until a recognized interrupt, or ends with an explicit diagnostic when
 * `segarecomp_m68k_stop_wake_possible` says no request it can ever raise could
 * be recognized.  Nothing here fetches or decodes program bytes.
 */
typedef struct SegarecompM68kInterruptState {
  uint8_t sampled_level;       /* the request level seen at the latest sample (0-7) */
  uint8_t level7_edge_pending; /* a lower -> 7 transition that has not been serviced */
  uint8_t stopped;             /* STOP executed; no instruction runs until an interrupt is accepted */
} SegarecompM68kInterruptState;

typedef enum SegarecompM68kInterruptAcknowledge {
  SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR = 0,
  SEGARECOMP_M68K_INTERRUPT_ACK_SUPPLIED_VECTOR = 1,
  SEGARECOMP_M68K_INTERRUPT_ACK_SPURIOUS = 2,
  SEGARECOMP_M68K_INTERRUPT_ACK_UNINITIALIZED = 3
} SegarecompM68kInterruptAcknowledge;

#define SEGARECOMP_M68K_VECTOR_UNINITIALIZED_INTERRUPT UINT32_C(15)
#define SEGARECOMP_M68K_VECTOR_SPURIOUS_INTERRUPT UINT32_C(24)

/* Record the machine's current request level (sampled at an instruction boundary). */
static inline void segarecomp_m68k_interrupt_sample(SegarecompM68kInterruptState *state, uint32_t request_level) {
  const uint8_t level = (uint8_t)(request_level & 7U);
  if (state == 0) return;
  if (level == 7U && state->sampled_level != 7U) state->level7_edge_pending = 1U;
  state->sampled_level = level;
}

/* The level recognized at this boundary under `sr` (0 = none). */
static inline uint32_t segarecomp_m68k_interrupt_recognized_level(const SegarecompM68kInterruptState *state,
                                                                  uint16_t sr) {
  const uint32_t mask = (uint32_t)((sr & SEGARECOMP_M68K_SR_INTERRUPT_MASK) >> 8U);
  if (state == 0) return 0U;
  if (state->level7_edge_pending) return 7U;
  return state->sampled_level > mask ? (uint32_t)state->sampled_level : 0U;
}

/* Map the machine's acknowledge answer for `level` to a vector number; 0 = refused. */
static inline uint32_t segarecomp_m68k_interrupt_vector(SegarecompM68kInterruptAcknowledge acknowledge,
                                                        uint32_t level, uint32_t supplied_vector) {
  if (level < 1U || level > 7U) return 0U;
  switch (acknowledge) {
  case SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR: return SEGARECOMP_M68K_VECTOR_SPURIOUS_INTERRUPT + level;
  case SEGARECOMP_M68K_INTERRUPT_ACK_SUPPLIED_VECTOR:
    return supplied_vector >= 64U && supplied_vector <= 255U ? supplied_vector : 0U;
  case SEGARECOMP_M68K_INTERRUPT_ACK_SPURIOUS: return SEGARECOMP_M68K_VECTOR_SPURIOUS_INTERRUPT;
  case SEGARECOMP_M68K_INTERRUPT_ACK_UNINITIALIZED: return SEGARECOMP_M68K_VECTOR_UNINITIALIZED_INTERRUPT;
  }
  return 0U;
}

/*
 * Take the recognized interrupt `level` through vector `vector` (from
 * segarecomp_m68k_interrupt_vector) at an instruction boundary: `stacked_pc` is
 * the next instruction (after a STOP, the instruction after STOP).  The entry
 * is the shared §5 entry with SR <- (SR & ~(T | I2-I0)) | S | level << 8; on
 * success a serviced level-7 transition is consumed and a stopped CPU resumes.
 * A refused vector or any entry failure changes nothing.
 */
static inline SegarecompM68kExceptionStatus segarecomp_m68k_interrupt_enter(
    const SegarecompM68kMachineHooks *hooks, const SegarecompM68kCpuBinding *cpu, SegarecompM68kInterruptState *state,
    uint32_t level, uint32_t vector, uint32_t stacked_pc, uint32_t *handler_entry_out) {
  SegarecompM68kExceptionStatus status;
  if (state == 0 || level < 1U || level > 7U) return SEGARECOMP_M68K_EXCEPTION_BAD_BINDING;
  if (vector == 0U) return SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE;
  status = segarecomp_m68k_exception_enter(
      hooks, cpu, vector, stacked_pc,
      (uint16_t)(UINT16_C(0xFFFF) & (uint16_t)~(SEGARECOMP_M68K_SR_TRACE | SEGARECOMP_M68K_SR_INTERRUPT_MASK)),
      (uint16_t)(SEGARECOMP_M68K_SR_SUPERVISOR | (uint16_t)(level << 8U)), handler_entry_out);
  if (status != SEGARECOMP_M68K_EXCEPTION_OK) return status;
  if (level == 7U) state->level7_edge_pending = 0U;
  state->stopped = 0U;
  return SEGARECOMP_M68K_EXCEPTION_OK;
}

/*
 * STOP wake decision.  `max_request_level` is the highest level the machine
 * can still raise while the CPU is stopped (0 when no wired, enabled source
 * with an installed handler exists).  A stopped CPU can wake iff a level-7
 * transition is already latched, the machine can raise level 7 (a transition
 * is then possible), or it can raise a level above the mask STOP loaded.
 */
static inline int segarecomp_m68k_stop_wake_possible(const SegarecompM68kInterruptState *state,
                                                     uint32_t max_request_level, uint16_t sr) {
  const uint32_t mask = (uint32_t)((sr & SEGARECOMP_M68K_SR_INTERRUPT_MASK) >> 8U);
  if (state != 0 && state->level7_edge_pending) return 1;
  max_request_level &= 7U;
  return max_request_level == 7U || max_request_level > mask;
}

#endif
