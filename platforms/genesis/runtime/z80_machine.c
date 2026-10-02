#include "z80_machine.h"

#include <string.h>

#include "z80_registry.h"

/* Contract section 2: the Z80 memory map. */
#define Z80_RAM_END UINT32_C(0x4000)        /* $0000-$3FFF: sound RAM and its mirror */
#define Z80_YM_END UINT32_C(0x6000)         /* $4000-$5FFF: YM2612, `address & 3` */
#define Z80_BANK_REG_END UINT32_C(0x6100)   /* $6000-$60FF: bank register (write only) */
#define Z80_VDP_WINDOW UINT32_C(0x7F00)     /* $7F00-$7FFF: window onto $C00000 + (address & $FF) */
#define Z80_BANKED UINT32_C(0x8000)         /* $8000-$FFFF: banked 68K view */
#define BANK_ROM_END UINT32_C(0x400000)     /* cartridge ROM space of the 68K map */
#define BANK_WORK_RAM UINT32_C(0xE00000)    /* work RAM, 64 KiB mirrored through $FFFFFF */

static GenesisZ80Machine *machine_of(void *context) { return (GenesisZ80Machine *)context; }

static uint64_t access_master_ticks(const GenesisZ80Machine *machine) {
  return machine->cycle_base_master_ticks + machine->cpu.state.cycles * GENESIS_Z80_CLOCK_DIVIDER;
}

static void latch_stop(GenesisZ80Machine *machine, GenesisDiagnosticCategory diagnostic) {
  if (machine->view_stop == (GenesisDiagnosticCategory)0) machine->view_stop = diagnostic;
  /* The Z80 stops at the next instruction boundary: the owner prologue sees `cycles >= deadline`. */
  machine->cpu.state.deadline = 0U;
}

static uint32_t banked_address(const GenesisZ80Machine *machine, uint16_t address) {
  return ((uint32_t)machine->runtime->devices.z80_bus.bank << 15) | (address & 0x7FFFU);
}

static int bank_rom_read(const GenesisZ80Machine *machine, uint32_t address24, uint8_t *value) {
  uint32_t index;
  if (address24 >= BANK_ROM_END) return 0;
  for (index = 0U; index < machine->runtime->owned_region_count; ++index) {
    const GenesisOwnedCartridgeRegion *region = &machine->runtime->owned_regions[index];
    if (region->end <= region->begin || region->length != region->end - region->begin) continue;
    if (address24 >= region->begin && address24 < region->end) {
      *value = region->data[address24 - region->begin];
      return 1;
    }
  }
  return 0;
}

static uint8_t z80_view_read(void *context, uint16_t address, uint64_t cycles) {
  GenesisZ80Machine *machine = machine_of(context);
  uint8_t value = 0xFFU;
  (void)cycles;
  if (address < Z80_RAM_END) return machine->runtime->devices.z80_bus.z80_ram[address & (GENESIS_Z80_RAM_BYTES - 1U)];
  if (address < Z80_YM_END) {
    if (!genesis_ym2612_port_read(machine->runtime, address & 3U, access_master_ticks(machine), &value)) {
      latch_stop(machine, GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS);
      return 0xFFU;
    }
    return value;
  }
  if (address < Z80_BANKED) {  /* bank register (write only), unused space and the $7F00 window: not supported for reads */
    latch_stop(machine, GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS);
    return 0xFFU;
  }
  machine->cpu.state.cycles += GENESIS_Z80_BUS_WAIT_CYCLES;
  ++machine->count_banked_reads;
  if (!bank_rom_read(machine, banked_address(machine, address), &value)) {
    latch_stop(machine, GENESIS_DIAG_Z80_BANK_TARGET_UNSUPPORTED);  /* open bus, work-RAM reads (open fact U3), the Z80 area itself */
    return 0xFFU;
  }
  return value;
}

