#!/usr/bin/env python3
"""SEG-007-T180: ONE_SHOT_SUMMARY normalized metrics count ACTUAL invocations.

Reviewer-requested observability fix. `generation_attempts` / `compile_attempts`
must reflect what the driver really did, not the intended pipeline shape:

  * emitter / static-translation rejection returns before any C is written and
    before the C compiler is invoked  =>  generation_attempts=1, compile_attempts=0
  * generation succeeds and the compiler is invoked once                =>  1, 1

Project-authored synthetic driver-level fixture, no commercial input. Reuses the
scripted `segarecomp`-shaped proxy from the Phase-B expansion test (same
established C6/C7 driver-test pattern) so this exercises
`tools/genesis_startup_bridge.py`'s own `--one-shot` summary emission in
isolation from CPU discovery correctness.
"""
import hashlib
import importlib.util
import json
import os
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


REJECT_PROXY = r'''#!/usr/bin/env python3
import re
import sys

if len(sys.argv) < 2 or sys.argv[1] != "emit-general-startup-bridge-c":
    raise SystemExit(2)
try:
    digest = sys.argv[sys.argv.index("--rom-sha256") + 1]
except (ValueError, IndexError):
    raise SystemExit(2)
if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
    raise SystemExit(2)
if "--reset-entry" not in sys.argv or "--external-hints" not in sys.argv:
    raise SystemExit(2)
# Fail-closed emitter rejection: emitted on stdout, returncode 0, no C body.
# generate_and_compile() returns status 1 before writing bridge.generated.c and
# before invoking the C compiler.
sys.stdout.write("/* translation rejected: project-authored-synthetic */\n")
'''


def one_shot_summary(stderr: str) -> dict | None:
    for line in stderr.splitlines():
        if line.startswith("ONE_SHOT_SUMMARY "):
            return json.loads(line[len("ONE_SHOT_SUMMARY "):])
    return None


def run_one_shot(driver: pathlib.Path, proxy: pathlib.Path, compiler: pathlib.Path,
                 rom: pathlib.Path, hints: pathlib.Path, root: pathlib.Path,
                 out_dir: pathlib.Path, scenario_path: pathlib.Path | None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    if scenario_path is not None:
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

    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-one-shot-") as directory:
        temporary = pathlib.Path(directory)
        rom = temporary / "synthetic.bin"
        rom.write_bytes(b"project-authored-one-shot-synthetic-rom")
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()
        hints = temporary / "hints.toml"
        hints.write_text("# project-authored synthetic external-hints opt-in file\n")

        # Case 1: emitter rejects before emitting C. The C compiler is never
        # invoked -> generation_attempts=1, compile_attempts=0.
        reject_proxy = temporary / "reject_proxy.py"
        reject_proxy.write_text(REJECT_PROXY)
        reject_proxy.chmod(0o755)
        out_reject = temporary / "out-reject"
        result = run_one_shot(driver, reject_proxy, compiler, rom, hints, root, out_reject, None)
        if result.returncode != 1:
            return fail("case1-reject-exit", result)
        summary = one_shot_summary(result.stderr)
        expected = {"runtime_confirmed_seed_count": 0, "generation_attempts": 1,
                    "compile_attempts": 0, "driver_result": "build_time_translation_rejected"}
        if summary != expected:
            sys.stderr.write(f"case1 unexpected ONE_SHOT_SUMMARY: {summary!r} != {expected!r}\n")
            return 1
        if (out_reject / "bridge.generated.c").exists() or (out_reject / "bridge").exists():
            sys.stderr.write("case1 emitted C / binary despite a pre-emission rejection\n")
            return 1

        # Case 2: generation succeeds and the C compiler is invoked exactly once
        # -> generation_attempts=1, compile_attempts=1.
        success_proxy = temporary / "success_proxy.py"
        _phase_b.write_proxy(success_proxy)
        sanitized, full = _phase_b.build_reports("completed", digest)
        scenario_path = temporary / "scenario.json"
        scenario_path.write_text(json.dumps({
            "": [json.dumps(sanitized, separators=(",", ":")),
                 json.dumps(full, separators=(",", ":"))]}))
        out_ok = temporary / "out-ok"
        result = run_one_shot(driver, success_proxy, compiler, rom, hints, root, out_ok, scenario_path)
        if result.returncode != 0:
            return fail("case2-success-exit", result)
        summary = one_shot_summary(result.stderr)
        if summary is None or summary.get("generation_attempts") != 1 or summary.get("compile_attempts") != 1:
            sys.stderr.write(f"case2 unexpected ONE_SHOT_SUMMARY: {summary!r}\n")
            return 1
        if "generation_rounds" in summary or "compiles" in summary:
            sys.stderr.write(f"case2 stale metric key still present: {summary!r}\n")
            return 1
        if not (out_ok / "bridge.generated.c").exists():
            sys.stderr.write("case2 did not emit bridge.generated.c on the success path\n")
            return 1

    print("genesis_startup_bridge_one_shot_attempt_metrics_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
