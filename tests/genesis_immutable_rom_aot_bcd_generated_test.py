#!/usr/bin/env python3
"""SEG-021-T015: strict-C11 generated-native proof of the ABCD/SBCD/NBCD immutable-ROM AOT roots.

The emitter (`m68k_pipeline_tests --emit-bcd-aot`) produces the complete generated program for a synthetic
seven-instruction ROM; the harness below executes it through the real Genesis runtime and checks hand-derived
results (Motorola M68000 Family Programmer's Reference Manual decimal semantics, not the emitter's own output):
decimal carry/borrow chaining through X, sticky Z, the A7 byte step of two, aliased predecrement pairs, the NBCD
"nothing to negate" outcome and routed-stop atomicity. The undefined N/V flags are asserted only where the
matched-to-Musashi policy is explicitly documented (see M68kDecimalArithmeticSpecification).
"""
import pathlib
import subprocess
import sys
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"
static GenesisControlTransfer run_chain(GenesisRuntime *runtime) {
  GenesisControlTransfer transfer;
  do { transfer = genesis_bridge_dispatch(runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  return transfer;
}
int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;
  /* Chain F08..F16: ABCD.B -(A1),-(A0); SBCD.B -(A1),-(A0); NBCD.B (A0)+; NBCD.B D3; ABCD.B D1,D0;
   * SBCD.B -(A7),-(A7); NBCD.B -(A2). Initial X=1, Z=1. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08); runtime.sr = UINT16_C(0x0014);
  runtime.a[1] = UINT32_C(0x00FF0103); runtime.a[0] = UINT32_C(0x00FF0201); runtime.a[2] = UINT32_C(0x00FF0401);
  runtime.a[7] = UINT32_C(0x00FF0300);
  runtime.d[3] = UINT32_C(0x12345600); runtime.d[1] = UINT32_C(0x05); runtime.d[0] = UINT32_C(0xAB000095);
  runtime.work_ram[0x102] = UINT8_C(0x99);  /* ABCD source */
  runtime.work_ram[0x200] = UINT8_C(0x01);  /* ABCD destination */
  runtime.work_ram[0x101] = UINT8_C(0x05);  /* SBCD source */
  runtime.work_ram[0x1FF] = UINT8_C(0x03);  /* SBCD destination */
  transfer = run_chain(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F16));
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  /* 1: ABCD 01+99+X = 101 -> 01, decimal carry. 2: SBCD 03-05-X = -3 -> 97, borrow. 3: NBCD 0-97-X = -98 -> 02
   * written back through (A0)+ at 0x1FF. */
  assert(runtime.work_ram[0x200] == UINT8_C(0x01));
  assert(runtime.work_ram[0x1FF] == UINT8_C(0x02));
  assert(runtime.a[1] == UINT32_C(0x00FF0101) && runtime.a[0] == UINT32_C(0x00FF0200));
  assert(runtime.d[3] == UINT32_C(0x12345699));                        /* NBCD.B D3: 0-0-X = 99 with borrow */
  assert(runtime.d[0] == UINT32_C(0xAB000001));                        /* ABCD.B D1,D0: 95+05+X = 101 -> 01 */
  assert(runtime.work_ram[0x2FC] == UINT8_C(0x99) && runtime.a[7] == UINT32_C(0x00FF02FC));  /* A7 byte step 2, alias */
  assert(runtime.work_ram[0x400] == UINT8_C(0x99) && runtime.a[2] == UINT32_C(0x00FF0400));  /* NBCD -(A2): 0-0-X */
  assert(runtime.sr == UINT16_C(0x0019));                              /* X/C set (borrow), Z cleared; N per policy */

  /* Sticky Z: NBCD.B -(A2) of 0x99 with X=1 gives zero: X/C set, Z unchanged (set), N/V clear by policy. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F14); runtime.sr = UINT16_C(0x0014);
  runtime.a[2] = UINT32_C(0x00FF0401); runtime.work_ram[0x400] = UINT8_C(0x99);
  transfer = run_chain(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F16));
  assert(runtime.work_ram[0x400] == UINT8_C(0x00) && runtime.sr == UINT16_C(0x0015));
  /* NBCD of zero without X: nothing to negate. The byte stays 00, X/C are clear and Z is unchanged (documented);
   * N is set by the matched-to-Musashi undefined-flag policy. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F14); runtime.sr = UINT16_C(0x0004);
  runtime.a[2] = UINT32_C(0x00FF0401); runtime.work_ram[0x400] = UINT8_C(0x00);
  transfer = run_chain(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.work_ram[0x400] == UINT8_C(0x00) && runtime.a[2] == UINT32_C(0x00FF0400));
  assert((runtime.sr & UINT16_C(0x0015)) == UINT16_C(0x0004) && (runtime.sr & UINT16_C(0x0008)) == UINT16_C(0x0008));
  /* ABCD zero result keeps a clear Z and never sets it: 0x50+0x50 = 100 -> 00 with carry, Z stays clear. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F10); runtime.sr = UINT16_C(0x0000);
  runtime.d[1] = UINT32_C(0x50); runtime.d[0] = UINT32_C(0x50);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime.pc == UINT32_C(0x00000F12));
  assert(runtime.d[0] == UINT32_C(0) && (runtime.sr & UINT16_C(0x0015)) == UINT16_C(0x0011));

  /* Routed-stop atomicity: the source read fails (unmapped A1) -> nothing changes. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08); runtime.sr = UINT16_C(0x0014);
  runtime.a[1] = UINT32_C(0x00500000); runtime.a[0] = UINT32_C(0x00FF0202);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F08));
  assert(runtime.a[1] == UINT32_C(0x00500000) && runtime.a[0] == UINT32_C(0x00FF0202) && runtime.sr == UINT16_C(0x0014));
  /* Source read succeeds but the destination read fails: the source register is NOT committed. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08); runtime.sr = UINT16_C(0x0014);
  runtime.a[1] = UINT32_C(0x00FF0102); runtime.a[0] = UINT32_C(0x00500000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F08));
  assert(runtime.a[1] == UINT32_C(0x00FF0102) && runtime.a[0] == UINT32_C(0x00500000) && runtime.sr == UINT16_C(0x0014));
  /* NBCD (A0)+ routed stop: an unmapped operand changes neither the register nor the CCR. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F0C); runtime.sr = UINT16_C(0x0014);
  runtime.a[0] = UINT32_C(0x00500000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F0C));
  assert(runtime.a[0] == UINT32_C(0x00500000) && runtime.sr == UINT16_C(0x0014));
  return 0;
}
'''


def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-bcd-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    for address in ("00000F08", "00000F0A", "00000F0C", "00000F0E", "00000F10", "00000F12", "00000F14"):
        assert "genesis_aot_" + address in result.stdout, address
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(result.stdout)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "bcd-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis_immutable_rom_aot_bcd_generated_test: OK")


if __name__ == "__main__":
    main()
