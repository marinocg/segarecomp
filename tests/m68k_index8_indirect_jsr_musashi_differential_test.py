#!/usr/bin/env python3
"""Pinned-Musashi differential for the Tier-2 `JSR (d8,An,Xn)` control EA.

Cross-checks the runtime effective address, the 4-byte return-continuation
push, and the resulting PC the shared Tier-2 lowering
(`build_genesis_frontier_stop_function`'s `is_index8` case,
libs/codegen/c11/src/frontend.cpp) computes for `JSR (4,A0,D0.W)`
(M68kEaMode::address_index8, SEG-021-T011) against the pinned local Musashi
MC68000 core. Skips cleanly (exit 0) unless
SEGARECOMP_M68K_INDEX8_INDIRECT_JSR_MUSASHI_CHECKOUT points at the pinned
checkout.

This project never attempts a Tier-1 finite-value proof for this combined
base+index control-EA shape (see `process_indirect_control_index8`'s own doc
comment in libs/cpu/m68k/src/static_discovery.cpp); every site of this shape
goes straight to the existing Tier-2/ADR-0024/ADR-0025 generalization
(runtime EA computation + `EmittedCodeAddressSet` membership guard), the same
mechanism already validated for `(d8,PC,Xn)` and pure `(An)` by
`m68k_indirect_control_musashi_differential_test.py`.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_INDEX8_INDIRECT_JSR_MUSASHI_CHECKOUT"

# NOP; JSR (4,A0,D0.W); NOP; NOP; BRA.S -10 -> 0x00000E00, at base 0x00000E00.
# Mirrors t011_index8_tier2_fixture::jsr_image (tests/m68k_pipeline_test.cpp)
# byte-for-byte.
ORACLE_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "m68k.h"
#define RAM_BEGIN UINT32_C(0x00FF0000)
static const uint8_t code[] = {0x4e, 0x71, 0x4e, 0xb0, 0x00, 0x04, 0x4e, 0x71, 0x4e, 0x71, 0x60, 0xf4};
static uint8_t ram[UINT32_C(0x10000)];
static unsigned int read8(unsigned int a) { if (a >= UINT32_C(0x00000E00) && a < UINT32_C(0x00000E0C)) return code[a-UINT32_C(0x00000E00)]; if (a >= RAM_BEGIN && a < RAM_BEGIN + UINT32_C(0x10000)) return ram[a-RAM_BEGIN]; return 0U; }
unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); } unsigned int m68k_read_memory_16(unsigned int a) { return (read8(a)<<8U)|read8(a+1U); } unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a)<<16U)|m68k_read_memory_16(a+2U); }
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); } unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); } unsigned int m68k_read_pcrelative_8(unsigned int a) { return read8(a); } unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); } unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); } unsigned int m68k_read_disassembler_8(unsigned int a) { return read8(a); } unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); } unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a,unsigned int v) { if(a>=RAM_BEGIN&&a<RAM_BEGIN+UINT32_C(0x10000)) ram[a-RAM_BEGIN]=(uint8_t)v; } void m68k_write_memory_16(unsigned int a,unsigned int v) { m68k_write_memory_8(a,v>>8U);m68k_write_memory_8(a+1U,v); } void m68k_write_memory_32(unsigned int a,unsigned int v) { m68k_write_memory_16(a,v>>16U);m68k_write_memory_16(a+2U,v); } void m68k_write_memory_32_pd(unsigned int a,unsigned int v) { m68k_write_memory_32(a,v); }
static void run(uint32_t a0, uint32_t d0) { m68k_init();m68k_set_cpu_type(M68K_CPU_TYPE_68000);m68k_pulse_reset();memset(ram,0,sizeof(ram));m68k_set_reg(M68K_REG_A0,a0);m68k_set_reg(M68K_REG_D0,d0);m68k_set_reg(M68K_REG_A7,UINT32_C(0x00FF0100));m68k_set_reg(M68K_REG_SR,UINT32_C(0x2700));m68k_set_reg(M68K_REG_PC,UINT32_C(0x00000E00));(void)m68k_execute(64);printf("%08X %08X %02X%02X%02X%02X\n",m68k_get_reg(NULL,M68K_REG_PC),m68k_get_reg(NULL,M68K_REG_A7),ram[0xfc],ram[0xfd],ram[0xfe],ram[0xff]); }
int main(void) { run(UINT32_C(0x00000DFC), 0U); run(UINT32_C(0x00000E06), 0U); return 0; }
'''

ACTUAL_SOURCE = r'''#include <stdint.h>
#include <stdio.h>
#include "runtime.h"
#include "generated.c"
static void run(uint32_t a0, uint32_t d0) { GenesisRuntime r={0}; GenesisControlTransfer t; r.pc=UINT32_C(0x00000E00);r.a[7]=UINT32_C(0x00FF0100);r.a[0]=a0;r.d[0]=d0;t=genesis_bridge_dispatch(&r);if(t.kind!=GENESIS_CONTINUE_AT_PC)return;r.pc=t.next_pc;printf("%08X %08X %02X%02X%02X%02X\n",r.pc,r.a[7],r.work_ram[0xfc],r.work_ram[0xfd],r.work_ram[0xfe],r.work_ram[0xff]); }
int main(void) { run(UINT32_C(0x00000DFC), 0U); run(UINT32_C(0x00000E06), 0U); return 0; }
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
        print("index8-indirect-JSR pinned Musashi oracle unavailable; synthetic generated-C checks completed "
              "without differential claim")
        return
    checkout = pathlib.Path(configured)
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
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
        generated = checked([executable, "--emit-general-startup-runtime-c4-indirect-jsr-index8"]).stdout
        (temp / "generated.c").write_text(generated)
        (temp / "actual.c").write_text(ACTUAL_SOURCE)
        actual = temp / "actual"
        checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "platforms/genesis/runtime"), "-I", str(temp), str(temp / "actual.c"), str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)], env=env())
        assert checked([actual]).stdout == checked([oracle]).stdout, (checked([actual]).stdout, checked([oracle]).stdout)
    print("compared synthetic brief address-register indexed JSR targets and stack result against pinned Musashi")


if __name__ == "__main__":
    main()
