#!/usr/bin/env python3
"""Strict-C11 generated-native no-hints immutable-ROM AOT proof for the
runtime-owned register-indirect control transfer (`JMP (An)` / `JSR (An)`).

SEG-021-T033: whole-image AOT enumeration (no external hints, no static CFG
reachability) admits `JMP (A3)` and `JSR (A4)` through the existing shared
dynamic-indirect lowering: runtime target = architectural An, membership
checked against the one final compiled-entry table, fail-closed
unresolved-indirect-target stop otherwise. This executes the generated C:
  - the NOP whose fixed fallthrough is the JMP identity continues into it
    (previously a known_but_unemitted_target stop);
  - two different runtime A3 values reach two different compiled targets
    (the target is runtime-owned, never folded);
  - an A3 value naming an address outside the compiled set (an extension
    word, never a candidate) fails closed failure-atomically;
  - JMP (A7) is admitted too: its target is the runtime A7 value;
  - JSR (A4): represented target pushes the correct return address and
    decrements A7 once; an unrepresented target and an unwritable stack both
    fail closed with A7, memory and PC unchanged;
  - dispatching the excluded JMP d16(An) identity executes no body;
  - no runtime opcode fetch/decode in the generated bodies.
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

static GenesisRuntime at(uint32_t pc) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  runtime.pc = pc;
  return runtime;
}

static void assert_ram_clear(const GenesisRuntime *runtime) {
  unsigned i;
  for (i = 0U; i < sizeof(runtime->work_ram); ++i) assert(runtime->work_ram[i] == 0U);
}

int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;
  unsigned i;

  /* The fallthrough into JMP (A3) is represented. */
  runtime = at(UINT32_C(0x00001008));
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x0000100A));

  /* JMP (A3), represented target #1: the reset block entry. */
  runtime = at(UINT32_C(0x0000100A));
  runtime.a[3] = UINT32_C(0x00001000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001000));
  assert(runtime.pc == UINT32_C(0x00001000) && runtime.scheduler.master_ticks != 0U);

  /* JMP (A3), represented target #2: a different runtime A3, a different AOT entry. */
  runtime = at(UINT32_C(0x0000100A));
  runtime.a[3] = UINT32_C(0x0000100E);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x0000100E));
  assert(runtime.pc == UINT32_C(0x0000100E));

  /* JMP (A3), unrepresented target: the trailing extension word is never compiled. */
  runtime = at(UINT32_C(0x0000100A));
  runtime.a[3] = UINT32_C(0x00001014);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET);
  assert(runtime.pc == UINT32_C(0x0000100A) && runtime.scheduler.master_ticks == 0U);
  for (i = 0U; i < 8U; ++i) assert(runtime.d[i] == 0U);
  for (i = 0U; i < 8U; ++i) assert(runtime.a[i] == (i == 3U ? UINT32_C(0x00001014) : 0U));

  /* JSR (A4), represented target: one push of the continuation, A7 - 4. */
  runtime = at(UINT32_C(0x0000100C));
  runtime.a[4] = UINT32_C(0x00001008);
  runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001008));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC) && runtime.a[4] == UINT32_C(0x00001008));
  assert(runtime.work_ram[0x00FC] == 0x00U && runtime.work_ram[0x00FD] == 0x00U &&
         runtime.work_ram[0x00FE] == 0x10U && runtime.work_ram[0x00FF] == 0x0EU);

  /* JSR (A4), unrepresented target: no push, no A7 change, PC unchanged. */
  runtime = at(UINT32_C(0x0000100C));
  runtime.a[4] = UINT32_C(0x00001014);
  runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET);
  assert(runtime.pc == UINT32_C(0x0000100C) && runtime.a[7] == UINT32_C(0x00FF0100));
  assert_ram_clear(&runtime);

  /* JSR (A4), represented target but unwritable stack (ROM window): fails closed atomically. */
  runtime = at(UINT32_C(0x0000100C));
  runtime.a[4] = UINT32_C(0x00001008);
  runtime.a[7] = UINT32_C(0x00000100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.pc == UINT32_C(0x0000100C) && runtime.a[7] == UINT32_C(0x00000100));
  assert_ram_clear(&runtime);

  /* JMP (A7): the runtime A7 value is the target (A7 itself is not modified). */
  runtime = at(UINT32_C(0x00001010));
  runtime.a[7] = UINT32_C(0x00001000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001000));
  assert(runtime.a[7] == UINT32_C(0x00001000));

  /* The excluded JMP d16(An) identity has no body: dispatch fails closed without executing it
     (A3 + 16 would name a compiled entry, so a stop proves the body never ran). */
  runtime = at(UINT32_C(0x00001012));
  runtime.a[3] = UINT32_C(0x00000FF0);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == UINT32_C(0x00001012));
  return 0;
}
'''


def body_of(source, address):
    return source.split(f"genesis_aot_{address}(GenesisRuntime *runtime) {{", 1)[1].split("\n}\n", 1)[0]


def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-jmp-an-indirect-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    source = result.stdout
    for address in ("0000100A", "0000100C", "00001010"):
        assert f"genesis_aot_{address}" in source, address
    for excluded in ("00001012", "00001014"):
        assert f"genesis_aot_{excluded}(" not in source, excluded
    jmp = body_of(source, "0000100A")
    assert "m68k_indirect_ea = runtime->a[3];" in jmp
    assert jmp.count("genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL") == 1
    assert "genesis_route_access" not in jmp  # no opcode fetch, no memory access at all
    jsr = body_of(source, "0000100C")
    assert "m68k_indirect_ea = runtime->a[4];" in jsr
    # The only routed access in JSR is the continuation push (a write), never an opcode fetch,
    # and the membership check precedes it.
    assert jsr.count("genesis_route_access(") == 1
    push = jsr.index("genesis_route_access(")
    assert "GENESIS_ACCESS_WRITE, &m68k_continuation" in jsr[push:jsr.index("\n", push)]
    assert jsr.index("genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL") < push
    for body in (jmp, jsr):
        assert "GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET" in body
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(source)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "jmp-an-indirect-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    # Deterministic repeated generation.
    again = subprocess.run([emitter, "--emit-jmp-an-indirect-aot"], text=True, capture_output=True)
    assert again.returncode == 0 and again.stdout == source
    print("genesis_immutable_rom_aot_jmp_an_indirect_generated_test: OK")


if __name__ == "__main__":
    main()
