#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MACHINE_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MACHINE_H

#include <stddef.h>
#include <stdint.h>

#include "segarecomp/codegen/c11/runtime/z80_runtime.h"
#include "sms_error.h"
#include "sms_input.h"
#include "sms_mapper_contract.h"
#include "sms_memory.h"
#include "sms_ports.h"
#include "sms_sha256.h"

/* Master System machine composition and deterministic scheduler (SEG-009-T003; machine contract sections 2, 6, 7, 8;
 * ADR 0064 sections 3-5 and 8).
 *
 * One object composes the generated Z80 image (`z80_run`, defined by the generated program) with the T002 memory map,
 * the I/O port decode and typed device seams that T004 (VDP), T006 (controllers, I/O control) and T007 (PSG) fill in.
 * Time is the Z80 `cycles` counter (T-states), the only timebase of the machine: a line is 228 T, a frame 262 lines =
 * 59,736 T, frame n is `[n x 59,736, (n+1) x 59,736)`.
 *
 * Scheduler. The CPU runs to `deadline = min(next scheduled device event, requested stop)` (absolute T-states, the only
 * deadline type the Z80 ABI has). Device events are scanline starts (T = 228 x absolute line); devices are advanced
 * through every event with `T_event <= T_access` before any memory or I/O access is applied (U11, instruction-start
 * ordering): events due at or before the current cycle are applied between `z80_run` calls and, defensively, at the top
 * of every host callback. Nothing is split inside an instruction. The result depends only on guest T-states, never on
 * how the host slices `run_until_*` calls.
 *
 * Interrupts. `int_line` is a level driven by the VDP seam (`irq_sources`) after every scheduled event and every
 * port access; `nmi_pending` is an edge raised at the frame start where the scripted pause input goes `-` to `P`. The
 * Z80 runtime never accepts either while `in_prefix_run` (contract), so the platform only latches them.
 *
 * Errors. A fail-closed `Z80Outcome` or a latched `SmsError` (memory latch, `mem.error`; also the unimplemented port
 * classes) stops the machine permanently: the platform lowers `state.deadline` so the run ends at the next instruction
 * boundary, and every later `sms_run_*` call returns the same stop. */