static void z80_view_write(void *context, uint16_t address, uint8_t value, uint64_t cycles) {
  GenesisZ80Machine *machine = machine_of(context);
  GenesisDeviceState *devices = &machine->runtime->devices;
  (void)cycles;
  if (address < Z80_RAM_END) {
    devices->z80_bus.z80_ram[address & (GENESIS_Z80_RAM_BYTES - 1U)] = value;
    return;
  }
  if (address < Z80_YM_END) {
    if (!genesis_ym2612_port_write(machine->runtime, address & 3U, value, access_master_ticks(machine)))
      latch_stop(machine, GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS);
    return;
  }
  if (address < Z80_BANK_REG_END && address >= UINT32_C(0x6000)) {
    devices->z80_bus.bank = (uint16_t)((((uint32_t)value & 1U) << 8) | (devices->z80_bus.bank >> 1));
    ++machine->count_bank_writes;
    return;
  }
  if (address >= Z80_VDP_WINDOW && address < Z80_BANKED) {  /* PSG at $7F11/13/15/17 only (contract section 2) */
    machine->cpu.state.cycles += GENESIS_Z80_BUS_WAIT_CYCLES;
    if ((address & 0xFFU) < 0x11U || (address & 0xFFU) > 0x17U || ((address & 1U) == 0U) ||
        !genesis_psg_port_write(machine->runtime, value, access_master_ticks(machine)))
      latch_stop(machine, GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS);
    return;
  }
  if (address < Z80_BANKED) {
    latch_stop(machine, GENESIS_DIAG_Z80_VIEW_UNMAPPED_ACCESS);
    return;
  }
  machine->cpu.state.cycles += GENESIS_Z80_BUS_WAIT_CYCLES;
  {
    const uint32_t target = banked_address(machine, address);
    if (target >= BANK_WORK_RAM) {
      machine->runtime->work_ram[target & 0xFFFFU] = value;
      return;
    }
  }
  latch_stop(machine, GENESIS_DIAG_Z80_BANK_TARGET_UNSUPPORTED);
}

static uint8_t z80_view_io_in(void *context, uint16_t port, uint64_t cycles) {
  (void)context; (void)port; (void)cycles;
  return 0xFFU; /* unused on the Genesis (ares APU::in, GPGX) */
}

static void z80_view_io_out(void *context, uint16_t port, uint8_t value, uint64_t cycles) {
  (void)context; (void)port; (void)value; (void)cycles;
}

static uint8_t z80_view_interrupt_acknowledge(void *context, uint64_t cycles) {
  (void)context; (void)cycles;
  return 0xFFU; /* GPGX z80_irq_callback returns -1: IM1 ignores it, IM2 vector low byte $FF */
}

static int z80_view_code_image(void *context, uint16_t address, Z80CodeImage *image) {
  const GenesisZ80Machine *machine = machine_of(context);
  if (address >= Z80_RAM_END || machine->bound_ordinal == 0U) return 0;
  image->identity = machine->bound_ordinal;
  image->window_base = 0U;
  return 1;
}

static int z80_view_code_fetch(void *context, uint16_t address, uint8_t *bytes, uint32_t length) {
  const GenesisZ80Machine *machine = machine_of(context);
  uint32_t index;
  for (index = 0U; index < length; ++index)
    bytes[index] = machine->runtime->devices.z80_bus.z80_ram[(address + index) & (GENESIS_Z80_RAM_BYTES - 1U)];
  return 1;
}

void genesis_z80_machine_init(GenesisZ80Machine *machine, GenesisRuntime *runtime) {
  memset(machine, 0, sizeof(*machine));
  machine->runtime = runtime;
  machine->cpu.host.context = machine;
  machine->cpu.host.read = z80_view_read;
  machine->cpu.host.write = z80_view_write;
  machine->cpu.host.io_in = z80_view_io_in;
  machine->cpu.host.io_out = z80_view_io_out;
  machine->cpu.host.interrupt_acknowledge = z80_view_interrupt_acknowledge;
  machine->cpu.host.code_image = z80_view_code_image;
  machine->cpu.host.code_fetch = z80_view_code_fetch;
  z80_reset(&machine->cpu.state);
}

uint64_t genesis_z80_machine_master_ticks(const GenesisZ80Machine *machine) { return access_master_ticks(machine); }

/* ------------------------------------------------------------------------------------------------------------------------ */
/* SEG-032-T005 (ADR 0072): the Z80 as a secondary CPU on the Genesis master clock.                                         */

