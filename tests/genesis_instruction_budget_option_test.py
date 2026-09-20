#!/usr/bin/env python3
"""SEG-007-T252 / ADR-0040: `instruction_budget_value` boundary validation.

Project-authored unit test with no commercial input. Exercises
`tools/genesis_startup_bridge.py`'s `instruction_budget_value` argparse type
function directly (imported as a module, matching the pattern used by
`tests/genesis_startup_bridge_checkpoint_test.py`) against the exact boundary
set the task record's Acceptance section requires: malformed, negative,
fractional, out-of-range, zero (rejected for this automated/headless entry
point), the maximum representable value (accepted), and overflow-adjacent
values just above the maximum (rejected).
"""
import argparse
import importlib.util
import pathlib


def load_bridge_module(root: pathlib.Path):
    spec = importlib.util.spec_from_file_location("genesis_startup_bridge", root / "tools" / "genesis_startup_bridge.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def rejects(function, value: str) -> bool:
    try:
        function(value)
    except argparse.ArgumentTypeError:
        return True
    return False


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    module = load_bridge_module(root)
    parse = module.instruction_budget_value

    # Malformed (non-digit) text is rejected.
    require(rejects(parse, "abc"), "non-digit text must be rejected")
    require(rejects(parse, ""), "empty text must be rejected")
    require(rejects(parse, "12abc"), "trailing garbage must be rejected")

    # Negative values are rejected (the digit-only gate rejects the leading '-').
    require(rejects(parse, "-5"), "negative values must be rejected")

    # Fractional values are rejected (the digit-only gate rejects the '.').
    require(rejects(parse, "12.5"), "fractional values must be rejected")

    # Zero is rejected for this automated/headless entry point, even though
    # the wire schema separately tolerates a foreign-produced zero.
    require(rejects(parse, "0"), "zero must be rejected for the automated/headless CLI path")

    # Out-of-range (beyond UINT32_MAX) values are rejected.
    require(rejects(parse, str(module.UINT32_MAX + 1)), "values beyond UINT32_MAX must be rejected")
    require(rejects(parse, str(module.UINT32_MAX + 1_000_000)), "far-out-of-range values must be rejected")

    # The maximum representable value is accepted, and small/ordinary
    # in-range values round-trip exactly with no overflow surprises.
    require(parse(str(module.UINT32_MAX)) == module.UINT32_MAX, "UINT32_MAX must be accepted exactly")
    require(parse("1") == 1, "the smallest positive value must be accepted exactly")
    require(parse("128") == 128, "the compiled-in default value must be accepted exactly")

    print("PASS: instruction_budget_value boundary coverage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
