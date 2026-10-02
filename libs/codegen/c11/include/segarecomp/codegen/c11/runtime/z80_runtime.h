/* Machine-neutral Z80 generated-code runtime ABI (SEG-008-T003; ADR 0058 sections 3-7, ADR 0059).
 *
 * Plain C11, no machine policy. Generated owners include this header and
 * the platform that hosts a generated program supplies a `Z80Host`. It declares:
 *   - the Z80 state structure, including the explicit in-prefix-run bit and the internal state bits;
 *   - memory read/write and I/O in/out callbacks, interrupt acknowledge callback;
 *   - interrupt inputs (INT level, NMI edge) as state fields the host drives between runs;
 *   - the current-code-image-identity query (identity and window base of a logical address);
 *   - a cycle counter with a deadline (checked only at instruction boundaries);
 *   - execution outcomes in two distinct classes with predicates.
 *
 * Address arithmetic is 16-bit and wraps. `z80_run` (defined by every generated image) executes until the deadline,
 * a HALT/prefix-lock resumable outcome or a fail-closed error. Callbacks receive the cycle count at the start of the
 * current instruction (intra-instruction offsets and wait states are outside the contract, docs section 7).
 */
#ifndef SEGARECOMP_Z80_RUNTIME_H
#define SEGARECOMP_Z80_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

/* Outcomes. Resumable outcomes are not errors: the state is architecturally exact and `z80_run` may be called again
 * with a later deadline. Errors are fail-closed typed stops; the state is left at the offending instruction start. */
typedef enum Z80Outcome {
  Z80_OUTCOME_NONE = 0, /* internal: no stop requested */
  Z80_OUTCOME_DEADLINE = 1,
  Z80_OUTCOME_HALTED = 2,
  Z80_OUTCOME_PREFIX_LOCK = 3,
  Z80_ERROR_NO_OWNER = 16,
  Z80_ERROR_MUTABLE_CODE = 17,
  Z80_ERROR_UNRESOLVED_FETCH_MAPPING = 18,
  Z80_ERROR_UNKNOWN_IMAGE_IDENTITY = 19,
  Z80_ERROR_EXCLUDED_FORM = 20,
  Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE = 21,
  Z80_ERROR_CODE_MISMATCH = 22 /* RAM-backed image: the live bytes of an instruction differ from the compiled ones */
} Z80Outcome;

static inline int z80_outcome_is_resumable(Z80Outcome outcome) {
  return outcome == Z80_OUTCOME_DEADLINE || outcome == Z80_OUTCOME_HALTED || outcome == Z80_OUTCOME_PREFIX_LOCK;
}
static inline int z80_outcome_is_error(Z80Outcome outcome) { return (int)outcome >= (int)Z80_ERROR_NO_OWNER; }

static inline const char *z80_outcome_name(Z80Outcome outcome) {
  switch (outcome) {
    case Z80_OUTCOME_NONE: return "none";
    case Z80_OUTCOME_DEADLINE: return "deadline";
    case Z80_OUTCOME_HALTED: return "halted";
    case Z80_OUTCOME_PREFIX_LOCK: return "prefix_lock";
    case Z80_ERROR_NO_OWNER: return "no_owner";
    case Z80_ERROR_MUTABLE_CODE: return "mutable_code";
    case Z80_ERROR_UNRESOLVED_FETCH_MAPPING: return "unresolved_fetch_mapping";
    case Z80_ERROR_UNKNOWN_IMAGE_IDENTITY: return "unknown_image_identity";
    case Z80_ERROR_EXCLUDED_FORM: return "excluded_form";
    case Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE: return "im0_unsupported_acknowledge_byte";
    case Z80_ERROR_CODE_MISMATCH: return "code_mismatch";
  }
  return "invalid";
}

/* Architectural and internal state (docs/architecture/z80-cpu-contract.md section 2). R keeps bit 7 across M1
 * increments; only LD R,A changes it. */