static uint64_t ceil_to_z80_clock(uint64_t master_ticks) {
  return ((master_ticks + GENESIS_Z80_CLOCK_DIVIDER - 1U) / GENESIS_Z80_CLOCK_DIVIDER) * GENESIS_Z80_CLOCK_DIVIDER;
}

static int stop_with(GenesisRuntimeStop *stop, GenesisStopClass stop_class, GenesisDiagnosticCategory diagnostic) {
  memset(stop, 0, sizeof(*stop));
  stop->stop_class = stop_class;
  stop->diagnostic_category = diagnostic;
  return 1;
}

static GenesisDiagnosticCategory diagnostic_of_outcome(Z80Outcome outcome) {
  switch (outcome) {
    case Z80_ERROR_CODE_MISMATCH: return GENESIS_DIAG_Z80_CODE_MISMATCH;
    case Z80_ERROR_UNKNOWN_IMAGE_IDENTITY: return GENESIS_DIAG_Z80_UNKNOWN_IMAGE;
    case Z80_ERROR_MUTABLE_CODE: return GENESIS_DIAG_Z80_MUTABLE_CODE;
    case Z80_ERROR_UNRESOLVED_FETCH_MAPPING: return GENESIS_DIAG_Z80_UNRESOLVED_FETCH_MAPPING;
    case Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE: return GENESIS_DIAG_Z80_UNSUPPORTED_ACKNOWLEDGE;
    default: return GENESIS_DIAG_Z80_NO_OWNER; /* no_owner and the reserved excluded_form */
  }
}

/* Contract section 8: the Z80 INT line is high during the VBlank onset scanline only. */
static int int_level_at(uint64_t master_ticks) {
  const uint64_t phase = master_ticks % GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  return phase >= GENESIS_NTSC_VBLANK_ONSET_TICK && phase < GENESIS_NTSC_VBLANK_ONSET_TICK + GENESIS_NTSC_MASTER_TICKS_PER_LINE;
}

static uint64_t next_int_edge(uint64_t master_ticks) {
  const uint64_t phase = master_ticks % GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  const uint64_t base = master_ticks - phase;
  if (phase < GENESIS_NTSC_VBLANK_ONSET_TICK) return base + GENESIS_NTSC_VBLANK_ONSET_TICK;
  if (phase < GENESIS_NTSC_VBLANK_ONSET_TICK + GENESIS_NTSC_MASTER_TICKS_PER_LINE)
    return base + GENESIS_NTSC_VBLANK_ONSET_TICK + GENESIS_NTSC_MASTER_TICKS_PER_LINE;
  return base + GENESIS_NTSC_MASTER_TICKS_PER_FRAME + GENESIS_NTSC_VBLANK_ONSET_TICK;
}

static int machine_runnable(const GenesisZ80Machine *machine) {
  const GenesisZ80BusState *bus = &machine->runtime->devices.z80_bus;
  return bus->reset_released && !bus->bus_requested && machine->bound_ordinal != 0U;
}

int genesis_z80_machine_run_to(GenesisZ80Machine *machine, uint64_t master_ticks, GenesisRuntimeStop *stop) {
  /* A faulted sound CPU executes nothing and answers nothing: it is quiesced (time is not advanced, no device write, no INT). */
  if (genesis_z80_machine_sound_faulted(machine) || !machine_runnable(machine)) return 0;
  for (;;) {
    const uint64_t now = access_master_ticks(machine);
    uint64_t segment_end, steps;
    Z80Outcome outcome;
    if (now >= master_ticks) return 0;
    segment_end = next_int_edge(now);
    if (segment_end > master_ticks) segment_end = master_ticks;
    steps = (segment_end - now + GENESIS_Z80_CLOCK_DIVIDER - 1U) / GENESIS_Z80_CLOCK_DIVIDER;
    machine->cpu.state.int_line = (uint8_t)int_level_at(now);
    outcome = z80_run(&machine->cpu, machine->cpu.state.cycles + steps);
    if (machine->view_stop != (GenesisDiagnosticCategory)0)
      return stop_with(stop, GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS, machine->view_stop);
    if (outcome == Z80_ERROR_CODE_MISMATCH) {  /* structural mutation: isolate the sound CPU, never interpret the bytes */
      machine->sound_fault = GENESIS_DIAG_Z80_CODE_MISMATCH;
      machine->sound_fault_epoch = machine->runtime->z80_epoch.epoch_count;
      machine->sound_fault_master_ticks = access_master_ticks(machine);
      machine->bound_ordinal = 0U;
      return 0;
    }
    if (z80_outcome_is_error(outcome))
      return stop_with(stop, GENESIS_STOP_UNSUPPORTED_Z80_EXECUTION, diagnostic_of_outcome(outcome));
  }
}

