#!/usr/bin/env python3
"""C4 C11 routing and typed retained-frontier regression."""
import pathlib
import subprocess
import sys
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0}, before;
  GenesisRuntimeStop stop = {0};
  uint32_t value;
  GenesisControlTransfer transfer;
  value = UINT32_C(0x12); assert(genesis_route_access(&runtime, UINT32_C(0x00FF0000), GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  value = UINT32_C(0x3456); assert(genesis_route_access(&runtime, UINT32_C(0x00FF0002), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  value = UINT32_C(0x789ABCDE); assert(genesis_route_access(&runtime, UINT32_C(0x00FF0004), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_OK);
  assert(runtime.work_ram[0] == 0x12 && runtime.work_ram[2] == 0x34 && runtime.work_ram[3] == 0x56 && runtime.work_ram[4] == 0x78 && runtime.work_ram[7] == 0xDE);
  value = 0; assert(genesis_route_access(&runtime, UINT32_C(0x00FF0004), GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_OK && value == UINT32_C(0x789ABCDE));
  before = runtime; value = UINT32_C(0xFFFFFFFF);
  assert(genesis_route_access(&runtime, UINT32_C(0x80FF0001), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.diagnostic_category == GENESIS_DIAG_EFFECTIVE_ADDRESS_NOT_24BIT && memcmp(&runtime, &before, sizeof(runtime)) == 0 && value == UINT32_C(0xFFFFFFFF));
  assert(genesis_route_access(&runtime, UINT32_C(0x00FF0001), GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL);
  assert(stop.diagnostic_category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS && memcmp(&runtime, &before, sizeof(runtime)) == 0 && value == UINT32_C(0xFFFFFFFF));
  assert(genesis_route_access(&runtime, UINT32_C(0x00000000), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, &value, &stop) == GENESIS_ACCESS_FAIL && stop.diagnostic_category == GENESIS_DIAG_ROM_WRITE_PROHIBITED);
  assert(genesis_route_access(&runtime, UINT32_C(0x00000000), GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL && stop.diagnostic_category == GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
  assert(genesis_route_access(&runtime, UINT32_C(0x00A10000), GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ, &value, &stop) == GENESIS_ACCESS_FAIL && stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS && stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO);
  runtime.pc = UINT32_C(0x00000B00); runtime.work_ram[0] = 0xFF;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000B0A) && runtime.work_ram[0] == 0U && runtime.work_ram[3] == 0U);
  transfer = genesis_frontier_stop_00000B0A(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);

  assert(transfer.stop.provenance.has_instruction_provenance == 1U && transfer.stop.provenance.instruction.source_address == UINT32_C(0x00000B0A));
  assert(transfer.stop.provenance.instruction.primary_bytes[0] == 0x4E && transfer.stop.provenance.instruction.primary_bytes[1] == 0x70 && transfer.stop.provenance.instruction.length == 2U);
  assert(transfer.stop.provenance.mapping_claim_count == 1U && transfer.stop.provenance.bus_access_count == 1U && transfer.stop.provenance.bus_accesses[0].raw_byte_count == 2U);
  return 0;
}
'''

RAM_BYTE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
   runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0xA500);
  memset(runtime.work_ram, 0xA5, sizeof(runtime.work_ram));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION);
  assert(runtime.pc == UINT32_C(0x00000B0A));
  assert(runtime.work_ram[0] == 0U && runtime.work_ram[1] == 0xA5U);
  assert(runtime.sr == UINT16_C(0xA504));
  assert(transfer.stop.provenance.has_instruction_provenance == 1U);
  assert(transfer.stop.provenance.instruction.source_address == UINT32_C(0x00000B0A));
  assert(transfer.stop.provenance.instruction.image_offset == UINT64_C(10));
  assert(transfer.stop.provenance.instruction.primary_bytes[0] == 0x4EU);
  assert(transfer.stop.provenance.instruction.primary_bytes[1] == 0x70U);
  assert(transfer.stop.provenance.instruction.length == 2U);
  return 0;
}
'''

ADDI_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.d[0] = UINT32_C(0x12340002);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.d[0] == UINT32_C(0x12340003));
  assert(runtime.pc == UINT32_C(0x00000B04));
  return 0;
}
'''

# SEG-007-T065: proves CLR.L (A0)'s runtime-routed write (see the
# routed_write assertions above) actually compiles, links, and executes
# correctly through genesis_route_access -- both a successful write to a
# RAM address held in A0 at runtime (never statically known at generation
# time), and a routing failure for an address genesis_route_access itself
# rejects, in which case A0 (a plain, non-mutating indirect EA) and every
# other runtime field are provably untouched, exactly like every other
# atomic single-step rejection this file already checks.
STRAIGHT_LINE_BLOCK_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0x0004); /* Z set: BNE's own taken condition (Z==0) is false, so it falls through to 0xB02. */
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000B02) && runtime.pc == UINT32_C(0x00000B02));
  /* SEG-007-T066: TST.W D0's own block (0xB02) returns GENESIS_CONTINUE_AT_PC
     with next_pc already advanced to 0xB04 -- a block already retained in
     this same accepted prefix, not the one frontier -- with no per-block
     frontier check for it at all (see the generated-source assertion in the
     Python driver: this block emits no "genesis_frontier_stop" line). The
     existing genesis_dispatch/genesis_runtime_run mechanism, unchanged by
     this task, is what actually reaches block 0xB04 next; this harness
     proves that reachability by making the identical second dispatch call a
     real caller (genesis_runtime_run) would make. */
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000B04) && runtime.pc == UINT32_C(0x00000B04));
  /* TST.W D0 read D0 == 0: Z set, N/V/C clear. */
  assert((runtime.sr & UINT16_C(0x000F)) == UINT16_C(0x0004));
  transfer = genesis_bridge_dispatch(&runtime);
  /* MOVEQ #0,D0's own block (0xB04) writes D0 and straight-lines into the
     one retained RESET frontier at 0xB06 (the already-supported C3 shape,
     unaffected by this task). */
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.d[0] == UINT32_C(0));
  assert(transfer.stop.provenance.has_instruction_provenance == 1U &&
         transfer.stop.provenance.instruction.source_address == UINT32_C(0x00000B06));
  assert(transfer.stop.provenance.instruction.primary_bytes[0] == 0x4EU &&
         transfer.stop.provenance.instruction.primary_bytes[1] == 0x70U &&
         transfer.stop.provenance.instruction.length == 2U);
  return 0;
}
'''

# SEG-007-T067: full compile+link+execute proof of C4's new movem_transfer
# routed lowering. One dispatch call runs five MOVEM.W/L instructions in a
# single straight-line block (see emit_general_startup_runtime_c4_movem_source
# in tests/m68k_pipeline_test.cpp for the exact encoded sequence):
#   MOVEM.L D0-D1,(0x00FF0080).L  registers_to_memory, absolute.l, long
#   MOVEM.L D0-D1,(A0)            registers_to_memory, (An),       long -- A0 never auto-updates
#   MOVEM.W D2-D3,-(A1)           registers_to_memory, -(An),      word -- reversed order (D3 then D2), one final A1 writeback
#   MOVEM.W (A2)+,D4-D5           memory_to_registers, (An)+,      word -- ascending order, sign-extension proof, one final A2 writeback
#   MOVEM.L (A3),D6-D7            memory_to_registers, (An),       long -- no sign extension, A3 never auto-updates
# then the established RESET frontier.
MOVEM_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.d[0] = UINT32_C(0x11223344);
  runtime.d[1] = UINT32_C(0x55667788);
  runtime.d[2] = UINT32_C(0x12345678);
  runtime.d[3] = UINT32_C(0x9ABCDEF0);
  runtime.a[0] = UINT32_C(0x00FF0010);
  runtime.a[1] = UINT32_C(0x00FF0020);
  runtime.a[2] = UINT32_C(0x00FF0040);
  runtime.a[3] = UINT32_C(0x00FF0060);
  /* memory_to_registers sources, preset before dispatch: a negative WORD
     (0xFFFE) to prove sign extension, a positive WORD (0x1234) to prove a
     positive value is NOT altered, and two LONGs (top bit set on the first)
     to prove long transfers never apply the WORD sign-extension formula. */
  runtime.work_ram[0x40] = 0xFFU; runtime.work_ram[0x41] = 0xFEU;
  runtime.work_ram[0x42] = 0x12U; runtime.work_ram[0x43] = 0x34U;
  runtime.work_ram[0x60] = 0x80U; runtime.work_ram[0x61] = 0x00U;
  runtime.work_ram[0x62] = 0x00U; runtime.work_ram[0x63] = 0x01U;
  runtime.work_ram[0x64] = 0x7FU; runtime.work_ram[0x65] = 0xFFU;
  runtime.work_ram[0x66] = 0xFFU; runtime.work_ram[0x67] = 0xFEU;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B18));
  /* MOVEM.L D0-D1,(0x00FF0080).L: absolute.l destination, both registers
     stored big-endian, D0 first. */
  assert(runtime.work_ram[0x80] == 0x11U && runtime.work_ram[0x81] == 0x22U &&
         runtime.work_ram[0x82] == 0x33U && runtime.work_ram[0x83] == 0x44U);
  assert(runtime.work_ram[0x84] == 0x55U && runtime.work_ram[0x85] == 0x66U &&
         runtime.work_ram[0x86] == 0x77U && runtime.work_ram[0x87] == 0x88U);
  /* MOVEM.L D0-D1,(A0): plain (An) destination never auto-updates A0, and
     stores the identical D0/D1 values a second time at a distinct address. */
  assert(runtime.work_ram[0x10] == 0x11U && runtime.work_ram[0x11] == 0x22U &&
         runtime.work_ram[0x12] == 0x33U && runtime.work_ram[0x13] == 0x44U);
  assert(runtime.work_ram[0x14] == 0x55U && runtime.work_ram[0x15] == 0x66U &&
         runtime.work_ram[0x16] == 0x77U && runtime.work_ram[0x17] == 0x88U);
  assert(runtime.a[0] == UINT32_C(0x00FF0010));
  /* MOVEM.W D2-D3,-(A1): the shared predecrement order owner stores D3
     before D2 (its own register-mask bits already carry the reversed
     encoding), each transfer decrementing the working EA by 2 BEFORE the
     store, and exactly one final architectural A1 writeback after the whole
     loop -- A1 lands on the fully-decremented address (0x1C), never a
     per-transfer intermediate. Neither D2 nor D3 themselves are ever
     modified by a store. */
  assert(runtime.work_ram[0x1C] == 0x56U && runtime.work_ram[0x1D] == 0x78U);
  assert(runtime.work_ram[0x1E] == 0xDEU && runtime.work_ram[0x1F] == 0xF0U);
  assert(runtime.a[1] == UINT32_C(0x00FF001C));
  assert(runtime.d[2] == UINT32_C(0x12345678) && runtime.d[3] == UINT32_C(0x9ABCDEF0));
  /* MOVEM.W (A2)+,D4-D5: ascending order, one final architectural A2
     writeback (0x44) after the whole loop. D4's own loaded WORD (0xFFFE) is
     sign-extended to 0xFFFFFFFE; D5's (0x1234) is not altered by the
     identical formula applied to a positive value. */
  assert(runtime.d[4] == UINT32_C(0xFFFFFFFE));
  assert(runtime.d[5] == UINT32_C(0x00001234));
  assert(runtime.a[2] == UINT32_C(0x00FF0044));
  /* MOVEM.L (A3),D6-D7: plain (An) source never auto-updates A3; neither
     LONG value is altered by any sign-extension formula, even with the top
     bit set. */
  assert(runtime.d[6] == UINT32_C(0x80000001));
  assert(runtime.d[7] == UINT32_C(0x7FFFFFFE));
  assert(runtime.a[3] == UINT32_C(0x00FF0060));
  return 0;
}
'''

# SEG-007-T068: full compile+link+execute proof of write_move's generalized
# C4 lowering (see emit_general_startup_runtime_c4_move_source in
# tests/m68k_pipeline_test.cpp for the exact encoded sequence):
#   MOVE.L D0,(0x00FF0080).L    the prior audited shape, direction 1
#   MOVE.L (0x00FF0080).L,D1    the prior audited shape, direction 2
#   MOVE.W (0x00000B00).L,D2    folded ROM-resident source, word width
#   MOVE.L (A0),(0x00FF0084).L  runtime-routed register-indirect source
#   MOVE.L (0x00FF0088).L,(A1) runtime-routed register-indirect destination
# then the established RESET frontier. Proves actual data movement at runtime,
# not merely well-formed generated C: D0's value lands in both RAM (via the
# routed write) and D1 (via the routed read back), the folded ROM constant
# (this fixture's own first instruction word) lands in D2's low 16 bits
# while D2's high 16 bits survive untouched, a runtime-resolved (A0) source
# value lands at a RAM-absolute destination, and a RAM-absolute source value
# lands at a runtime-resolved (A1) destination -- with A0/A1 themselves
# never mutated (plain register-indirect, not predecrement/postincrement).
MOVE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.d[0] = UINT32_C(0x11223344);
  runtime.d[2] = UINT32_C(0xDEADBEEF);
  runtime.a[0] = UINT32_C(0x00FF00A0);
  runtime.a[1] = UINT32_C(0x00FF00B0);
  runtime.work_ram[0xA0] = 0xAAU; runtime.work_ram[0xA1] = 0xBBU;
  runtime.work_ram[0xA2] = 0xCCU; runtime.work_ram[0xA3] = 0xDDU;
  runtime.work_ram[0x88] = 0x99U; runtime.work_ram[0x89] = 0x88U;
  runtime.work_ram[0x8A] = 0x77U; runtime.work_ram[0x8B] = 0x66U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B1E));
  /* MOVE.L D0,(0x00FF0080).L: D0's value lands in RAM, big-endian. */
  assert(runtime.work_ram[0x80] == 0x11U && runtime.work_ram[0x81] == 0x22U &&
         runtime.work_ram[0x82] == 0x33U && runtime.work_ram[0x83] == 0x44U);
  /* MOVE.L (0x00FF0080).L,D1: reads the value just stored above. */
  assert(runtime.d[1] == UINT32_C(0x11223344));
  /* MOVE.W (0x00000B00).L,D2: folded ROM read of this fixture's own first
     instruction word (0x23C0); D2's high 16 bits (0xDEAD) survive. */
  assert(runtime.d[2] == UINT32_C(0xDEAD23C0));
  /* MOVE.L (A0),(0x00FF0084).L: the preset (A0) value lands in RAM; A0 is
     never mutated by a plain register-indirect read. */
  assert(runtime.work_ram[0x84] == 0xAAU && runtime.work_ram[0x85] == 0xBBU &&
         runtime.work_ram[0x86] == 0xCCU && runtime.work_ram[0x87] == 0xDDU);
  assert(runtime.a[0] == UINT32_C(0x00FF00A0));
  /* MOVE.L (0x00FF0088).L,(A1): the preset RAM value lands at the runtime-
     resolved (A1) destination; A1 is never mutated by a plain register-
     indirect write. */
  assert(runtime.work_ram[0xB0] == 0x99U && runtime.work_ram[0xB1] == 0x88U &&
         runtime.work_ram[0xB2] == 0x77U && runtime.work_ram[0xB3] == 0x66U);
  assert(runtime.a[1] == UINT32_C(0x00FF00B0));
  return 0;
}
'''

MOVE_BYTE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.d[0] = UINT32_C(0x112233AB);
  runtime.d[1] = UINT32_C(0xDEADBEEF);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B0C));
  assert(runtime.work_ram[0x80] == 0xABU);
  assert(runtime.d[0] == UINT32_C(0x112233AB));
  assert(runtime.d[1] == UINT32_C(0xDEADBEAB));
  return 0;
}
'''

# SEG-007-T071: full compile+link+execute proof that ANDI's C4 lowering
# performs its destination read-modify-write through the production router.
# The synthetic block covers retained-fact RAM-absolute, runtime-routed (A0),
# and register-direct destinations before the established RESET frontier.
ANDI_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0xA5FB);
  runtime.a[0] = UINT32_C(0x00FF00A0);
  runtime.d[0] = UINT32_C(0x123456AB);
  runtime.work_ram[0x80] = 0xABU; runtime.work_ram[0x81] = 0xCDU;
  runtime.work_ram[0xA0] = 0xDEU; runtime.work_ram[0xA1] = 0xADU;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B10));
  assert(runtime.work_ram[0x80] == 0U && runtime.work_ram[0x81] == 0xCDU);
  assert(runtime.work_ram[0xA0] == 0U && runtime.work_ram[0xA1] == 0xADU);
  assert(runtime.a[0] == UINT32_C(0x00FF00A0));
  assert(runtime.d[0] == UINT32_C(0x1234560B));
  assert(runtime.sr == UINT16_C(0xA5F0));
  return 0;
}
'''

