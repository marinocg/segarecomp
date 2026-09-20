#!/usr/bin/env python3
"""Strict-C11 generated-native NEG.B d16(An) immutable-ROM AOT proof."""
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
  static const uint8_t read_only_byte[] = { UINT8_C(1) };
  static const GenesisOwnedCartridgeRegion read_only_region[] = {
    { UINT32_C(0x00000100), UINT32_C(0x00000101), read_only_byte, UINT32_C(1) }
  };
  /* Successful routed read then write: 0 - 1 is 0xff, with N/C/X set. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08);
  runtime.a[5] = UINT32_C(0x00FF0070); runtime.sr = UINT16_C(0x0010);
  runtime.work_ram[0x70] = UINT8_C(1);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000F0C));
  assert(runtime.work_ram[0x70] == UINT8_C(0xff));
  assert(runtime.a[5] == UINT32_C(0x00FF0070) && runtime.sr == UINT16_C(0x0019));
  /* Zero proves Z and NEG's carry-derived X clearing separately. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08);
  runtime.a[5] = UINT32_C(0x00FF0074); runtime.sr = UINT16_C(0x0010);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime.sr == UINT16_C(0x0004));
  assert(runtime.a[5] == UINT32_C(0x00FF0074));
  /* The signed minimum stays negative and proves V together with N/C/X. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08);
  runtime.a[5] = UINT32_C(0x00FF0078); runtime.sr = UINT16_C(0x0010);
  runtime.work_ram[0x78] = UINT8_C(0x80);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime.work_ram[0x78] == UINT8_C(0x80));
  assert(runtime.sr == UINT16_C(0x001B) && runtime.a[5] == UINT32_C(0x00FF0078));
  /* An unrouted address fails the read: neither write nor architectural commit occurs. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08);
  runtime.a[5] = UINT32_C(0); runtime.sr = UINT16_C(0x0010);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F08));
  assert(runtime.a[5] == UINT32_C(0) && runtime.sr == UINT16_C(0x0010));
  /* A generated immutable cartridge-data region admits the routed read but
     prohibits the following write. The routing contract keeps CCR, PC, An,
     and immutable backing storage uncommitted. */
  runtime = (GenesisRuntime){0}; runtime.pc = UINT32_C(0x00000F08);
  runtime.a[5] = UINT32_C(0x00000100); runtime.sr = UINT16_C(0x0010);
  runtime.owned_regions = read_only_region; runtime.owned_region_count = UINT32_C(1);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000F08));
  assert(runtime.a[5] == UINT32_C(0x00000100) && runtime.sr == UINT16_C(0x0010));
  assert(read_only_byte[0] == UINT8_C(1));
  return 0;
}
'''

def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-negate-disp16-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert "genesis_aot_00000F08" in result.stdout
    body = result.stdout.split("genesis_aot_00000F08(GenesisRuntime *runtime) {", 1)[1].split("\n}\n", 1)[0]
    assert body.count("genesis_route_access(runtime,") == 2
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(result.stdout)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "negate-disp16-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis_immutable_rom_aot_negate_disp16_generated_test: OK")

if __name__ == "__main__":
    main()
