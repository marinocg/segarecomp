#!/usr/bin/env python3
"""SEG-007-T238: strict-C11 compile/execute proof for the build-time-only,
explicitly opt-in broad aligned-M68k-ROM AOT representation experiment.

Proves the ONE observable contract the experiment's seam guarantees on a
project-authored synthetic fixture: a valid, independently decoded/lowered
aligned candidate reaches real, dispatchable generated code through the
static precompiled-entry lookup, and the generated runtime never fetches or
decodes a target byte to do it.
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
  GenesisRuntime runtime = {0};
  GenesisControlTransfer transfer;

  /* The ordinary CFG root remains selectable beside the independent AOT
     identities. Its (A1) read supplies the dynamic JSR destination. */
  runtime.pc = UINT32_C(0x00000C00);
  runtime.a[1] = UINT32_C(0x00FF0000);
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.work_ram[0] = UINT8_C(0x0C);
  runtime.work_ram[1] = UINT8_C(0x06);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C06));

  /* Dispatch directly to V1 (base+0x06): a valid, experimentally-proposed
     aligned entry admitted through the unmodified existing decode/lowering
     walk. It must reach real generated code and execute its own BRA.S back
     to the ingress (base = 0x00000C00). */
  runtime.pc = UINT32_C(0x00000C06);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C00));
  assert(runtime.d[3] == UINT32_C(0)); /* compiled data remains inert */

  /* Dispatch to V2 (base+0x08): the second independently admitted valid
     experimental entry. */
  runtime.pc = UINT32_C(0x00000C08);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C00));

  /* The invalid-encoding and excluded-form candidate addresses never became
     emitted-code representation: dispatching to either must fail closed
     after lookup misses, never by fetching or decoding a byte there. */
  runtime.pc = UINT32_C(0x00000C0A);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind != GENESIS_CONTINUE_AT_PC);

  runtime.pc = UINT32_C(0x00000C0C);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind != GENESIS_CONTINUE_AT_PC);

  /* Two valid starts overlap: the second starts in the first instruction's
     extension word, but each has its own PC-keyed compiled body. */
  runtime.pc = UINT32_C(0x00000C12);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C14));
  assert(runtime.d[3] == UINT32_C(0));

  /* The same bytes classified above as inert data execute only when the
     architectural PC explicitly selects their compiled identity. */
  runtime.pc = UINT32_C(0x00000C14);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(runtime.d[3] == UINT32_C(5));

  return 0;
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    generated = subprocess.run(
        [emitter, "--emit-experiment-aligned-aot"],
        text=True, capture_output=True,
    )
    assert generated.returncode == 0, generated.stderr
    assert not generated.stdout.startswith("/* translation rejected:")
    assert "static GenesisCompiledEntry genesis_compiled_entry_lookup(uint32_t address)" in generated.stdout
    assert "while (low < high)" in generated.stdout
    assert "GenesisCompiledEntry entry = genesis_compiled_entry_lookup(runtime->pc)" in generated.stdout
    assert "if (runtime->pc == UINT32_C(0x00000C06))" not in generated.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "experiment-aligned-aot"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_experiment_aligned_aot_generated_test: OK")


if __name__ == "__main__":
    main()
