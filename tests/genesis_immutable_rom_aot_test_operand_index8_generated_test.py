#!/usr/bin/env python3
"""SEG-007-T248: strict-C11 compile/execute proof for the newly admitted
brief-format indexed `test_operand` source (`M68kEaMode::address_index8`) in
the immutable-ROM AOT fact-free contract.

Proves, on the same project-authored `experiment_aligned_aot_fixture` synthetic
fixture the write_move family's own generated-native tests already reuse
(`tests/genesis_immutable_rom_aot_write_move_generated_test.py`), that
`TST.B (4,A6,D0.W)` dispatched as an independent immutable-ROM AOT identity:
  * on a successful routed read, updates only N/Z from the tested byte (V/C
    always cleared, per TST's existing shared CCR contract) and mutates
    neither the base address register (A6) nor the index register (D0) --
    the same no-mutation discipline already proven for the sibling `d16(An)`
    carve-out (SEG-007-T245's fifth iteration);
  * on a forced routed-read failure, leaves SR, A6, D0, and PC completely
    uncommitted and reports a runtime stop instead of a bogus continuation.

Also proves the excluded PC-relative sibling (`TST.B (4,PC,D0.W)`,
`M68kEaMode::pc_index8`) never becomes a dispatchable AOT identity at all: it
is TST's own decode-stage legal-EA mask (never any AOT-admission question)
that rejects every PC-relative form for TST, so the emitted generated source
never contains a `genesis_aot_<addr>` body for that instruction's address.

Also proves the write-direction symmetric case (SEG-007-T248's seventh
iteration): `MOVE.B D1,(4,A5,D0.W)` dispatched as an independent immutable-
ROM AOT identity -- a storage-free source paired with the same brief-format
indexed destination -- routes its write through the same shared runtime
memory gate, mutates neither A5 nor D0, and a forced routed-write failure
leaves every architectural register/work RAM/SR/PC completely uncommitted.

Also proves the read-modify-write case (SEG-007-T248's eighth iteration):
`SUBQ.B #1,(4,A5,D0.W)` dispatched as an independent immutable-ROM AOT
identity -- ADDQ/SUBQ's own instruction-embedded quick immediate paired with
a brief-format indexed read-modify-write destination -- performs one routed
read then one routed write to the same computed address, mutates neither A5
nor D0, and a forced routed-read failure leaves every architectural
register/work RAM/SR/PC completely uncommitted (the write is never even
reached).
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

  /* Success: A6 (the indexed base register) points into work RAM; D0 (the
     word-size index register) contributes a zero index so the effective
     address is exactly A6 + 4. The tested byte is negative, so N is set and
     Z is clear; V/C are always cleared by TST's shared CCR contract. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C46);
  runtime.a[6] = UINT32_C(0x00FF0070);
  runtime.d[0] = UINT32_C(0x00000000);
  runtime.work_ram[0x74] = UINT8_C(0x80);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(runtime.pc == UINT32_C(0x00000C4A));
  assert(runtime.sr == UINT16_C(0x0008));
  /* Neither the base nor the index register mutates -- address_index8 emits
     no prelude/postlude of any kind. */
  assert(runtime.a[6] == UINT32_C(0x00FF0070));
  assert(runtime.d[0] == UINT32_C(0x00000000));

  /* A nonzero, non-negative index moves the effective address as expected:
     A6 + sign_extend(D0.W) + 4. The tested byte is zero, so Z is set. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C46);
  runtime.a[6] = UINT32_C(0x00FF0070);
  runtime.d[0] = UINT32_C(0x00000002);
  runtime.work_ram[0x76] = UINT8_C(0x00);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(runtime.pc == UINT32_C(0x00000C4A));
  assert(runtime.sr == UINT16_C(0x0004));
  assert(runtime.a[6] == UINT32_C(0x00FF0070));
  assert(runtime.d[0] == UINT32_C(0x00000002));

  /* Forced routed-read failure: A6 points at an unregistered cartridge
     address, so genesis_route_access fails closed before SR is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C46);
  runtime.a[6] = UINT32_C(0x00000000);
  runtime.d[0] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[6] == UINT32_C(0x00000000));
  assert(runtime.d[0] == UINT32_C(0x00000000));
  assert(runtime.pc == UINT32_C(0x00000C46));

  /* Seventh-iteration admission (SEG-007-T248): MOVE.B D1,(4,A5,D0.W), the
     write-direction symmetric case. Success: A5 points into work RAM; D0
     contributes a zero index, so the effective address is exactly A5 + 4.
     0x99 is a negative byte, so exactly the N bit is set; neither A5 nor D0
     mutates. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C4E);
  runtime.a[5] = UINT32_C(0x00FF0080);
  runtime.d[0] = UINT32_C(0x00000000);
  runtime.d[1] = UINT32_C(0x00000099);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C52));
  assert(runtime.work_ram[0x84] == UINT8_C(0x99));
  assert(runtime.sr == UINT16_C(0x0008));
  assert(runtime.a[5] == UINT32_C(0x00FF0080));
  assert(runtime.d[0] == UINT32_C(0x00000000));
  assert(runtime.d[1] == UINT32_C(0x00000099));

  /* Forced routed-write failure: A5 points at an unregistered cartridge
     address, so genesis_route_access fails closed before any architectural
     state (including work RAM) is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C4E);
  runtime.a[5] = UINT32_C(0x00000000);
  runtime.d[0] = UINT32_C(0x00000000);
  runtime.d[1] = UINT32_C(0x00000099);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.d[0] == UINT32_C(0x00000000));
  assert(runtime.d[1] == UINT32_C(0x00000099));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C4E));

  /* Eighth-iteration admission (SEG-007-T248): SUBQ.B #1,(4,A5,D0.W), a
     read-modify-write brief-format indexed destination. Success: A5 points
     into work RAM; D0 contributes a zero index, so the effective address is
     exactly A5 + 4. 5 - 1 = 4: no borrow, no overflow, result positive and
     nonzero, so every CCR bit clears. Neither A5 nor D0 mutates. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C52);
  runtime.a[5] = UINT32_C(0x00FF0090);
  runtime.d[0] = UINT32_C(0x00000000);
  runtime.work_ram[0x94] = UINT8_C(5);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C56));
  assert(runtime.work_ram[0x94] == UINT8_C(4));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[5] == UINT32_C(0x00FF0090));
  assert(runtime.d[0] == UINT32_C(0x00000000));

  /* Forced routed-read failure: A5 points at an unregistered cartridge
     address, so genesis_route_access fails closed on the read before any
     architectural state (including work RAM, SR, or the never-reached
     write) is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C52);
  runtime.a[5] = UINT32_C(0x00000000);
  runtime.d[0] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.d[0] == UINT32_C(0x00000000));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C52));

  return 0;
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    generated = subprocess.run(
        [emitter, "--emit-write-move-register-indirect-aot"],
        text=True, capture_output=True,
    )
    assert generated.returncode == 0, generated.stderr
    assert not generated.stdout.startswith("/* translation rejected:")
    assert "genesis_aot_00000C46" in generated.stdout
    # The PC-relative sibling never decodes at all (TST admits no
    # PC-relative EA), so it never receives a generated AOT body either.
    assert "genesis_aot_00000C4A" not in generated.stdout
    assert "genesis_aot_00000C4E" in generated.stdout
    assert "genesis_aot_00000C52" in generated.stdout
    assert "genesis_route_access(runtime," in generated.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "test-operand-index8-aot"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_immutable_rom_aot_test_operand_index8_generated_test: OK")


if __name__ == "__main__":
    main()
