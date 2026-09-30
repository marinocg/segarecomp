#!/usr/bin/env python3
"""SEG-022-T009: run the generator, then compile each generated source as strict C11 and check lookups."""
import pathlib
import subprocess
import sys

generator, compiler, work = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
work.mkdir(parents=True, exist_ok=True)
subprocess.run([generator, str(work)], check=True)
sources = sorted(work.glob("*.c"))
assert sources, "generator produced no sources"
for source in sources:
    binary = source.with_suffix(".exe")
    compiled = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                               "-o", str(binary), str(source)], capture_output=True, text=True)
    assert compiled.returncode == 0, (source.name, compiled.stderr)
    ran = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
    expected = source.with_suffix(".expected").read_text().split()
    assert ran.stdout.split() == expected, source.name
# Chunked tables (SEG-008-T009): every directory with a probe.c is compiled as strict C11 (each TU separately) and linked.
chunked = sorted(d for d in work.iterdir() if d.is_dir() and (d / "probe.c").exists())
assert chunked, "generator produced no chunked table"
flags = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-D_CRT_SECURE_NO_WARNINGS", "-I"]
import concurrent.futures


def compile_unit(job):
    source, directory = job
    obj = source.with_suffix(".o")
    done = subprocess.run(flags[:-1] + ["-I", str(directory), "-c", str(source), "-o", str(obj)], capture_output=True, text=True)
    assert done.returncode == 0, (source.name, done.stderr)
    return str(obj)


for directory in chunked:
    jobs = [(s, directory) for s in sorted(directory.glob("*.c")) if s.name != "ct_main.c"]  # the probe includes the main TU
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_unit, jobs))
    binary = directory / "probe.exe"
    linked = subprocess.run([compiler, *objects, "-o", str(binary)], capture_output=True, text=True)
    assert linked.returncode == 0, (directory.name, linked.stderr)
    ran = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
    assert ran.stdout.split() == (directory / "probe.expected").read_text().split(), directory.name
print("ok", len(sources), "chunked", len(chunked))
