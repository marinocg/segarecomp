#!/usr/bin/env python3
"""SEG-021-T014: strict-C11 generated-native proof of the ADDX/SUBX/NEGX/CMPM immutable-ROM AOT roots.

The emitter (`m68k_pipeline_tests --emit-extended-arithmetic-aot`) produces the complete generated program for a
synthetic seven-instruction ROM; the harness below executes it through the real Genesis runtime and checks
hand-derived results (Motorola M68000 Family Programmer's Reference Manual semantics, not the emitter's own output):
carry/borrow chaining through X, sticky Z, the A7 byte step of two, aliased register pairs and routed-stop atomicity.
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
int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;
  /* Chain F08..F16: ADDX.W -(A1),-(A0); CMPM.W (A1)+,(A0)+; NEGX.L (A0)+; NEGX.B D3; SUBX.B D1,D0;
   * ADDX.B -(A7),-(A7); CMPM.W (A2)+,(A2)+. Initial X=1, Z=0. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08); runtime.sr = UINT16_C(0x0010);
  runtime.a[1] = UINT32_C(0x00FF0102); runtime.a[0] = UINT32_C(0x00FF0202); runtime.a[2] = UINT32_C(0x00FF0400);
  runtime.a[7] = UINT32_C(0x00FF0300);
  runtime.d[3] = UINT32_C(0x12345600); runtime.d[1] = UINT32_C(1); runtime.d[0] = UINT32_C(1);
  runtime.work_ram[0x100] = UINT8_C(0xFF); runtime.work_ram[0x101] = UINT8_C(0xFF);  /* source word 0xFFFF */
  runtime.work_ram[0x200] = UINT8_C(0x00); runtime.work_ram[0x201] = UINT8_C(0x01);  /* destination word 0x0001 */
  runtime.work_ram[0x2FE] = UINT8_C(0x7F); runtime.work_ram[0x2FC] = UINT8_C(0x00);
  runtime.work_ram[0x400] = UINT8_C(0x00); runtime.work_ram[0x401] = UINT8_C(0x05);
  runtime.work_ram[0x402] = UINT8_C(0x00); runtime.work_ram[0x403] = UINT8_C(0x05);
  runtime.work_ram[0x202] = UINT8_C(0x01);                                            /* NEGX.L operand 0x01000000 */
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F16));
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  /* ADDX.W: 0x0001 + 0xFFFF + X(1) = 0x10001 -> 0x0001, carry into X/C, non-zero clears Z. */
  assert(runtime.work_ram[0x200] == UINT8_C(0x00) && runtime.work_ram[0x201] == UINT8_C(0x01));
  /* CMPM.W compares 0x0001 - 0xFFFF (borrow, X preserved) and writes nothing. NEGX.L (A0)+ at 0x202:
   * 0 - 0x01000000 - X(1) = 0xFEFFFFFF (borrow keeps X). */
  assert(runtime.work_ram[0x202] == UINT8_C(0xFE) && runtime.work_ram[0x203] == UINT8_C(0xFF) &&
         runtime.work_ram[0x204] == UINT8_C(0xFF) && runtime.work_ram[0x205] == UINT8_C(0xFF));
  assert(runtime.d[3] == UINT32_C(0x123456FF));                       /* NEGX.B D3: 0 - 0 - X = 0xFF */
  assert(runtime.d[0] == UINT32_C(0x000000FF));                       /* SUBX.B D1,D0: 1 - 1 - X = 0xFF */
  assert(runtime.a[1] == UINT32_C(0x00FF0102) && runtime.a[0] == UINT32_C(0x00FF0206));
  assert(runtime.work_ram[0x2FC] == UINT8_C(0x80) && runtime.a[7] == UINT32_C(0x00FF02FC));  /* A7 byte step 2 */
  assert(runtime.a[2] == UINT32_C(0x00FF0404));                       /* aliased CMPM.W pair, both postincrements */
  assert(runtime.sr == UINT16_C(0x0004));                             /* equal words: Z set, X preserved (clear) */

  /* Routed-stop atomicity: the source read fails (unmapped A1, outside the work-RAM mirrors) -> nothing changes. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08); runtime.sr = UINT16_C(0x0014);
  runtime.a[1] = UINT32_C(0x00500000); runtime.a[0] = UINT32_C(0x00FF0202);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F08));
  assert(runtime.a[1] == UINT32_C(0x00500000) && runtime.a[0] == UINT32_C(0x00FF0202) && runtime.sr == UINT16_C(0x0014));
  /* The source read succeeds but the destination read fails: the source register is NOT committed. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08); runtime.sr = UINT16_C(0x0014);
  runtime.a[1] = UINT32_C(0x00FF0102); runtime.a[0] = UINT32_C(0x00500000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F08));
  assert(runtime.a[1] == UINT32_C(0x00FF0102) && runtime.a[0] == UINT32_C(0x00500000) && runtime.sr == UINT16_C(0x0014));
  return 0;
}
'''


def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-extended-arithmetic-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    for address in ("00000F08", "00000F0A", "00000F0C", "00000F0E", "00000F10", "00000F12", "00000F14"):
        assert "genesis_aot_" + address in result.stdout, address
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(result.stdout)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "extended-arithmetic-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis_immutable_rom_aot_extended_arithmetic_generated_test: OK")


if __name__ == "__main__":
    main()
