#!/usr/bin/env python3
"""Strict-C11 execution proof for ADR-0032 descriptor-driven JSR (An)."""

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
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;
  runtime.pc = UINT32_C(0x00000B00);
  runtime.a[1] = UINT32_C(0x00FF0000);
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.work_ram[0] = UINT8_C(0x0B);
  runtime.work_ram[1] = UINT8_C(0x08);

  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000B08));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC));

  /* Execute the selected, descriptor-proposed and ADR-0025-admitted target.
     Its synthetic BRA result proves dispatch reached real generated target
     code rather than merely finding text for a membership arm. */
  runtime.pc = transfer.next_pc;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000B00));
  return 0;
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    generated = subprocess.run(
        [emitter, "--emit-code-pointer-descriptor-jsr-an"],
        text=True, capture_output=True,
    )
    assert generated.returncode == 0, generated.stderr
    assert not generated.stdout.startswith("/* translation rejected:")

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "descriptor-jsr-an"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_code_pointer_descriptor_generated_test: OK")


if __name__ == "__main__":
    main()