static const uint8_t k_signature_tag[] = "segarecomp.genesis.z80.signature.v1.extents";

void genesis_z80_activation_signature(const uint8_t *ram, const uint8_t *written, uint32_t *extent_count, uint8_t out[32]) {
  GenesisSha256 sha;
  uint32_t offset = 0U, runs = 0U;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, k_signature_tag, (uint32_t)(sizeof(k_signature_tag) - 1U));
  while (offset < GENESIS_Z80_RAM_BYTES) {
    uint32_t end;
    uint8_t header[4];
    if (((written[offset >> 3] >> (offset & 7U)) & 1U) == 0U) { ++offset; continue; }
    end = offset;
    while (end < GENESIS_Z80_RAM_BYTES && ((written[end >> 3] >> (end & 7U)) & 1U) != 0U) ++end;
    header[0] = (uint8_t)(offset & 0xFFU); header[1] = (uint8_t)(offset >> 8);
    header[2] = (uint8_t)((end - offset) & 0xFFU); header[3] = (uint8_t)((end - offset) >> 8);
    genesis_sha256_update(&sha, header, 4U);
    genesis_sha256_update(&sha, ram + offset, end - offset);
    offset = end;
    ++runs;
  }
  genesis_sha256_final(&sha, out);
  if (extent_count != NULL) *extent_count = runs;
}

/* Contract section 5 / ADR 0073: the image epoch activates a compiled image, or stops fail closed. */
static int activate_image(GenesisZ80Machine *machine, const uint8_t *written, GenesisRuntimeStop *stop) {
  uint8_t signature[32];
  uint32_t extents, ordinal;
  if (genesis_z80_machine_sound_faulted(machine)) return 0;  /* a fault is permanent: no re-activation, no image lookup */
  genesis_z80_activation_signature(machine->runtime->devices.z80_bus.z80_ram, written, &extents, signature);
  if (extents == 0U && machine->last_bound != 0U) {  /* a plain restart: the code already in RAM, i.e. the previously bound image */
    machine->bound_ordinal = machine->last_bound;
    return 0;
  }
  ordinal = genesis_z80_image_for_signature(signature);
  if (ordinal == 0U) {
    if (machine->on_unknown_image != NULL)
      machine->on_unknown_image(machine->unknown_image_context, machine->runtime->z80_epoch.epoch_count,
                                machine->runtime->devices.z80_bus.z80_ram, written);
    machine->bound_ordinal = 0U;
    return stop_with(stop, GENESIS_STOP_UNSUPPORTED_Z80_EXECUTION, GENESIS_DIAG_Z80_UNKNOWN_IMAGE);
  }
  machine->bound_ordinal = machine->last_bound = ordinal;
  return 0;
}

static int hook_run_to(void *context, GenesisRuntime *runtime, uint64_t master_ticks, GenesisRuntimeStop *stop) {
  const int stopped = genesis_z80_machine_run_to((GenesisZ80Machine *)context, master_ticks, stop);
  if (!stopped && runtime->audio_hooks != NULL && runtime->audio_hooks->sync != NULL)
    runtime->audio_hooks->sync(runtime->audio_hooks->context, runtime, master_ticks);
  return stopped;
}

