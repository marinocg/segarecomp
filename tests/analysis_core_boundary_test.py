#!/usr/bin/env python3
"""SEG-029 (ADR 0078): the generic analysis core is CPU-free, and no production target links the analysis.

1. Forbidden identifiers: every file under libs/analysis/include is scanned (// and /* */ comments stripped, string literals kept)
   for M68K/Z80 register, effective-address, stack, exception, prefix, bank/mapper and platform terms, case-insensitively at word
   boundaries ('_' and '.' separate words). Its only project includes are other segarecomp/analysis headers.
2. Linkage: the analysis targets (segarecomp::analysis and every segarecomp::cpu_*_analysis adapter) are referenced only by
   libs/analysis, libs/cpu/<cpu>/analysis and tests/CMakeLists.txt; no app, platform, codegen or other library CMakeLists.txt links them.
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


def allowed_cmake(path):
    rel = path.relative_to(root).as_posix()
    return (rel.startswith("libs/analysis/") or re.match(r"libs/cpu/[^/]+/analysis/", rel) is not None
            or rel == "tests/CMakeLists.txt")


def main():
    headers = sorted(CORE.rglob("*.hpp"))
    check(len(headers) >= 2, "core headers present (%d)" % len(headers))
    for header in headers:
        code = strip_comments(header.read_text())
        hits = scan(code)
        check(not hits, header.name + " names no forbidden identifier" + (": " + ", ".join(hits) if hits else ""))
        bad = [p for p in project_includes(code) if not p.startswith("segarecomp/analysis/")]
        check(not bad, header.name + " includes only segarecomp/analysis headers" + (": " + ", ".join(bad) if bad else ""))

    offenders = []
    for cmake in sorted(root.rglob("CMakeLists.txt")):
        rel = cmake.relative_to(root).as_posix()
        if rel.startswith(("build", ".", "games/")) or "/_deps/" in rel:
            continue
        if LINKED.search(strip_comments(cmake.read_text().replace("#", "//"))) and not allowed_cmake(cmake):
            offenders.append(rel)
    check(not offenders, "no production CMakeLists.txt links the analysis" + (": " + ", ".join(offenders) if offenders else ""))
    tests_cmake = (root / "tests" / "CMakeLists.txt").read_text()
    check("segarecomp::analysis" in tests_cmake, "the core is exercised by tests")

    check(scan(strip_comments('auto x = "HL";')) == ["hl"], "self-check: a token in a string literal is detected")
    check(scan(strip_comments("int v; // z80 bank\n/* D0 */")) == [], "self-check: comments are ignored")
    check(scan("struct Stack_Frame;") == ["stack"] and scan("int idea;") == [], "self-check: '_' separates words; substrings do not")
    check(LINKED.search("target_link_libraries(x PRIVATE segarecomp::cpu_z80_analysis)") is not None and
          LINKED.search("segarecomp::cpu_z80") is None, "self-check: adapter targets match, CPU targets do not")
    check(not allowed_cmake(root / "apps" / "segarecomp" / "CMakeLists.txt") and
          allowed_cmake(root / "libs" / "cpu" / "z80" / "analysis" / "CMakeLists.txt"), "self-check: allowed-path rule")

    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("analysis_core_boundary_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
