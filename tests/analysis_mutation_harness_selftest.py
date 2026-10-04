#!/usr/bin/env python3
"""SEG-030-T009: self-test of the mutation harness's incremental-rebuild timestamp discipline (`SourceStamper`).

`analysis_mutation_test.py` mutates a source, rebuilds incrementally, runs, and restores. A build tool rebuilds only when a
prerequisite is strictly newer than its target, and GNU Make 3.81 (or a coarse filesystem) compares whole seconds, so a cycle
faster than one second can skip the recompile of the written source or the archive/relink of the previous build's outputs, and a
stale executable then decides the verdict. This test drives a tiny scratch CMake project (a static library linked into an
executable, the same object -> archive -> executable chain as the analysis fixtures) through mutate(A) -> build -> run,
restore -> build -> run, mutate(B) -> build -> run, each step inside one simulated wall-clock second: before every write the whole
build tree is pinned to the current whole second, exactly the state a build that just finished in this second leaves behind. Every
write and build goes through the harness's own `SourceStamper` (`write`, `settle`, `built`) and `executable_stamps`/`rebuilt`; with
the stamping or the settling bypassed the scenario observes a stale executable (or the contract check fails) and this test fails.
It runs with the configured generator and, when the configured one is not it, also with Unix Makefiles (whole-second make).

usage: analysis_mutation_harness_selftest.py <cmake> <c-compiler> [--generator G] [--make-program P]
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
SECOND_NS = 1_000_000_000


def load_harness():
    spec = importlib.util.spec_from_file_location("analysis_mutation_test", HERE / "analysis_mutation_test.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses resolve their module through sys.modules
    spec.loader.exec_module(module)
    for name in ("SourceStamper", "executable_stamps", "rebuilt"):
        if not hasattr(module, name):
            raise SystemExit(f"FAIL: the mutation harness no longer provides {name}")
    return module


VALUE = 'const char *value(void) {{ return "{value}"; }}\n'
MAIN = '#include <stdio.h>\nconst char *value(void);\nint main(void) { puts(value()); return 0; }\n'
CMAKELISTS = ("cmake_minimum_required(VERSION 3.16)\nproject(stamp C)\nadd_library(value STATIC value.c)\n"
              "add_executable(stamp main.c)\ntarget_link_libraries(stamp PRIVATE value)\n")


def run(command: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=300)


def files(build: pathlib.Path) -> list[pathlib.Path]:
    return list(build.rglob("*"))


def pin_same_second(harness, build: pathlib.Path) -> None:
    """Simulate that the previous build finished in the current whole second (the source write that follows shares that tick)."""
    second = time.time_ns() // SECOND_NS * SECOND_NS
    for p in files(build):
        harness.set_mtime_no_follow(p, second)


def no_follow_fallback(harness, work: pathlib.Path) -> None:
    """Exercise the Windows branch even on hosts whose os.utime supports follow_symlinks=False."""
    probe = work / "utime-probe"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text("probe", encoding="utf-8")
    original = harness.os.utime
    fallback_called = False

    def windows_utime(path, *, ns, follow_symlinks=True):
        nonlocal fallback_called
        if not follow_symlinks:
            raise NotImplementedError("simulated Windows os.utime")
        fallback_called = True
        return original(path, ns=ns)

    harness.os.utime = windows_utime
    try:
        harness.set_mtime_no_follow(probe, time.time_ns())
    finally:
        harness.os.utime = original
    if not fallback_called:
        raise SystemExit("FAIL: unsupported no-follow utime did not use the portable regular-file fallback")


def scenario(harness, cmake: str, c_compiler: str, generator: str, make_program: str, work: pathlib.Path) -> None:
    tag = generator or "default"
    src = work / "src"
    build = work / "build"
    src.mkdir(parents=True)
    (src / "CMakeLists.txt").write_text(CMAKELISTS, encoding="utf-8")
    (src / "main.c").write_text(MAIN, encoding="utf-8")
    source = src / "value.c"
    source.write_text(VALUE.format(value="original"), encoding="utf-8")
    original = source.read_bytes()
    configure = [cmake, "-S", str(src), "-B", str(build), f"-DCMAKE_C_COMPILER={c_compiler}"]
    if generator:
        configure += ["-G", generator]
    if make_program:
        configure.append(f"-DCMAKE_MAKE_PROGRAM={make_program}")
    stamper = harness.SourceStamper(build)
    for command in (configure, [cmake, "--build", str(build), "--target", "stamp"]):
        r = run(command)
        if r.returncode != 0:
            print(r.stdout[-3000:])
            raise SystemExit(f"FAIL [{tag}]: scratch project configure/build")
    stamper.built(True)
    executable = next(p for p in build.rglob("stamp*") if p.is_file() and os.access(p, os.X_OK) and p.stem == "stamp")
    previous = 0

    def step(label: str, data: bytes, expected: str) -> None:
        nonlocal previous
        pin_same_second(harness, build)
        stamper.write(source, data)
        stamper.settle()
        # The helper's contract, independent of the build tool: the written source is at least one stamp step (> 1 s) newer than
        # every build file and strictly newer than every earlier stamp, and every build file lies in a whole second strictly older
        # than the current one.
        stamp = source.stat().st_mtime_ns
        newest = max(p.lstat().st_mtime_ns for p in files(build))
        if (stamp < newest + harness.SourceStamper.STEP_NS or stamp <= previous or
                newest // SECOND_NS >= time.time_ns() // SECOND_NS):
            raise SystemExit(f"FAIL [{tag}] {label}: SourceStamper contract violated (source stamp not a full step newer than the "
                             "build tree and the previous stamp, or the build tree not settled below the current second)")
        previous = stamp
        before = harness.executable_stamps([executable])
        r = run([cmake, "--build", str(build), "--target", "stamp"])
        stamper.built(r.returncode == 0)
        if r.returncode != 0:
            print(r.stdout[-3000:])
            raise SystemExit(f"FAIL [{tag}] {label}: build failed")
        observed = run([str(executable)]).stdout.strip()
        relinked = harness.rebuilt(before)
        if observed != expected or not relinked:
            raise SystemExit(f"FAIL [{tag}] {label}: stale build (observed {observed!r}, expected {expected!r}, "
                             f"relinked={relinked})")
        print(f"ok    [{tag}] {label}: rebuilt and observed {observed!r}")

    step("mutate to A", VALUE.format(value="variant A").encode(), "variant A")
    step("restore original", original, "original")
    step("mutate to B", VALUE.format(value="variant B").encode(), "variant B")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cmake")
    parser.add_argument("c_compiler")
    parser.add_argument("--generator", default="")
    parser.add_argument("--make-program", default="")
    args = parser.parse_args()
    harness = load_harness()
    started = time.monotonic()
    work = pathlib.Path(tempfile.mkdtemp(prefix="segarecomp-mutation-selftest-"))
    try:
        no_follow_fallback(harness, work / "portable")
        scenario(harness, args.cmake, args.c_compiler, args.generator, args.make_program, work / "configured")
        # The whole-second build tool the defect was observed with, when present and not already the configured generator.
        if args.generator != "Unix Makefiles" and os.name != "nt" and shutil.which("make"):
            scenario(harness, args.cmake, args.c_compiler, "Unix Makefiles", "", work / "make")
        elapsed = time.monotonic() - started
        print(f"analysis_mutation_harness_selftest: OK (same-second mutate/restore/mutate rebuilt, {elapsed:.2f} s)")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