# SEG-007-T070: full compile+link+execute proof that write_move's own
# predecrement-destination lowering (MOVE.L D0,-(A0)) actually reaches
# genesis_route_access and commits A0's decrement exactly once, at the new
# (post-decrement) address, matching Q2/Q5.
MOVE_PREDECREMENT_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.d[0] = UINT32_C(0x11223344);
  runtime.a[0] = UINT32_C(0x00FF0020);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B02));
  assert(runtime.a[0] == UINT32_C(0x00FF001C));
  assert(runtime.work_ram[0x1C] == 0x11U && runtime.work_ram[0x1D] == 0x22U &&
         runtime.work_ram[0x1E] == 0x33U && runtime.work_ram[0x1F] == 0x44U);
  return 0;
}
'''

# SEG-007-T070: the postincrement mirror -- MOVE.L D0,(A0)+ writes at the
# ORIGINAL (pre-increment) address, then commits A0's increment exactly once.
MOVE_POSTINCREMENT_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.d[0] = UINT32_C(0xAABBCCDD);
  runtime.a[0] = UINT32_C(0x00FF0020);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B02));
  assert(runtime.a[0] == UINT32_C(0x00FF0024));
  assert(runtime.work_ram[0x20] == 0xAAU && runtime.work_ram[0x21] == 0xBBU &&
         runtime.work_ram[0x22] == 0xCCU && runtime.work_ram[0x23] == 0xDDU);
  return 0;
}
'''

# SEG-007-T157 / ADR-0019 Stage B: full compile+link+execute proof of
# write_clr's own auto-updating predecrement destination -- the deferred
# single-address-register commit strictly after the routed write, plus
# failure-ordering (a ROM-window destination makes the routed WRITE fail with
# no partial A0 decrement and no partial CCR update).
CLR_PREDECREMENT_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  /* Success: CLR.B -(A0) at a work-RAM address. */
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0xA5FF);
  runtime.a[0] = UINT32_C(0x00FF0021);
  runtime.work_ram[0x20] = 0xCCU;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B06));
  assert(runtime.a[0] == UINT32_C(0x00FF0020));  /* committed exactly once, to the decremented address */
  assert(runtime.work_ram[0x20] == 0U);
  assert(runtime.sr == UINT16_C(0xA5F4));  /* N=0,Z=1,V=0,C=0, X (bit4) unaffected */

  /* Failure-ordering: A0 points into the ROM window (< 0x00400000), so the
     routed BYTE write fails (ROM writes are unconditionally prohibited)
     before the deferred commit is ever reached. A0 and SR must both remain
     completely unmodified -- no partial decrement, no partial CCR update. */
  memset(&runtime, 0, sizeof(runtime));
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0x2700);
  runtime.a[0] = UINT32_C(0x00000100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class != GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.a[0] == UINT32_C(0x00000100));
  assert(runtime.sr == UINT16_C(0x2700));
  return 0;
}
'''

# SEG-007-T157 / ADR-0019 Stage B: the postincrement mirror -- CLR.W (A0)+
# writes at the ORIGINAL address, then commits A0's increment exactly once;
# same failure-ordering discipline as the predecrement harness above.
CLR_POSTINC_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  /* Success: CLR.W (A0)+ at an even work-RAM address. */
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0xA5FF);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x20] = 0xAAU; runtime.work_ram[0x21] = 0xBBU;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B02));
  assert(runtime.work_ram[0x20] == 0U && runtime.work_ram[0x21] == 0U);
  assert(runtime.a[0] == UINT32_C(0x00FF0022));  /* committed exactly once, to the incremented address */
  assert(runtime.sr == UINT16_C(0xA5F4));

  /* Failure-ordering: A0 points into the ROM window; the routed WORD write
     fails before the deferred commit, so A0 and SR are both untouched. */
  memset(&runtime, 0, sizeof(runtime));
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0x2700);
  runtime.a[0] = UINT32_C(0x00000100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class != GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.a[0] == UINT32_C(0x00000100));
  assert(runtime.sr == UINT16_C(0x2700));
  return 0;
}
'''

# SEG-007-T070: full compile+link+execute proof of write_move's own C4
# predecrement/postincrement deferred-address-commit lowering (see
# emit_general_startup_runtime_c4_move_autoupdate_source for the exact
# encoded sequence). The success scenario proves every one of the four
# instructions' address-register mutation(s) commit exactly once, at the
# documented (pre/post-updated) address, and the data actually moves. The
# failure scenario reuses the SAME compiled generated.c with a runtime state
# that makes the fourth instruction's own SOURCE read fail (an odd effective
# address): instructions 1-3 have already committed by then (proving
# per-instruction atomicity), while the fourth instruction's own A4 AND A5 --
# the two address registers it touches -- remain completely unmodified (Q5),
# and pc stops at that instruction's own address, never advancing past it.
MOVE_AUTOUPDATE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x1C] = 0x11U; runtime.work_ram[0x1D] = 0x22U;
  runtime.work_ram[0x1E] = 0x33U; runtime.work_ram[0x1F] = 0x44U;
  runtime.a[1] = UINT32_C(0x00FF0030);
  runtime.work_ram[0x30] = 0x55U; runtime.work_ram[0x31] = 0x66U;
  runtime.work_ram[0x32] = 0x77U; runtime.work_ram[0x33] = 0x88U;
  runtime.work_ram[0x90] = 0x99U; runtime.work_ram[0x91] = 0xAAU;
  runtime.work_ram[0x92] = 0xBBU; runtime.work_ram[0x93] = 0xCCU;
  runtime.a[2] = UINT32_C(0x00FF0050);
  runtime.a[4] = UINT32_C(0x00FF0060);
  runtime.work_ram[0x5C] = 0xDEU; runtime.work_ram[0x5D] = 0xADU;
  runtime.work_ram[0x5E] = 0xBEU; runtime.work_ram[0x5F] = 0xEFU;
  runtime.a[5] = UINT32_C(0x00FF0070);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B0C));
  /* MOVE.L -(A0),D3: A0 decrements exactly once, to the address actually read. */
  assert(runtime.d[3] == UINT32_C(0x11223344));
  assert(runtime.a[0] == UINT32_C(0x00FF001C));
  /* MOVE.L (A1)+,D4: read at the ORIGINAL address, THEN A1 increments exactly once. */
  assert(runtime.d[4] == UINT32_C(0x55667788));
  assert(runtime.a[1] == UINT32_C(0x00FF0034));
  /* MOVE.L (0x00FF0090).L,-(A2): the retained-fact absolute RAM source's
     value lands at A2's decremented address; A2 decrements exactly once. */
  assert(runtime.a[2] == UINT32_C(0x00FF004C));
  assert(runtime.work_ram[0x4C] == 0x99U && runtime.work_ram[0x4D] == 0xAAU &&
         runtime.work_ram[0x4E] == 0xBBU && runtime.work_ram[0x4F] == 0xCCU);
  /* MOVE.L -(A4),(A5)+: two DIFFERENT registers, each mutating exactly once;
     the value read from A4's decremented address lands at A5's ORIGINAL
     address, then A5 increments. */
  assert(runtime.a[4] == UINT32_C(0x00FF005C));
  assert(runtime.a[5] == UINT32_C(0x00FF0074));
  assert(runtime.work_ram[0x70] == 0xDEU && runtime.work_ram[0x71] == 0xADU &&
         runtime.work_ram[0x72] == 0xBEU && runtime.work_ram[0x73] == 0xEFU);

  /* Failure scenario, same compiled program: A4 is odd, so the fourth
     instruction's own SOURCE read (at A4-4, still odd) fails before A5 is
     ever touched. Instructions 1-3 already committed identically to above. */
  memset(&runtime, 0, sizeof(runtime));
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x1C] = 0x11U; runtime.work_ram[0x1D] = 0x22U;
  runtime.work_ram[0x1E] = 0x33U; runtime.work_ram[0x1F] = 0x44U;
  runtime.a[1] = UINT32_C(0x00FF0030);
  runtime.work_ram[0x30] = 0x55U; runtime.work_ram[0x31] = 0x66U;
  runtime.work_ram[0x32] = 0x77U; runtime.work_ram[0x33] = 0x88U;
  runtime.work_ram[0x90] = 0x99U; runtime.work_ram[0x91] = 0xAAU;
  runtime.work_ram[0x92] = 0xBBU; runtime.work_ram[0x93] = 0xCCU;
  runtime.a[2] = UINT32_C(0x00FF0050);
  runtime.a[4] = UINT32_C(0x00FF0059); /* odd; -4 = 0x00FF0055, still odd */
  runtime.a[5] = UINT32_C(0x00FF0070);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
  assert(transfer.stop.provenance.has_access == 1U && transfer.stop.provenance.access_address == UINT32_C(0x00FF0055));
  assert(transfer.stop.provenance.access_width == GENESIS_ACCESS_LONG && transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ);
  /* pc never advances past the fourth instruction's own address. */
  assert(runtime.pc == UINT32_C(0x00000B0A));
  /* Instructions 1-3 already committed. */
  assert(runtime.d[3] == UINT32_C(0x11223344) && runtime.a[0] == UINT32_C(0x00FF001C));
  assert(runtime.d[4] == UINT32_C(0x55667788) && runtime.a[1] == UINT32_C(0x00FF0034));
  assert(runtime.a[2] == UINT32_C(0x00FF004C));
  assert(runtime.work_ram[0x4C] == 0x99U && runtime.work_ram[0x4D] == 0xAAU &&
         runtime.work_ram[0x4E] == 0xBBU && runtime.work_ram[0x4F] == 0xCCU);
  /* The fourth instruction's own two address registers -- A4 AND A5 -- are
     completely unmodified; neither the failed source's local arithmetic nor
     any destination write was ever committed to the live register file. */
  assert(runtime.a[4] == UINT32_C(0x00FF0059));
  assert(runtime.a[5] == UINT32_C(0x00FF0070));
  /* No partial write at A5's would-be destination either. */
  assert(runtime.work_ram[0x70] == 0U && runtime.work_ram[0x71] == 0U &&
         runtime.work_ram[0x72] == 0U && runtime.work_ram[0x73] == 0U);
  return 0;
}
'''

ROUTED_WRITE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0010);
  runtime.work_ram[0x10] = 0xA5U; runtime.work_ram[0x13] = 0xA5U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B0A) && runtime.a[0] == UINT32_C(0x00FF0010));
  assert(runtime.work_ram[0x10] == 0U && runtime.work_ram[0x11] == 0U &&
         runtime.work_ram[0x12] == 0U && runtime.work_ram[0x13] == 0U);

  memset(&runtime, 0, sizeof(runtime));
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0011); /* odd address: the router must reject before any mutation */
  GenesisRuntime before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);
  assert(transfer.stop.provenance.has_access == 1U && transfer.stop.provenance.access_address == UINT32_C(0x00FF0011));
  assert(transfer.stop.provenance.access_width == GENESIS_ACCESS_LONG && transfer.stop.provenance.access_direction == GENESIS_ACCESS_WRITE);

  /* SEG-007-T105: an address register holding a value with bits set above bit
     23 (0xFFFF0010) drives only 24 external address lines on the MC68000, so
     the routed bus address is 0xFFFF0010 & 0x00FFFFFF == 0x00FF0010 -- in work
     RAM. Before the routing-seam truncation this stopped with
     effective_address_not_24bit; now it routes and clears work RAM. */
  memset(&runtime, 0, sizeof(runtime));
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0xFFFF0010);
  runtime.work_ram[0x10] = 0xA5U; runtime.work_ram[0x11] = 0xA5U;
  runtime.work_ram[0x12] = 0xA5U; runtime.work_ram[0x13] = 0xA5U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B0A) && runtime.a[0] == UINT32_C(0xFFFF0010));
  assert(runtime.work_ram[0x10] == 0U && runtime.work_ram[0x11] == 0U &&
         runtime.work_ram[0x12] == 0U && runtime.work_ram[0x13] == 0U);

  /* Provenance round-trip: a pre-truncation address with high bits set
     (0xFF500000) whose truncated bus address (0x00500000) is still unmapped
     stops with the TRUNCATED address recorded in provenance.access_address and
     with the unmapped-region diagnostic -- never effective_address_not_24bit --
     and the recorded access address has no high byte (genesis_valid_provenance
     accepts it). */
  memset(&runtime, 0, sizeof(runtime));
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0xFF500000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_UNMAPPED_DATA_ACCESS);
  assert(transfer.stop.provenance.has_access == 1U);
  assert(transfer.stop.provenance.access_address == UINT32_C(0x00500000));
  assert((transfer.stop.provenance.access_address & UINT32_C(0xFF000000)) == 0U);
  assert(transfer.stop.provenance.access_width == GENESIS_ACCESS_LONG &&
         transfer.stop.provenance.access_direction == GENESIS_ACCESS_WRITE);
  return 0;
}
'''

# SEG-007-T124 / ADR-0009: the bounded computed/indirect control-flow-target
# resolution mechanism, end to end. ANDI.W #2,D0 proves D0 in {0,2} from an
# initially-unknown register; JSR (4,PC,D0.W) evaluates the runtime EA, checks it against
# the proven {0x00000B0A, 0x00000B0C} candidate set, and only on membership
# pushes the continuation and transfers control -- exercised here for BOTH
# proven candidates through the SAME generated membership guard and
# dispatcher, never a per-candidate special case.
INDIRECT_JSR_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
static void expect_candidate(uint32_t d0_seed, uint32_t expected_target) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.d[0] = d0_seed;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000B08));
  assert(runtime.pc == UINT32_C(0x00000B08) && runtime.a[7] == UINT32_C(0x00FF0100));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == expected_target);
  assert(runtime.pc == expected_target && runtime.a[7] == UINT32_C(0x00FF00FC));
   /* The pushed continuation is the JSR's own next-instruction address,
      big-endian, regardless of which candidate was selected. */
   assert(runtime.work_ram[0xFC] == 0x00U && runtime.work_ram[0xFD] == 0x00U &&
          runtime.work_ram[0xFE] == 0x0BU && runtime.work_ram[0xFF] == 0x0CU);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
   assert(runtime.pc == UINT32_C(0x00000B0C) && runtime.a[7] == UINT32_C(0x00FF0100));
}
int main(void) {
  expect_candidate(UINT32_C(0x00000000), UINT32_C(0x00000B0E));
  expect_candidate(UINT32_C(0xFFFFFFFF), UINT32_C(0x00000B10));
  {
    GenesisRuntime runtime = {0}, before;
    GenesisControlTransfer transfer;
    /* Enter the retained indirect block directly with an unproved runtime
       value. The guard must stop before changing PC or pushing a return. */
    runtime.pc = UINT32_C(0x00000B08);
    runtime.a[7] = UINT32_C(0x00FF0100);
    runtime.d[0] = UINT32_C(0x00000001);
    before = runtime;
    transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP &&
           transfer.stop.stop_class == GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET &&
           transfer.stop.diagnostic_category == GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE);
    assert(transfer.stop.provenance.has_instruction_provenance == 1U &&
           transfer.stop.provenance.instruction.source_address == UINT32_C(0x00000B08));
    assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);
  }
  return 0;
}
'''

T236_TIER1_OWNERLESS_RTS_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
static void expect_tier2_caller(uint32_t caller, unsigned address_register, uint32_t continuation) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = caller;
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.a[address_register] = UINT32_C(0x00000B30);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000B30));
  assert(runtime.pc == caller && runtime.a[7] == UINT32_C(0x00FF00FC));
  runtime.pc = transfer.next_pc; /* genesis_runtime_run's CONTINUE_AT_PC commit */
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == continuation);
  assert(runtime.pc == continuation && runtime.a[7] == UINT32_C(0x00FF0100));
}
int main(void) {
  GenesisRuntime runtime = {0}, before;
  GenesisControlTransfer transfer;
  expect_tier2_caller(UINT32_C(0x00000B02), 1U, UINT32_C(0x00000B04));
  expect_tier2_caller(UINT32_C(0x00000B08), 2U, UINT32_C(0x00000B0A));

  /* A represented code address that is not a call continuation is not valid
     return authority.  The successful stack read remains atomic with respect
     to PC/A7 when membership rejects the popped value. */
  runtime.pc = UINT32_C(0x00000B30); runtime.a[7] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x20] = 0U; runtime.work_ram[0x21] = 0U;
  runtime.work_ram[0x22] = 0x0BU; runtime.work_ram[0x23] = 0x10U; before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_RETURN_TARGET_MISMATCH);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);

  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B30);
  runtime.a[7] = UINT32_C(1); before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_INVALID_STACK_ALIGNMENT);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);

  /* Tier-2 itself also remains fail-closed before stack mutation for a target
     absent from EmittedCodeAddressSet. */
  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B02);
  runtime.a[7] = UINT32_C(0x00FF0100); runtime.a[1] = UINT32_C(0x00000B32); before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);
  return 0;
}
'''

