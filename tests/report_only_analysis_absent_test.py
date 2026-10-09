#!/usr/bin/env python3
"""SEG-047-T007 (ADR 0096): the superseded report-only abstract-analysis framework stays out of the product.

The SEG-029/030/031 abstract-analysis core, its M68K/Z80 adapters, the Genesis analysis-report driver, the external-proof (angr/Ghidra
fact) producers and the primary-word-classify proof helper were retired. Production admission is the broad immutable-ROM universe plus
the native ML region producer, structural pruning and the unchanged validator. This test keeps it that way:
  * the retired directories do not exist;
  * no CMake file defines or links an analysis/driver target, and no product source includes a retired header;
  * no build file depends on the optional Ghidra tooling (it is developer-only diagnostic evidence, never a completeness authority).
usage: report_only_analysis_absent_test.py <product-root>
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
RETIRED_DIRS = ("libs/analysis", "libs/cpu/m68k/analysis", "libs/cpu/z80/analysis", "platforms/genesis/analysis_report",
                "apps/m68k-primary-word-classify")
RETIRED_TOKENS = re.compile(r"segarecomp::analysis\b|cpu_m68k_analysis|cpu_z80_analysis|genesis_analysis_report|genesis-analysis-report|"
                            r"m68k-primary-word-classify|segarecomp/analysis/|cpu/m68k/analysis/|cpu/z80/analysis/|genesis_analysis_report/")
failures = []
for rel in RETIRED_DIRS:
    if (ROOT / rel).exists():
        failures.append(f"retired directory is back: {rel}")
scanned = 0
for base in ("libs", "platforms", "apps", "tests", "CMakeLists.txt", "packaging"):
    path = ROOT / base
    files = [path] if path.is_file() else [p for p in path.rglob("*") if p.is_file()] if path.exists() else []
    for f in files:
        if f.suffix not in {".cpp", ".hpp", ".h", ".c", ".txt", ".cmake", ".sh", ".yml", ".in"} and f.name != "CMakeLists.txt":
            continue
        if f.resolve() == pathlib.Path(__file__).resolve():
            continue
        scanned += 1
        text = f.read_text(encoding="utf-8", errors="ignore")
        if RETIRED_TOKENS.search(text):
            failures.append(f"{f.relative_to(ROOT)} references the retired analysis framework")
        if f.name == "CMakeLists.txt" or f.suffix == ".cmake":
            code = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))
            if re.search(r"tools/ghidra|ghidra\.py", code, flags=re.I):
                failures.append(f"{f.relative_to(ROOT)}: the build graph depends on Ghidra tooling")
assert scanned > 50, "scanner saw too few files"
# negative control: the token scanner bites.
assert RETIRED_TOKENS.search("target_link_libraries(x PRIVATE segarecomp::cpu_m68k_analysis)")
if failures:
    print("\n".join("FAIL: " + m for m in failures))
    sys.exit(1)
print("report-only analysis framework absent from the product graph: ok (%d files scanned)" % scanned)
