#!/usr/bin/env python3
"""C3 finite static multi-block persistent-runtime dispatch regression."""
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
  GenesisRuntime runtime = {0};
  GenesisRuntime before_unknown;
  uint8_t saved_ram[65536];
  GenesisControlTransfer transfer;
  runtime.d[1] = UINT32_C(0x11223344);
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.a[3] = UINT32_C(0x55667788);
  runtime.sr = UINT16_C(0xA5F0);  /* Z clear: BNE takes 0x908. */
  runtime.pc = UINT32_C(0x00000900);
  memset(runtime.work_ram, 0xA5, sizeof(runtime.work_ram));
  memcpy(saved_ram, runtime.work_ram, sizeof(saved_ram));

  transfer = genesis_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000908));
  assert(memcmp(&transfer.stop, &(GenesisRuntimeStop){0}, sizeof(transfer.stop)) == 0);
  assert(runtime.d[0] == UINT32_C(0x00000000));
  assert(runtime.d[1] == UINT32_C(0x11223344));
  assert(runtime.a[7] == UINT32_C(0x00FF0100));
  assert(runtime.a[3] == UINT32_C(0x55667788));
  assert(runtime.a[2] == UINT32_C(0x55667788));
  assert(runtime.sr == UINT16_C(0xA5F0));
  assert(runtime.pc == UINT32_C(0x00000908));
  assert(memcmp(runtime.work_ram, saved_ram, sizeof(saved_ram)) == 0);

  runtime.pc = transfer.next_pc;
  transfer = genesis_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000900));
  assert(memcmp(&transfer.stop, &(GenesisRuntimeStop){0}, sizeof(transfer.stop)) == 0);
  assert(runtime.d[0] == UINT32_C(0xFFFFFFFF));
  assert(runtime.d[1] == UINT32_C(0x11223344));
  assert(runtime.a[7] == UINT32_C(0x00FF0100));
  assert(runtime.a[3] == UINT32_C(0x55667788));
  assert(runtime.a[2] == UINT32_C(0x55667788));
  assert(runtime.sr == UINT16_C(0xA5F8));
  assert(runtime.pc == UINT32_C(0x00000900));
  assert(memcmp(runtime.work_ram, saved_ram, sizeof(saved_ram)) == 0);

  /* Z set: BNE falls through to the independently emitted 0x904 block. */
  runtime.d[0] = UINT32_C(0x0000002A);
  runtime.sr = UINT16_C(0xA5F4);
  runtime.pc = UINT32_C(0x00000900);
  transfer = genesis_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000904));
  assert(runtime.d[0] == UINT32_C(0x0000002A));
  assert(runtime.a[3] == UINT32_C(0x55667788));
  assert(runtime.a[7] == UINT32_C(0x00FF0100));
  assert(memcmp(runtime.work_ram, saved_ram, sizeof(saved_ram)) == 0);
  runtime.pc = transfer.next_pc;
  transfer = genesis_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000900));
  assert(runtime.d[0] == UINT32_C(0x00000029));
  assert(runtime.a[7] == UINT32_C(0x00FF0100));
  assert(memcmp(runtime.work_ram, saved_ram, sizeof(saved_ram)) == 0);

  runtime.pc = UINT32_C(0x00000906);
  before_unknown = runtime;
  transfer = genesis_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.next_pc == 0U);
  assert(transfer.stop.stop_class == GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY);
  assert(memcmp(&transfer.stop.provenance, &(GenesisProvenance){0}, sizeof(transfer.stop.provenance)) == 0);
  assert(memcmp(&runtime, &before_unknown, sizeof(runtime)) == 0);
  return 0;
}
'''


def main() -> None:
  executable, compiler, source_root = sys.argv[1:]
  source_root = pathlib.Path(source_root)
  command = [executable, "--emit-general-startup-runtime-c3"]
  first = subprocess.run(command, text=True, capture_output=True, check=False)
  second = subprocess.run(command, text=True, capture_output=True, check=False)
  assert first.returncode == second.returncode == 0 and first.stderr == second.stderr == ""
  assert first.stdout == second.stdout
  generated = first.stdout
  forged_condition = subprocess.run([executable, "--emit-general-startup-runtime-c3-forged-condition"],
                                    text=True, capture_output=True, check=False)
  assert forged_condition.returncode == 0 and forged_condition.stderr == ""
  assert forged_condition.stdout == "/* translation rejected: invalid C3 operation provenance */\n"
  for forged_mode in ("branch", "missing", "duplicate", "uncompiled", "nonterminal", "ir", "memory", "stack"):
    forged = subprocess.run([executable, f"--emit-general-startup-runtime-c3-forged-{forged_mode}"],
                            text=True, capture_output=True, check=False)
    assert forged.returncode == 0 and forged.stderr == ""
    assert forged.stdout.startswith("/* translation rejected:")
  for entry in ("00000900", "00000904", "00000908"):
    assert f"static GenesisControlTransfer genesis_block_{entry}(GenesisRuntime *runtime)" in generated
  assert "static GenesisControlTransfer genesis_dispatch(GenesisRuntime *runtime)" in generated
  assert "genesis_internal_dispatch_inconsistency_stop(runtime)" in generated
  assert generated.count("transfer.next_pc = runtime->pc;") == 3
  for forbidden in ("genesis_route_access", "main(", "decode", "work_ram[", "uint32_t d[", "uint32_t a["):
    assert forbidden not in generated
  with tempfile.TemporaryDirectory() as directory:
    directory = pathlib.Path(directory)
    (directory / "generated.c").write_text(generated)
    (directory / "harness.c").write_text(HARNESS)
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(source_root / "platforms/genesis/runtime"), "-I", str(directory),
                str(directory / "harness.c"), str(source_root / "platforms/genesis/runtime/runtime.c"),
               "-o", str(directory / "runtime-c3")]
    compiled = subprocess.run(command, text=True, capture_output=True, check=False)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(directory / "runtime-c3")], text=True, capture_output=True, check=False)
    assert ran.returncode == 0 and ran.stdout == "" and ran.stderr == "", ran
  print("genesis startup runtime C3: ok")


if __name__ == "__main__":
  main()
