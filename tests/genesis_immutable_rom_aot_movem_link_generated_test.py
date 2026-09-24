#!/usr/bin/env python3
"""SEG-021-T016: strict-C11 generated-native proof of the MOVEM/LINK/UNLK immutable-ROM AOT roots.

The emitter (`m68k_pipeline_tests --emit-movem-link-aot`) produces the complete generated program for a synthetic ROM whose
instructions are reachable only as isolated AOT identities. The harness executes each root through the real Genesis runtime and
checks hand-derived results (Motorola M68000 Family Programmer's Reference Manual: predecrement stores highest register at
the highest address, one final An commit, word loads sign-extended, LINK/UNLK stack effects, published cycle counts) and the
routed-stop atomicity contract (an unmapped store/push/pop changes no An/A7/PC/memory). CPU semantics of the shared MOVEM
owner are covered by the existing SEG-021-T012 Musashi conformance; this test proves admission and routed AOT emission.
"""
import pathlib
import subprocess
import sys
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
static uint64_t cycles_of(const GenesisRuntime *runtime) { return runtime->scheduler.master_ticks / GENESIS_M68K_CYCLE_MASTER_TICKS; }
static GenesisRuntime fresh(uint32_t pc, uint16_t sr) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  runtime.pc = pc; runtime.sr = sr;
  return runtime;
}
static void step(GenesisRuntime *runtime, uint32_t expected_next, uint64_t expected_cycles) {
  const uint64_t before = cycles_of(runtime);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  if ((transfer.kind != GENESIS_CONTINUE_AT_PC && transfer.kind != GENESIS_STOP) || runtime->pc != expected_next ||
      cycles_of(runtime) - before != expected_cycles)
    fprintf(stderr, "step to %04X: kind=%d pc=%04X cycles=%llu expected=%llu\n", (unsigned)expected_next, (int)transfer.kind,
            (unsigned)runtime->pc, (unsigned long long)(cycles_of(runtime) - before), (unsigned long long)expected_cycles);
  assert(runtime->pc == expected_next && cycles_of(runtime) - before == expected_cycles);
}
static void expect_stop(GenesisRuntime *runtime, uint32_t pc) {
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  assert(transfer.kind == GENESIS_STOP && runtime->pc == pc);
}
int main(void) {
  GenesisRuntime runtime;
  /* MOVEM.L D0-D1,-(A7): D1 at A7-4, D0 at A7-8, one A7 commit; CCR untouched; 8+8*2 = 24 cycles. */
  runtime = fresh(0x0F08, 0x2715);
  runtime.a[7] = 0x00FF0400; runtime.d[0] = 0x11223344; runtime.d[1] = 0x55667788;
  step(&runtime, 0x0F0C, 24);
  assert(runtime.a[7] == 0x00FF03F8 && runtime.sr == 0x2715);
  assert(memcmp(runtime.work_ram + 0x3F8, "\x11\x22\x33\x44\x55\x66\x77\x88", 8) == 0);
  /* MOVEM.L (A7)+,D0-D1: ascending load, A7 committed once; 12+8*2 = 28 cycles. */
  runtime = fresh(0x0F0C, 0x2700);
  runtime.a[7] = 0x00FF03F8; memcpy(runtime.work_ram + 0x3F8, "\xAA\xBB\xCC\xDD\x01\x02\x03\x04", 8);
  step(&runtime, 0x0F10, 28);
  assert(runtime.d[0] == 0xAABBCCDD && runtime.d[1] == 0x01020304 && runtime.a[7] == 0x00FF0400 && runtime.sr == 0x2700);
  /* MOVEM.W (A0),D2-D3: words sign-extended to 32 bits, A0 unchanged; 12+4*2 = 20 cycles. */
  runtime = fresh(0x0F10, 0x2700);
  runtime.a[0] = 0x00FF0500; memcpy(runtime.work_ram + 0x500, "\x80\x01\x7F\xFE", 4);
  step(&runtime, 0x0F14, 20);
  assert(runtime.d[2] == 0xFFFF8001 && runtime.d[3] == 0x00007FFE && runtime.a[0] == 0x00FF0500);
  /* MOVEM.L D0-D1,(16,A0): ascending stores, A0 unchanged; 12+8*2 = 28 cycles. */
  runtime = fresh(0x0F14, 0x2700);
  runtime.a[0] = 0x00FF0600; runtime.d[0] = 0x0A0B0C0D; runtime.d[1] = 0x0E0F1011;
  step(&runtime, 0x0F1A, 28);
  assert(memcmp(runtime.work_ram + 0x610, "\x0A\x0B\x0C\x0D\x0E\x0F\x10\x11", 8) == 0 && runtime.a[0] == 0x00FF0600);
  /* MOVEM.W D0-D1,-(A7): low words, 8+4*2 = 16 cycles. */
  runtime = fresh(0x0F1A, 0x2700);
  runtime.a[7] = 0x00FF0400; runtime.d[0] = 0xAAAA1234; runtime.d[1] = 0xBBBB5678;
  step(&runtime, 0x0F1E, 16);
  assert(runtime.a[7] == 0x00FF03FC && memcmp(runtime.work_ram + 0x3FC, "\x12\x34\x56\x78", 4) == 0);
  /* LINK A6,#-8: push old A6, A6 = new A7, A7 += -8; 16 cycles; CCR untouched. */
  runtime = fresh(0x0F1E, 0x2715);
  runtime.a[7] = 0x00FF0700; runtime.a[6] = 0xAAAA5555;
  step(&runtime, 0x0F22, 16);
  assert(runtime.a[6] == 0x00FF06FC && runtime.a[7] == 0x00FF06F4 && runtime.sr == 0x2715);
  assert(memcmp(runtime.work_ram + 0x6FC, "\xAA\xAA\x55\x55", 4) == 0);
  /* UNLK A6: A7 = A6 + 4, A6 = popped long; 12 cycles. */
  runtime = fresh(0x0F22, 0x2700);
  runtime.a[6] = 0x00FF06FC; runtime.a[7] = 0x00FF0000; memcpy(runtime.work_ram + 0x6FC, "\x12\x34\x56\x78", 4);
  step(&runtime, 0x0F24, 12);
  assert(runtime.a[6] == 0x12345678 && runtime.a[7] == 0x00FF0700);
  /* MOVEM.L $FF0800.L,D4: absolute long load, routed; 20+8 = 28 cycles. */
  runtime = fresh(0x0F24, 0x2700);
  memcpy(runtime.work_ram + 0x800, "\xDE\xAD\xBE\xEF", 4);
  step(&runtime, 0x0F2C, 28);
  assert(runtime.d[4] == 0xDEADBEEF);

  /* Routed stops are atomic: no An/A7/PC change and no memory write. */
  runtime = fresh(0x0F08, 0x2715); runtime.a[7] = 0x00500004; runtime.d[0] = 1; runtime.d[1] = 2;
  expect_stop(&runtime, 0x0F08); assert(runtime.a[7] == 0x00500004 && runtime.sr == 0x2715);
  runtime = fresh(0x0F0C, 0x2715); runtime.a[7] = 0x00500000; runtime.d[0] = 7; runtime.d[1] = 8;
  expect_stop(&runtime, 0x0F0C); assert(runtime.a[7] == 0x00500000 && runtime.d[0] == 7 && runtime.d[1] == 8);
  runtime = fresh(0x0F14, 0x2715); runtime.a[0] = 0x00500000;
  expect_stop(&runtime, 0x0F14); assert(runtime.a[0] == 0x00500000);
  runtime = fresh(0x0F1A, 0x2715); runtime.a[7] = 0x00500004;
  expect_stop(&runtime, 0x0F1A); assert(runtime.a[7] == 0x00500004);
  runtime = fresh(0x0F1E, 0x2715); runtime.a[7] = 0x00500004; runtime.a[6] = 0xCAFEBABE;
  expect_stop(&runtime, 0x0F1E); assert(runtime.a[7] == 0x00500004 && runtime.a[6] == 0xCAFEBABE);
  runtime = fresh(0x0F22, 0x2715); runtime.a[6] = 0x00500000; runtime.a[7] = 0x00FF0100;
  expect_stop(&runtime, 0x0F22); assert(runtime.a[6] == 0x00500000 && runtime.a[7] == 0x00FF0100);
  return 0;
}
'''


def root_text(source, address):
    start = source.index("static GenesisControlTransfer genesis_aot_%s(" % address)
    end = source.find("static GenesisControlTransfer ", start + 10)
    return source[start:end if end != -1 else len(source)]


def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-movem-link-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    for address in ("00000F08", "00000F0C", "00000F10", "00000F14", "00000F1A", "00000F1E", "00000F22", "00000F24"):
        assert "genesis_aot_" + address in result.stdout, address
    # Deferred commit: the live A7 is assigned only after every routed store of the predecrement MOVEM/LINK.
    for address, register in (("00000F08", "runtime->a[7] ="), ("00000F1E", "runtime->a[6] =")):
        text = root_text(result.stdout, address)
        assert text.index("genesis_route_access(") < text.index(register), address
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(result.stdout)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "movem-link-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr + ran.stdout
    print("genesis_immutable_rom_aot_movem_link_generated_test: OK")


if __name__ == "__main__":
    main()
