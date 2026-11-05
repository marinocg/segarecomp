#!/usr/bin/env python3
"""SEG-122-T010 (ADR 0270): the closed plan of a RAM-thunk emission, on a project-authored synthetic Genesis image.

Properties:
1. with a materialized RAM thunk the prune rejects a cartridge-only proposal and the closure of the same proposal is an ordinary plan the unchanged validator accepts
   for that exact emission, deterministically; a plan for another thunk set is refused;
2. closure follows the thunk's fixed target, not a dynamic transfer;
3. a program built from a closed plan stops with its typed stop when a dynamic transfer reaches a start outside the admitted set, while broad reaches it.
Every generated program runs through the startup bridge with an explicit instruction budget.
"""
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

BUDGET = 20000
DYNAMIC_TARGET = 0x1000  # reached only by JMP (A0)
THUNK_TARGET = 0x2000    # reached only by the RAM thunk's JMP
THUNK = "00ff0100:4ef900002000"


def rom_image() -> bytes:
    rom = bytearray(0x4000)

    def words(at, *values):
        for value in values:
            rom[at:at + 2] = struct.pack(">H", value)
            at += 2

    rom[0:4] = struct.pack(">I", 0x00FFFE00)
    rom[4:8] = struct.pack(">I", 0x200)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    words(0x200, 0x21FC, 0x0000, DYNAMIC_TARGET, 0xF000, 0x2078, 0xF000, 0x4ED0)  # MOVE.L #target,($F000).W; MOVEA.L ($F000).W,A0; JMP (A0)
    words(DYNAMIC_TARGET, 0x4E71, 0x60FC)  # NOP; BRA to the NOP
    words(THUNK_TARGET, 0x4E71, 0x4E71, 0x60FA)  # NOP; NOP; BRA to the first NOP
    return bytes(rom)


def run(command):
    return subprocess.run(command, text=True, capture_output=True, timeout=240)


def main():
    segarecomp, source = sys.argv[1:3]
    (pathlib.Path(source) / "build").mkdir(exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(dir=pathlib.Path(source) / "build", prefix="ram-thunk-closure-"))
    rom = work / "rom.bin"
    data = rom_image()
    rom.write_bytes(data)
    sha = hashlib.sha256(data).hexdigest()
    regions = work / "regions.txt"
    regions.write_text(f"segarecomp.m68k_executable_regions.v1\nrom_sha256 {sha}\nrange 00000200 0000020e\nend\n", newline="\n")
    emit = [segarecomp, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"]

    def plan(name, closure, *extra):
        path = work / name
        return path, run(emit + list(extra) + ["--immutable-aot-region-proposal", str(regions), "--region-admission-plan-output", str(path), "--region-proposal-closure", closure])

    pruned_path, pruned = plan("prune.plan", "prune", "--ram-thunk", THUNK)
    assert not pruned_path.exists() and "outside_region" in pruned.stderr, pruned.stderr
    first_path, first = plan("close1.plan", "close", "--ram-thunk", THUNK)
    second_path, second = plan("close2.plan", "close", "--ram-thunk", THUNK)
    assert first.returncode == 0 and first_path.read_bytes() == second_path.read_bytes(), first.stderr
    ranges = [line.split()[1:] for line in first_path.read_text().splitlines() if line.startswith("range ")]
    assert any(int(b, 16) <= 0xFF0100 < int(e, 16) for b, e in ranges), "the thunk identity is admitted"
    assert any(int(b, 16) <= THUNK_TARGET < int(e, 16) for b, e in ranges), "closure follows the thunk's fixed target"
    assert not any(int(b, 16) <= DYNAMIC_TARGET < int(e, 16) for b, e in ranges), "closure does not follow a dynamic transfer"
    good = run(emit + ["--ram-thunk", THUNK, "--immutable-rom-aot-admission", str(first_path)])
    assert good.returncode == 0, good.stderr
    other = run(emit + ["--ram-thunk", "00ff0200:4ef900002000", "--immutable-rom-aot-admission", str(first_path)])
    assert other.returncode != 0, "a plan for another thunk set is refused"

    # The bridge builds without materialized thunks, so the generated behaviour uses the closed plan of the thunk-less emission.
    bridge_plan, closed = plan("bridge.plan", "close")
    assert closed.returncode == 0, closed.stderr
    bridge = [sys.executable, str(pathlib.Path(source) / "tools/genesis_startup_bridge.py"), "--segarecomp", segarecomp, "--rom", str(rom),
              "--mode", "synthetic", "--immutable-rom-aot", "--instruction-budget", str(BUDGET)]

    def build_and_run(name, extra):
        report = work / f"{name}.json"
        result = run(bridge + ["--out-dir", str(work / name), "--full-report-path", str(report)] + extra)
        assert result.returncode == 0, (name, result.stdout, result.stderr)
        return json.loads(report.read_text())

    broad = build_and_run("broad", [])
    selective = build_and_run("selective", ["--admission-plan", str(bridge_plan)])
    assert broad["result"] == "runner_resource_limit", broad["result"]
    assert selective["result"] == "stop" and selective["stop_class"] == "unresolved_indirect_target", (selective["result"], selective["stop_class"])
    shutil.rmtree(work, ignore_errors=True)
    print("genesis_ram_thunk_closure_generated_test: OK")


if __name__ == "__main__":
    main()
