#include "sms_machine.h"

#include <string.h>

/* ---- trace helpers -------------------------------------------------------------------------------------------- */

static void irq_trace(SmsMachine *m, uint64_t cycles, SmsIrqSource source, SmsIrqEvent event) {
  if (m->irq_trace != NULL && m->irq_count < m->irq_capacity) {
    SmsIrqTraceEntry *e = &m->irq_trace[m->irq_count++];
    e->cycles = cycles;
    e->source = (uint8_t)source;
    e->event = (uint8_t)event;
  } else {
    ++m->irq_dropped;
  }
}

static void mapper_trace(SmsMachine *m, uint64_t cycles, uint8_t reg, uint8_t value) {
  if (m->mapper_trace != NULL && m->mapper_count < m->mapper_capacity) {
    SmsMapperTraceEntry *e = &m->mapper_trace[m->mapper_count++];
    e->cycles = cycles;
    e->reg = reg;
    e->value = value;
  } else {
    ++m->mapper_dropped;
  }
}

const char *sms_stop_kind_name(SmsStopKind kind) {
  switch (kind) {
    case SMS_STOP_CYCLE: return "cycle";
    case SMS_STOP_FRAME: return "frame";
    case SMS_STOP_HALT_IDLE: return "halt_idle";
    case SMS_STOP_CYCLE_BUDGET: return "cycle_budget";
    case SMS_STOP_Z80_ERROR: return "z80_error";
    case SMS_STOP_PLATFORM_ERROR: return "platform_error";
  }
  return "invalid";
}

/* ---- interrupt wiring ----------------------------------------------------------------------------------------- */

/* Re-samples the VDP /INT sources, traces each source edge at time `t` and drives the Z80 `int_line` level. */
static void sample_irq(SmsMachine *m, uint64_t t) {
  const uint8_t mask = m->vdp.irq_sources != NULL
                           ? (uint8_t)(m->vdp.irq_sources(m->vdp.port.context) & (SMS_IRQ_FRAME | SMS_IRQ_LINE))
                           : 0u;
  const uint8_t changed = (uint8_t)(mask ^ m->irq_mask);
  if (changed != 0u) {
    if ((changed & SMS_IRQ_FRAME) != 0u)
      irq_trace(m, t, SMS_IRQ_TRACE_FRAME, (mask & SMS_IRQ_FRAME) != 0u ? SMS_IRQ_ASSERTED : SMS_IRQ_DEASSERTED);
    if ((changed & SMS_IRQ_LINE) != 0u)
      irq_trace(m, t, SMS_IRQ_TRACE_LINE, (mask & SMS_IRQ_LINE) != 0u ? SMS_IRQ_ASSERTED : SMS_IRQ_DEASSERTED);
    m->irq_mask = mask;
  }
  m->rt.state.int_line = mask != 0u ? 1u : 0u;
}

static void apply_input_at_frame_start(SmsMachine *m, uint64_t frame, uint64_t t) {
  while (m->input_next < m->input_count && m->input_events[m->input_next].frame <= frame) {
    const SmsInputEvent *ev = &m->input_events[m->input_next++];
    const uint8_t was_pressed = m->input.pause;
    m->input.p1 = ev->p1;
    m->input.p2 = ev->p2;
    m->input.pause = ev->pause;
    if (was_pressed == 0u && ev->pause != 0u) { /* NMI is an edge: one per press, holding does not repeat */
      m->rt.state.nmi_pending = 1u;
      irq_trace(m, t, SMS_IRQ_TRACE_PAUSE, SMS_IRQ_ASSERTED);
    }
  }
}

/* Applies every scanline event with T_event <= `cycles`, in time order (U11: device state is advanced through every
 * event at or before an access before the access). */
static void advance_events(SmsMachine *m, uint64_t cycles) {
  while (m->mem.error == SMS_OK && sms_next_event_cycles(m) <= cycles) {
    const uint64_t t = sms_next_event_cycles(m);
    const uint64_t frame = m->next_line / SMS_LINES_PER_FRAME;
    const uint32_t line = (uint32_t)(m->next_line % SMS_LINES_PER_FRAME);
    if (line == 0u) apply_input_at_frame_start(m, frame, t);
    if (m->vdp.scanline != NULL) m->vdp.scanline(m->vdp.port.context, frame, line, t);
    sample_irq(m, t);
    ++m->next_line;
  }
}

/* The Z80 ABI cannot abort from inside a callback; lowering the deadline ends the run at the next instruction boundary. */
static void request_stop(SmsMachine *m) { m->rt.state.deadline = 0; }

/* ---- Z80 host callbacks --------------------------------------------------------------------------------------- */

