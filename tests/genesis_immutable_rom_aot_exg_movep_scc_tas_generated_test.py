#!/usr/bin/env python3
"""SEG-021-T016: strict-C11 generated-native proof of the EXG/MOVEP/Scc/TAS immutable-ROM AOT roots.

The emitter (`m68k_pipeline_tests --emit-exg-movep-scc-tas-aot`) produces the complete generated program for a synthetic
eleven-instruction ROM; the harness below executes each root through the real Genesis runtime and checks hand-derived
results (Motorola M68000 Family Programmer's Reference Manual semantics, not the emitter's own output): the register swap,
MOVEP byte striding and preserved odd bytes, the Scc byte value for a true and a false condition with untouched CCR and
upper register bits, the A7 byte step of two, TAS N/Z/V/C and bit 7, published retirement cycles (Scc Dn 4 false / 6 true)
and routed-stop atomicity.
"""
import pathlib
import subprocess
import sys
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
static uint64_t cycles_of(const GenesisRuntime *runtime) { return runtime->scheduler.master_ticks / GENESIS_M68K_CYCLE_MASTER_TICKS; }
static GenesisRuntime fresh(uint32_t pc, uint16_t sr) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  runtime.pc = pc; runtime.sr = sr;
  return runtime;
}
static GenesisControlTransfer step(GenesisRuntime *runtime, uint32_t expected_next, uint64_t expected_cycles) {
  const uint64_t before = cycles_of(runtime);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  if (transfer.kind != GENESIS_CONTINUE_AT_PC || runtime->pc != expected_next || cycles_of(runtime) - before != expected_cycles)
    fprintf(stderr, "step to %04X: kind=%d pc=%04X cycles=%llu expected=%llu\n", (unsigned)expected_next, (int)transfer.kind,
            (unsigned)runtime->pc, (unsigned long long)(cycles_of(runtime) - before), (unsigned long long)expected_cycles);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime->pc == expected_next);
  assert(cycles_of(runtime) - before == expected_cycles);
  return transfer;
}
/* The last root falls off the end of the synthetic image: the next PC is a typed known-but-unemitted-target stop. */
static void step_to_end(GenesisRuntime *runtime, uint32_t expected_next, uint64_t expected_cycles) {
  const uint64_t before = cycles_of(runtime);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(runtime->pc == expected_next && cycles_of(runtime) - before == expected_cycles);
}
int main(void) {
  GenesisRuntime runtime;
  GenesisControlTransfer transfer;
  /* EXG D1,D2 / A1,A2 / D3,A4: full 32-bit swaps, 6 cycles each, CCR untouched. */
  runtime = fresh(0x0F08, 0x271F);
  runtime.d[1] = 0x11111111; runtime.d[2] = 0x22222222; runtime.a[1] = 0x00FF0100; runtime.a[2] = 0x00FF0200;
  runtime.d[3] = 0xAAAA0003; runtime.a[4] = 0x00FF1234;
  step(&runtime, 0x0F0A, 6);
  assert(runtime.d[1] == 0x22222222 && runtime.d[2] == 0x11111111);
  step(&runtime, 0x0F0C, 6);
  assert(runtime.a[1] == 0x00FF0200 && runtime.a[2] == 0x00FF0100);
  step(&runtime, 0x0F0E, 6);
  assert(runtime.d[3] == 0x00FF1234 && runtime.a[4] == 0xAAAA0003 && runtime.sr == 0x271F);

  /* MOVEP.W D1,(16,A0): bytes 0x12,0x34 land at +0 and +2; +1 and +3 keep their old values; 16 cycles; CCR untouched. */
  runtime = fresh(0x0F0E, 0x2715);
  runtime.a[0] = 0x00FF0200; runtime.d[1] = 0xAABB1234;
  memset(runtime.work_ram + 0x210, 0xEE, 8);
  step(&runtime, 0x0F12, 16);
  assert(runtime.work_ram[0x210] == 0x12 && runtime.work_ram[0x211] == 0xEE &&
         runtime.work_ram[0x212] == 0x34 && runtime.work_ram[0x213] == 0xEE && runtime.sr == 0x2715);
  assert(runtime.a[0] == 0x00FF0200 && runtime.d[1] == 0xAABB1234);
  /* MOVEP.L (16,A0),D1: every second byte, most significant first; odd bytes ignored; 24 cycles. */
  runtime = fresh(0x0F12, 0x2700);
  runtime.a[0] = 0x00FF0200; runtime.d[1] = 0xFFFFFFFF;
  runtime.work_ram[0x210] = 0x11; runtime.work_ram[0x211] = 0xEE; runtime.work_ram[0x212] = 0x22;
  runtime.work_ram[0x213] = 0xEE; runtime.work_ram[0x214] = 0x33; runtime.work_ram[0x215] = 0xEE;
  runtime.work_ram[0x216] = 0x44;
  step(&runtime, 0x0F16, 24);
  assert(runtime.d[1] == 0x11223344 && runtime.sr == 0x2700);
  /* An odd base address is legal for MOVEP (byte accesses). */
  runtime = fresh(0x0F12, 0x2700);
  runtime.a[0] = 0x00FF0201; runtime.work_ram[0x211] = 0x01; runtime.work_ram[0x213] = 0x02;
  runtime.work_ram[0x215] = 0x03; runtime.work_ram[0x217] = 0x04;
  step(&runtime, 0x0F16, 24);
  assert(runtime.d[1] == 0x01020304);
  /* Routed stop: an unmapped base changes neither Dn, An, SR nor PC. */
  runtime = fresh(0x0F12, 0x2704); runtime.a[0] = 0x00500000; runtime.d[1] = 0xCAFEBABE;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0F12 && runtime.d[1] == 0xCAFEBABE &&
         runtime.a[0] == 0x00500000 && runtime.sr == 0x2704);
  runtime = fresh(0x0F0E, 0x2704); runtime.a[0] = 0x00500000; runtime.d[1] = 0xCAFEBABE;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0F0E && runtime.d[1] == 0xCAFEBABE && runtime.sr == 0x2704);

  /* SEQ D3 with Z set writes 0xFF into the low byte only (6 cycles); with Z clear writes 0x00 (4 cycles). */
  runtime = fresh(0x0F16, 0x2704); runtime.d[3] = 0x12345600;
  step(&runtime, 0x0F18, 6);
  assert(runtime.d[3] == 0x123456FF && runtime.sr == 0x2704);
  runtime = fresh(0x0F16, 0x271B); runtime.d[3] = 0x123456FF;
  step(&runtime, 0x0F18, 4);
  assert(runtime.d[3] == 0x12345600 && runtime.sr == 0x271B);
  /* SNE (A0)+: Z clear -> 0xFF, A0 += 1 (12 cycles, condition independent for memory). */
  runtime = fresh(0x0F18, 0x2700); runtime.a[0] = 0x00FF0300;
  step(&runtime, 0x0F1A, 12);
  assert(runtime.work_ram[0x300] == 0xFF && runtime.a[0] == 0x00FF0301 && runtime.sr == 0x2700);
  runtime = fresh(0x0F18, 0x2704); runtime.a[0] = 0x00FF0300; runtime.work_ram[0x300] = 0x55;
  step(&runtime, 0x0F1A, 12);
  assert(runtime.work_ram[0x300] == 0x00 && runtime.a[0] == 0x00FF0301);
  /* ST -(A7): always true, A7 byte step is two (14 cycles). */
  runtime = fresh(0x0F1A, 0x2700); runtime.a[7] = 0x00FF0400;
  step(&runtime, 0x0F1C, 14);
  assert(runtime.work_ram[0x3FE] == 0xFF && runtime.a[7] == 0x00FF03FE);
  /* Scc routed stop: nothing changes. */
  runtime = fresh(0x0F18, 0x2700); runtime.a[0] = 0x00500000;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0F18 && runtime.a[0] == 0x00500000);

  /* TAS D2 of 0x00: Z set, N/V/C clear, X kept, byte becomes 0x80 (4 cycles). */
  runtime = fresh(0x0F1C, 0x2713); runtime.d[2] = 0x12345600;
  step(&runtime, 0x0F1E, 4);
  assert(runtime.d[2] == 0x12345680 && runtime.sr == 0x2714);
  /* TAS (A2)+ of 0x85: N set, Z clear, byte stays 0x85, A2 += 1 (14 cycles). */
  runtime = fresh(0x0F1E, 0x2700); runtime.a[2] = 0x00FF0500; runtime.work_ram[0x500] = 0x85;
  step(&runtime, 0x0F20, 14);
  assert(runtime.work_ram[0x500] == 0x85 && runtime.a[2] == 0x00FF0501 && runtime.sr == 0x2708);
  runtime = fresh(0x0F1E, 0x2700); runtime.a[2] = 0x00FF0500; runtime.work_ram[0x500] = 0x7F;
  step(&runtime, 0x0F20, 14);
  assert(runtime.work_ram[0x500] == 0xFF && runtime.sr == 0x2700);
  /* TAS routed stop: unmapped operand changes neither A2 nor CCR. */
  runtime = fresh(0x0F1E, 0x2713); runtime.a[2] = 0x00500000;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0F1E && runtime.a[2] == 0x00500000 && runtime.sr == 0x2713);

  /* SHI (16,A3): hi = !C && !Z. True with a clear CCR -> 0xFF at A3+16 (16 cycles); false with C set -> 0x00. */
  runtime = fresh(0x0F20, 0x2700); runtime.a[3] = 0x00FF0600; runtime.work_ram[0x610] = 0x33;
  step(&runtime, 0x0F24, 16);
  assert(runtime.work_ram[0x610] == 0xFF);
  runtime = fresh(0x0F20, 0x2701); runtime.a[3] = 0x00FF0600; runtime.work_ram[0x610] = 0x33;
  step(&runtime, 0x0F24, 16);
  assert(runtime.work_ram[0x610] == 0x00 && runtime.sr == 0x2701);

  /* SEQ (A1): memory destination, 8 + 4 = 12 cycles; 0xFF when Z set, 0x00 when clear; CCR unchanged, A1 unchanged. */
  runtime = fresh(0x0F24, 0x2704); runtime.a[1] = 0x00FF0700; runtime.work_ram[0x700] = 0x33;
  step_to_end(&runtime, 0x0F26, 12);
  assert(runtime.work_ram[0x700] == 0xFF && runtime.a[1] == 0x00FF0700 && runtime.sr == 0x2704);
  runtime = fresh(0x0F24, 0x271B); runtime.a[1] = 0x00FF0700; runtime.work_ram[0x700] = 0x33;
  step_to_end(&runtime, 0x0F26, 12);
  assert(runtime.work_ram[0x700] == 0x00 && runtime.a[1] == 0x00FF0700 && runtime.sr == 0x271B);
  /* Auto-update forms commit exactly once: (An)+ by one, -(A7) by two (byte operand). */
  runtime = fresh(0x0F18, 0x2700); runtime.a[0] = 0x00FF0301;
  step(&runtime, 0x0F1A, 12);
  assert(runtime.a[0] == 0x00FF0302 && runtime.work_ram[0x301] == 0xFF);
  runtime = fresh(0x0F1A, 0x2700); runtime.a[7] = 0x00FF0400;
  step(&runtime, 0x0F1C, 14);
  assert(runtime.a[7] == 0x00FF03FE && runtime.work_ram[0x3FF] == 0x00);

  /* Dn Scc is register-only: with every address register aimed at the write-only PSG port (whose byte read is
     rejected) it must still succeed, touch no device, keep the upper 24 bits and the CCR. */
  runtime = fresh(0x0F16, 0x2704); runtime.d[3] = 0xDEADBE00;
  runtime.a[0] = runtime.a[1] = runtime.a[7] = 0x00C00011;
  step(&runtime, 0x0F18, 6);
  assert(runtime.d[3] == 0xDEADBEFF && runtime.sr == 0x2704 && runtime.devices.psg.latch_valid == 0U);
  runtime = fresh(0x0F16, 0x2700); runtime.d[3] = 0xDEADBEFF;
  runtime.a[0] = runtime.a[1] = runtime.a[7] = 0x00C00011;
  step(&runtime, 0x0F18, 4);
  assert(runtime.d[3] == 0xDEADBE00 && runtime.devices.psg.latch_valid == 0U);

  /* Memory Scc reads its byte destination BEFORE writing it. The PSG port at 0xC00011 is an existing asymmetric
     lane: a byte WRITE is recognised (proved directly below) but a byte READ is rejected, so a write-only implementation
     would have succeeded. Required: the stop is the READ, no device write, PC/CCR/auto-update register unchanged. */
  {
    GenesisRuntime probe = fresh(0x0F18, 0x2700);
    uint32_t probe_value = 0xFF; GenesisRuntimeStop probe_stop = {0};
    assert(genesis_route_access(&probe, 0x00C00011, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &probe_value, &probe_stop) ==
           GENESIS_ACCESS_OK && probe.devices.psg.latch_valid == 1U);
    probe_value = 0;
    assert(genesis_route_access(&probe, 0x00C00011, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &probe_value, &probe_stop) !=
           GENESIS_ACCESS_OK);
  }
  /* SNE (A0)+ (true: Z clear). */
  runtime = fresh(0x0F18, 0x2700); runtime.a[0] = 0x00C00011;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.has_access &&
         transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ && transfer.stop.provenance.access_address == 0x00C00011U);
  assert(runtime.pc == 0x0F18 && runtime.a[0] == 0x00C00011 && runtime.sr == 0x2700 && runtime.devices.psg.latch_valid == 0U &&
         runtime.devices.psg.attenuation[3] == 0U);
  /* ST -(A7): A7 = 0xC00013 decrements by two to the same port; the read stops it, A7 untouched. */
  runtime = fresh(0x0F1A, 0x2715); runtime.a[7] = 0x00C00013;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ &&
         transfer.stop.provenance.access_address == 0x00C00011U);
  assert(runtime.pc == 0x0F1A && runtime.a[7] == 0x00C00013 && runtime.sr == 0x2715 && runtime.devices.psg.latch_valid == 0U);
  /* SEQ (A1) (true: Z set). */
  runtime = fresh(0x0F24, 0x2704); runtime.a[1] = 0x00C00011;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ);
  assert(runtime.pc == 0x0F24 && runtime.a[1] == 0x00C00011 && runtime.sr == 0x2704 && runtime.devices.psg.latch_valid == 0U);

  /* Write failure AFTER a successful read: an owned read-only cartridge region is readable but not writable. The stop
     is the WRITE (so the read was performed first); no auto-update commit, PC and CCR unchanged, region data intact. */
  {
    static const uint8_t rom_bytes[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                          0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    const GenesisOwnedCartridgeRegion region = {UINT32_C(0x1000), UINT32_C(0x1010), rom_bytes, 16U};
    runtime = fresh(0x0F18, 0x2700); runtime.owned_regions = &region; runtime.owned_region_count = 1U; runtime.a[0] = 0x1004;
    transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.access_direction == GENESIS_ACCESS_WRITE &&
           transfer.stop.provenance.access_address == 0x1004U);
    assert(runtime.pc == 0x0F18 && runtime.a[0] == 0x1004 && runtime.sr == 0x2700 && rom_bytes[4] == 0x14);
    runtime = fresh(0x0F1A, 0x2715); runtime.owned_regions = &region; runtime.owned_region_count = 1U; runtime.a[7] = 0x1006;
    transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.access_direction == GENESIS_ACCESS_WRITE &&
           transfer.stop.provenance.access_address == 0x1004U);
    assert(runtime.pc == 0x0F1A && runtime.a[7] == 0x1006 && runtime.sr == 0x2715 && rom_bytes[4] == 0x14);
    runtime = fresh(0x0F24, 0x2704); runtime.owned_regions = &region; runtime.owned_region_count = 1U; runtime.a[1] = 0x1008;
    transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.access_direction == GENESIS_ACCESS_WRITE);
    assert(runtime.pc == 0x0F24 && runtime.a[1] == 0x1008 && runtime.sr == 0x2704 && rom_bytes[8] == 0x18);
  }
  return 0;
}
'''

C4_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
int main(void) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  GenesisControlTransfer transfer;
  runtime.pc = 0x0B00; runtime.sr = 0x2704;
  runtime.d[1] = 0xAABB1234; runtime.d[2] = 0x12345600; runtime.d[3] = 0x12345600;
  runtime.a[0] = 0x00FF0200; runtime.a[2] = 0x00FF0500; runtime.a[3] = 0x00FF0600; runtime.a[7] = 0x00FF0400;
  memset(runtime.work_ram + 0x210, 0xEE, 8);
  runtime.work_ram[0x214] = 0x77; runtime.work_ram[0x216] = 0x88; runtime.work_ram[0x200] = 0x55;
  runtime.work_ram[0x500] = 0x00; runtime.work_ram[0x3FE] = 0x81; runtime.work_ram[0x700] = 0x55;
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0B1E);
  /* EXG D1,D2 swaps; MOVEP.W D1,(16,A0) stores 56/00 at +0/+2; MOVEP.L reloads 56 00 77 88 (odd bytes untouched). */
  assert(runtime.d[1] == 0x56007788 && runtime.work_ram[0x210] == 0x56 && runtime.work_ram[0x211] == 0xEE &&
         runtime.work_ram[0x212] == 0x00 && runtime.work_ram[0x213] == 0xEE);
  assert(runtime.d[3] == 0x123456FF);                                   /* SEQ with Z set */
  assert(runtime.work_ram[0x200] == 0x00 && runtime.a[0] == 0x00FF0201); /* SNE (A0)+ with Z set -> false */
  assert(runtime.d[2] == 0xAABB12B4);                                   /* EXG'd 0x34 | 0x80 */
  assert(runtime.work_ram[0x500] == 0x80 && runtime.a[2] == 0x00FF0501); /* TAS (A2)+ of 0x00 */
  assert(runtime.work_ram[0x3FE] == 0x81 && runtime.a[7] == 0x00FF03FE); /* TAS -(A7): byte step two, 0x81 stays 0x81 */
  assert(runtime.sr == 0x2708);                                         /* last TAS: N set, Z/V/C clear, X kept */
  assert(runtime.work_ram[0x610] == 0xFF);                              /* SHI: hi with N only -> true */
  assert(runtime.work_ram[0x700] == 0x00);                              /* SEQ $FF0700.L: Z clear -> false, read+write facts */
  return 0;
}
'''


