/* SEG-062-T003 (ADR 0128): the Sega CD machine core: bus views of both processor instances, the generated-code ABI, the
 * CPU-owned exception/interrupt rules bound per processor, and the deterministic single-threaded scheduler.
 * Contract: docs/architecture/sega-cd-machine-contract.md sections 2-4, 10-12. */
#include "scd_runtime.h"

#include <string.h>

static const char *const scd_stop_names[SCD_STOP_CLASS_COUNT] = {
    "none", "unmapped", "bios_absent", "open_bus_read", "wordram_cell_image_unsupported", "wordram_dot_image_unsupported",
    "backup_ram_unsupported", "pcm_unsupported", "gate_array_register_unsupported", "vdp_access_unsupported", "vdp_dma_unsupported",
    "address_error", "entry_not_compiled", "return_target_not_compiled", "code_bytes_mismatch", "instruction_unsupported",
    "exception_vector_not_compiled", "exception_stack_invalid", "trace_unsupported", "interrupt_source_unsupported",
    "cdc_destination_unsupported", "cdd_command_unsupported", "cd_read_audio_track", "cd_read_failed", "internal"};

const char *scd_stop_class_name(ScdStopClass stop_class) {
  return (unsigned)stop_class < SCD_STOP_CLASS_COUNT ? scd_stop_names[stop_class] : "unknown";
}

static void scd_fnv(uint64_t *hash, uint64_t value) {
  unsigned i;
  for (i = 0; i < 8U; ++i) {
    *hash ^= (uint8_t)(value >> (8U * i));
    *hash *= UINT64_C(0x100000001B3);
  }
}

void scd_log(ScdMachine *m, uint32_t kind, uint32_t address, uint32_t value) {
  scd_fnv(&m->device_log, m->now);
  scd_fnv(&m->device_log, kind);
  scd_fnv(&m->device_log, address);
  scd_fnv(&m->device_log, value);
}

static int scd_fail_access(ScdMachine *m, uint32_t cpu, ScdStopClass stop_class, uint32_t pc, uint32_t address, uint32_t width,
                           int is_write) {
  if (m->stop.stop_class == SCD_STOP_NONE) {
    m->stop.stop_class = stop_class;
    m->stop.cpu = cpu;
    m->stop.pc = pc;
    m->stop.address = address;
    m->stop.width = width;
    m->stop.is_write = is_write ? 1U : 0U;
    m->stop.tick = m->now;
  }
  return 1;
}

int scd_fail(ScdMachine *m, uint32_t cpu, ScdStopClass stop_class, uint32_t pc, uint32_t detail) {
  return scd_fail_access(m, cpu, stop_class, pc, detail, 0U, 0);
}

/* ---- memories --------------------------------------------------------------------------------------------------------- */

static uint32_t scd_ram_get(const uint8_t *ram, uint32_t offset, uint32_t width) {
  return width == 1U ? ram[offset] : (uint32_t)(((uint32_t)ram[offset] << 8U) | ram[offset + 1U]);
}

static void scd_ram_put(uint8_t *ram, uint32_t offset, uint32_t width, uint32_t value) {
  if (width == 1U) {
    ram[offset] = (uint8_t)value;
  } else {
    ram[offset] = (uint8_t)(value >> 8U);
    ram[offset + 1U] = (uint8_t)value;
  }
}

/* An access with no hardware effect (GPGX "unused" areas): a write is dropped and logged; a read would return the processor's
 * prefetched instruction word (open bus), which is not modelled - the typed stop open_bus_read. */
static int scd_unused(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  if (!is_write) return scd_fail_access(m, cpu, SCD_STOP_OPEN_BUS_READ, pc, address, width, 0);
  ++m->ignored_writes;
  scd_log(m, 0x51U, address, *value);
  return 0;
}

/* Side-effect-free view of plain RAM for code guards and exception frames: the byte at `address`, or NULL when the address is
 * not plain RAM of `cpu` (a device, an absent region, or Word RAM). Mirrors follow the bus maps below. */
