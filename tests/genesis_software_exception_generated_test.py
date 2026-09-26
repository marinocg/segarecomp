#!/usr/bin/env python3
"""SEG-021-T019 / ADR 0043 §3, §5: strict-C11 generated-native proof of the software exceptions (TRAP #n, TRAPV,
CHK.W, ILLEGAL, line 1010/1111, other architecturally illegal words) and RTR on every generated-native route.

1. Immutable-ROM AOT roots (`m68k_pipeline_tests --emit-software-exception-aot`), executed through the real Genesis
   runtime against hand-derived Motorola M68000 Family Programmer's Reference Manual results: the six-byte frame on
   the SSP (saved SR word at SP, saved PC long at SP+2) from supervisor and user mode, the stacked PC of each kind
   (TRAP/TRAPV/CHK: the next instruction; ILLEGAL/line 1010/line 1111/other illegal words: the instruction itself),
   S set / T clear / mask kept, the CHK bound check and condition codes (N defined on a trap; Z/V/C matched to the
   pinned Musashi core), (An)+/-(An) bound auto-update, RTR restoring only the CCR plus PC, published retirement
   cycles for the retiring paths (exception entry charges nothing new, ADR 0043 §8), and the fail-closed stops
   (uninstalled handler, unconstructible frame, failed routed read) that change nothing.
2. The ordinary whole-program C4 route (`--emit-software-exception-c4`), with a retained work-RAM fact for the absolute
   CHK bound.
3. The whole-program bridge (`--emit-general-startup-bridge-software-exception[-no-handler]`): handlers resolved and
   rooted at build time from the synthetic vector table, entered from supervisor and user mode, returning with RTE;
   RTR; and the sanitized fail-closed report when no handler is installed.
All programs are project-authored synthetic fixtures.
"""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

