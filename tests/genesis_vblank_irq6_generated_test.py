#!/usr/bin/env python3
"""SEG-007-T047 / ADR-0020: generated-C coverage for the RTE (C4) lowering and
the IRQ6 autovector main wiring.

The focused C++ suite genesis_vblank_irq6_interrupt_tests drives the runtime
with hand-written dispatch closures. This test closes the emitter gap: it emits
the *generated* bridge C for a synthetic general-startup program that carries a
real MC68000 exception vector table whose level-6 autovector resolves to an
in-prefix handler ending in RTE, then asserts the generated output contains the
RTE call-site, the exception-return block terminal, and the generated-main
irq6_handler_entry / irq6_handler_present wiring -- and that it compiles as
strict C11 with warnings enabled and runs deterministically.

ADR-0021 §5 coverage items 3 and 4 extend this file with the two generated-C /
genesis_dispatch EXECUTION-level assertions the focused synthetic C++ suite
(tests/m68k_pipeline_test.cpp) cannot make on its own: that the fully generated
program -- the same emitted main()/genesis_runtime_run wiring production
bridges use, unmodified -- actually admits an IRQ6 interrupt at runtime and
either (item 3) completes the entry -> handler -> RTE -> resumed-mainline round
trip, or (item 4) runs the retained handler prefix to its own
discovery_prefix_boundary cut. Neither fixture's generated `main()` seeds an
admissible initial CPU/VDP state on its own (`GenesisRuntime runtime = {0}`
leaves S = 0 / IE0 clear, so admission is fail-closedly rejected by design) --
`_admission_seeded_source` reproduces the exact proven admission-seeding values
from genesis_vblank_irq6_interrupt_test.cpp (`r.sr = 0x2000` -- supervisor
mode, interrupt mask open; VDP register 1 bit 5 set -- IE0 enabled) as a
textual patch of the generated main, plus one line of test-only instrumentation
that surfaces `runtime.devices.interrupt.vblank_transition_count` (internal
scheduler state, never part of the architectural sanitized/full report) to
`stderr`.

`vblank_transition_count` only proves the VBlank scheduler itself transitioned
-- it does NOT prove IRQ6 was actually admitted, that the retained handler
prefix executed, or that its RTE completed, since a scheduler edge can occur
independently of SR-masked admission. Item 3's own fixture
(emit_general_startup_bridge_irq6_partial_prefix_rte_source) can still reach
its mainline RESET terminal on the unseeded/never-admitted path alone (the
entry's own bounded DBRA countdown loop finishes on its own regardless of
whether IRQ6 ever fires), so `vblank_transition_count >= 1` plus the RESET
terminal does not, by itself, independently prove the required generated-C IRQ
entry -> handler -> RTE -> resumed-mainline round trip. Item 3 therefore also
uses `_marker_instrumented_source`, which surfaces `runtime.d[7]` (an
otherwise-unused register in that fixture; D0 is the mainline's own live
countdown register) to `stderr` after `genesis_runtime_run` returns.
The fixture's retained IRQ6 handler prefix executes `MOVEQ #0x55,D7`
immediately before its own RTE, so a nonzero D7 can only result from that
exact handler instruction genuinely executing -- and, since the marker sits
*before* RTE and the harness still asserts the mainline's own recognizable
RESET terminal is reached afterward, a nonzero D7 alongside that terminal
establishes all three required facts together: the handler executed, its RTE
completed (execution did not stop inside the handler), and control genuinely
resumed the mainline. An unseeded negative control (no admission seeding)
confirms D7 stays at its zero-initialized value while the same RESET terminal
is still reached via the entry loop's own unconditional completion, proving
the marker is admission-gated and not an artifact of the terminal check alone.
No production runtime admission counter or new production ABI is introduced by
either instrumentation; both are textual patches of the generated main()
applied only inside this test.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


def _admission_seeded_source(generated_c):
  """Patch generated bridge C so its own auto-generated main() admits IRQ6.

  Mirrors genesis_vblank_irq6_interrupt_test.cpp's own proven admission
  values (supervisor mode, interrupt mask open, VDP IE0 set) and adds one
  test-only stderr print of the scheduler's own admission counter -- the
  unmodified genesis_runtime_run/genesis_dispatch machinery is otherwise
  untouched.
  """
  anchor = "runtime.irq6_handler_present = 1; "
  assert anchor in generated_c, generated_c[:400]
  seeded = generated_c.replace(
      anchor,
      anchor + "runtime.sr = 0x2000; runtime.devices.vdp.registers[1] = 0x0020; runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28U; ",
      1)
  drive_call = re.search(r"result = genesis_runtime_run\([^;]*\);", seeded)
  assert drive_call is not None, seeded
  instrumentation = (
      ' fprintf(stderr, "vblank_transition_count=%u\\n", '
      '(unsigned)runtime.devices.interrupt.vblank_transition_count);')
  return seeded[:drive_call.end()] + instrumentation + seeded[drive_call.end():]


def _admission_count(run):
  match = re.search(r"vblank_transition_count=(\d+)", run.stderr)
  assert match is not None, run.stderr
  return int(match.group(1))


def _marker_instrumented_source(generated_c):
  """Patch generated bridge C with a test-only stderr print of `runtime.d[7]`
  after `genesis_runtime_run` returns.

  ADR-0021 §5 coverage item 3's fixture executes `MOVEQ #0x55,D7` inside its
  retained IRQ6 handler prefix immediately before RTE; D7 is otherwise unused
  anywhere else in that fixture. Unlike `vblank_transition_count` (a
  scheduler-level VBlank edge, independent of SR-masked admission), a nonzero
  D7 can only result from this exact handler instruction genuinely executing.
  Applied independently of `_admission_seeded_source` so the same
  instrumentation can be layered on both a seeded (admission-enabled) and an
  unseeded (admission-rejected) build of the same generated source.
  """
  drive_call = re.search(r"result = genesis_runtime_run\([^;]*\);", generated_c)
  assert drive_call is not None, generated_c
  instrumentation = (
      ' fprintf(stderr, "handler_marker_d7=%u\\n", (unsigned)runtime.d[7]);')
  return generated_c[:drive_call.end()] + instrumentation + generated_c[drive_call.end():]


def _marker_value(run):
  match = re.search(r"handler_marker_d7=(\d+)", run.stderr)
  assert match is not None, run.stderr
  return int(match.group(1))


def _build_and_run_twice(compiler, runtime_dir, runtime_c, flags, source_text, name):
  with tempfile.TemporaryDirectory() as temp:
    path = pathlib.Path(temp)
    (path / "generated.c").write_text(source_text)
    prog = path / name
    build = subprocess.run(
        [compiler, *flags, "-I", str(runtime_dir),
         str(path / "generated.c"), str(runtime_c), "-o", str(prog)],
        text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    run_a = subprocess.run([str(prog), "--instruction-budget", "40000"], text=True, capture_output=True)
    run_b = subprocess.run([str(prog), "--instruction-budget", "40000"], text=True, capture_output=True)
    assert run_a.returncode == run_b.returncode, (run_a.returncode, run_b.returncode)
    assert run_a.stdout == run_b.stdout, "generated program output must be deterministic"
    assert run_a.stderr == run_b.stderr, "generated program admission instrumentation must be deterministic"
    return run_a


def _check_irq6_partial_prefix_rte(executable, compiler, runtime_dir, runtime_c, flags):
  # ADR-0021 §5 coverage item 3: a runtime path that stays entirely within
  # the retained async prefix and reaches RTE.
  first = subprocess.run([executable, "--emit-general-startup-bridge-irq6-partial-prefix-rte"],
                         text=True, capture_output=True)
  second = subprocess.run([executable, "--emit-general-startup-bridge-irq6-partial-prefix-rte"],
                          text=True, capture_output=True)
  assert first.returncode == 0, first.stderr
  assert second.returncode == 0, second.stderr
  assert first.stdout == second.stdout, "generated bridge C must be deterministic"
  out = first.stdout
  assert not out.startswith("/* translation rejected:"), out[:200]
  assert "genesis_runtime_retire_m68k_instruction" in out, out
  assert "runtime.irq6_handler_present = 1;" in out, out

  # Negative control: without admission seeding, IE0/SR admission is
  # fail-closedly rejected by construction (see module docstring), so the
  # IRQ6 handler must never execute. The entry's own bounded DBRA countdown
  # loop still finishes on its own regardless of whether IRQ6 ever fires, so
  # this unseeded run reaches the SAME recognizable RESET terminal as the
  # seeded run below -- proving that terminal alone is NOT sufficient
  # evidence of the round trip, and that the D7 marker is genuinely
  # admission-gated rather than an artifact of the terminal check.
  unseeded = _marker_instrumented_source(out)
  unseeded_run = _build_and_run_twice(compiler, runtime_dir, runtime_c, flags, unseeded,
                                       "irq6-partial-prefix-rte-unseeded")
  assert _marker_value(unseeded_run) == 0, (
      "item 3 negative control: D7 marker was set without IRQ6 admission -- "
      "the handler executed when it must not have: " + unseeded_run.stderr)
  assert '"stop_class":"unsupported_cpu_form"' in unseeded_run.stdout, unseeded_run.stdout
  assert '"family":"reset"' in unseeded_run.stdout, unseeded_run.stdout

  seeded = _marker_instrumented_source(_admission_seeded_source(out))
  run = _build_and_run_twice(compiler, runtime_dir, runtime_c, flags, seeded,
                              "irq6-partial-prefix-rte")
  assert _admission_count(run) >= 1, (
      "item 3: the seeded IRQ6 admission never fired -- the round trip was never exercised: " +
      run.stderr)
  # The D7 marker is the required positive proof: it is written by the
  # retained IRQ6 handler prefix's own MOVEQ #0x55,D7, immediately before
  # RTE. A nonzero value here can only mean the handler genuinely executed
  # through to its own RTE (execution did not stop inside the handler).
  assert _marker_value(run) == 0x55, (
      "item 3: the D7 execution marker was not set -- the retained IRQ6 "
      "handler prefix did not execute through to its own RTE: " + run.stderr)
  assert '"result":"stop"' in run.stdout, run.stdout
  # The mainline's own entry-block countdown loop only ever reaches its
  # RESET terminal by resuming correctly after every admitted round trip;
  # the exact stop this produces WITHOUT any admission at all (Z always 0,
  # both the live BEQ-fallthrough-to-RTE path and every resumption land back
  # in the same countdown loop) is asserted identical here to prove the
  # round trip is fully transparent to the resumed mainline, per ADR-0020.
  # Together with the D7 marker above, this establishes all three required
  # facts: the handler executed, its RTE completed, and control genuinely
  # resumed the mainline.
  assert '"stop_class":"unsupported_cpu_form"' in run.stdout, run.stdout
  assert '"family":"reset"' in run.stdout, run.stdout
  # The handler's own dead branch (beyond the 256-instruction discovery
  # ceiling) is provably never reached at runtime: the boundary stop class
  # from item 4 below must NOT appear here.
  assert '"stop_class":"discovery_prefix_boundary"' not in run.stdout, run.stdout


def _check_irq6_partial_prefix_boundary(executable, compiler, runtime_dir, runtime_c, flags):
  # ADR-0021 §5 coverage item 4: a runtime path that reaches a prefix cut
  # deterministically reaches the ordinary typed discovery_prefix_boundary
  # stop via genesis_dispatch's existing frontier arm, unmodified.
  first = subprocess.run([executable, "--emit-general-startup-bridge-irq6-partial-prefix-boundary"],
                         text=True, capture_output=True)
  second = subprocess.run([executable, "--emit-general-startup-bridge-irq6-partial-prefix-boundary"],
                          text=True, capture_output=True)
  assert first.returncode == 0, first.stderr
  assert second.returncode == 0, second.stderr
  assert first.stdout == second.stdout, "generated bridge C must be deterministic"
  out = first.stdout
  assert not out.startswith("/* translation rejected:"), out[:200]

  # Without admission the self-looping entry can never leave the reset
  # walk's own block; SEG-007-T252 / ADR-0040: it must exhaust the runner's
  # own finite dispatch allowance (the disjoint runner_resource_limit
  # result, never a guest stop) instead of reaching the boundary, proving
  # the boundary stop below is genuinely admission-gated.
  unseeded = _build_and_run_twice(compiler, runtime_dir, runtime_c, flags, out,
                                   "irq6-partial-prefix-boundary-unseeded")
  assert '"result":"runner_resource_limit"' in unseeded.stdout, unseeded.stdout

  seeded = _admission_seeded_source(out)
  run = _build_and_run_twice(compiler, runtime_dir, runtime_c, flags, seeded,
                              "irq6-partial-prefix-boundary")
  assert _admission_count(run) >= 1, (
      "item 4: the seeded IRQ6 admission never fired -- the boundary was never reached: " +
      run.stderr)
  assert '"result":"stop"' in run.stdout, run.stdout
  assert '"stop_class":"discovery_prefix_boundary"' in run.stdout, run.stdout
  assert '"diagnostic_category":"discovery_budget_exhausted"' in run.stdout, run.stdout


def main():
  executable, compiler, root = sys.argv[1:]
  runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
  runtime_c = runtime_dir / "runtime.c"
  flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]

  first = subprocess.run([executable, "--emit-general-startup-bridge-irq6-rte"],
                         text=True, capture_output=True)
  second = subprocess.run([executable, "--emit-general-startup-bridge-irq6-rte"],
                          text=True, capture_output=True)
  assert first.returncode == 0, first.stderr
  assert second.returncode == 0, second.stderr
  assert first.stdout == second.stdout, "generated bridge C must be deterministic"
  out = first.stdout
  assert not out.startswith("/* translation rejected:"), out[:200]

  # RTE (C4) lowering: the genesis_exception_return call-site and its
  # fail-closed stop plumbing.
  assert "genesis_exception_return(runtime, &m68k_rte_pc, &m68k_rte_stop)" in out, out
  assert "m68k_rte_stop" in out
  # The exception-return block terminal writes the runtime-popped PC back.
  assert "= m68k_rte_pc; }" in out, out
  # No runtime opcode fetch/decode was introduced by this terminal.
  assert "GenesisRuntimeStop m68k_rte_stop = {0}" in out

  # Generated-main IRQ6 autovector wiring (genesis.cpp).
  assert "runtime.irq6_handler_entry = UINT32_C(0x00000120);" in out, out
  assert "runtime.irq6_handler_present = 1;" in out, out

  with tempfile.TemporaryDirectory() as temp:
    path = pathlib.Path(temp)
    (path / "generated.c").write_text(out)
    prog = path / "irq6-rte-bridge"
    build = subprocess.run(
        [compiler, *flags, "-I", str(runtime_dir),
         str(path / "generated.c"), str(runtime_c), "-o", str(prog)],
        text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    # The fully-generated program runs to a deterministic terminal (the entry
    # block's own RESET CPU frontier); repeated runs are byte-identical.
    run_a = subprocess.run([str(prog), "--instruction-budget", "40000"], text=True, capture_output=True)
    run_b = subprocess.run([str(prog), "--instruction-budget", "40000"], text=True, capture_output=True)
    assert run_a.returncode == run_b.returncode, (run_a.returncode, run_b.returncode)
    assert run_a.stdout == run_b.stdout, "generated program output must be deterministic"

  _check_irq6_partial_prefix_rte(executable, compiler, runtime_dir, runtime_c, flags)
  _check_irq6_partial_prefix_boundary(executable, compiler, runtime_dir, runtime_c, flags)

  print("genesis vblank irq6 generated: ok")


if __name__ == "__main__":
  main()