static uint8_t *scd_ram_byte(ScdMachine *m, uint32_t cpu, uint32_t address) {
  address &= UINT32_C(0x00FFFFFF);
  if (cpu == SCD_MAIN) {
    if (address >= UINT32_C(0x00E00000)) return &m->work_ram[address & 0xFFFFU];
    if (address < UINT32_C(0x00200000) && (address & UINT32_C(0x3FFFF)) >= UINT32_C(0x20000) && m->ga.prg_window)
      return &m->prg_ram[(uint32_t)m->ga.prg_bank * UINT32_C(0x20000) + (address & UINT32_C(0x1FFFF))];
    return NULL;
  }
  if ((address & UINT32_C(0xFFFFF)) < SCD_PRG_RAM_BYTES) return &m->prg_ram[address & UINT32_C(0x7FFFF)];
  return NULL;
}

/* One byte- or word-wide bus cycle of the main processor (contract section 3; mirrors as in GPGX scd_init). */
static int scd_main_bus(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  if (address < UINT32_C(0x00200000)) { /* expansion port: 128 KiB BIOS + PRG-RAM window, mirrored every 256 KiB */
    const uint32_t a = address & UINT32_C(0x3FFFF);
    uint32_t offset;
    if (a < UINT32_C(0x20000)) {
      if (is_write) return scd_unused(m, SCD_MAIN, address, width, 1, value, pc); /* ROM: the write has no effect */
      return scd_fail_access(m, SCD_MAIN, SCD_STOP_BIOS_ABSENT, pc, address, width, 0);
    }
    if (!m->ga.prg_window) return scd_unused(m, SCD_MAIN, address, width, is_write, value, pc);
    offset = (uint32_t)m->ga.prg_bank * UINT32_C(0x20000) + (a - UINT32_C(0x20000));
    if (is_write) scd_ram_put(m->prg_ram, offset, width, *value);
    else *value = scd_ram_get(m->prg_ram, offset, width);
    return 0;
  }
  if (address < UINT32_C(0x00400000)) return scd_word_ram_access(m, SCD_MAIN, address & UINT32_C(0x23FFFF), width, is_write, value, pc);
  if (address >= UINT32_C(0x00A12000) && address < UINT32_C(0x00A12100))
    return scd_ga_main_access(m, address & 0x3FU, width, is_write, value, pc);
  if (address >= UINT32_C(0x00C00000) && address < UINT32_C(0x00C00008)) return scd_vdp_access(m, address, width, is_write, value, pc);
  if (address >= UINT32_C(0x00E00000)) { /* 64 KiB work RAM mirrored through $E00000-$FFFFFF */
    const uint32_t offset = address & 0xFFFFU;
    if (is_write) scd_ram_put(m->work_ram, offset, width, *value);
    else *value = scd_ram_get(m->work_ram, offset, width);
    return 0;
  }
  return scd_fail_access(m, SCD_MAIN, SCD_STOP_UNMAPPED, pc, address, width, is_write);
}

/* One byte- or word-wide bus cycle of the sub processor (contract section 4): only A1-A19 decode, so the 1 MiB map repeats. */
static int scd_sub_bus(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  const uint32_t a = address & UINT32_C(0xFFFFF);
  if (a < SCD_PRG_RAM_BYTES) {
    if (is_write) {
      if (a < (uint32_t)m->ga.write_protect * UINT32_C(0x200)) {
        ++m->prg_write_protect_drops; /* contract section 4: the write is ignored (V2) */
        scd_log(m, 0x50U, address, *value);
        return 0;
      }
      scd_ram_put(m->prg_ram, a, width, *value);
    } else {
      *value = scd_ram_get(m->prg_ram, a, width);
    }
    return 0;
  }
  if (a < UINT32_C(0xE0000)) return scd_word_ram_access(m, SCD_SUB, a, width, is_write, value, pc);
  if (a < UINT32_C(0xF0000)) return scd_fail_access(m, SCD_SUB, SCD_STOP_BACKUP_RAM_UNSUPPORTED, pc, address, width, is_write);
  if (!(a & 0x8000U)) return scd_fail_access(m, SCD_SUB, SCD_STOP_PCM_UNSUPPORTED, pc, address, width, is_write);
  return scd_ga_sub_access(m, a & 0x1FFU, width, is_write, value, pc);
}

static int scd_bus(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  return cpu == SCD_MAIN ? scd_main_bus(m, address, width, is_write, value, pc) : scd_sub_bus(m, address, width, is_write, value, pc);
}

