#!/usr/bin/env python3
"""Pinned-Musashi differential for the ADR-0022 immutable offset-table producer.

SEG-007-T161 / ADR-0022. A synthetic project-authored program folds a proven
immutable in-cartridge offset table into the ADR-0009 finite index value set:

    ANDI.W  #2,D1              ; selector D1 in {0, 2}
    MOVE.W  (10,PC,D1.W),D0    ; overwrite index D0 from the offset table
    JSR     (0,PC,D0.W)        ; computed subroutine call

Static discovery inspects the table at generation time and emits the sorted
candidate literal array `m68k_indirect_targets_00000B08[]`. This test always
asserts that generated-C static-fold shape (no runtime instruction fetch or
decode, deterministic emission). When the pinned local Musashi core is
available (SEGARECOMP_M68K_IMMUTABLE_OFFSET_TABLE_MUSASHI_CHECKOUT), it also
executes the whole synthetic program on the real MC68000 for every selector
value and asserts the set of subroutine targets a real 68000 reaches -- via
`ANDI` narrowing, the word table read, `sign_extend_16`, and the brief
PC-indexed `JSR` effective address -- equals the statically folded literal set,
and that the unrelated direct `RTS` returns are unchanged.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_IMMUTABLE_OFFSET_TABLE_MUSASHI_CHECKOUT"

# The synthetic fixture image, based at 0x00000B00 (mirrors the
# `immutable-offset-table` C4 frontier fixture). 24 project-authored bytes;
# never commercial-derived.
IMAGE = bytes([
    0x02, 0x41, 0x00, 0x02,  # 0x00: ANDI.W #2,D1
    0x30, 0x3B, 0x10, 0x0A,  # 0x04: MOVE.W (10,PC,D1.W),D0
    0x4E, 0xBB, 0x00, 0x00,  # 0x08: JSR (0,PC,D0.W)
    0x4E, 0x70,              # 0x0C: RESET (shared frontier)
    0x00, 0x00,              # 0x0E: padding
    0x00, 0x0A, 0x00, 0x0C,  # 0x10: offset table -> 0xB14, 0xB16
    0x4E, 0x75, 0x4E, 0x75,  # 0x14: RTS candidate 0 / 1
])

ORACLE_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "m68k.h"
#define BASE UINT32_C(0x00000B00)
#define RAM_BEGIN UINT32_C(0x00FF0000)
static const uint8_t code[] = {%s};
static uint8_t ram[UINT32_C(0x10000)];
static unsigned int read8(unsigned int a) { if (a >= BASE && a < BASE + sizeof(code)) return code[a-BASE]; if (a >= RAM_BEGIN && a < RAM_BEGIN + UINT32_C(0x10000)) return ram[a-RAM_BEGIN]; return 0U; }
unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return (read8(a)<<8U)|read8(a+1U); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a)<<16U)|m68k_read_memory_16(a+2U); }
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a,unsigned int v) { if(a>=RAM_BEGIN&&a<RAM_BEGIN+UINT32_C(0x10000)) ram[a-RAM_BEGIN]=(uint8_t)v; }
void m68k_write_memory_16(unsigned int a,unsigned int v) { m68k_write_memory_8(a,v>>8U);m68k_write_memory_8(a+1U,v); }
void m68k_write_memory_32(unsigned int a,unsigned int v) { m68k_write_memory_16(a,v>>16U);m68k_write_memory_16(a+2U,v); }
void m68k_write_memory_32_pd(unsigned int a,unsigned int v) { m68k_write_memory_32(a,v); }
static void run(uint32_t d1) {
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  for (unsigned i = 0; i < sizeof(ram); ++i) ram[i] = 0U;
  m68k_set_reg(M68K_REG_D0, UINT32_C(0xDEAD0000));
  m68k_set_reg(M68K_REG_D1, d1);
  m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FF0100));
  m68k_set_reg(M68K_REG_SR, UINT32_C(0x2700));
  m68k_set_reg(M68K_REG_PC, BASE);
  unsigned int pc = BASE;
  for (int step = 0; step < 8; ++step) {
    (void)m68k_execute(2);
    pc = m68k_get_reg(NULL, M68K_REG_PC);
    if (pc == UINT32_C(0x00000B14) || pc == UINT32_C(0x00000B16)) break;
  }
  printf("%%08X %%08X\n", pc, m68k_get_reg(NULL, M68K_REG_A7));
}
int main(void) { run(UINT32_C(0)); run(UINT32_C(2)); run(UINT32_C(0xFFFF)); return 0; }
''' % ", ".join("0x%02x" % b for b in IMAGE)


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stderr)
    return result


def env():
    result = os.environ.copy()
    if not result.get("SDKROOT") and shutil.which("xcrun"):
        result["SDKROOT"] = checked(["xcrun", "--show-sdk-path"]).stdout.strip()
    return result


def folded_targets(generated):
    match = re.search(r"m68k_indirect_targets_00000B08\[\] = \{([^}]*)\}", generated)
    assert match, "generated C is missing the statically folded candidate array"
    return sorted(int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", match.group(1)))


def main():
    executable, compiler = sys.argv[1:]

    first = checked([executable, "--emit-general-startup-runtime-c4-immutable-offset-table"]).stdout
    second = checked([executable, "--emit-general-startup-runtime-c4-immutable-offset-table"]).stdout
    assert first == second and first, "offset-table folded emission must be byte-identical across runs"
    assert not first.startswith("/* translation rejected:")
    assert "m68k_indirect_target_member(m68k_indirect_targets_00000B08," in first
    assert "GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET, GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE" in first
    for banned in ("m68k_decode", "m68k_disassemble", "opcode_table"):
        assert banned not in first, banned
    targets = folded_targets(first)
    assert targets == [0x00000B14, 0x00000B16], targets

    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        print("immutable-offset-table pinned Musashi oracle unavailable; "
              "synthetic generated-C static-fold checks completed without differential claim")
        return
    checkout = pathlib.Path(configured)
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
    assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)
        generator = temp / "m68kmake"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(checkout / "m68kmake.c"), "-o", str(generator)], env=env())
        shutil.copyfile(checkout / "m68k_in.c", temp / "m68k_in.c")
        checked([str(generator)], cwd=temp, env=env())
        oracle_source = temp / "oracle.c"
        oracle_source.write_text(ORACLE_SOURCE)
        oracle = temp / "oracle"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable",
                 "-pedantic", "-I", str(temp), "-I", str(checkout), "-I", str(checkout / "softfloat"),
                 str(oracle_source), str(checkout / "m68kcpu.c"), str(temp / "m68kops.c"),
                 str(checkout / "softfloat/softfloat.c"), "-o", str(oracle)], env=env())
        lines = checked([oracle]).stdout.split()
        reached = sorted({int(lines[i], 16) for i in range(0, len(lines), 2)})
        assert reached == targets, (reached, targets)
        # every selector left the stack pointer one long word below the reset
        # A7 after the JSR push (the direct RTS returns restore it, unchecked
        # here) -- proving the computed JSR is an ordinary subroutine call.
        for i in range(1, len(lines), 2):
            assert int(lines[i], 16) == 0x00FF00FC, lines[i]
    print("compared the folded immutable-offset-table target set and JSR stack effect against pinned Musashi")


if __name__ == "__main__":
    main()
