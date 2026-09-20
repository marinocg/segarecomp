// Synthetic guest-boundary timing coverage; no ROM or host clock is involved.
#include "runtime.h"

#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
void check(bool value, const char *message) { if (!value) { std::printf("FAIL: %s\n", message); ++failures; } }

void advances_and_places_vblank() {
  GenesisRuntime runtime{};
  runtime.pc = 0x200u;
  runtime.sr = 0x2000u;
  runtime.devices.vdp.registers[1] = 0x20u;
  runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28u;
  const GenesisControlTransfer result = genesis_runtime_retire_m68k_instruction(&runtime, 4u, 0x200u);
  check(result.kind == GENESIS_CONTINUE_AT_PC, "completed instruction retires");
  check(runtime.scheduler.master_ticks == GENESIS_NTSC_VBLANK_ONSET_TICK,
        "four MC68000 cycles advance exactly 28 master ticks");
  check(runtime.devices.interrupt.vblank_transition_count == 1u,
        "VBlank rises exactly at line 224 onset");
}

void fails_closed_for_missing_or_overflow_timing() {
  GenesisRuntime missing{};
  missing.pc = 0x200u;
  const GenesisControlTransfer missing_result = genesis_runtime_retire_m68k_instruction(&missing, 0u, 0x200u);
  check(missing_result.kind == GENESIS_STOP &&
            missing_result.stop.diagnostic_category == GENESIS_DIAG_UNACCOUNTED_INSTRUCTION_TIMING,
        "unaccounted instruction timing fails closed");

  GenesisRuntime overflow{};
  overflow.pc = 0x200u;
  overflow.scheduler.master_ticks = UINT64_MAX - 1u;
  const GenesisControlTransfer overflow_result = genesis_runtime_retire_m68k_instruction(&overflow, 4u, 0x200u);
  check(overflow_result.kind == GENESIS_STOP &&
            overflow_result.stop.diagnostic_category == GENESIS_DIAG_VIRTUAL_TIME_OVERFLOW,
        "master-tick overflow fails closed");
  GenesisRuntime count_overflow{};
  count_overflow.sr = 0x2000u; count_overflow.devices.vdp.registers[1] = 0x20u;
  count_overflow.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28u;
  count_overflow.devices.interrupt.vblank_transition_count = UINT32_MAX;
  const GenesisControlTransfer count_result = genesis_runtime_retire_m68k_instruction(&count_overflow, 4u, 0x200u);
  check(count_result.kind == GENESIS_STOP && count_overflow.scheduler.master_ticks == GENESIS_NTSC_VBLANK_ONSET_TICK - 28u &&
            count_overflow.devices.interrupt.vblank_transition_count == UINT32_MAX,
        "VBlank counter overflow fails before phase or event mutation");
}

void phase_boundaries_and_partition_are_invariant() {
  GenesisRuntime before{};
  before.sr = 0x2000u; before.devices.vdp.registers[1] = 0x20u;
  before.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 29u;
  (void)genesis_runtime_retire_m68k_instruction(&before, 4u, 0x200u);
  check(before.devices.interrupt.vblank_transition_count == 0u, "before onset has no event");
  GenesisRuntime split = before;
  (void)genesis_runtime_retire_m68k_instruction(&split, 4u, 0x202u);
  GenesisRuntime combined = before;
  (void)genesis_runtime_retire_m68k_instruction(&combined, 4u, 0x204u);
  check(split.scheduler.master_ticks == combined.scheduler.master_ticks &&
            split.devices.interrupt.vblank_transition_count == combined.devices.interrupt.vblank_transition_count,
        "instruction partitioning preserves virtual phase and VBlank sequence");
  GenesisRuntime long_crossing{};
  long_crossing.sr = 0x2000u; long_crossing.devices.vdp.registers[1] = 0x20u;
  long_crossing.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28u;
  (void)genesis_runtime_retire_m68k_instruction(&long_crossing,
      (uint32_t)(GENESIS_NTSC_MASTER_TICKS_PER_FRAME / GENESIS_M68K_CYCLE_MASTER_TICKS + 4u), 0x200u);
  check(long_crossing.devices.interrupt.vblank_transition_count == 1u,
        "one long retirement crossing one onset produces one edge");
}
}  // namespace

int main() {
  advances_and_places_vblank();
  fails_closed_for_missing_or_overflow_timing();
  phase_boundaries_and_partition_are_invariant();
  return failures == 0 ? 0 : 1;
}
