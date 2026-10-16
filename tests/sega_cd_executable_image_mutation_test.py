#!/usr/bin/env python3
"""SEG-062-T001 (ADR 0126): the Sega CD executable-image model test must detect a weakened validator.

Builds tests/sega_cd_executable_image_test.cpp twice against a temporary copy of the generic header: once unchanged (control,
must pass) and once with the cross-image overlap rule weakened so that two images may share an execution address without a
platform selector (mutant, must fail). A second mutant drops the intra-image overlap check. The production header is never touched.

usage: sega_cd_executable_image_mutation_test.py <c++-compiler> <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

CXX, ROOT = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
HEADER = ROOT / "libs" / "recompiler" / "include" / "segarecomp" / "recompiler" / "executable_image.hpp"
TEST = ROOT / "tests" / "sega_cd_executable_image_test.cpp"
RULE = "if (!same_image && set.platform_selector) continue;"
MUTANTS = {
    "cross-image overlap allowed without platform_selector": ("if (!same_image) continue;"),
    "intra-image overlap allowed": ("if (same_image || set.platform_selector) continue;"),
}

failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def build_and_run(header_text, work):
    include = work / "include" / "segarecomp" / "recompiler"
    include.mkdir(parents=True, exist_ok=True)
    (include / "executable_image.hpp").write_text(header_text)
    exe = work / "model_test"
    subprocess.run([CXX, "-std=c++20", "-I", str(work / "include"), "-I", str(ROOT / "libs" / "core" / "include"), str(TEST),
                    "-o", str(exe)], check=True)
    return subprocess.run([str(exe)], capture_output=True, text=True)


def main():
    text = HEADER.read_text()
    check(text.count(RULE) == 1, "the overlap rule is present exactly once in the generic header")
    with tempfile.TemporaryDirectory(prefix="scd-image-mutation-") as tmp:
        tmp = pathlib.Path(tmp)
        control = build_and_run(text, tmp / "control")
        check(control.returncode == 0, "control: the unmodified validator passes the model test")
        for name, replacement in MUTANTS.items():
            mutated = text.replace(RULE, replacement, 1)
            check(mutated != text, "mutant applied: " + name)
            result = build_and_run(mutated, tmp / name.replace(" ", "_"))
            check(result.returncode != 0 and "FAIL" in result.stdout, "mutant detected: " + name)
    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_executable_image_mutation_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
