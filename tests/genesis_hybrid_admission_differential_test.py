#!/usr/bin/env python3
"""SEG-031 (ADR 0080): seeded differential of hybrid against broad generated-native execution on random island-producing images.

The generated program itself is the concrete executor: each seeded, project-authored Genesis image walks a strided code-pointer
dispatch (128 or 256 slots: above the 64 exact-offset bound, so every dispatch is an island, never an exact target) whose index
changes every iteration, with random slot bodies (branch back, a shared tail, a call to a common subroutine that returns, a second
island dispatched through the same proven index) and random unreachable decodable code. For each image the report-only planner
writes a plan, broad and hybrid programs are built and run through the unchanged bridge with the same explicit instruction budget,
and:
  * the plan must be hybrid with at least one island (else the image did not exercise the machinery: a failure);
  * the hybrid program must reach the same result and the identical final runtime state as broad (an execution that left the hybrid
    admission would stop fail-closed with an unresolved target instead: a BLOCKER);
  * the hybrid admits strictly fewer broad identities, and repeated planning is byte-identical.
"""
import hashlib
import json
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile

SEEDS = range(0x5E031, 0x5E031 + 10)
BUDGET = 30000
ENTRY = 0x200


class Rom:
    def __init__(self):
        self.data = bytearray(0x8000)
        self.data[0:4] = struct.pack(">I", 0x00FFFE00)
        self.data[4:8] = struct.pack(">I", ENTRY)
        self.data[0x100:0x110] = b"SEGA GENESIS    "

    def words(self, at, *values):
        for value in values:
            self.data[at:at + 2] = struct.pack(">H", value & 0xFFFF)
            at += 2
        return at

    def long(self, at, value):
        self.data[at:at + 4] = struct.pack(">I", value)

    def bra_w(self, at, target):
        return self.words(at, 0x6000, (target - (at + 2)) & 0xFFFF)

    def jmp_abs(self, at, target):
        return self.words(at, 0x4EF9, target >> 16, target)

    def jsr_abs(self, at, target):
        return self.words(at, 0x4EB9, target >> 16, target)


def build(seed):
    rng = random.Random(seed)
    rom = Rom()
    count = rng.choice((128, 256))
    stride = rng.choice((16, 32))
    call = rng.random() < 0.5
    ptrs, ptrs2, slots, slots2 = 0x800, 0xC00, 0x1000, 0x4000
    tail, sub = 0x7000, 0x7100
    step = rng.randrange(1, 9)
    # A direct call first: a static call frame gives broad AOT its return-target authority, so the slots' RTS are broad identities
    # (otherwise broad itself could not execute the call-mode images and the comparison would prove nothing).
    at = rom.jsr_abs(ENTRY, sub)
    at = rom.words(at, 0x5078 | (step & 7) << 9, 0xF000)      # ADDQ.W #step,($F000).W
    at = rom.words(at, 0x3038, 0xF000)                         # MOVE.W ($F000).W,D0
    at = rom.words(at, 0x0240, 4 * count - 4)                  # ANDI.W #mask,D0
    at = rom.words(at, 0x41F9, ptrs >> 16, ptrs)               # LEA ptrs,A0
    at = rom.words(at, 0x2270, 0x0000)                         # MOVEA.L 0(A0,D0.W),A1
    if call:
        at = rom.words(at, 0x4E91)                             # JSR (A1)
        rom.bra_w(at, ENTRY)
    else:
        rom.words(at, 0x4ED1)                                  # JMP (A1)
    rom.words(tail, 0x5278, 0xF006)                            # tail: ADDQ.W #1,($F006).W
    rom.bra_w(tail + 4, ENTRY)
    rom.words(sub, 0x5278, 0xF008, 0x4E75)                     # sub: ADDQ.W #1,($F008).W; RTS
    second = False
    for k in range(count):
        rom.long(ptrs + 4 * k, slots + stride * k)
        rom.long(ptrs2 + 4 * k, slots2 + stride * k)
        p = slots + stride * k
        kind = rng.randrange(3)
        if call:
            if kind == 0:
                rom.words(p, 0x4E71, 0x4E75)                   # NOP; RTS
            else:
                rom.words(rom.jsr_abs(p, sub), 0x4E75)         # JSR sub; RTS
        elif kind == 0:
            rom.bra_w(p, ENTRY)
        elif kind == 1:
            rom.jmp_abs(p, tail)
        else:
            second = True                                      # LEA ptrs2,A2; MOVEA.L 0(A2,D0.W),A3; JMP (A3)
            rom.words(p, 0x45F9, ptrs2 >> 16, ptrs2, 0x2672, 0x0000, 0x4ED3)
        q = slots2 + stride * k
        rom.bra_w(q, ENTRY)
    for junk in range(0x6000, 0x6000 + 2 * rng.randrange(64, 512), 2):
        rom.words(junk, rng.choice((0x4E71, 0x7001, 0x5240)))  # unreachable NOP / MOVEQ / ADDQ
    return bytes(rom.data), {"count": count, "stride": stride, "call": call, "step": step, "second_island": second}


