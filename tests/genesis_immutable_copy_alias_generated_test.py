#!/usr/bin/env python3
"""SEG-021-T041 / ADR 0049: strict-C11 compile/execute proof of the immutable-copy alias.

Project-authored synthetic image (see `immutable_copy_alias_fixture` in m68k_pipeline_test.cpp). A ROM prologue copies
a 40-byte routine verbatim to work RAM with a MOVE.L (A0)+,(A1)+/DBRA loop and calls it with JSR abs.L. The routine
has MOVEQ/ADDQ arithmetic, BSR.S to an internal subroutine (call continuation), fallthrough, a relative BNE.S, a
PC-relative data read (d16,PC), an absolute store and RTS. The alias is a generated-data descriptor
(execution base, source base, length); the compiled bodies are the SAME immutable source bytes decoded a second time
at the RAM execution base, each guarded by a runtime byte-identity comparison of its own bytes.

Proves: the RAM copy executes at RAM-relative semantics (the PC-relative read observes the RAM data word, not the ROM
one), the architectural PC and the pushed BSR continuation are RAM addresses, the return stack is correct, every byte
of every executed instruction fails closed when tampered (before any effect), a copy that never happened and a later
change both fail closed, ordinary ROM execution of the same routine is unchanged (ROM-relative), the control program
without the alias still stops at the RAM target, no runtime opcode decode exists in the generated program, output is
deterministic, and everything compiles under strict C11.
"""

import pathlib
import subprocess
import sys
import tempfile

HARNESS = r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"

#define RAM UINT32_C(0x00FF0400)
static const uint8_t routine[40] = {
  0x70, 0x05, 0x61, 0x18, 0x4A, 0x40, 0x66, 0x04, 0x7C, 0x63, 0x4E, 0x71, 0x30, 0x3A, 0x00, 0x16,
  0x33, 0xC0, 0x00, 0xFF, 0x08, 0x00, 0x4E, 0x75, 0x4E, 0x71, 0x4E, 0x71, 0x52, 0x40, 0x4E, 0x75,
  0x4E, 0x71, 0x4E, 0x71, 0x12, 0x34, 0x00, 0x00 };

/* The immutable cartridge bytes of the routine's ROM source (ADR 0006 owned region): the copy loop's runtime
   read and the ROM-relative (d16,PC) read of the ROM execution route through it. */
static const GenesisOwnedCartridgeRegion rom_region = { UINT32_C(0x00000E40), UINT32_C(0x00000E68), routine, 40U };

static void put32(GenesisRuntime *r, uint32_t off, uint32_t v) {
  r->work_ram[off] = (uint8_t)(v >> 24); r->work_ram[off + 1] = (uint8_t)(v >> 16);
  r->work_ram[off + 2] = (uint8_t)(v >> 8); r->work_ram[off + 3] = (uint8_t)v;
}

static void at_ram_routine(GenesisRuntime *r) {
  *r = (GenesisRuntime){0};
  r->owned_regions = &rom_region; r->owned_region_count = 1U;
  memcpy(&r->work_ram[0x400], routine, sizeof(routine));
  r->pc = RAM;
  r->a[7] = UINT32_C(0x00FF0FFC);
  put32(r, 0xFFC, UINT32_C(0x00000E28));  /* return address of the JSR that called the routine */
}

static GenesisControlTransfer step(GenesisRuntime *r) {
  GenesisControlTransfer t = genesis_bridge_dispatch(r);
  if (t.kind == GENESIS_CONTINUE_AT_PC) r->pc = t.next_pc;
  return t;
}

