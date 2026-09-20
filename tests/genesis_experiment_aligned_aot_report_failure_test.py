#!/usr/bin/env python3
"""A measurement-tool failure is never reported as ceiling evidence or success."""

import json
import subprocess
import sys


def main() -> None:
    report_tool, segarecomp = sys.argv[1:]
    # The Python interpreter is executable but cannot compile C source. This
    # reaches strict compilation after one cheap synthetic generation and
    # fails immediately, without exercising normal report repetitions.
    completed = subprocess.run(
        [sys.executable, report_tool, "--segarecomp", segarecomp,
         "--cc", sys.executable, "--skip-canonical"],
        text=True, capture_output=True,
    )
    assert completed.returncode != 0
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise AssertionError(f"report tool wrote non-JSON stdout; stderr={completed.stderr!r}") from error
    verdict = report["synthetic_fixture"]["verdict"]
    assert verdict["overall"] == "tool_failure"
    for variant in ("baseline", "experiment"):
        result = report["synthetic_fixture"][variant]
        assert result["resource_ceiling_hit"] is None
        assert result["tool_failure"].startswith("command_failed:")
        assert result["measurement_complete"] is False
    print("genesis_experiment_aligned_aot_report_failure_test: OK")


if __name__ == "__main__":
    main()
