#!/usr/bin/env python3
"""Strict-C11 generated-native no-hints immutable-ROM AOT proof for the
remaining register-relative control EAs (SEG-021-T034).

Whole-image AOT enumeration (no hints, no static CFG reachability) admits
JMP/JSR with d16(An), (d8,An,Xn) (word/long, Dn/An index) and the long-index
(d8,PC,Xn) variant. The shared dynamic-indirect lowering computes the runtime
EA via the existing m68k_emit_runtime_ea_address helper, checks membership in
the final compiled-entry table, and otherwise fails closed atomically. This
executes the generated C:
  - two represented targets reached from different base/index values,
    including a negative word index (upper Dn bits ignored) and a negative
    long address-register index;
  - an unrepresented target failing closed with registers/memory/PC unchanged;
  - JSR pushes the correct return address and decrements A7 once;
  - the long-index PC form uses the whole 32-bit index (a word-truncating
    implementation would reach a represented target instead of stopping);
  - JSR (d8,A7,Xn.L) computes its EA from the pre-push A7 (MC68000 order);
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

static void assert_stop_atomic(GenesisRuntime *runtime, uint32_t pc, uint32_t a7) {
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(transfer.stop.stop_class == GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET);
  assert(runtime->pc == pc && runtime->a[7] == a7 && runtime->scheduler.master_ticks == 0U);
  assert_ram_clear(runtime);
}

int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;

  /* JMP (4,A3,D1.W): negative word index, upper D1 bits ignored -> 0x1000. */
  runtime = at(UINT32_C(0x0000100A));
  runtime.a[3] = UINT32_C(0x00001000);
  runtime.d[1] = UINT32_C(0x1234FFFC);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001000));
  assert(runtime.d[1] == UINT32_C(0x1234FFFC) && runtime.a[3] == UINT32_C(0x00001000));

  /* JMP (4,A3,D1.W): a different base/index pair -> a different entry (0x1012). */
  runtime = at(UINT32_C(0x0000100A));
  runtime.a[3] = UINT32_C(0x0000100A);
  runtime.d[1] = UINT32_C(0x00000004);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001012));

  /* JMP (4,A3,D1.W): unrepresented target. */
  runtime = at(UINT32_C(0x0000100A));
  runtime.a[3] = UINT32_C(0x00002000);
  assert_stop_atomic(&runtime, UINT32_C(0x0000100A), 0U);
  assert(runtime.a[3] == UINT32_C(0x00002000));

  /* JSR (-2,A4,A2.L): positive long index -> 0x1008; pushes 0x1012. */
  runtime = at(UINT32_C(0x0000100E));
  runtime.a[4] = UINT32_C(0x00001000);
  runtime.a[2] = UINT32_C(0x0000000A);
  runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001008));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC));
  assert(runtime.work_ram[0x00FC] == 0x00U && runtime.work_ram[0x00FD] == 0x00U &&
         runtime.work_ram[0x00FE] == 0x10U && runtime.work_ram[0x00FF] == 0x12U);

  /* JSR (-2,A4,A2.L): negative long index -> 0x1012. */
  runtime = at(UINT32_C(0x0000100E));
  runtime.a[4] = UINT32_C(0x00001024);
  runtime.a[2] = UINT32_C(0xFFFFFFF0);
  runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001012));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC));

  /* JSR (-2,A4,A2.L): a long index whose low word alone would name 0x1008 is unrepresented. */
  runtime = at(UINT32_C(0x0000100E));
  runtime.a[4] = UINT32_C(0x00001000);
  runtime.a[2] = UINT32_C(0x0001000A);
  runtime.a[7] = UINT32_C(0x00FF0100);
  assert_stop_atomic(&runtime, UINT32_C(0x0000100E), UINT32_C(0x00FF0100));

  /* JMP (16,A5) -> 0x1008. */
  runtime = at(UINT32_C(0x00001014));
  runtime.a[5] = UINT32_C(0x00000FF8);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001008));

  /* JSR (-8,A6) -> 0x1018; pushes 0x101C. */
  runtime = at(UINT32_C(0x00001018));
  runtime.a[6] = UINT32_C(0x00001020);
  runtime.a[7] = UINT32_C(0x00FF0100);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001018));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC) && runtime.work_ram[0x00FF] == 0x1CU);

  /* JSR (-8,A6): unrepresented target, unchanged A7/memory/PC. */
  runtime = at(UINT32_C(0x00001018));
  runtime.a[6] = UINT32_C(0x00003000);
  runtime.a[7] = UINT32_C(0x00FF0100);
  assert_stop_atomic(&runtime, UINT32_C(0x00001018), UINT32_C(0x00FF0100));

  /* JMP (0,PC,D2.L): 0x101E + 2 -> 0x1020; a long index 0x00010002 must not truncate to word. */
  runtime = at(UINT32_C(0x0000101C));
  runtime.d[2] = UINT32_C(0x00000002);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001020));
  runtime = at(UINT32_C(0x0000101C));
  runtime.d[2] = UINT32_C(0x00010002);
  assert_stop_atomic(&runtime, UINT32_C(0x0000101C), 0U);

  /* JSR (0,A7,D0.L): EA from the pre-push A7 (MC68000 order): 0x00FF0100 + 0xFF010F26 = 0x1026. A post-push
     EA would name 0x1022 (also a compiled entry), so the exact next_pc proves the order. */
  runtime = at(UINT32_C(0x00001022));
  runtime.a[7] = UINT32_C(0x00FF0100);
  runtime.d[0] = UINT32_C(0xFF010F26);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x00001026));
  assert(runtime.a[7] == UINT32_C(0x00FF00FC) && runtime.work_ram[0x00FE] == 0x10U &&
         runtime.work_ram[0x00FF] == 0x26U);
  return 0;
}
'''


def body_of(source, address):
    return source.split(f"genesis_aot_{address}(GenesisRuntime *runtime) {{", 1)[1].split("\n}\n", 1)[0]


def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-jmp-an-relative-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    source = result.stdout
    for address in ("0000100A", "0000100E", "00001014", "00001018", "0000101C", "00001022"):
        body = body_of(source, address)
        assert body.count("genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL") == 1, address
        assert "GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET" in body, address
    for address in ("0000100A", "00001014", "0000101C"):
        assert "genesis_route_access" not in body_of(source, address)  # JMP: no memory access at all
    for address in ("0000100E", "00001018", "00001022"):
        jsr = body_of(source, address)
        # The only routed access in JSR is the continuation push, after the membership check.
        assert jsr.count("genesis_route_access(") == 1, address
        push = jsr.index("genesis_route_access(")
        assert "GENESIS_ACCESS_WRITE, &m68k_continuation" in jsr[push:jsr.index("\n", push)]
        assert jsr.index("genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL") < push
        assert jsr.index("m68k_indirect_ea =") < push
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(source)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "jmp-an-relative-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    again = subprocess.run([emitter, "--emit-jmp-an-relative-aot"], text=True, capture_output=True)
    assert again.returncode == 0 and again.stdout == source
    print("genesis_immutable_rom_aot_jmp_an_relative_generated_test: OK")


if __name__ == "__main__":
    main()
