#!/usr/bin/env python3
"""Pinned-Musashi differential for the project-authored synthetic
`DIVS.W D2,D3` vector (SEG-007-T222 / ADR-0037), closing the base-MC68000
multiply/divide family alongside MULS.W/MULU.W/DIVU.W.

Cross-checks the signed 16-bit quotient/remainder packing and condition-code
result the shared C4 retained-block dispatcher produces against the pinned
local Musashi MC68000 core for every architecturally-DEFINED vector (no
divisor==0, no quotient overflow -- those are covered by dedicated,
non-Musashi-compared tests: the divide-by-zero synchronous exception path by
a separate runtime-level test, and the overflow "Dn unchanged, V set" rule by
a local assertion below that does not compare against Musashi's own
architecturally-undefined N/Z bits for that case). Skips the Musashi
differential claim cleanly unless SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT
(the same pinned checkout MULS.W/MULU.W use) points at the pinned checkout.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT"

# DIVS.W D2,D3 -> 0x87C2 (opcode line 8, Dn=D3=011, opmode=111, mode=000,
# reg=D2=010) followed by RESET (0x4E70), a synthetic, project-authored
# register-direct vector, never commercial-derived bytes.
ORACLE_TEMPLATE = r'''#include <stdint.h>
#include <stdio.h>
#include "m68k.h"
static const uint8_t code[] = {0x87, 0xc2, 0x4e, 0x70};
static uint8_t ram[UINT32_C(0x10000)];
#define RAM_BEGIN UINT32_C(0x00FF0000)
static unsigned int read8(unsigned int a) { if (a >= UINT32_C(0x00000B00) && a < UINT32_C(0x00000B04)) return code[a-UINT32_C(0x00000B00)]; if (a >= RAM_BEGIN && a < RAM_BEGIN + UINT32_C(0x10000)) return ram[a-RAM_BEGIN]; return 0U; }
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
static void run(uint32_t d2, uint32_t d3_seed) {
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  m68k_set_reg(M68K_REG_D2, d2); m68k_set_reg(M68K_REG_D3, d3_seed);
  m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FF0100)); m68k_set_reg(M68K_REG_SR, UINT32_C(0x2700));
  m68k_set_reg(M68K_REG_PC, UINT32_C(0x00000B00));
  (void)m68k_execute(64);
  printf("%08X %04X\n", m68k_get_reg(NULL, M68K_REG_D3), m68k_get_reg(NULL, M68K_REG_SR) & 0x1FU);
}
int main(void) {
  run(UINT32_C(5), UINT32_C(100));               /* 100/5 = 20 rem 0 */
  run(UINT32_C(3), UINT32_C(0xFFFFFFF6));         /* -10/3 = -3 rem -1 */
  run(UINT32_C(0xFFFFFFFD), UINT32_C(10));        /* 10/-3 = -3 rem 1 */
  run(UINT32_C(0xFFFFFFFB), UINT32_C(0xFFFFFFF6));/* -10/-5 = 2 rem 0 */
  run(UINT32_C(7), UINT32_C(0));                  /* 0/7 = 0 rem 0 (zero quotient) */
  return 0;
}
'''

ACTUAL_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
static void run(uint32_t d2, uint32_t d3_seed) {
  GenesisRuntime r = {0};
  r.pc = UINT32_C(0x00000B00); r.d[2] = d2; r.d[3] = d3_seed; r.a[7] = UINT32_C(0x00FF0100);
  r.sr = UINT16_C(0x2700);
  (void)genesis_bridge_dispatch(&r);
  printf("%08X %04X\n", r.d[3], r.sr & 0x1FU);
}
int main(void) {
  run(UINT32_C(5), UINT32_C(100));
  run(UINT32_C(3), UINT32_C(0xFFFFFFF6));
  run(UINT32_C(0xFFFFFFFD), UINT32_C(10));
  run(UINT32_C(0xFFFFFFFB), UINT32_C(0xFFFFFFF6));
  run(UINT32_C(7), UINT32_C(0));
  return 0;
}
'''

# SEG-007-T222 / ADR-0037: quotient-overflow case (0x80000000 / -1). Dn must
# stay COMPLETELY unchanged and V=1 -- checked directly against the actual
# binary only (never against Musashi's own architecturally-undefined N/Z
# bits for this case, per this project's explicit documented choice).
OVERFLOW_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime r = {0};
  r.pc = UINT32_C(0x00000B00); r.d[2] = UINT32_C(0xFFFFFFFF); r.d[3] = UINT32_C(0x80000000);
  r.a[7] = UINT32_C(0x00FF0100); r.sr = UINT16_C(0x2700);
  (void)genesis_bridge_dispatch(&r);
  printf("%08X %04X\n", r.d[3], r.sr & UINT16_C(0x0007));
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
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)
        generated = checked([executable, "--emit-operation-c4-divs-word"]).stdout
        assert not generated.startswith("/* translation rejected:"), generated
        assert "genesis_m68k_divs_word_cycles" not in generated
        assert "m68k_timing_divisor" not in generated
        assert "UINT32_C(158)" in generated
        (temp / "generated.c").write_text(generated)
        (temp / "actual.c").write_text(ACTUAL_SOURCE)
        actual = temp / "actual"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "actual.c"),
                 str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)], env=env())
        actual_output = checked([actual]).stdout

        # Quotient-overflow: V=1 (bit 1 = 0x0002), Dn completely unchanged.
        (temp / "overflow.c").write_text(OVERFLOW_SOURCE)
        overflow_bin = temp / "overflow"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "overflow.c"),
                 str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(overflow_bin)], env=env())
        overflow_output = checked([overflow_bin]).stdout.strip()
        assert overflow_output == "80000000 0002", overflow_output

        if not configured:
            print("compiled and ran synthetic C4 DIVS.W dispatcher; pinned Musashi oracle unavailable")
            return 0
        checkout = pathlib.Path(configured)
        assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
        assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0
        generator = temp / "m68kmake"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(checkout / "m68kmake.c"), "-o", str(generator)], env=env())
        shutil.copyfile(checkout / "m68k_in.c", temp / "m68k_in.c")
        checked([str(generator)], cwd=temp, env=env())
        oracle_source = temp / "oracle.c"
        oracle_source.write_text(ORACLE_TEMPLATE)
        oracle = temp / "oracle"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic",
                 "-I", str(temp), "-I", str(checkout), "-I", str(checkout / "softfloat"), str(oracle_source),
                 str(checkout / "m68kcpu.c"), str(temp / "m68kops.c"), str(checkout / "softfloat/softfloat.c"),
                 "-o", str(oracle)], env=env())
        oracle_output = checked([oracle]).stdout
        assert actual_output == oracle_output, (actual_output, oracle_output)
        print("DIVS.W D2,D3 matches pinned Musashi oracle across 5 defined vectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
