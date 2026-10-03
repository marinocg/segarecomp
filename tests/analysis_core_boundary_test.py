#!/usr/bin/env python3
"""SEG-029 (ADR 0078): the generic analysis core is CPU-free, and no production target links the analysis.

1. Forbidden identifiers: every file under libs/analysis/include is scanned (// and /* */ comments stripped, string literals kept)
   for M68K/Z80 register, effective-address, stack, exception, prefix, bank/mapper and platform terms, case-insensitively at word
   boundaries ('_' and '.' separate words). Its only project includes are other segarecomp/analysis headers.
2. Linkage: the analysis targets (segarecomp::analysis and every segarecomp::cpu_*_analysis adapter) are referenced only by
   libs/analysis, libs/cpu/<cpu>/analysis, the SEG-030 report-only driver platforms/genesis/analysis_report/CMakeLists.txt (ADR 0079
   decision 2) and tests/ (CMakeLists.txt and *.cmake); no app, platform, codegen or other library CMakeLists.txt or *.cmake links them.
3. The report-only driver (segarecomp::genesis_analysis_report / segarecomp_genesis_analysis_report and the executable
   segarecomp-genesis-analysis-report) is referenced only by its own directory and tests/; that directory declares exactly those
   targets and never install()s. No production source (apps/, platforms/, libs/ outside the analysis libraries) includes an analysis
   or driver header.
Self-checks plant violations and require the scans to find them.

usage: analysis_core_boundary_test.py <source-root>
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1]).resolve()
CORE = root / "libs" / "analysis" / "include"

FORBIDDEN = [
    # M68K
    "m68k", "mc68000", "68000", "68k", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "a5", "a6",
    "a7", "usp", "ssp", "ccr", "rte", "rtr", "rts", "jsr", "jmp", "ea", "effective_address", "pc_index", "trap",
    # Z80
    "z80", "hl", "ix", "iy", "de", "bc", "af", "sp", "reti", "retn", "djnz", "prefix", "dd", "fd", "cb", "ed", "exx",
    # stack / exception / platform
    "stack", "exception", "interrupt", "bank", "mapper", "slot", "epoch", "signature", "genesis", "sms", "master_system", "mega",
    "cartridge", "vdp", "ym2612", "psg",
]
LINKED = re.compile(r"segarecomp(?:::|_)(?:analysis|cpu_[a-z0-9]+_analysis)\b")
REPORT = re.compile(r"segarecomp(?:::|_)genesis_analysis_report\b|segarecomp-genesis-analysis-report")
REPORT_DIR = "platforms/genesis/analysis_report/"
REPORT_CMAKE = REPORT_DIR + "CMakeLists.txt"
REPORT_TARGETS = {"segarecomp_genesis_analysis_report", "segarecomp::genesis_analysis_report", "segarecomp-genesis-analysis-report"}
ANALYSIS_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]segarecomp/(?:analysis|cpu/[a-z0-9]+/analysis|genesis_analysis_report)/', re.M)

failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            out.append(" ")
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


PATTERNS = [(w, re.compile(r"(?<![A-Za-z0-9])" + re.escape(w) + r"(?![A-Za-z0-9])", re.I)) for w in FORBIDDEN]


def scan(code):
    return sorted({w for w, p in PATTERNS if p.search(code)})


def project_includes(code):
    return [p for k, p in re.findall(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', code, re.M) if k == '"' or "/" in p]


def is_tests_cmake(rel):
    return rel == "tests/CMakeLists.txt" or re.fullmatch(r"tests/[^/]+\.cmake", rel) is not None


def allowed_cmake(path):
    rel = path.relative_to(root).as_posix()
    return (rel.startswith("libs/analysis/") or re.match(r"libs/cpu/[^/]+/analysis/", rel) is not None
            or rel == REPORT_CMAKE or is_tests_cmake(rel))


def allowed_report_cmake(path):
    rel = path.relative_to(root).as_posix()
    return rel == REPORT_CMAKE or is_tests_cmake(rel)


def allowed_include(path):
    rel = path.relative_to(root).as_posix()
    return (rel.startswith(("libs/analysis/", REPORT_DIR, "tests/")) or re.match(r"libs/cpu/[^/]+/analysis/", rel) is not None)


def cmake_code(text):
    return strip_comments(text.replace("#", "//"))


def declared_targets(code):
    return {m.group(2) for m in re.finditer(r"\badd_(library|executable)\s*\(\s*([A-Za-z0-9_:.+-]+)", code)}


def skipped(rel):
    return rel.startswith(("build", ".", "games/")) or "/_deps/" in rel


def cmake_files():
    return sorted(p for pattern in ("CMakeLists.txt", "*.cmake") for p in root.rglob(pattern)
                  if not skipped(p.relative_to(root).as_posix()))


def main():
    headers = sorted(CORE.rglob("*.hpp"))
    check(len(headers) >= 2, "core headers present (%d)" % len(headers))
    for header in headers:
        code = strip_comments(header.read_text())
        hits = scan(code)
        check(not hits, header.name + " names no forbidden identifier" + (": " + ", ".join(hits) if hits else ""))
        bad = [p for p in project_includes(code) if not p.startswith("segarecomp/analysis/")]
        check(not bad, header.name + " includes only segarecomp/analysis headers" + (": " + ", ".join(bad) if bad else ""))

    offenders, report_offenders = [], []
    for cmake in cmake_files():
        rel = cmake.relative_to(root).as_posix()
        code = cmake_code(cmake.read_text())
        if LINKED.search(code) and not allowed_cmake(cmake):
            offenders.append(rel)
        if REPORT.search(code) and not allowed_report_cmake(cmake):
            report_offenders.append(rel)
    check(not offenders, "no production CMakeLists.txt/*.cmake links the analysis" + (": " + ", ".join(offenders) if offenders else ""))
    check(not report_offenders, "only the driver directory and tests/ reference the report-only driver" +
          (": " + ", ".join(report_offenders) if report_offenders else ""))
    report_cmake = root / REPORT_CMAKE
    check(report_cmake.is_file(), "the report-only driver directory exists")
    if report_cmake.is_file():
        code = cmake_code(report_cmake.read_text())
        targets = declared_targets(code)
        check(targets == REPORT_TARGETS, "the driver directory declares exactly the driver library, its alias and executable: " +
              ", ".join(sorted(targets)))
        check(re.search(r"\binstall\s*\(", code) is None, "the driver is never installed")
    include_offenders = []
    for source in sorted(p for top in ("apps", "libs", "platforms") for ext in ("*.hpp", "*.cpp", "*.h", "*.c")
                         for p in (root / top).rglob(ext)):
        if not allowed_include(source) and ANALYSIS_INCLUDE.search(strip_comments(source.read_text(errors="replace"))):
            include_offenders.append(source.relative_to(root).as_posix())
    check(not include_offenders, "no production source includes an analysis or driver header" +
          (": " + ", ".join(include_offenders) if include_offenders else ""))
    tests_cmake = (root / "tests" / "CMakeLists.txt").read_text()
    check("segarecomp::analysis" in tests_cmake, "the core is exercised by tests")

    check(scan(strip_comments('auto x = "HL";')) == ["hl"], "self-check: a token in a string literal is detected")
    check(scan(strip_comments("int v; // z80 bank\n/* D0 */")) == [], "self-check: comments are ignored")
    check(scan("struct Stack_Frame;") == ["stack"] and scan("int idea;") == [], "self-check: '_' separates words; substrings do not")
    check(LINKED.search("target_link_libraries(x PRIVATE segarecomp::cpu_z80_analysis)") is not None and
          LINKED.search("segarecomp::cpu_z80") is None, "self-check: adapter targets match, CPU targets do not")
    check(not allowed_cmake(root / "apps" / "segarecomp" / "CMakeLists.txt") and
          allowed_cmake(root / "libs" / "cpu" / "z80" / "analysis" / "CMakeLists.txt"), "self-check: allowed-path rule")
    check(not allowed_cmake(root / "platforms" / "genesis" / "machine" / "CMakeLists.txt") and
          not allowed_cmake(root / "platforms" / "genesis" / "analysis_report" / "other.cmake") and
          allowed_cmake(root / REPORT_CMAKE) and allowed_cmake(root / "tests" / "analysis_report_tests.cmake") and
          not allowed_cmake(root / "tests" / "sub" / "x.cmake"),
          "self-check: the driver directory is admitted; apps/segarecomp and platforms/genesis/machine are not")
    planted_app = cmake_code("add_executable(segarecomp main.cpp)\ntarget_link_libraries(segarecomp PRIVATE segarecomp::genesis_analysis_report)")
    check(REPORT.search(planted_app) is not None and not allowed_report_cmake(root / "apps" / "segarecomp" / "CMakeLists.txt"),
          "self-check: a planted driver reference in the apps CMakeLists.txt is caught")
    check(REPORT.search(cmake_code("# segarecomp::genesis_analysis_report in a comment")) is None and
          REPORT.search("add_test(NAME x COMMAND $<TARGET_FILE:segarecomp-genesis-analysis-report>)") is not None and
          REPORT.search("segarecomp::genesis_analysis_reporter") is None, "self-check: driver target pattern")
    planted_cmake = root / "platforms" / "genesis" / "machine" / "planted.cmake"
    check(LINKED.search(cmake_code("target_link_libraries(x PRIVATE segarecomp::cpu_m68k_analysis)")) is not None and
          not allowed_cmake(planted_cmake), "self-check: a planted *.cmake link is caught")
    check(declared_targets("add_library(a src.cpp)\nadd_library(b::a ALIAS a)\nadd_executable(c-d m.cpp)") == {"a", "b::a", "c-d"},
          "self-check: target declarations are parsed")
    check(ANALYSIS_INCLUDE.search('#include "segarecomp/genesis_analysis_report/report.hpp"') is not None and
          ANALYSIS_INCLUDE.search('#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"') is not None and
          ANALYSIS_INCLUDE.search('#include "segarecomp/cpu/m68k/decode.hpp"') is None and
          not allowed_include(root / "apps" / "segarecomp" / "main.cpp") and allowed_include(root / REPORT_DIR / "src" / "main.cpp"),
          "self-check: a planted analysis include in a production source is caught")

    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("analysis_core_boundary_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