static uint8_t host_read(void *context, uint16_t address, uint64_t cycles) {
  SmsMachine *m = (SmsMachine *)context;
  uint8_t value;
  advance_events(m, cycles);
  value = sms_memory_read(&m->mem, address, cycles);
  if (m->mem.error != SMS_OK) request_stop(m);
  return value;
}

static void host_write(void *context, uint16_t address, uint8_t value, uint64_t cycles) {
  SmsMachine *m = (SmsMachine *)context;
  advance_events(m, cycles);
  sms_memory_write(&m->mem, address, value, cycles);
  if (m->mem.error != SMS_OK) {
    request_stop(m);
    return;
  }
  if (m->mem.family == SMS_MAPPER_SEGA && address >= SMS_MAPPER_REG_BASE)
    mapper_trace(m, cycles, (uint8_t)(address - SMS_MAPPER_REG_BASE), value);
}

static void unimplemented(SmsMachine *m, uint16_t port, uint8_t value, uint64_t cycles) {
  sms_memory_latch_error(&m->mem, SMS_ERROR_PORT_UNIMPLEMENTED, port, value, cycles);
  request_stop(m);
}

static const SmsPortDevice *device_for(const SmsMachine *m, SmsPortOwner owner) {
  switch (owner) {
    case SMS_OWNER_VDP: return &m->vdp.port;
    case SMS_OWNER_PSG: return &m->psg;
    case SMS_OWNER_PAD: return &m->pad;
    default: return NULL;
  }
}

static uint8_t host_io_in(void *context, uint16_t port, uint64_t cycles) {
  SmsMachine *m = (SmsMachine *)context;
  const SmsPortClass cls = sms_port_decode(port, 0);
  const SmsPortOwner owner = sms_port_owner(cls);
  const SmsPortDevice *dev;
  uint8_t value;
  advance_events(m, cycles);
  if (m->mem.error != SMS_OK) return 0xFFu;
  if (owner == SMS_OWNER_NONE) return 0xFFu; /* reads of $00-$3F */
  if (owner == SMS_OWNER_PAD && sms_memory_io_disabled(&m->mem)) return 0xFFu; /* port $3E bit 2: $C0-$FF read $FF */
  dev = device_for(m, owner);
  if (dev == NULL || dev->read == NULL) {
    unimplemented(m, port, 0xFFu, cycles);
    return 0xFFu;
  }
  value = dev->read(dev->context, cls, cycles);
  sample_irq(m, cycles); /* a status read acknowledges and may deassert /INT */
  return value;
}

static void host_io_out(void *context, uint16_t port, uint8_t value, uint64_t cycles) {
  SmsMachine *m = (SmsMachine *)context;
  const SmsPortClass cls = sms_port_decode(port, 1);
  const SmsPortOwner owner = sms_port_owner(cls);
  const SmsPortDevice *dev;
  advance_events(m, cycles);
  if (m->mem.error != SMS_OK) return;
  if (owner == SMS_OWNER_NONE) return; /* writes to $C0-$FF have no effect */
  if (owner == SMS_OWNER_MEMORY) {
    sms_memory_control_write(&m->mem, port, value, cycles);
    if (m->mem.error != SMS_OK) request_stop(m);
    return;
  }
  dev = device_for(m, owner);
  if (dev == NULL || dev->write == NULL) {
    unimplemented(m, port, value, cycles);
    return;
  }
  if (cls == SMS_PORT_IO_CONTROL) m->io_control = value;
  dev->write(dev->context, cls, value, cycles);
  sample_irq(m, cycles); /* a VDP register/control write can change the enable bits */
}

static uint8_t host_interrupt_acknowledge(void *context, uint64_t cycles) {
  SmsMachine *m = (SmsMachine *)context;
  advance_events(m, cycles);
  if ((m->irq_mask & SMS_IRQ_FRAME) != 0u) irq_trace(m, cycles, SMS_IRQ_TRACE_FRAME, SMS_IRQ_ACCEPTED);
  if ((m->irq_mask & SMS_IRQ_LINE) != 0u) irq_trace(m, cycles, SMS_IRQ_TRACE_LINE, SMS_IRQ_ACCEPTED);
  return 0xFFu; /* SMS 2 data bus: $FF (IM1 -> $0038, IM0 = RST 38h, IM2 vector low byte $FF) */
}

static int host_code_image(void *context, uint16_t address, Z80CodeImage *image) {
  return sms_memory_code_image(&((SmsMachine *)context)->mem, address, image);
}

/* ---- init / reset --------------------------------------------------------------------------------------------- */

