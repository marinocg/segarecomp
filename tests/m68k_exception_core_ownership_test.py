#!/usr/bin/env python3
"""SEG-021-T018 / ADR 0043 §7: the MC68000 exception-entry / RTE core is M68K-owned and machine-independent.

Checks, fail-closed:

1. ``libs/cpu/m68k/include/segarecomp/cpu/m68k/exception_core.h`` includes only ``<stdint.h>``, names no
   Genesis (or other machine) identifier or path, and holds no file-scope mutable state (every file-scope
   ``static`` is a ``static inline`` function; no file-scope object definitions).
2. The header compiles standalone as strict C11 (``-std=c11 -Wall -Wextra -pedantic -Werror``).
3. Bound to a synthetic flat-memory machine written here (no Genesis code), the core implements the ADR 0043
   rules: user-mode entry goes on the SSP with S = 0 saved and the USP parked in the inactive slot; validation
   and vector-resolution failures change nothing; RTE restores S = 0 onto the USP; a restored T = 1 is the
   deferred-trace refusal with nothing committed; two CPU instances bound to two contexts never share state.

usage: m68k_exception_core_ownership_test.py [<cc>]
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "libs" / "cpu" / "m68k" / "include" / "segarecomp" / "cpu" / "m68k" / "exception_core.h"
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror"]

HARNESS = r"""
#include "exception_core.h"
#include <stdio.h>
#include <string.h>
typedef struct { uint8_t ram[0x1000]; int write_count; int installed; } Machine;
static int valid(void *c, uint32_t base, uint32_t length, SegarecompM68kStackDirection d) {
  (void)c; (void)d; return base >= 0x100U && base + length <= 0x1000U; }
static int rd(void *c, uint32_t a, uint32_t size, uint32_t *v) {
  Machine *m = (Machine *)c; uint32_t i, x = 0;
  if (a + size > 0x1000U) return 0;
  for (i = 0; i < size; ++i) x = (x << 8) | m->ram[a + i];
  *v = x; return 1; }
static int wr(void *c, uint32_t a, uint32_t size, uint32_t v) {
  Machine *m = (Machine *)c; uint32_t i;
  if (a + size > 0x1000U) return 0;
  for (i = 0; i < size; ++i) m->ram[a + i] = (uint8_t)(v >> (8U * (size - 1U - i)));
  ++m->write_count; return 1; }
static SegarecompM68kVectorResolution vec(void *c, uint32_t v, uint32_t *h) {
  if (!((Machine *)c)->installed) return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
  *h = 0x4000U + v * 0x10U; return SEGARECOMP_M68K_VECTOR_HANDLER; }
