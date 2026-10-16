#!/usr/bin/env python3
"""SEG-068-T005 / ADR 0048 + ADR 0121 section 5: push-then-RTS computed jump through the H-interrupt root.

Project-authored synthetic image: a 0x200-byte vector table (level-4 autovector -> 0x210) and code at 0x200:

  0x200 JSR $230 ; 0x206 RESET           (mainline; 0x230 RTS is the real call's callee)
  0x210 MOVE.L $00FF0000.L,-(A7)         push whose value comes from RAM (block route, H-interrupt root)
  0x216 BEQ.S +2                         conditional branch owned by the same block route
  0x218 RTS                              AOT-only RTS popping the pushed value (the idiom's computed jump)
  0x21A ADDQ.L #4,A7 ; 0x21C RTE         the zero-value arm discards the push

The RTS is reached statically only through the interrupt root's bounded body (no JSR/BSR). Proves: a pushed value that is a
member of the final compiled-entry set dispatches (SP net unchanged); a non-member fails closed with
`return_target_mismatch` and the RTS uncommitted; the zero arm never reaches the RTS; generated output is deterministic.
"""

import pathlib
import subprocess
import sys
import tempfile

# Mainline: JSR $230 / RESET (gives the program a real call frame and a non-empty JSR-continuation set).
CODE = (
    "4EB900000230" +                           # 0x200 JSR $00000230
    "4E70" +                                   # 0x206 RESET (mainline terminal)
    "4E71" * 4 +                               # 0x208..0x20E NOP
    "2F3900FF0000" +                           # 0x210 MOVE.L $00FF0000.L,-(A7)
    "6702" +                                   # 0x216 BEQ.S +2
    "4E75" +                                   # 0x218 RTS
    "588F" +                                   # 0x21A ADDQ.L #4,A7
    "4E73" +                                   # 0x21C RTE
    "4E71" * 9 +                               # 0x21E..0x22E NOP
    "4E75"                                     # 0x230 callee RTS (the real JSR's return)
)

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#define main generated_bridge_main
#include "generated.c"
#undef main

static void put32(GenesisRuntime *r, uint32_t off, uint32_t v) {
  r->work_ram[off] = (uint8_t)(v >> 24); r->work_ram[off + 1] = (uint8_t)(v >> 16);
  r->work_ram[off + 2] = (uint8_t)(v >> 8); r->work_ram[off + 3] = (uint8_t)v;
}

int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;

  /* Positive: RAM value 0x200 is a compiled identity: push, BEQ not taken, RTS dispatches to it. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000210);
  runtime.a[7] = UINT32_C(0x00FF0100);
  put32(&runtime, 0x0, UINT32_C(0x00000200));
  transfer = genesis_bridge_dispatch(&runtime); /* the block route runs the push and the BEQ */
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000218));
  runtime.pc = transfer.next_pc;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000200));
  assert(runtime.a[7] == UINT32_C(0x00FF0100)); /* push 4 + pop 4: SP net unchanged */

  /* Negative: a non-member value fails closed with the RTS uncommitted. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000210);
  runtime.a[7] = UINT32_C(0x00FF0100);
  put32(&runtime, 0x0, UINT32_C(0x00001000));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00000218));
  runtime.pc = transfer.next_pc;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.diagnostic_category == GENESIS_DIAG_RETURN_TARGET_MISMATCH);
  assert(runtime.pc == UINT32_C(0x00000218));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC)); /* RTS did not pop */

  /* Zero arm: BEQ taken, the RTS is never reached. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000210);
  runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x0000021A));
  return 0;
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    runs = [subprocess.run([emitter, "--emit-synthetic-hint-aot-bridge", "00000210", CODE], text=True, capture_output=True)
            for _ in range(2)]
    for run in runs:
        assert run.returncode == 0, run.stderr
        assert not run.stdout.startswith("/* translation rejected:")
    assert runs[0].stdout == runs[1].stdout, "generated output must be deterministic"
    generated = runs[0]
    assert "genesis_compiled_entry_lookup(m68k_observed_return)" in generated.stdout, \
        "the AOT-only RTS popped by the block-route push must validate against the compiled-entry lookup"

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "hint-push-then-rts"
        built = subprocess.run(
            [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_hint_root_push_then_rts_generated_test: OK")


if __name__ == "__main__":
    main()
