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

/* Materialization seam (SEG-032-T008, ADR 0073): called at an image epoch whose activation signature is not in the compiled
 * registry, before the typed stop. `ram` is the full snapshot, `written` the 68K-written hold-window bitmap. The machine stops
 * with z80_unknown_image after the callback returns. */
typedef void (*GenesisZ80UnknownImageFunction)(void *context, uint32_t epoch_ordinal, const uint8_t *ram, const uint8_t *written);

typedef struct GenesisZ80Machine {
  Z80Runtime cpu;
  GenesisRuntime *runtime;
  GenesisZ80Hooks hooks;                  /* installed into runtime->z80_hooks by genesis_z80_machine_attach */
  uint32_t bound_ordinal;                 /* the RAM-backed image the Z80 executes (code-image identity); 0 = none */
  uint32_t last_bound;                    /* the image bound by the previous epoch (a restart re-binds it) */
  uint64_t cycle_base_master_ticks;       /* master time at which this Z80 run's cycle counter was 0 */
  GenesisDiagnosticCategory view_stop;    /* 0, or the first typed view stop latched since the last clear */
  GenesisZ80UnknownImageFunction on_unknown_image;
  void *unknown_image_context;
  /* Opt-in aggregate evidence (counts only; never an address or value; SEG-032-T010). */
  uint64_t count_reset_assert, count_reset_release, count_busreq_assert, count_busreq_release, count_bank_writes, count_banked_reads;
} GenesisZ80Machine;

/* Zeroes the machine, installs the host callbacks and puts the Z80 in its architectural reset state. The RAM is the
 * runtime's `devices.z80_bus.z80_ram`; the bank register is `devices.z80_bus.bank`. */
void genesis_z80_machine_init(GenesisZ80Machine *machine, GenesisRuntime *runtime);

/* init + install the runtime hooks: from now on the Z80 follows the M68K clock, BUSREQ/RESET edges act on it and an image
 * epoch activates a compiled image (or stops with z80_unknown_image). */
void genesis_z80_machine_attach(GenesisZ80Machine *machine, GenesisRuntime *runtime);

/* Runs the Z80 to guest time `master_ticks` (the hook the runtime calls). 0 = ok, non-zero = typed stop in *stop. */
int genesis_z80_machine_run_to(GenesisZ80Machine *machine, uint64_t master_ticks, GenesisRuntimeStop *stop);

/* SHA-256 of the architectural Z80 state, the bound image and the time base (determinism evidence; never a snapshot). */
void genesis_z80_machine_state_digest(const GenesisZ80Machine *machine, uint8_t out[32]);

/* The activation signature S1* (contract section 7) of a hold window: SHA-256 over the tag and, for every maximal run of written
 * bytes, u16le offset, u16le length and the bytes. `extent_count` receives the number of runs. */
void genesis_z80_activation_signature(const uint8_t *ram, const uint8_t *written, uint32_t *extent_count, uint8_t out[32]);

/* Master time of the machine's current Z80 cycle counter. */
uint64_t genesis_z80_machine_master_ticks(const GenesisZ80Machine *machine);

#endif