typedef struct Z80State {
  uint8_t a, f, b, c, d, e, h, l;
  uint8_t a2, f2, b2, c2, d2, e2, h2, l2;
  uint16_t ix, iy, sp, pc, wz;
  uint8_t i, r, im, iff1, iff2, q;
  uint8_t halted;        /* HALT executed and not yet left; PC is the address after HALT */
  uint8_t int_deferral;  /* maskable INT not accepted at the next boundary (EI, IFF1-changing RETI/RETN) */
  uint8_t ld_a_ir;       /* NMOS marker: the last instruction was LD A,I or LD A,R */
  uint8_t in_prefix_run; /* inside an endless DD/FD run (prefix_lock); clear at every ordinary boundary */
  uint8_t int_line;      /* maskable INT level, driven by the host */
  uint8_t nmi_pending;   /* latched NMI edge, raised by the host */
  uint8_t nmi_reject;    /* an NMI response just started: an NMI edge at the next boundary is discarded */
  uint64_t cycles;       /* T-states executed */
  uint64_t deadline;     /* stop at the first instruction boundary with cycles >= deadline */
} Z80State;

/* Answer of the current-code-image-identity query for a logical address. `window_base` is the logical address at
 * which image offset 0 is mapped. Window-relative owners derive every PC-dependent value from it. */
typedef struct Z80CodeImage {
  uint32_t identity;
  uint16_t window_base;
} Z80CodeImage;

typedef struct Z80Host {
  void *context;
  uint8_t (*read)(void *context, uint16_t address, uint64_t cycles);
  void (*write)(void *context, uint16_t address, uint8_t value, uint64_t cycles);
  uint8_t (*io_in)(void *context, uint16_t port, uint64_t cycles);
  void (*io_out)(void *context, uint16_t port, uint8_t value, uint64_t cycles);
  /* Interrupt acknowledge: the byte the device puts on the data bus (IM2 vector byte, IM0 RST opcode). */
  uint8_t (*interrupt_acknowledge)(void *context, uint64_t cycles);
  /* Non-zero and fills `image` if immutable code is mapped at `address`; zero means mutable/non-code (fail closed). */
  int (*code_image)(void *context, uint16_t address, Z80CodeImage *image);
  /* RAM-backed images only (SEG-032-T003/T012): copies the `length` (1-4) live memory bytes at `address` (wrapping, mirrors resolved
   * by the platform) into `bytes` and returns non-zero. Not an architectural read (no cycles, no side effects). Absent (NULL) in a
   * host that runs a RAM-backed image: every entry guard fails. */
  int (*code_fetch)(void *context, uint16_t address, uint8_t *bytes, uint32_t length);
} Z80Host;

typedef struct Z80Runtime {
  Z80State state;
  Z80Host host;
  Z80Outcome outcome;
  /* Non-architectural: entry snapshot of the live bytes of the RAM-backed instruction being executed (ADR 0073). Displacement and
   * immediate operands are read from it, never re-fetched, so an instruction that writes its own operand uses the entry value. */
  uint8_t live_code[4];
} Z80Runtime;

/* Every instruction start has an exact generated entry. An owner is one generated function holding a bounded set of exact entries
 * (ADR 0071); it selects the active entry from PC (absolute-PC owners) or PC - window base (window-relative owners). It executes
 * one or more directly chained entries, running the ordinary boundary prologue before every instruction, and returns the next
 * owner to run (direct binding, statically invariant windows only) or a null reference to return to the dispatcher with
 * `state.pc` set. */
struct Z80OwnerRef;
typedef struct Z80OwnerRef (*Z80Owner)(struct Z80Runtime *rt, uint16_t window_base);
struct Z80OwnerRef {
  Z80Owner next;
};
#define Z80_OWNER_STOP ((struct Z80OwnerRef){NULL})
#define Z80_OWNER_NEXT(owner) ((struct Z80OwnerRef){(owner)})

/* Defined by every generated image (main translation unit). */
Z80Outcome z80_run(Z80Runtime *rt, uint64_t deadline);

/* Contract reset state (section 2): PC = 0, I = R = 0, IFF = 0, IM 0; AF = SP and the other registers 0xFFFF. */
static inline void z80_reset(Z80State *s) {
  s->a = s->f = s->b = s->c = s->d = s->e = s->h = s->l = 0xFF;
  s->a2 = s->f2 = s->b2 = s->c2 = s->d2 = s->e2 = s->h2 = s->l2 = 0xFF;
  s->ix = s->iy = s->sp = s->wz = 0xFFFFu;
  s->pc = 0;
  s->i = s->r = s->im = s->iff1 = s->iff2 = s->q = 0;
  s->halted = s->int_deferral = s->ld_a_ir = s->in_prefix_run = 0;
  s->int_line = s->nmi_pending = s->nmi_reject = 0;
  s->cycles = 0;
  s->deadline = 0;
}

