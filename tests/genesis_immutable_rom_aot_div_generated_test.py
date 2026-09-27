#!/usr/bin/env python3
"""Strict-C11 generated-native no-hints immutable-ROM AOT proof for DIVS.W/DIVU.W (SEG-021-T035).

Whole-image AOT enumeration (no hints) admits DIVS.W/DIVU.W identities that static discovery never
reaches; their bodies are the existing routed DIV lowering (ADR-0037 vector-5 raise, SEG-021-T010
deferred auto-update commit). This executes the generated C:
  - signed/unsigned quotient and remainder packing, N/Z/V/C flags (X preserved);
  - signed overflow leaves Dn unwritten and sets V;
  - divide-by-zero enters the build-time vector-5 handler with a six-byte frame whose PC is the next
    instruction; with no installed handler it fails closed with Dn/A7/memory unchanged;
  - (An)+ source: success commits the post-increment; a failed routed read changes nothing;
  - -(An) source divisor zero: the predecrement is committed before the vector-5 entry (MC68000 order);
  - deterministic regeneration; no runtime opcode fetch.
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
  runtime.sr = UINT16_C(0x2710); /* supervisor, IPL 7, X set */
  runtime.a[7] = UINT32_C(0x00FF8000);
  return runtime;
}

int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;
  unsigned i;

  /* DIVS.W D2,D3: 100 / 7 = 14 r 2. */
  runtime = at(UINT32_C(0x110));
  runtime.d[3] = 100U; runtime.d[2] = UINT32_C(0xABCD0007); /* upper divisor bits ignored */
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x112));
  assert(runtime.d[3] == UINT32_C(0x0002000E));
  assert((runtime.sr & 0x1FU) == 0x10U); /* X kept, NZVC clear */
  assert(runtime.scheduler.master_ticks != 0U);

  /* DIVS.W D2,D3: -100 / 7 = -14 r -2 (remainder takes the dividend's sign), N set. */
  runtime = at(UINT32_C(0x110));
  runtime.d[3] = (uint32_t)-100; runtime.d[2] = 7U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(runtime.d[3] == UINT32_C(0xFFFEFFF2));
  assert((runtime.sr & 0x0FU) == 0x08U);

  /* DIVS.W overflow: quotient does not fit 16 bits -> V set, D3 unwritten. */
  runtime = at(UINT32_C(0x110));
  runtime.d[3] = UINT32_C(0x00100000); runtime.d[2] = 1U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x112));
  assert(runtime.d[3] == UINT32_C(0x00100000));
  assert((runtime.sr & 0x02U) == 0x02U && (runtime.sr & 0x01U) == 0U);

  /* DIVU.W #7,D4: 0x00010000 / 7 = 9362 r 2 (unsigned 32/16); Z clear. */
  runtime = at(UINT32_C(0x112));
  runtime.d[4] = UINT32_C(0x00010000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x116));
  assert(runtime.d[4] == ((UINT32_C(2) << 16) | UINT32_C(9362)));

  /* DIVU.W #7,D4: zero dividend -> Z set. */
  runtime = at(UINT32_C(0x112));
  runtime.d[4] = 0U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(runtime.d[4] == 0U && (runtime.sr & 0x0FU) == 0x04U);

  /* DIVS.W D2,D3 by zero with the vector-5 handler installed: frame {SR, PC = 0x112}, enter 0x180. */
  runtime = at(UINT32_C(0x110));
  runtime.divide_by_zero_handler_present = 1; runtime.divide_by_zero_handler_entry = UINT32_C(0x180);
  runtime.d[3] = UINT32_C(0x12345678); runtime.d[2] = UINT32_C(0xFFFF0000);
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x180));
  assert(runtime.d[3] == UINT32_C(0x12345678));
  assert(runtime.a[7] == UINT32_C(0x00FF7FFA));
  assert(runtime.work_ram[0x7FFA] == 0x27U && runtime.work_ram[0x7FFB] == 0x10U);
  assert(runtime.work_ram[0x7FFC] == 0x00U && runtime.work_ram[0x7FFD] == 0x00U &&
         runtime.work_ram[0x7FFE] == 0x01U && runtime.work_ram[0x7FFF] == 0x12U);

  /* The same divide-by-zero with no installed handler fails closed: nothing changes. */
  runtime = at(UINT32_C(0x110));
  runtime.d[3] = UINT32_C(0x12345678); runtime.d[2] = 0U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.pc == UINT32_C(0x110) && runtime.d[3] == UINT32_C(0x12345678));
  assert(runtime.a[7] == UINT32_C(0x00FF8000));
  for (i = 0U; i < sizeof(runtime.work_ram); ++i) assert(runtime.work_ram[i] == 0U);

  /* DIVS.W (A0)+,D5 from work RAM: 50 / 5 = 10, A0 += 2. */
  runtime = at(UINT32_C(0x116));
  runtime.a[0] = UINT32_C(0x00FF0010); runtime.work_ram[0x10] = 0x00U; runtime.work_ram[0x11] = 0x05U;
  runtime.d[5] = 50U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x118));
  assert(runtime.d[5] == 10U && runtime.a[0] == UINT32_C(0x00FF0012));

  /* DIVS.W (A0)+,D5 with an unroutable source: stop, A0/D5 unchanged. */
  runtime = at(UINT32_C(0x116));
  runtime.a[0] = UINT32_C(0x00B00000); runtime.d[5] = 50U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP);
  assert(runtime.pc == UINT32_C(0x116) && runtime.a[0] == UINT32_C(0x00B00000) && runtime.d[5] == 50U);

  /* DIVU.W -(A1),D6 by a zero divisor: A1 is predecremented, then vector 5 (frame PC = 0x11A). */
  runtime = at(UINT32_C(0x118));
  runtime.divide_by_zero_handler_present = 1; runtime.divide_by_zero_handler_entry = UINT32_C(0x180);
  runtime.a[1] = UINT32_C(0x00FF0022); runtime.d[6] = 99U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == UINT32_C(0x180));
  assert(runtime.a[1] == UINT32_C(0x00FF0020) && runtime.d[6] == 99U);
  assert(runtime.work_ram[0x7FFF] == 0x1AU);
  return 0;
}
'''


def body_of(source, address):
    return source.split(f"genesis_aot_{address}(GenesisRuntime *runtime) {{", 1)[1].split("\n}\n", 1)[0]


def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-div-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    source = result.stdout
    for address in ("00000110", "00000112", "00000116", "00000118"):
        body = body_of(source, address)
        assert body.count("genesis_raise_divide_by_zero(") == 1, address
        assert "GENESIS_ACCESS_FETCH" not in body, address
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(source)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "div-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    again = subprocess.run([emitter, "--emit-div-aot"], text=True, capture_output=True)
    assert again.returncode == 0 and again.stdout == source
    print("genesis_immutable_rom_aot_div_generated_test: OK")


if __name__ == "__main__":
    main()
