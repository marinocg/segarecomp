#!/usr/bin/env python3
"""SEG-031 (ADR 0080): the explicit hybrid admission candidate end to end on a project-authored synthetic Genesis image.

1. The report-only planner (`segarecomp-genesis-analysis-report --hybrid-plan`) proves a bounded island: a strided code-pointer
   dispatch (128 slots) whose index walks every slot at run time, plus unreachable decodable code that broad AOT admits.
2. Broad and hybrid generated-native programs are built and run (strict C11, explicit instruction budget) through the unchanged
   bridge tool; the hybrid emission admits strictly fewer broad identities, yet the final runtime state and the result are identical.
3. Broad emission without the option is byte-identical to an emission given a `broad` plan, and repeated hybrid emission is
   byte-identical (deterministic).
4. Fail closed: a plan for another ROM digest, a malformed plan, a plan whose alias set differs and a plan that omits a fixed successor
   are rejected by the emitter; a tampered plan that omits one dynamic island member (a containment fact the production owner cannot
   see) builds, and the generated program stops fail-closed when it reaches that member: never wrong execution.
"""
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys

BUDGET = 20000


def rom_image() -> bytes:
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
        words(at, 0x4E71)                      # unreachable NOPs: broad identities, never hybrid
    entry = 0x200
    at = words(entry, 0x5878, 0xF000)         # ADDQ.W #4,($F000).W   (walks every slot)
    at = words(at, 0x3038, 0xF000)            # MOVE.W ($F000).W,D0
    at = words(at, 0x0240, 0x01FC)            # ANDI.W #$1FC,D0
    at = words(at, 0x41F9, 0x0000, 0x0800)    # LEA $800,A0
    at = words(at, 0x2270, 0x0000)            # MOVEA.L 0(A0,D0.W),A1
    words(at, 0x4ED1)                         # JMP (A1)
    for k in range(128):
        rom[0x800 + 4 * k:0x804 + 4 * k] = struct.pack(">I", 0x1000 + 16 * k)
        slot = words(0x1000 + 16 * k, 0x5278, 0xF004)   # ADDQ.W #1,($F004).W
        words(slot, 0x6000, (entry - (slot + 2)) & 0xFFFF)  # BRA.W entry
    return bytes(rom)


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


