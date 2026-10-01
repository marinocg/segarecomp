#!/usr/bin/env python3
"""SEG-033-T006: the production SMS build route stays inside its structural and resource budgets (extended tier).

usage: sms_build_benchmark_test.py <segarecomp-cli> <product-root> [<cc>]

Runs `tools/sms_build_benchmark.py` on a seeded random 128 KiB image through the real `segarecomp build` route (default
compile policy) and checks what must hold on any host: the build succeeds, the exact entry count is the image's start count,
owners are grouped and effect bodies shared (host function count an order of magnitude below the entry count), no compiler
process exceeds the ADR 0058 compiler-RSS budget, the executable stays within its size budget and the generated program starts.
Wall-clock time is recorded in the report but never compared with a fixed threshold (host variance); the only time bound is a
very generous ceiling that catches a hung or pathological build. The machine-relative figures live in the SEG-033 records.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

CLI, ROOT = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
CC = sys.argv[3] if len(sys.argv) > 3 else "cc"
ENTRIES = 1024 + 8 * 0x4000
LIMITS = {"owners": 1100, "functions": 20000, "process_rss_mib": 1536, "exe_mib": 64.0, "total_wall_s": 1800.0}


def main():
    failed = []
    with tempfile.TemporaryDirectory(prefix="sms-build-bench-") as tmp:
        report = pathlib.Path(tmp) / "report.json"
        run = subprocess.run([sys.executable, str(ROOT / "tools" / "sms_build_benchmark.py"), "--cli", CLI, "--root", str(ROOT),
                              "--image", "random:128", "--cc", CC, "--run-repeats", "1", "--run-cycles", "1000000", "--json", str(report),
                              "--work", str(pathlib.Path(tmp) / "work")], capture_output=True, text=True, timeout=3600)
        if run.returncode != 0:
            print("benchmark failed:", (run.stdout + run.stderr)[-800:])
            return 1
        data = json.loads(report.read_text())
    checks = [
        (data["exact_entries"] == ENTRIES, "exact entries %d, expected %d" % (data["exact_entries"], ENTRIES)),
        (data["unique_owners"] <= LIMITS["owners"], "owners %d > %d" % (data["unique_owners"], LIMITS["owners"])),
        (data["host_functions"] <= LIMITS["functions"], "host functions %d > %d" % (data["host_functions"], LIMITS["functions"])),
        (data["entries_per_owner"] >= 100, "entries per owner %s < 100" % data["entries_per_owner"]),
        (data["rss_mib"]["compile"]["proc_mib"] <= LIMITS["process_rss_mib"], "a compiler process used %s MiB" % data["rss_mib"]["compile"]["proc_mib"]),
        (data["executable_mib"] <= LIMITS["exe_mib"], "executable %s MiB" % data["executable_mib"]),
        (data["total_wall_s"] <= LIMITS["total_wall_s"], "build took %s s" % data["total_wall_s"]),
        (data["run"]["exit"] in (0, 2, 3), "the generated program did not start (exit %s)" % data["run"]["exit"]),
    ]
    for ok, message in checks:
        if not ok:
            failed.append(message)
    if failed:
        print("\n".join(failed))
        return 1
    print("sms build benchmark: ok (wall %.1f s, compile %.1f s, %d host functions, exe %.1f MiB)" %
          (data["total_wall_s"], data["compile_wall_s"], data["host_functions"], data["executable_mib"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