void sms_machine_reset(SmsMachine *m) {
  z80_reset(&m->rt.state);
  m->rt.outcome = Z80_OUTCOME_NONE;
  sms_memory_reset(&m->mem);
  m->io_control = 0xFFu;
  memset(&m->input, 0, sizeof m->input);
  m->input_next = 0;
  m->next_line = 0;
  m->irq_mask = 0;
  m->dead = 0;
  memset(&m->dead_stop, 0, sizeof m->dead_stop);
  m->irq_count = m->irq_dropped = 0;
  m->mapper_count = m->mapper_dropped = 0;
  if (m->vdp.port.reset != NULL) m->vdp.port.reset(m->vdp.port.context); /* T004 (including U9) */
  if (m->psg.reset != NULL) m->psg.reset(m->psg.context);                /* T007 */
  if (m->pad.reset != NULL) m->pad.reset(m->pad.context);                /* T006 */
  sample_irq(m, 0);
}

SmsError sms_machine_init(SmsMachine *m, const uint8_t *rom, uint32_t rom_size, SmsMapperFamily family) {
  SmsError error;
  memset(m, 0, sizeof *m);
  error = sms_memory_init(&m->mem, rom, rom_size, family);
  if (error != SMS_OK) return error;
  m->rt.host.context = m;
  m->rt.host.read = host_read;
  m->rt.host.write = host_write;
  m->rt.host.io_in = host_io_in;
  m->rt.host.io_out = host_io_out;
  m->rt.host.interrupt_acknowledge = host_interrupt_acknowledge;
  m->rt.host.code_image = host_code_image;
  sms_machine_reset(m);
  return SMS_OK;
}

void sms_machine_set_input(SmsMachine *m, const SmsInputEvent *events, uint32_t count) {
  m->input_events = events;
  m->input_count = count;
  m->input_next = 0;
}

void sms_machine_set_traces(SmsMachine *m, SmsIrqTraceEntry *irq, uint32_t irq_capacity, SmsMapperTraceEntry *mapper,
                            uint32_t mapper_capacity) {
  m->irq_trace = irq;
  m->irq_capacity = irq_capacity;
  m->irq_count = m->irq_dropped = 0;
  m->mapper_trace = mapper;
  m->mapper_capacity = mapper_capacity;
  m->mapper_count = m->mapper_dropped = 0;
}

/* ---- run API -------------------------------------------------------------------------------------------------- */

static SmsStop make_stop(SmsMachine *m, SmsStopKind kind) {
  SmsStop s;
  Z80CodeImage image;
  const SmsError latched = m->mem.error;
  memset(&s, 0, sizeof s);
  s.kind = kind;
  s.cycles = m->rt.state.cycles;
  s.frame = s.cycles / SMS_CYCLES_PER_FRAME;
  s.pc = m->rt.state.pc;
  s.z80_outcome = m->rt.outcome;
  s.sms_error = latched;
  s.error_address = m->mem.error_address;
  s.error_value = m->mem.error_value;
  s.error_cycles = m->mem.error_cycles;
  m->mem.error = SMS_OK; /* the latch disables code_image; identity is reported for the stop location regardless */
  if (sms_memory_code_image(&m->mem, s.pc, &image)) s.image_identity = image.identity;
  m->mem.error = latched;
  return s;
}

static SmsStop die(SmsMachine *m, SmsStopKind kind) {
  m->dead = 1;
  m->dead_stop = make_stop(m, kind);
  return m->dead_stop;
}

/* One z80_run slice to `deadline`, with NMI acceptance tracing. Returns 1 with a permanent stop, else 0 (continue). */
static int run_slice(SmsMachine *m, uint64_t deadline, SmsStop *stop) {
  Z80State *st = &m->rt.state;
  const uint8_t nmi_before = st->nmi_pending;
  const uint8_t reject_before = st->nmi_reject;
  const uint8_t prefix_before = st->in_prefix_run;
  /* An NMI pending at a run entry outside a prefix run is sampled at the entry boundary (cycles < deadline): accepted, or
   * discarded when an NMI response just started (nmi_reject). Logged before the run so the trace stays in time order. */
  if (nmi_before != 0u && prefix_before == 0u && st->cycles < deadline)
    irq_trace(m, st->cycles, SMS_IRQ_TRACE_PAUSE, reject_before != 0u ? SMS_IRQ_DEASSERTED : SMS_IRQ_ACCEPTED);
  m->rt.outcome = z80_run(&m->rt, deadline);
  if (nmi_before != 0u && prefix_before != 0u && st->nmi_pending == 0u) {
    /* NMI pending across a prefix-lock run is accepted at the first boundary after the run ends, inside this call; only an
     * upper bound (the T-state at return) is known to the platform. */
    irq_trace(m, st->cycles, SMS_IRQ_TRACE_PAUSE, SMS_IRQ_ACCEPTED);
  }
  if (m->mem.error != SMS_OK) {
    *stop = die(m, SMS_STOP_PLATFORM_ERROR);
    return 1;
  }
  if (z80_outcome_is_error(m->rt.outcome)) {
    *stop = die(m, SMS_STOP_Z80_ERROR);
    return 1;
  }
  return 0;
}