/* ---- helpers used by generated owners and by the boundary logic ---- */
#define Z80_FLAG_PV 0x04u

static inline uint8_t z80_r_add(uint8_t r, uint64_t count) {
  return (uint8_t)((r & 0x80u) | ((r + count) & 0x7Fu));
}
static inline uint8_t z80_read(Z80Runtime *rt, uint16_t address) {
  return rt->host.read(rt->host.context, address, rt->state.cycles);
}
static inline void z80_write(Z80Runtime *rt, uint16_t address, uint8_t value) {
  rt->host.write(rt->host.context, address, value, rt->state.cycles);
}
static inline uint8_t z80_io_in(Z80Runtime *rt, uint16_t port) {
  return rt->host.io_in(rt->host.context, port, rt->state.cycles);
}
static inline void z80_io_out(Z80Runtime *rt, uint16_t port, uint8_t value) {
  rt->host.io_out(rt->host.context, port, value, rt->state.cycles);
}
static inline uint16_t z80_read16(Z80Runtime *rt, uint16_t address) {
  const uint8_t low = z80_read(rt, address);
  return (uint16_t)(low | ((uint16_t)z80_read(rt, (uint16_t)(address + 1u)) << 8));
}
/* Push writes the high byte at SP-1 first, then the low byte at SP-2. */
static inline void z80_push16(Z80Runtime *rt, uint16_t value) {
  Z80State *s = &rt->state;
  s->sp = (uint16_t)(s->sp - 2u);
  z80_write(rt, (uint16_t)(s->sp + 1u), (uint8_t)(value >> 8));
  z80_write(rt, s->sp, (uint8_t)value);
}

/* Owner prologue (ADR 0058 section 6): returns non-zero when the owner must return to the dispatcher with PC = its
 * own address because the deadline is reached or an interrupt may be accepted at this boundary. Otherwise the
 * instruction starts: the one-boundary deferral and the LD A,I/R marker have done their job. */
static inline int z80_owner_boundary(Z80Runtime *rt, uint16_t pc) {
  Z80State *s = &rt->state;
  if (s->cycles >= s->deadline || s->nmi_pending || s->nmi_reject || (s->int_line && s->iff1 && !s->int_deferral)) {
    s->pc = pc;
    return 1;
  }
  return 0;
}
/* Instruction-begin bookkeeping: the one-boundary deferral and the LD A,I/R marker have done their job. */
static inline void z80_owner_begin(Z80Runtime *rt) {
  rt->state.int_deferral = 0;
  rt->state.ld_a_ir = 0;
}
static inline int z80_owner_prologue(Z80Runtime *rt, uint16_t pc) {
  if (z80_owner_boundary(rt, pc)) return 1;
  z80_owner_begin(rt);
  return 0;
}

/* RAM-backed entry guard (ADR 0073, SEG-032-T012): after the boundary check, before the instruction-begin bookkeeping and any
 * effect. Snapshots the `length` live bytes into `rt->live_code`, then requires every byte whose bit is set in `structural` (bit i =
 * byte i: prefixes, opcode, anything but a displacement/immediate payload) to equal the compiled one. Returns non-zero (outcome set,
 * PC at the instruction start, no architectural state change, boundary state intact) on a mismatch or a missing `code_fetch`. */
static inline int z80_live_guard(Z80Runtime *rt, uint16_t pc, unsigned length, unsigned structural, unsigned b0, unsigned b1, unsigned b2,
                                 unsigned b3) {
  const uint8_t expected[4] = {(uint8_t)b0, (uint8_t)b1, (uint8_t)b2, (uint8_t)b3};
  unsigned i;
  if (rt->host.code_fetch != NULL && rt->host.code_fetch(rt->host.context, pc, rt->live_code, length)) {
    for (i = 0; i < length; ++i)
      if (((structural >> i) & 1u) && rt->live_code[i] != expected[i]) break;
    if (i == length) return 0;
  }
  rt->state.pc = pc;
  rt->outcome = Z80_ERROR_CODE_MISMATCH;
  return 1;
}

/* Prefix-lock owner entry (ADR 0058 section 5). Entered with in-prefix-run clear it runs the ordinary prologue and
 * then sets the bit. With the bit set (resume) interrupts are never considered; only the deadline is. */
