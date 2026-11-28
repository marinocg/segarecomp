/* SEG-147-T004: the Master System machine expects the generated program's CPU entry. The conformance rig has no guest program: this scripted one only lets guest time pass in 4 T-state steps. */
#include "segarecomp/codegen/c11/runtime/z80_runtime.h"

Z80Outcome z80_run(Z80Runtime *rt, uint64_t deadline) {
  while (rt->state.cycles < deadline) rt->state.cycles += 4U;
  return Z80_OUTCOME_NONE;
}
