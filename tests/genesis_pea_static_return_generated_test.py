#!/usr/bin/env python3
"""SEG-021-T041 / ADR 0049: strict-C11 compile/execute proof of the statically-foldable PEA return authority.

Project-authored synthetic image (see `pea_static_return_fixture` in m68k_pipeline_test.cpp):
  0xF10 PEA (d16,PC)->0xF1A ; 0xF14 BRA.S -> 0xF1C ; 0xF1C RTS   (manual call, PC-relative pushed address)
  0xF1E PEA $F2E.L ; 0xF24 BRA.S -> 0xF1C                          (manual call, absolute pushed address)
  0xF30 PEA $1000.W ; 0xF34 BRA.S -> 0xF1C                         (pushed value is not a compiled identity)

Proves: a RTS popping a statically-foldable PEA-pushed compiled address returns to it with SP net unchanged; a
pushed non-compiled value and a compiled address that neither a JSR continuation nor a PEA names both still fail
closed with the RTS uncommitted; a genuine JSR continuation still returns; generated output is deterministic.
"""

import pathlib
import subprocess
import sys
import tempfile

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"

static void put32(GenesisRuntime *r, uint32_t off, uint32_t v) {
  r->work_ram[off] = (uint8_t)(v >> 24); r->work_ram[off + 1] = (uint8_t)(v >> 16);
  r->work_ram[off + 2] = (uint8_t)(v >> 8); r->work_ram[off + 3] = (uint8_t)v;
}

static GenesisControlTransfer run_manual_call(GenesisRuntime *r, uint32_t start, uint32_t *final_pc) {
  GenesisControlTransfer t = {0};
  unsigned i;
  *r = (GenesisRuntime){0};
  r->pc = start;
  r->a[7] = UINT32_C(0x00FF0100);
  for (i = 0; i < 3; ++i) {  /* PEA, BRA.S, RTS */
    t = genesis_bridge_dispatch(r);
    if (t.kind != GENESIS_CONTINUE_AT_PC) break;
    r->pc = t.next_pc;
  }
  *final_pc = r->pc;
  return t;
}

int main(void) {
  GenesisRuntime r;
  GenesisControlTransfer t;
  uint32_t pc;

  t = run_manual_call(&r, UINT32_C(0x00000F10), &pc);            /* PEA (d16,PC) */
  assert(t.kind == GENESIS_CONTINUE_AT_PC && pc == UINT32_C(0x00000F1A) && r.a[7] == UINT32_C(0x00FF0100));
  t = run_manual_call(&r, UINT32_C(0x00000F1E), &pc);            /* PEA abs.L */
  assert(t.kind == GENESIS_CONTINUE_AT_PC && pc == UINT32_C(0x00000F2E) && r.a[7] == UINT32_C(0x00FF0100));
  t = run_manual_call(&r, UINT32_C(0x00000F30), &pc);            /* pushed value is not compiled */
  assert(t.kind == GENESIS_STOP && pc == UINT32_C(0x00000F1C) && r.a[7] == UINT32_C(0x00FF00FC));

  /* A compiled address that no JSR continuation and no PEA names is still refused by the RTS. */
  r = (GenesisRuntime){0};
  r.pc = UINT32_C(0x00000F1C); r.a[7] = UINT32_C(0x00FF0080);
  put32(&r, 0x80, UINT32_C(0x00000F16));
  t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_STOP && r.a[7] == UINT32_C(0x00FF0080));

  /* A genuine JSR continuation still returns. */
  r = (GenesisRuntime){0};
  r.pc = UINT32_C(0x00000F1C); r.a[7] = UINT32_C(0x00FF0080);
  put32(&r, 0x80, UINT32_C(0x00000F06));
  t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_CONTINUE_AT_PC && t.next_pc == UINT32_C(0x00000F06) && r.a[7] == UINT32_C(0x00FF0084));
  return 0;
}
"""


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    runs = [subprocess.run([emitter, "--emit-pea-static-return"], text=True, capture_output=True) for _ in range(2)]
    for run in runs:
        assert run.returncode == 0, run.stderr
        assert not run.stdout.startswith("/* translation rejected:")
    assert runs[0].stdout == runs[1].stdout, "generated output must be deterministic"
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(runs[0].stdout, encoding="utf-8")
        (path / "harness.c").write_text(HARNESS, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "pea-static-return"
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
    print("genesis_pea_static_return_generated_test: OK")


if __name__ == "__main__":
    main()