/* The VDP device owns its 32-bit port semantics (a long control-port write is one two-word command): long main-CPU accesses to
 * the VDP ports go to it whole instead of as two bus cycles. */
static int scd_is_vdp_long(uint32_t cpu, uint32_t address, uint32_t width) {
  return cpu == SCD_MAIN && width == 4U && address >= UINT32_C(0x00C00000) && address < UINT32_C(0x00C00008);
}

int scd_read(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, uint32_t *value, uint32_t pc) {
  address &= UINT32_C(0x00FFFFFF);
  if (width != 1U && (address & 1U) != 0U) return scd_fail_access(m, cpu, SCD_STOP_ADDRESS_ERROR, pc, address, width, 0);
  if (scd_is_vdp_long(cpu, address, width)) return scd_vdp_access(m, address, 4U, 0, value, pc);
  if (width == 4U) {
    uint32_t high = 0U, low = 0U;
    if (scd_bus(m, cpu, address, 2U, 0, &high, pc) != 0) return 1;
    if (scd_bus(m, cpu, (address + 2U) & UINT32_C(0x00FFFFFF), 2U, 0, &low, pc) != 0) return 1;
    *value = (high << 16U) | (low & 0xFFFFU);
    return 0;
  }
  return scd_bus(m, cpu, address, width, 0, value, pc);
}

int scd_write(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, uint32_t value, uint32_t pc) {
  address &= UINT32_C(0x00FFFFFF);
  if (width != 1U && (address & 1U) != 0U) return scd_fail_access(m, cpu, SCD_STOP_ADDRESS_ERROR, pc, address, width, 1);
  if (scd_is_vdp_long(cpu, address, width)) return scd_vdp_access(m, address, 4U, 1, &value, pc);
  if (width == 4U) {
    uint32_t high = value >> 16U, low = value & 0xFFFFU;
    if (scd_bus(m, cpu, address, 2U, 1, &high, pc) != 0) return 1;
    return scd_bus(m, cpu, (address + 2U) & UINT32_C(0x00FFFFFF), 2U, 1, &low, pc);
  }
  value &= width == 1U ? 0xFFU : 0xFFFFU;
  return scd_bus(m, cpu, address, width, 1, &value, pc);
}

int scd_code_guard(ScdMachine *m, uint32_t cpu, uint32_t address, const uint8_t *expected, uint32_t length) {
  uint32_t i;
  for (i = 0; i < length; ++i) {
    const uint8_t *live = scd_ram_byte(m, cpu, address + i);
    if (live == NULL || *live != expected[i]) return scd_fail(m, cpu, SCD_STOP_CODE_BYTES_MISMATCH, address, address + i);
  }
  return 0;
}

int scd_compiled(const ScdMachine *m, uint32_t cpu, uint32_t address) {
  const ScdProcessorProgram *program = &m->program->processors[cpu];
  size_t lo = 0U, hi = program->entry_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2U;
    if (program->entries[mid] == address) return 1;
    if (program->entries[mid] < address) lo = mid + 1U;
    else hi = mid;
  }
  return 0;
}

static void scd_trace(ScdCpu *c) {
  unsigned i;
  scd_fnv(&c->trace, c->pc);
  scd_fnv(&c->trace, c->sr);
  for (i = 0; i < 8U; ++i) {
    scd_fnv(&c->trace, c->d[i]);
    scd_fnv(&c->trace, c->a[i]);
  }
}

int scd_retire(ScdMachine *m, uint32_t cpu, uint32_t cycles) {
  ScdCpu *c = &m->cpu[cpu];
  c->cycles += cycles;
  c->retired += 1U;
  c->next += (uint64_t)cycles * c->cycle_ticks;
  scd_trace(c);
  return 0;
}

void scd_enter_stop(ScdMachine *m, uint32_t cpu) { m->cpu[cpu].irq.stopped = 1U; }

/* ---- exceptions: the CPU-owned core (exception_core.h) bound to one processor -------------------------------------------- */

typedef struct ScdExceptionContext {
  ScdMachine *m;
  uint32_t cpu;
  uint32_t pc;
  ScdStopClass vector_failure;
} ScdExceptionContext;

