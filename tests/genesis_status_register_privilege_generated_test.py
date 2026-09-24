#!/usr/bin/env python3
"""SEG-021-T018 / ADR 0043: strict-C11 generated-native proof of the supervisor/user model and the status-register
/ USP transfer family on every generated-native admission route.

1. Immutable-ROM AOT roots (`m68k_pipeline_tests --emit-status-register-aot`): MOVE to SR from Dn/(An)+/-(An)/abs.L/#imm,
   MOVE to CCR, MOVE from SR to Dn/-(An)/abs.L, MOVE USP both directions, ANDI/ORI/EORI to SR and CCR, and RTE, each
   executed through the real Genesis runtime against hand-derived Motorola M68000 Family Programmer's Reference Manual
   results: implemented-bit masking, the SSP/USP swap on every S change (the old A7 auto-update commits before the
   swap), privilege violation in user mode (vector 8, six-byte frame on the SSP, stacked PC = the privileged
   instruction, saved SR with S = 0, nothing of the instruction executed), the fail-closed missing-handler and
   deferred-trace stops (nothing changed), RTE restoring S = 0 onto the USP, published retirement cycles.
2. The ordinary whole-program C4 route (`--emit-status-register-c4`): the same family in one straight-line block with
   retained work-RAM facts for the absolute operands, ending in user mode.
3. The whole-program bridge (`--emit-general-startup-bridge-privilege-violation[-no-handler|-trace]`): a build-time
   resolved vector-8 handler entered from user mode, returning with RTE to user mode; the sanitized reports of the
   missing-handler and deferred-trace stops.
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
static uint64_t cycles_of(const GenesisRuntime *runtime) { return runtime->scheduler.master_ticks / GENESIS_M68K_CYCLE_MASTER_TICKS; }
static GenesisRuntime fresh(uint32_t pc, uint16_t sr) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  runtime.pc = pc; runtime.sr = sr;
  runtime.a[7] = 0x00FF0400; runtime.usp = 0x00FF0600;
  runtime.privilege_violation_handler_entry = 0x00001044; runtime.privilege_violation_handler_present = 1U;
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
static uint32_t frame_pc(const GenesisRuntime *r, uint32_t base) {
  const uint8_t *m = r->work_ram + (base - 0x00FF0000U);
  return ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
}
static uint16_t frame_sr(const GenesisRuntime *r, uint32_t base) {
  const uint8_t *m = r->work_ram + (base - 0x00FF0000U);
  return (uint16_t)((m[0] << 8) | m[1]);
}
/* User mode: vector 8 on the SSP (inactive slot 0xFF0700), stacked PC = the instruction, S = 0 saved, no cycles
   retired, and nothing of the privileged instruction executed. */
static void expect_privilege_violation(uint32_t pc) {
  GenesisRuntime runtime = fresh(pc, 0x0015);
  runtime.a[7] = 0x00FF0500; runtime.usp = 0x00FF0700; runtime.d[1] = 0x00002700; runtime.a[0] = 0x00FF0800;
  runtime.a[2] = 0x11111111; runtime.a[3] = 0x22222222;
  const GenesisRuntime before = runtime;
  step(&runtime, 0x00001044, 0);
  assert(runtime.sr == 0x2015 && runtime.a[7] == 0x00FF06FA && runtime.usp == 0x00FF0500);
  assert(frame_sr(&runtime, 0x00FF06FA) == 0x0015 && frame_pc(&runtime, 0x00FF06FA) == pc);
  assert(runtime.d[1] == before.d[1] && runtime.a[0] == before.a[0] && runtime.a[2] == before.a[2] && runtime.a[3] == before.a[3]);
  /* Without a build-resolved vector-8 handler: fail-closed stop, nothing changed. */
  runtime = before; runtime.privilege_violation_handler_present = 0U;
  {
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION &&
           transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_PRIVILEGE_VIOLATION_EXCEPTION);
    assert(runtime.pc == pc && runtime.sr == 0x0015 && runtime.a[7] == 0x00FF0500 && runtime.usp == 0x00FF0700);
    assert(memcmp(runtime.work_ram, before.work_ram, sizeof runtime.work_ram) == 0);
  }
}
static void expect_trace_stop(GenesisRuntime runtime) {
  const GenesisRuntime before = runtime;
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION &&
         transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_TRACE_EXCEPTION);
  assert(runtime.pc == before.pc && runtime.sr == before.sr && runtime.a[7] == before.a[7] && runtime.usp == before.usp &&
         runtime.a[0] == before.a[0]);
}
int main(void) {
  GenesisRuntime runtime;
  /* MOVE D1,SR: masked to the implemented bits; S cleared -> the USP becomes active (12 cycles). */
  runtime = fresh(0x1008, 0x2700); runtime.d[1] = 0x12345F15;
  step(&runtime, 0x100A, 12);
  assert(runtime.sr == 0x0715 && runtime.a[7] == 0x00FF0600 && runtime.usp == 0x00FF0400);
  /* MOVE D1,SR keeping S: no swap. */
  runtime = fresh(0x1008, 0x2700); runtime.d[1] = 0x0000271F;
  step(&runtime, 0x100A, 12);
  assert(runtime.sr == 0x271F && runtime.a[7] == 0x00FF0400 && runtime.usp == 0x00FF0600);
  /* T = 1: deferred trace, nothing committed. */
  runtime = fresh(0x1008, 0x2700); runtime.d[1] = 0x0000A700;
  expect_trace_stop(runtime);
  /* MOVE (A0)+,SR (16 cycles). */
  runtime = fresh(0x100A, 0x2700); runtime.a[0] = 0x00FF0800; runtime.work_ram[0x800] = 0x20; runtime.work_ram[0x801] = 0x15;
  step(&runtime, 0x100C, 16);
  assert(runtime.sr == 0x2015 && runtime.a[0] == 0x00FF0802);
  /* MOVE (A0)+,SR with T = 1: the deferred-trace stop commits neither SR nor A0. */
  runtime = fresh(0x100A, 0x2700); runtime.a[0] = 0x00FF0800; runtime.work_ram[0x800] = 0x80; runtime.work_ram[0x801] = 0x00;
  expect_trace_stop(runtime);
  /* MOVE -(A7),SR clearing S: A7 (the SSP) is decremented first, then the stacks swap (18 cycles). */
  runtime = fresh(0x100C, 0x2700); runtime.a[7] = 0x00FF0402; runtime.work_ram[0x400] = 0x00; runtime.work_ram[0x401] = 0x15;
  step(&runtime, 0x100E, 18);
  assert(runtime.sr == 0x0015 && runtime.a[7] == 0x00FF0600 && runtime.usp == 0x00FF0400);
  /* MOVE $FF0800.L,SR (24 cycles). */
  runtime = fresh(0x100E, 0x2700); runtime.work_ram[0x800] = 0x27; runtime.work_ram[0x801] = 0x04;
  step(&runtime, 0x1014, 24);
  assert(runtime.sr == 0x2704);
  /* MOVE #$2700,SR (16 cycles). */
  runtime = fresh(0x1014, 0x271F);
  step(&runtime, 0x1018, 16);
  assert(runtime.sr == 0x2700);
  /* MOVE (A0),CCR: only X/N/Z/V/C from the source's low byte (16 cycles). */
  runtime = fresh(0x1018, 0x2700); runtime.a[0] = 0x00FF0800; runtime.work_ram[0x800] = 0xFF; runtime.work_ram[0x801] = 0xF5;
  step(&runtime, 0x101A, 16);
  assert(runtime.sr == 0x2715 && runtime.a[0] == 0x00FF0800);
  /* MOVE #$15,CCR is unprivileged: it runs in user mode (16 cycles). */
  runtime = fresh(0x101A, 0x0700);
  step(&runtime, 0x101E, 16);
  assert(runtime.sr == 0x0715);
  /* MOVE SR,D3 in user mode (unprivileged on the MC68000): low word only (6 cycles). */
  runtime = fresh(0x101E, 0x0715); runtime.d[3] = 0xAAAA5555;
  step(&runtime, 0x1020, 6);
  assert(runtime.d[3] == 0xAAAA0715 && runtime.sr == 0x0715);
  /* MOVE SR,-(A0) (14 cycles). */
  runtime = fresh(0x1020, 0x2704); runtime.a[0] = 0x00FF0A02;
  step(&runtime, 0x1022, 14);
  assert(runtime.a[0] == 0x00FF0A00 && runtime.work_ram[0xA00] == 0x27 && runtime.work_ram[0xA01] == 0x04);
  /* MOVE SR,$FF0900.L (20 cycles). */
  runtime = fresh(0x1022, 0x2711);
  step(&runtime, 0x1028, 20);
  assert(runtime.work_ram[0x900] == 0x27 && runtime.work_ram[0x901] == 0x11);
  /* MOVE USP,A2 / MOVE A3,USP in supervisor mode (4 cycles each); the USP is the inactive slot. */
  runtime = fresh(0x1028, 0x2700);
  step(&runtime, 0x102A, 4);
  assert(runtime.a[2] == 0x00FF0600 && runtime.a[7] == 0x00FF0400);
  runtime = fresh(0x102A, 0x2700); runtime.a[3] = 0x00FF1234;
  step(&runtime, 0x102C, 4);
  assert(runtime.usp == 0x00FF1234 && runtime.a[7] == 0x00FF0400);
  /* ANDI #$DFFF,SR: clears S, the stacks swap (20 cycles). */
  runtime = fresh(0x102C, 0x2715);
  step(&runtime, 0x1030, 20);
  assert(runtime.sr == 0x0715 && runtime.a[7] == 0x00FF0600 && runtime.usp == 0x00FF0400);
  /* ORI #$0700,SR (20 cycles). */
  runtime = fresh(0x1030, 0x2000);
  step(&runtime, 0x1034, 20);
  assert(runtime.sr == 0x2700 && runtime.a[7] == 0x00FF0400);
  /* EORI #$2000,SR: toggles S off, the stacks swap (20 cycles). */
  runtime = fresh(0x1034, 0x2004);
  step(&runtime, 0x1038, 20);
  assert(runtime.sr == 0x0004 && runtime.a[7] == 0x00FF0600 && runtime.usp == 0x00FF0400);
  /* ANDI/ORI/EORI to CCR are unprivileged and touch only X/N/Z/V/C (20 cycles each). */
  runtime = fresh(0x1038, 0x001F);
  step(&runtime, 0x103C, 20);
  assert(runtime.sr == 0x0010);
  runtime = fresh(0x103C, 0x2700);
  step(&runtime, 0x1040, 20);
  assert(runtime.sr == 0x2711);
  runtime = fresh(0x1040, 0x2715);
  step(&runtime, 0x1044, 20);
  assert(runtime.sr == 0x270A);
  /* RTE restoring S = 0: the USP becomes active and the incremented SSP moves to the inactive slot (20 cycles). */
  runtime = fresh(0x1044, 0x2700);
  runtime.work_ram[0x400] = 0x00; runtime.work_ram[0x401] = 0x15;
  runtime.work_ram[0x402] = 0x00; runtime.work_ram[0x403] = 0x00; runtime.work_ram[0x404] = 0x10; runtime.work_ram[0x405] = 0x08;
  step(&runtime, 0x1008, 20);
  assert(runtime.sr == 0x0015 && runtime.a[7] == 0x00FF0600 && runtime.usp == 0x00FF0406);
  /* RTE of a T = 1 frame: deferred trace, nothing restored. */
  runtime = fresh(0x1044, 0x2700); runtime.work_ram[0x400] = 0x80; runtime.work_ram[0x405] = 0x08;
  expect_trace_stop(runtime);
  /* Every privileged form raises vector 8 in user mode before any part of it executes. */
  expect_privilege_violation(0x1008);
  expect_privilege_violation(0x100A);
  expect_privilege_violation(0x100E);
  expect_privilege_violation(0x1014);
  expect_privilege_violation(0x1028);
  expect_privilege_violation(0x102A);
  expect_privilege_violation(0x102C);
  expect_privilege_violation(0x1030);
  expect_privilege_violation(0x1034);
  expect_privilege_violation(0x1044);
  /* A frame that cannot be placed on the SSP fails closed (odd SSP): nothing changes. */
  runtime = fresh(0x1008, 0x0015); runtime.usp = 0x00FF0701;
  {
    const GenesisRuntime before = runtime;
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION &&
           transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_PRIVILEGE_VIOLATION_EXCEPTION);
    assert(runtime.pc == before.pc && runtime.sr == before.sr && runtime.a[7] == before.a[7] && runtime.usp == before.usp);
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
  runtime.pc = 0x0B00; runtime.sr = 0x2700;
  runtime.a[7] = 0x00FF0400; runtime.usp = 0x00FF0600; runtime.a[3] = 0x00FF1234;
  runtime.work_ram[0x800] = 0x27; runtime.work_ram[0x801] = 0x04;
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && runtime.pc == 0x0B2C);
  assert((runtime.d[3] & 0xFFFFU) == 0x2715);                            /* MOVE SR,D3 after MOVE #$15,CCR */
  assert(runtime.work_ram[0x900] == 0x27 && runtime.work_ram[0x901] == 0x04); /* MOVE $FF0800,SR then SR,$FF0900 */
  assert(runtime.a[2] == 0x00FF0600);                                    /* MOVE USP,A2 */
  assert((runtime.d[4] & 0xFFFFU) == 0x070E && runtime.sr == 0x070E);     /* CCR ops, EORI #$2000,SR, MOVE SR,D4 */
  assert(runtime.a[7] == 0x00FF1234 && runtime.usp == 0x00FF0400);       /* user mode: the USP (set by MOVE A3,USP) */
  return 0;
}
'''

OBSERVE = (' fprintf(stderr, "d6=%u d7=%u a1=%u a7=%u usp=%u sr=%u pc=%u frame=%02x%02x%02x%02x%02x%02x\\n", '
           '(unsigned)runtime.d[6], (unsigned)runtime.d[7], (unsigned)runtime.a[1], (unsigned)runtime.a[7], '
           '(unsigned)runtime.usp, (unsigned)runtime.sr, (unsigned)runtime.pc, runtime.work_ram[0x7FFA], '
           'runtime.work_ram[0x7FFB], runtime.work_ram[0x7FFC], runtime.work_ram[0x7FFD], runtime.work_ram[0x7FFE], '
           'runtime.work_ram[0x7FFF]);')


def root_text(source, address):
    start = source.index("static GenesisControlTransfer genesis_aot_%s(" % address)
    end = source.find("static GenesisControlTransfer ", start + 10)
    return source[start:end if end != -1 else len(source)]


def check_structure(source):
    """Privileged roots test SR.S first and raise vector 8 with their own address; MOVE from SR to memory routes one
    WORD read and then one WORD write to the same EA (read-before-write); MOVE from SR / to CCR are unprivileged."""
    for address in ("00001008", "0000100A", "0000100C", "0000100E", "00001014", "00001028", "0000102A", "0000102C",
                    "00001030", "00001034", "00001044"):
        text = root_text(source, address)
        guard = "if ((runtime->sr & UINT16_C(0x2000)) == 0U) "
        assert text.count(guard) == 1 and "genesis_raise_privilege_violation(runtime, UINT32_C(0x%s)" % address in text, address
        assert text.index(guard) < text.find("genesis_route_access") if "genesis_route_access" in text else True, address
    for address in ("00001018", "0000101A", "0000101E", "00001020", "00001022", "00001038", "0000103C", "00001040"):
        assert "genesis_raise_privilege_violation" not in root_text(source, address), address
    for address in ("00001020", "00001022"):
        text = root_text(source, address)
        calls = [i for i in range(len(text)) if text.startswith("genesis_route_access(", i)]
        assert len(calls) == 2, (address, len(calls))
        assert "GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &" in text[calls[0]:calls[1]], address
        assert "GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &" in text[calls[1]:], address


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
    generated = subprocess.run([emitter, "--emit-general-startup-bridge-privilege-violation" + variant],
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
        binary = path / "privilege-bridge"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(runtime),
                                str(path / "generated.c"), str(runtime / "runtime.c"), "-o", str(binary)],
                               text=True, capture_output=True, env=env)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(binary)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr
        return generated.stdout, json.loads(ran.stdout), ran.stderr


def main():
    emitter, compiler, root = sys.argv[1:]
    aot = subprocess.run([emitter, "--emit-status-register-aot"], text=True, capture_output=True)
    assert aot.returncode == 0, aot.stderr
    check_structure(aot.stdout)
    build_and_run(compiler, root, aot.stdout, AOT_HARNESS, "status-register-aot")
    c4 = subprocess.run([emitter, "--emit-status-register-c4"], text=True, capture_output=True)
    assert c4.returncode == 0 and "translation rejected" not in c4.stdout, c4.stderr + c4.stdout[:300]
    build_and_run(compiler, root, c4.stdout, C4_HARNESS, "status-register-c4")

    # Vector 8 from user mode through the build-time-resolved handler, RTE back to user mode, RESET frontier.
    source, report, observed = run_bridge(emitter, compiler, root, "")
    assert "runtime.privilege_violation_handler_entry = UINT32_C(0x00000180); runtime.privilege_violation_handler_present = 1;" in source
    assert "runtime.sr = UINT16_C(0x2700);" in source
    assert report["result"] == "stop" and report["stop_class"] == "unsupported_cpu_form", report
    # D6 = SR in user mode after RTE; D7 = handler marker; A1 = USP read in the handler; A7 = USP again; the SSP is
    # parked in the inactive slot; the frame holds the saved SR (S = 0) and the stacked PC advanced by the handler.
    expected = ("d6=21 d7=85 a1=16740352 a7=16740352 usp=16744448 sr=21 pc=274 frame=001500000110")
    assert expected in observed, observed
    # No vector-8 handler: the privileged MOVE to SR in user mode stops fail-closed, nothing of it executed.
    source, report, observed = run_bridge(emitter, compiler, root, "-no-handler")
    assert "privilege_violation_handler_present" not in source
    assert (report["stop_class"], report["diagnostic_category"]) == (
        "unsupported_cpu_exception", "unsupported_privilege_violation_exception"), report
    assert "sr=21 pc=268" in observed and "a7=16740352 usp=16744448" in observed, observed
    # T set by an SR write: the deferred-trace stop, before the SR write commits.
    source, report, observed = run_bridge(emitter, compiler, root, "-trace")
    assert (report["stop_class"], report["diagnostic_category"]) == (
        "unsupported_cpu_exception", "unsupported_trace_exception"), report
    assert "a7=16744448 usp=16740352 sr=9984 pc=264" in observed, observed
    print("genesis_status_register_privilege_generated_test: OK")


if __name__ == "__main__":
    main()