def run(command):
    return subprocess.run(command, text=True, capture_output=True)


def main():
    segarecomp, driver, source = sys.argv[1:4]
    # The bridge requires an ignored output directory inside the product root (the precedent of the other bridge tests).
    (pathlib.Path(source) / "build").mkdir(exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(dir=pathlib.Path(source) / "build", prefix="hybrid-diff-"))
    bridge = pathlib.Path(source) / "tools/genesis_startup_bridge.py"
    totals = {"images": 0, "islands": 0, "broad_u": 0, "hybrid_total": 0}
    for seed in SEEDS:
        data, shape = build(seed)
        case = work / f"{seed:x}"
        case.mkdir()
        rom = case / "rom.bin"
        rom.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        plans = []
        for index in range(2):
            plan = case / f"plan{index}"
            planned = run([driver, "--rom", str(rom), "--rom-sha256", sha, "--reset-entry", "--private-output",
                           str(case / f"private{index}.json"), "--hybrid-plan", str(plan)])
            assert planned.returncode == 0, (seed, planned.stderr)
            plans.append((json.loads(planned.stdout), plan.read_text()))
        aggregate, plan_text = plans[0]
        assert plans[1] == plans[0], f"seed {seed:x}: non-deterministic plan"
        assert aggregate["outcome"] == "hybrid" and aggregate["fallback_island_count"] >= 1, (seed, shape, aggregate)
        assert aggregate["hybrid_total"] < aggregate["broad_u"], (seed, aggregate)
        outcomes = {}
        for name, extra in (("broad", []), ("hybrid", ["--admission-plan", str(case / "plan0")])):
            full = case / f"{name}.full.json"
            result = run([sys.executable, str(bridge), "--segarecomp", segarecomp, "--rom", str(rom), "--mode", "synthetic",
                          "--immutable-rom-aot", "--instruction-budget", str(BUDGET), "--out-dir", str(case / name),
                          "--full-report-path", str(full)] + extra)
            assert result.returncode == 0, (seed, name, result.stderr[-2000:])
            outcomes[name] = json.loads(full.read_text())
        broad, hybrid = outcomes["broad"], outcomes["hybrid"]
        assert broad["result"] == "runner_resource_limit", (seed, broad["result"], broad["stop_class"])
        assert hybrid["result"] == broad["result"] and hybrid["stop_class"] == broad["stop_class"], \
            f"BLOCKER seed {seed:x} {shape}: hybrid ended {hybrid['result']}/{hybrid['stop_class']}, broad {broad['result']}"
        assert hybrid["runtime"] == broad["runtime"], f"BLOCKER seed {seed:x} {shape}: hybrid final state differs from broad"
        totals["images"] += 1
        totals["islands"] += aggregate["fallback_island_count"]
        totals["broad_u"] += aggregate["broad_u"]
        totals["hybrid_total"] += aggregate["hybrid_total"]
        shutil.rmtree(case / "broad", ignore_errors=True)
        shutil.rmtree(case / "hybrid", ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    print("genesis_hybrid_admission_differential_test: OK", json.dumps(totals))


if __name__ == "__main__":
    main()