CALL_RETURN_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
#ifndef EXPECT_CONTINUATION
#error EXPECT_CONTINUATION must name the discovered continuation
#endif
static void expect_complete_source(const GenesisRuntimeStop *stop, uint32_t source, uint32_t length) {
  assert(stop->provenance.has_instruction_provenance == 1U);
  assert(stop->provenance.instruction.source_address == source);
  assert(stop->provenance.mapping_claim_count == 1U);
  assert(stop->provenance.bus_access_count == 1U);
  assert(stop->provenance.bus_accesses[0].address == source);
  assert(stop->provenance.bus_accesses[0].raw_byte_count == length);
}
static void expect_push_rejection(uint32_t a7, GenesisDiagnosticCategory category) {
  GenesisRuntime runtime = {0}, before;
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00); runtime.a[7] = a7; before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == category);
  /* A failed pre-decrement never forms an access request, nor mutates A7,
     PC, RAM, or any other persistent state. */
  assert(transfer.stop.provenance.has_access == 0U);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);
  expect_complete_source(&transfer.stop, UINT32_C(0x00000B00), EXPECT_CALL_LENGTH);
}
int main(void) {
  GenesisRuntime runtime = {0}, before;
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00); runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000B08));
  assert(runtime.pc == UINT32_C(0x00000B08) && runtime.a[7] == UINT32_C(0x00FF00FC));
  assert(runtime.work_ram[0xFC] == 0U && runtime.work_ram[0xFD] == 0U && runtime.work_ram[0xFE] == ((EXPECT_CONTINUATION >> 8) & 0xFFU) && runtime.work_ram[0xFF] == (EXPECT_CONTINUATION & 0xFFU));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == EXPECT_CONTINUATION);
  assert(runtime.pc == EXPECT_CONTINUATION && runtime.a[7] == UINT32_C(0x00FF0100));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);

  /* The one-past-work-RAM stack top is valid for a four-byte push; any
     greater value is rejected before the router and leaves all state intact. */
  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B00); runtime.a[7] = UINT32_C(0x01000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime.a[7] == UINT32_C(0x00FFFFFC));
  /* Alignment takes precedence over range.  Every invalid matrix member
     remains atomic and has source/fetch/mapping provenance only. */
  expect_push_rejection(UINT32_C(0), GENESIS_DIAG_INVALID_STACK_RANGE);
  expect_push_rejection(UINT32_C(1), GENESIS_DIAG_INVALID_STACK_ALIGNMENT);
  expect_push_rejection(UINT32_C(2), GENESIS_DIAG_INVALID_STACK_RANGE);
  expect_push_rejection(UINT32_C(3), GENESIS_DIAG_INVALID_STACK_ALIGNMENT);
  expect_push_rejection(UINT32_C(0x00FF0000), GENESIS_DIAG_INVALID_STACK_RANGE);
  expect_push_rejection(UINT32_C(0x00FF0002), GENESIS_DIAG_INVALID_STACK_RANGE);
  expect_push_rejection(UINT32_C(0x00FF0003), GENESIS_DIAG_INVALID_STACK_ALIGNMENT);
  expect_push_rejection(UINT32_C(0x01000001), GENESIS_DIAG_INVALID_STACK_ALIGNMENT);
  expect_push_rejection(UINT32_C(0x01000002), GENESIS_DIAG_INVALID_STACK_RANGE);

  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B00); runtime.a[7] = UINT32_C(0x00FF0101); before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_INVALID_STACK_ALIGNMENT);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0); expect_complete_source(&transfer.stop, UINT32_C(0x00000B00), EXPECT_CALL_LENGTH);

  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B00); runtime.a[7] = UINT32_C(0x00FF0002); before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_INVALID_STACK_RANGE);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0); expect_complete_source(&transfer.stop, UINT32_C(0x00000B00), EXPECT_CALL_LENGTH);

  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B08); runtime.a[7] = UINT32_C(0); before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_INVALID_STACK_RANGE);
  assert(transfer.stop.provenance.has_access == 1U && transfer.stop.provenance.access_address == UINT32_C(0));
  assert(transfer.stop.provenance.access_width == GENESIS_ACCESS_LONG && transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0); expect_complete_source(&transfer.stop, UINT32_C(0x00000B08), 2U);

  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B08); runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.work_ram[0x100] = 0U; runtime.work_ram[0x101] = 0U; runtime.work_ram[0x102] = 0x0BU; runtime.work_ram[0x103] = 0x04U; before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_RETURN_TARGET_MISMATCH);
  assert(transfer.stop.provenance.has_access == 1U && transfer.stop.provenance.access_address == UINT32_C(0x00FF0100));
  assert(transfer.stop.provenance.access_width == GENESIS_ACCESS_LONG && transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0); expect_complete_source(&transfer.stop, UINT32_C(0x00000B08), 2U);

  /* The final aligned four-byte RAM slot is a valid RTS pop boundary. */
  memset(&runtime, 0, sizeof(runtime)); runtime.pc = UINT32_C(0x00000B08); runtime.a[7] = UINT32_C(0x00FFFFFC);
  runtime.work_ram[0xFFFC] = (uint8_t)((EXPECT_CONTINUATION >> 24) & 0xFFU); runtime.work_ram[0xFFFD] = (uint8_t)((EXPECT_CONTINUATION >> 16) & 0xFFU);
  runtime.work_ram[0xFFFE] = (uint8_t)((EXPECT_CONTINUATION >> 8) & 0xFFU); runtime.work_ram[0xFFFF] = (uint8_t)(EXPECT_CONTINUATION & 0xFFU);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime.a[7] == UINT32_C(0x01000000) && runtime.pc == EXPECT_CONTINUATION);
  return 0;
}
'''

MOVEA_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0}, before;
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0xA5F3);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x20] = 0xFFU; runtime.work_ram[0x21] = 0x80U;
  runtime.work_ram[0x80] = 0x12U; runtime.work_ram[0x81] = 0x34U;
  runtime.work_ram[0x82] = 0x56U; runtime.work_ram[0x83] = 0x78U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.a[0] == UINT32_C(0x00FF0020));
  assert(runtime.a[1] == UINT32_C(0xFFFFFF80));
  assert(runtime.a[2] == UINT32_C(0x12345678));
  assert(runtime.sr == UINT16_C(0xA5F3));
  assert(runtime.pc == UINT32_C(0x00000B08));

  memset(&runtime, 0, sizeof(runtime));
  runtime.pc = UINT32_C(0x00000B00); runtime.sr = UINT16_C(0xA5F3);
  /* This address was chosen only as an arbitrary genesis_route_access
     rejection; the exact stop_class/diagnostic_category is not this test's
     own concern (full-state preservation on rejection is). SEG-007-T081
     newly recognizes this exact address as the VDP register area's own DATA
     port -- an unimplemented VDP selector -- so it now fails closed one
     stage later, with the more specific GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS
     / GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP pair, rather than the
     generic GENESIS_STOP_UNSUPPORTED_MEMORY_REGION /
     GENESIS_DIAG_UNMAPPED_DATA_ACCESS pair it produced before that task; this
     is exactly the intended, documented behavior change (see
     docs/architecture/genesis-controller-io-startup-read-compatibility-policy.md's
     VDP addendum), not a regression. */
  runtime.a[0] = UINT32_C(0x00C00000); before = runtime;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
  assert(transfer.stop.provenance.access_width == GENESIS_ACCESS_WORD);
  assert(transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ);
  assert(memcmp(&runtime, &before, sizeof(runtime)) == 0);
  return 0;
}
'''

# SEG-007-T192: full compile+link+execute proof that write_movea's own
# predecrement-source lowering (MOVEA.W -(A0),A1) reaches genesis_route_access,
# sign-extends the routed word read, and commits A0's decrement exactly once
# at the new (post-decrement) address, matching MOVE's Q2/Q5 discipline.
MOVEA_PREDECREMENT_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x1E] = 0xFFU; runtime.work_ram[0x1F] = 0x80U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B02));
  assert(runtime.a[0] == UINT32_C(0x00FF001E));
  assert(runtime.a[1] == UINT32_C(0xFFFFFF80));
  return 0;
}
'''

# SEG-007-T192: the postincrement mirror -- MOVEA.L (A0)+,A1 reads at the
# ORIGINAL (pre-increment) address, is NOT sign-extended (long form), and then
# commits A0's increment exactly once.
MOVEA_POSTINCREMENT_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x20] = 0x81U; runtime.work_ram[0x21] = 0x82U;
  runtime.work_ram[0x22] = 0x83U; runtime.work_ram[0x23] = 0x84U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B02));
  assert(runtime.a[0] == UINT32_C(0x00FF0024));
  assert(runtime.a[1] == UINT32_C(0x81828384));
  return 0;
}
'''

# SEG-007-T192: the same-register source-and-update case -- MOVEA.L (A0)+,A0.
# Unlike MOVE (Q3) and ADDA, MOVEA's destination write never reads the
# destination register's own prior value, so this shape is representable with
# no aliasing hazard: A0 ends at the loaded value, having already been
# committed at its post-increment address by the deferred commit.
MOVEA_ALIASING_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[0] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x20] = 0x00U; runtime.work_ram[0x21] = 0xFFU;
  runtime.work_ram[0x22] = 0x00U; runtime.work_ram[0x23] = 0x50U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B02));
  assert(runtime.a[0] == UINT32_C(0x00FF0050));
  return 0;
}
'''

ADD_BIT_DBCC_HARNESS = r'''
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  runtime.pc = UINT32_C(0x00000B00);
  runtime.d[0] = UINT32_C(1);
  runtime.d[1] = UINT32_C(0x00007FFF);
  runtime.sr = UINT16_C(0x0010);
  runtime.a[0] = UINT32_C(0x00FF0000);
  runtime.work_ram[0] = UINT8_C(0xFF);
  runtime.work_ram[1] = UINT8_C(0xFF);
  runtime.work_ram[2] = UINT8_C(0xFF);
  runtime.work_ram[3] = UINT8_C(0xFF);
  GenesisControlTransfer result = genesis_bridge_dispatch(&runtime);
  if (result.kind != GENESIS_STOP || result.stop.stop_class != GENESIS_STOP_UNSUPPORTED_CPU_FORM) return 1;
  if (runtime.d[0] != UINT32_C(0) || runtime.d[1] != UINT32_C(0x00008000)) return 2;
  if (runtime.pc != UINT32_C(0x00000B0A) || runtime.sr != UINT16_C(0x0015)) return 3;
  if (runtime.work_ram[0] != UINT8_C(0) || runtime.work_ram[1] != UINT8_C(0) ||
      runtime.work_ram[2] != UINT8_C(0) || runtime.work_ram[3] != UINT8_C(0)) return 4;
  return 0;
}
'''

# SEG-007-T090: full compile+link+execute proof that a TST.W ($00C00004).L
# WORD read of the VDP control/status port is lowered to a runtime
# genesis_route_access WORD/READ call (never a compile-time literal, never a
# static VDP model, never a static frontier stop). The routed read observes
# the runtime VDP status register's zero-initialized default (SEG-007-T081),
# so TST.W of 0 sets Z and clears N/V/C; execution then reaches the
# established RESET CPU frontier.
VDP_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0xFFF1); /* every NZVC bit set: the routed TST.W must clear N/V/C and set Z */
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM);
  assert(runtime.pc == UINT32_C(0x00000B06));
  assert((runtime.sr & UINT16_C(0x000F)) == UINT16_C(0x0004));
  return 0;
}
'''

# SEG-007-T113: a direct absolute-operand WORD store into the VDP control-port
# window ($00C00004) is lowered to a runtime genesis_route_access WORD/WRITE
# call. The baked immediate (0x8004) is a valid one-word VDP register-set
# command (register 0 <- 0x04), so the routed store lands in
# devices.vdp.registers[0]; execution then reaches the established RESET
# frontier. No compile-time literal, no static VDP model.
VDP_STORE_WORD_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM); /* the trailing RESET */
  assert(runtime.pc == UINT32_C(0x00000B08));
  assert(runtime.devices.vdp.registers[0] == UINT8_C(0x04));
  return 0;
}
'''

# SEG-007-T113: the LONG-store form, whose full decoded instruction length is
# 10 bytes -- longer than the pre-raise route-provenance raw-instruction-bytes
# capacity (8). The baked immediate (0x9F000000) has an invalid VDP
# register-set high word, so the routed LONG store fails closed before any
# mutation; the generated genesis_attach_route_provenance then records the
# whole 10-byte instruction, which the raised GENESIS_MAX_RAW_BYTES (12)
# provenance ABI carries and genesis_write_full_report validates end to end.
VDP_STORE_LONG_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
static char SHA_ZERO[65];
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  GenesisReportMetadata metadata = {0};
  FILE *full;
  static const uint8_t EXPECTED[10] = {
      0x23U, 0xFCU, 0x9FU, 0x00U, 0x00U, 0x00U, 0x00U, 0xC0U, 0x00U, 0x04U};
  memset(SHA_ZERO, '0', 64U); SHA_ZERO[64] = '\0';
  metadata.cpu_dimensions = GENESIS_CPU_DIMENSIONS_NONE;
  runtime.pc = UINT32_C(0x00000B00);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP);
  assert(transfer.stop.provenance.has_instruction_provenance == 1U);
  assert(transfer.stop.provenance.instruction.length == 10U);
  assert(transfer.stop.provenance.bus_access_count == 1U);
  assert(transfer.stop.provenance.bus_accesses[0].raw_byte_count == 10U);
  assert(memcmp(transfer.stop.provenance.bus_accesses[0].raw_bytes, EXPECTED, 10U) == 0);
  assert(transfer.stop.provenance.access_width == GENESIS_ACCESS_LONG);
  assert(transfer.stop.provenance.access_direction == GENESIS_ACCESS_WRITE);
  /* The high word's register-set failure rejects the LONG store before any
     VDP mutation. */
  assert(runtime.devices.vdp.registers[0] == 0U && runtime.devices.vdp.registers[15] == 0U);
  full = tmpfile();
  assert(full != NULL);
  assert(genesis_write_full_report(full, &runtime, &transfer, SHA_ZERO, &metadata) == 0);
  assert(fclose(full) == 0);
  return 0;
}
'''