def root_text(source, address):
    start = source.index("static GenesisControlTransfer genesis_aot_%s(" % address)
    end = source.find("static GenesisControlTransfer ", start + 10)
    return source[start:end if end != -1 else len(source)]


def check_scc_read_before_write(source):
    """Memory Scc must route exactly one BYTE read and then one BYTE write to the same EA (MC68000: a memory destination
    is read before it is written); auto-update forms commit the live address register exactly once, after the write."""
    dn = root_text(source, "00000F16")
    assert "genesis_route_access(" not in dn, "Scc Dn is register-only"
    for address, register, auto in (("00000F18", "a[0]", True), ("00000F1A", "a[7]", True), ("00000F24", "a[1]", False)):
        text = root_text(source, address)
        calls = [i for i in range(len(text)) if text.startswith("genesis_route_access(", i)]
        assert len(calls) == 2, (address, len(calls))
        first, second = text[calls[0]:calls[1]], text[calls[1]:text.index("pc += ", calls[1])]
        assert "GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &" in first, address
        assert "GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &" in second, address
        if auto:
            assert first.count("m68k_scc_auto_ea") == 1 and second.count("m68k_scc_auto_ea") >= 1, address
            commit = "runtime->%s = m68k_scc_auto_ea;" % register
            assert text.count(commit) == 1 and text.index(commit) > calls[1], address
        else:
            assert "runtime->%s =" % register not in text, "(An) is never updated"


def main():
    emitter, compiler, root = sys.argv[1:]
    result = subprocess.run([emitter, "--emit-exg-movep-scc-tas-aot"], text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    for address in ("00000F08", "00000F0A", "00000F0C", "00000F0E", "00000F12", "00000F16", "00000F18", "00000F1A",
                    "00000F1C", "00000F1E", "00000F20", "00000F24"):
        assert "genesis_aot_" + address in result.stdout, address
    check_scc_read_before_write(result.stdout)
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(result.stdout)
        (path / "harness.c").write_text(HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "exg-movep-scc-tas-aot"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr + ran.stdout
    c4 = subprocess.run([emitter, "--emit-exg-movep-scc-tas-c4"], text=True, capture_output=True)
    assert c4.returncode == 0 and "translation rejected" not in c4.stdout, c4.stderr + c4.stdout[:300]
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(c4.stdout)
        (path / "harness.c").write_text(C4_HARNESS)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / "exg-movep-scc-tas-c4"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr + ran.stdout
    print("genesis_immutable_rom_aot_exg_movep_scc_tas_generated_test: OK")


if __name__ == "__main__":
    main()
