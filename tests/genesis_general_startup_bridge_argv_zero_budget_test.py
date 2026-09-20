#!/usr/bin/env python3
"""SEG-007-T252 / ADR-0040 correction: the generated bridge's own
`genesis_parse_bridge_argv` CLI boundary must itself reject
`--instruction-budget 0` -- not merely `tools/genesis_startup_bridge.py`'s
Python-side `instruction_budget_value` validator.

Before this correction, `genesis_parse_bridge_argv` only rejected a parsed
value greater than UINT32_MAX, never a value of exactly zero, so a bad
*runner* option (`dispatch_allowance == 0`) reached `genesis_runtime_run`,
which then produced the internal-dispatch-inconsistency *guest* stop -- a
runner-policy error must never cross into guest-semantic-stop territory.

Project-authored synthetic driver-level fixture, no commercial input.
Generates a real minimal bridge from a synthetic ROM via the production
`emit-general-startup-bridge-c` route, compiles it exactly like
`tools/genesis_startup_bridge.py` does, then invokes the resulting binary
directly (bypassing the Python CLI's own validation entirely) to exercise the
generated argv parser itself.
"""
import hashlib
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location("genesis_startup_bridge", ROOT.parent / "tools" / "genesis_startup_bridge.py")
assert _SPEC and _SPEC.loader
_bridge = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_bridge)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    binary, compiler, root = (pathlib.Path(value).resolve() for value in sys.argv[1:])

    # Reset SSP, reset PC=8, then an unsupported/fail-closed frontier -- a
    # minimal, deterministic, project-authored synthetic image; the exact
    # guest behavior past the argv boundary is irrelevant to this test.
    image = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08,
                   0x70, 0x00, 0x4E, 0x70))
    digest = hashlib.sha256(image).hexdigest()

    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-argv-zero-budget-") as directory:
        temporary = pathlib.Path(directory)
        rom = temporary / "argv-zero-budget.bin"
        rom.write_bytes(image)

        emitter_command = [str(binary), "emit-general-startup-bridge-c", "--rom", str(rom),
                          "--reset-entry", "--rom-sha256", digest]
        out_dir = temporary / "out"
        out_dir.mkdir()
        status, _generated_bytes, executable = _bridge.generate_and_compile(
            emitter_command, compiler, root, out_dir, False)
        require(status == 0 and executable is not None, "synthetic bridge generation/compilation must succeed")
        assert executable is not None

        # Zero is rejected before any guest step executes: nonzero process
        # exit, no sanitized guest-stop report on stdout, no stderr either --
        # this must fail closed at argv parsing, before genesis_runtime_run
        # (and therefore genesis_runtime_step / any dispatch()) is ever called.
        zero_budget = subprocess.run([str(executable), "--instruction-budget", "0"],
                                     text=True, capture_output=True, cwd=root)
        require(zero_budget.returncode != 0, "--instruction-budget 0 must exit nonzero")
        require(zero_budget.stdout == "", "no sanitized guest-stop report may be written for a rejected argv")
        require(zero_budget.stderr == "", "no guest step executes; nothing is written to stderr either")

        # A genuinely malformed non-digit value is still rejected the same way
        # (pre-existing behavior; kept here as a same-file contrast case).
        malformed = subprocess.run([str(executable), "--instruction-budget", "abc"],
                                   text=True, capture_output=True, cwd=root)
        require(malformed.returncode != 0, "a non-digit --instruction-budget value must exit nonzero")
        require(malformed.stdout == "", "a rejected argv must never produce a sanitized report")

        # A well-formed positive value is still accepted (no regression).
        accepted = subprocess.run([str(executable), "--instruction-budget", "1"],
                                  text=True, capture_output=True, cwd=root)
        require(accepted.returncode == 0, "--instruction-budget 1 must still be accepted")
        require(accepted.stdout != "", "an accepted invocation must still produce a sanitized report")

    print("genesis_general_startup_bridge_argv_zero_budget_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