def main():
    segarecomp, driver, source, work = sys.argv[1:]
    work = pathlib.Path(work)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    rom = work / "rom.bin"
    data = rom_image()
    rom.write_bytes(data)
    sha = hashlib.sha256(data).hexdigest()

    plan_path = work / "hybrid.plan"
    planned = run([driver, "--rom", str(rom), "--rom-sha256", sha, "--reset-entry", "--private-output", str(work / "plan.private.json"),
                   "--hybrid-plan", str(plan_path)])
    assert planned.returncode == 0, planned.stderr
    aggregate = json.loads(planned.stdout)
    assert aggregate["outcome"] == "hybrid" and aggregate["fallback_island_count"] == 1, aggregate
    assert aggregate["hybrid_total"] < aggregate["broad_u"], aggregate
    plan_text = plan_path.read_text()
    assert plan_text.splitlines()[2] == "strategy hybrid", plan_text

    bridge = [sys.executable, str(pathlib.Path(source) / "tools/genesis_startup_bridge.py"), "--segarecomp", segarecomp,
              "--rom", str(rom), "--mode", "synthetic", "--immutable-rom-aot", "--instruction-budget", str(BUDGET)]

    def build_and_run(name, extra):
        out = work / name
        result = run(bridge + ["--out-dir", str(out), "--full-report-path", str(work / f"{name}.full.json")] + extra)
        assert result.returncode == 0, (name, result.stdout, result.stderr)
        full = json.loads((work / f"{name}.full.json").read_text())
        # Generated C: the sharded tree (large programs) or the single translation unit (below the shard threshold).
        files = [path for path in (out / "generated").rglob("*") if path.is_file()] if (out / "generated").exists() else []
        files += [path for path in out.glob("*.generated.c") if path.is_file()]
        generated = sum(path.stat().st_size for path in files)
        return full, generated, result.stderr

    broad, broad_bytes, _ = build_and_run("broad", [])
    hybrid, hybrid_bytes, hybrid_log = build_and_run("hybrid", ["--admission-plan", str(plan_path)])
    assert broad["result"] == "runner_resource_limit", broad["result"]
    assert hybrid["result"] == broad["result"] and hybrid["runtime"] == broad["runtime"], "hybrid behaviour differs from broad"
    assert broad_bytes > 0 and hybrid_bytes < broad_bytes, (broad_bytes, hybrid_bytes)

    emit = [segarecomp, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"]
    plain = run(emit)
    assert plain.returncode == 0, plain.stderr
    broad_plan = work / "broad.plan"
    broad_plan.write_text(f"segarecomp.m68k_hybrid_admission_plan.v1\nrom_sha256 {sha}\nstrategy broad\nend\n")
    with_broad = run(emit + ["--immutable-rom-aot-admission", str(broad_plan)])
    assert with_broad.returncode == 0 and with_broad.stdout == plain.stdout, "a broad plan must not change the emission"
    first = run(emit + ["--immutable-rom-aot-admission", str(plan_path)])
    second = run(emit + ["--immutable-rom-aot-admission", str(plan_path)])
    assert first.returncode == 0 and first.stdout == second.stdout and first.stdout != plain.stdout, "hybrid emission is deterministic"
    assert "m68k admission: strategy=hybrid" in first.stderr, first.stderr

    def rejected(text, reason):
        bad = work / "bad.plan"
        bad.write_text(text)
        result = run(emit + ["--immutable-rom-aot-admission", str(bad)])
        assert result.returncode == 2 and reason in result.stderr, (reason, result.returncode, result.stderr)

    lines = plan_text.splitlines()
    rejected(plan_text.replace(sha, "0" * 64), "rom_sha256_mismatch")
    rejected(plan_text.replace("strategy hybrid", "strategy maybe"), "plan_strategy")
    rejected("\n".join(lines[:2] + ["alias 00ff0000:00002000:00000010"] + lines[2:]) + "\n", "alias_set_mismatch")
    ranges = [line for line in lines if line.startswith("range ")]
    # Dropping the identity at $204 (the fallthrough of the entry instruction at $200) leaves a structurally open plan.
    fallthrough = next(line for line in ranges if int(line.split()[1], 16) <= 0x204 < int(line.split()[2], 16))
    assert int(fallthrough.split()[1], 16) == 0x204, fallthrough
    rejected(plan_text.replace(fallthrough + "\n", ""), "fixed_successor_not_admitted")

    # Omit one dynamic island member (slot 5): structurally closed, so production cannot see it; the program stops fail-closed there.
    slot5 = 0x1000 + 16 * 5
    tampered_lines = []
    for line in lines:
        if line.startswith("range "):
            begin, end = int(line.split()[1], 16), int(line.split()[2], 16)
            if begin <= slot5 < end:
                if begin < slot5:
                    tampered_lines.append(f"range {begin:08x} {slot5:08x}")
                if slot5 + 8 < end:
                    tampered_lines.append(f"range {slot5 + 8:08x} {end:08x}")
                continue
        tampered_lines.append(line)
    tampered = work / "tampered.plan"
    tampered.write_text("\n".join(tampered_lines) + "\n")
    escaped, _, _ = build_and_run("tampered", ["--admission-plan", str(tampered)])
    assert escaped["result"] != "runner_resource_limit" and escaped["stop_class"] is not None, escaped
    print("genesis_hybrid_admission_generated_test: OK", json.dumps({"broad_generated_bytes": broad_bytes,
                                                                       "hybrid_generated_bytes": hybrid_bytes,
                                                                       "broad_u": aggregate["broad_u"],
                                                                       "hybrid_total": aggregate["hybrid_total"]}))


if __name__ == "__main__":
    main()
