#include "z80_machine.h"

#include <string.h>

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

static int z80_view_code_matches(void *context, uint16_t address, const uint8_t *expected, uint32_t length) {
  const GenesisZ80Machine *machine = machine_of(context);
  uint32_t index;
  for (index = 0U; index < length; ++index)
    if (machine->runtime->devices.z80_bus.z80_ram[(address + index) & (GENESIS_Z80_RAM_BYTES - 1U)] != expected[index]) return 0;
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
  machine->cpu.host.code_matches = z80_view_code_matches;
  z80_reset(&machine->cpu.state);
}

uint64_t genesis_z80_machine_master_ticks(const GenesisZ80Machine *machine) { return access_master_ticks(machine); }
