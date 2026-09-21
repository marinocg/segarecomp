#!/usr/bin/env python3
"""SEG-007-T245: strict-C11 compile/execute proof for the narrow register-
indirect-source `write_move` family newly admitted into the immutable-ROM AOT
fact-free contract.

Proves, on a project-authored synthetic fixture, that `MOVE.B (A4)+,D0`
dispatched as an independent immutable-ROM AOT identity:
  * on a successful routed read, updates only D0's low byte (preserving its
    upper bytes exactly as real MOVE.B never sign/zero-extends the
    destination), sets the byte-MOVE condition codes, increments A4 exactly
    once, and advances PC exactly once;
  * on a forced routed-read failure, leaves D0, A4, and SR completely
    unmutated and reports a runtime stop instead of a bogus continuation --
    matching the same no-partial-mutation discipline the ordinary CFG-rooted
    C4 MOVE predecrement/postincrement mechanism already proves (SEG-007-
    T069/T070), reused here verbatim and unmodified.

Also proves the symmetric write direction admitted in the same task's second
iteration: `MOVE.B D0,(0,A5)` dispatched as an independent immutable-ROM AOT
identity routes the write through the same shared runtime memory gate, and a
forced routed-write failure leaves every architectural register untouched.

Also proves the third-iteration read-only `BTST #0,(A5)` admission: a
successful routed read only updates the Z condition code (never any
register), and a forced routed-read failure leaves SR completely unmutated.

Also proves the fourth-iteration `MOVEA.L (0,A6),A1` admission: a successful
routed read overwrites A1 with the loaded long word and leaves A6 (the
non-mutating `d16(An)` source register) unchanged, and a forced routed-read
failure leaves every architectural register untouched.

Also proves the fifth-iteration `TST.B (0,A6)` admission: a successful
routed read only updates N/Z (never any register, including A6), and a
forced routed-read failure leaves SR completely unmutated.

Also proves the sixth-iteration (SEG-007-T246) memory-to-memory
`MOVE.B (A4)+,(0,A5)` admission -- a mutating register-indirect source and a
`d16(An)` destination on two DISTINCT address registers: a successful routed
read-then-write increments A4 exactly once, leaves A5 (the non-mutating
destination register) unchanged, and sets the byte-MOVE condition codes; a
forced routed-read failure leaves every architectural register/work RAM
untouched, and a forced routed-write failure (source read already succeeded)
still leaves A4 uncommitted and work RAM untouched, matching the same
"both accesses succeed before either register writeback" discipline every
other admitted mutating-operand form above already proves. The excluded
same-register-aliasing sibling shape (`MOVE.B (A4)+,(0,A4)`) is proven
excluded separately at the classification/AOT-root level in
`tests/m68k_pipeline_test.cpp`; it never becomes a dispatchable identity, so
it has no generated-C harness case here.

Also proves the eleventh-iteration (SEG-007-T247) non-mutating
memory-to-memory `MOVE.B (A4),(0,A5)` admission -- a plain register-indirect
`(An)` source (never auto-incrementing/decrementing) paired with a `d16(An)`
destination on a distinct address register: neither operand mutates any
address register, so a successful routed read-then-write leaves BOTH A4 and
A5 completely unchanged, and either a forced routed-READ or a forced routed-
WRITE failure leaves every architectural register/work RAM/SR/PC untouched.
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

  /* Success: A4 points into work RAM, which genesis_route_access always
     resolves generically with no owned-region wiring required. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C16);
  runtime.a[4] = UINT32_C(0x00FF0000);
  runtime.d[0] = UINT32_C(0xDEADBEEF);
  runtime.work_ram[0] = UINT8_C(0xAB);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C18));
  /* Only the low byte changes; the upper three bytes of D0 are preserved
     exactly as real MOVE.B never sign/zero-extends the destination. */
  assert(runtime.d[0] == UINT32_C(0xDEADBEAB));
  /* A4 increments exactly once (byte-sized postincrement steps by 1). */
  assert(runtime.a[4] == UINT32_C(0x00FF0001));
  /* MOVE sets N/Z from the (sign-extended) result and always clears V/C;
     0xAB is a negative byte, so exactly the N bit is set. */
  assert(runtime.sr == UINT16_C(0x0008));

  /* Forced routed-access failure: A4 points at an unregistered cartridge
     address (the harness never wires runtime.owned_regions), so
     genesis_route_access fails closed before any architectural state is
     touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C16);
  runtime.a[4] = UINT32_C(0x00000000);
  runtime.d[0] = UINT32_C(0x12345678);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  /* No partial architectural mutation survives the failed routed read. */
  assert(runtime.d[0] == UINT32_C(0x12345678));
  assert(runtime.a[4] == UINT32_C(0x00000000));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C16));

  /* Symmetric write direction: MOVE.B D0,(0,A5). Success: A5 points into
     work RAM. d16(An) never mutates its own address register, so A5 and D0
     (the source) are both unchanged; only the routed write side effect
     (work_ram) and PC/SR change. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C18);
  runtime.a[5] = UINT32_C(0x00FF0010);
  runtime.d[0] = UINT32_C(0x000000CD);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C1C));
  assert(runtime.work_ram[0x10] == UINT8_C(0xCD));
  assert(runtime.d[0] == UINT32_C(0x000000CD));
  assert(runtime.a[5] == UINT32_C(0x00FF0010));
  /* 0xCD is a negative byte, so exactly the N bit is set, same as the read
     direction above. */
  assert(runtime.sr == UINT16_C(0x0008));

  /* Forced routed-write failure: A5 points at an unregistered cartridge
     address, so genesis_route_access fails closed before any architectural
     state (including work RAM) is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C18);
  runtime.a[5] = UINT32_C(0x00000000);
  runtime.d[0] = UINT32_C(0x000000CD);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.d[0] == UINT32_C(0x000000CD));
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C18));

  /* Third-iteration read-only admission: BTST #0,(A5). Success: A5 points
     into work RAM whose bit 0 is clear, so Z is set; BTST never mutates any
     register (not even the tested one -- there is none, since the bit
     number is a static immediate). */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C1C);
  runtime.a[5] = UINT32_C(0x00FF0020);
  runtime.work_ram[0x20] = UINT8_C(0x00);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C20));
  assert(runtime.sr == UINT16_C(0x0004));
  assert(runtime.a[5] == UINT32_C(0x00FF0020));
  assert(runtime.d[0] == UINT32_C(0));

  /* Forced routed-read failure: A5 points at an unregistered cartridge
     address, so genesis_route_access fails closed before SR is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C1C);
  runtime.a[5] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.pc == UINT32_C(0x00000C1C));

  /* Fourth-iteration admission: MOVEA.L (0,A6),A1. Success: A6 points into
     work RAM. MOVEA never touches condition codes and its d16(An) source
     never mutates A6. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C20);
  runtime.a[6] = UINT32_C(0x00FF0030);
  runtime.work_ram[0x30] = UINT8_C(0x11);
  runtime.work_ram[0x31] = UINT8_C(0x22);
  runtime.work_ram[0x32] = UINT8_C(0x33);
  runtime.work_ram[0x33] = UINT8_C(0x44);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C24));
  assert(runtime.a[1] == UINT32_C(0x11223344));
  assert(runtime.a[6] == UINT32_C(0x00FF0030));
  assert(runtime.sr == UINT16_C(0x0000));

  /* Forced routed-read failure: A6 points at an unregistered cartridge
     address, so genesis_route_access fails closed before A1 is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C20);
  runtime.a[6] = UINT32_C(0x00000000);
  runtime.a[1] = UINT32_C(0xCAFEBABE);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[1] == UINT32_C(0xCAFEBABE));
  assert(runtime.a[6] == UINT32_C(0x00000000));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C20));

  /* Fifth-iteration admission: TST.B (0,A6). Success: A6 points into work
     RAM whose value is nonzero and negative as a byte, so N is set and Z
     is clear; TST never mutates any register, including its own
     non-mutating d16(An) source register. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C24);
  runtime.a[6] = UINT32_C(0x00FF0040);
  runtime.work_ram[0x40] = UINT8_C(0x80);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C28));
  assert(runtime.sr == UINT16_C(0x0008));
  assert(runtime.a[6] == UINT32_C(0x00FF0040));

  /* Forced routed-read failure: A6 points at an unregistered cartridge
     address, so genesis_route_access fails closed before SR is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C24);
  runtime.a[6] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[6] == UINT32_C(0x00000000));
  assert(runtime.pc == UINT32_C(0x00000C24));

  /* Sixth-iteration admission (SEG-007-T246): MOVE.B (A4)+,(0,A5), a
     memory-to-memory MOVE on two DISTINCT address registers. Success: both
     A4 and A5 point into work RAM. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C28);
  runtime.a[4] = UINT32_C(0x00FF0050);
  runtime.a[5] = UINT32_C(0x00FF0060);
  runtime.work_ram[0x50] = UINT8_C(0x99);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C2C));
  assert(runtime.work_ram[0x60] == UINT8_C(0x99));
  /* A4 (the mutating source) increments exactly once; A5 (the non-mutating
     d16(An) destination register) is completely unchanged. */
  assert(runtime.a[4] == UINT32_C(0x00FF0051));
  assert(runtime.a[5] == UINT32_C(0x00FF0060));
  /* 0x99 is a negative byte, so exactly the N bit is set. */
  assert(runtime.sr == UINT16_C(0x0008));

  /* Forced routed-READ failure: A4 points at an unregistered cartridge
     address, so genesis_route_access fails closed on the source access
     before any architectural state (including A4 itself, A5, work RAM, or
     SR) is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C28);
  runtime.a[4] = UINT32_C(0x00000000);
  runtime.a[5] = UINT32_C(0x00FF0060);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[4] == UINT32_C(0x00000000));
  assert(runtime.a[5] == UINT32_C(0x00FF0060));
  assert(runtime.work_ram[0x60] == UINT8_C(0x00));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C28));

  /* Forced routed-WRITE failure: A4 points into work RAM (the source read
     succeeds), but A5 points at an unregistered cartridge address, so the
     destination's routed write fails closed BEFORE either address
     register's deferred commit is reached -- A4 stays uncommitted even
     though its own routed read already succeeded, exactly matching the
     established "both accesses succeed before either register writeback"
     no-partial-mutation discipline. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C28);
  runtime.a[4] = UINT32_C(0x00FF0050);
  runtime.a[5] = UINT32_C(0x00000000);
  runtime.work_ram[0x50] = UINT8_C(0x99);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[4] == UINT32_C(0x00FF0050));
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.work_ram[0x50] == UINT8_C(0x99));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C28));

  /* Eleventh-iteration admission (SEG-007-T247): MOVE.B (A4),(0,A5), a
     non-mutating memory-to-memory MOVE on two DISTINCT address registers.
     Success: both A4 and A5 point into work RAM. Unlike the sixth-
     iteration's mutating (A4)+ source, plain (A4) never auto-increments, so
     A4 is completely unchanged too -- not just A5. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C42);
  runtime.a[4] = UINT32_C(0x00FF0050);
  runtime.a[5] = UINT32_C(0x00FF0060);
  runtime.work_ram[0x50] = UINT8_C(0x99);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C46));
  assert(runtime.work_ram[0x60] == UINT8_C(0x99));
  /* Neither address register mutates -- not even the source. */
  assert(runtime.a[4] == UINT32_C(0x00FF0050));
  assert(runtime.a[5] == UINT32_C(0x00FF0060));
  /* 0x99 is a negative byte, so exactly the N bit is set. */
  assert(runtime.sr == UINT16_C(0x0008));

  /* Forced routed-READ failure: A4 points at an unregistered cartridge
     address, so genesis_route_access fails closed on the source access
     before any architectural state (including A5, work RAM, or SR) is
     touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C42);
  runtime.a[4] = UINT32_C(0x00000000);
  runtime.a[5] = UINT32_C(0x00FF0060);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[4] == UINT32_C(0x00000000));
  assert(runtime.a[5] == UINT32_C(0x00FF0060));
  assert(runtime.work_ram[0x60] == UINT8_C(0x00));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C42));

  /* Forced routed-WRITE failure: A4 points into work RAM (the source read
     succeeds), but A5 points at an unregistered cartridge address, so the
     destination's routed write fails closed. Since neither operand ever
     mutates its address register in this shape, there is no deferred
     commit to prove uncommitted -- this instead proves the routed write
     itself is the only side effect, and it never partially applies. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C42);
  runtime.a[4] = UINT32_C(0x00FF0050);
  runtime.a[5] = UINT32_C(0x00000000);
  runtime.work_ram[0x50] = UINT8_C(0x99);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[4] == UINT32_C(0x00FF0050));
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.work_ram[0x50] == UINT8_C(0x99));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C42));

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
    assert "genesis_aot_00000C16" in generated.stdout
    assert "genesis_aot_00000C18" in generated.stdout
    assert "genesis_aot_00000C1C" in generated.stdout
    assert "genesis_aot_00000C20" in generated.stdout
    assert "genesis_aot_00000C24" in generated.stdout
    assert "genesis_aot_00000C28" in generated.stdout
    # SEG-021-T005: the same-register aliasing form is admitted (destination EA derived from the updated source local).
    assert "genesis_aot_00000C2C" in generated.stdout
    assert "genesis_aot_00000C42" in generated.stdout
    assert "genesis_route_access(runtime," in generated.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "write-move-register-indirect-aot"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_immutable_rom_aot_write_move_generated_test: OK")


if __name__ == "__main__":
    main()