typedef struct { uint16_t sr; uint32_t a7, other, pc; } Cpu;
static int failures;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); ++failures; } }
int main(void) {
  static Machine m1, m2;
  Cpu c1 = {0x0015U, 0x0800U, 0x0C00U, 0x2000U}, c2 = {0x2700U, 0x0A00U, 0x0B00U, 0x3000U};
  SegarecompM68kMachineHooks h1 = {&m1, valid, rd, wr, vec, 0, 0}, h2 = {&m2, valid, rd, wr, vec, 0, 0};
  SegarecompM68kCpuBinding b1 = {&c1.sr, &c1.a7, &c1.other, &c1.pc}, b2 = {&c2.sr, &c2.a7, &c2.other, &c2.pc};
  uint32_t handler = 0, restored = 0;
  Cpu before;
  /* vector not installed: nothing written, nothing committed */
  before = c1;
  check(segarecomp_m68k_exception_enter(&h1, &b1, 8U, 0x2000U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE, "an absent vector fails closed");
  check(memcmp(&before, &c1, sizeof c1) == 0 && m1.write_count == 0, "an absent vector changes nothing");
  m1.installed = 1; m2.installed = 1;
  /* user-mode privilege violation: frame on the SSP (inactive slot), S = 0 saved, USP parked */
  check(segarecomp_m68k_exception_enter(&h1, &b1, 8U, 0x2000U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_OK, "user-mode entry succeeds");
  check(c1.a7 == 0x0BFAU && c1.other == 0x0800U && c1.sr == 0x2015U && c1.pc == 0x4080U && handler == 0x4080U,
        "entry switches to the SSP, parks the USP, sets S and jumps to the vector-8 handler");
  check(m1.ram[0xBFA] == 0x00U && m1.ram[0xBFB] == 0x15U && m1.ram[0xBFC] == 0x00U && m1.ram[0xBFD] == 0x00U &&
        m1.ram[0xBFE] == 0x20U && m1.ram[0xBFF] == 0x00U, "six-byte frame: saved SR (S = 0) then stacked PC");
  /* the second instance is untouched by the first */
  check(c2.sr == 0x2700U && c2.a7 == 0x0A00U && m2.write_count == 0, "instances share no state");
  /* RTE restores S = 0 onto the USP */
  check(segarecomp_m68k_exception_return(&h1, &b1, &restored) == SEGARECOMP_M68K_EXCEPTION_OK, "RTE succeeds");
  check(c1.sr == 0x0015U && c1.pc == 0x2000U && restored == 0x2000U && c1.a7 == 0x0800U && c1.other == 0x0C00U,
        "RTE restores the user SR/PC, re-activates the USP and parks the incremented SSP");
  /* supervisor entry with an invalid SSP: nothing changes */
  c2.a7 = 0x0101U; before = c2;
  check(segarecomp_m68k_exception_enter(&h2, &b2, 5U, 0x3002U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_STACK_INVALID, "an odd SSP fails closed");
  c2.a7 = 0x0104U;
  check(segarecomp_m68k_exception_enter(&h2, &b2, 5U, 0x3002U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_STACK_INVALID, "a frame outside the machine's stack extent fails closed");
  check(m2.write_count == 0 && c2.sr == before.sr && c2.pc == before.pc, "a refused frame writes nothing");
  /* RTE of a T = 1 frame: the deferred-trace refusal commits nothing */
  c2.a7 = 0x0A00U; m2.ram[0xA00] = 0xA7U; m2.ram[0xA01] = 0x00U; m2.ram[0xA05] = 0x40U; before = c2;
  check(segarecomp_m68k_exception_return(&h2, &b2, &restored) == SEGARECOMP_M68K_EXCEPTION_TRACE_DEFERRED,
        "RTE with T = 1 is the deferred-trace refusal");
  check(memcmp(&before, &c2, sizeof c2) == 0, "the deferred-trace refusal restores nothing");
  if (failures == 0) printf("ok\n");
  return failures == 0 ? 0 : 1;
}
"""


def strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL))


def check_source(text: str) -> None:
    code = strip_comments(text)
    includes = re.findall(r"^\s*#\s*include\s*(\S+)", code, re.MULTILINE)
    assert includes == ["<stdint.h>"], f"the exception core may include only <stdint.h>, found {includes}"
    for forbidden in ("genesis", "machine/", "platforms/", "device/"):
        assert forbidden not in code.lower(), f"the exception core names {forbidden!r}"
    for match in re.finditer(r"\bstatic\b(?!\s+inline\b)", code):
        raise AssertionError(f"file-scope static that is not a static inline function at offset {match.start()}")
    assert "extern" not in code, "the exception core declares external state"
    # No file-scope object definitions: every depth-0 statement is a preprocessor line, a typedef or a
    # function definition.
    depth, statement = 0, ""
    for line in code.splitlines():
        if depth == 0 and line.lstrip().startswith("#"):
            continue
        for char in line + "\n":
            if depth == 0:
                statement += char
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0 and not statement.lstrip().startswith("typedef"):
                    statement = ""  # a function body ends the definition
            elif char == ";" and depth == 0:
                stripped = " ".join(statement.split())
                assert stripped.startswith("typedef") or stripped.endswith("};") or stripped == ";", (
                    f"file-scope declaration that is not a typedef: {stripped!r}")
                statement = ""


def main() -> None:
    text = HEADER.read_text(encoding="utf-8")
    check_source(text)
    # Negative controls: the checker must reject drifted sources.
    for drift in (text.replace("#include <stdint.h>", "#include <stdint.h>\n#include \"runtime.h\"", 1),
                  text.replace("#define SEGARECOMP_M68K_SR_TRACE", "static uint32_t shared_state;\n#define SEGARECOMP_M68K_SR_TRACE", 1),
                  text.replace("typedef struct SegarecompM68kCpuBinding", "typedef struct GenesisCpuBinding", 1)):
        assert drift != text
        try:
            check_source(drift)
        except AssertionError:
            continue
        raise AssertionError("the ownership checker accepted a drifted exception core")
    cc = sys.argv[1] if len(sys.argv) > 1 else (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang"))
    if cc is None:
        print("m68k exception core ownership: ok (no C compiler; compile checks skipped)")
        return
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        (work / "only.c").write_text('#include "exception_core.h"\nint unused_translation_unit_anchor;\n')
        compiled = subprocess.run([cc, *CFLAGS, "-I", str(HEADER.parent), "-c", str(work / "only.c"), "-o",
                                   str(work / "only.o")], capture_output=True, text=True)
        assert compiled.returncode == 0, compiled.stderr
        (work / "harness.c").write_text(HARNESS)
        built = subprocess.run([cc, *CFLAGS, "-I", str(HEADER.parent), str(work / "harness.c"), "-o",
                                str(work / "harness")], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        run = subprocess.run([str(work / "harness")], capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout.strip() == "ok", run.stdout + run.stderr
    print("m68k exception core ownership: ok")


if __name__ == "__main__":
    main()