static int scd_hook_validate(void *context, uint32_t base, uint32_t length, SegarecompM68kStackDirection direction) {
  ScdExceptionContext *x = (ScdExceptionContext *)context;
  uint32_t i;
  for (i = 0; i < length; ++i) {
    const uint32_t address = (base + i) & UINT32_C(0x00FFFFFF);
    if (scd_ram_byte(x->m, x->cpu, address) == NULL) return 0;
    /* A frame write into the sub's write-protected PRG RAM would be silently dropped: refuse it (fail closed). */
    if (direction == SEGARECOMP_M68K_STACK_WRITE && x->cpu == SCD_SUB &&
        (address & UINT32_C(0xFFFFF)) < (uint32_t)x->m->ga.write_protect * 0x200U)
      return 0;
  }
  return 1;
}

static int scd_hook_stack_read(void *context, uint32_t address, uint32_t size, uint32_t *value) {
  ScdExceptionContext *x = (ScdExceptionContext *)context;
  return scd_read(x->m, x->cpu, address, size, value, x->pc) == 0;
}

static void scd_hook_frame_write(void *context, uint32_t address, uint32_t size, uint32_t value) {
  ScdExceptionContext *x = (ScdExceptionContext *)context;
  uint32_t i;
  for (i = 0; i < size; ++i) {
    uint8_t *byte = scd_ram_byte(x->m, x->cpu, address + i);
    if (byte != NULL) *byte = (uint8_t)(value >> (8U * (size - 1U - i)));
  }
}

static SegarecompM68kVectorResolution scd_hook_vector(void *context, uint32_t vector, uint32_t *handler) {
  ScdExceptionContext *x = (ScdExceptionContext *)context;
  uint32_t i, value = 0U;
  if (x->cpu == SCD_MAIN) { /* the main vectors live in the absent BIOS ROM (contract section 10) */
    x->vector_failure = SCD_STOP_BIOS_ABSENT;
    return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
  }
  for (i = 0; i < 4U; ++i) value = (value << 8U) | x->m->prg_ram[vector * 4U + i]; /* PRG RAM $000000-$0003FF */
  if (!scd_compiled(x->m, x->cpu, value & UINT32_C(0x00FFFFFF))) {
    x->vector_failure = SCD_STOP_EXCEPTION_VECTOR_NOT_COMPILED;
    return SEGARECOMP_M68K_VECTOR_UNREPRESENTABLE;
  }
  *handler = value & UINT32_C(0x00FFFFFF);
  return SEGARECOMP_M68K_VECTOR_HANDLER;
}

static SegarecompM68kMachineHooks scd_hooks(ScdExceptionContext *context) {
  SegarecompM68kMachineHooks hooks;
  memset(&hooks, 0, sizeof hooks);
  hooks.context = context;
  hooks.validate_stack_extent = scd_hook_validate;
  hooks.stack_read = scd_hook_stack_read;
  hooks.frame_write = scd_hook_frame_write;
  hooks.resolve_vector = scd_hook_vector;
  return hooks;
}

static SegarecompM68kCpuBinding scd_binding(ScdCpu *c) {
  SegarecompM68kCpuBinding binding;
  binding.sr = &c->sr;
  binding.active_sp = &c->a[7];
  binding.inactive_sp = &c->usp;
  binding.pc = &c->pc;
  return binding;
}

static int scd_exception_status(ScdMachine *m, uint32_t cpu, SegarecompM68kExceptionStatus status, const ScdExceptionContext *x,
                                uint32_t pc, uint32_t detail) {
  switch (status) {
  case SEGARECOMP_M68K_EXCEPTION_OK: return 0;
  case SEGARECOMP_M68K_EXCEPTION_STACK_INVALID: return scd_fail(m, cpu, SCD_STOP_EXCEPTION_STACK_INVALID, pc, detail);
  case SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE:
    return scd_fail(m, cpu, x->vector_failure != SCD_STOP_NONE ? x->vector_failure : SCD_STOP_EXCEPTION_VECTOR_NOT_COMPILED, pc, detail);
  case SEGARECOMP_M68K_EXCEPTION_TRACE_DEFERRED: return scd_fail(m, cpu, SCD_STOP_TRACE_UNSUPPORTED, pc, detail);
  case SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED:
    return m->stop.stop_class != SCD_STOP_NONE ? 1 : scd_fail(m, cpu, SCD_STOP_EXCEPTION_STACK_INVALID, pc, detail);
  case SEGARECOMP_M68K_EXCEPTION_BAD_BINDING: break;
  }
  return scd_fail(m, cpu, SCD_STOP_INTERNAL, pc, detail);
}

