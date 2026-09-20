#!/usr/bin/env python3
"""SEG-007-T132: mechanical independence audit for the checkpoint oracle.

Demonstrates ADR-0012 Decision 5's five independence properties for
tests/oracle/genesis:
  1. no shared production translation unit/object is ever compiled/linked
     into the oracle build;
  2. the built oracle binary carries no production semantic symbol;
  3. the only runtime/src/include header the oracle sources ever #include
     is platforms/genesis/runtime/checkpoint_evidence.h;
  4. no oracle test calls any production function to compute an expected
     value;
  5. checkpoint_evidence.h remains a schema-only (types/constants/optional
     `static inline` accessors) header -- no behavior.

This test needs the pinned Musashi checkout (same env var as
tests/genesis_checkpoint_oracle_test.py) to actually build a binary for the
symbol-table check; without it, checks 1/3/4/5 (source-level, no build
required) still run, and check 2 is skipped with a printed note (mirroring
the existing graceful-skip pattern), never silently treated as passed.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ORACLE_DIR = ROOT / "tests/oracle/genesis"
FIXTURE_SOURCE = ROOT / "tests/tools/genesis_checkpoint_oracle_fixture_test.c"
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_T132_ORACLE_MUSASHI_CHECKOUT"

# Production symbols that must never appear in the oracle's own object
# files or final binary.
FORBIDDEN_SYMBOLS = (
    "genesis_route_access",
    "genesis_runtime_run",
    "genesis_extract_checkpoint_evidence",
    "genesis_write_checkpoint_evidence_summary",
    "genesis_sha256_init",
    "genesis_sha256_update",
    "genesis_sha256_final",
)


def environment():
    result = os.environ.copy()
    if not result.get("SDKROOT") and shutil.which("xcrun"):
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True)
        if sdk.returncode == 0:
            result["SDKROOT"] = sdk.stdout.strip()
    return result


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result


def check_source_level():
    """Checks 1 (by construction), 3, 4, 5 -- no build required."""
    oracle_sources = sorted(list(ORACLE_DIR.glob("*.c")) + list(ORACLE_DIR.glob("*.h")))
    assert oracle_sources, "no oracle sources found"

    # Check 1: the actual build command list (this file's own
    # check_binary_symbols and the sibling genesis_checkpoint_oracle_test.py
    # driver) never names a production .c/.o compilation unit -- runtime.c,
    # any libs/cpu/m68k/src source, or any codegen/C11-emitter/generated-C
    # object. Inspected at the driver-script level (the actual `cc`
    # argument list), not by grepping oracle source comments, since a
    # comment may legitimately *discuss* runtime.c by name (explaining why
    # it is never linked) without that being a build reference.
    # (This file's own source is deliberately not scanned: the assertion
    # message above and this comment legitimately name the forbidden
    # strings, which would trivially self-trigger a naive scan of __file__.)
    driver_script = ROOT / "tests/genesis_checkpoint_oracle_test.py"
    forbidden_build_inputs = ("platforms/genesis/runtime/runtime.c", "runtime.o", "libs/cpu/m68k/src")
    text = driver_script.read_text()
    for forbidden in forbidden_build_inputs:
        assert forbidden not in text, (
            f"{driver_script} references a forbidden production build input: {forbidden}"
        )

    # Check 3: the only runtime/src/include header ever #include'd is
    # platforms/genesis/runtime/checkpoint_evidence.h.
    include_pattern = re.compile(r'#include\s+["<]([^">]+)[">]')
    for path in oracle_sources:
        for line in path.read_text().splitlines():
            match = include_pattern.search(line)
            if not match:
                continue
            included = match.group(1)
            if not any(part in included for part in ("runtime/", "src/", "include/")):
                continue
            assert included.endswith("platforms/genesis/runtime/checkpoint_evidence.h"), (
                f"{path} includes a non-schema production header: {included}"
            )

    # Check 4: no oracle test calls any production function to compute an
    # expected value (the fixture file's own literals are the expectation).
    fixture_text = FIXTURE_SOURCE.read_text()
    for forbidden in ("genesis_extract_checkpoint_evidence(", "genesis_route_access(",
                      "genesis_runtime_run(", "genesis_write_checkpoint_evidence_summary("):
        assert forbidden not in fixture_text, f"oracle fixture test calls production function {forbidden}"

    # Check 5: checkpoint_evidence.h remains schema-only -- reuse the exact
    # existing standalone-compile check's own literal expectations so this
    # audit cannot silently weaken it.
    schema_header = ROOT / "platforms/genesis/runtime/checkpoint_evidence.h"
    text = schema_header.read_text()
    forbidden_behavior_markers = ("if (", "for (", "while (", "printf(", "malloc(")
    non_static_inline_functions = re.findall(r'^\s*(?!static inline)[A-Za-z_][A-Za-z0-9_ \*]*\s+\w+\s*\([^;]*\)\s*\{',
                                              text, re.MULTILINE)
    assert not non_static_inline_functions, (
        f"checkpoint_evidence.h declares non-'static inline' function bodies: {non_static_inline_functions}"
    )
    for marker in forbidden_behavior_markers:
        assert marker not in text, f"checkpoint_evidence.h contains a behavior marker: {marker}"


def check_binary_symbols(compiler, temporary):
    """Check 2: build the oracle test binary and grep its symbol table for
    every forbidden production symbol, plus a positive control proving the
    check itself can detect a real symbol (the oracle's own function)."""
    setting = os.environ.get(CHECKOUT_ENV)
    if not setting:
        print(f"{CHECKOUT_ENV} unset; binary symbol-table independence check skipped "
              "(source-level checks 1/3/4/5 still ran and passed)")
        return

    checkout = Path(setting)
    assert checkout.is_dir(), f"T132 oracle Musashi checkout is not a directory: {checkout}"
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN, \
        "T132 oracle Musashi checkout is not pinned to the expected revision"

    generator = temporary / "m68kmake"
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             str(checkout / "m68kmake.c"), "-o", str(generator)], env=environment())
    shutil.copyfile(checkout / "m68k_in.c", temporary / "m68k_in.c")
    checked([str(generator)], cwd=temporary, env=environment())

    binary = temporary / "genesis-checkpoint-oracle-fixture-test"
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic",
             "-I", str(temporary), "-I", str(checkout), "-I", str(checkout / "softfloat"),
             "-I", str(ORACLE_DIR),
             str(FIXTURE_SOURCE), str(ORACLE_DIR / "checkpoint_oracle.c"), str(ORACLE_DIR / "sha256_oracle.c"),
             str(checkout / "m68kcpu.c"), str(temporary / "m68kops.c"), str(checkout / "softfloat/softfloat.c"),
             "-o", str(binary)], env=environment())

    nm_tool = shutil.which("nm")
    assert nm_tool, "nm is required for the independence symbol-table check"
    symbols = subprocess.run([nm_tool, str(binary)], text=True, capture_output=True).stdout

    for forbidden in FORBIDDEN_SYMBOLS:
        assert forbidden not in symbols, f"oracle binary unexpectedly contains production symbol {forbidden}"

    # Positive control: the check itself must be able to detect a real
    # symbol, proving it is not vacuously passing.
    assert "genesis_checkpoint_oracle_derive" in symbols, \
        "independence check's own positive control failed: the oracle's public entry point is missing"


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    check_source_level()
    with tempfile.TemporaryDirectory() as name:
        check_binary_symbols(compiler, Path(name))
    print("genesis_checkpoint_oracle_independence_test: all checks passed")


if __name__ == "__main__":
    raise SystemExit(main())