static inline int z80_lock_enter(Z80Runtime *rt, uint16_t pc) {
  Z80State *s = &rt->state;
  if (s->in_prefix_run) {
    if (s->cycles < s->deadline) return 0;
    s->pc = pc;
    return 1;
  }
  if (z80_owner_prologue(rt, pc)) return 1;
  s->in_prefix_run = 1;
  return 0;
}
/* One prefix per 4 T-states and one M1 (R + 1) each, until the deadline; PC is the wrapping next-fetch address. */
static inline void z80_lock_run(Z80Runtime *rt, uint16_t pc) {
  Z80State *s = &rt->state;
  const uint64_t count = (s->deadline - s->cycles + 3u) / 4u;
  s->cycles += 4u * count;
  s->r = z80_r_add(s->r, count);
  s->pc = (uint16_t)((pc + count) & 0xFFFFu);
  rt->outcome = Z80_OUTCOME_PREFIX_LOCK;
}

static inline void z80_accept_common(Z80State *s) {
  s->halted = 0;
  s->int_deferral = 0;
  s->ld_a_ir = 0;
  s->q = 0;
  s->r = z80_r_add(s->r, 1);
}

/* NMI response: IFF1 cleared (IFF2 kept), push PC, PC = 0x0066, 11 T, R + 1. */
static inline void z80_accept_nmi(Z80Runtime *rt) {
  Z80State *s = &rt->state;
  s->nmi_pending = 0;
  s->nmi_reject = 1;
  s->iff1 = 0;
  z80_accept_common(s);
  z80_push16(rt, s->pc);
  s->pc = s->wz = 0x0066u;
  s->cycles += 11u;
}

/* Maskable INT response (IM0 RST-only static contract, IM1, IM2). Every mode performs the interrupt-acknowledge bus
 * transaction (the device's data-bus byte); IM1 disregards the byte. Returns non-zero on a fail-closed error, which
 * leaves the architectural state untouched. */
static inline int z80_accept_int(Z80Runtime *rt) {
  Z80State *s = &rt->state;
  const uint8_t byte = rt->host.interrupt_acknowledge(rt->host.context, s->cycles);
  if (s->im == 0u && (byte & 0xC7u) != 0xC7u) { /* IM0: only the eight single-byte RST opcodes are admissible */
    rt->outcome = Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE; /* fail closed before any state changes */
    return 1;
  }
  const uint8_t marker = s->ld_a_ir;
  s->iff1 = s->iff2 = 0;
  if (marker) s->f = (uint8_t)(s->f & ~Z80_FLAG_PV);
  z80_accept_common(s);
  if (s->im == 1u) {
    z80_push16(rt, s->pc);
    s->pc = s->wz = 0x0038u;
    s->cycles += 13u;
  } else if (s->im == 2u) {
    z80_push16(rt, s->pc);
    s->pc = s->wz = z80_read16(rt, (uint16_t)(((uint16_t)s->i << 8) | byte));
    s->cycles += 19u;
  } else {
    z80_push16(rt, s->pc);
    s->pc = s->wz = (uint16_t)(byte & 0x38u);
    s->cycles += 13u;
  }
  return 0;
}

/* Boundary logic of the dispatcher. Returns 1 when `z80_run` must return (`rt->outcome` set), 2 when an interrupt
 * response was performed (re-evaluate the boundary), 0 to look up and run the owner at `state.pc`. */
static inline int z80_boundary(Z80Runtime *rt) {
  Z80State *s = &rt->state;
  if (s->cycles >= s->deadline) {
    rt->outcome = s->in_prefix_run ? Z80_OUTCOME_PREFIX_LOCK : s->halted ? Z80_OUTCOME_HALTED : Z80_OUTCOME_DEADLINE;
    return 1;
  }
  if (s->in_prefix_run) return 0;
  if (s->nmi_reject) {
    s->nmi_reject = 0;
    s->nmi_pending = 0;
  } else if (s->nmi_pending) {
    z80_accept_nmi(rt);
    return 2;
  } else if (s->int_line && s->iff1 && !s->int_deferral) {
    return z80_accept_int(rt) ? 1 : 2;
  }
  if (s->halted) { /* halted M1 cycles: 4 T and R + 1 each, accounted up to the deadline */
    const uint64_t count = (s->deadline - s->cycles + 3u) / 4u;
    s->cycles += 4u * count;
    s->r = z80_r_add(s->r, count);
    rt->outcome = Z80_OUTCOME_HALTED;
    return 1;
  }
  return 0;
}

#endif /* SEGARECOMP_Z80_RUNTIME_H */
