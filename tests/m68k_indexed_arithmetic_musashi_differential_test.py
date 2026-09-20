#!/usr/bin/env python3
"""Pinned-Musashi differential for project-authored brief indexed ADDA vectors.

Cross-checks the effective address and loaded value the shared runtime EA
helper computes for synthetic address-register indexed ADDA vectors
(`M68kEaMode::address_index8`, SEG-007-T137) against the pinned local Musashi
MC68000 core. Skips cleanly
(exit 0) unless SEGARECOMP_M68K_INDEXED_ARITHMETIC_MUSASHI_CHECKOUT points at
the pinned checkout.

The generated C under test is produced by the full C4 retained-block dispatcher
(`--emit-operation-c4-indexed-arithmetic`), including preflight and the normal
runtime access route, rather than a standalone operation wrapper.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_INDEXED_ARITHMETIC_MUSASHI_CHECKOUT"

# ADDA.W (0x08,A1,D2.W),A3 at 0x00000B00 -> 0xD6F1 0x2008. The read target is
# A1 + sign_extend(D2.W) + 0x08 -- a synthetic project-authored work-RAM
# location, never commercial-derived bytes.
ORACLE_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "m68k.h"
static const uint8_t code[] = {0xd6, 0xf1, 0x20, 0x08};
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
static void run(uint32_t a1, uint32_t d2, uint32_t a3_seed, uint16_t ram_word) {
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  m68k_set_reg(M68K_REG_A1, a1); m68k_set_reg(M68K_REG_D2, d2); m68k_set_reg(M68K_REG_A3, a3_seed);
  m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FF0100)); m68k_set_reg(M68K_REG_SR, UINT32_C(0x2700));
  m68k_set_reg(M68K_REG_PC, UINT32_C(0x00000B00));
  m68k_write_memory_16(a1 + (uint32_t)(int32_t)(int16_t)d2 + UINT32_C(0x08), ram_word);
  (void)m68k_execute(64);
  printf("%08X\n", m68k_get_reg(NULL, M68K_REG_A3));
}
int main(void) {
  run(UINT32_C(0x00FF0080), UINT32_C(0x00000004), UINT32_C(0x00001000), UINT32_C(0x1122));
  run(UINT32_C(0x00FF0060), UINT32_C(0x0000FFFE), UINT32_C(0xFFFFFFF0), UINT32_C(0x8001));
  return 0;
}
'''

ACTUAL_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
static void run(uint32_t a1, uint32_t d2, uint32_t a3_seed, uint16_t ram_word) {
  GenesisRuntime r = {0};
  r.pc = UINT32_C(0x00000B00); r.a[1] = a1; r.d[2] = d2; r.a[3] = a3_seed; r.a[7] = UINT32_C(0x00FF0100);
  const uint32_t target = a1 + (uint32_t)(int32_t)(int16_t)d2 + UINT32_C(0x08);
  const uint32_t offset = target - UINT32_C(0x00FF0000);
  r.work_ram[offset] = (uint8_t)(ram_word >> 8U);
  r.work_ram[offset + 1U] = (uint8_t)(ram_word);
  (void)genesis_bridge_dispatch(&r);
  printf("%08X\n", r.a[3]);
}
int main(void) {
  run(UINT32_C(0x00FF0080), UINT32_C(0x00000004), UINT32_C(0x00001000), UINT32_C(0x1122));
  run(UINT32_C(0x00FF0060), UINT32_C(0x0000FFFE), UINT32_C(0xFFFFFFF0), UINT32_C(0x8001));
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
        # This is always a full C4 proof: the generator constructs a retained
        # prefix, passes C4 preflight, emits its normal dispatcher, then this
        # harness compiles and executes that dispatcher through runtime.c.
        generated = checked([executable, "--emit-operation-c4-indexed-arithmetic"]).stdout
        assert not generated.startswith("/* translation rejected:"), generated
        (temp / "generated.c").write_text(generated)
        (temp / "actual.c").write_text(ACTUAL_SOURCE)
        actual = temp / "actual"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "actual.c"), str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)], env=env())
        actual_output = checked([actual]).stdout
        if not configured:
            print("compiled and ran synthetic C4 indexed-ADDA dispatcher; pinned Musashi oracle unavailable")
            return
        checkout = pathlib.Path(configured)
        assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
        assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0
        generator = temp / "m68kmake"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(checkout / "m68kmake.c"), "-o", str(generator)], env=env())
        shutil.copyfile(checkout / "m68k_in.c", temp / "m68k_in.c")
        checked([str(generator)], cwd=temp, env=env())
        oracle_source = temp / "oracle.c"
        oracle_source.write_text(ORACLE_SOURCE)
        oracle = temp / "oracle"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic", "-I", str(temp), "-I", str(checkout), "-I", str(checkout / "softfloat"), str(oracle_source), str(checkout / "m68kcpu.c"), str(temp / "m68kops.c"), str(checkout / "softfloat/softfloat.c"), "-o", str(oracle)], env=env())
        assert actual_output == checked([oracle]).stdout, (actual_output, checked([oracle]).stdout)
    print("compared synthetic brief address-register indexed ADDA effective address and loaded value against pinned Musashi")


if __name__ == "__main__":
    main()
