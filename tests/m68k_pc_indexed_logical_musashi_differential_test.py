#!/usr/bin/env python3
"""Pinned-Musashi differential for project-authored brief PC-relative indexed
word AND/OR vectors.

Cross-checks the effective address, loaded value, result register, and full
condition-code result the shared runtime EA helper and the logical semantic
owner produce for synthetic `AND.W (0x08,PC,D2.L),D3` / `OR.W (0x08,PC,D2.L),D3`
vectors (`M68kEaMode::pc_index8`, SEG-007-T198) against the pinned local
Musashi MC68000 core. The generated C under test is produced by the full C4
retained-block dispatcher, including preflight and the normal runtime access
route. Skips the differential claim cleanly unless
SEGARECOMP_M68K_PC_INDEXED_LOGICAL_MUSASHI_CHECKOUT points at the pinned
checkout.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_PC_INDEXED_LOGICAL_MUSASHI_CHECKOUT"

# AND.W (0x08,PC,D2.L),D3 -> 0xC67B 0x2808 ; OR.W ... -> 0x867B 0x2808 at
# 0x00000B00. The extension word's own address is 0x00000B02, so the read
# target is 0x00000B02 + D2 + 0x08 -- a synthetic project-authored work-RAM
# location, never commercial-derived bytes.
ORACLE_TEMPLATE = r'''#include <stdint.h>
#include <stdio.h>
#include "m68k.h"
static const uint8_t code[] = {0x__PRIMARY__, 0x7b, 0x28, 0x08, 0x4e, 0x70};
static uint8_t ram[UINT32_C(0x10000)];
#define RAM_BEGIN UINT32_C(0x00FF0000)
static unsigned int read8(unsigned int a) { if (a >= UINT32_C(0x00000B00) && a < UINT32_C(0x00000B06)) return code[a-UINT32_C(0x00000B00)]; if (a >= RAM_BEGIN && a < RAM_BEGIN + UINT32_C(0x10000)) return ram[a-RAM_BEGIN]; return 0U; }
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
static void run(uint32_t d2, uint32_t d3_seed, uint16_t ram_word) {
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  m68k_set_reg(M68K_REG_D2, d2); m68k_set_reg(M68K_REG_D3, d3_seed);
  m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FF0100)); m68k_set_reg(M68K_REG_SR, UINT32_C(0x2704));
  m68k_set_reg(M68K_REG_PC, UINT32_C(0x00000B00));
  m68k_write_memory_16(UINT32_C(0x00000B02) + d2 + UINT32_C(0x08), ram_word);
  (void)m68k_execute(64);
  printf("%08X %04X\n", m68k_get_reg(NULL, M68K_REG_D3), m68k_get_reg(NULL, M68K_REG_SR) & 0x1FU);
}
int main(void) {
  run(UINT32_C(0x00FF0076), UINT32_C(0x1234ABCD), UINT32_C(0x0F0F));
  run(UINT32_C(0x00FF0080), UINT32_C(0xFFFF8000), UINT32_C(0xFFFF));
  return 0;
}
'''

ACTUAL_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
static void run(uint32_t d2, uint32_t d3_seed, uint16_t ram_word) {
  GenesisRuntime r = {0};
  r.pc = UINT32_C(0x00000B00); r.d[2] = d2; r.d[3] = d3_seed; r.a[7] = UINT32_C(0x00FF0100);
  r.sr = UINT16_C(0x2704);
  const uint32_t target = UINT32_C(0x00000B02) + d2 + UINT32_C(0x08);
  const uint32_t offset = target - UINT32_C(0x00FF0000);
  r.work_ram[offset] = (uint8_t)(ram_word >> 8U);
  r.work_ram[offset + 1U] = (uint8_t)(ram_word);
  (void)genesis_bridge_dispatch(&r);
  printf("%08X %04X\n", r.d[3], r.sr & 0x1FU);
}
int main(void) {
  run(UINT32_C(0x00FF0076), UINT32_C(0x1234ABCD), UINT32_C(0x0F0F));
  run(UINT32_C(0x00FF0080), UINT32_C(0xFFFF8000), UINT32_C(0xFFFF));
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
    forms = [("and", "--emit-operation-c4-pc-indexed-logical", 0xC6),
             ("or", "--emit-operation-c4-pc-indexed-logical-or", 0x86)]
    for name, flag, primary in forms:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            generated = checked([executable, flag]).stdout
            assert not generated.startswith("/* translation rejected:"), generated
            (temp / "generated.c").write_text(generated)
            (temp / "actual.c").write_text(ACTUAL_SOURCE)
            actual = temp / "actual"
            checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                     "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "actual.c"),
                     str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)], env=env())
            actual_output = checked([actual]).stdout
            if not configured:
                print(f"compiled and ran synthetic C4 PC-relative indexed {name.upper()}.W dispatcher; "
                      "pinned Musashi oracle unavailable")
                continue
            checkout = pathlib.Path(configured)
            assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
            assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0
            generator = temp / "m68kmake"
            checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                     str(checkout / "m68kmake.c"), "-o", str(generator)], env=env())
            shutil.copyfile(checkout / "m68k_in.c", temp / "m68k_in.c")
            checked([str(generator)], cwd=temp, env=env())
            oracle_source = temp / "oracle.c"
            oracle_source.write_text(ORACLE_TEMPLATE.replace("__PRIMARY__", "%02x" % primary))
            oracle = temp / "oracle"
            checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic",
                     "-I", str(temp), "-I", str(checkout), "-I", str(checkout / "softfloat"), str(oracle_source),
                     str(checkout / "m68kcpu.c"), str(temp / "m68kops.c"), str(checkout / "softfloat/softfloat.c"),
                     "-o", str(oracle)], env=env())
            assert actual_output == checked([oracle]).stdout, (name, actual_output, checked([oracle]).stdout)
            print(f"compared synthetic brief PC-relative indexed {name.upper()}.W result and condition codes "
                  "against pinned Musashi")


if __name__ == "__main__":
    main()