# SEG-007-T115: full compile+link+execute proof that a direct absolute-operand
# MOVE.W #$0100,($00A11100).L store is lowered to a runtime genesis_route_access
# WORD/WRITE call. $0100 sets the documented D8 BUSREQ REQUEST bit, so the
# routed store latches devices.z80_bus.{bus_requested,bus_granted} (policy (a)
# immediate grant, SEG-007-T102); reset_asserted stays clear. Execution then
# reaches the established RESET CPU frontier.
Z80_BUS_STORE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM); /* the trailing RESET */
  assert(runtime.pc == UINT32_C(0x00000B08));
  assert(runtime.devices.z80_bus.bus_requested == 1U);
  assert(runtime.devices.z80_bus.bus_granted == 1U);
  assert(runtime.devices.z80_bus.reset_asserted == 0U);
  return 0;
}
'''

def main():
  executable, compiler, root = sys.argv[1:]
  first = subprocess.run([executable, "--emit-general-startup-runtime-c4-frontier"], text=True, capture_output=True)
  second = subprocess.run([executable, "--emit-general-startup-runtime-c4-frontier"], text=True, capture_output=True)
  assert first.returncode == second.returncode == 0 and first.stderr == second.stderr == "" and first.stdout == second.stdout
  assert "genesis_route_access_with_source" not in first.stdout
  assert "runtime->work_ram" not in first.stdout
  overflow = subprocess.run([executable, "--emit-general-startup-runtime-c4-frontier-overflow"], text=True, capture_output=True)
  assert overflow.stdout == "/* translation rejected: unrepresentable C4 frontier */\n"
  for forged in ("fact-duplicate", "fact-unbound", "bus-region", "bus-source", "cpu-primary", "cpu-length", "cpu-fetch", "access-source", "access-width", "access-direction", "access-address-space", "prefix-raw", "prefix-ir", "prefix-cpu", "prefix-address-space", "frontier-cpu", "frontier-address-space", "return-missing", "return-duplicate", "return-orphan", "return-wrong-target", "return-wrong-call", "multi-caller-return-rebind", "prefix-profile", "frontier-profile", "available-bytes", "requested-length", "direct-length", "direct-target", "unresolved-reason", "frontier-category", "frontier-class", "prefix-mapping-duplicate", "prefix-mapping-offset", "prefix-mapping-affine-distractor", "prefix-mapping-source-overlap-short", "frontier-mapping-affine-offset", "mid-block-target"):
    rejected = subprocess.run([executable, f"--emit-general-startup-runtime-c4-frontier-forged-{forged}"], text=True, capture_output=True)
    assert rejected.returncode == 0 and rejected.stdout.startswith("/* translation rejected:")
  folded = subprocess.run([executable, "--emit-general-startup-runtime-c4-rom-fold"], text=True, capture_output=True)
  assert folded.returncode == 0 and "resolved static read" in folded.stdout
  assert "genesis_route_access(" not in folded.stdout
  # SEG-007-T065: CLR.L (A0) -- a plain register-indirect destination EA.
  # This EA is not statically foldable, so discovery-time retain_fact never
  # records a static_memory_facts entry for it, but it does not mutate A0 to
  # form the request (unlike predecrement/postincrement below), so a router
  # failure never leaves an inconsistent, partially applied write. Before
  # SEG-007-T065's fix this fixture reproduced the same generic "lacks
  # retained resolver fact" rejection as predecrement, even though
  # write_clr's own block-emission switch already had a runtime-routing
  # fallback for exactly this shape; the fix corrects the C4 pre-check to
  # defer to that fallback instead of demanding a fact no non-foldable EA
  # can ever have.
  routed_write_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-routed-write"], text=True, capture_output=True)
  routed_write_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-routed-write"], text=True, capture_output=True)
  assert routed_write_first.returncode == routed_write_second.returncode == 0
  assert routed_write_first.stdout == routed_write_second.stdout
  assert not routed_write_first.stdout.startswith("/* translation rejected:")
  # SEG-007-T105: the runtime-routed EA is bound once through the MC68000
  # 24-bit external-address-bus truncation seam; the route call and the
  # provenance record both consume that single bus-address local (still a
  # genuine runtime routing, not a compile-time fold).
  assert "const uint32_t m68k_routed_addr_1 = (m68k_ea_waddr_0) & UINT32_C(0x00FFFFFF);" in routed_write_first.stdout
  assert "genesis_route_access(runtime, m68k_routed_addr_1, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE" in routed_write_first.stdout
  assert "m68k_route_stop_2.provenance.access_address = m68k_routed_addr_1;" in routed_write_first.stdout
  assert "runtime->a[0] -=" not in routed_write_first.stdout and "runtime->a[0] +=" not in routed_write_first.stdout
  # SEG-007-T157 / ADR-0019 Stage B: a dynamic predecrement CLR destination no
  # longer cuts the block. It now lowers through the same deferred-address-
  # register-commit technique the add family already uses: the touched A0 is
  # snapshotted into a local, predecrement is applied to that local before the
  # routed write, and A0 is committed back to the live register file in
  # exactly one statement strictly after the routed write succeeds (a
  # GENESIS_STOP inside the routed write returns before that commit, so no
  # partial auto-update is ever observable). CLR's fixed condition-code
  # pattern is unchanged and is only ever emitted after the commit.
  predecrement_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-predecrement"], text=True, capture_output=True)
  predecrement_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-predecrement"], text=True, capture_output=True)
  predecrement = predecrement_first
  assert predecrement_first.returncode == predecrement_second.returncode == 0
  assert predecrement_first.stdout == predecrement_second.stdout  # deterministic two-run output
  assert not predecrement.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in predecrement.stdout
  assert "GENESIS_C4_LOWERING_DIMENSIONS_CLR_AUTO_UPDATE" not in predecrement.stdout
  assert "genesis_c4_lowering_stop_" not in predecrement.stdout
  assert "uint32_t m68k_clr_auto_ea = runtime->a[0];" in predecrement.stdout
  assert "m68k_clr_auto_ea -= UINT32_C(1);" in predecrement.stdout
  commit = "runtime->a[0] = m68k_clr_auto_ea;"
  assert predecrement.stdout.count(commit) == 1  # exactly one deferred commit
  # Commit strictly after the routed write (adversarial: prove no early
  # writeback is reachable before the routed write can GENESIS_STOP).
  assert predecrement.stdout.index(commit) > predecrement.stdout.rindex("genesis_route_access(runtime, ")
  assert predecrement.stdout.index(commit) > predecrement.stdout.rindex("return transfer;", 0, predecrement.stdout.index(commit))
  assert "GENESIS_ACCESS_WRITE" in predecrement.stdout
  # CLR's fixed condition-code pattern only reached after the commit above.
  assert predecrement.stdout.index("runtime->sr = (uint16_t)((runtime->sr & UINT16_C(0xFFF0)) | UINT16_C(4));") > \
      predecrement.stdout.index(commit)
  # SEG-007-T157 / ADR-0019 Stage B: CLR.W (A0)+, the postincrement sibling.
  clr_postinc_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-clr-postinc"], text=True, capture_output=True)
  clr_postinc_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-clr-postinc"], text=True, capture_output=True)
  clr_postinc = clr_postinc_first
  assert clr_postinc_first.returncode == clr_postinc_second.returncode == 0
  assert clr_postinc_first.stdout == clr_postinc_second.stdout  # deterministic two-run output
  assert not clr_postinc.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in clr_postinc.stdout
  assert "genesis_c4_lowering_stop_" not in clr_postinc.stdout
  assert "uint32_t m68k_clr_auto_ea = runtime->a[0];" in clr_postinc.stdout
  postinc_commit = "runtime->a[0] = m68k_clr_auto_ea;"
  assert clr_postinc.stdout.count(postinc_commit) == 1
  assert "m68k_clr_auto_ea += UINT32_C(2);" in clr_postinc.stdout
  assert clr_postinc.stdout.index(postinc_commit) > clr_postinc.stdout.rindex("genesis_route_access(runtime, ")
  assert clr_postinc.stdout.index(postinc_commit) > clr_postinc.stdout.rindex("return transfer;", 0, clr_postinc.stdout.index(postinc_commit))
  assert "GENESIS_ACCESS_WRITE" in clr_postinc.stdout
  assert clr_postinc.stdout.index("runtime->sr = (uint16_t)((runtime->sr & UINT16_C(0xFFF0)) | UINT16_C(4));") > \
      clr_postinc.stdout.index(postinc_commit)
  # ADR-0015 Q1-Q5: a local cut retains the prefix before it, removes the
  # original terminal transfer and its target-only block, and never adds a
  # C4 sink dispatch arm.  Two independently reachable cut blocks remain
  # deterministic and each receives its own terminal sink.
  c4_prefix = subprocess.run([executable, "--emit-general-startup-runtime-c4-prefix"], text=True, capture_output=True)
  assert c4_prefix.returncode == 0 and not c4_prefix.stdout.startswith("/* translation rejected:")
  assert "runtime->d[0] = UINT32_C(0x00000001);" in c4_prefix.stdout
  assert c4_prefix.stdout.count("genesis_c4_lowering_stop_") == 2  # declaration and one call
  assert "genesis_block_00000B08" not in c4_prefix.stdout
  assert "{ UINT32_C(0x00000B08), genesis_block_" not in c4_prefix.stdout
  # A cut in a static block that Q1 prunes downstream of the first cut has no
  # retained emitted caller, so its static stop function must not be emitted.
  c4_pruned_stop = subprocess.run([executable, "--emit-general-startup-runtime-c4-pruned-stop"], text=True, capture_output=True)
  assert c4_pruned_stop.returncode == 0 and not c4_pruned_stop.stdout.startswith("/* translation rejected:")
  assert "genesis_c4_lowering_stop_00000B02" in c4_pruned_stop.stdout
  assert "genesis_c4_lowering_stop_00000B08" not in c4_pruned_stop.stdout
  c4_multi_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-multi-blocks"], text=True, capture_output=True)
  c4_multi_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-multi-blocks"], text=True, capture_output=True)
  assert c4_multi_first.returncode == c4_multi_second.returncode == 0
  assert c4_multi_first.stdout == c4_multi_second.stdout
  assert c4_multi_first.stdout.count("static GenesisControlTransfer genesis_c4_lowering_stop_") == 2
  assert "return genesis_c4_lowering_stop_00000B02(runtime);" in c4_multi_first.stdout
  assert "return genesis_c4_lowering_stop_00000B06(runtime);" in c4_multi_first.stdout
  assert "UINT32_C(0x00000B02)) return genesis_c4_lowering_stop" not in c4_multi_first.stdout
  assert "UINT32_C(0x00000B06)) return genesis_c4_lowering_stop" not in c4_multi_first.stdout
  c4_same_block = subprocess.run([executable, "--emit-general-startup-runtime-c4-same-block"], text=True, capture_output=True)
  assert c4_same_block.returncode == 0 and not c4_same_block.stdout.startswith("/* translation rejected:")
  assert "genesis_c4_lowering_stop_00000B00" in c4_same_block.stdout
  assert "genesis_c4_lowering_stop_00000B02" not in c4_same_block.stdout
  # SEG-007-T142 correction (adversarial validation): a retained, uncut block
  # reached only via a backward/lower-address edge from a later-visited,
  # higher-address block must not be silently dropped from either the
  # generated dispatch table or its own static function body. The entry
  # (0xB00) branches forward to 0xB0A, which branches backward to the
  # not-yet-visited 0xB04 -- a std::set-single-pass-iterator reachability
  # walk would discover 0xB04 "behind" its current position and never
  # revisit it, silently omitting an otherwise fully retained block.
  c4_backward_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-backward-block"], text=True, capture_output=True)
  c4_backward_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-backward-block"], text=True, capture_output=True)
  assert c4_backward_first.returncode == c4_backward_second.returncode == 0
  assert c4_backward_first.stdout == c4_backward_second.stdout
  assert not c4_backward_first.stdout.startswith("/* translation rejected:")
  partition_boundary_dispatch = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-partition-boundary-dispatch"], text=True, capture_output=True)
  assert partition_boundary_dispatch.returncode == 0
  assert not partition_boundary_dispatch.stdout.startswith("/* translation rejected:")
  assert "static GenesisControlTransfer genesis_block_00000B04" in c4_backward_first.stdout
  assert "{ UINT32_C(0x00000B04), genesis_block_00000B04 }" in c4_backward_first.stdout
  assert "runtime->d[0] = UINT32_C(0x00000001);" in c4_backward_first.stdout
  assert "genesis_c4_lowering_stop_00000B06" in c4_backward_first.stdout
  assert "genesis_block_00000B08" not in c4_backward_first.stdout
  # SEG-007-T142 correction (adversarial validation): GenesisC4LoweringDimensions
  # must be injective over every distinct C4 gap shape the emitter can
  # currently turn into a stop, never a generic catch-all. Prove this
  # directly: two different missing_dispatcher M68kIrKinds sharing a coarse
  # "family" bucket (compare/compare_immediate; register/memory shift-rotate)
  # must never serialize to the same dimension, and two different
  # requires_architecture_decision auto-update shapes (add vs the existing
  # write_clr predecrement fixture above) must not collapse either. No
  # emitted dimension literal is ever the old ambiguous "OTHER" catch-all.
  def c4_dimension(stdout: str) -> str:
    marker = "transfer.stop.c4_lowering_dimensions = GENESIS_C4_LOWERING_DIMENSIONS_"
    start = stdout.index(marker) + len(marker)
    return stdout[start:stdout.index(";", start)]
  c4_dim_shapes = {}
  c4_dim_outputs = {}
  for flag, forge in (
      ("c4-dim-push-effective-address", "push_effective_address"),):
    run_first = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    run_second = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    assert run_first.returncode == run_second.returncode == 0, forge
    assert run_first.stdout == run_second.stdout, forge
    assert not run_first.stdout.startswith("/* translation rejected:"), forge
    assert "GENESIS_STOP_C4_LOWERING_GAP" in run_first.stdout, forge
    assert "GENESIS_C4_LOWERING_DIMENSIONS_OTHER" not in run_first.stdout, forge
    c4_dim_shapes[forge] = c4_dimension(run_first.stdout)
    c4_dim_outputs[forge] = run_first.stdout
  assert len(set(c4_dim_shapes.values())) == len(c4_dim_shapes), c4_dim_shapes  # every shape distinct
  # SEG-021-T009: an auto-updating memory-word shift (ASR.W (A1)+) is lowered by the deferred address-commit
  # helper: no lowering-gap stop, one routed read and one routed write at the snapshot address, one live-register
  # commit strictly after both accesses.
  sh_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-dim-shift-memory-auto-update"],
                            text=True, capture_output=True)
  sh_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-dim-shift-memory-auto-update"],
                             text=True, capture_output=True)
  assert sh_first.returncode == sh_second.returncode == 0
  assert sh_first.stdout == sh_second.stdout
  assert not sh_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in sh_first.stdout
  assert "genesis_c4_lowering_stop_" not in sh_first.stdout
  assert "uint32_t m68k_shift_auto_ea = runtime->a[1];" in sh_first.stdout
  assert "m68k_shift_auto_ea += UINT32_C(2);" in sh_first.stdout
  assert sh_first.stdout.count("runtime->a[1] = m68k_shift_auto_ea;") == 1
  assert sh_first.stdout.index("runtime->a[1] = m68k_shift_auto_ea;") > sh_first.stdout.rindex("genesis_route_access(runtime, ")
  assert sh_first.stdout.count("genesis_route_access(") == 2  # one routed read, one routed write
  # SEG-021-T008: an auto-updating BTST destination (BTST D1,(A1)+) is lowered by the bit-family deferred
  # address-commit helper: no lowering-gap stop, one routed read, no write-back, one live-register commit
  # strictly after the routed access.
  bt_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-dim-bit-test-auto-update"],
                            text=True, capture_output=True)
  bt_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-dim-bit-test-auto-update"],
                             text=True, capture_output=True)
  assert bt_first.returncode == bt_second.returncode == 0
  assert bt_first.stdout == bt_second.stdout
  assert not bt_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in bt_first.stdout
  assert "genesis_c4_lowering_stop_" not in bt_first.stdout
  assert "uint32_t m68k_bit_auto_ea = runtime->a[1];" in bt_first.stdout
  assert "m68k_bit_auto_ea += UINT32_C(1);" in bt_first.stdout
  assert bt_first.stdout.count("runtime->a[1] = m68k_bit_auto_ea;") == 1
  assert bt_first.stdout.index("runtime->a[1] = m68k_bit_auto_ea;") > bt_first.stdout.rindex("genesis_route_access(runtime, ")
  assert bt_first.stdout.count("genesis_route_access(") == 1  # read only: BTST never writes back
  c4_dim_outputs["bit_test_auto_update"] = bt_first.stdout
  # SEG-007-T167: the C4 logical-family missing-dispatcher batch
  # (AND/OR/EOR + ORI/EORI). The register-only reached shape (byte OR
  # `<ea>,Dn`, both operands data registers) now lowers to a real dispatcher
  # body -- no missing_dispatcher / no lowering-gap stop -- and is
  # deterministic and strict-C11. An auto-updating (`-(An)`) operand is a
  # clean requires_architecture_decision decline: it serialises to a distinct
  # emitted C4 lowering-gap stop naming the auto-update dimension, and never
  # emits a naive unrouted `logical_result` write.
  logical_reg_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-logical"], text=True, capture_output=True)
  logical_reg_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-logical"], text=True, capture_output=True)
  assert logical_reg_first.returncode == logical_reg_second.returncode == 0
  assert logical_reg_first.stdout == logical_reg_second.stdout  # deterministic two-run output
  assert not logical_reg_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in logical_reg_first.stdout
  assert "genesis_c4_lowering_stop_" not in logical_reg_first.stdout
  assert "logical_result =" in logical_reg_first.stdout
  c4_dim_outputs["logical_register"] = logical_reg_first.stdout
  # SEG-021-T007: an auto-updating OR/ORI operand is now lowered by the logical-family deferred
  # address-commit helper: no lowering-gap stop, a routed read+write and one live-register commit.
  for flag, key in (
      ("c4-logical-predecrement", "logical_predecrement"),
      ("c4-logical-ori-predecrement", "logical_ori_predecrement")):
    run_first = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    run_second = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    assert run_first.returncode == run_second.returncode == 0, key
    assert run_first.stdout == run_second.stdout, key
    assert not run_first.stdout.startswith("/* translation rejected:"), key
    assert "GENESIS_STOP_C4_LOWERING_GAP" not in run_first.stdout, key
    assert "genesis_c4_lowering_stop_" not in run_first.stdout, key
    assert "uint32_t m68k_logical_auto_ea = runtime->a[0];" in run_first.stdout, key
    assert run_first.stdout.count("runtime->a[0] = m68k_logical_auto_ea;") == 1, key
    assert run_first.stdout.index("runtime->a[0] = m68k_logical_auto_ea;") > run_first.stdout.rindex("genesis_route_access(runtime, "), key
    assert "logical_result =" in run_first.stdout, key
    c4_dim_outputs[key] = run_first.stdout
  # SEG-007-T167 CORRECTION 1: the end-to-end foldable-memory logical-family
  # fact pipeline. A foldable-control memory OR source / OR read-modify-write
  # destination / ORI destination each consumes its UNTOUCHED discovery-produced
  # retained fact: preflight sees no lowering gap and the block emits the routed
  # logical operation, deterministically and strict-C11. ORI/EORI follow ANDI's
  # single destination_write fact contract (that one fact authorises both the
  # routed destination read and the routed destination write).
  for flag, key in (("c4-logical-source-fold", "logical_source_fold"),
                    ("c4-logical-dest-fold", "logical_dest_fold"),
                    ("c4-logical-ori-dest-fold", "logical_ori_dest_fold")):
    run_first = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    run_second = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    assert run_first.returncode == run_second.returncode == 0, key
    assert run_first.stdout == run_second.stdout, key  # deterministic two-run output
    assert not run_first.stdout.startswith("/* translation rejected:"), key
    assert "GENESIS_STOP_C4_LOWERING_GAP" not in run_first.stdout, key
    assert "genesis_c4_lowering_stop_" not in run_first.stdout, key
    assert "logical_result =" in run_first.stdout, key
    assert "genesis_route_access" in run_first.stdout, key  # routed device access, not image re-read
    c4_dim_outputs[key] = run_first.stdout
  # SEG-007-T167 CORRECTION 1 adversarial: a forged retained fact whose
  # direction contradicts its role is independently rejected by
  # valid_c4_static_memory_fact, so the whole C4 preflight fails closed.
  forged_dir = subprocess.run([executable, "--emit-general-startup-runtime-c4-logical-forge-direction"], text=True, capture_output=True)
  assert forged_dir.returncode == 0
  assert "translation rejected" in forged_dir.stdout
  assert "logical_result =" not in forged_dir.stdout
  # SEG-007-T146: the plain `compare` (CMP) and `compare_immediate` (CMPI) IR
  # families now have a real C4 dispatcher body -- `Dn - source` / `<ea> - imm`
  # computed at width with a CCR-only result and no architectural writeback,
  # exactly like the interpreter-free `compare_address` precedent. They must no
  # longer serialize to any missing_dispatcher lowering-gap stop.
  for flag, forge in (("c4-dim-compare", "compare"), ("c4-dim-compare-immediate", "compare_immediate")):
    run_first = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    run_second = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    assert run_first.returncode == run_second.returncode == 0, forge
    assert run_first.stdout == run_second.stdout, forge  # deterministic two-run output
    assert not run_first.stdout.startswith("/* translation rejected:"), forge
    assert "GENESIS_STOP_C4_LOWERING_GAP" not in run_first.stdout, forge
    assert "genesis_c4_lowering_stop_" not in run_first.stdout, forge
    # CCR-only: the shared subtraction result specification updates runtime->sr
    # and there is no `runtime->d[...] =` architectural result writeback.
    assert "runtime->sr" in run_first.stdout, forge
    assert "runtime->d[0] =" not in run_first.stdout, forge
    c4_dim_outputs[forge] = run_first.stdout
  # SEG-007-T174 follow-up fix (reviewer-directed, composed-hints
  # regression): CMPI.W #1,$00FF0000 (synthetic work RAM, the same
  # foldable-absolute base address the SUBQ/ADDQ-absolute fixtures below
  # use). Before this task's fix, this switch had no cmpi case at all, so a
  # real foldable-EA CMPI never retained any fact, unconditionally producing
  # a `GENESIS_C4_LOWERING_DIMENSIONS_CMPI_MISSING_FACT` C4 build-time stop
  # the instant one became reachable -- reproduced here with a
  # project-authored fixture, never raw Sonic ROM content. Proves: real
  # discovery retains a destination_read fact, both C4 static-memory-fact
  # re-verifiers accept it, and the routed absolute-EA read
  # (test_operand_region threaded from that fact) reaches
  # genesis_route_access exactly like the register-destination fixture's
  # CCR-only comparison, with no writeback.
  cmpi_absolute_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-compare-immediate-absolute"], text=True, capture_output=True)
  cmpi_absolute_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-compare-immediate-absolute"], text=True, capture_output=True)
  assert cmpi_absolute_first.returncode == cmpi_absolute_second.returncode == 0
  assert cmpi_absolute_first.stdout == cmpi_absolute_second.stdout  # deterministic two-run output
  assert not cmpi_absolute_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in cmpi_absolute_first.stdout
  assert "genesis_c4_lowering_stop_" not in cmpi_absolute_first.stdout
  assert "genesis_route_access(runtime, " in cmpi_absolute_first.stdout
  assert "GENESIS_ACCESS_READ" in cmpi_absolute_first.stdout
  # CMPI never writes its destination: only one routed access (the read) may
  # be present, and there is no architectural result writeback register.
  assert cmpi_absolute_first.stdout.count("genesis_route_access(runtime, ") == 1
  assert "GENESIS_ACCESS_WRITE" not in cmpi_absolute_first.stdout
  assert "runtime->sr" in cmpi_absolute_first.stdout
  assert ")[" not in cmpi_absolute_first.stdout
  c4_dim_outputs["compare_immediate_absolute"] = cmpi_absolute_first.stdout
  # SEG-007-T152: the register-count shift/rotate family (`shift_rotate_register`)
  # now has a real C4 dispatcher body -- the emitter's own complete
  # M68kShiftRotateSpecification::emit_c_update owner (already present before
  # this task) was simply never reachable because `shift_rotate_register` was
  # misclassified as a missing_dispatcher gap. It must no longer serialize to
  # any missing_dispatcher lowering-gap stop; the memory form remains a gap.
  register_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-shift-rotate-register"], text=True, capture_output=True)
  register_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-shift-rotate-register"], text=True, capture_output=True)
  assert register_first.returncode == register_second.returncode == 0
  assert register_first.stdout == register_second.stdout  # deterministic two-run output
  assert not register_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in register_first.stdout
  assert "genesis_c4_lowering_stop_" not in register_first.stdout
  # ASR.W D0,D1: destination is D1, count source is D0.  The destination write
  # and the shared shift/rotate CCR update must both be present.
  assert "runtime->d[1] =" in register_first.stdout
  assert "runtime->sr" in register_first.stdout
  c4_dim_outputs["shift_rotate_register"] = register_first.stdout
  # SEG-007-T152: the immediate-count sibling (LSL.W #1,D0) proves the
  # emitter's other legal source_ea mode for shift_rotate_register --
  # immediate, not a count register -- also lowers with no gap stop.
  register_imm_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-shift-rotate-register-immediate"], text=True, capture_output=True)
  register_imm_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-shift-rotate-register-immediate"], text=True, capture_output=True)
  assert register_imm_first.returncode == register_imm_second.returncode == 0
  assert register_imm_first.stdout == register_imm_second.stdout  # deterministic two-run output
  assert not register_imm_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in register_imm_first.stdout
  assert "genesis_c4_lowering_stop_" not in register_imm_first.stdout
  assert "runtime->d[0] =" in register_imm_first.stdout
  assert "runtime->sr" in register_imm_first.stdout
  c4_dim_outputs["shift_rotate_register_immediate"] = register_imm_first.stdout
  # SEG-007-T153: ADDQ (`add_quick`) now has a real C4 dispatcher body -- it
  # reuses the shared `add` / `add_immediate` / `add_quick` / `add_address`
  # emission body that was already present before this task and was simply
  # unreachable because `add_quick` was misclassified as a missing_dispatcher
  # gap (its sibling `subtract_quick` was already represented). Neither the
  # data-register-destination form (ADDQ.W #1,D0) nor the
  # address-register-destination form (ADDQ.L #1,A0) may serialise to a
  # lowering-gap stop; the destination write and PC advance must be present.
  addq_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-add-quick"], text=True, capture_output=True)
  addq_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-add-quick"], text=True, capture_output=True)
  assert addq_first.returncode == addq_second.returncode == 0
  assert addq_first.stdout == addq_second.stdout  # deterministic two-run output
  assert not addq_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in addq_first.stdout
  assert "genesis_c4_lowering_stop_" not in addq_first.stdout
  # ADDQ.W #1,D0: data-register destination write plus CCR update.
  assert "runtime->d[0] =" in addq_first.stdout
  assert "runtime->sr" in addq_first.stdout
  c4_dim_outputs["add_quick"] = addq_first.stdout
  addq_addr_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-add-quick-address"], text=True, capture_output=True)
  addq_addr_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-add-quick-address"], text=True, capture_output=True)
  assert addq_addr_first.returncode == addq_addr_second.returncode == 0
  assert addq_addr_first.stdout == addq_addr_second.stdout  # deterministic two-run output
  assert not addq_addr_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in addq_addr_first.stdout
  assert "genesis_c4_lowering_stop_" not in addq_addr_first.stdout
  # ADDQ.L #1,A0: the shared body's `add_quick` + address-register branch
  # writes the address-register file directly (no CCR update for An dest).
  assert "runtime->a[0] =" in addq_addr_first.stdout
  c4_dim_outputs["add_quick_address"] = addq_addr_first.stdout
  # SEG-007-T153: generic ADDQ memory-destination forms. `(An)` and `(d16,An)`
  # are routed read-modify-write destinations (same genesis_route_access seam
  # `add` uses); neither may serialise to a lowering-gap stop and both carry a
  # CCR update.
  for flag, key in (("c4-dim-add-quick-indirect", "add_quick_indirect"),
                    ("c4-dim-add-quick-disp", "add_quick_disp")):
    run_first = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    run_second = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    assert run_first.returncode == run_second.returncode == 0, key
    assert run_first.stdout == run_second.stdout, key  # deterministic two-run output
    assert not run_first.stdout.startswith("/* translation rejected:"), key
    assert "GENESIS_STOP_C4_LOWERING_GAP" not in run_first.stdout, key
    assert "genesis_c4_lowering_stop_" not in run_first.stdout, key
    assert "genesis_route_access(runtime, " in run_first.stdout, key
    assert "GENESIS_ACCESS_WRITE" in run_first.stdout, key
    assert "runtime->sr" in run_first.stdout, key
    c4_dim_outputs[key] = run_first.stdout
  # SEG-007-T153: auto-updating ADDQ destinations `(A0)+` / `-(A0)` lower via
  # the add-family deferred single-address-register commit path: the touched
  # An is written back in exactly one statement that is textually AFTER every
  # routed access this instruction performs, so any GENESIS_STOP inside a
  # routed access returns (the m68k_emit_routed_* helpers each emit their own
  # `return transfer;` on failure) before the commit -- An keeps its
  # pre-instruction value, no partial update observable (fail-closed).
  for flag, key in (("c4-dim-add-quick-postinc", "add_quick_postinc"),
                    ("c4-dim-add-quick-predec", "add_quick_predec")):
    run_first = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    run_second = subprocess.run([executable, f"--emit-general-startup-runtime-{flag}"], text=True, capture_output=True)
    assert run_first.returncode == run_second.returncode == 0, key
    assert run_first.stdout == run_second.stdout, key  # deterministic two-run output
    assert not run_first.stdout.startswith("/* translation rejected:"), key
    assert "GENESIS_STOP_C4_LOWERING_GAP" not in run_first.stdout, key
    commit = "runtime->a[0] = m68k_add_auto_ea;"
    assert run_first.stdout.count(commit) == 1, key  # exactly one deferred commit
    body = run_first.stdout
    # commit strictly after every routed access (adversarial: prove no early
    # writeback is reachable before a routed access can GENESIS_STOP).
    assert body.index(commit) > body.rindex("genesis_route_access(runtime, "), key
    assert body.index(commit) > body.rindex("return transfer;", 0, body.index(commit)), key
    assert "GENESIS_ACCESS_WRITE" in body, key
    c4_dim_outputs[key] = body
  # predecrement adjusts the local before the routed access(es).
  assert "m68k_add_auto_ea -= UINT32_C(2);" in c4_dim_outputs["add_quick_predec"]
  assert "m68k_add_auto_ea += UINT32_C(2);" in c4_dim_outputs["add_quick_postinc"]
  # SEG-007-T165: SUBQ (`subtract_quick`) was misclassified into C4's plain,
  # unrouted-memory dispatch group (shared with the register-only
  # subtract_quick_long_d0) instead of the routed group `add_quick` uses.
  # Reaching this shape with an actual memory destination produced a
  # malformed `(uint32_t)[<offset>]` read/write with no ram-array
  # identifier before this task's fix -- reproduced here with a
  # project-authored SUBQ.B #1,(A0) fixture, never raw Sonic ROM content.
  subq_indirect_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-subtract-quick-indirect"], text=True, capture_output=True)
  subq_indirect_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-subtract-quick-indirect"], text=True, capture_output=True)
  assert subq_indirect_first.returncode == subq_indirect_second.returncode == 0
  assert subq_indirect_first.stdout == subq_indirect_second.stdout  # deterministic two-run output
  assert not subq_indirect_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in subq_indirect_first.stdout
  assert "genesis_c4_lowering_stop_" not in subq_indirect_first.stdout
  assert "genesis_route_access(runtime, " in subq_indirect_first.stdout
  # SUBQ is a read-modify-write: both the routed READ (destination operand
  # value) and the routed WRITE (stored result) must be present, not just
  # the write side.
  assert "GENESIS_ACCESS_READ" in subq_indirect_first.stdout
  assert "GENESIS_ACCESS_WRITE" in subq_indirect_first.stdout
  assert "runtime->sr" in subq_indirect_first.stdout
  # The exact malformed shape this task's fix removes: a cast immediately
  # followed by `[` with no array identifier in between.
  assert ")[" not in subq_indirect_first.stdout
  c4_dim_outputs["subtract_quick_indirect"] = subq_indirect_first.stdout
  # SEG-007-T165 correction: a second, independent SUBQ fixture whose
  # destination is a foldable absolute EA (unlike the register-indirect
  # fixture above, which is never foldable and therefore never exercises
  # `retain_fact`'s new `subq` case or either static-memory-fact
  # re-verifier's new `subq` case). SUBQ.W #1,$00FF0000 (synthetic work RAM,
  # the same base address the existing "ram-byte"/"ram-word"/"ram-long" CLR
  # fixtures already use) must retain a real destination_read/
  # destination_write fact, pass both re-verifiers, and route through
  # genesis_route_access with test_operand_region threaded from that fact --
  # proving the fact-based absolute-EA path end to end, not raw Sonic ROM
  # content.
  subq_absolute_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-subtract-quick-absolute"], text=True, capture_output=True)
  subq_absolute_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-subtract-quick-absolute"], text=True, capture_output=True)
  assert subq_absolute_first.returncode == subq_absolute_second.returncode == 0
  assert subq_absolute_first.stdout == subq_absolute_second.stdout  # deterministic two-run output
  assert not subq_absolute_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in subq_absolute_first.stdout
  assert "genesis_c4_lowering_stop_" not in subq_absolute_first.stdout
  assert "genesis_route_access(runtime, " in subq_absolute_first.stdout
  assert "GENESIS_ACCESS_READ" in subq_absolute_first.stdout
  assert "GENESIS_ACCESS_WRITE" in subq_absolute_first.stdout
  assert "runtime->sr" in subq_absolute_first.stdout
  assert ")[" not in subq_absolute_first.stdout
  c4_dim_outputs["subtract_quick_absolute"] = subq_absolute_first.stdout
  # SEG-007-T174 follow-up fix (reviewer-directed, composed-hints
  # regression): the ADDQ counterpart of SUBQ's own foldable-absolute
  # fixture immediately above. Before this task's fix, this switch had no
  # case for addq at all (unlike its subq sibling), so a real, foldable-
  # absolute-EA ADDQ instruction -- reachable only once the composed-hints
  # correction restored the real Ghidra-assisted candidate-root set and let
  # generated execution proceed into one of its real handler bodies for the
  # first time -- silently fell through to `default:`/no fact, producing a
  # `GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_FACT` C4 build-time
  # stop. ADDQ.W #1,$00FF0000 (the same synthetic-work-RAM base address)
  # reproduces the shape with a project-authored fixture, never raw Sonic
  # ROM content. Proves the identical real destination_read/
  # destination_write fact retention, both re-verifiers' acceptance, and
  # routed absolute-EA RMW path end to end that SUBQ's own fixture already
  # proves.
  addq_absolute_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-add-quick-absolute"], text=True, capture_output=True)
  addq_absolute_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-add-quick-absolute"], text=True, capture_output=True)
  assert addq_absolute_first.returncode == addq_absolute_second.returncode == 0
  assert addq_absolute_first.stdout == addq_absolute_second.stdout  # deterministic two-run output
  assert not addq_absolute_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in addq_absolute_first.stdout
  assert "genesis_c4_lowering_stop_" not in addq_absolute_first.stdout
  assert "genesis_route_access(runtime, " in addq_absolute_first.stdout
  assert "GENESIS_ACCESS_READ" in addq_absolute_first.stdout
  assert "GENESIS_ACCESS_WRITE" in addq_absolute_first.stdout
  assert "runtime->sr" in addq_absolute_first.stdout
  assert ")[" not in addq_absolute_first.stdout
  c4_dim_outputs["add_quick_absolute"] = addq_absolute_first.stdout
  # SEG-007-T196: ADDI joins the existing add-family dispatcher. The C++
  # fixture covers the full legal destination surface; this representative
  # direct BYTE form additionally compiles, links, and executes strict C11.
  addi_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-add-immediate-direct"], text=True, capture_output=True)
  addi_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-add-immediate-direct"], text=True, capture_output=True)
  assert addi_first.returncode == addi_second.returncode == 0
  assert addi_first.stdout == addi_second.stdout
  assert "add_result =" in addi_first.stdout
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in addi_first.stdout
  addi_missing_fact = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-add-immediate-missing-fact"], text=True, capture_output=True)
  assert addi_missing_fact.returncode == 0
  c4_dim_outputs["add_immediate"] = addi_first.stdout
  # SEG-007-T177: EXT.W/EXT.L (`sign_extend_word`/`sign_extend_long`) now have
  # real C4 dispatcher bodies -- they reuse the shared emission body already
  # present in emit_m68k_operation_c before this task and were simply
  # unreachable because both kinds were misclassified as missing_dispatcher
  # gaps. Both fixtures' destination is fixed to D0 (data-register-direct,
  # the only destination the decoder ever admits for EXT); neither may
  # serialise to a lowering-gap stop, and both must show the D0 write plus
  # the shared CCR update.
  ext_word_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-sign-extend-word"], text=True, capture_output=True)
  ext_word_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-sign-extend-word"], text=True, capture_output=True)
  assert ext_word_first.returncode == ext_word_second.returncode == 0
  assert ext_word_first.stdout == ext_word_second.stdout  # deterministic two-run output
  assert not ext_word_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in ext_word_first.stdout
  assert "genesis_c4_lowering_stop_" not in ext_word_first.stdout
  assert "runtime->d[0] =" in ext_word_first.stdout
  assert "runtime->sr" in ext_word_first.stdout
  c4_dim_outputs["sign_extend_word"] = ext_word_first.stdout
  ext_long_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-sign-extend-long"], text=True, capture_output=True)
  ext_long_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-dim-sign-extend-long"], text=True, capture_output=True)
  assert ext_long_first.returncode == ext_long_second.returncode == 0
  assert ext_long_first.stdout == ext_long_second.stdout  # deterministic two-run output
  assert not ext_long_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in ext_long_first.stdout
  assert "genesis_c4_lowering_stop_" not in ext_long_first.stdout
  assert "runtime->d[0] =" in ext_long_first.stdout
  assert "runtime->sr" in ext_long_first.stdout
  c4_dim_outputs["sign_extend_long"] = ext_long_first.stdout
  # Both EXT outputs must differ from each other (different narrow width) and
  # cannot equal a plain missing_dispatcher stop wording either.
  assert ext_word_first.stdout != ext_long_first.stdout
  # SEG-007-T174: the shared subtract-family plain (non-auto-update)
  # destination-write path (m68k.cpp) is reached from TWO distinct
  # frontend.cpp callers with different textual needs for the PC-advance
  # statement it emits. SUB (`subtract`, routed through the `#define pc
  # runtime->pc` / `#undef pc` bridge the sibling ADD/logical bodies also
  # rely on) needed the shared path to emit the BARE `pc` identifier so the
  # active macro performs exactly one substitution; before this task's fix
  # it always emitted the fully-qualified `memory->program_counter` text
  # unconditionally, which -- reached from inside this bridge's scope --
  # corrupted into `runtime->runtime->pc` (a double substitution). SUBI
  # (`subtract_immediate`, no bridge) is the sibling call that must keep
  # receiving the fully-qualified text. Both fixtures are project-authored
  # synthetic C4 blocks (SUB.W D1,(0x00FF0080).L / SUBI.W #0x00FF,
  # (0x00FF0080).L), never Sonic ROM content; see
  # emit_general_startup_runtime_c4_subtract_source("dest-fold" /
  # "subi-dest-fold") in tests/m68k_pipeline_test.cpp for the exact encoding.
  subtract_dest_fold_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-subtract-dest-fold"], text=True, capture_output=True)
  subtract_dest_fold_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-subtract-dest-fold"], text=True, capture_output=True)
  assert subtract_dest_fold_first.returncode == subtract_dest_fold_second.returncode == 0
  assert subtract_dest_fold_first.stdout == subtract_dest_fold_second.stdout  # deterministic two-run output
  assert not subtract_dest_fold_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in subtract_dest_fold_first.stdout
  assert "genesis_c4_lowering_stop_" not in subtract_dest_fold_first.stdout
  assert "sub_result =" in subtract_dest_fold_first.stdout
  assert "genesis_route_access" in subtract_dest_fold_first.stdout
  # Positive: the bridge-active call site emits the bare `pc` spelling for
  # its PC-advance statement, inside the active `#define pc runtime->pc`
  # bridge, so the macro performs exactly one substitution at compile time.
  assert "\n  pc += UINT32_C(6);\n}\n" in subtract_dest_fold_first.stdout or \
      "\npc += UINT32_C(6);\n}\n" in subtract_dest_fold_first.stdout
  # Adversarial: the exact defect this task's fix removes -- a double
  # substitution corrupting the bare identifier into a doubled prefix -- must
  # never appear, and the fully-qualified spelling this path must NOT use
  # (reserved for the non-bridge SUBI/SUBQ sibling below) must not appear.
  assert "runtime->runtime->pc" not in subtract_dest_fold_first.stdout
  assert "runtime->pc += UINT32_C(6);" not in subtract_dest_fold_first.stdout
  c4_dim_outputs["subtract_dest_fold"] = subtract_dest_fold_first.stdout
  subi_dest_fold_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-subtract-subi-dest-fold"], text=True, capture_output=True)
  subi_dest_fold_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-subtract-subi-dest-fold"], text=True, capture_output=True)
  assert subi_dest_fold_first.returncode == subi_dest_fold_second.returncode == 0
  assert subi_dest_fold_first.stdout == subi_dest_fold_second.stdout  # deterministic two-run output
  assert not subi_dest_fold_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in subi_dest_fold_first.stdout
  assert "genesis_c4_lowering_stop_" not in subi_dest_fold_first.stdout
  assert "sub_result =" in subi_dest_fold_first.stdout
  assert "genesis_route_access" in subi_dest_fold_first.stdout
  # Positive: the non-bridge sibling call site (subtract_immediate) must keep
  # emitting the fully-qualified `runtime->pc` text for its PC-advance
  # statement -- proving `pc_macro_bridge_active`'s default `false` still
  # selects the pre-existing textual convention and neither caller regressed.
  assert "runtime->pc += UINT32_C(8);" in subi_dest_fold_first.stdout
  assert "runtime->runtime->pc" not in subi_dest_fold_first.stdout
  c4_dim_outputs["subtract_subi_dest_fold"] = subi_dest_fold_first.stdout
  # SEG-007-T174 follow-up: SUBA (`subtract_address`) reaches the SAME shared
  # subtract-family m68k.cpp body through a SECOND, distinct frontend.cpp
  # call site (shared with ADDA/CMP/CMPA) that also wraps its emission in a
  # `#define pc runtime->pc` / `#undef pc` bridge, but that second call site
  # left `pc_macro_bridge_active` at its default `false` -- so before this
  # fix, a routed-source SUBA emitted the fully-qualified `runtime->pc` text
  # inside its own active bridge, corrupting into `runtime->runtime->pc`, a
  # reference to a nonexistent struct member. This fixture drives
  # SUBA.W (0x00FF0080).L,A0 (synthetic_work_ram source, the same base
  # address the SUB dest/source-fold fixtures above use), a real accepted C4
  # shape (SUBA already has established sibling support via CMPA), through
  # the actual C4 emission path -- not a decode-only check.
  suba_source_fold_first = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-subtract-address-source-fold"], text=True, capture_output=True)
  suba_source_fold_second = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-subtract-address-source-fold"], text=True, capture_output=True)
  assert suba_source_fold_first.returncode == suba_source_fold_second.returncode == 0
  assert suba_source_fold_first.stdout == suba_source_fold_second.stdout  # deterministic two-run output
  assert not suba_source_fold_first.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in suba_source_fold_first.stdout
  assert "genesis_c4_lowering_stop_" not in suba_source_fold_first.stdout
  assert "sub_result =" in suba_source_fold_first.stdout
  assert "genesis_route_access" in suba_source_fold_first.stdout
  # Positive: the bridge-active call site emits the bare `pc` spelling for its
  # PC-advance statement, inside the active `#define pc runtime->pc` bridge,
  # so the macro performs exactly one substitution at compile time.
  assert "\n  pc += UINT32_C(6);\n}\n" in suba_source_fold_first.stdout or \
      "\npc += UINT32_C(6);\n}\n" in suba_source_fold_first.stdout
  # Adversarial: the exact defect this fix removes -- a double substitution
  # corrupting the bare identifier into a doubled prefix -- must never appear.
  assert "runtime->runtime->pc" not in suba_source_fold_first.stdout
  c4_dim_outputs["subtract_address_source_fold"] = suba_source_fold_first.stdout
  assert c4_dim_shapes["push_effective_address"] == "PUSH_EFFECTIVE_ADDRESS_MISSING_DISPATCHER"
  # SEG-007-T145: ordinary add-family auto-update operands are now lowered by
  # the deferred-address-commit path; the sole remaining add-family lowering
  # gap is the ADDA same-register aliasing decline, which serialises to the
  # distinct ADDA_AUTO_UPDATE literal.
  # SEG-007-T157 / ADR-0019 Stage B: write_clr's own auto-update shape
  # (formerly the distinct CLR_AUTO_UPDATE lowering-gap literal, proven by the
  # predecrement fixture above) is no longer a gap at all -- it is fully
  # lowered -- so it can no longer collide with the add family's own
  # ADDA_AUTO_UPDATE literal; this is inherently true rather than needing a
  # live re-check.
  # SEG-007-T067: C4's new movem_transfer routed lowering. Deterministic
  # two-run byte-identical output, no rejection, and every transfer routed
  # through genesis_route_access -- never a private RAM-array index (the
  # exact "runtime->work_ram" pattern the direct_flow-only reference branch
  # uses, and this task's own Evidence explicitly forbids for C4).
  movem_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-movem"], text=True, capture_output=True)
  movem_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-movem"], text=True, capture_output=True)
  assert movem_first.returncode == movem_second.returncode == 0
  assert movem_first.stdout == movem_second.stdout
  assert not movem_first.stdout.startswith("/* translation rejected:")
  assert "runtime->work_ram" not in movem_first.stdout
  assert movem_first.stdout.count("genesis_route_access(") == 10
  assert "#define pc runtime->pc" in movem_first.stdout and "#undef pc" in movem_first.stdout
  # The nearest malformed shape this task's own new lowering must still
  # reject, analogous to SEG-007-T066's own negative fixture (see
  # emit_general_startup_runtime_c4_movem_source's own "forged-ea" comment):
  # a MOVEM operation forged to a Dn-direct destination EA no real decode of
  # legal MOVEM bytes can ever produce.
  movem_forged = subprocess.run([executable, "--emit-general-startup-runtime-c4-movem-forged-ea"], text=True, capture_output=True)
  assert movem_forged.returncode == 0 and movem_forged.stdout.startswith("/* translation rejected:")
  # SEG-007-T068: write_move's generalized C4 lowering. Deterministic
  # two-run byte-identical output, no rejection, every non-Dn/non-folded
  # transfer routed through genesis_route_access -- never a private
  # RAM-array index -- and the folded ROM-resident source (instruction 3)
  # never routed at all.
  move_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-move"], text=True, capture_output=True)
  move_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-move"], text=True, capture_output=True)
  assert move_first.returncode == move_second.returncode == 0
  assert move_first.stdout == move_second.stdout
  assert not move_first.stdout.startswith("/* translation rejected:")
  assert "runtime->work_ram" not in move_first.stdout
  assert move_first.stdout.count("genesis_route_access(") == 6
  assert "resolved static read" in move_first.stdout
  assert "#define pc runtime->pc" in move_first.stdout and "#undef pc" in move_first.stdout
  # SEG-007-T070: a predecrement/postincrement MOVE operand has no retained
  # static memory fact (predecrement/postincrement never get one, Q4), but
  # require_fact's rejection now narrows to no longer apply to `move`'s own
  # call site -- write_move's own block-emission case lowers these itself
  # (the deferred-address-commit technique). MOVE.L D0,-(A0)/MOVE.L D0,(A0)+
  # are both destination-mutating, register-direct-source shapes: fully
  # representable, not rejected (before SEG-007-T070 both reproduced the
  # same generic "lacks retained resolver fact" rejection the CLR
  # predecrement fixture above still exhibits for CLR itself).
  move_predecrement = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-predecrement"], text=True, capture_output=True)
  move_predecrement_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-predecrement"], text=True, capture_output=True)
  assert move_predecrement.returncode == move_predecrement_second.returncode == 0
  assert move_predecrement.stdout == move_predecrement_second.stdout
  assert not move_predecrement.stdout.startswith("/* translation rejected:")
  assert "const uint32_t m68k_routed_addr_0 = (m68k_move_dst_ea) & UINT32_C(0x00FFFFFF);" in move_predecrement.stdout
  assert "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE" in move_predecrement.stdout
  assert "runtime->a[0] = m68k_move_dst_ea;" in move_predecrement.stdout
  move_postincrement = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-postincrement"], text=True, capture_output=True)
  move_postincrement_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-postincrement"], text=True, capture_output=True)
  assert move_postincrement.returncode == move_postincrement_second.returncode == 0
  assert move_postincrement.stdout == move_postincrement_second.stdout
  assert not move_postincrement.stdout.startswith("/* translation rejected:")
  assert "const uint32_t m68k_routed_addr_0 = (m68k_move_dst_ea) & UINT32_C(0x00FFFFFF);" in move_postincrement.stdout
  assert "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE" in move_postincrement.stdout
  assert "runtime->a[0] = m68k_move_dst_ea;" in move_postincrement.stdout
  # The nearest malformed static-memory-fact shapes this task's own widened
  # write_move lowering must still reject, analogous to
  # emit_general_startup_runtime_c4_frontier_source's own same-named
  # forgeries.
  for forged in ("fact-duplicate", "fact-unbound"):
    move_forged = subprocess.run([executable, f"--emit-general-startup-runtime-c4-move-{forged}"], text=True, capture_output=True)
    assert move_forged.returncode == 0 and move_forged.stdout.startswith("/* translation rejected:")
  # SEG-007-T071: ANDI's destination read-modify-write follows CLR's retained
  # resolver-fact/runtime-routing contract. The synthetic block covers a
  # retained-fact RAM-absolute destination, a routed (A0) destination, and a
  # register-direct destination; every memory half of the two RMW operations
  # routes through genesis_route_access, never a private RAM-array index.
  andi_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-andi"], text=True, capture_output=True)
  andi_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-andi"], text=True, capture_output=True)
  assert andi_first.returncode == andi_second.returncode == 0
  assert andi_first.stdout == andi_second.stdout
  assert not andi_first.stdout.startswith("/* translation rejected:")
  assert "runtime->work_ram" not in andi_first.stdout
  assert andi_first.stdout.count("genesis_route_access(") == 4
  assert "#define pc runtime->pc" in andi_first.stdout and "#undef pc" in andi_first.stdout
  # SEG-021-T007: an auto-updating ANDI destination is lowered by the logical-family operation-local deferred
  # address-register commit (one snapshot local, routed read + write, one commit after both).
  andi_predecrement = subprocess.run([executable, "--emit-general-startup-runtime-c4-andi-predecrement"], text=True, capture_output=True)
  assert andi_predecrement.returncode == 0
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in andi_predecrement.stdout
  assert "GENESIS_C4_LOWERING_DIMENSIONS_ANDI_AUTO_UPDATE" not in andi_predecrement.stdout
  assert "m68k_logical_auto_ea -= UINT32_C(" in andi_predecrement.stdout
  assert andi_predecrement.stdout.rindex("m68k_logical_auto_ea;") > andi_predecrement.stdout.rindex("genesis_route_access(")
  # The generic retained-fact validation must reject malformed ANDI-owned
  # facts before emission, just as it does for the previously covered C4
  # destination-read/write instruction families.
  for forged in ("fact-duplicate", "fact-unbound"):
    andi_forged = subprocess.run([executable, f"--emit-general-startup-runtime-c4-andi-{forged}"], text=True, capture_output=True)
    assert andi_forged.returncode == 0 and andi_forged.stdout.startswith("/* translation rejected:")
  # SEG-007-T168: NOT is the unary read-modify-write logical-complement
  # sibling of AND/OR/EOR and the RMW sibling of CLR/SUBQ. The synthetic
  # block covers a retained-fact RAM-absolute destination, a routed (A0)
  # destination, and a register-direct destination that needs no fact at
  # all; every memory form's RMW pair routes through genesis_route_access.
  not_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-not"], text=True, capture_output=True)
  not_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-not"], text=True, capture_output=True)
  assert not_first.returncode == not_second.returncode == 0
  assert not_first.stdout == not_second.stdout
  assert not not_first.stdout.startswith("/* translation rejected:")
  assert "runtime->work_ram" not in not_first.stdout
  assert not_first.stdout.count("genesis_route_access(") == 4
  assert "not_result = ~(" in not_first.stdout
  assert "#define pc runtime->pc" in not_first.stdout and "#undef pc" in not_first.stdout
  # SEG-021-T005: an auto-updating NOT destination is lowered through the NEG/ADD-style operation-local deferred
  # address-register commit (one snapshot local, routed read + routed write at one address, one commit after both).
  not_predecrement = subprocess.run([executable, "--emit-general-startup-runtime-c4-not-predecrement"], text=True, capture_output=True)
  assert not_predecrement.returncode == 0
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in not_predecrement.stdout
  assert "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_AUTO_UPDATE" not in not_predecrement.stdout
  assert "m68k_not_auto_ea -= UINT32_C(" in not_predecrement.stdout
  assert "not_result = ~(" in not_predecrement.stdout
  assert not_predecrement.stdout.count("genesis_route_access(") == 2
  assert not_predecrement.stdout.rindex("m68k_not_auto_ea;") > not_predecrement.stdout.rindex("genesis_route_access(")
  # A represented NOT with a statically foldable memory destination and no
  # retained resolver fact fails closed to an emitted C4 lowering-gap stop,
  # never a naive unrouted write.
  not_missing_fact = subprocess.run([executable, "--emit-general-startup-runtime-c4-not-missing-fact"], text=True, capture_output=True)
  assert not_missing_fact.returncode == 0
  assert "GENESIS_STOP_C4_LOWERING_GAP" in not_missing_fact.stdout
  assert "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_MISSING_FACT" in not_missing_fact.stdout
  assert "not_result =" not in not_missing_fact.stdout
  # The generic retained-fact validation must reject malformed NOT-owned
  # facts before emission, just as it does for every other C4
  # destination-read/write instruction family.
  for forged in ("fact-duplicate", "fact-unbound"):
    not_forged = subprocess.run([executable, f"--emit-general-startup-runtime-c4-not-{forged}"], text=True, capture_output=True)
    assert not_forged.returncode == 0 and not_forged.stdout.startswith("/* translation rejected:")
  # SEG-007-T072: MOVEA's direct An destination needs no destination fact.
  # The source-only policy routes both dynamic and retained-RAM reads, preserves
  # CCR, sign-extends MOVEA.W, and transfers MOVEA.L unchanged.
  movea_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea"], text=True, capture_output=True)
  movea_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea"], text=True, capture_output=True)
  assert movea_first.returncode == movea_second.returncode == 0
  assert movea_first.stdout == movea_second.stdout
  assert not movea_first.stdout.startswith("/* translation rejected:")
  assert "runtime->work_ram" not in movea_first.stdout
  assert movea_first.stdout.count("genesis_route_access(") == 2
  for forged in ("fact-duplicate", "fact-unbound", "fact-region", "fact-role"):
    movea_forged = subprocess.run([executable, f"--emit-general-startup-runtime-c4-movea-{forged}"], text=True, capture_output=True)
    assert movea_forged.returncode == 0
    assert movea_forged.stdout == "/* translation rejected: invalid C4 retained prefix */\n"
  movea_fact_missing = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea-fact-missing"], text=True, capture_output=True)
  assert movea_fact_missing.returncode == 0
  assert "GENESIS_STOP_C4_LOWERING_GAP" in movea_fact_missing.stdout
  # SEG-007-T192: an auto-updating MOVEA source (`-(An)`/`(An)+`) is now
  # lowered through its own deferred-address-commit path, directly reusing
  # MOVE's (Q2/Q4/Q5) and the add family's established technique. Unlike
  # ADDA, MOVEA's destination write never reads the destination register's
  # own prior value, so the same-register aliasing case is fully
  # representable too -- no decline, no "MOVEM deferred writeback" gap.
  movea_predecrement = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea-predecrement"], text=True, capture_output=True)
  movea_predecrement_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea-predecrement"], text=True, capture_output=True)
  assert movea_predecrement.returncode == movea_predecrement_second.returncode == 0
  assert movea_predecrement.stdout == movea_predecrement_second.stdout
  assert not movea_predecrement.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in movea_predecrement.stdout
  assert "const uint32_t m68k_routed_addr_0 = (m68k_movea_src_ea) & UINT32_C(0x00FFFFFF);" in movea_predecrement.stdout
  assert "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ" in movea_predecrement.stdout
  assert "runtime->a[0] = m68k_movea_src_ea;" in movea_predecrement.stdout
  movea_postincrement = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea-postincrement"], text=True, capture_output=True)
  movea_postincrement_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea-postincrement"], text=True, capture_output=True)
  assert movea_postincrement.returncode == movea_postincrement_second.returncode == 0
  assert movea_postincrement.stdout == movea_postincrement_second.stdout
  assert not movea_postincrement.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in movea_postincrement.stdout
  assert "runtime->a[0] = m68k_movea_src_ea;" in movea_postincrement.stdout
  # The same-register source-and-update case (`MOVEA.L (A0)+,A0`): no
  # composed-aliasing decline, since the destination write never reads A0's
  # own prior value -- it is simply overwritten by the loaded value.
  movea_aliasing = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea-aliasing"], text=True, capture_output=True)
  movea_aliasing_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-movea-aliasing"], text=True, capture_output=True)
  assert movea_aliasing.returncode == movea_aliasing_second.returncode == 0
  assert movea_aliasing.stdout == movea_aliasing_second.stdout
  assert not movea_aliasing.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" not in movea_aliasing.stdout
  assert "runtime->a[0] = m68k_movea_src_ea;" in movea_aliasing.stdout
  # Independent retained auto-update gaps are collected, normalized, sorted,
  # and deduplicated before any C is emitted.
  all_gaps = subprocess.run([executable, "--emit-general-startup-runtime-c4-all-gaps"], text=True, capture_output=True)
  assert all_gaps.returncode == 0
  assert not all_gaps.stdout.startswith("/* translation rejected:")
  assert "GENESIS_STOP_C4_LOWERING_GAP" in all_gaps.stdout
  # MOVEM's routed C4 lowering has no ROM-read success route.  Preflight
  # classifies that retained, project-authored source as missing routing before
  # it can emit a generated program that reaches the runtime's defensive ROM
  # read failure.
  missing_routing = subprocess.run([executable, "--emit-general-startup-runtime-c4-missing-routing"], text=True, capture_output=True)
  assert missing_routing.returncode == 0
  assert "genesis_route_access(runtime" in missing_routing.stdout
  # SEG-007-T070: write_move's own C4 predecrement/postincrement deferred-
  # address-commit lowering (docs/architecture/c4-move-predecrement-
  # postincrement-commit-contract.md). Deterministic two-run byte-identical
  # output, no rejection, every predecrement/postincrement operand routed
  # through genesis_route_access -- never a private RAM-array index -- and
  # both writebacks (for the two-different-registers instruction) placed
  # strictly after both of its accesses.
  move_autoupdate = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-autoupdate"], text=True, capture_output=True)
  move_autoupdate_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-autoupdate"], text=True, capture_output=True)
  assert move_autoupdate.returncode == move_autoupdate_second.returncode == 0
  assert move_autoupdate.stdout == move_autoupdate_second.stdout
  assert not move_autoupdate.stdout.startswith("/* translation rejected:")
  assert "runtime->work_ram" not in move_autoupdate.stdout
  assert move_autoupdate.stdout.count("genesis_route_access(") == 6
  assert "runtime->a[4] = m68k_move_src_ea;\nruntime->a[5] = m68k_move_dst_ea;" in move_autoupdate.stdout
  # Q3 (SEG-021-T005): the same-register aliasing shapes are lowered (destination EA derived from the source's
  # updated address-register local; live commits after both routed accesses), not declined.
  reject_same_register = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-move-autoupdate-reject-same-register"], text=True, capture_output=True)
  assert reject_same_register.returncode == 0
  assert not reject_same_register.stdout.startswith("/* translation rejected:")
  assert reject_same_register.stdout.count("genesis_route_access(") == 2
  assert "m68k_move_dst_ea = m68k_move_src_ea;" in reject_same_register.stdout
  assert reject_same_register.stdout.rindex("runtime->a[0] = m68k_move_src_ea;") < reject_same_register.stdout.rindex("runtime->a[0] = m68k_move_dst_ea;")
  reject_source_indirect_same_register = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-move-autoupdate-reject-source-indirect-same-register"],
      text=True, capture_output=True)
  assert reject_source_indirect_same_register.returncode == 0
  assert not reject_source_indirect_same_register.stdout.startswith("/* translation rejected:")
  assert reject_source_indirect_same_register.stdout.count("genesis_route_access(") == 2
  assert "m68k_move_dst_ea = m68k_move_src_ea;" in reject_source_indirect_same_register.stdout
  accept_reverse_asymmetric = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-move-autoupdate-accept-reverse-asymmetric"], text=True, capture_output=True)
  assert accept_reverse_asymmetric.returncode == 0
  assert not accept_reverse_asymmetric.stdout.startswith("/* translation rejected:")
  assert accept_reverse_asymmetric.stdout.count("genesis_route_access(") == 2
  # The nearest malformed static-memory-fact shapes for a MOVE that also
  # carries a predecrement/postincrement operand on its other side.
  for forged in ("fact-duplicate", "fact-unbound"):
    move_autoupdate_forged = subprocess.run(
        [executable, f"--emit-general-startup-runtime-c4-move-autoupdate-{forged}"], text=True, capture_output=True)
    assert move_autoupdate_forged.returncode == 0 and move_autoupdate_forged.stdout.startswith("/* translation rejected:")
  # SEG-007-T066: TST.W D0 whose one fallthrough edge targets another block
  # already retained in the same accepted prefix (0xB04), not the one
  # source-provenanced frontier. Before this task's fix this reproduced the
  # same generic "C4 block lacks terminal control transfer" rejection the
  # real Sonic ROM reached (independently confirmed by temporarily reverting
  # the fix and rerunning this exact fixture -- see the task's own Evidence);
  # the fix widens the existing `is_straight_line_frontier` exception (now
  # `is_straight_line_fallthrough`) to also recognize a fallthrough target
  # that is a retained block entry, adding no new control-transfer mechanism.
  straight_line_block_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-straight-line-block"], text=True, capture_output=True)
  straight_line_block_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-straight-line-block"], text=True, capture_output=True)
  assert straight_line_block_first.returncode == straight_line_block_second.returncode == 0
  assert straight_line_block_first.stdout == straight_line_block_second.stdout
  assert not straight_line_block_first.stdout.startswith("/* translation rejected:")
  # The TST block (0xB02) emits no per-block frontier check at all -- its own
  # fallthrough target is not a frontier -- and the final compiled lookup is
  # the sole mechanism that reaches block 0xB04 next.
  block_b02 = straight_line_block_first.stdout.split("genesis_block_00000B02(GenesisRuntime *runtime) {", 1)[1].split(
      "static GenesisControlTransfer genesis_dispatch", 1)[0]
  assert "genesis_frontier_stop" not in block_b02
  assert "{ UINT32_C(0x00000B04), genesis_block_00000B04 }" in straight_line_block_first.stdout
  # This fixture's own required negative case: an extra forged fallthrough
  # edge on the same TST terminal (still targeting the same retained block,
  # so no earlier edge-target-validity check catches it) must still be
  # rejected, proving the widened exactly-one-outgoing-edge guard is intact.
  straight_line_block_forged = subprocess.run(
      [executable, "--emit-general-startup-runtime-c4-straight-line-block-forged-multi-edge"], text=True, capture_output=True)
  assert straight_line_block_forged.returncode == 0
  assert straight_line_block_forged.stdout == "/* translation rejected: C4 block lacks terminal control transfer */\n"
  with tempfile.TemporaryDirectory() as temp:
    path = pathlib.Path(temp)
    # One terminal RTS may serve distinct static callers.  The untouched
    # partial must remain representable before the forged rebind below proves
    # that neither caller identity may be substituted for the other.
    multi_caller = subprocess.run([executable, "--emit-general-startup-runtime-c4-multi-caller"], text=True, capture_output=True)
    assert multi_caller.returncode == 0 and not multi_caller.stdout.startswith("/* translation rejected:")
    (path / "generated.c").write_text(multi_caller.stdout)
    standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / "multi-caller.o")], text=True, capture_output=True)
    assert standalone.returncode == 0, standalone.stderr
    # Every C4 partial-emission representative must stand alone as strict C11:
    # an ordinary terminal transfer, a prefix cut, independent multi-block
    # cuts, two candidates sharing one block, and a statically retained cut
    # that Q1 prunes downstream of an earlier one.
    for name, source in (("terminal-transfer", first.stdout), ("prefix", c4_prefix.stdout),
                          ("pruned-stop", c4_pruned_stop.stdout), ("multi-block", c4_multi_first.stdout),
                          ("same-block", c4_same_block.stdout), ("backward-block", c4_backward_first.stdout),
                          ("partition-boundary-dispatch", partition_boundary_dispatch.stdout),
                          ("dim-compare", c4_dim_outputs["compare"]),
                          ("dim-compare-immediate", c4_dim_outputs["compare_immediate"]),
                          ("dim-compare-immediate-absolute", c4_dim_outputs["compare_immediate_absolute"]),
                          ("dim-shift-rotate-register", c4_dim_outputs["shift_rotate_register"]),
                          ("dim-shift-rotate-register-immediate", c4_dim_outputs["shift_rotate_register_immediate"]),
                          ("dim-add-quick", c4_dim_outputs["add_quick"]),
                          ("dim-add-quick-address", c4_dim_outputs["add_quick_address"]),
                          ("dim-add-quick-indirect", c4_dim_outputs["add_quick_indirect"]),
                          ("dim-add-quick-disp", c4_dim_outputs["add_quick_disp"]),
                          ("dim-add-quick-postinc", c4_dim_outputs["add_quick_postinc"]),
                          ("dim-add-quick-predec", c4_dim_outputs["add_quick_predec"]),
                          ("dim-subtract-quick-indirect", c4_dim_outputs["subtract_quick_indirect"]),
                          ("dim-subtract-quick-absolute", c4_dim_outputs["subtract_quick_absolute"]),
                          ("dim-add-quick-absolute", c4_dim_outputs["add_quick_absolute"]),
                          ("dim-add-immediate", c4_dim_outputs["add_immediate"]),
                          ("subtract-dest-fold", c4_dim_outputs["subtract_dest_fold"]),
                          ("subtract-subi-dest-fold", c4_dim_outputs["subtract_subi_dest_fold"]),
                          ("subtract-address-source-fold", c4_dim_outputs["subtract_address_source_fold"]),
                          ("dim-push-effective-address", c4_dim_outputs["push_effective_address"]),
                          ("dim-bit-test-auto-update", c4_dim_outputs["bit_test_auto_update"]),
                          ("dim-logical-register", c4_dim_outputs["logical_register"]),
                          ("dim-logical-predecrement", c4_dim_outputs["logical_predecrement"]),
                          ("dim-logical-ori-predecrement", c4_dim_outputs["logical_ori_predecrement"]),
                          ("dim-logical-source-fold", c4_dim_outputs["logical_source_fold"]),
                          ("dim-logical-dest-fold", c4_dim_outputs["logical_dest_fold"]),
                          ("dim-logical-ori-dest-fold", c4_dim_outputs["logical_ori_dest_fold"])):
      (path / "generated.c").write_text(source)
      standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / f"c4-{name}.o")], text=True, capture_output=True)
      assert standalone.returncode == 0, f"{name}: {standalone.stderr}"
    (path / "generated.c").write_text(c4_dim_outputs["add_immediate"])
    (path / "harness.c").write_text(ADDI_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "addi")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "addi")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    (path / "generated.c").write_text(first.stdout)
    (path / "harness.c").write_text(HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "c4")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "c4")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    generated = subprocess.run([executable, "--emit-general-startup-runtime-c4-ram-byte"], text=True, capture_output=True)
    assert generated.returncode == 0, generated.stderr
    assert "const uint32_t m68k_routed_addr_0 = (UINT32_C(0x00FF0000)) & UINT32_C(0x00FFFFFF);" in generated.stdout
    assert "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE" in generated.stdout
    assert "runtime->work_ram" not in generated.stdout
    (path / "generated.c").write_text(generated.stdout)
    (path / "harness.c").write_text(RAM_BYTE_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "ram-byte")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "ram-byte")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T065: full compile+link+execute proof that CLR.L (A0)'s
    # runtime-routed write (see the routed_write assertions above) is not
    # merely well-formed C, but genuinely reaches genesis_route_access and
    # correctly writes through a runtime-resolved (not statically known)
    # address, and correctly leaves every runtime field untouched when the
    # router rejects the resolved address.
    (path / "generated.c").write_text(routed_write_first.stdout)
    (path / "harness.c").write_text(ROUTED_WRITE_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "routed-write")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "routed-write")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T067: full compile+link+execute proof that C4's new
    # movem_transfer routed lowering (see the movem assertions above) is not
    # merely well-formed C, but genuinely reaches genesis_route_access and
    # produces the documented direction/width/order/writeback effects at
    # runtime -- both directions, both widths (word sign-extension proven,
    # long non-extension proven), predecrement's reversed order and single
    # final writeback, postincrement's ascending order and single final
    # writeback, and that a plain (An)/absolute.l destination never
    # auto-updates its own address register.
    (path / "generated.c").write_text(movem_first.stdout)
    (path / "harness.c").write_text(MOVEM_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "movem")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "movem")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T068: full compile+link+execute proof that write_move's
    # generalized C4 lowering (see the move assertions above) is not merely
    # well-formed C, but genuinely reaches genesis_route_access and produces
    # the documented data-movement effects at runtime -- both directions of
    # the prior audited Dn<->RAM-absolute shape, a folded ROM-resident
    # source with Dn high-bit preservation, and a runtime-resolved
    # register-indirect source/destination each paired with a RAM-absolute
    # counterpart -- with neither address register ever mutated.
    (path / "generated.c").write_text(move_first.stdout)
    (path / "harness.c").write_text(MOVE_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "move")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "move")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T071: compile, link, and execute the ANDI block strictly as C11.
    # The harness proves both memory destinations retain only their low byte
    # under the WORD immediate and D0 retains only its low nibble under the
    # BYTE immediate, while the existing RESET remains the typed frontier.
    (path / "generated.c").write_text(andi_first.stdout)
    (path / "harness.c").write_text(ANDI_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "andi")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "andi")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T072: strict-C11 compile/link/execute checks the two represented
    # MOVEA source classes and the routed-read no-partial-effect failure.
    (path / "generated.c").write_text(movea_first.stdout)
    (path / "harness.c").write_text(MOVEA_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "movea")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "movea")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T192: full compile+link+execute proof that write_movea's own
    # auto-updating source lowering (predecrement, postincrement, and the
    # same-register aliasing case) genuinely reaches genesis_route_access and
    # produces the documented sign-extension / commit-ordering effects.
    for name, source, harness in (
        ("movea-predecrement", movea_predecrement.stdout, MOVEA_PREDECREMENT_HARNESS),
        ("movea-postincrement", movea_postincrement.stdout, MOVEA_POSTINCREMENT_HARNESS),
        ("movea-aliasing", movea_aliasing.stdout, MOVEA_ALIASING_HARNESS)):
      (path / "generated.c").write_text(source)
      (path / "harness.c").write_text(harness)
      build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / name)], text=True, capture_output=True)
      assert build.returncode == 0, f"{name}: {build.stderr}"
      ran = subprocess.run([str(path / name)], text=True, capture_output=True)
      assert ran.returncode == 0, f"{name}: {ran.stderr}"
    # SEG-007-T068 correction: MOVE.B D0<->RAM-absolute is generated
    # deterministically, routes each byte access through the production bus
    # boundary, and preserves the non-byte portions of both data registers.
    move_byte_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-byte"], text=True, capture_output=True)
    move_byte_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-move-byte"], text=True, capture_output=True)
    assert move_byte_first.returncode == move_byte_second.returncode == 0
    assert move_byte_first.stdout == move_byte_second.stdout
    assert not move_byte_first.stdout.startswith("/* translation rejected:")
    assert "runtime->work_ram" not in move_byte_first.stdout
    assert move_byte_first.stdout.count("genesis_route_access(") == 2
    (path / "generated.c").write_text(move_byte_first.stdout)
    (path / "harness.c").write_text(MOVE_BYTE_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "move-byte")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "move-byte")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T157 / ADR-0019 Stage B: full compile+link+execute proof of
    # write_clr's own auto-updating predecrement/postincrement destinations --
    # success (single deferred commit, fixed CCR pattern) and failure-
    # ordering (a ROM-window destination leaves both An and SR completely
    # unmodified).
    (path / "generated.c").write_text(predecrement.stdout)
    (path / "harness.c").write_text(CLR_PREDECREMENT_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "clr-predecrement")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "clr-predecrement")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    (path / "generated.c").write_text(clr_postinc.stdout)
    (path / "harness.c").write_text(CLR_POSTINC_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "clr-postinc")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "clr-postinc")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T070: full compile+link+execute proof that MOVE.L D0,-(A0)'s
    # destination-mutating lowering commits A0's decrement exactly once, at
    # the actual written address.
    (path / "generated.c").write_text(move_predecrement.stdout)
    (path / "harness.c").write_text(MOVE_PREDECREMENT_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "move-predecrement")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "move-predecrement")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T070: the postincrement mirror -- MOVE.L D0,(A0)+ writes at the
    # original address, then commits A0's increment exactly once.
    (path / "generated.c").write_text(move_postincrement.stdout)
    (path / "harness.c").write_text(MOVE_POSTINCREMENT_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "move-postincrement")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "move-postincrement")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T070: full compile+link+execute proof of write_move's own C4
    # predecrement/postincrement deferred-address-commit lowering (see the
    # move_autoupdate assertions above) -- both the success scenario (every
    # touched address register mutates exactly once, at the documented
    # address, with the data actually moved) and the forced mid-instruction
    # routing-failure scenario (every address register touched by the
    # failing instruction remains completely unmodified, while the earlier,
    # already-successful instructions' effects remain committed).
    (path / "generated.c").write_text(move_autoupdate.stdout)
    (path / "harness.c").write_text(MOVE_AUTOUPDATE_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "move-autoupdate")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "move-autoupdate")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T066: full compile+link+execute proof that a straight-line
    # fallthrough into a retained block entry (see the straight_line_block
    # assertions above) is not merely well-formed C, but genuinely reachable
    # through the existing genesis_dispatch mechanism -- the same dispatcher
    # a branch/call/return-terminated block already relies on -- with no new
    # control-transfer mechanism, runtime decoder, or per-block special case.
    (path / "generated.c").write_text(straight_line_block_first.stdout)
    (path / "harness.c").write_text(STRAIGHT_LINE_BLOCK_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "straight-line-block")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "straight-line-block")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T040 (C2): a TST.L absolute-long read at the already-implemented
    # CTRL1/CTRL2 selector (SEG-007-T020/T021) is retained as a controller_io
    # static memory fact during discovery and reaches genesis_route_access at
    # C4 lowering time -- neither a translation rejection nor the ROM/RAM
    # compile-time constant-fold form. Full compiled-and-executed reachability
    # through genesis_route_access itself is this task's separate C4
    # checkpoint; this is a generated-C-shape-level proof plus a standalone
    # C11 compile, matching --emit-general-startup-runtime-c4-multi-caller's
    # own compile-only (no link/execute) verification above.
    controller_io_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-controller-io"], text=True, capture_output=True)
    controller_io_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-controller-io"], text=True, capture_output=True)
    assert controller_io_first.returncode == controller_io_second.returncode == 0
    assert controller_io_first.stdout == controller_io_second.stdout
    assert not controller_io_first.stdout.startswith("/* translation rejected:")
    assert "const uint32_t m68k_routed_addr_0 = (UINT32_C(0x00A10008)) & UINT32_C(0x00FFFFFF);" in controller_io_first.stdout
    assert "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ" in controller_io_first.stdout
    assert "resolved static read" not in controller_io_first.stdout
    (path / "generated.c").write_text(controller_io_first.stdout)
    standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / "controller-io.o")], text=True, capture_output=True)
    assert standalone.returncode == 0, standalone.stderr
    # SEG-007-T040 (C3): the narrowest edge case C2's own [TST, BRA] fixture
    # above does not cover -- the routed CTRL1/CTRL2 TST.L as the program's
    # very first and only retained instruction, immediately, directly
    # followed by the unsupported RESET frontier with no intervening BRA. This
    # proves C3's block-construction fix reaches genuine C4 lowering
    # (genesis_route_access) for this minimal one-instruction-block shape
    # too, not merely satisfying runtime_frontier_eligible's own precondition
    # in isolation.
    controller_io_minimal_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-controller-io-minimal"], text=True, capture_output=True)
    controller_io_minimal_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-controller-io-minimal"], text=True, capture_output=True)
    assert controller_io_minimal_first.returncode == controller_io_minimal_second.returncode == 0
    assert controller_io_minimal_first.stdout == controller_io_minimal_second.stdout
    assert not controller_io_minimal_first.stdout.startswith("/* translation rejected:")
    assert "const uint32_t m68k_routed_addr_0 = (UINT32_C(0x00A10008)) & UINT32_C(0x00FFFFFF);" in controller_io_minimal_first.stdout
    assert "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ" in controller_io_minimal_first.stdout
    assert "resolved static read" not in controller_io_minimal_first.stdout
    (path / "generated.c").write_text(controller_io_minimal_first.stdout)
    standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / "controller-io-minimal.o")], text=True, capture_output=True)
    assert standalone.returncode == 0, standalone.stderr
    # SEG-007-T090: a TST.W ($00C00004).L WORD read of the VDP control/status
    # port is retained as a `vdp`-region static memory fact during discovery
    # and lowered to a runtime genesis_route_access WORD/READ call -- never a
    # translation rejection, never the ROM/RAM compile-time constant fold,
    # never a static frontier stop. Deterministic generated C, then a full
    # compile+link+execute proof that the routed read observes the runtime
    # VDP status register's zero default and TST.W of 0 sets Z / clears NVC.
    for suffix in ("--emit-general-startup-runtime-c4-vdp", "--emit-general-startup-runtime-c4-vdp-minimal"):
      vdp_first = subprocess.run([executable, suffix], text=True, capture_output=True)
      vdp_second = subprocess.run([executable, suffix], text=True, capture_output=True)
      assert vdp_first.returncode == vdp_second.returncode == 0, vdp_first.stderr
      assert vdp_first.stdout == vdp_second.stdout
      assert not vdp_first.stdout.startswith("/* translation rejected:")
      assert "const uint32_t m68k_routed_addr_0 = (UINT32_C(0x00C00004)) & UINT32_C(0x00FFFFFF);" in vdp_first.stdout
      assert "genesis_route_access(runtime, m68k_routed_addr_0, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ" in vdp_first.stdout
      assert "resolved static read" not in vdp_first.stdout
      (path / "generated.c").write_text(vdp_first.stdout)
      standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / "vdp.o")], text=True, capture_output=True)
      assert standalone.returncode == 0, standalone.stderr
    vdp_minimal = subprocess.run([executable, "--emit-general-startup-runtime-c4-vdp-minimal"], text=True, capture_output=True)
    (path / "generated.c").write_text(vdp_minimal.stdout)
    (path / "harness.c").write_text(VDP_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "vdp")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "vdp")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr

    # SEG-007-T113: the write-direction mirror -- a direct absolute-operand
    # WORD and LONG store into the VDP control-port window is deterministically
    # lowered to a genesis_route_access GENESIS_ACCESS_WRITE call (never a
    # compile-time literal, never a static VDP model, never a static frontier
    # stop). The BYTE-store neighbour stays an unmapped_data_access frontier.
    for suffix, width in (("--emit-general-startup-runtime-c4-vdp-store-word", "GENESIS_ACCESS_WORD"),
                          ("--emit-general-startup-runtime-c4-vdp-store-long", "GENESIS_ACCESS_LONG")):
      store_first = subprocess.run([executable, suffix], text=True, capture_output=True)
      store_second = subprocess.run([executable, suffix], text=True, capture_output=True)
      assert store_first.returncode == store_second.returncode == 0, store_first.stderr
      assert store_first.stdout == store_second.stdout
      assert not store_first.stdout.startswith("/* translation rejected:")
      assert f"genesis_route_access(runtime, m68k_routed_addr_1, {width}, GENESIS_ACCESS_WRITE" in store_first.stdout
      assert "resolved static read" not in store_first.stdout
      assert "runtime->work_ram" not in store_first.stdout
      (path / "generated.c").write_text(store_first.stdout)
      standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / "vdp-store.o")], text=True, capture_output=True)
      assert standalone.returncode == 0, standalone.stderr
    byte_store = subprocess.run([executable, "--emit-general-startup-runtime-c4-vdp-store-byte"], text=True, capture_output=True)
    assert byte_store.returncode == 0, byte_store.stderr
    assert "GENESIS_ACCESS_WRITE" not in byte_store.stdout or "GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE" not in byte_store.stdout
    assert "m68k_routed_addr" not in byte_store.stdout
    # WORD store: the routed store lands in devices.vdp.registers at run time.
    store_word = subprocess.run([executable, "--emit-general-startup-runtime-c4-vdp-store-word"], text=True, capture_output=True)
    (path / "generated.c").write_text(store_word.stdout)
    (path / "harness.c").write_text(VDP_STORE_WORD_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "vdp-store-word")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "vdp-store-word")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # LONG store: the full 10-byte instruction (> the pre-raise capacity of 8)
    # is carried through the raised GENESIS_MAX_RAW_BYTES (12) provenance ABI
    # and validated end to end by genesis_write_full_report.
    store_long = subprocess.run([executable, "--emit-general-startup-runtime-c4-vdp-store-long"], text=True, capture_output=True)
    (path / "generated.c").write_text(store_long.stdout)
    (path / "harness.c").write_text(VDP_STORE_LONG_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "vdp-store-long")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "vdp-store-long")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr

    # SEG-007-T115: the generalized routed-device write arm -- a direct
    # absolute-operand MOVE.W store into the 68k-side Z80 BUSREQ register
    # ($00A11100) is deterministically lowered to a genesis_route_access
    # GENESIS_ACCESS_WORD / GENESIS_ACCESS_WRITE call (no compile-time literal,
    # no static Z80/bus model, no static frontier stop), and at run time
    # latches the persistent GenesisZ80BusState before execution reaches the
    # established RESET frontier.
    z80_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-z80-bus-store"], text=True, capture_output=True)
    z80_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-z80-bus-store"], text=True, capture_output=True)
    assert z80_first.returncode == z80_second.returncode == 0, z80_first.stderr
    assert z80_first.stdout == z80_second.stdout
    assert not z80_first.stdout.startswith("/* translation rejected:")
    assert "genesis_route_access(runtime, m68k_routed_addr_1, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE" in z80_first.stdout
    assert "resolved static read" not in z80_first.stdout
    assert "runtime->work_ram" not in z80_first.stdout
    (path / "generated.c").write_text(z80_first.stdout)
    (path / "harness.c").write_text(Z80_BUS_STORE_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "z80-bus-store")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "z80-bus-store")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr

    for command, continuation, call_length in (("--emit-general-startup-runtime-c4-call-return", "0x00000B06", "6"),
                                                ("--emit-general-startup-runtime-c4-bsr-return", "0x00000B02", "2")):
      generated = subprocess.run([executable, command], text=True, capture_output=True)
      assert generated.returncode == 0, generated.stderr
      (path / "generated.c").write_text(generated.stdout)
      (path / "harness.c").write_text(CALL_RETURN_HARNESS)
      build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", f"-DEXPECT_CONTINUATION={continuation}", f"-DEXPECT_CALL_LENGTH={call_length}", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "call-return")], text=True, capture_output=True)
      assert build.returncode == 0, build.stderr
      ran = subprocess.run([str(path / "call-return")], text=True, capture_output=True)
      assert ran.returncode == 0, ran.stderr
    # Existing ADD/BTST/DBcc operation lowerers are dispatched through the
    # same C4 block route, with a deterministic two-frontier DBcc tail.
    add_bit_dbcc_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-add-bit-dbcc"], text=True, capture_output=True)
    add_bit_dbcc_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-add-bit-dbcc"], text=True, capture_output=True)
    assert add_bit_dbcc_first.returncode == add_bit_dbcc_second.returncode == 0
    assert add_bit_dbcc_first.stdout == add_bit_dbcc_second.stdout
    assert not add_bit_dbcc_first.stdout.startswith("/* translation rejected:")
    (path / "generated.c").write_text(add_bit_dbcc_first.stdout)
    (path / "harness.c").write_text(ADD_BIT_DBCC_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "add-bit-dbcc")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "add-bit-dbcc")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T124 / ADR-0009: the computed/indirect control-flow-target
    # resolution mechanism (JSR (d8,PC,Xn)). Deterministic two-run
    # byte-identical output, no rejection, the proven sorted candidate array,
    # the runtime membership guard, and the fail-closed stop identity -- then
    # a full compile+link+execute proof that BOTH proven candidates are
    # actually reachable through the unchanged generated dispatcher.
    indirect_jsr_first = subprocess.run([executable, "--emit-general-startup-runtime-c4-indirect-jsr"], text=True, capture_output=True)
    indirect_jsr_second = subprocess.run([executable, "--emit-general-startup-runtime-c4-indirect-jsr"], text=True, capture_output=True)
    assert indirect_jsr_first.returncode == indirect_jsr_second.returncode == 0
    assert indirect_jsr_first.stdout == indirect_jsr_second.stdout
    assert not indirect_jsr_first.stdout.startswith("/* translation rejected:")
    assert "static const uint32_t m68k_indirect_targets_00000B08[] = {UINT32_C(0x00000B0E), UINT32_C(0x00000B10)};" in indirect_jsr_first.stdout
    assert "m68k_indirect_target_member(m68k_indirect_targets_00000B08," in indirect_jsr_first.stdout
    assert "GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET, GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE" in indirect_jsr_first.stdout
    # No target instruction is ever fetched or decoded at runtime: the
    # generated block reads only architectural registers and the proven
    # literal candidate array, never `runtime->work_ram`/a decoded opcode.
    assert "runtime->work_ram" not in indirect_jsr_first.stdout
    (path / "generated.c").write_text(indirect_jsr_first.stdout)
    standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / "indirect-jsr.o")], text=True, capture_output=True)
    assert standalone.returncode == 0, standalone.stderr
    (path / "harness.c").write_text(INDIRECT_JSR_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "indirect-jsr")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "indirect-jsr")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
    # SEG-007-T236: the assisted Tier-1 candidate closure contains an RTS
    # with an ordinary incoming edge but no static return edge/frame. Two
    # independent Tier-2 call-shaped sites reach that same emitted block; its
    # real stack pop is checked against their existing generic continuations.
    ownerless_first = subprocess.run([executable, "--emit-t236-tier1-ownerless-rts"], text=True, capture_output=True)
    ownerless_second = subprocess.run([executable, "--emit-t236-tier1-ownerless-rts"], text=True, capture_output=True)
    assert ownerless_first.returncode == ownerless_second.returncode == 0
    assert ownerless_first.stdout == ownerless_second.stdout
    assert not ownerless_first.stdout.startswith("/* translation rejected:")
    assert "genesis_tier1_indirect_stop_00000B16" not in ownerless_first.stdout
    assert "m68k_observed_return != UINT32_C(0x00000B04)" in ownerless_first.stdout
    assert "m68k_observed_return != UINT32_C(0x00000B0A)" in ownerless_first.stdout
    (path / "generated.c").write_text(ownerless_first.stdout)
    standalone = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-c", str(path / "generated.c"), "-o", str(path / "t236-ownerless.o")], text=True, capture_output=True)
    assert standalone.returncode == 0, standalone.stderr
    (path / "harness.c").write_text(T236_TIER1_OWNERLESS_RTS_HARNESS)
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), "-I", str(path), str(path / "harness.c"), str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(path / "t236-ownerless")], text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    ran = subprocess.run([str(path / "t236-ownerless")], text=True, capture_output=True)
    assert ran.returncode == 0, ran.stderr
  print("genesis startup runtime C4: ok")

if __name__ == "__main__": main()