int main(void) {
  GenesisRuntime r;
  GenesisControlTransfer t;
  unsigned i;

  /* 1. Generated copy loop, then transfer to the RAM copy through the ordinary JSR. */
  r = (GenesisRuntime){0};
  r.owned_regions = &rom_region; r.owned_region_count = 1U;
  r.pc = UINT32_C(0x00000E10);
  r.a[7] = UINT32_C(0x00FF1000);
  for (i = 0; i < 200 && r.pc != RAM; ++i) {
    t = step(&r);
    assert(t.kind == GENESIS_CONTINUE_AT_PC);
  }
  assert(r.pc == RAM);
  assert(memcmp(&r.work_ram[0x400], routine, sizeof(routine)) == 0);  /* verbatim copy performed by generated code */
  assert(r.a[7] == UINT32_C(0x00FF0FFC));
  assert(r.work_ram[0xFFC] == 0x00 && r.work_ram[0xFFD] == 0x00 && r.work_ram[0xFFE] == 0x0E && r.work_ram[0xFFF] == 0x28);

  /* 2. RAM-relative semantics. Only the data word (not an executed instruction) is changed. */
  r.work_ram[0x424] = 0xBE; r.work_ram[0x425] = 0xEF;
  t = step(&r); assert(t.kind == GENESIS_CONTINUE_AT_PC && r.pc == UINT32_C(0x00FF0402));   /* MOVEQ: fallthrough */
  t = step(&r); assert(t.kind == GENESIS_CONTINUE_AT_PC && r.pc == UINT32_C(0x00FF041C));   /* BSR.S: relative call */
  assert(r.a[7] == UINT32_C(0x00FF0FF8));
  assert(r.work_ram[0xFF8] == 0x00 && r.work_ram[0xFF9] == 0xFF && r.work_ram[0xFFA] == 0x04 && r.work_ram[0xFFB] == 0x04);
  t = step(&r); assert(r.pc == UINT32_C(0x00FF041E) && r.d[0] == 6U);                     /* ADDQ */
  t = step(&r); assert(t.kind == GENESIS_CONTINUE_AT_PC && r.pc == UINT32_C(0x00FF0404));   /* RTS to the RAM continuation */
  assert(r.a[7] == UINT32_C(0x00FF0FFC));
  t = step(&r); assert(r.pc == UINT32_C(0x00FF0406));                                     /* TST.W */
  t = step(&r); assert(r.pc == UINT32_C(0x00FF040C));                                     /* BNE.S taken: RAM-relative */
  t = step(&r); assert(r.pc == UINT32_C(0x00FF0410) && r.d[0] == UINT32_C(0xBEEF));       /* (d16,PC) reads the RAM copy */
  t = step(&r); assert(r.pc == UINT32_C(0x00FF0416));
  assert(r.work_ram[0x800] == 0xBE && r.work_ram[0x801] == 0xEF && r.d[6] == 0U);
  t = step(&r); assert(t.kind == GENESIS_CONTINUE_AT_PC && r.pc == UINT32_C(0x00000E28)); /* RTS back into ROM */
  assert(r.a[7] == UINT32_C(0x00FF1000));

  /* 3. Ordinary ROM execution of the same routine is unchanged and ROM-relative (no alias effect). */
  r = (GenesisRuntime){0};
  r.owned_regions = &rom_region; r.owned_region_count = 1U;
  r.pc = UINT32_C(0x00000E40);
  r.a[7] = UINT32_C(0x00FF0FFC);
  put32(&r, 0xFFC, UINT32_C(0x00000E28));
  {
    static const uint32_t rom_path[] = {0xE42, 0xE5C, 0xE5E, 0xE44, 0xE46, 0xE4C, 0xE50, 0xE56, 0xE28};
    for (i = 0; i < sizeof(rom_path) / sizeof(rom_path[0]); ++i) {
      t = step(&r);
      assert(t.kind == GENESIS_CONTINUE_AT_PC && r.pc == rom_path[i]);
    }
    assert(r.d[0] == UINT32_C(0x1234) && r.work_ram[0x800] == 0x12 && r.work_ram[0x801] == 0x34);
  }

  /* 4. Single-byte tamper of every byte of every executed instruction fails closed before any effect. */
  {
    static const struct { uint32_t off, len; } executed[] = {{0x00, 2}, {0x02, 2}, {0x04, 2}, {0x06, 2},
                                                             {0x0C, 4}, {0x10, 6}, {0x16, 2}, {0x1C, 2}, {0x1E, 2}};
    unsigned e, b;
    for (e = 0; e < sizeof(executed) / sizeof(executed[0]); ++e)
      for (b = 0; b < executed[e].len; ++b) {
        int steps = 0;
        at_ram_routine(&r);
        r.work_ram[0x400 + executed[e].off + b] ^= 0x01;
        for (;;) {
          const uint32_t pc_before = r.pc;
          const uint32_t a7_before = r.a[7];
          const uint32_t d0_before = r.d[0];
          t = step(&r);
          if (t.kind == GENESIS_STOP) {
            assert(pc_before == RAM + executed[e].off);          /* stops exactly at the tampered instruction */
            assert(r.pc == pc_before && r.a[7] == a7_before && r.d[0] == d0_before);  /* nothing committed */
            assert(t.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
            break;
          }
          assert(++steps < 64);
        }
      }
  }

  /* 5. The copy never happened (zero RAM): fails closed at the very first alias instruction. */
  r = (GenesisRuntime){0};
  r.pc = RAM; r.a[7] = UINT32_C(0x00FF0FFC);
  t = step(&r);
  assert(t.kind == GENESIS_STOP && r.pc == RAM && r.d[0] == 0U && r.a[7] == UINT32_C(0x00FF0FFC));

  /* 6. Bytes that change after execution started are re-checked (the identity is checked at every fetch). */
  at_ram_routine(&r);
  t = step(&r); t = step(&r); t = step(&r); t = step(&r);  /* MOVEQ, BSR, ADDQ, RTS */
  assert(r.pc == UINT32_C(0x00FF0404));
  r.work_ram[0x40C + 2] ^= 0x40;  /* extension word of the later (d16,PC) instruction */
  t = step(&r); t = step(&r);
  assert(r.pc == UINT32_C(0x00FF040C));
  t = step(&r);
  assert(t.kind == GENESIS_STOP && r.pc == UINT32_C(0x00FF040C) && r.d[0] == 6U);

  /* 7. Mirrors and non-alias RAM addresses are never executed as the alias (fail closed, not compiled). */
  r = (GenesisRuntime){0};
  memcpy(&r.work_ram[0x400], routine, sizeof(routine));
  r.pc = UINT32_C(0x00E00400); r.a[7] = UINT32_C(0x00FF0FFC);
  t = step(&r);
  assert(t.kind == GENESIS_STOP && r.pc == UINT32_C(0x00E00400));
  return 0;
}
'''

CONTROL_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"

int main(void) {
  static const uint8_t rom[40] = {0};
  static const GenesisOwnedCartridgeRegion region = { UINT32_C(0x00000E40), UINT32_C(0x00000E68), rom, 40U };
  GenesisRuntime r = {0};
  GenesisControlTransfer t = {0};
  unsigned i;
  r.owned_regions = &region; r.owned_region_count = 1U;
  r.pc = UINT32_C(0x00000E10);
  r.a[7] = UINT32_C(0x00FF1000);
  for (i = 0; i < 200; ++i) {
    t = genesis_bridge_dispatch(&r);
    if (t.kind != GENESIS_CONTINUE_AT_PC) break;
    r.pc = t.next_pc;
  }
  /* Without the alias the RAM target is not compiled: the unchanged fail-closed stop at the RAM PC. */
  assert(t.kind == GENESIS_STOP && r.pc == UINT32_C(0x00FF0400));
  assert(t.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  return 0;
}
'''


def build_and_run(compiler: str, root: str, generated: str, harness: str, name: str) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated, encoding="utf-8")
        (path / "harness.c").write_text(harness, encoding="utf-8")
        runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / name
        built = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), "-I", str(path), str(path / "harness.c"),
             str(runtime_dir / "runtime.c"), "-o", str(executable)],
            text=True, capture_output=True,
        )
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr


