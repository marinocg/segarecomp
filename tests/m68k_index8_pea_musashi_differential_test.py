#!/usr/bin/env python3
"""Pinned-Musashi differential for synthetic brief address-register indexed PEA.

Cross-checks the 32-bit address the shared runtime EA helper computes and
pushes for `PEA (0x10,A0,D1.W)` (M68kEaMode::address_index8, SEG-021-T011)
against the pinned local Musashi MC68000 core. Skips cleanly (exit 0) unless
SEGARECOMP_M68K_INDEX8_PEA_MUSASHI_CHECKOUT points at the pinned checkout.

PEA computes an address value and pushes it, never dereferencing the
addressed location -- the same non-control widening precedent
`m68k_indexed_lea_musashi_differential_test.py` (SEG-007-T135) already
validates for LEA. This differential exercises the DIRECT single-operation
C emission (`emit_m68k_operation_c`, via `--emit-operation-c4-indexed-pea`)
rather than the full C4 block-dispatch pipeline: `push_effective_address` is
not yet listed in `m68k_c4_represented_ir_kind`
(libs/codegen/c11/src/frontend.cpp), a PRE-EXISTING gap that declines PEA's
whole C4 block-dispatch route for every control-EA form (not something this
task's control-EA widening introduces); see that CLI flag's own C++ helper
doc comment (tests/m68k_pipeline_test.cpp) for the full explanation.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_INDEX8_PEA_MUSASHI_CHECKOUT"

# PEA (0x10,A0,D1.W) at 0x00000B00 -> 0x4870 0x1010.
ORACLE_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "m68k.h"
static const uint8_t code[] = {0x48, 0x70, 0x10, 0x10};
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
static void run(uint32_t a0, uint32_t d1) {
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  memset(ram, 0, sizeof(ram));
  m68k_set_reg(M68K_REG_A0, a0); m68k_set_reg(M68K_REG_D1, d1);
  m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FF0100)); m68k_set_reg(M68K_REG_SR, UINT32_C(0x2700));
  m68k_set_reg(M68K_REG_PC, UINT32_C(0x00000B00));
  (void)m68k_execute(64);
  printf("%08X %02X%02X%02X%02X\n", m68k_get_reg(NULL, M68K_REG_A7), ram[0xfc], ram[0xfd], ram[0xfe], ram[0xff]);
}
int main(void) { run(UINT32_C(0x00FF0080), UINT32_C(0x00000004)); run(UINT32_C(0x00FF00A0), UINT32_C(0x0000FFFE)); return 0; }
'''

ACTUAL_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
void genesis_attach_route_provenance(GenesisRuntimeStop *stop, const GenesisInstructionProvenance *source) { (void)stop; (void)source; }
#include "generated.c"
static void run(uint32_t a0, uint32_t d1) {
  GenesisRuntime r = {0};
  r.pc = UINT32_C(0x00000B00); r.a[0] = a0; r.d[1] = d1; r.a[7] = UINT32_C(0x00FF0100);
  (void)m68k_pea_op(&r);
  printf("%08X %02X%02X%02X%02X\n", r.a[7], r.work_ram[0xfc], r.work_ram[0xfd], r.work_ram[0xfe], r.work_ram[0xff]);
}
int main(void) { run(UINT32_C(0x00FF0080), UINT32_C(0x00000004)); run(UINT32_C(0x00FF00A0), UINT32_C(0x0000FFFE)); return 0; }
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
        print("index8-PEA pinned Musashi oracle unavailable; synthetic generated-C checks completed without "
              "differential claim")
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
        generated = checked([executable, "--emit-operation-c4-indexed-pea"]).stdout
        (temp / "generated.c").write_text(generated)
        (temp / "actual.c").write_text(ACTUAL_SOURCE)
        actual = temp / "actual"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "actual.c"), str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)], env=env())
        assert checked([actual]).stdout == checked([oracle]).stdout, (checked([actual]).stdout, checked([oracle]).stdout)
    print("compared synthetic brief address-register indexed PEA push result against pinned Musashi")


if __name__ == "__main__":
    main()
