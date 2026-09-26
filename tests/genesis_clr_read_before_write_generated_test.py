#!/usr/bin/env python3
"""SEG-021-T029: strict-C11 generated-native proof that memory-destination CLR reads before it writes.

MC68000 CLR of a memory destination performs a read of the destination (value discarded) before writing zero, the same
bus shape SEG-021-T016 established for memory Scc. The emitter (`m68k_pipeline_tests --emit-clr-read-before-write-aot`
and `--emit-clr-read-before-write-c4`) produces complete generated programs for synthetic CLR fixtures on the
immutable-ROM AOT route and the whole-program C4 route; the harnesses below execute them through the real Genesis
runtime and check hand-derived results (Motorola M68000 Family Programmer's Reference Manual, CLR): the cleared bytes
and their untouched neighbours, CCR N=0 Z=1 V=0 C=0 with X kept, one auto-update commit (A7 byte step two), register
CLR touching no device, published retirement cycles, and failure ordering against existing asymmetric lanes:
  * read failure before the write: the PSG port (byte write accepted, byte read rejected) and the Z80 RESET register
    (word write accepted, word read rejected) -- the stop is the READ, no device write, no An commit, PC/CCR unchanged;
  * write failure after a successful read: an owned read-only cartridge region -- the stop is the WRITE, no An commit,
    PC/CCR unchanged, region intact.
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
  memset(runtime.work_ram, 0x5A, sizeof runtime.work_ram);
  return runtime;
}
static void step(GenesisRuntime *runtime, uint32_t expected_next, uint64_t expected_cycles) {
  const uint64_t before = cycles_of(runtime);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  if (transfer.kind != GENESIS_CONTINUE_AT_PC || runtime->pc != expected_next || cycles_of(runtime) - before != expected_cycles)
    fprintf(stderr, "step to %04X: kind=%d pc=%04X cycles=%llu expected=%llu\n", (unsigned)expected_next, (int)transfer.kind,
            (unsigned)runtime->pc, (unsigned long long)(cycles_of(runtime) - before), (unsigned long long)expected_cycles);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime->pc == expected_next);
  assert(cycles_of(runtime) - before == expected_cycles);
}
/* The last root falls off the end of the synthetic image: the next PC is a typed known-but-unemitted-target stop. */
static void step_to_end(GenesisRuntime *runtime, uint32_t expected_next, uint64_t expected_cycles) {
  const uint64_t before = cycles_of(runtime);
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert(runtime->pc == expected_next && cycles_of(runtime) - before == expected_cycles);
}
static void expect_stop(GenesisRuntime *runtime, GenesisAccessDirection direction, uint32_t address) {
  GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.has_access &&
         transfer.stop.provenance.access_direction == direction && transfer.stop.provenance.access_address == address);
}
int main(void) {
  GenesisRuntime runtime;

  /* Register CLR is register-only: with the address registers aimed at the PSG port (whose byte read is rejected) it
     still succeeds, touches no device, clears only the operand width and sets Z (X kept). */
  runtime = fresh(0x0F08, 0x271B); runtime.d[3] = 0xDEADBEEF;
  runtime.a[0] = runtime.a[1] = runtime.a[7] = 0x00C00011;
  step(&runtime, 0x0F0A, 4);
  assert(runtime.d[3] == 0xDEADBE00 && runtime.sr == 0x2714 && runtime.devices.psg.latch_valid == 0U);
  runtime = fresh(0x0F18, 0x2708); runtime.d[5] = 0xFFFFFFFF;
  step(&runtime, 0x0F1A, 6);
  assert(runtime.d[5] == 0 && runtime.sr == 0x2704);

  /* Work-RAM success, every size; CCR N=0 Z=1 V=0 C=0, X kept; neighbours untouched; one auto-update commit. */
  runtime = fresh(0x0F0A, 0x271B); runtime.a[0] = 0x00FF0300;  /* CLR.B (A0)+ */
  step(&runtime, 0x0F0C, 12);
  assert(runtime.work_ram[0x300] == 0 && runtime.work_ram[0x301] == 0x5A && runtime.a[0] == 0x00FF0301 && runtime.sr == 0x2714);
  runtime = fresh(0x0F0C, 0x2708); runtime.a[7] = 0x00FF0400;  /* CLR.B -(A7): byte step two */
  step(&runtime, 0x0F0E, 14);
  assert(runtime.a[7] == 0x00FF03FE && runtime.work_ram[0x3FE] == 0 && runtime.work_ram[0x3FF] == 0x5A && runtime.sr == 0x2704);
  runtime = fresh(0x0F0E, 0x2700); runtime.a[1] = 0x00FF0500;  /* CLR.W (A1) */
  step(&runtime, 0x0F10, 12);
  assert(runtime.work_ram[0x500] == 0 && runtime.work_ram[0x501] == 0 && runtime.work_ram[0x502] == 0x5A &&
         runtime.a[1] == 0x00FF0500 && runtime.sr == 0x2704);
  runtime = fresh(0x0F10, 0x271F); runtime.a[2] = 0x00FF0602;  /* CLR.W -(A2) */
  step(&runtime, 0x0F12, 14);
  assert(runtime.a[2] == 0x00FF0600 && runtime.work_ram[0x600] == 0 && runtime.work_ram[0x601] == 0 &&
         runtime.work_ram[0x602] == 0x5A && runtime.sr == 0x2714);
  runtime = fresh(0x0F12, 0x2700); runtime.a[3] = 0x00FF0700;  /* CLR.L (A3)+ */
  step(&runtime, 0x0F14, 20);
  assert(runtime.a[3] == 0x00FF0704 && runtime.work_ram[0x700] == 0 && runtime.work_ram[0x703] == 0 &&
         runtime.work_ram[0x704] == 0x5A);
  runtime = fresh(0x0F14, 0x2700); runtime.a[4] = 0x00FF0800;  /* CLR.L (16,A4) */
  step(&runtime, 0x0F18, 24);
  assert(runtime.work_ram[0x80F] == 0x5A && runtime.work_ram[0x810] == 0 && runtime.work_ram[0x813] == 0 &&
         runtime.work_ram[0x814] == 0x5A && runtime.a[4] == 0x00FF0800);
  runtime = fresh(0x0F1A, 0x2700); runtime.a[7] = 0x00FF0900;  /* CLR.B (A7)+: byte step two */
  step_to_end(&runtime, 0x0F1C, 12);
  assert(runtime.a[7] == 0x00FF0902 && runtime.work_ram[0x900] == 0 && runtime.work_ram[0x901] == 0x5A);

  /* Read failure BEFORE the write. Probe the lanes first. The Z80 RESET register accepts the word write of zero but
     rejects the word read, so a write-only CLR would have succeeded there. The PSG port is a byte-write lane whose read
     is rejected (a zero DATA byte with no latched register is also rejected, so there the discriminator is the stop
     direction: a write-only CLR would stop on the WRITE, never the READ). */
  {
    GenesisRuntime probe = fresh(0x0F08, 0x2700);
    uint32_t value = 0x9F; GenesisRuntimeStop stop = {0};
    assert(genesis_route_access(&probe, 0x00C00011, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) ==
           GENESIS_ACCESS_OK && probe.devices.psg.latch_valid == 1U);
    assert(genesis_route_access(&probe, 0x00C00011, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ, &value, &stop) !=
           GENESIS_ACCESS_OK);
    value = 0;
    assert(genesis_route_access(&probe, 0x00A11200, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &stop) ==
           GENESIS_ACCESS_OK && probe.devices.z80_bus.reset_asserted == 1U);
    assert(genesis_route_access(&probe, 0x00A11200, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &value, &stop) !=
           GENESIS_ACCESS_OK);
  }
  runtime = fresh(0x0F0A, 0x271B); runtime.a[0] = 0x00C00011;  /* CLR.B (A0)+ */
  expect_stop(&runtime, GENESIS_ACCESS_READ, 0x00C00011U);
  assert(runtime.pc == 0x0F0A && runtime.a[0] == 0x00C00011 && runtime.sr == 0x271B && runtime.devices.psg.latch_valid == 0U);
  runtime = fresh(0x0F0C, 0x2715); runtime.a[7] = 0x00C00013;  /* CLR.B -(A7): steps two onto the port */
  expect_stop(&runtime, GENESIS_ACCESS_READ, 0x00C00011U);
  assert(runtime.pc == 0x0F0C && runtime.a[7] == 0x00C00013 && runtime.sr == 0x2715 && runtime.devices.psg.latch_valid == 0U);
  runtime = fresh(0x0F0E, 0x2708); runtime.a[1] = 0x00A11200;  /* CLR.W (A1) */
  expect_stop(&runtime, GENESIS_ACCESS_READ, 0x00A11200U);
  assert(runtime.pc == 0x0F0E && runtime.a[1] == 0x00A11200 && runtime.sr == 0x2708 &&
         runtime.devices.z80_bus.reset_asserted == 0U);
  runtime = fresh(0x0F10, 0x2701); runtime.a[2] = 0x00A11202;  /* CLR.W -(A2) */
  expect_stop(&runtime, GENESIS_ACCESS_READ, 0x00A11200U);
  assert(runtime.pc == 0x0F10 && runtime.a[2] == 0x00A11202 && runtime.sr == 0x2701 &&
         runtime.devices.z80_bus.reset_asserted == 0U);

  /* Write failure AFTER a successful read: an owned read-only cartridge region is readable but not writable. The stop
     is the WRITE (so the read was performed first); no auto-update commit, PC and CCR unchanged, region intact. */
  {
    static const uint8_t rom_bytes[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                          0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    const GenesisOwnedCartridgeRegion region = {UINT32_C(0x1000), UINT32_C(0x1010), rom_bytes, 16U};
#define WITH_ROM(pc_, sr_) runtime = fresh(pc_, sr_); runtime.owned_regions = &region; runtime.owned_region_count = 1U
    WITH_ROM(0x0F0A, 0x271B); runtime.a[0] = 0x1004;  /* CLR.B (A0)+ */
    expect_stop(&runtime, GENESIS_ACCESS_WRITE, 0x1004U);
    assert(runtime.pc == 0x0F0A && runtime.a[0] == 0x1004 && runtime.sr == 0x271B);
    WITH_ROM(0x0F0C, 0x2715); runtime.a[7] = 0x1006;  /* CLR.B -(A7) */
    expect_stop(&runtime, GENESIS_ACCESS_WRITE, 0x1004U);
    assert(runtime.pc == 0x0F0C && runtime.a[7] == 0x1006 && runtime.sr == 0x2715);
    WITH_ROM(0x0F0E, 0x2708); runtime.a[1] = 0x1008;  /* CLR.W (A1) */
    expect_stop(&runtime, GENESIS_ACCESS_WRITE, 0x1008U);
    assert(runtime.pc == 0x0F0E && runtime.a[1] == 0x1008 && runtime.sr == 0x2708);
    WITH_ROM(0x0F10, 0x2701); runtime.a[2] = 0x100A;  /* CLR.W -(A2) */
    expect_stop(&runtime, GENESIS_ACCESS_WRITE, 0x1008U);
    assert(runtime.pc == 0x0F10 && runtime.a[2] == 0x100A && runtime.sr == 0x2701);
    WITH_ROM(0x0F12, 0x271F); runtime.a[3] = 0x1004;  /* CLR.L (A3)+ */
    expect_stop(&runtime, GENESIS_ACCESS_WRITE, 0x1004U);
    assert(runtime.pc == 0x0F12 && runtime.a[3] == 0x1004 && runtime.sr == 0x271F);
    WITH_ROM(0x0F14, 0x2700); runtime.a[4] = 0x0FF8;  /* CLR.L (16,A4) */
    expect_stop(&runtime, GENESIS_ACCESS_WRITE, 0x1008U);
    assert(runtime.pc == 0x0F14 && runtime.a[4] == 0x0FF8 && runtime.sr == 0x2700);
    WITH_ROM(0x0F1A, 0x2708); runtime.a[7] = 0x100E;  /* CLR.B (A7)+ */
    expect_stop(&runtime, GENESIS_ACCESS_WRITE, 0x100EU);
    assert(runtime.pc == 0x0F1A && runtime.a[7] == 0x100E && runtime.sr == 0x2708);
#undef WITH_ROM
    for (unsigned i = 0; i < 16U; ++i) assert(rom_bytes[i] == (uint8_t)(0x10U + i));
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
static GenesisRuntime fresh(void) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  runtime.pc = 0x0B00; runtime.sr = 0x271B; runtime.d[3] = 0xDEADBEEF;
  runtime.a[0] = 0x00FF0100; runtime.a[2] = 0x00FF0200; runtime.a[3] = 0x00FF0600; runtime.a[7] = 0x00FF0400;
  memset(runtime.work_ram, 0x5A, sizeof runtime.work_ram);
  return runtime;
}
int main(void) {
  GenesisRuntime runtime = fresh();
  GenesisControlTransfer transfer;
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0B14);
  assert(runtime.d[3] == 0xDEADBE00);                                                      /* CLR.B D3 */
  assert(runtime.work_ram[0x100] == 0 && runtime.work_ram[0x101] == 0x5A && runtime.a[0] == 0x00FF0101); /* (A0)+ */
  assert(runtime.work_ram[0x3FE] == 0 && runtime.work_ram[0x3FF] == 0);                    /* CLR.W -(A7) */
  assert(runtime.work_ram[0x200] == 0 && runtime.work_ram[0x203] == 0 && runtime.work_ram[0x204] == 0x5A); /* (A2) */
  assert(runtime.a[7] == 0x00FF03FC && runtime.work_ram[0x3FC] == 0 && runtime.work_ram[0x3FD] == 0x5A); /* -(A7).B */
  assert(runtime.work_ram[0x700] == 0 && runtime.work_ram[0x701] == 0 && runtime.work_ram[0x702] == 0x5A); /* abs.L */
  assert(runtime.work_ram[0x60F] == 0x5A && runtime.work_ram[0x610] == 0 && runtime.work_ram[0x613] == 0 &&
         runtime.work_ram[0x614] == 0x5A);                                                  /* CLR.L (16,A3) */
  assert(runtime.sr == 0x2714);                                                            /* Z set, X kept */

  /* Read failure inside the C4 block: the READ of the PSG port stops CLR.B (A0)+ before any write or commit. */
  runtime = fresh(); runtime.a[0] = 0x00C00011;
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.has_access &&
         transfer.stop.provenance.access_direction == GENESIS_ACCESS_READ &&
         transfer.stop.provenance.access_address == 0x00C00011U);
  assert(runtime.pc == 0x0B02 && runtime.a[0] == 0x00C00011 && runtime.sr == 0x2714 && runtime.d[3] == 0xDEADBE00 &&
         runtime.devices.psg.latch_valid == 0U);

  /* Write failure after a successful read inside the C4 block: CLR.L (A2) on an owned read-only region. */
  {
    static const uint8_t rom_bytes[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                          0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    const GenesisOwnedCartridgeRegion region = {UINT32_C(0x1000), UINT32_C(0x1010), rom_bytes, 16U};
    runtime = fresh(); runtime.owned_regions = &region; runtime.owned_region_count = 1U; runtime.a[2] = 0x1004;
    do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.provenance.access_direction == GENESIS_ACCESS_WRITE &&
           transfer.stop.provenance.access_address == 0x1004U);
    assert(runtime.pc == 0x0B06 && runtime.a[2] == 0x1004 && runtime.a[7] == 0x00FF03FE && rom_bytes[4] == 0x14);
  }
  return 0;
}
'''


def root_text(source, address):
    start = source.index("static GenesisControlTransfer genesis_aot_%s(" % address)
    end = source.find("static GenesisControlTransfer ", start + 10)
    return source[start:end if end != -1 else len(source)]


CCR = "runtime->sr = (uint16_t)((runtime->sr & UINT16_C(0xFFF0)) | UINT16_C(4));"


def check_clr_read_before_write(source):
    """Memory CLR routes exactly one read and then one write of the operand width to the same EA; auto-update forms
    commit the live address register exactly once, after the write; the CCR follows both accesses."""
    for address in ("00000F08", "00000F18"):
        assert "genesis_route_access(" not in root_text(source, address), "CLR Dn is register-only"
    for address, width, register in (("00000F0A", "BYTE", "a[0]"), ("00000F0C", "BYTE", "a[7]"),
                                     ("00000F0E", "WORD", None), ("00000F10", "WORD", "a[2]"),
                                     ("00000F12", "LONG", "a[3]"), ("00000F14", "LONG", None),
                                     ("00000F1A", "BYTE", "a[7]")):
        text = root_text(source, address)
        calls = [i for i in range(len(text)) if text.startswith("genesis_route_access(", i)]
        assert len(calls) == 2, (address, len(calls))
        first, second = text[calls[0]:calls[1]], text[calls[1]:]
        assert "GENESIS_ACCESS_%s, GENESIS_ACCESS_READ, &" % width in first, address
        assert "GENESIS_ACCESS_%s, GENESIS_ACCESS_WRITE, &" % width in second, address
        assert text.count(CCR) == 1 and text.index(CCR) > calls[1] and text.index(CCR) < text.index("pc += "), address
        if register is not None:
            commit = "runtime->%s = m68k_clr_auto_ea;" % register
            assert text.count(commit) == 1 and calls[1] < text.index(commit) < text.index(CCR), address
        else:
            assert "m68k_clr_auto_ea" not in text and "runtime->a[1] =" not in text and "runtime->a[4] =" not in text


def build_and_run(compiler, root, generated, harness, name):
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(generated)
        (path / "harness.c").write_text(harness)
        runtime = pathlib.Path(root) / "platforms/genesis/runtime"
        executable = path / name
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
            "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr + ran.stdout


def main():
    emitter, compiler, root = sys.argv[1:]
    aot = subprocess.run([emitter, "--emit-clr-read-before-write-aot"], text=True, capture_output=True)
    assert aot.returncode == 0, aot.stderr
    for address in ("00000F08", "00000F0A", "00000F0C", "00000F0E", "00000F10", "00000F12", "00000F14", "00000F18",
                    "00000F1A"):
        assert "genesis_aot_" + address in aot.stdout, address
    check_clr_read_before_write(aot.stdout)
    build_and_run(compiler, root, aot.stdout, HARNESS, "clr-read-before-write-aot")
    c4 = subprocess.run([emitter, "--emit-clr-read-before-write-c4"], text=True, capture_output=True)
    assert c4.returncode == 0 and "translation rejected" not in c4.stdout, c4.stderr + c4.stdout[:300]
    assert "GENESIS_C4_LOWERING_DIMENSIONS_" not in c4.stdout
    build_and_run(compiler, root, c4.stdout, C4_HARNESS, "clr-read-before-write-c4")
    print("genesis_clr_read_before_write_generated_test: OK")


if __name__ == "__main__":
    main()
