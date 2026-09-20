#!/usr/bin/env python3
"""Pinned-Musashi differential for synthetic absolute-long-source plain CMP.

Cross-checks the CCR bits the shared C4 compare-family emission body computes
for `CMP.W (0x00FF0080).L,D0` (M68kIrKind::compare, a foldable-control
absolute-long source read routed to synthetic work RAM) against the pinned
local Musashi MC68000 core. Skips cleanly (exit 0) unless
SEGARECOMP_M68K_CMP_ABSOLUTE_SOURCE_MUSASHI_CHECKOUT points at the pinned
checkout.

SEG-007-T216: found while classifying the T215 closure-constructed
`c4_lowering_gap`/`cmp_missing_fact` frontier at a real reachable Sonic
startup block -- `platforms/genesis/machine/src/frontend.cpp`'s discovery-time
`retain_fact` switch had no `case M68kInstructionKind::cmp:` at all (only its
already-represented `cmpa`/`cmpi` siblings), and
`libs/codegen/c11/src/frontend.cpp`'s independent `valid_c4_static_memory_fact`
re-verification switch likewise had no `cmp` case (only its `adda`/`suba`/
`cmpa` group) -- a genuine missing-case gap for an already-supported
instruction (plain CMP), not a block-reconstruction, stitching, or
admission-policy defect. Mirrors `m68k_pc_indexed_lea_musashi_differential_
test.py` (SEG-007-T215) and its own sibling differentials.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_CMP_ABSOLUTE_SOURCE_MUSASHI_CHECKOUT"

# CMP.W (0x00FF0080).L,D0 at 0x00000B00 -> 0xB079 0x00FF 0x0080.
ORACLE_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "m68k.h"
static const uint8_t code[] = {0xb0, 0x79, 0x00, 0xff, 0x00, 0x80, 0x4e, 0x70};
static uint8_t ram[UINT32_C(0x10000)];
#define RAM_BEGIN UINT32_C(0x00FF0000)
static unsigned int read8(unsigned int a) { if (a >= UINT32_C(0x00000B00) && a < UINT32_C(0x00000B08)) return code[a-UINT32_C(0x00000B00)]; if (a >= RAM_BEGIN && a < RAM_BEGIN + UINT32_C(0x10000)) return ram[a-RAM_BEGIN]; return 0U; }
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
static void run(uint32_t d0, uint16_t ram_word) {
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  ram[UINT32_C(0x80)] = (uint8_t)(ram_word >> 8U); ram[UINT32_C(0x81)] = (uint8_t)ram_word;
  m68k_set_reg(M68K_REG_D0, d0);
  m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FF0100)); m68k_set_reg(M68K_REG_SR, UINT32_C(0x2700));
  m68k_set_reg(M68K_REG_PC, UINT32_C(0x00000B00));
  (void)m68k_execute(64);
  printf("%04X\n", m68k_get_reg(NULL, M68K_REG_SR) & UINT32_C(0x1F));
}
int main(void) {
  run(UINT32_C(0x00001234), UINT32_C(0x1234));  /* equal: Z set */
  run(UINT32_C(0x00000001), UINT32_C(0x0002));  /* destination < source: N/C set */
  run(UINT32_C(0x00000002), UINT32_C(0x0001));  /* destination > source: none set */
  return 0;
}
'''

ACTUAL_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
static void run(uint32_t d0, uint16_t ram_word) {
  GenesisRuntime r = {0};
  r.pc = UINT32_C(0x00000B00); r.d[0] = d0; r.a[7] = UINT32_C(0x00FF0100); r.sr = UINT32_C(0x2700);
  r.work_ram[UINT32_C(0x80)] = (uint8_t)(ram_word >> 8U); r.work_ram[UINT32_C(0x81)] = (uint8_t)ram_word;
  (void)genesis_bridge_dispatch(&r);
  printf("%04X\n", r.sr & UINT32_C(0x1F));
}
int main(void) {
  run(UINT32_C(0x00001234), UINT32_C(0x1234));
  run(UINT32_C(0x00000001), UINT32_C(0x0002));
  run(UINT32_C(0x00000002), UINT32_C(0x0001));
  return 0;
}
'''


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stderr)
    return result


def env():
    result = os.environ.copy()
    if not result.get("SDKROOT") and shutil.which("xcrun"):
        result["SDKROOT"] = checked(["xcrun", "--show-sdk-path"]).stdout.strip()
    return result


def main():
    executable, compiler = sys.argv[1:]
    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        print("CMP-absolute-source pinned Musashi oracle unavailable; synthetic generated-C checks completed without differential claim")
        return
    checkout = pathlib.Path(configured)
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
    assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)
        generator = temp / "m68kmake"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(checkout / "m68kmake.c"), "-o", str(generator)], env=env())
        shutil.copyfile(checkout / "m68k_in.c", temp / "m68k_in.c")
        checked([str(generator)], cwd=temp, env=env())
        oracle_source = temp / "oracle.c"
        oracle_source.write_text(ORACLE_SOURCE)
        oracle = temp / "oracle"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic", "-I", str(temp), "-I", str(checkout), "-I", str(checkout / "softfloat"), str(oracle_source), str(checkout / "m68kcpu.c"), str(temp / "m68kops.c"), str(checkout / "softfloat/softfloat.c"), "-o", str(oracle)], env=env())
        generated = checked([executable, "--emit-general-startup-runtime-c4-compare-source"]).stdout
        (temp / "generated.c").write_text(generated)
        (temp / "actual.c").write_text(ACTUAL_SOURCE)
        actual = temp / "actual"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "actual.c"), str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)], env=env())
        assert checked([actual]).stdout == checked([oracle]).stdout, (checked([actual]).stdout, checked([oracle]).stdout)
    print("compared synthetic absolute-long-source plain CMP CCR result against pinned Musashi")


if __name__ == "__main__":
    main()