AOT_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#include "generated.c"
#define HANDLER UINT32_C(0x00001028)
static uint64_t cycles_of(const GenesisRuntime *runtime) { return runtime->scheduler.master_ticks / GENESIS_M68K_CYCLE_MASTER_TICKS; }
static GenesisRuntime fresh(uint32_t pc, uint16_t sr) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  unsigned v;
  runtime.pc = pc; runtime.sr = sr;
  if ((sr & 0x2000U) != 0U) { runtime.a[7] = 0x00FF0400; runtime.usp = 0x00FF0600; }
  else { runtime.a[7] = 0x00FF0500; runtime.usp = 0x00FF0700; }
  for (v = 0; v < GENESIS_M68K_SOFTWARE_EXCEPTION_VECTOR_LIMIT; ++v) {
    runtime.software_exception_handler_entry[v] = HANDLER; runtime.software_exception_handler_present[v] = 1U;
  }
  return runtime;
}
static GenesisControlTransfer step(GenesisRuntime *runtime, uint32_t expected_next, uint64_t expected_cycles) {
  const uint64_t before = cycles_of(runtime);
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  if (transfer.kind != GENESIS_CONTINUE_AT_PC || runtime->pc != expected_next || cycles_of(runtime) - before != expected_cycles)
    fprintf(stderr, "step to %04X: kind=%d pc=%04X cycles=%llu expected=%llu\n", (unsigned)expected_next, (int)transfer.kind,
            (unsigned)runtime->pc, (unsigned long long)(cycles_of(runtime) - before), (unsigned long long)expected_cycles);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime->pc == expected_next && transfer.next_pc == expected_next);
  assert(cycles_of(runtime) - before == expected_cycles);
  return transfer;
}
static uint32_t frame_pc(const GenesisRuntime *r, uint32_t base) {
  const uint8_t *m = r->work_ram + (base - 0x00FF0000U);
  return ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
}
static uint16_t frame_sr(const GenesisRuntime *r, uint32_t base) {
  const uint8_t *m = r->work_ram + (base - 0x00FF0000U);
  return (uint16_t)((m[0] << 8) | m[1]);
}
/* The exception is taken: frame on the SSP, handler entered, no retirement charged (ADR 0043 §8). */
static void expect_entry(GenesisRuntime *runtime, uint32_t stacked_pc) {
  const GenesisRuntime before = *runtime;
  const uint32_t ssp = (before.sr & 0x2000U) != 0U ? before.a[7] : before.usp;
  step(runtime, HANDLER, 0);
  assert(runtime->a[7] == ssp - 6U);
  assert(runtime->usp == ((before.sr & 0x2000U) != 0U ? before.usp : before.a[7]));
  assert(frame_pc(runtime, ssp - 6U) == stacked_pc);
  assert(runtime->sr == (uint16_t)((frame_sr(runtime, ssp - 6U) & 0x271FU) | 0x2000U));
}
/* A software exception that cannot be delivered stops fail-closed with nothing changed. */
static void expect_fail_closed(GenesisRuntime runtime) {
  const GenesisRuntime before = runtime;
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION &&
         transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_SOFTWARE_EXCEPTION);
  assert(runtime.pc == before.pc && runtime.sr == before.sr && runtime.a[7] == before.a[7] && runtime.usp == before.usp);
  assert(memcmp(runtime.d, before.d, sizeof runtime.d) == 0 && memcmp(runtime.a, before.a, sizeof runtime.a) == 0);
  assert(memcmp(runtime.work_ram, before.work_ram, sizeof runtime.work_ram) == 0);
}
int main(void) {
  GenesisRuntime runtime;
  /* TRAP #0 in supervisor mode: vector 32, next instruction stacked, S/mask/CCR kept. */
  runtime = fresh(0x1008, 0x2715);
  expect_entry(&runtime, 0x100A);
  assert(runtime.sr == 0x2715 && frame_sr(&runtime, 0x00FF03FA) == 0x2715 && runtime.usp == 0x00FF0600);
  /* TRAP #15 in user mode: the frame goes on the SSP (inactive slot), saved S = 0, the USP moves to the slot. */
  runtime = fresh(0x100A, 0x0015);
  expect_entry(&runtime, 0x100C);
  assert(runtime.sr == 0x2015 && runtime.a[7] == 0x00FF06FA && runtime.usp == 0x00FF0500 &&
         frame_sr(&runtime, 0x00FF06FA) == 0x0015);
  /* The trace bit is cleared on entry. */
  runtime = fresh(0x1008, 0xA700);
  expect_entry(&runtime, 0x100A);
  assert(runtime.sr == 0x2700 && frame_sr(&runtime, 0x00FF03FA) == 0xA700);
  /* No build-resolved handler for vector 47 / an odd SSP: fail closed, nothing changed. */
  runtime = fresh(0x100A, 0x2700); runtime.software_exception_handler_present[47] = 0U;
  expect_fail_closed(runtime);
  runtime = fresh(0x1008, 0x2700); runtime.a[7] = 0x00FF0401;
  expect_fail_closed(runtime);
  runtime = fresh(0x1008, 0x0000); runtime.usp = 0x00000004; /* SSP outside work RAM */
  expect_fail_closed(runtime);
  /* TRAPV: V = 0 retires in 4 cycles; V = 1 takes vector 7 with the next instruction stacked. */
  runtime = fresh(0x100C, 0x2700);
  step(&runtime, 0x100E, 4);
  assert(runtime.sr == 0x2700 && runtime.a[7] == 0x00FF0400);
  runtime = fresh(0x100C, 0x0002);
  expect_entry(&runtime, 0x100E);
  assert(frame_sr(&runtime, 0x00FF06FA) == 0x0002);
  runtime = fresh(0x100C, 0x2702); runtime.software_exception_handler_present[7] = 0U;
  expect_fail_closed(runtime);
  /* CHK.W D1,D0 in range: Z <- (Dn.W == 0), V = C = 0, N and X unchanged, 10 cycles. */
  runtime = fresh(0x100E, 0x271F); runtime.d[0] = 3; runtime.d[1] = 5;
  step(&runtime, 0x1010, 10);
  assert(runtime.sr == 0x2718);
  runtime = fresh(0x100E, 0x2700); runtime.d[0] = 0xFFFF0000; runtime.d[1] = 5;
  step(&runtime, 0x1010, 10);
  assert(runtime.sr == 0x2704);
  /* Dn.W < 0: vector 6, N set; the saved SR carries the new flags; the next instruction is stacked. */
  runtime = fresh(0x100E, 0x2703); runtime.d[0] = 0x00008000; runtime.d[1] = 5;
  expect_entry(&runtime, 0x1010);
  assert(frame_sr(&runtime, 0x00FF03FA) == 0x2708 && runtime.sr == 0x2708 && runtime.d[0] == 0x00008000);
  /* Dn.W > bound: vector 6, N cleared. */
  runtime = fresh(0x100E, 0x0008); runtime.d[0] = 6; runtime.d[1] = 0x12340005;
  expect_entry(&runtime, 0x1010);
  assert(frame_sr(&runtime, 0x00FF06FA) == 0x0000);
  /* Negative bound: 1 > -1 traps with N cleared. */
  runtime = fresh(0x100E, 0x2708); runtime.d[0] = 1; runtime.d[1] = 0xFFFF;
  expect_entry(&runtime, 0x1010);
  assert(frame_sr(&runtime, 0x00FF03FA) == 0x2700);
  /* CHK.W (A0)+,D2: the bound is read through the routed gate, A0 advances by two (14 cycles in range) and stays
     advanced when the check traps; a failed routed read changes nothing. */
  runtime = fresh(0x1010, 0x2700); runtime.a[0] = 0x00FF0800; runtime.work_ram[0x800] = 0x00; runtime.work_ram[0x801] = 0x10;
  runtime.d[2] = 4;
  step(&runtime, 0x1012, 14);
  assert(runtime.a[0] == 0x00FF0802);
  runtime = fresh(0x1010, 0x2700); runtime.a[0] = 0x00FF0800; runtime.work_ram[0x801] = 0x10; runtime.d[2] = 0x20;
  expect_entry(&runtime, 0x1012);
  assert(runtime.a[0] == 0x00FF0802);
  runtime = fresh(0x1010, 0x2700); runtime.a[0] = 0x00000000; runtime.d[2] = 0x20;
  {
    const GenesisRuntime before = runtime;
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class != GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION);
    assert(runtime.a[0] == before.a[0] && runtime.sr == before.sr && runtime.pc == before.pc && runtime.a[7] == before.a[7]);
  }
  /* CHK.W -(A1),D3 (16 cycles; equal to the bound is in range). */
  runtime = fresh(0x1012, 0x2700); runtime.a[1] = 0x00FF0802; runtime.work_ram[0x801] = 0x10; runtime.d[3] = 0x10;
  step(&runtime, 0x1014, 16);
  assert(runtime.a[1] == 0x00FF0800);
  /* CHK.W #$0010,D4 (14 cycles) and its trap. */
  runtime = fresh(0x1014, 0x2700); runtime.d[4] = 0x10;
  step(&runtime, 0x1018, 14);
  runtime = fresh(0x1014, 0x2700); runtime.d[4] = 0x11;
  expect_entry(&runtime, 0x1018);
  /* CHK.W $FF0800.L,D5 (22 cycles) and its trap (N set). */
  runtime = fresh(0x1018, 0x2700); runtime.work_ram[0x801] = 0x10; runtime.d[5] = 7;
  step(&runtime, 0x101E, 22);
  runtime = fresh(0x1018, 0x2700); runtime.work_ram[0x801] = 0x10; runtime.d[5] = 0xFFFF;
  expect_entry(&runtime, 0x101E);
  assert(frame_sr(&runtime, 0x00FF03FA) == 0x2708);
  /* ILLEGAL, line 1010, line 1111 and the MC68010 MOVEC word: vectors 4/10/11/4 with THIS instruction stacked. */
  runtime = fresh(0x101E, 0x2700);
  expect_entry(&runtime, 0x101E);
  runtime = fresh(0x1020, 0x0000);
  expect_entry(&runtime, 0x1020);
  assert(runtime.a[7] == 0x00FF06FA && runtime.usp == 0x00FF0500 && runtime.sr == 0x2000);
  runtime = fresh(0x1022, 0x2700);
  expect_entry(&runtime, 0x1022);
  runtime = fresh(0x1024, 0x2700);
  expect_entry(&runtime, 0x1024);
  runtime = fresh(0x1020, 0x2700); runtime.software_exception_handler_present[10] = 0U;
  expect_fail_closed(runtime);
  runtime = fresh(0x1022, 0x2700); runtime.software_exception_handler_present[11] = 0U;
  expect_fail_closed(runtime);
  runtime = fresh(0x1024, 0x2700); runtime.software_exception_handler_present[4] = 0U;
  expect_fail_closed(runtime);
  /* RTR (supervisor): only X/N/Z/V/C come from the frame word; PC and SP += 6 follow; 20 cycles. */
  runtime = fresh(0x1026, 0x2700);
  runtime.work_ram[0x400] = 0xFF; runtime.work_ram[0x401] = 0xE5;
  runtime.work_ram[0x402] = 0x00; runtime.work_ram[0x403] = 0x00; runtime.work_ram[0x404] = 0x10; runtime.work_ram[0x405] = 0x28;
  step(&runtime, 0x1028, 20);
  assert(runtime.sr == 0x2705 && runtime.a[7] == 0x00FF0406 && runtime.usp == 0x00FF0600);
  /* RTR (user mode, unprivileged): pops the USP; the system byte is unchanged. */
  runtime = fresh(0x1026, 0x0715);
  runtime.work_ram[0x500] = 0x00; runtime.work_ram[0x501] = 0x0A;
  runtime.work_ram[0x502] = 0x00; runtime.work_ram[0x503] = 0x00; runtime.work_ram[0x504] = 0x10; runtime.work_ram[0x505] = 0x08;
  step(&runtime, 0x1008, 20);
  assert(runtime.sr == 0x070A && runtime.a[7] == 0x00FF0506 && runtime.usp == 0x00FF0700);
  /* RTR with an unreadable frame: nothing restored. */
  runtime = fresh(0x1026, 0x2700); runtime.a[7] = 0x00000100;
  {
    const GenesisRuntime before = runtime;
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP);
    assert(runtime.pc == before.pc && runtime.sr == before.sr && runtime.a[7] == before.a[7]);
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
  runtime.pc = 0x0B00; runtime.sr = 0x2700; runtime.a[7] = 0x00FF0400; runtime.usp = 0x00FF0600;
  runtime.work_ram[0x800] = 0x00; runtime.work_ram[0x801] = 0x10;
  runtime.d[4] = 5; runtime.d[5] = 5;
  runtime.software_exception_handler_entry[35] = 0x0B0E; runtime.software_exception_handler_present[35] = 1U;
  runtime.software_exception_handler_entry[6] = 0x0B0E; runtime.software_exception_handler_present[6] = 1U;
  return runtime;
}
int main(void) {
  GenesisRuntime runtime = fresh();
  GenesisControlTransfer transfer;
  /* Both CHKs in range, TRAPV not taken, TRAP #3 enters its handler (the RESET frontier address). */
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0B0E && runtime.a[7] == 0x00FF03FA);
  assert(runtime.work_ram[0x3FA] == 0x27 && runtime.work_ram[0x3FB] == 0x00 && runtime.work_ram[0x3FE] == 0x0B &&
         runtime.work_ram[0x3FF] == 0x0E);
  /* The absolute CHK bound (retained work-RAM fact) traps: vector 6 with the next instruction stacked. */
  runtime = fresh(); runtime.d[5] = 0x11;
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0B0E && runtime.a[7] == 0x00FF03FA);
  assert(runtime.work_ram[0x3FE] == 0x0B && runtime.work_ram[0x3FF] == 0x0A);
  /* The immediate CHK traps with no vector-6 handler installed: fail closed at the CHK, nothing changed. */
  runtime = fresh(); runtime.d[4] = 0x20; runtime.software_exception_handler_present[6] = 0U;
  {
    const GenesisRuntime before = runtime;
    transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION &&
           transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_SOFTWARE_EXCEPTION);
    assert(runtime.pc == 0x0B00 && runtime.a[7] == before.a[7] && runtime.sr == before.sr &&
           memcmp(runtime.work_ram, before.work_ram, sizeof runtime.work_ram) == 0);
  }
  return 0;
}
'''

OBSERVE = (' fprintf(stderr, "d5=%u d6=%u d7=%u a7=%u usp=%u sr=%u pc=%u frame=%02x%02x%02x%02x%02x%02x\\n", '
           '(unsigned)runtime.d[5], (unsigned)runtime.d[6], (unsigned)runtime.d[7], (unsigned)runtime.a[7], '
           '(unsigned)runtime.usp, (unsigned)runtime.sr, (unsigned)runtime.pc, runtime.work_ram[0x7FFA], '
           'runtime.work_ram[0x7FFB], runtime.work_ram[0x7FFC], runtime.work_ram[0x7FFD], runtime.work_ram[0x7FFE], '
           'runtime.work_ram[0x7FFF]);')


def root_text(source, address):
    start = source.index("static GenesisControlTransfer genesis_aot_%s(" % address)
    end = source.find("static GenesisControlTransfer ", start + 10)
    return source[start:end if end != -1 else len(source)]


def check_structure(source):
    """Each exception root raises its build-time vector with the ADR 0043 §3 stacked PC; nothing reads an opcode."""
    for address, vector, stacked in (("00001008", 32, "0000100A"), ("0000100A", 47, "0000100C"),
                                     ("0000100C", 7, "0000100E"), ("0000100E", 6, "00001010"),
                                     ("00001014", 6, "00001018"), ("00001018", 6, "0000101E"),
                                     ("0000101E", 4, "0000101E"), ("00001020", 10, "00001020"),
                                     ("00001022", 11, "00001022"), ("00001024", 4, "00001024")):
        text = root_text(source, address)
        call = "genesis_raise_software_exception(runtime, UINT32_C(%d), UINT32_C(0x%s)" % (vector, stacked)
        assert text.count("genesis_raise_software_exception(") == 1 and call in text, (address, text[:400])
    assert "genesis_return_restore_condition_codes(runtime" in root_text(source, "00001026")
    assert "genesis_raise_software_exception" not in root_text(source, "00001026")


def build_and_run(compiler, root, source_text, harness, name):
    runtime = pathlib.Path(root) / "platforms/genesis/runtime"
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(source_text)
        (path / "harness.c").write_text(harness)
        executable = path / name
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
                                "-o", str(executable)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr + ran.stdout


def run_bridge(emitter, compiler, root, variant):
    generated = subprocess.run([emitter, "--emit-general-startup-bridge-software-exception" + variant],
                               text=True, capture_output=True)
    assert generated.returncode == 0, generated.stderr
    source = generated.stdout
    anchor = re.search(r"result = genesis_runtime_run\([^;]*\);", source)
    assert anchor is not None
    source = source[:anchor.end()] + OBSERVE + source[anchor.end():]
    env = os.environ.copy()
    if not env.get("SDKROOT") and shutil.which("xcrun"):
        env["SDKROOT"] = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True,
                                        check=True).stdout.strip()
    runtime = pathlib.Path(root) / "platforms/genesis/runtime"
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(source)
        binary = path / "software-exception-bridge"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(runtime),
                                str(path / "generated.c"), str(runtime / "runtime.c"), "-o", str(binary)],
                               text=True, capture_output=True, env=env)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(binary)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
        return generated.stdout, json.loads(ran.stdout), ran.stderr


def main():
    emitter, compiler, root = sys.argv[1:]
    aot = subprocess.run([emitter, "--emit-software-exception-aot"], text=True, capture_output=True)
    assert aot.returncode == 0, aot.stderr
    check_structure(aot.stdout)
    build_and_run(compiler, root, aot.stdout, AOT_HARNESS, "software-exception-aot")
    c4 = subprocess.run([emitter, "--emit-software-exception-c4"], text=True, capture_output=True)
    assert c4.returncode == 0 and "translation rejected" not in c4.stdout, c4.stderr + c4.stdout[:300]
    build_and_run(compiler, root, c4.stdout, C4_HARNESS, "software-exception-c4")

    # Build-time-resolved handlers entered from supervisor and user mode; RTE back; RTR; the RESET frontier.
    source, report, observed = run_bridge(emitter, compiler, root, "")
    for vector, handler in ((4, "00000190"), (6, "00000180"), (7, "00000180"), (10, "00000190"), (11, "00000190"),
                            (32, "00000180"), (33, "00000180")):
        assert ("runtime.software_exception_handler_entry[%d] = UINT32_C(0x%s); "
                "runtime.software_exception_handler_present[%d] = 1;" % (vector, handler, vector)) in source, vector
    assert "software_exception_handler_entry[5]" not in source and "software_exception_handler_entry[34]" not in source
    assert report["result"] == "stop" and report["stop_class"] == "unsupported_cpu_form", report
    # D7 = TRAP #0 + TRAP #1 + TRAPV + CHK; D5 = ILLEGAL + line 1010 + line 1111 (each skipped by its handler);
    # D6 = SR after RTR (user mode, CCR = X|C); A7 = USP; the SSP is parked in the inactive slot; the last frame on
    # the SSP is the line-1111 frame whose stacked PC the handler advanced past the word.
    expected = "d5=3 d6=17 d7=4 a7=16740352 usp=16744448 sr=17 pc=312 frame=000000000128"
    assert expected in observed, observed
    # No handler installed for any software-exception vector: the first TRAP stops fail-closed, nothing changed.
    source, report, observed = run_bridge(emitter, compiler, root, "-no-handler")
    assert "software_exception_handler_present" not in source
    assert (report["stop_class"], report["diagnostic_category"]) == (
        "unsupported_cpu_exception", "unsupported_software_exception"), report
    assert "d7=0" in observed and "a7=16744448" in observed and "sr=9984 pc=264" in observed, observed
    print("genesis_software_exception_generated_test: OK")


if __name__ == "__main__":
    main()
