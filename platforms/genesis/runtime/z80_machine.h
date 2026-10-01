/*
 * SEG-032-T004 (ADR 0072, contract sections 2 and 9): the Genesis Z80 as a secondary CPU of the Genesis machine.
 *
 * This module owns the Z80's view of the machine: the `Z80Host` callbacks that give a generated-native Z80 image the sound
 * RAM and its mirror, the YM2612 and PSG ports, the 9-bit bank register and the banked 68K window. It is plain C11 and
 * is compiled only into programs that carry a Z80 image registry; runtime.c itself never includes a Z80 header.
 *
 * Fail closed: an access the contract does not support latches a typed diagnostic in `view_stop` (the access returns $FF /
 * is ignored, the Z80 stops at the next instruction boundary) and the machine reports it as a Genesis stop. Nothing here
 * decodes a Z80 opcode.
 */
#ifndef SEGARECOMP_GENESIS_Z80_MACHINE_H
#define SEGARECOMP_GENESIS_Z80_MACHINE_H

#include "runtime.h"
#include "segarecomp/codegen/c11/runtime/z80_runtime.h"

#define GENESIS_Z80_CLOCK_DIVIDER 15U /* master ticks per Z80 clock (contract section 1) */
#define GENESIS_Z80_BUS_WAIT_CYCLES 3U /* extra Z80 cycles per access to the 68K bus ($7F00 window / banked window) */

typedef struct GenesisZ80Machine {
  Z80Runtime cpu;
  GenesisRuntime *runtime;
  uint32_t bound_ordinal;                 /* the RAM-backed image the Z80 executes (code-image identity); 0 = none */
  uint64_t cycle_base_master_ticks;       /* master time at which this Z80 run's cycle counter was 0 */
  GenesisDiagnosticCategory view_stop;    /* 0, or the first typed view stop latched since the last clear */
} GenesisZ80Machine;

/* Zeroes the machine, installs the host callbacks and puts the Z80 in its architectural reset state. The RAM is the
 * runtime's `devices.z80_bus.z80_ram`; the bank register is `devices.z80_bus.bank`. */
void genesis_z80_machine_init(GenesisZ80Machine *machine, GenesisRuntime *runtime);

/* Master time of the machine's current Z80 cycle counter. */
uint64_t genesis_z80_machine_master_ticks(const GenesisZ80Machine *machine);

#endif
