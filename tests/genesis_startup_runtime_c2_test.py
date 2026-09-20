#!/usr/bin/env python3
"""C2 persistent-runtime ABI and one-block C11 lowering regression."""
import pathlib
import subprocess
import sys
import tempfile


HARNESS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"

int main(void) {
  _Static_assert(GENESIS_ACCESS_BYTE == 1, "access byte ABI");
  _Static_assert(GENESIS_ACCESS_WORD == 2, "access word ABI");
  _Static_assert(GENESIS_ACCESS_LONG == 4, "access long ABI");
  _Static_assert(GENESIS_ACCESS_READ == 0, "access read ABI");
  _Static_assert(GENESIS_ACCESS_WRITE == 1, "access write ABI");
  _Static_assert(GENESIS_STOP_UNSUPPORTED_CPU_FORM == 1, "first stop ABI");
  _Static_assert(GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED == 8, "last stop ABI");
  _Static_assert(GENESIS_CPU_MC68000 == 1, "CPU ABI");
  _Static_assert(GENESIS_BUS_INSTRUCTION_READ == 1 && GENESIS_BUS_STACK_WRITE == 5,
                 "bus ABI");
  _Static_assert(GENESIS_REGION_RAW_CARTRIDGE_ROM == 1 &&
                     GENESIS_REGION_SYNTHETIC_WORK_RAM == 2,
                 "region ABI");
  _Static_assert(GENESIS_DIAG_ODD_INSTRUCTION_ADDRESS == 1 &&
                     GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET == 37,
                 "diagnostic ABI");
  _Static_assert(GENESIS_CONTINUE_AT_PC == 0 && GENESIS_STOP == 1 && GENESIS_COMPLETE == 2,
                 "transfer ABI");
  _Static_assert(GENESIS_MAX_RAW_BYTES == 12U && GENESIS_MAX_NAME_LENGTH == 64U &&
                     GENESIS_MAX_MAPPING_CLAIMS == 4U && GENESIS_MAX_BUS_ACCESSES == 4U,
                 "provenance capacity ABI");
  _Static_assert(offsetof(GenesisControlTransfer, kind) == 0U, "transfer kind order");
  _Static_assert(offsetof(GenesisControlTransfer, next_pc) > offsetof(GenesisControlTransfer, kind),
                 "transfer next PC order");
  _Static_assert(offsetof(GenesisControlTransfer, stop) > offsetof(GenesisControlTransfer, next_pc),
                 "transfer stop order");
  GenesisRuntime runtime = {0};
  GenesisRuntime *const same_object = &runtime;
  uint8_t saved_ram[65536];
  runtime.d[1] = UINT32_C(0x11223344);
  runtime.a[3] = UINT32_C(0x55667788);
  runtime.sr = UINT16_C(0xFFF0);
  runtime.pc = UINT32_C(0x00000900);
  memset(runtime.work_ram, 0xA5, sizeof(runtime.work_ram));
  memcpy(saved_ram, runtime.work_ram, sizeof(saved_ram));
  const GenesisControlTransfer transfer = genesis_block_00000900(same_object);
  assert(same_object == &runtime);
  assert(runtime.d[0] == UINT32_C(0x0000002A));
  assert(runtime.d[1] == UINT32_C(0x11223344));
  assert(runtime.a[3] == UINT32_C(0x55667788));
  assert(runtime.sr == UINT16_C(0xFFF0));
  assert(runtime.pc == UINT32_C(0x00000900));
  assert(memcmp(runtime.work_ram, saved_ram, sizeof(saved_ram)) == 0);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000900));
  assert(memcmp(&transfer.stop, &(GenesisRuntimeStop){0}, sizeof(transfer.stop)) == 0);
  return 0;
}
'''


def main() -> None:
  executable, compiler, source_root = sys.argv[1:]
  source_root = pathlib.Path(source_root)
  command = [executable, "--emit-general-startup-runtime-c2"]
  first = subprocess.run(command, text=True, capture_output=True, check=False)
  second = subprocess.run(command, text=True, capture_output=True, check=False)
  assert first.returncode == second.returncode == 0 and first.stderr == second.stderr == ""
  assert first.stdout == second.stdout
  generated = first.stdout
  assert "static GenesisControlTransfer genesis_block_00000900(GenesisRuntime *runtime)" in generated
  assert "runtime->d[0]" in generated and "runtime->sr" in generated and "runtime->pc" in generated
  assert "GenesisControlTransfer transfer = {0};" in generated
  assert "transfer.kind = GENESIS_CONTINUE_AT_PC;" in generated
  assert "transfer.next_pc = runtime->pc;" in generated
  assert "uint32_t d[" not in generated and "uint16_t sr" not in generated and "uint32_t pc" not in generated
  for forbidden in ("_anchor", "genesis_dispatch", "genesis_route_access", "main(",
                    "dispatch(", "decode", "driver"):
    assert forbidden not in generated
  with tempfile.TemporaryDirectory() as directory:
    directory = pathlib.Path(directory)
    (directory / "generated.c").write_text(generated)
    (directory / "harness.c").write_text(HARNESS)
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(source_root / "platforms/genesis/runtime"), "-I", str(directory),
                str(directory / "harness.c"), str(source_root / "platforms/genesis/runtime/runtime.c"),
               "-o", str(directory / "runtime-c2")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-c2")], text=True, capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime C2: ok")


if __name__ == "__main__":
  main()