SmsStop sms_run_until_cycle(SmsMachine *m, uint64_t t_state) {
  SmsStop stop;
  if (m->dead) return m->dead_stop;
  for (;;) {
    uint64_t deadline;
    advance_events(m, m->rt.state.cycles);
    if (m->rt.state.cycles >= t_state) return make_stop(m, SMS_STOP_CYCLE);
    deadline = sms_next_event_cycles(m);
    if (deadline > t_state) deadline = t_state;
    if (run_slice(m, deadline, &stop)) return stop;
    if (m->stop_on_halt_idle && m->rt.state.halted && m->rt.state.iff1 == 0u && m->rt.state.nmi_pending == 0u &&
        m->input_next >= m->input_count)
      return make_stop(m, SMS_STOP_HALT_IDLE);
  }
}

SmsStop sms_run_until_frame(SmsMachine *m, uint64_t frame) {
  SmsStop stop;
  if (frame > UINT64_MAX / SMS_CYCLES_PER_FRAME) frame = UINT64_MAX / SMS_CYCLES_PER_FRAME;
  stop = sms_run_until_cycle(m, frame * SMS_CYCLES_PER_FRAME);
  if (stop.kind == SMS_STOP_CYCLE) stop.kind = SMS_STOP_FRAME;
  return stop;
}

SmsStop sms_run_bounded(SmsMachine *m, uint64_t cycle_budget, uint64_t frames) {
  uint64_t frame_target = SMS_NO_LIMIT;
  uint64_t target;
  SmsStop stop;
  if (frames != SMS_NO_LIMIT) {
    if (frames > UINT64_MAX / SMS_CYCLES_PER_FRAME) frames = UINT64_MAX / SMS_CYCLES_PER_FRAME;
    frame_target = frames * SMS_CYCLES_PER_FRAME;
  }
  target = cycle_budget < frame_target ? cycle_budget : frame_target;
  stop = sms_run_until_cycle(m, target);
  if (stop.kind == SMS_STOP_CYCLE) {
    if (frame_target != SMS_NO_LIMIT && stop.cycles >= frame_target) stop.kind = SMS_STOP_FRAME;
    else stop.kind = SMS_STOP_CYCLE_BUDGET;
  }
  return stop;
}

/* ---- digest --------------------------------------------------------------------------------------------------- */

void sms_machine_digest(const SmsMachine *m, uint8_t out[32]) {
  SmsSha256 sha;
  const Z80State *s = &m->rt.state;
  sms_sha256_init(&sha);
  {
    const uint8_t regs[16] = {s->a, s->f, s->b, s->c, s->d, s->e, s->h, s->l,
                              s->a2, s->f2, s->b2, s->c2, s->d2, s->e2, s->h2, s->l2};
    sms_sha256_update(&sha, regs, sizeof regs);
  }
  sms_sha256_update_u16(&sha, s->ix);
  sms_sha256_update_u16(&sha, s->iy);
  sms_sha256_update_u16(&sha, s->sp);
  sms_sha256_update_u16(&sha, s->pc);
  sms_sha256_update_u16(&sha, s->wz);
  {
    const uint8_t flags[13] = {s->i, s->r, s->im, s->iff1, s->iff2, s->q, s->halted, s->int_deferral, s->ld_a_ir,
                               s->in_prefix_run, s->int_line, s->nmi_pending, s->nmi_reject};
    sms_sha256_update(&sha, flags, sizeof flags);
  }
  sms_sha256_update_u64(&sha, s->cycles);
  sms_sha256_update(&sha, m->mem.ram, sizeof m->mem.ram);
  sms_sha256_update(&sha, m->mem.cart_ram, sizeof m->mem.cart_ram);
  sms_sha256_update(&sha, m->mem.mapper_regs, sizeof m->mem.mapper_regs);
  sms_sha256_update_u8(&sha, m->mem.memory_control);
  sms_sha256_update_u8(&sha, m->io_control);
  sms_sha256_update_u32(&sha, (uint32_t)m->mem.error);
  sms_sha256_update_u64(&sha, m->next_line);
  sms_sha256_update_u8(&sha, m->irq_mask);
  sms_sha256_update_u8(&sha, m->input.p1);
  sms_sha256_update_u8(&sha, m->input.p2);
  sms_sha256_update_u8(&sha, m->input.pause);
  sms_sha256_update_u32(&sha, m->input_next);
  if (m->vdp.port.digest != NULL) m->vdp.port.digest(m->vdp.port.context, &sha);
  if (m->psg.digest != NULL) m->psg.digest(m->psg.context, &sha);
  if (m->pad.digest != NULL) m->pad.digest(m->pad.context, &sha);
  sms_sha256_final(&sha, out);
}
