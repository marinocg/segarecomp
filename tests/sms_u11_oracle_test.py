#!/usr/bin/env python3
"""SEG-009-T003 (open fact U11): instruction-start access ordering against both pinned machine references.

The project-authored `u11_probe` fixture (tools/sms_fixture_rom.py, analysis in sms_u11.py) reads the V counter behind k
superseded DD prefixes at instruction-start offsets 4n T after a line interrupt. A machine that orders accesses at the
instruction start (this platform's contract, machine contract section 2) sees the counter flip at the same n* for every k;
a machine that orders at the real bus cycle flips k steps earlier. The probe runs black-box through each finalist's
libretro core (tests/sms_oracle/libretro_host.c) and every deviation from the platform's n*(k) is CLASSIFIED and BOUNDED:
  * `instruction_start`: n*(k) independent of k (agrees with the platform);
  * `bus_cycle`: n*(k) = n*(0) - k (orders at the bus cycle of the access; the discrepancy is the 4 k T of the superseded
    prefixes, within the contract's 23 T + 4 T per prefix bound);
  * `bus_cycle_partial`: 0 <= n*(0) - n*(k) <= k with some k short of the full bus-cycle shift (within the same bound);
  * anything else (a shift outside 0..k, a missing flip, a non-monotone result) fails the test (unclassified).
The probe cannot resolve intra-instruction offsets below one NOP step (4 T) and cancels the plain-form offset (the same
read instruction is used throughout), so it classifies the prefix-chain component of U11 only.
The classification printed here is the U11 evidence; it does not change the platform (no Z80 ABI change is justified by a
4 T-per-prefix shift that only affects chains of superseded prefixes).

Skips cleanly (exit 0) unless SEGARECOMP_SMS_ORACLE_CHECKOUT holds the pinned checkouts (references are never run in CI).
usage: sms_u11_oracle_test.py [cc]
"""
import os
import pathlib
import sys
import tempfile
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "sms_oracle"))
sys.path.insert(0, str(HERE.parent / "tools"))
import pins  # noqa: E402
import sms_fixture_rom  # noqa: E402
import sms_oracle_smoke_test as smoke  # noqa: E402
import sms_u11 as u11  # noqa: E402


def classify(n_star):
    """Shift s(k) = n*(0) - n*(k) in NOP steps (4 T). The bus-cycle bound of the contract is one step per superseded prefix."""
    base = n_star[0]
    if base is None or any(v is None for v in n_star.values()):
        return "unclassified"
    shifts = {k: base - v for k, v in n_star.items() if k}
    if any(s < 0 or s > k for k, s in shifts.items()):
        return "unclassified"
    if all(s == 0 for s in shifts.values()):
        return "instruction_start"
    if all(s == k for k, s in shifts.items()):
        return "bus_cycle"
    return "bus_cycle_partial"


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    if os.name != "posix":
        print("skipped: the libretro host needs POSIX dlopen")
        return 0
    root = pins.checkout(smoke.NAMES)
    if root is None:
        print("skipped: " + pins.skip_reason(smoke.NAMES))
        return 0
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        host = str(tmp / "libretro_host")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O1",
                        str(HERE / "sms_oracle" / "libretro_host.c"), "-o", host, "-ldl"], check=True)
        rom_bytes, meta = sms_fixture_rom.build("u11_probe")
        rom = str(tmp / "u11_probe.sms")
        pathlib.Path(rom).write_bytes(rom_bytes)
        for name, spec in smoke.FINALISTS.items():
            core = smoke.build_core(root, spec, tmp)
            first = smoke.run(host, core, rom, spec["options"])
            second = smoke.run(host, core, rom, spec["options"])
            analysis = u11.analyze(bytes.fromhex(first["results"]))
            kind = classify(analysis["n_star"]) if analysis["monotone"] and first["complete"] else "incomplete"
            ok = first == second and kind in ("instruction_start", "bus_cycle", "bus_cycle_partial")
            print("%s: n*(k)=%r monotone=%s deterministic=%s -> %s" % (name, analysis["n_star"], analysis["monotone"], first == second, kind))
            failures += 0 if ok else 1
        print("sms u11 oracle: %d failure(s) (fixture sha256 %s)" % (failures, meta["sha256"][:16]))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
