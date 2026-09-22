#!/usr/bin/env python3
"""SEG-007-T249: strict-C11 compile/execute proof for the general (non-quick)
ADD/SUB/AND/OR/EOR read-modify-write memory-destination admission and
compare's own broader memory-operand admission in the immutable-ROM AOT
fact-free contract.

Proves, on the independent `general_arithmetic_memory_operand_fixture`
synthetic fixture (`tests/m68k_pipeline_test.cpp`), that three newly admitted
forms dispatch as independent immutable-ROM AOT identities:

  * `ADD.B D1,(0,A5)` (ninth iteration, same bounded family T245 opened): a
    plain Dn source paired with a d16(An) read-modify-write destination.
    On a successful routed read-then-write, updates the addressed byte and
    N/Z/V/C from the addition result, and mutates neither D1 nor A5. On a
    forced routed-read failure, leaves every architectural register, work
    RAM, SR, and PC completely uncommitted (the write is never even
    reached).

  * `CMP.B (0,A6),D2` and `CMP.B (4,A6,D0.W),D3` (compare's own broader
    admission): a non-mutating d16(An) or brief-format indexed source,
    read-only by construction. On a successful routed read, updates only
    N/Z/V/C from the comparison and mutates neither the addressed byte, A6,
    D0, D2, nor D3. On a forced routed-read failure, leaves every
    architectural register, work RAM, SR, and PC completely uncommitted.

  * `CLR.W (0,A4)` (this task's own corrected same-seam continuation
    iteration, consuming what was initially misclassified as a successor
    frontier): CLR's own dedicated d16(An) destination carve-out. On a
    successful routed WORD write of zero, the addressed word becomes zero,
    A4 is unmutated, N/Z/V/C become CLR's fixed pattern (N=0,Z=1,V=0,C=0)
    and X is preserved. On a forced routed-write failure, the addressed
    memory, A4, SR, and PC are all left completely uncommitted.

  * `MOVE.B (0,A0),(0,A5)` (this task's own second corrected same-seam
    continuation iteration): a d16(An) source paired with a d16(An)
    destination. On a successful routed read-then-write, the destination
    byte becomes the source byte, N/Z update from the moved value (V/C
    always cleared, X preserved), and neither A0 nor A5 mutates. On a
    forced routed-read failure, every architectural register, both work RAM
    locations, SR, and PC are left completely uncommitted (the write is
    never even reached). On a forced routed-write failure (read succeeds,
    write fails), the destination memory, both address registers, SR, and
    PC are all left completely uncommitted.
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

  /* ADD.B D1,(0,A5): success. A5 points into work RAM; the addressed byte
     is 0x05, D1 is 0x03, so the result is 0x08 (no carry, no overflow,
     positive, nonzero -- every CCR bit clears). Neither D1 nor A5
     mutates. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E08);
  runtime.a[5] = UINT32_C(0x00FF0070);
  runtime.d[1] = UINT32_C(0x00000003);
  runtime.work_ram[0x70] = UINT8_C(0x05);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000E0C));
  assert(runtime.work_ram[0x70] == UINT8_C(0x08));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[5] == UINT32_C(0x00FF0070));
  assert(runtime.d[1] == UINT32_C(0x00000003));

  /* ADD.B D1,(0,A5): a negative-result addition sets N. 0x7F + 0x7F =
     0xFE: N set, an unsigned/sign overflow (V) since two positives sum to
     a negative byte, no unsigned carry (C clear). */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E08);
  runtime.a[5] = UINT32_C(0x00FF0074);
  runtime.d[1] = UINT32_C(0x0000007F);
  runtime.work_ram[0x74] = UINT8_C(0x7F);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000E0C));
  assert(runtime.work_ram[0x74] == UINT8_C(0xFE));
  assert(runtime.sr == UINT16_C(0x000A)); /* N | V */

  /* ADD.B D1,(0,A5): forced routed-read failure. A5 points at an
     unregistered cartridge address, so genesis_route_access fails closed
     before the write, SR, D1, or A5 is ever touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E08);
  runtime.a[5] = UINT32_C(0x00000000);
  runtime.d[1] = UINT32_C(0x00000003);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.d[1] == UINT32_C(0x00000003));
  assert(runtime.pc == UINT32_C(0x00000E08));

  /* CMP.B (0,A6),D2: success, equal operands set Z. A6 points into work
     RAM; the addressed byte and D2 are both 0x05, so the compare result is
     zero. Neither the addressed byte, A6, nor D2 mutates. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E0C);
  runtime.a[6] = UINT32_C(0x00FF0080);
  runtime.d[2] = UINT32_C(0x00000005);
  runtime.work_ram[0x80] = UINT8_C(0x05);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000E10));
  assert(runtime.work_ram[0x80] == UINT8_C(0x05));
  assert(runtime.sr == UINT16_C(0x0004)); /* Z */
  assert(runtime.a[6] == UINT32_C(0x00FF0080));
  assert(runtime.d[2] == UINT32_C(0x00000005));

  /* CMP.B (0,A6),D2: a borrow (source > destination) sets C; the byte
     result is negative, setting N too. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E0C);
  runtime.a[6] = UINT32_C(0x00FF0084);
  runtime.d[2] = UINT32_C(0x00000001);
  runtime.work_ram[0x84] = UINT8_C(0x02);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000E10));
  assert(runtime.sr == UINT16_C(0x0009)); /* N | C */

  /* CMP.B (0,A6),D2: forced routed-read failure. A6 points at an
     unregistered cartridge address, so genesis_route_access fails closed
     before SR, D2, or A6 is ever touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E0C);
  runtime.a[6] = UINT32_C(0x00000000);
  runtime.d[2] = UINT32_C(0x00000005);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[6] == UINT32_C(0x00000000));
  assert(runtime.d[2] == UINT32_C(0x00000005));
  assert(runtime.pc == UINT32_C(0x00000E0C));

  /* CMP.B (4,A6,D0.W),D3: success. A6 points into work RAM; D0 contributes
     a zero index, so the effective address is exactly A6 + 4. Both the
     addressed byte and D3 are 0x05, so the compare result is zero. Neither
     the addressed byte, A6, D0, nor D3 mutates. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E10);
  runtime.a[6] = UINT32_C(0x00FF0090);
  runtime.d[0] = UINT32_C(0x00000000);
  runtime.d[3] = UINT32_C(0x00000005);
  runtime.work_ram[0x94] = UINT8_C(0x05);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000E14));
  assert(runtime.work_ram[0x94] == UINT8_C(0x05));
  assert(runtime.sr == UINT16_C(0x0004)); /* Z */
  assert(runtime.a[6] == UINT32_C(0x00FF0090));
  assert(runtime.d[0] == UINT32_C(0x00000000));
  assert(runtime.d[3] == UINT32_C(0x00000005));

  /* CMP.B (4,A6,D0.W),D3: a nonzero index moves the effective address as
     expected -- A6 + sign_extend(D0.W) + 4. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E10);
  runtime.a[6] = UINT32_C(0x00FF0090);
  runtime.d[0] = UINT32_C(0x00000002);
  runtime.d[3] = UINT32_C(0x00000009);
  runtime.work_ram[0x96] = UINT8_C(0x09);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000E14));
  assert(runtime.sr == UINT16_C(0x0004)); /* Z */
  assert(runtime.d[0] == UINT32_C(0x00000002));

  /* CMP.B (4,A6,D0.W),D3: forced routed-read failure. A6 points at an
     unregistered cartridge address, so genesis_route_access fails closed
     before SR, D0, D3, or A6 is ever touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E10);
  runtime.a[6] = UINT32_C(0x00000000);
  runtime.d[0] = UINT32_C(0x00000000);
  runtime.d[3] = UINT32_C(0x00000005);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[6] == UINT32_C(0x00000000));
  assert(runtime.d[0] == UINT32_C(0x00000000));
  assert(runtime.d[3] == UINT32_C(0x00000005));
  assert(runtime.pc == UINT32_C(0x00000E10));

  /* CLR.W (0,A4): success. A4 points into work RAM; the addressed word is
     nonzero. SR starts with every CCR bit set (X/N/Z/V/C = 0x1F) so the
     fixed CLR pattern is observable and X's preservation is distinguishable
     from an accidental full clear. After execution the addressed word is
     zero, A4 is unmutated, and SR is exactly X|Z (0x14): N=0,Z=1,V=0,C=0,
     X preserved. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E14);
  runtime.a[4] = UINT32_C(0x00FF00A0);
  runtime.sr = UINT16_C(0x001F);
  runtime.work_ram[0xA0] = UINT8_C(0xFF);
  runtime.work_ram[0xA1] = UINT8_C(0xFF);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000E18));
  assert(runtime.work_ram[0xA0] == UINT8_C(0x00));
  assert(runtime.work_ram[0xA1] == UINT8_C(0x00));
  assert(runtime.sr == UINT16_C(0x0014)); /* X | Z */
  assert(runtime.a[4] == UINT32_C(0x00FF00A0));

  /* CLR.W (0,A4): forced routed-write failure. A4 points at an unregistered
     cartridge address, so genesis_route_access fails closed before the
     addressed memory, A4, SR, or PC is ever touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E14);
  runtime.a[4] = UINT32_C(0x00000000);
  runtime.sr = UINT16_C(0x001F);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[4] == UINT32_C(0x00000000));
  assert(runtime.sr == UINT16_C(0x001F));
  assert(runtime.pc == UINT32_C(0x00000E14));

  /* MOVE.B (0,A0),(0,A5): success. A0 and A5 point into (distinct) work
     RAM; the source byte is negative (0x85), so N is set and Z is clear;
     V/C are always cleared, X is preserved. Neither A0 nor A5 mutates. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E18);
  runtime.a[0] = UINT32_C(0x00FF00B0);
  runtime.a[5] = UINT32_C(0x00FF00C0);
  runtime.sr = UINT16_C(0x0010); /* X set, everything else clear */
  runtime.work_ram[0xB0] = UINT8_C(0x85);
  runtime.work_ram[0xC0] = UINT8_C(0x00);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(runtime.pc == UINT32_C(0x00000E1E));
  assert(runtime.work_ram[0xC0] == UINT8_C(0x85));
  assert(runtime.work_ram[0xB0] == UINT8_C(0x85));
  assert(runtime.sr == UINT16_C(0x0018)); /* X | N */
  assert(runtime.a[0] == UINT32_C(0x00FF00B0));
  assert(runtime.a[5] == UINT32_C(0x00FF00C0));

  /* MOVE.B (0,A0),(0,A5): a zero source byte sets Z instead. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E18);
  runtime.a[0] = UINT32_C(0x00FF00B4);
  runtime.a[5] = UINT32_C(0x00FF00C4);
  runtime.work_ram[0xB4] = UINT8_C(0x00);
  runtime.work_ram[0xC4] = UINT8_C(0x7F);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(runtime.pc == UINT32_C(0x00000E1E));
  assert(runtime.work_ram[0xC4] == UINT8_C(0x00));
  assert(runtime.sr == UINT16_C(0x0004)); /* Z */

  /* MOVE.B (0,A0),(0,A5): forced routed-read failure. A0 points at an
     unregistered cartridge address, so genesis_route_access fails closed
     before the write, A5's memory, SR, A0, or A5 is ever touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E18);
  runtime.a[0] = UINT32_C(0x00000000);
  runtime.a[5] = UINT32_C(0x00FF00C8);
  runtime.work_ram[0xC8] = UINT8_C(0x42);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.work_ram[0xC8] == UINT8_C(0x42));
  assert(runtime.a[0] == UINT32_C(0x00000000));
  assert(runtime.a[5] == UINT32_C(0x00FF00C8));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000E18));

  /* MOVE.B (0,A0),(0,A5): forced routed-write failure. A0 points into work
     RAM (the read succeeds), but A5 points at an unregistered cartridge
     address, so genesis_route_access fails closed on the write before the
     destination memory, A0, A5, SR, or PC is ever touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000E18);
  runtime.a[0] = UINT32_C(0x00FF00CC);
  runtime.a[5] = UINT32_C(0x00000000);
  runtime.work_ram[0xCC] = UINT8_C(0x42);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.work_ram[0xCC] == UINT8_C(0x42));
  assert(runtime.a[0] == UINT32_C(0x00FF00CC));
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000E18));

  return 0;
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    generated = subprocess.run(
        [emitter, "--emit-general-arithmetic-memory-operand-aot"],
        text=True, capture_output=True,
    )
    assert generated.returncode == 0, generated.stderr
    assert not generated.stdout.startswith("/* translation rejected:")
    assert "genesis_aot_00000E08" in generated.stdout
    assert "genesis_aot_00000E0C" in generated.stdout
    assert "genesis_aot_00000E10" in generated.stdout
    assert "genesis_aot_00000E14" in generated.stdout
    assert "genesis_aot_00000E18" in generated.stdout
    assert "genesis_route_access(runtime," in generated.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "general-arithmetic-memory-operand-aot"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_immutable_rom_aot_general_arithmetic_memory_operand_generated_test: OK")


if __name__ == "__main__":
    main()
