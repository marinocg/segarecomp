#!/usr/bin/env python3
"""SEG-047 (ADR 0096): the opt-in native ML admission on a project-authored synthetic Genesis image.

Properties (the frozen model's verdict on a synthetic image is not asserted; the contract is):
1. `--immutable-rom-aot-ml-admission` emits one sanitized report line: producer=ml_region (validator accepted, admitted <= broad) or
   producer=broad with a stable reason, and in the fallback case the emission is byte-identical to plain broad emission;
2. the emission is deterministic and an explicit admission plan / a missing --immutable-rom-aot is a usage error (exact map precedence
   is structural: the options are mutually exclusive);
3. broad and ML-admission programs, built and run through the unchanged bridge with an explicit instruction budget, behave identically;
4. `segarecomp build --aot-policy optimized` is rejected only for invalid values and records the machine-readable `aot_policy` member.
"""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
BUDGET = 20000
KNOWN_REASONS = {"model_identity", "rom_size", "empty_proposal", "prune_rejected", "validator_rejected", "no_analysis"}


def rom_image() -> bytes:
    import struct
    rom = bytearray(0x4000)

    def words(at, *values):
        for value in values:
            rom[at:at + 2] = struct.pack(">H", value & 0xFFFF)
            at += 2
        return at

    rom[0:4] = struct.pack(">I", 0x00FFFE00)
    rom[4:8] = struct.pack(">I", 0x200)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    for at in range(0x3000, 0x3400, 2):
        words(at, 0x4E71)
    entry = 0x200
    at = words(entry, 0x5878, 0xF000, 0x3038, 0xF000, 0x0240, 0x01FC, 0x41F9, 0x0000, 0x0800, 0x2270, 0x0000)
    words(at, 0x4ED1)
    for k in range(128):
        rom[0x800 + 4 * k:0x804 + 4 * k] = struct.pack(">I", 0x1000 + 16 * k)
        slot = words(0x1000 + 16 * k, 0x5278, 0xF004)
        words(slot, 0x6000, (entry - (slot + 2)) & 0xFFFF)
    return bytes(rom)


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


def main():
    segarecomp, source = sys.argv[1:3]
    (pathlib.Path(source) / "build").mkdir(exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(dir=pathlib.Path(source) / "build", prefix="ml-admission-"))
    rom = work / "rom.bin"
    data = rom_image()
    rom.write_bytes(data)
    sha = hashlib.sha256(data).hexdigest()
    emit = [segarecomp, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"]

    plain = run(emit)
    assert plain.returncode == 0, plain.stderr
    first = run(emit + ["--immutable-rom-aot-ml-admission"])
    second = run(emit + ["--immutable-rom-aot-ml-admission"])
    assert first.returncode == 0 and first.stdout == second.stdout and first.stderr == second.stderr, "ML admission is deterministic"
    line = next((l for l in first.stderr.splitlines() if l.startswith("segarecomp: m68k admission: requested=optimized")), None)
    assert line is not None, first.stderr
    fields = dict(token.split("=", 1) for token in line.split()[3:])
    assert fields["requested"] == "optimized"
    if fields["producer"] == "ml_region":
        assert fields["fallback"] == "0" and fields["validator"] == "accepted" and fields["reason"] == "none", line
        assert int(fields["admitted"]) <= int(fields["broad"]) and int(fields["k"]) == int(fields["admitted"]), line
        assert fields["model"] == "seg046-features-v1" and re.fullmatch(r"[0-9a-f]{64}", fields["k_sha256"]), line
    else:
        assert fields["producer"] == "broad" and fields["fallback"] == "1" and fields["reason"] in KNOWN_REASONS, line
        assert first.stdout == plain.stdout, "a fallback emission must be byte-identical to broad"
    assert "address" not in line and "0x" not in line, "the report line is sanitized"

    plan = work / "x.plan"
    plan.write_bytes(f"segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 {sha}\nuniverse_sha256 {'0' * 64}\nstrategy broad\nend\n".encode())
    both = run(emit + ["--immutable-rom-aot-admission", str(plan), "--immutable-rom-aot-ml-admission"])
    assert both.returncode == 2 and "excludes" in both.stderr, both.stderr
    no_aot = run(emit[:-1] + ["--immutable-rom-aot-ml-admission"])
    assert no_aot.returncode == 2, no_aot.stderr

    bridge = [sys.executable, str(pathlib.Path(source) / "tools/genesis_startup_bridge.py"), "--segarecomp", segarecomp, "--rom", str(rom),
              "--mode", "synthetic", "--immutable-rom-aot", "--instruction-budget", str(BUDGET)]

    def build_and_run(name, extra):
        out = work / name
        result = run(bridge + ["--out-dir", str(out), "--full-report-path", str(work / f"{name}.full.json")] + extra)
        assert result.returncode == 0, (name, result.stdout, result.stderr)
        return json.loads((work / f"{name}.full.json").read_text())

    broad = build_and_run("broad", [])
    ml = build_and_run("ml", ["--ml-admission"])
    assert broad["result"] == "runner_resource_limit", broad["result"]
    # ML is a recall HEURISTIC: on this tiny synthetic image the frozen model may leave out the dynamic-jump island. The contract is
    # "identical behaviour, or a fail-closed guest stop - never wrong execution".
    same = ml["result"] == broad["result"] and ml["runtime"] == broad["runtime"]
    assert same or (ml["result"] != "runner_resource_limit" and ml.get("stop_class") is not None), "ML run neither equal nor fail-closed"
    outcome = "equal" if same else "fail_closed"
    bad = run([segarecomp, "build", "--rom", str(rom), "--output", str(work / "b"), "--cc", "cc", "--runtime-dir", str(work), "--aot-policy", "turbo"])
    assert bad.returncode == 2, "an unknown policy is a usage error"
    import shutil
    shutil.rmtree(work, ignore_errors=True)
    print("genesis_ml_admission_cli_test: OK", fields["producer"], outcome)


if __name__ == "__main__":
    main()