#ifdef __cplusplus
extern "C" {
#endif

#define SMS_CYCLES_PER_LINE UINT64_C(228)
#define SMS_LINES_PER_FRAME UINT64_C(262)
#define SMS_CYCLES_PER_FRAME UINT64_C(59736) /* 262 x 228 */
#define SMS_NO_LIMIT UINT64_MAX

/* ---- device seams --------------------------------------------------------------------------------------------- */

/* A device reachable through port classes. A NULL `read`/`write` makes every class of that owner in that direction
 * fail closed with SMS_ERROR_PORT_UNIMPLEMENTED (never a silent value). `cycles` is the instruction-start T-state; a PSG
 * write catches the device up to it (T007). `reset` is called by `sms_machine_reset` in the fixed order VDP, PSG, pad and
 * owns the device's whole reset state (VDP reset state including U9 is T004, PSG is T007; T003 defines neither).
 * `digest` (optional) appends the device state to the machine-state digest. */
typedef struct SmsPortDevice {
  void *context;
  void (*reset)(void *context);
  uint8_t (*read)(void *context, SmsPortClass cls, uint64_t cycles);
  void (*write)(void *context, SmsPortClass cls, uint8_t value, uint64_t cycles);
  void (*digest)(void *context, SmsSha256 *sha);
} SmsPortDevice;

#define SMS_IRQ_FRAME 0x01u /* frame interrupt asserting /INT: frame pending and R1 bit 5 */
#define SMS_IRQ_LINE 0x02u  /* line interrupt asserting /INT: line pending and R0 bit 4 */

/* The VDP seam: the port device plus the scanline event hook and the interrupt level. `scanline` is called at the start
 * of every line (absolute line = frame x 262 + line, T = absolute line x 228) in time order, before the CPU runs past
 * it. T004 attaches the frame/line flag and counter logic here (the offset of those events within the line is U2; until
 * T004 resolves it they happen at the line start). `irq_sources` returns the set of /INT sources currently asserted
 * (already gated by the enable bits); the machine drives `int_line = (sources != 0)` and traces the edges. */
typedef struct SmsVdpDevice {
  SmsPortDevice port;
  void (*scanline)(void *context, uint64_t frame, uint32_t line, uint64_t cycles);
  uint8_t (*irq_sources)(void *context);
} SmsVdpDevice;

/* ---- traces --------------------------------------------------------------------------------------------------- */

typedef enum SmsIrqSource { SMS_IRQ_TRACE_FRAME = 0, SMS_IRQ_TRACE_LINE = 1, SMS_IRQ_TRACE_PAUSE = 2 } SmsIrqSource;
typedef enum SmsIrqEvent { SMS_IRQ_ASSERTED = 0, SMS_IRQ_ACCEPTED = 1, SMS_IRQ_DEASSERTED = 2 } SmsIrqEvent;

typedef struct SmsIrqTraceEntry {
  uint64_t cycles;
  uint8_t source; /* SmsIrqSource */
  uint8_t event;  /* SmsIrqEvent */
} SmsIrqTraceEntry;

typedef struct SmsMapperTraceEntry {
  uint64_t cycles;
  uint8_t reg; /* 0..3 = $FFFC..$FFFF */
  uint8_t value;
} SmsMapperTraceEntry;

/* ---- stops ---------------------------------------------------------------------------------------------------- */

typedef enum SmsStopKind {
  SMS_STOP_CYCLE = 0,          /* resumable: the requested T-state was reached */
  SMS_STOP_FRAME = 1,          /* resumable: the requested frame start was reached */
  SMS_STOP_HALT_IDLE = 2,      /* resumable diagnostic: halted, interrupts disabled, no pause input left */
  SMS_STOP_CYCLE_BUDGET = 3,   /* resumable: the --cycle-budget bound was reached first (report, not a failure) */
  SMS_STOP_Z80_ERROR = 4,      /* permanent: fail-closed Z80 outcome */
  SMS_STOP_PLATFORM_ERROR = 5  /* permanent: SMS_ERROR_* */
} SmsStopKind;

typedef struct SmsStop {
  SmsStopKind kind;
  uint64_t cycles;  /* Z80 cycle counter at the stop (an instruction boundary) */
  uint64_t frame;   /* cycles / SMS_CYCLES_PER_FRAME */
  uint16_t pc;      /* PC at the stop: the start of the next instruction (for an error, the boundary that stopped) */
  uint32_t image_identity; /* code image identity at `pc` (0 when unmapped or unavailable) */
  Z80Outcome z80_outcome;
  SmsError sms_error;
  uint16_t error_address; /* address or port of the offending access */
  uint8_t error_value;
  uint64_t error_cycles; /* instruction-start T-state of the offending access */
} SmsStop;

static inline int sms_stop_is_resumable(SmsStopKind kind) {
  return kind != SMS_STOP_Z80_ERROR && kind != SMS_STOP_PLATFORM_ERROR;
}
const char *sms_stop_kind_name(SmsStopKind kind);

/* ---- machine -------------------------------------------------------------------------------------------------- */

typedef struct SmsMachine {
  Z80Runtime rt;
  SmsMemory mem;
  uint8_t io_control; /* last value written to port $3F (composition state; T006 owns its effects) */
  SmsVdpDevice vdp;
  SmsPortDevice psg;
  SmsPortDevice pad;

  SmsInputState input;
  const SmsInputEvent *input_events;
  uint32_t input_count;
  uint32_t input_next;

  uint64_t next_line; /* absolute index of the next scanline event not yet applied */
  uint8_t irq_mask;   /* /INT sources asserted at the last sample (SMS_IRQ_* bits) */

  int stop_on_halt_idle; /* report SMS_STOP_HALT_IDLE (resumable) instead of spinning while halted with no wake source */
  int dead;              /* permanent stop latched */
  SmsStop dead_stop;

  SmsIrqTraceEntry *irq_trace;
  uint32_t irq_capacity, irq_count, irq_dropped;
  SmsMapperTraceEntry *mapper_trace;
  uint32_t mapper_capacity, mapper_count, mapper_dropped;
} SmsMachine;

/* Binds the cartridge (see sms_memory_init), wires the Z80 host callbacks and performs `sms_machine_reset` with no
 * devices attached. Attach devices (assign `vdp`, `psg`, `pad`) and call `sms_machine_reset` again so their resets run. */
SmsError sms_machine_init(SmsMachine *m, const uint8_t *rom, uint32_t rom_size, SmsMapperFamily family);

/* Deterministic T001 post-BIOS reset: `z80_reset`, work RAM ($C000 = $AB), mapper, memory control $AB, I/O control
 * $FF, scheduler state, traces cleared, permanent stop cleared; then the device resets in the fixed order VDP, PSG,
 * pad, and finally the first interrupt sample. Devices, input schedule, trace buffers and options are kept. */
void sms_machine_reset(SmsMachine *m);

/* The schedule must outlive the machine; events are applied at frame starts (contract section 11). */
void sms_machine_set_input(SmsMachine *m, const SmsInputEvent *events, uint32_t count);
void sms_machine_set_traces(SmsMachine *m, SmsIrqTraceEntry *irq, uint32_t irq_capacity, SmsMapperTraceEntry *mapper,
                            uint32_t mapper_capacity);

/* Run API. Resumable stops leave the machine ready for another call; a permanent stop is returned unchanged forever
 * (until `sms_machine_reset`). `t_state` / frame targets are absolute; a target already reached does no work. */
SmsStop sms_run_until_cycle(SmsMachine *m, uint64_t t_state);
SmsStop sms_run_until_frame(SmsMachine *m, uint64_t frame);
/* Headless form: stops at the first boundary with `cycles >= cycle_budget` and/or `cycles >= frames x 59,736`
 * (SMS_NO_LIMIT for none). With both reached at the same boundary the frame wins (SMS_STOP_FRAME); otherwise
 * SMS_STOP_FRAME or SMS_STOP_CYCLE_BUDGET. With no limit it runs until a stop that is not a CYCLE. */
SmsStop sms_run_bounded(SmsMachine *m, uint64_t cycle_budget, uint64_t frames);

/* SHA-256 over the canonical machine state: Z80 state (ABI field order, deadline excluded), work RAM, cartridge RAM,
 * mapper registers, memory control, I/O control, latched error, scheduler and input state, then each attached device's
 * `digest`. Identical across runs and across any slicing of a run. */
void sms_machine_digest(const SmsMachine *m, uint8_t out[32]);

/* Internals exposed for the unit tests and the viewer's pacing: the T-state of the next scheduled device event. */
static inline uint64_t sms_next_event_cycles(const SmsMachine *m) { return m->next_line * SMS_CYCLES_PER_LINE; }

#ifdef __cplusplus
}
#endif
#endif
