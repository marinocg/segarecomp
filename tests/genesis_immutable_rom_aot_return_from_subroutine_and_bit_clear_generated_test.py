#!/usr/bin/env python3
"""SEG-007-T246: strict-C11 compile/execute proof that an isolated, ownerless
`RTS` immutable-ROM AOT candidate validates against ADR-0011's real
whole-program continuation authority -- existing-owner integration, not a
new architecture -- and that the new BCHG/BCLR/BSET plain-`(An)` destination
carve-out routes its read-modify-write correctly through the runtime memory
gate.

Proves, on a project-authored synthetic fixture whose ordinary direct call
(`JSR $00000D14`) genuinely populates ADR-0011's whole-program
`runtime_return_target_set` with its own real continuation (0x00000D06):

  * the isolated, PC-keyed-only `RTS` candidate at 0x00000D0E (never on any
    walked CFG path, never owning an `M68kStaticCall`/frame/`return_to_
    continuation` edge of its own) reads the real popped stack longword
    through `genesis_route_access`, advances A7 by exactly 4, and dispatches
    to the real popped PC when that value equals the genuine call
    continuation -- ADR-0039's ownerless-RTS model applied to an
    independent AOT identity, reusing the exact same shared authority every
    ordinary CFG-rooted RTS in a real program already validates against;
  * a forced routed-read failure leaves A7, PC, and SR completely
    unmutated;
  * a popped value that is itself a real, compiled, dispatchable address
    (another AOT identity) but NOT a member of the whole-program
    continuation authority still fails closed -- proving compiled
    membership alone never authorizes an RTS return, exactly as SEG-007-
    T239 already established;
  * the new BCLR #3,(A5) admission (0x00000D10): a successful routed
    read-modify-write clears the tested bit, updates only the Z condition
    code, and leaves every register untouched; a forced routed-access
    failure leaves SR and the target byte completely untouched.

Also proves the foldable-target JMP admission (0x00000D16): a pure constant
PC assignment with no memory access, no register mutation, and no
precondition of any kind.

Also proves the foldable-target JSR admission (0x00000D1A), an isolated
candidate with no CFG/frame ownership of its own: a successful routed stack
push writes exactly its own provenance-derived continuation (this
candidate's own address+length, never an external/whole-program fact),
decrements A7 by exactly 4, and dispatches to the folded constant callee
target; a forced routed-write failure leaves A7, PC, and the target stack
slot completely unmutated.

Also proves the T208-mirrored non-fabrication gate on the admitted-AOT-call
continuation producer: an admitted AOT call's own continuation joins RTS
return-target authority ONLY when that continuation is itself a genuine,
independently represented/dispatchable program identity (an ordinary
emitted block or another admitted AOT identity), never merely because the
source instruction has call semantics.

  * POSITIVE: 0x00000D1A's own continuation (0x00000D20) is itself a real,
    independently admitted AOT identity (an RTS placed exactly there), so
    the isolated RTS at 0x00000D0E genuinely accepts a popped value equal
    to it and dispatches to it.
  * NEGATIVE: a second isolated JSR at 0x00000D22 (foldable-target,
    otherwise identical) has a sequential continuation (0x00000D28) with
    no generated executable identity of its own (the image ends exactly
    there). The call itself remains validly admitted and dispatchable per
    its own semantic contract (it still pushes that continuation and jumps
    to the folded callee, unaffected), but that unrepresented continuation
    is proven to NEVER become RTS return-target authority: popping it at
    the same isolated RTS still fails closed, with A7 staying uncommitted
    -- mirroring SEG-007-T208's own
    `t208_tier2_call_shaped_continuation_excluded_when_not_independently_
    retained` non-fabrication invariant, extended (never weakened) from
    Tier-2 call-shaped sites to admitted immutable-ROM AOT call sites.
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

  /* Success: A7 points into work RAM holding the real, genuine call
     continuation (0x00000D06) that this program's own ordinary direct
     JSR $00000D14 actually proved and unioned into ADR-0011's
     whole-program `runtime_return_target_set`. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D0E);
  runtime.a[7] = UINT32_C(0x00FF0080);
  runtime.work_ram[0x80] = UINT8_C(0x00);
  runtime.work_ram[0x81] = UINT8_C(0x00);
  runtime.work_ram[0x82] = UINT8_C(0x0D);
  runtime.work_ram[0x83] = UINT8_C(0x06);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D06));
  assert(runtime.a[7] == UINT32_C(0x00FF0084));

  /* Forced routed-READ failure: A7 points at an unregistered cartridge
     address, so genesis_route_access fails closed before A7 or PC is
     touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D0E);
  runtime.a[7] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[7] == UINT32_C(0x00000000));
  assert(runtime.pc == UINT32_C(0x00000D0E));
  assert(runtime.sr == UINT16_C(0x0000));

  /* Non-member: the popped value (0x00000D10) is itself a real, compiled,
     dispatchable AOT identity (the BCLR body below) -- but it is NOT a
     member of the whole-program continuation authority (no call in this
     program ever proved it as a continuation). Compiled membership alone
     must never authorize an RTS return; this must still fail closed, and
     A7 must stay uncommitted even though the routed read itself
     succeeded. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D0E);
  runtime.a[7] = UINT32_C(0x00FF0090);
  runtime.work_ram[0x90] = UINT8_C(0x00);
  runtime.work_ram[0x91] = UINT8_C(0x00);
  runtime.work_ram[0x92] = UINT8_C(0x0D);
  runtime.work_ram[0x93] = UINT8_C(0x10);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[7] == UINT32_C(0x00FF0090));
  assert(runtime.pc == UINT32_C(0x00000D0E));

  /* POSITIVE represented-continuation case: JSR_FOLDABLE_ISOLATED's own
     continuation (0x00000D20) is itself a genuine, independently admitted,
     dispatchable AOT identity (an RTS placed exactly there), so it is a
     real member of the whole-program return-target authority. A later,
     independent ownerless RTS must accept it and dispatch to it. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D0E);
  runtime.a[7] = UINT32_C(0x00FF00A8);
  runtime.work_ram[0xA8] = UINT8_C(0x00);
  runtime.work_ram[0xA9] = UINT8_C(0x00);
  runtime.work_ram[0xAA] = UINT8_C(0x0D);
  runtime.work_ram[0xAB] = UINT8_C(0x20);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D20));
  assert(runtime.a[7] == UINT32_C(0x00FF00AC));

  /* NEGATIVE unrepresented-continuation case, mirroring SEG-007-T208's own
     `_excluded_when_not_independently_retained` invariant: the SECOND
     isolated JSR (at 0x00000D22)'s own sequential continuation
     (0x00000D28) has no generated executable identity of its own (the
     image ends exactly there) -- it must NOT have been fabricated into
     the whole-program return-target authority merely because its source
     instruction has call semantics. Popping it at the same ownerless RTS
     must still fail closed, with A7 staying uncommitted. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D0E);
  runtime.a[7] = UINT32_C(0x00FF00B8);
  runtime.work_ram[0xB8] = UINT8_C(0x00);
  runtime.work_ram[0xB9] = UINT8_C(0x00);
  runtime.work_ram[0xBA] = UINT8_C(0x0D);
  runtime.work_ram[0xBB] = UINT8_C(0x28);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[7] == UINT32_C(0x00FF00B8));
  assert(runtime.pc == UINT32_C(0x00000D0E));

  /* The second isolated JSR itself (0x00000D22) remains validly admitted
     and dispatchable per its own semantic contract even though its own
     continuation is unrepresented: it still pushes that continuation
     value and jumps to the folded callee, exactly like the first isolated
     JSR. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D22);
  runtime.a[7] = UINT32_C(0x00FF00C0);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D14));
  assert(runtime.a[7] == UINT32_C(0x00FF00BC));
  assert(runtime.work_ram[0xBC] == UINT8_C(0x00));
  assert(runtime.work_ram[0xBD] == UINT8_C(0x00));
  assert(runtime.work_ram[0xBE] == UINT8_C(0x0D));
  assert(runtime.work_ram[0xBF] == UINT8_C(0x28));

  /* SEG-007-T246 seventh-iteration admission: BCLR #3,(A5). Success: A5
     points into work RAM whose byte has bit 3 set; BCLR clears exactly
     that bit and sets Z from the ORIGINAL (pre-clear) bit value (clear,
     since bit 3 was originally set). */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D10);
  runtime.a[5] = UINT32_C(0x00FF00A0);
  runtime.work_ram[0xA0] = UINT8_C(0x0F);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D14));
  assert(runtime.work_ram[0xA0] == UINT8_C(0x07));
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[5] == UINT32_C(0x00FF00A0));

  /* BCLR success, original bit already clear: Z is set (bit4 -> mask
     0x10, original value 0x00 has bit 3 clear). */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D10);
  runtime.a[5] = UINT32_C(0x00FF00B0);
  runtime.work_ram[0xB0] = UINT8_C(0x00);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D14));
  assert(runtime.work_ram[0xB0] == UINT8_C(0x00));
  assert(runtime.sr == UINT16_C(0x0004));

  /* Forced routed-access failure: A5 points at an unregistered cartridge
     address, so genesis_route_access fails closed on the read before SR
     or any memory content is touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D10);
  runtime.a[5] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.sr == UINT16_C(0x0000));
  assert(runtime.a[5] == UINT32_C(0x00000000));
  assert(runtime.pc == UINT32_C(0x00000D10));

  /* Eighth-iteration admission: the foldable-target JMP at 0x00000D16. A
     pure constant PC assignment -- no precondition, no memory access, no
     register mutation of any kind. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D16);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D0E));
  assert(runtime.sr == UINT16_C(0x0000));

  /* Ninth-iteration admission: the isolated foldable-target JSR at
     0x00000D1A (no CFG/frame ownership of its own). Success: A7 points into
     work RAM. The routed stack push writes exactly this candidate's own
     provenance-derived continuation (0x00000D1A + 6 = 0x00000D20, never an
     external/whole-program fact), A7 decrements by exactly 4, and PC
     dispatches to the folded constant callee target (0x00000D14). */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D1A);
  runtime.a[7] = UINT32_C(0x00FF0080);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000D14));
  assert(runtime.a[7] == UINT32_C(0x00FF007C));
  assert(runtime.work_ram[0x7C] == UINT8_C(0x00));
  assert(runtime.work_ram[0x7D] == UINT8_C(0x00));
  assert(runtime.work_ram[0x7E] == UINT8_C(0x0D));
  assert(runtime.work_ram[0x7F] == UINT8_C(0x20));

  /* Forced routed-WRITE failure: A7 points at an unregistered cartridge
     address, so genesis_route_access fails closed before A7 or PC is
     touched. */
  runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000D1A);
  runtime.a[7] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.a[7] == UINT32_C(0x00000000));
  assert(runtime.pc == UINT32_C(0x00000D1A));

  return 0;
}
'''


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    generated = subprocess.run(
        [emitter, "--emit-immutable-rom-aot-return-from-subroutine-and-bit-clear"],
        text=True, capture_output=True,
    )
    assert generated.returncode == 0, generated.stderr
    assert not generated.stdout.startswith("/* translation rejected:")
    assert "genesis_aot_00000D0E" in generated.stdout
    assert "genesis_aot_00000D10" in generated.stdout
    assert "genesis_block_00000D14" in generated.stdout
    assert "genesis_aot_00000D14" not in generated.stdout
    assert "genesis_aot_00000D16" in generated.stdout
    assert "genesis_aot_00000D1A" in generated.stdout
    assert "genesis_aot_00000D20" in generated.stdout
    assert "genesis_aot_00000D22" in generated.stdout
    assert "genesis_route_access(runtime," in generated.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated.stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "return-from-subroutine-and-bit-clear-aot"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr

    print("genesis_immutable_rom_aot_return_from_subroutine_and_bit_clear_generated_test: OK")


if __name__ == "__main__":
    main()