def main() -> None:
    emitter, compiler, root = sys.argv[1:]
    validation = subprocess.run([emitter, "--check-immutable-copy-alias-validation"], text=True, capture_output=True)
    assert validation.returncode == 0, f"descriptor validation check failed with {validation.returncode}"

    runs = [subprocess.run([emitter, "--emit-immutable-copy-alias"], text=True, capture_output=True) for _ in range(2)]
    for run in runs:
        assert run.returncode == 0, run.stderr
        assert not run.stdout.startswith("/* translation rejected:")
    assert runs[0].stdout == runs[1].stdout, "generated output must be deterministic"
    generated = runs[0].stdout
    for pc in ("00FF0400", "00FF0402", "00FF0404", "00FF0406", "00FF040C", "00FF0410", "00FF0416",
               "00FF041C", "00FF041E"):
        assert "genesis_aot_" + pc in generated, pc
    # The guard is identity comparison of literal bytes; the generated program has no opcode decoder.
    assert "runtime->work_ram[UINT32_C(1024)] != UINT8_C(0x70)" in generated
    assert "decode" not in generated.lower()

    control = subprocess.run([emitter, "--emit-immutable-copy-alias-control"], text=True, capture_output=True)
    assert control.returncode == 0, control.stderr
    assert "genesis_aot_00FF0400" not in control.stdout

    build_and_run(compiler, root, generated, HARNESS, "alias")
    build_and_run(compiler, root, control.stdout, CONTROL_HARNESS, "alias-control")
    print("genesis_immutable_copy_alias_generated_test: OK")


if __name__ == "__main__":
    main()
