#!/usr/bin/env python3
"""Strict-C11 IRQ6/RTE interior-resumption and partition-invariance proof."""
import pathlib
import re
import subprocess
import sys
import tempfile


def _instrumented_source(generated_c):
    anchor = "runtime.irq6_handler_present = 1; "
    assert anchor in generated_c, generated_c[:400]
    seeded = generated_c.replace(
        anchor,
        anchor + "runtime.sr = 0x2000; runtime.devices.vdp.registers[1] = 0x0020; "
        "runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 56U; ",
        1)
    drive_call = re.search(r"result = genesis_runtime_run\([^;]*\);", seeded)
    assert drive_call is not None, seeded
    instrumentation = (
        ' fprintf(stderr, "pc=%u d0=%u d1=%u vblank_edges=%u ticks=%llu dispatches=%u a7=%u\\n", '
        '(unsigned)runtime.pc, (unsigned)runtime.d[0], (unsigned)runtime.d[1], '
        '(unsigned)runtime.devices.interrupt.vblank_transition_count, '
        '(unsigned long long)runtime.scheduler.master_ticks, '
        '(unsigned)runtime.recent_pc_history_count, (unsigned)runtime.a[7]);')
    return seeded[:drive_call.end()] + instrumentation + seeded[drive_call.end():]


def _emit(emitter, option):
    first = subprocess.run([emitter, option], text=True, capture_output=True)
    second = subprocess.run([emitter, option], text=True, capture_output=True)
    assert first.returncode == 0, first.stderr
    assert second.returncode == 0, second.stderr
    assert first.stdout == second.stdout, "frontend-generated C must be deterministic"
    assert not first.stdout.startswith("/* translation rejected:"), first.stdout[:200]
    return first.stdout


def _build_and_run(compiler, runtime_dir, generated_c, name):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        source = path / "generated.c"
        program = path / name
        source.write_text(_instrumented_source(generated_c))
        build = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(runtime_dir), str(source), str(runtime_dir / "runtime.c"),
             "-o", str(program)], text=True, capture_output=True)
        assert build.returncode == 0, build.stderr
        run_a = subprocess.run([str(program), "--instruction-budget", "8"],
                               text=True, capture_output=True)
        run_b = subprocess.run([str(program), "--instruction-budget", "8"],
                               text=True, capture_output=True)
    assert run_a.returncode == run_b.returncode
    assert run_a.stdout == run_b.stdout
    assert run_a.stderr == run_b.stderr
    assert '"stop_class":"unsupported_cpu_form"' in run_a.stdout, run_a.stdout
    assert '"family":"reset"' in run_a.stdout, run_a.stdout
    match = re.search(
        r"pc=(\d+) d0=(\d+) d1=(\d+) vblank_edges=(\d+) ticks=(\d+) dispatches=(\d+) a7=(\d+)",
        run_a.stderr)
    assert match is not None, run_a.stderr
    return tuple(map(int, match.groups()))


def main():
    emitter, compiler, root = sys.argv[1:]
    same = _emit(emitter, "--emit-general-startup-bridge-irq6-retirement-redirect")
    split = _emit(emitter, "--emit-general-startup-bridge-irq6-retirement-redirect-split")

    # One ordinary function owns both real source boundaries in the same-block
    # form; direct labels resume without a wrapper or a second dispatcher.
    assert "case UINT32_C(0x00000100): goto genesis_instruction_00000100;" in same
    assert "case UINT32_C(0x00000102): goto genesis_instruction_00000102;" in same
    assert "{ UINT32_C(0x00000100), genesis_block_00000100 }" in same
    assert "{ UINT32_C(0x00000102), genesis_block_00000100 }" in same
    assert "{ UINT32_C(0x00000102), genesis_block_00000102 }" in split
    assert same.count("static GenesisCompiledEntry genesis_compiled_entry_lookup(uint32_t address)") == 2

    runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
    same_result = _build_and_run(compiler, runtime_dir, same, "same-block")
    split_result = _build_and_run(compiler, runtime_dir, split, "split-block")
    assert same_result == split_result, (same_result, split_result)

    pc, d0, d1, edges, ticks, dispatches, a7 = same_result
    assert pc == 0x104
    assert d0 == 1 and d1 == 1, "prefix replay or resumed-instruction loss"
    assert edges == 1
    # ADDQ.L Dn (8) + RTE (20) + resumed ADDQ.L Dn (8), seven master ticks
    # per MC68000 cycle. The first instruction begins 56 ticks before onset.
    assert ticks == 766080 + 196
    assert dispatches == 3, "entry, handler/RTE, and interior resume must each dispatch once"
    assert a7 == 0x00FF8000, "RTE did not consume exactly its six-byte exception frame"


if __name__ == "__main__":
    main()
