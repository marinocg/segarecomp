#!/usr/bin/env python3
"""SEG-021-T020 / ADR 0043 §3, §7, ADR 0041: strict-C11 generated-native proof of STOP #imm and the MC68000 interrupt
acceptance contract on the Genesis binding, on every generated-native route.

1. Immutable-ROM AOT roots (`m68k_pipeline_tests --emit-stop-aot`), executed through the real Genesis runtime against
   hand-derived Motorola results: STOP loads SR and halts; the retirement boundary (the machine scheduler) advances
   virtual time exactly to the next VBlank onset, where the level-6 request is accepted with the instruction after STOP
   stacked (six-byte frame, SR <- S = 1, T = 0, I = 6); a request that is already pending is accepted at STOP's own
   boundary; a mask STOP loads that blocks level 6, a disabled source (IE0 clear) or an uninstalled handler ends with
   the explicit stopped_without_wake_source diagnostic after exactly STOP's own 4 cycles (never a spin); user mode
   raises vector 8 with STOP itself stacked and nothing executed; T = 1 fails closed (deferred trace) with nothing
   committed; STOP #$0000 swaps to the USP and the wake frame still goes on the SSP; determinism.
2. The ordinary whole-program C4 route (`--emit-stop-c4`).
3. The whole-program bridge (`--emit-general-startup-bridge-stop[-no-handler]`): the level-6 handler is resolved and
   rooted at build time; the program enables IE0, STOPs, is woken by VBlank IRQ6, returns with RTE to the instruction
   after STOP and ends at a second STOP with no wake source (sanitized report pair
   unsupported_interrupt_or_scheduling_event / stopped_without_wake_source).
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
#define HANDLER UINT32_C(0x00001018)
#define ONSET ((uint64_t)GENESIS_NTSC_VBLANK_ONSET_TICK)
#define FRAME ((uint64_t)GENESIS_NTSC_MASTER_TICKS_PER_FRAME)
static GenesisRuntime fresh(uint32_t pc, uint16_t sr) {
  GenesisRuntime runtime = (GenesisRuntime){0};
  runtime.pc = pc; runtime.sr = sr;
  if ((sr & 0x2000U) != 0U) { runtime.a[7] = 0x00FF0400; runtime.usp = 0x00FF0600; }
  else { runtime.a[7] = 0x00FF0600; runtime.usp = 0x00FF0400; }
  runtime.irq6_handler_entry = HANDLER; runtime.irq6_handler_present = 1U;
  runtime.privilege_violation_handler_entry = HANDLER; runtime.privilege_violation_handler_present = 1U;
  runtime.devices.vdp.registers[1] = 0x0020U; /* IE0 */
  return runtime;
}
static uint32_t frame_pc(const GenesisRuntime *r, uint32_t base) {
  const uint8_t *m = r->work_ram + (base - 0x00FF0000U);
  return ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
}
static uint16_t frame_sr(const GenesisRuntime *r, uint32_t base) {
  const uint8_t *m = r->work_ram + (base - 0x00FF0000U);
  return (uint16_t)((m[0] << 8) | m[1]);
}
/* Whole CPU cycles from `from` to the first VBlank onset strictly after `from + 4 cycles` (STOP's own retirement). */
static uint64_t wake_ticks(uint64_t from) {
  const uint64_t after_stop = from + 4U * GENESIS_M68K_CYCLE_MASTER_TICKS;
  const uint64_t target = after_stop < ONSET ? ONSET : ONSET + ((after_stop - ONSET) / FRAME + 1U) * FRAME;
  const uint64_t cycles = (target - after_stop + GENESIS_M68K_CYCLE_MASTER_TICKS - 1U) / GENESIS_M68K_CYCLE_MASTER_TICKS;
  return after_stop + cycles * GENESIS_M68K_CYCLE_MASTER_TICKS;
}
/* STOP woken by the next VBlank: frame on the SSP with the instruction after STOP stacked, SR <- S | I = 6. */
static void expect_wake(GenesisRuntime *runtime, uint32_t next_pc, uint16_t stop_sr, uint32_t ssp) {
  const uint64_t before = runtime->scheduler.master_ticks;
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  if (transfer.kind != GENESIS_CONTINUE_AT_PC || runtime->pc != HANDLER)
    fprintf(stderr, "wake: kind=%d pc=%04X diag=%d\n", (int)transfer.kind, (unsigned)runtime->pc,
            (int)transfer.stop.diagnostic_category);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == HANDLER && runtime->pc == HANDLER);
  assert(runtime->scheduler.master_ticks == wake_ticks(before));
  assert(runtime->a[7] == ssp - 6U && frame_pc(runtime, ssp - 6U) == next_pc && frame_sr(runtime, ssp - 6U) == stop_sr);
  assert(runtime->sr == (uint16_t)((stop_sr & 0x78FFU) | 0x2600U));
  assert(runtime->m68k_interrupt.stopped == 0U && runtime->devices.interrupt.vblank_pending == 0U);
}
/* STOP with no possible wake source: STOP completed (SR loaded, PC after STOP), 4 cycles only, explicit diagnostic. */
static void expect_no_wake(GenesisRuntime *runtime, uint32_t next_pc, uint16_t stop_sr) {
  const uint64_t before = runtime->scheduler.master_ticks;
  const GenesisControlTransfer transfer = genesis_bridge_dispatch(runtime);
  assert(transfer.kind == GENESIS_STOP &&
         transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT &&
         transfer.stop.diagnostic_category == GENESIS_DIAG_STOPPED_WITHOUT_WAKE_SOURCE);
  assert(runtime->pc == next_pc && runtime->sr == stop_sr && runtime->m68k_interrupt.stopped == 1U);
  assert(runtime->scheduler.master_ticks - before == 4U * GENESIS_M68K_CYCLE_MASTER_TICKS);
}
int main(void) {
  GenesisRuntime runtime, again;
  /* STOP #$2000 from reset time: woken exactly at the first VBlank onset. */
  runtime = fresh(0x1008, 0x2700);
  expect_wake(&runtime, 0x100C, 0x2000, 0x00FF0400);
  assert(runtime.scheduler.master_ticks == ONSET && runtime.devices.interrupt.vblank_transition_count == 1U);
  /* Determinism: the identical start reaches the identical state. */
  again = fresh(0x1008, 0x2700);
  expect_wake(&again, 0x100C, 0x2000, 0x00FF0400);
  assert(memcmp(&runtime, &again, sizeof runtime) == 0);
  /* Mid-frame, past this frame's onset: the wait runs to the NEXT frame's onset. */
  runtime = fresh(0x1008, 0x2700); runtime.scheduler.master_ticks = ONSET + 5U;
  expect_wake(&runtime, 0x100C, 0x2000, 0x00FF0400);
  assert(runtime.scheduler.master_ticks >= ONSET + FRAME && runtime.scheduler.master_ticks < ONSET + FRAME + 7U);
  /* A request already pending is accepted at STOP's own boundary (4 cycles, no wait). */
  runtime = fresh(0x1008, 0x2700); runtime.devices.interrupt.vblank_pending = 1U;
  {
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime.pc == HANDLER);
    assert(runtime.scheduler.master_ticks == 4U * GENESIS_M68K_CYCLE_MASTER_TICKS);
    assert(frame_pc(&runtime, 0x00FF03FA) == 0x100C && runtime.sr == 0x2600 && runtime.m68k_interrupt.stopped == 0U);
  }
  /* The mask STOP loads (6) blocks the only wired source: no wake source. */
  runtime = fresh(0x100C, 0x2000);
  expect_no_wake(&runtime, 0x1010, 0x2600);
  /* A pending request that the loaded mask blocks is still no wake source (it stays pending). */
  runtime = fresh(0x100C, 0x2000); runtime.devices.interrupt.vblank_pending = 1U;
  expect_no_wake(&runtime, 0x1010, 0x2600);
  assert(runtime.devices.interrupt.vblank_pending == 1U);
  /* VBlank interrupts disabled (IE0 clear) or no installed level-6 handler: no wake source. */
  runtime = fresh(0x1008, 0x2700); runtime.devices.vdp.registers[1] = 0U;
  expect_no_wake(&runtime, 0x100C, 0x2000);
  runtime = fresh(0x1008, 0x2700); runtime.irq6_handler_present = 0U;
  expect_no_wake(&runtime, 0x100C, 0x2000);
  /* User mode: vector 8 with STOP itself stacked; nothing of STOP executes (SR, stopped, time). */
  runtime = fresh(0x1008, 0x0015);
  {
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_CONTINUE_AT_PC && runtime.pc == HANDLER);
    assert(runtime.a[7] == 0x00FF03FA && runtime.usp == 0x00FF0600 && frame_pc(&runtime, 0x00FF03FA) == 0x1008 &&
           frame_sr(&runtime, 0x00FF03FA) == 0x0015 && runtime.sr == 0x2015);
    assert(runtime.m68k_interrupt.stopped == 0U && runtime.scheduler.master_ticks == 0U);
  }
  /* T = 1 in the immediate: deferred-trace stop, nothing committed. */
  runtime = fresh(0x1014, 0x2700);
  {
    const GenesisRuntime before = runtime;
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP && transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION &&
           transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_TRACE_EXCEPTION);
    assert(memcmp(&runtime, &before, sizeof runtime) == 0);
  }
  /* STOP #$0000 from supervisor mode: S clears (A7 <- USP, the SSP is parked), mask 0; the wake frame goes on the
     SSP and entry swaps back (A7 = SSP - 6, USP restored to the inactive slot). */
  runtime = fresh(0x1010, 0x2700);
  expect_wake(&runtime, 0x1014, 0x0000, 0x00FF0400);
  assert(runtime.usp == 0x00FF0600 && runtime.sr == 0x2600);
  /* An interrupt frame that cannot be built fails closed with the interrupt diagnostic (odd SSP). */
  runtime = fresh(0x1008, 0x2700); runtime.a[7] = 0x00FF0401;
  {
    const GenesisControlTransfer transfer = genesis_bridge_dispatch(&runtime);
    assert(transfer.kind == GENESIS_STOP &&
           transfer.stop.stop_class == GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT &&
           transfer.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT);
    assert(runtime.a[7] == 0x00FF0401 && runtime.pc == 0x100C && runtime.sr == 0x2000);
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
  runtime.pc = 0x0B00; runtime.sr = 0x2700; runtime.a[7] = 0x00FF0400; runtime.usp = 0x00FF0600;
  /* No level-6 handler: MOVEQ, then STOP #$2000 has no wake source; the run ends there (never at RESET). */
  do { transfer = genesis_bridge_dispatch(&runtime); } while (transfer.kind == GENESIS_CONTINUE_AT_PC);
  assert(transfer.kind == GENESIS_STOP && transfer.stop.diagnostic_category == GENESIS_DIAG_STOPPED_WITHOUT_WAKE_SOURCE);
  assert(runtime.pc == 0x0B06 && runtime.d[0] == 1U && runtime.d[1] == 0U && runtime.sr == 0x2000);
  /* With a level-6 handler (the block entry) and IE0 set: one dispatch runs MOVEQ and STOP, is woken at the VBlank
     onset and enters the handler with the instruction after STOP (0xB06) stacked. */
  runtime = (GenesisRuntime){0};
  runtime.pc = 0x0B00; runtime.sr = 0x2700; runtime.a[7] = 0x00FF0400; runtime.usp = 0x00FF0600;
  runtime.irq6_handler_entry = 0x0B00; runtime.irq6_handler_present = 1U; runtime.devices.vdp.registers[1] = 0x0020U;
  transfer = genesis_bridge_dispatch(&runtime);
  assert(transfer.kind == GENESIS_CONTINUE_AT_PC && transfer.next_pc == 0x0B00 && runtime.pc == 0x0B00);
  assert(runtime.d[0] == 1U && runtime.d[1] == 0U && runtime.a[7] == 0x00FF03FA && runtime.sr == 0x2600);
  assert(runtime.work_ram[0x3FA] == 0x20 && runtime.work_ram[0x3FB] == 0x00 && runtime.work_ram[0x3FE] == 0x0B &&
         runtime.work_ram[0x3FF] == 0x06);
  assert(runtime.scheduler.master_ticks == (uint64_t)GENESIS_NTSC_VBLANK_ONSET_TICK);
  return 0;
}
'''

OBSERVE = (' fprintf(stderr, "d6=%u d7=%u a7=%u sr=%u pc=%u stopped=%u ticks=%llu\\n", (unsigned)runtime.d[6], '
           '(unsigned)runtime.d[7], (unsigned)runtime.a[7], (unsigned)runtime.sr, (unsigned)runtime.pc, '
           '(unsigned)runtime.m68k_interrupt.stopped, (unsigned long long)runtime.scheduler.master_ticks);')


def root_text(source, address):
    start = source.index("static GenesisControlTransfer genesis_aot_%s(" % address)
    end = source.find("static GenesisControlTransfer ", start + 10)
    return source[start:end if end != -1 else len(source)]


def check_structure(source):
    """Each STOP root: privilege guard, immediate SR write, PC advance, stopped mark, 4-cycle retirement; no loop."""
    for address, immediate in (("00001008", "2000"), ("0000100C", "2600"), ("00001010", "0000"),
                               ("00001014", "8700")):
        text = root_text(source, address)
        order = [text.find("genesis_raise_privilege_violation(runtime, UINT32_C(0x%s)" % address),
                 text.find("UINT32_C(0x%s)) & UINT32_C(0xA71F)" % immediate), text.find("pc += UINT32_C(4);"),
                 text.find("genesis_m68k_enter_stopped_state(runtime);"),
                 text.find("genesis_runtime_retire_m68k_instruction(runtime, UINT32_C(4), runtime->pc)")]
        assert all(i >= 0 for i in order) and order == sorted(order), (address, order, text[:600])
        assert "while" not in text and "for (" not in text, address


def compiler_env():
    env = os.environ.copy()
    if not env.get("SDKROOT") and shutil.which("xcrun"):
        env["SDKROOT"] = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True,
                                        check=True).stdout.strip()
    return env


def build_and_run(compiler, root, source_text, harness, name):
    runtime = pathlib.Path(root) / "platforms/genesis/runtime"
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(source_text)
        (path / "harness.c").write_text(harness)
        executable = path / name
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                "-I", str(runtime), "-I", str(path), str(path / "harness.c"), str(runtime / "runtime.c"),
                                "-o", str(executable)], text=True, capture_output=True, env=compiler_env())
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(executable)], text=True, capture_output=True)
        assert ran.returncode == 0, ran.stderr + ran.stdout


def run_bridge(emitter, compiler, root, variant):
    generated = subprocess.run([emitter, "--emit-general-startup-bridge-stop" + variant],
                               text=True, capture_output=True)
    assert generated.returncode == 0, generated.stderr
    source = generated.stdout
    anchor = re.search(r"result = genesis_runtime_run\([^;]*\);", source)
    assert anchor is not None
    source = source[:anchor.end()] + OBSERVE + source[anchor.end():]
    runtime = pathlib.Path(root) / "platforms/genesis/runtime"
    outputs = []
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary)
        (path / "generated.c").write_text(source)
        binary = path / "stop-bridge"
        built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(runtime),
                                str(path / "generated.c"), str(runtime / "runtime.c"), "-o", str(binary)],
                               text=True, capture_output=True, env=compiler_env())
        assert built.returncode == 0, built.stderr
        for _ in range(2):  # determinism: two runs, byte-identical report and observed state
            ran = subprocess.run([str(binary)], text=True, capture_output=True)
            assert ran.returncode == 0, ran.stderr
            outputs.append((ran.stdout, ran.stderr))
    assert outputs[0] == outputs[1], outputs
    return generated.stdout, json.loads(outputs[0][0]), outputs[0][1]


def main():
    emitter, compiler, root = sys.argv[1:]
    aot = subprocess.run([emitter, "--emit-stop-aot"], text=True, capture_output=True)
    assert aot.returncode == 0, aot.stderr
    check_structure(aot.stdout)
    build_and_run(compiler, root, aot.stdout, AOT_HARNESS, "stop-aot")
    c4 = subprocess.run([emitter, "--emit-stop-c4"], text=True, capture_output=True)
    assert c4.returncode == 0 and "translation rejected" not in c4.stdout, c4.stderr + c4.stdout[:300]
    assert "genesis_m68k_enter_stopped_state(runtime);" in c4.stdout
    build_and_run(compiler, root, c4.stdout, C4_HARNESS, "stop-c4")

    # Build-time-resolved level-6 handler; STOP woken by VBlank IRQ6; RTE to the instruction after STOP; the second
    # STOP (mask 7) has no wake source and ends the run with the explicit diagnostic.
    source, report, observed = run_bridge(emitter, compiler, root, "")
    assert "runtime.irq6_handler_entry = UINT32_C(0x00000180)" in source, source[:2000]
    assert (report["result"], report["stop_class"], report["diagnostic_category"]) == (
        "stop", "unsupported_interrupt_or_scheduling_event", "stopped_without_wake_source"), report
    # D7 = one handler entry; D6 = SR after RTE (the STOP #$2000 SR); SR = the second STOP's; PC after it; stopped.
    assert "d6=8192 d7=1 a7=16744448 sr=9984 pc=274 stopped=1" in observed, observed
    # No level-6 handler: the first STOP already has no wake source; nothing ran after it.
    source, report, observed = run_bridge(emitter, compiler, root, "-no-handler")
    assert "irq6_handler_present" not in source
    assert (report["stop_class"], report["diagnostic_category"]) == (
        "unsupported_interrupt_or_scheduling_event", "stopped_without_wake_source"), report
    assert "d6=0 d7=0 a7=16744448 sr=8192 pc=268 stopped=1" in observed, observed
    print("genesis_stop_interrupt_generated_test: OK")


if __name__ == "__main__":
    main()
