#!/usr/bin/env python3
"""SEG-007-T247: strict-C11 compile/execute proof for the tenth-iteration
`write_move` absolute-destination family newly admitted into the
immutable-ROM AOT fact-free contract.

Proves, on the same project-authored synthetic fixture
`tests/m68k_pipeline_test.cpp`'s `experiment_aligned_aot_fixture` already
uses for the write_move family, that:

  * `MOVE.B D0,(0x00FF0070).L` (storage-free source, absolute_long
    destination) dispatched as an independent immutable-ROM AOT identity
    routes its write through the shared runtime memory gate on success, sets
    the byte-MOVE condition codes, and leaves D0 completely unmutated. Its
    identically-shaped sibling `MOVE.B D0,(0x00000000).L` (an address
    genesis_route_access always fails closed on -- an absolute destination's
    address is a compile-time constant, so this forced-write-failure case
    needs its own dedicated candidate rather than parametrized runtime
    state) leaves every architectural register/SR/PC unmutated.
  * `MOVE.B (A4)+,(0x00FF0080).L` (mutating register-indirect source,
    absolute_long destination) dispatched as an independent immutable-ROM
    AOT identity increments A4 exactly once on a successful routed
    read-then-write; a forced routed-READ failure leaves every
    architectural register/SR/PC unmutated, and a forced routed-WRITE
    failure (source read already succeeded) still leaves A4 uncommitted --
    matching the same "both accesses succeed before either register
    writeback" no-partial-mutation discipline T245/T246's own memory-to-
    memory carve-out already establishes. An absolute destination never
    reads or mutates any address register, so there is no cross-operand
    aliasing hazard to prove excluded here.
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

  /* MOVE.B D0,(0x00FF0070).L. Success: the absolute destination lies inside
     work RAM, which genesis_route_access always resolves generically. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C30);
  runtime.d[0] = UINT32_C(0x000000CD);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C36));
  assert(runtime.work_ram[0x70] == UINT8_C(0xCD));
  /* The storage-free source is completely unmutated by a MOVE write. */
  assert(runtime.d[0] == UINT32_C(0x000000CD));
  /* 0xCD is a negative byte, so exactly the N bit is set. */
  assert(runtime.sr == UINT16_C(0x0008));

  /* Forced routed-write failure: MOVE.B D0,(0x00000000).L is the same
     admitted addressing-mode shape, targeting an address genesis_route_
     access always fails closed on (no owned region is ever wired there),
     so genesis_route_access fails closed before any architectural state is
     touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C3C);
  runtime.d[0] = UINT32_C(0x000000CD);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.d[0] == UINT32_C(0x000000CD));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C3C));

  /* MOVE.B (A4)+,(0x00FF0080).L. Success: A4 points into work RAM. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C36);
  runtime.a[4] = UINT32_C(0x00FF0090);
  runtime.work_ram[0x90] = UINT8_C(0x42);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000C3C));
  assert(runtime.work_ram[0x80] == UINT8_C(0x42));
  /* A4 (the mutating source) increments exactly once (byte-sized
     postincrement steps by 1). */
  assert(runtime.a[4] == UINT32_C(0x00FF0091));
  /* 0x42 is a positive, nonzero byte, so N and Z are both clear. */
  assert(runtime.sr == UINT16_C(0x0000));

  /* Forced routed-READ failure: A4 points at an unregistered cartridge
     address, so genesis_route_access fails closed on the source access
     before any architectural state (including A4 itself, work RAM, or SR)
     is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000C36);
  runtime.a[4] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[4] == UINT32_C(0x00000000));
  assert(runtime.work_ram[0x80] == UINT8_C(0x00));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.pc == UINT32_C(0x00000C36));

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
    assert "genesis_aot_00000C30" in generated.stdout
    assert "genesis_aot_00000C36" in generated.stdout
    assert "genesis_aot_00000C3C" in generated.stdout
    assert "genesis_route_access(runtime," in generated.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "move-absolute-destination-aot"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_immutable_rom_aot_move_absolute_destination_generated_test: OK")


if __name__ == "__main__":
    main()
