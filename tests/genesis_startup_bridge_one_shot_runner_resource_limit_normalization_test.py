#!/usr/bin/env python3
"""SEG-007-T252 correction: one-shot ONE_SHOT_SUMMARY normalization must not
conflate runner-resource-limit exhaustion with a guest/static inventory stop.

Regression for a defect where `driver_result` was computed as
`"completed" if completed else "offline_inventory_incomplete"`, which silently
classified a `runner_resource_limit` frontier (host runner policy, never a
guest semantic stop) as a guest/static inventory deficiency, contradicting the
already-correct `frontier_class` computed immediately above it.

Project-authored synthetic driver-level fixture, no commercial input. Reuses
the established C6/C7 scripted `segarecomp`-shaped proxy pattern (same as
`genesis_startup_bridge_one_shot_attempt_metrics_test.py`) so this exercises
`tools/genesis_startup_bridge.py`'s own `--one-shot` summary normalization in
isolation from CPU discovery correctness.
"""
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "genesis_startup_bridge_phase_b_expansion_test",
    ROOT / "genesis_startup_bridge_phase_b_expansion_test.py")
assert _SPEC and _SPEC.loader
_phase_b = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_phase_b)


def one_shot_summary(stderr: str) -> dict | None:
    for line in stderr.splitlines():
        if line.startswith("ONE_SHOT_SUMMARY "):
            return json.loads(line[len("ONE_SHOT_SUMMARY "):])
    return None


def run_one_shot(driver: pathlib.Path, proxy: pathlib.Path, compiler: pathlib.Path,
                 rom: pathlib.Path, hints: pathlib.Path, root: pathlib.Path,
                 out_dir: pathlib.Path, scenario_path: pathlib.Path) -> subprocess.CompletedProcess[str]:
    import os
    env = dict(os.environ)
    env["GSB_TEST_SCENARIO"] = str(scenario_path)
    env["GSB_TEST_LOG"] = str(out_dir.with_suffix(".log"))
    return subprocess.run([
        sys.executable, str(driver), "--segarecomp", str(proxy), "--cc", str(compiler),
        "--rom", str(rom), "--mode", "commercial", "--diagnose-frontier", "--one-shot",
        "--external-hints", str(hints), "--out-dir", str(out_dir)],
        text=True, capture_output=True, cwd=root, env=env)


def fail(name: str, result: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(f"{name} failed: rc={result.returncode}\n"
                     f"STDOUT={result.stdout!r}\nSTDERR={result.stderr!r}\n")
    return 1


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    _, compiler_arg, root_arg = sys.argv[1:]
    compiler, root = pathlib.Path(compiler_arg).resolve(), pathlib.Path(root_arg).resolve()
    driver = root / "tools" / "genesis_startup_bridge.py"

    with tempfile.TemporaryDirectory(dir=root / "build",
                                     prefix="genesis-startup-bridge-one-shot-runner-limit-") as directory:
        temporary = pathlib.Path(directory)
        rom = temporary / "synthetic.bin"
        rom.write_bytes(b"project-authored-runner-resource-limit-synthetic-rom")
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()
        hints = temporary / "hints.toml"
        hints.write_text("# project-authored synthetic external-hints opt-in file\n")

        proxy = temporary / "success_proxy.py"
        _phase_b.write_proxy(proxy)
        sanitized, full = _phase_b.build_reports("runner_resource_limit", digest)
        scenario_path = temporary / "scenario.json"
        scenario_path.write_text(json.dumps({
            "": [json.dumps(sanitized, separators=(",", ":")),
                 json.dumps(full, separators=(",", ":"))]}))
        out_dir = temporary / "out"
        result = run_one_shot(driver, proxy, compiler, rom, hints, root, out_dir, scenario_path)
        if result.returncode != 0:
            return fail("runner-resource-limit-exit", result)

        summary = one_shot_summary(result.stderr)
        if summary is None:
            sys.stderr.write(f"no ONE_SHOT_SUMMARY emitted: {result.stderr!r}\n")
            return 1
        if summary.get("frontier_class") != "runner_resource_limit":
            sys.stderr.write(f"unexpected frontier_class: {summary!r}\n")
            return 1
        if summary.get("driver_result") != "runner_resource_limit":
            sys.stderr.write(
                "runner_resource_limit must normalize driver_result to "
                f"'runner_resource_limit', never 'offline_inventory_incomplete' or any "
                f"other guest/static-inventory label: {summary!r}\n")
            return 1

    print("genesis_startup_bridge_one_shot_runner_resource_limit_normalization_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
