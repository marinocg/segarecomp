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
print("ok", len(sources))
