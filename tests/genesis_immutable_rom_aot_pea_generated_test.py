#!/usr/bin/env python3
"""Strict-C11 generated-native PEA d16(An) immutable-ROM AOT proof.

SEG-021-T011: PEA is now family-level admitted to immutable-ROM AOT
(`m68k_operation_is_immutable_rom_aot_safe`). This proves the atomic
local-A7-snapshot/deferred-commit lowering survives the AOT emission path
unchanged: success (A7 decrements exactly once, the pushed long equals the
computed effective address, PC advances only after the write succeeds) and
fail-closed atomicity (a ROM-window push target leaves A7, every register,
and PC completely unmodified, with no partial write).
"""
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
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;
  /* Success: PEA (0,A5) with A5 in work RAM, A7 also in work RAM. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08);
  runtime.a[5] = UINT32_C(0x00FF0080); runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F0C));
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(runtime.a[7] == UINT32_C(0x00FF00FC));  /* decremented exactly once, by 4 */
  assert(runtime.a[5] == UINT32_C(0x00FF0080));  /* PEA never mutates its source register */
  assert(runtime.work_ram[0x00FC] == 0x00U && runtime.work_ram[0x00FD] == 0xFFU &&
         runtime.work_ram[0x00FE] == 0x00U && runtime.work_ram[0x00FF] == 0x80U);
  /* Failure-ordering: A7-4 lands in the ROM window (< 0x00400000), so the
     routed LONG write fails before the deferred A7 commit is ever reached.
     A7, A5, and PC must all remain completely unmodified, and no byte of
     work_ram is touched. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08);
  runtime.a[5] = UINT32_C(0x00FF0090); runtime.a[7] = UINT32_C(0x00000100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F08));
  assert(runtime.a[7] == UINT32_C(0x00000100));
  assert(runtime.a[5] == UINT32_C(0x00FF0090));
  {
    unsigned i;
    for (i = 0U; i < sizeof(runtime.work_ram); ++i) assert(runtime.work_ram[i] == 0U);
  }
  return 0;
}
'''

def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-pea-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert "genesis_aot_00000F08" in result.stdout
    body = result.stdout.split("genesis_aot_00000F08(GenesisRuntime *runtime) {", 1)[1].split("\n}\n", 1)[0]
    assert body.count("genesis_route_access(runtime,") == 1
    assert "runtime->a[7] = m68k_pea_a7;" in body
    assert body.index("genesis_route_access(runtime,") < body.index("runtime->a[7] = m68k_pea_a7;")
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(result.stdout)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "pea-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis_immutable_rom_aot_pea_generated_test: OK")

if __name__ == "__main__":
    main()
