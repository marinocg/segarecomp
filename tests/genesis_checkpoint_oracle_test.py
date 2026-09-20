#!/usr/bin/env python3
"""SEG-007-T132: independent checkpoint-evidence oracle fixture coverage.

Builds tests/oracle/genesis (the independent oracle, linked against the
pinned local Musashi core) and runs the four hand-authored synthetic
fixtures in tests/tools/genesis_checkpoint_oracle_fixture_test.c. Mirrors
tests/m68k_batch_b_musashi_differential_test.py's pinning/graceful-skip
pattern: if the pinned checkout env var is unset, this prints a message and
exits 0 without failing the build.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ORACLE_DIR = ROOT / "tests/oracle/genesis"
FIXTURE_SOURCE = ROOT / "tests/tools/genesis_checkpoint_oracle_fixture_test.c"
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_T132_ORACLE_MUSASHI_CHECKOUT"


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


def build_and_run(compiler, temporary):
    setting = os.environ.get(CHECKOUT_ENV)
    if not setting:
        print(f"{CHECKOUT_ENV} unset; oracle fixture coverage skipped (no differential claim made)")
        return True
    checkout = Path(setting)
    assert checkout.is_dir(), f"T132 oracle Musashi checkout is not a directory: {checkout}"
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN, \
        "T132 oracle Musashi checkout is not pinned to the expected revision"
    assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0, \
        "T132 oracle Musashi checkout has tracked changes"
    for source in ("m68kmake.c", "m68k_in.c", "m68kcpu.c", "softfloat/softfloat.c"):
        assert (checkout / source).is_file(), f"T132 oracle Musashi checkout lacks {source}"

    generator = temporary / "m68kmake"
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             str(checkout / "m68kmake.c"), "-o", str(generator)], env=environment())
    shutil.copyfile(checkout / "m68k_in.c", temporary / "m68k_in.c")
    checked([str(generator)], cwd=temporary, env=environment())
    assert (temporary / "m68kops.c").is_file() and (temporary / "m68kops.h").is_file()

    binary = temporary / "genesis-checkpoint-oracle-fixture-test"
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic",
             "-I", str(temporary), "-I", str(checkout), "-I", str(checkout / "softfloat"),
             "-I", str(ORACLE_DIR),
             str(FIXTURE_SOURCE), str(ORACLE_DIR / "checkpoint_oracle.c"), str(ORACLE_DIR / "sha256_oracle.c"),
             str(checkout / "m68kcpu.c"), str(temporary / "m68kops.c"), str(checkout / "softfloat/softfloat.c"),
             "-o", str(binary)], env=environment())
    result = checked([str(binary)])
    print(result.stdout.strip())
    return True


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    import tempfile
    with tempfile.TemporaryDirectory() as name:
        build_and_run(compiler, Path(name))


if __name__ == "__main__":
    raise SystemExit(main())