int scd_raise(ScdMachine *m, uint32_t cpu, uint32_t vector, uint32_t stacked_pc, uint32_t entry_cycles, uint32_t pc) {
  ScdCpu *c = &m->cpu[cpu];
  ScdExceptionContext x = {m, cpu, pc, SCD_STOP_NONE};
  SegarecompM68kMachineHooks hooks = scd_hooks(&x);
  SegarecompM68kCpuBinding binding = scd_binding(c);
  uint32_t handler = 0U;
  SegarecompM68kExceptionStatus status;
  if (entry_cycles == 0U) return scd_fail(m, cpu, SCD_STOP_INTERNAL, pc, vector);
  status = segarecomp_m68k_exception_enter(&hooks, &binding, vector, stacked_pc,
                                           (uint16_t)(UINT16_C(0xFFFF) & (uint16_t)~SEGARECOMP_M68K_SR_TRACE),
                                           SEGARECOMP_M68K_SR_SUPERVISOR, &handler);
  if (scd_exception_status(m, cpu, status, &x, pc, vector) != 0) return 1;
  c->pc = handler;
  return scd_retire(m, cpu, entry_cycles);
}

int scd_rte(ScdMachine *m, uint32_t cpu, uint32_t *restored_pc, uint32_t pc) {
  ScdExceptionContext x = {m, cpu, pc, SCD_STOP_NONE};
  SegarecompM68kMachineHooks hooks = scd_hooks(&x);
  SegarecompM68kCpuBinding binding = scd_binding(&m->cpu[cpu]);
  return scd_exception_status(m, cpu, segarecomp_m68k_exception_return(&hooks, &binding, restored_pc), &x, pc, 0U);
}

int scd_rtr(ScdMachine *m, uint32_t cpu, uint32_t *restored_pc, uint32_t pc) {
  ScdExceptionContext x = {m, cpu, pc, SCD_STOP_NONE};
  SegarecompM68kMachineHooks hooks = scd_hooks(&x);
  SegarecompM68kCpuBinding binding = scd_binding(&m->cpu[cpu]);
  return scd_exception_status(m, cpu, segarecomp_m68k_return_restore_ccr(&hooks, &binding, restored_pc), &x, pc, 0U);
}

/* ---- interrupts --------------------------------------------------------------------------------------------------------- */

/* The sub request level: the highest pending level whose enable bit is set (contract section 10). */
static uint32_t scd_sub_request_level(const ScdMachine *m) {
  uint32_t level;
  const uint32_t active = (uint32_t)(m->ga.pending & m->ga.interrupt_enable);
  for (level = 7U; level >= 1U; --level)
    if ((active >> level) & 1U) return level;
  return 0U;
}

void scd_raise_sub_level(ScdMachine *m, uint32_t level) {
  m->ga.pending = (uint8_t)(m->ga.pending | (1U << level));
  scd_log(m, 0x60U, level, m->ga.interrupt_enable);
}

void scd_update_sub_interrupt(ScdMachine *m) { (void)m; /* the level is sampled at every sub boundary */ }

/* Accept a recognized sub interrupt at this boundary. Returns 1 when an interrupt was taken, 0 when none, -1 on a stop. */
static int scd_sub_accept_interrupt(ScdMachine *m) {
  ScdCpu *c = &m->cpu[SCD_SUB];
  uint32_t level, handler = 0U;
  ScdExceptionContext x = {m, SCD_SUB, c->pc, SCD_STOP_NONE};
  SegarecompM68kMachineHooks hooks = scd_hooks(&x);
  SegarecompM68kCpuBinding binding = scd_binding(c);
  SegarecompM68kExceptionStatus status;
  segarecomp_m68k_interrupt_sample(&c->irq, scd_sub_request_level(m));
  level = segarecomp_m68k_interrupt_recognized_level(&c->irq, c->sr);
  if (level == 0U) return 0;
  if (level == 1U || level == 6U || level == 7U) {
    scd_fail(m, SCD_SUB, SCD_STOP_INTERRUPT_SOURCE_UNSUPPORTED, c->pc, level);
    return -1;
  }
  status = segarecomp_m68k_interrupt_enter(&hooks, &binding, &c->irq, level,
                                           segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR, level, 0U), c->pc,
                                           &handler);
  if (scd_exception_status(m, SCD_SUB, status, &x, c->pc, 24U + level) != 0) return -1;
  /* Acknowledge (autovector): the pending level clears; level 2 also clears IFL2 (contract section 10). */
  m->ga.pending = (uint8_t)(m->ga.pending & ~(1U << level));
  if (level == 2U) m->ga.ifl2 = 0U;
  m->sub_interrupts_taken[level] += 1U;
  scd_log(m, 0x61U, level, handler);
  c->pc = handler;
  scd_retire(m, SCD_SUB, segarecomp_m68k_exception_entry_cycles(24U + level));
  return 1;
}

