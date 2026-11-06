#!/usr/bin/env python3
"""SEG-122-T011 (ADR 0271): pinned-Musashi differential for the live-operand work-RAM `JMP (xxx).L` body.

The generated body of a statically established thunk is run for several live operands and compared with the pinned Musashi MC68000
core executing the same six bytes from work RAM: final PC, SR, Dn/An and the cycle count of the instruction must agree. Operands the
generated program refuses by design (odd, unmapped or uncompiled targets; the core would raise an address/bus error instead) are
outside the compared set and are covered by the fail-closed synthetic test. Without the checkout the synthetic checks run and no
differential claim is made.
"""
import hashlib
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_RAM_JMP_MUSASHI_CHECKOUT"
STUB = 0x00FFFFF0
OPERANDS = [0x00001000, 0x00002000, 0x00003000, 0xAB002000, 0xFF000200, 0x00000200, 0x00000206]
INITIAL_D = [0x11111111 * (i + 1) for i in range(8)]
INITIAL_A = [0x01010101 * (i + 1) for i in range(7)]


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stderr[:3000])
    return result


def rom():
    image = bytearray(0x4000)
    image[0:4] = struct.pack(">I", 0x00FFFE00)
    image[4:8] = struct.pack(">I", 0x200)
    image[0x78:0x7C] = struct.pack(">I", STUB)
    image[0x100:0x110] = b"SEGA GENESIS    "
    code = [0x31FC, 0x4EF9, 0xFFF0, 0x21FC, 0x0000, 0x1000, 0xFFF2, 0x207C, 0x0000, 0x0200, 0x4ED0]
    for i, word in enumerate(code):
        image[0x200 + 2 * i:0x202 + 2 * i] = struct.pack(">H", word)
    return bytes(image)


ORACLE = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "m68k.h"
#define RAM_BEGIN UINT32_C(0x00FF0000)
static uint8_t ram[UINT32_C(0x10000)];
static unsigned int read8(unsigned int a) { a &= 0x00FFFFFFU; if (a >= RAM_BEGIN) return ram[a - RAM_BEGIN]; return 0U; }
unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); } unsigned int m68k_read_memory_16(unsigned int a) { return (read8(a)<<8U)|read8(a+1U); } unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a)<<16U)|m68k_read_memory_16(a+2U); }
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); } unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); } unsigned int m68k_read_pcrelative_8(unsigned int a) { return read8(a); } unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); } unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); } unsigned int m68k_read_disassembler_8(unsigned int a) { return read8(a); } unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); } unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { (void)a; (void)v; } void m68k_write_memory_16(unsigned int a, unsigned int v) { (void)a; (void)v; } void m68k_write_memory_32(unsigned int a, unsigned int v) { (void)a; (void)v; } void m68k_write_memory_32_pd(unsigned int a, unsigned int v) { (void)a; (void)v; }
static void run(uint32_t operand) {
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset(); (void)m68k_execute(132);
  memset(ram, 0, sizeof ram);
  ram[0xFFF0] = 0x4E; ram[0xFFF1] = 0xF9; ram[0xFFF2] = (uint8_t)(operand >> 24); ram[0xFFF3] = (uint8_t)(operand >> 16); ram[0xFFF4] = (uint8_t)(operand >> 8); ram[0xFFF5] = (uint8_t)operand;
  for (int i = 0; i < 8; ++i) m68k_set_reg(M68K_REG_D0 + i, UINT32_C(0x11111111) * (uint32_t)(i + 1));
  for (int i = 0; i < 7; ++i) m68k_set_reg(M68K_REG_A0 + i, UINT32_C(0x01010101) * (uint32_t)(i + 1));
  m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FFFE00)); m68k_set_reg(M68K_REG_PC, UINT32_C(0x00FFFFF0)); m68k_set_reg(M68K_REG_SR, UINT32_C(0x271B));
  int cycles = m68k_execute(1);
  printf("%08X %04X %d", m68k_get_reg(NULL, M68K_REG_PC) & 0x00FFFFFFU, m68k_get_reg(NULL, M68K_REG_SR), cycles);
  for (int i = 0; i < 8; ++i) printf(" %08X", m68k_get_reg(NULL, M68K_REG_D0 + i));
  for (int i = 0; i < 8; ++i) printf(" %08X", m68k_get_reg(NULL, M68K_REG_A0 + i));
  printf("\n");
}
int main(void) { const uint32_t operands[] = {%OPERANDS%}; for (unsigned i = 0; i < sizeof operands / sizeof operands[0]; ++i) run(operands[i]); return 0; }
'''

ACTUAL = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#define main generated_bridge_main
#include "generated.c"
#undef main
static void run(uint32_t operand) {
  GenesisRuntime r = {0}; GenesisControlTransfer t;
  r.pc = UINT32_C(0x00FFFFF0); r.a[7] = UINT32_C(0x00FFFE00); r.sr = UINT16_C(0x271B);
  for (int i = 0; i < 8; ++i) r.d[i] = UINT32_C(0x11111111) * (uint32_t)(i + 1);
  for (int i = 0; i < 7; ++i) r.a[i] = UINT32_C(0x01010101) * (uint32_t)(i + 1);
  r.work_ram[0xFFF0] = 0x4E; r.work_ram[0xFFF1] = 0xF9; r.work_ram[0xFFF2] = (uint8_t)(operand >> 24); r.work_ram[0xFFF3] = (uint8_t)(operand >> 16);
  r.work_ram[0xFFF4] = (uint8_t)(operand >> 8); r.work_ram[0xFFF5] = (uint8_t)operand;
  t = genesis_bridge_dispatch(&r);
  if (t.kind != GENESIS_CONTINUE_AT_PC) { printf("STOP\n"); return; }
  printf("%08X %04X %u", r.pc, r.sr, (unsigned)(r.scheduler.master_ticks / GENESIS_M68K_CYCLE_MASTER_TICKS));
  for (int i = 0; i < 8; ++i) printf(" %08X", r.d[i]);
  for (int i = 0; i < 8; ++i) printf(" %08X", r.a[i]);
  printf("\n");
}
int main(void) { const uint32_t operands[] = {%OPERANDS%}; for (unsigned i = 0; i < sizeof operands / sizeof operands[0]; ++i) run(operands[i]); return 0; }
'''


