#!/usr/bin/env python3
"""Strict-C11 generated-native immutable-ROM AOT proof for the runtime-owned
brief PC-indexed indirect JMP (`(d8,PC,D0.W)`).

SEG-021-T027: the previously rejected PC-indexed dynamic-jump immutable-AOT
identity is now admitted (`m68k_operation_is_immutable_rom_aot_safe` /
`m68k_operation_is_runtime_owned_indirect_jump`), reusing the exact existing
`jump_general` dynamic-indirect target-computation and membership-check owner
(`m68k_indirect_target_member` against the caller-supplied final
compiled-address authority) rather than a second target-proof mechanism.

This proves, at minimum:
  - a represented target: two distinct runtime index values -- including a
    negative word-index value -- reach and execute the intended compiled
    target, correctly retired/timed;
  - an unrepresented target: the same generated identity with a different
    runtime register state computes a target absent from the compiled-address
    authority, executes no body at that address, and returns the existing
    typed fail-closed dynamic-control diagnostic with every architectural
    register (in particular PC itself) completely unmodified;
  - no runtime opcode decode: the generated body only ever compares one
    computed 32-bit integer against a compiled-in address table.
"""
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

static GenesisRuntime fresh_runtime(void) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  runtime.pc = UINT32_C(0x00000F08);
  return runtime;
}

int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;

  /* Represented target #1: D0 == -10 (0xFFFFFFF6) -> the block entry
     0x00000F00, reached through the exact shared runtime-EA/membership
     lowering, correctly retired (CONTINUE_AT_PC, no residual stop). */
  runtime = fresh_runtime();
  runtime.d[0] = UINT32_C(0xFFFFFFF6);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000F00));
  assert(runtime.pc == UINT32_C(0x00000F00));

  /* Represented target #2: D0 == -2 (0xFFFFFFFE) -> the JMP's own AOT
     address 0x00000F08 (self), a distinct index value from target #1 and
     also negative -- both proving the sign-extended word index, not just a
     zero/positive case. */
  runtime = fresh_runtime();
  runtime.d[0] = UINT32_C(0xFFFFFFFE);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.next_pc == UINT32_C(0x00000F08));
  assert(runtime.pc == UINT32_C(0x00000F08));

  /* Unrepresented target: D0 == 0 -> the extension word's own address
     0x00000F0A, which is never independently compiled. The generated body
     must fail closed through the existing typed dynamic-control diagnostic
     -- never execute a body at that address -- with PC (and every other
     register: this operation writes none besides PC) completely
     unmodified, exactly like every other admitted AOT candidate's
     failure-atomic contract. */
  runtime = fresh_runtime();
  runtime.d[0] = UINT32_C(0x00000000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET);
  assert(runtime.pc == UINT32_C(0x00000F08));
  {
    unsigned i;
    for (i = 0U; i < 8U; ++i) assert(runtime.d[i] == (i == 0U ? UINT32_C(0x00000000) : UINT32_C(0)));
    for (i = 0U; i < 8U; ++i) assert(runtime.a[i] == UINT32_C(0));
  }

  return 0;
}
'''

def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-jmp-pc-indexed-word-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert "genesis_aot_00000F08" in result.stdout
    assert "genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL" in result.stdout
    body = result.stdout.split("genesis_aot_00000F08(GenesisRuntime *runtime) {", 1)[1].split("\n}\n", 1)[0]
    # No runtime opcode fetch/decode: the body only ever computes one 32-bit
    # integer and compares it against the compiled-in candidate array.
    assert "genesis_route_access" not in body
    # SEG-022-T006: membership queries the one final compiled-entry table; no site-local set copy.
    assert body.count("genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL") == 1
    assert "m68k_indirect_target_member(" not in body and "m68k_indirect_targets_" not in body
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(result.stdout)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "jmp-pc-indexed-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis_immutable_rom_aot_jmp_pc_indexed_generated_test: OK")

if __name__ == "__main__":
    main()
