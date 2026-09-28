#!/usr/bin/env python3
"""SEG-021-T041 / ADR 0048: strict-C11 compile/execute proof of the push-then-RTS computed jump.

Project-authored synthetic image (see `push_then_rts_fixture` in m68k_pipeline_test.cpp):

  0xD10 MOVE.L $10(A2,D1.W),-(A7)   0xD14 RTS   (RTS immediately preceded by a long push: computed jump)
  0xD16 MOVE.L (A2),-(A7)           0xD18 ADDQ.L #4,A7   0xD1A RTS  (A7 rewritten between: ordinary RTS)
  0xD0E RTS (preceded by BRA.S: ordinary RTS)    0xD1C RTS  (callee of the real JSR at 0xD00)
  0xD1E MOVE.L (A2),-(A7); TST.B; BNE.S; ST; 0xD2E RTS  (push, stack-neutral diamond, RTS: computed jump)

Proves: the pushed table entry (a compiled identity) is dispatched with SP net-unchanged and the pushed longword
still visible in work RAM; a pushed non-compiled value fails closed with the RTS uncommitted; every RTS not
reached from a push through a stack-neutral window keeps the JSR-continuation authority (a compiled non-continuation value still fails closed,
a real continuation still returns); generated output is deterministic.
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

static void put32(GenesisRuntime *r, uint32_t off, uint32_t v) {
  r->work_ram[off] = (uint8_t)(v >> 24); r->work_ram[off + 1] = (uint8_t)(v >> 16);
  r->work_ram[off + 2] = (uint8_t)(v >> 8); r->work_ram[off + 3] = (uint8_t)v;
}

int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;

  /* Positive: table entry (compiled identity 0xD0E) fetched via (d8,A2,D1.W), pushed, RTS dispatches to it. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D10);
  runtime.a[2] = UINT32_C(0x00FF0040);
  runtime.a[7] = UINT32_C(0x00FF0100);
  put32(&runtime, 0x50, UINT32_C(0x00000D0E));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D14));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC));
  assert(runtime.work_ram[0xFC] == 0x00 && runtime.work_ram[0xFD] == 0x00 &&
         runtime.work_ram[0xFE] == 0x0D && runtime.work_ram[0xFF] == 0x0E);
  runtime.pc = transfer.next_pc;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D0E));
  assert(runtime.a[7] == UINT32_C(0x00FF0100)); /* push 4 + pop 4: SP net unchanged */
  /* the pushed longword stays visible in the stack slot */
  assert(runtime.work_ram[0xFE] == 0x0D && runtime.work_ram[0xFF] == 0x0E);

  /* Positive: a second distinct table entry (the callee body 0xD1C) selects that identity. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D10);
  runtime.a[2] = UINT32_C(0x00FF0040);
  runtime.a[1] = UINT32_C(0);
  runtime.a[7] = UINT32_C(0x00FF0100);
  put32(&runtime, 0x50, UINT32_C(0x00000D1C));
  transfer = genesis_bridge_dispatch(&runtime);
  runtime.pc = transfer.next_pc;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000D1C));

  /* Negative: pushed value is not a compiled identity: fail closed, RTS uncommitted (push already committed). */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D10);
  runtime.a[2] = UINT32_C(0x00FF0040);
  runtime.a[7] = UINT32_C(0x00FF0100);
  put32(&runtime, 0x50, UINT32_C(0x00001000));
  transfer = genesis_bridge_dispatch(&runtime);
  runtime.pc = transfer.next_pc;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.pc == UINT32_C(0x00000D14));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC));

  /* Unchanged: ordinary RTS (0xD0E, preceded by BRA.S) still rejects a compiled non-continuation value. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D0E);
  runtime.a[7] = UINT32_C(0x00FF0080);
  put32(&runtime, 0x80, UINT32_C(0x00000D1C));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[7] == UINT32_C(0x00FF0080));

  /* Unchanged: ordinary RTS still returns to a genuine JSR continuation (0xD06). */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D0E);
  runtime.a[7] = UINT32_C(0x00FF0080);
  put32(&runtime, 0x80, UINT32_C(0x00000D06));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000D06));
  assert(runtime.a[7] == UINT32_C(0x00FF0084));

  /* Stack-altering instruction between push and RTS (MOVE.L, ADDQ.L #4,A7, RTS): the RTS at 0xD1A keeps the continuation authority. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D1A);
  runtime.a[7] = UINT32_C(0x00FF0080);
  put32(&runtime, 0x80, UINT32_C(0x00000D0E));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[7] == UINT32_C(0x00FF0080));

  /* Push, stack-neutral diamond (TST.B / BNE.S / ST), RTS: the RTS at 0xD2E is a computed jump on both arms. */
  for (int arm = 0; arm < 2; ++arm) {
    int step;
    runtime = (GenesisRuntime){0};
    runtime.pc = UINT32_C(0x00000D1E);
    runtime.a[2] = UINT32_C(0x00FF0040);
    runtime.a[7] = UINT32_C(0x00FF0100);
    runtime.work_ram[0x20] = (uint8_t)(arm ? 0x01 : 0x00);
    put32(&runtime, 0x40, UINT32_C(0x00000D1C));
    for (step = 0; step < 8 && runtime.pc != UINT32_C(0x00000D1C); ++step) {
      transfer = genesis_bridge_dispatch(&runtime);
      assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
      runtime.pc = transfer.next_pc;
    }
    assert(runtime.pc == UINT32_C(0x00000D1C));
    assert(runtime.a[7] == UINT32_C(0x00FF0100));
    assert(runtime.work_ram[0x21] == (uint8_t)(arm ? 0x00 : 0xFF));
  }
  /* The same diamond RTS with a non-compiled pushed value fails closed. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D1E);
  runtime.a[2] = UINT32_C(0x00FF0040);
  runtime.a[7] = UINT32_C(0x00FF0100);
  put32(&runtime, 0x40, UINT32_C(0x00001000));
  {
    int step;
    for (step = 0; step < 8; ++step) {
      transfer = genesis_bridge_dispatch(&runtime);
      if (transfer.kind != GENESIS_CONTINUE_AT_PC) break;
      runtime.pc = transfer.next_pc;
    }
    assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00000D2E));
    assert(runtime.a[7] == UINT32_C(0x00FF00FC));
  }

  /* Ordinary JSR/RTS: the real JSR's callee RTS (0xD1C) returns to its continuation. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D1C);
  runtime.a[7] = UINT32_C(0x00FF0080);
  put32(&runtime, 0x80, UINT32_C(0x00000D06));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000D06));

  return 0;
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    runs = [subprocess.run([emitter, "--emit-push-then-rts"], text=True, capture_output=True) for _ in range(2)]
    for run in runs:
        assert run.returncode == 0, run.stderr
        assert not run.stdout.startswith("/* translation rejected:")
    assert runs[0].stdout == runs[1].stdout, "generated output must be deterministic"
    generated = runs[0]
    for pc in ("D10", "D14", "D16", "D18", "D1A", "D0E", "D1E", "D2E"):
        assert "genesis_aot_00000" + pc in generated.stdout, pc

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "push-then-rts"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_push_then_rts_computed_jump_generated_test: OK")


if __name__ == "__main__":
    main()