static int hook_bus_event(void *context, GenesisRuntime *runtime, GenesisZ80Event event, uint64_t master_ticks, int transition,
                          int epoch, const uint8_t *written, GenesisRuntimeStop *stop) {
  GenesisZ80Machine *machine = (GenesisZ80Machine *)context;
  const uint64_t aligned = ceil_to_z80_clock(master_ticks);
  switch (event) {
    case GENESIS_Z80_EVENT_RESET_ASSERT:
      ++machine->count_reset_assert;
      genesis_ym2612_port_reset(runtime, master_ticks);
      break;
    case GENESIS_Z80_EVENT_RESET_RELEASE:  /* the architectural reset happens at release (contract section 4.6) */
      genesis_ym2612_port_reset(runtime, master_ticks);
      ++machine->count_reset_release;
      if (genesis_z80_machine_sound_faulted(machine)) break;  /* the YM reset above is a device effect; the Z80 stays faulted */
      z80_reset(&machine->cpu.state);
      machine->view_stop = (GenesisDiagnosticCategory)0;
      machine->cycle_base_master_ticks = aligned;
      break;
    case GENESIS_Z80_EVENT_BUSREQ_RELEASE:
      ++machine->count_busreq_release;
      break;
    case GENESIS_Z80_EVENT_BUSREQ_ASSERT:
      ++machine->count_busreq_assert;
      break;
  }
  if (transition && !genesis_z80_machine_sound_faulted(machine)) {  /* resume: time held on the bus is not made up (GPGX: the Z80 restarts at the next multiple of 15 ticks) */
    const uint64_t z80_time = access_master_ticks(machine);
    if (z80_time < aligned) machine->cycle_base_master_ticks += aligned - z80_time;
  }
  if (epoch) return activate_image(machine, written, stop);
  return 0;
}

void genesis_z80_machine_attach(GenesisZ80Machine *machine, GenesisRuntime *runtime) {
  genesis_z80_machine_init(machine, runtime);
  machine->hooks.context = machine;
  machine->hooks.run_to = hook_run_to;
  machine->hooks.bus_event = hook_bus_event;
  runtime->z80_hooks = &machine->hooks;
}

void genesis_z80_machine_state_digest(const GenesisZ80Machine *machine, uint8_t out[32]) {
  GenesisSha256 sha;
  const Z80State *s = &machine->cpu.state;
  uint8_t bytes[64];
  uint64_t values[6];
  uint32_t i;
  genesis_sha256_init(&sha);
  bytes[0] = s->a; bytes[1] = s->f; bytes[2] = s->b; bytes[3] = s->c; bytes[4] = s->d; bytes[5] = s->e; bytes[6] = s->h; bytes[7] = s->l;
  bytes[8] = s->a2; bytes[9] = s->f2; bytes[10] = s->b2; bytes[11] = s->c2; bytes[12] = s->d2; bytes[13] = s->e2; bytes[14] = s->h2;
  bytes[15] = s->l2; bytes[16] = (uint8_t)s->ix; bytes[17] = (uint8_t)(s->ix >> 8); bytes[18] = (uint8_t)s->iy; bytes[19] = (uint8_t)(s->iy >> 8);
  bytes[20] = (uint8_t)s->sp; bytes[21] = (uint8_t)(s->sp >> 8); bytes[22] = (uint8_t)s->pc; bytes[23] = (uint8_t)(s->pc >> 8);
  bytes[24] = (uint8_t)s->wz; bytes[25] = (uint8_t)(s->wz >> 8); bytes[26] = s->i; bytes[27] = s->r; bytes[28] = s->im; bytes[29] = s->iff1;
  bytes[30] = s->iff2; bytes[31] = s->q; bytes[32] = s->halted; bytes[33] = s->int_deferral; bytes[34] = s->ld_a_ir; bytes[35] = s->in_prefix_run;
  genesis_sha256_update(&sha, bytes, 36U);
  values[0] = s->cycles; values[1] = machine->cycle_base_master_ticks; values[2] = machine->bound_ordinal; values[3] = machine->last_bound;
  values[4] = (uint64_t)machine->sound_fault; values[5] = machine->sound_fault_master_ticks;
  for (i = 0U; i < (genesis_z80_machine_sound_faulted(machine) ? 6U : 4U); ++i) {  /* a healthy digest is unchanged */
    uint8_t word[8];
    uint32_t b;
    for (b = 0U; b < 8U; ++b) word[b] = (uint8_t)(values[i] >> (8U * b));
    genesis_sha256_update(&sha, word, 8U);
  }
  genesis_sha256_final(&sha, out);
}