def main():
    cli, compiler = sys.argv[1:3]
    configured = os.environ.get(CHECKOUT_ENV)
    image = rom()
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)
        (temp / "rom.bin").write_bytes(image)
        sha = hashlib.sha256(image).hexdigest()
        generated = checked([cli, "emit-general-startup-bridge-c", "--rom", str(temp / "rom.bin"), "--reset-entry", "--rom-sha256", sha,
                             "--immutable-rom-aot"]).stdout
        assert "genesis_aot_00FFFFF0(GenesisRuntime *runtime)" in generated
        # Operands that are compiled targets in this image: every even ROM address (the immutable AOT enumerates the whole image).
        operands = ", ".join(f"UINT32_C(0x{value:08X})" for value in OPERANDS)
        (temp / "generated.c").write_text(generated)
        (temp / "actual.c").write_text(ACTUAL.replace("%OPERANDS%", operands))
        actual = temp / "actual"
        checked([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "platforms/genesis/runtime"),
                 "-I", str(temp), str(temp / "actual.c"), str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(actual)])
        got = checked([actual]).stdout.splitlines()
        if not configured:
            print("RAM JMP live-operand pinned Musashi oracle unavailable; synthetic generated-C checks completed without differential claim")
            return
        checkout = pathlib.Path(configured)
        assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
        generator = temp / "m68kmake"
        checked([compiler, "-std=c11", "-w", str(checkout / "m68kmake.c"), "-o", str(generator)])  # third-party generator: not held to project warnings
        shutil.copyfile(checkout / "m68k_in.c", temp / "m68k_in.c")
        checked([str(generator)], cwd=temp)
        (temp / "oracle.c").write_text(ORACLE.replace("%OPERANDS%", operands))
        oracle = temp / "oracle"
        checked([compiler, "-std=c11", "-w", "-I", str(temp), "-I", str(checkout),
                 "-I", str(checkout / "softfloat"), str(temp / "oracle.c"), str(checkout / "m68kcpu.c"), str(temp / "m68kops.c"),
                 str(checkout / "softfloat/softfloat.c"), "-lm", "-o", str(oracle)])
        want = checked([oracle]).stdout.splitlines()
        assert len(got) == len(want) == len(OPERANDS), (got, want)
        for operand, ours, theirs in zip(OPERANDS, got, want):
            assert ours == theirs, (hex(operand), ours, theirs)
    print(f"compared {len(OPERANDS)} live operands (PC, SR, Dn, An, cycles) against pinned Musashi")


if __name__ == "__main__":
    main()