/* ---- scheduler ---------------------------------------------------------------------------------------------------------- */

static int scd_sub_held(const ScdMachine *m) { return m->ga.sub_halted || m->ga.sub_wait; }

/* The highest level any enabled sub source could still raise (for the STOP wake rule). */
static uint32_t scd_sub_max_wake_level(const ScdMachine *m) {
  uint32_t level, best = 0U;
  for (level = 2U; level <= 5U; ++level) {
    if (!((m->ga.interrupt_enable >> level) & 1U)) continue;
    if ((m->ga.pending >> level) & 1U) best = level;
    else if (level == 2U && !m->cpu[SCD_MAIN].irq.stopped) best = level; /* main may still raise IFL2 */
    else if (level != 2U && scd_devices_can_raise(m, level)) best = level;
  }
  return best;
}

/* The tick at which processor `cpu` can execute next, or UINT64_MAX if it cannot run now. */
static uint64_t scd_cpu_ready(ScdMachine *m, uint32_t cpu) {
  ScdCpu *c = &m->cpu[cpu];
  if (cpu == SCD_SUB && scd_sub_held(m)) return UINT64_MAX;
  if (!c->irq.stopped) return c->next;
  if (cpu == SCD_SUB) {
    const uint32_t level = scd_sub_request_level(m);
    const uint32_t mask = (uint32_t)((c->sr & SEGARECOMP_M68K_SR_INTERRUPT_MASK) >> 8U);
    if (level > mask || level == 7U) return c->next > m->now ? c->next : m->now;
  }
  return UINT64_MAX;
}

static int scd_machine_idle(const ScdMachine *m) {
  const ScdCpu *main = &m->cpu[SCD_MAIN];
  const ScdCpu *sub = &m->cpu[SCD_SUB];
  if (!main->irq.stopped) return 0;
  if (scd_sub_held(m)) return 1;
  if (!sub->irq.stopped) return 0;
  return !segarecomp_m68k_stop_wake_possible(&sub->irq, scd_sub_max_wake_level(m), sub->sr);
}

ScdRunResult scd_machine_run(ScdMachine *m, uint64_t deadline) {
  for (;;) {
    uint64_t ready_main, ready_sub, ready, device;
    uint32_t cpu;
    if (m->stop.stop_class != SCD_STOP_NONE) return SCD_RUN_STOPPED;
    ready_main = scd_cpu_ready(m, SCD_MAIN);
    ready_sub = scd_cpu_ready(m, SCD_SUB);
    cpu = ready_sub < ready_main ? SCD_SUB : SCD_MAIN;
    ready = ready_sub < ready_main ? ready_sub : ready_main;
    device = scd_devices_next_event(m);
    if (ready == UINT64_MAX && scd_machine_idle(m)) return SCD_RUN_IDLE;
    if (device != UINT64_MAX && device <= ready) {
      if (device >= deadline) return SCD_RUN_DEADLINE;
      m->now = device;
      scd_devices_advance(m, device);
      continue;
    }
    if (ready == UINT64_MAX) return SCD_RUN_IDLE;
    if (ready >= deadline) return SCD_RUN_DEADLINE;
    m->now = ready;
    m->cpu[cpu].next = ready;
    if (cpu == SCD_SUB) {
      const int taken = scd_sub_accept_interrupt(m);
      if (taken < 0) return SCD_RUN_STOPPED;
      if (taken > 0) continue;
    }
    if (m->instruction_budget == 0U) return SCD_RUN_BUDGET;
    m->instruction_budget -= 1U;
    if (cpu == SCD_SUB) {
      /* A sub access to 2M Word RAM it does not own withholds DTACK: the instruction is abandoned with the processor's registers
       * as they were before it (GPGX s68k_pulse_wait / s68k_clear_wait; memory written earlier in the instruction stays) and
       * retried from its start once the main processor hands Word RAM over. */
      const ScdCpu before = m->cpu[SCD_SUB];
      if (m->program->processors[cpu].dispatch(m) != 0) {
        if (m->stop.stop_class != SCD_STOP_NONE || !m->sub_wait_request) {
          if (m->stop.stop_class == SCD_STOP_NONE) scd_fail(m, SCD_SUB, SCD_STOP_INTERNAL, m->cpu[SCD_SUB].pc, 0U);
          return SCD_RUN_STOPPED;
        }
        m->cpu[SCD_SUB] = before;
        m->sub_wait_request = 0U;
        m->ga.sub_wait = 1U;
        m->sub_waits += 1U;
        scd_log(m, 0x14U, before.pc, 0U);
      }
      continue;
    }
    if (m->program->processors[cpu].dispatch(m) != 0) return SCD_RUN_STOPPED;
  }
}

uint64_t scd_machine_time(const ScdMachine *m) { return m->now; }

/* ---- profile ------------------------------------------------------------------------------------------------------------ */

int scd_machine_init(ScdMachine *m, const ScdProgram *program, ScdDisc *disc, uint64_t instruction_budget) {
  uint8_t area[16U * 2048U];
  uint32_t s, k;
  memset(m, 0, sizeof *m);
  m->program = program;
  m->disc = disc;
  m->instruction_budget = instruction_budget;
  m->device_log = UINT64_C(0xCBF29CE484222325);
  for (k = 0; k < 2U; ++k) m->cpu[k].trace = UINT64_C(0xCBF29CE484222325);
  /* The system area of track 1 (contract section 9): read through the strict-C11 sector reader, like a BIOS would. */
  if (disc->track_count == 0U) return 1;
  for (s = 0; s < 16U; ++s)
    if (scd_disc_read_user_data(disc, disc->tracks[0].first_lba + s, area + s * 2048U) != SCD_DISC_OK) return 1;
  {
    const ScdProcessorProgram *main = &program->processors[SCD_MAIN];
    const ScdProcessorProgram *sub = &program->processors[SCD_SUB];
    if (main->image_disc_offset + main->image_length > sizeof area || sub->image_disc_offset + sub->image_length > sizeof area ||
        main->image_base < UINT32_C(0x00FF0000) || main->image_base + main->image_length > UINT32_C(0x01000000) ||
        sub->image_base + sub->image_length > SCD_PRG_RAM_BYTES)
      return 1;
    memcpy(&m->work_ram[main->image_base - UINT32_C(0x00FF0000)], area + main->image_disc_offset, main->image_length);
    memcpy(&m->prg_ram[sub->image_base], area + sub->image_disc_offset, sub->image_length);
    m->cpu[SCD_MAIN].pc = main->entry_pc;
    m->cpu[SCD_MAIN].a[7] = program->main_initial_ssp;
    m->cpu[SCD_MAIN].sr = UINT16_C(0x2700);
    m->cpu[SCD_SUB].sr = UINT16_C(0x2700);
    m->cpu[SCD_SUB].pc = sub->entry_pc;
  }
  m->cpu[SCD_MAIN].cycle_ticks = SCD_MAIN_CYCLE_TICKS;
  m->cpu[SCD_SUB].cycle_ticks = SCD_SUB_CYCLE_TICKS;
  /* Power-on Gate Array state (contract sections 5, 6; GPGX scd_reset(1)): the sub held in reset and halted with SBRQ reading 1, the
   * PRG-RAM window mapped, 2M mode with main owning Word RAM. */
  m->ga.ctl = 0x02U;
  m->ga.sub_halted = 1U;
  m->ga.prg_window = 1U;
  m->ga.mode = SCD_MODE_RET;
  scd_devices_power_on(m);
  return scd_vdp_init(m);
}

void scd_machine_release(ScdMachine *m) { scd_vdp_release(m); }
