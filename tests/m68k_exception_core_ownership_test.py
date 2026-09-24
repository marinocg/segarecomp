#!/usr/bin/env python3
"""SEG-021-T018 / ADR 0043 §7: the MC68000 exception-entry / RTE core is M68K-owned and machine-independent.

Checks, fail-closed:

1. ``libs/cpu/m68k/include/segarecomp/cpu/m68k/exception_core.h`` includes only ``<stdint.h>``, names no
   Genesis (or other machine) identifier or path, and holds no file-scope mutable state (every file-scope
   ``static`` is a ``static inline`` function; no file-scope object definitions).
2. The header compiles standalone as strict C11 (``-std=c11 -Wall -Wextra -pedantic -Werror``).
3. Bound to a synthetic flat-memory machine written here (no Genesis code), the core implements the ADR 0043
   rules: validation and vector resolution precede the frame writes (ordered call log); user-mode and
   supervisor-mode entry perform exactly two frame writes (SR word at base, PC long at base+2) and change only
   those six bytes before committing; user-mode entry goes on the SSP with S = 0 saved and the USP parked in the inactive slot; validation
   and vector-resolution failures change nothing; RTE restores S = 0 onto the USP; a restored T = 1 is the
   deferred-trace refusal with nothing committed; two CPU instances bound to two contexts never share state.

4. ADR 0043 §5 guard: the frame-write hook is declared ``void`` (non-fallible after a successful
   ``validate_stack_extent``) and the core never consumes a frame-write result or returns ACCESS_FAILED from
   entry.

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
typedef struct { uint16_t sr; uint32_t a7, other, pc; } Cpu;
/* A synthetic machine-independent binding: flat memory image plus an ordered call log
   (V = validate_stack_extent, R = resolve_vector, W = frame_write, E = on_exception_entry). */
typedef struct {
  uint8_t ram[0x1000];
  int write_count, installed, refuse_extent;
  char log[16]; int log_len;
  uint32_t write_address[4], write_size[4], write_value[4];
  const Cpu *cpu; Cpu cpu_before; int wrote_after_commit;
  uint32_t validated_base, validated_length; int validated_direction;
} Machine;
static void note(Machine *m, char c) { if (m->log_len < 15) m->log[m->log_len++] = c; m->log[m->log_len] = 0; }
static int valid(void *c, uint32_t base, uint32_t length, SegarecompM68kStackDirection d) {
  Machine *m = (Machine *)c; note(m, 'V');
  m->validated_base = base; m->validated_length = length; m->validated_direction = (int)d;
  return !m->refuse_extent && base >= 0x100U && base + length <= 0x1000U; }
static int rd(void *c, uint32_t a, uint32_t size, uint32_t *v) {
  Machine *m = (Machine *)c; uint32_t i, x = 0;
  if (a + size > 0x1000U) return 0;
  for (i = 0; i < size; ++i) x = (x << 8) | m->ram[a + i];
  *v = x; return 1; }
static void wr(void *c, uint32_t a, uint32_t size, uint32_t v) {
  Machine *m = (Machine *)c; uint32_t i;
  note(m, 'W');
  if (m->write_count < 4) { m->write_address[m->write_count] = a; m->write_size[m->write_count] = size;
                            m->write_value[m->write_count] = v; }
  /* the hook guarantees success inside a validated extent; a write outside it would be a core bug */
  if (a < m->validated_base || a + size > m->validated_base + m->validated_length) m->wrote_after_commit = 1;
  if (m->cpu != 0 && memcmp(m->cpu, &m->cpu_before, sizeof *m->cpu) != 0) m->wrote_after_commit = 1;
  for (i = 0; i < size; ++i) m->ram[a + i] = (uint8_t)(v >> (8U * (size - 1U - i)));
  ++m->write_count; }
static SegarecompM68kVectorResolution vec(void *c, uint32_t v, uint32_t *h) {
  Machine *m = (Machine *)c; note(m, 'R');
  if (!m->installed) return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
  *h = 0x4000U + v * 0x10U; return SEGARECOMP_M68K_VECTOR_HANDLER; }
static void entered(void *c, uint32_t v, uint32_t base, uint32_t h) {
  (void)v; (void)base; (void)h; note((Machine *)c, 'E'); }
static int failures;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); ++failures; } }
static void arm(Machine *m, const Cpu *cpu) { m->log_len = 0; m->log[0] = 0; m->write_count = 0;
  m->cpu = cpu; m->cpu_before = *cpu; m->wrote_after_commit = 0; }
static void fill(Machine *m) { uint32_t i; for (i = 0; i < sizeof m->ram; ++i) m->ram[i] = (uint8_t)(0xA5U ^ i); }
/* Only [base, base + 6) differs from `image`, and it holds SR word then PC long. */
static int frame_only(const Machine *m, const uint8_t *image, uint32_t base, uint16_t sr, uint32_t pc) {
  uint8_t expect[0x1000]; memcpy(expect, image, sizeof expect);
  expect[base] = (uint8_t)(sr >> 8); expect[base + 1U] = (uint8_t)sr;
  expect[base + 2U] = (uint8_t)(pc >> 24); expect[base + 3U] = (uint8_t)(pc >> 16);
  expect[base + 4U] = (uint8_t)(pc >> 8); expect[base + 5U] = (uint8_t)pc;
  return memcmp(expect, m->ram, sizeof expect) == 0; }
int main(void) {
  static Machine m1, m2, m3;
  static uint8_t image[0x1000];
  Cpu c1 = {0x0015U, 0x0800U, 0x0C00U, 0x2000U}, c2 = {0x2700U, 0x0A00U, 0x0B00U, 0x3000U};
  Cpu c3 = {0x2304U, 0x0E00U, 0x0600U, 0x5000U};
  SegarecompM68kMachineHooks h1 = {&m1, valid, rd, wr, vec, entered, 0}, h2 = {&m2, valid, rd, wr, vec, 0, 0};
  SegarecompM68kMachineHooks h3 = {&m3, valid, rd, wr, vec, entered, 0};
  SegarecompM68kCpuBinding b1 = {&c1.sr, &c1.a7, &c1.other, &c1.pc}, b2 = {&c2.sr, &c2.a7, &c2.other, &c2.pc};
  SegarecompM68kCpuBinding b3 = {&c3.sr, &c3.a7, &c3.other, &c3.pc};
  uint32_t handler = 0, restored = 0;
  Cpu before;
  fill(&m1); fill(&m3);
  /* (3) extent-validation refusal: memory and SR/PC/active SP/inactive SP untouched, zero writes */
  m1.installed = 1; m1.refuse_extent = 1; before = c1; memcpy(image, m1.ram, sizeof image); arm(&m1, &c1);
  check(segarecomp_m68k_exception_enter(&h1, &b1, 8U, 0x2000U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_STACK_INVALID, "a refused extent fails closed");
  check(memcmp(&before, &c1, sizeof c1) == 0, "a refused extent leaves SR, PC and both SPs untouched");
  check(memcmp(image, m1.ram, sizeof image) == 0, "a refused extent leaves every memory byte untouched");
  check(m1.write_count == 0 && strcmp(m1.log, "V") == 0, "a refused extent performs zero frame writes");
  m1.refuse_extent = 0;
  /* (4) vector-resolution refusal: validated first, then refused, nothing written or committed */
  m1.installed = 0; arm(&m1, &c1);
  check(segarecomp_m68k_exception_enter(&h1, &b1, 8U, 0x2000U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE, "an absent vector fails closed");
  check(memcmp(&before, &c1, sizeof c1) == 0, "an absent vector leaves SR, PC and both SPs untouched");
  check(memcmp(image, m1.ram, sizeof image) == 0, "an absent vector leaves every memory byte untouched");
  check(m1.write_count == 0 && strcmp(m1.log, "VR") == 0, "an absent vector performs zero frame writes");
  m1.installed = 1; m2.installed = 1; m3.installed = 1;
  /* (1)(2)(5) user-mode privilege violation: V, R, then exactly two frame writes, then commit */
  arm(&m1, &c1);
  check(segarecomp_m68k_exception_enter(&h1, &b1, 8U, 0x2000U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_OK, "user-mode entry succeeds");
  check(strcmp(m1.log, "VRWWE") == 0, "user entry order: validate, resolve, two frame writes, commit/notify");
  check(m1.validated_base == 0x0BFAU && m1.validated_length == 6U &&
        m1.validated_direction == (int)SEGARECOMP_M68K_STACK_WRITE, "user entry validates [SSP-6, SSP) for write");
  check(m1.write_count == 2 && m1.write_address[0] == 0x0BFAU && m1.write_size[0] == 2U &&
        m1.write_value[0] == 0x0015U && m1.write_address[1] == 0x0BFCU && m1.write_size[1] == 4U &&
        m1.write_value[1] == 0x2000U, "user entry: SR word at base, PC long at base+2");
  check(!m1.wrote_after_commit, "user entry frame writes stay in the extent and precede the commit");
  check(frame_only(&m1, image, 0x0BFAU, 0x0015U, 0x2000U), "user entry changes exactly the six frame bytes");
  check(c1.a7 == 0x0BFAU && c1.other == 0x0800U && c1.sr == 0x2015U && c1.pc == 0x4080U && handler == 0x4080U,
        "entry switches to the SSP, parks the USP, sets S and jumps to the vector-8 handler");
  /* the second instance is untouched by the first */
  check(c2.sr == 0x2700U && c2.a7 == 0x0A00U && m2.write_count == 0, "instances share no state");
  /* RTE restores S = 0 onto the USP */
  check(segarecomp_m68k_exception_return(&h1, &b1, &restored) == SEGARECOMP_M68K_EXCEPTION_OK, "RTE succeeds");
  check(c1.sr == 0x0015U && c1.pc == 0x2000U && restored == 0x2000U && c1.a7 == 0x0800U && c1.other == 0x0C00U,
        "RTE restores the user SR/PC, re-activates the USP and parks the incremented SSP");
  /* (1)(2)(5) supervisor-mode origin: frame on the active SP, inactive USP untouched */
  memcpy(image, m3.ram, sizeof image); arm(&m3, &c3);
  check(segarecomp_m68k_exception_enter(&h3, &b3, 5U, 0x5006U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_OK, "supervisor-mode entry succeeds");
  check(strcmp(m3.log, "VRWWE") == 0, "supervisor entry order: validate, resolve, two frame writes, commit/notify");
  check(m3.validated_base == 0x0DFAU && m3.validated_length == 6U, "supervisor entry validates [SSP-6, SSP)");
  check(m3.write_count == 2 && m3.write_address[0] == 0x0DFAU && m3.write_size[0] == 2U &&
        m3.write_value[0] == 0x2304U && m3.write_address[1] == 0x0DFCU && m3.write_size[1] == 4U &&
        m3.write_value[1] == 0x5006U, "supervisor entry: SR word at base, PC long at base+2");
  check(!m3.wrote_after_commit, "supervisor entry frame writes stay in the extent and precede the commit");
  check(frame_only(&m3, image, 0x0DFAU, 0x2304U, 0x5006U), "supervisor entry changes exactly the six frame bytes");
  check(c3.a7 == 0x0DFAU && c3.other == 0x0600U && c3.sr == 0x2304U && c3.pc == 0x4050U && handler == 0x4050U,
        "supervisor entry stays on the SSP, keeps the USP slot and jumps to the vector-5 handler");
  /* supervisor entry with an invalid SSP: nothing changes, no hook is reached for an odd SSP */
  c2.a7 = 0x0101U; before = c2; arm(&m2, &c2);
  check(segarecomp_m68k_exception_enter(&h2, &b2, 5U, 0x3002U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_STACK_INVALID, "an odd SSP fails closed");
  check(m2.log_len == 0, "an odd SSP is refused before any hook");
  c2.a7 = 0x0104U; before = c2; arm(&m2, &c2);
  check(segarecomp_m68k_exception_enter(&h2, &b2, 5U, 0x3002U, 0x7FFFU, 0x2000U, &handler) ==
        SEGARECOMP_M68K_EXCEPTION_STACK_INVALID, "a frame outside the machine's stack extent fails closed");
  check(m2.write_count == 0 && memcmp(&before, &c2, sizeof c2) == 0, "a refused frame writes nothing");
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


FRAME_WRITE_CONTRACT = (
    "ADR 0043 §5 (validate-then-commit): after a successful validate_stack_extent the exception-entry frame "
    "writes cannot fail, so the frame-write hook must be declared `void` and the core must never read a "
    "return value from it (no failure path may exist after the first frame byte is written)")


def check_frame_write_contract(text: str) -> None:
    code = strip_comments(text)
    declaration = re.search(r"([\w\s\*]+?)\(\s*\*\s*frame_write\s*\)\s*\(", code)
    assert declaration is not None, f"no frame_write hook declared; {FRAME_WRITE_CONTRACT}"
    return_type = " ".join(declaration.group(1).split()[-1:])
    assert return_type == "void", f"frame_write hook returns {return_type!r}; {FRAME_WRITE_CONTRACT}"
    assert not re.search(r"\(\s*\*\s*stack_write\s*\)", code), f"a fallible stack_write hook remains; {FRAME_WRITE_CONTRACT}"
    calls = list(re.finditer(r"hooks->frame_write\s*\(", code))
    assert len(calls) == 2, f"expected exactly two frame_write calls (SR word, PC long); {FRAME_WRITE_CONTRACT}"
    for call in calls:
        # A discarded expression statement: the previous token ends a statement/block, and the call is
        # immediately followed by `;` (nothing assigns, tests, casts or returns its value).
        preceding = code[:call.start()].rstrip()[-1:]
        depth, closing = 0, call.end() - 1
        for closing in range(call.end() - 1, len(code)):
            depth += {"(": 1, ")": -1}.get(code[closing], 0)
            if depth == 0:
                break
        following = code[closing + 1:].lstrip()[:1]
        assert preceding in (";", "{", "}") and following == ";", (
            f"the core consumes the frame_write result; {FRAME_WRITE_CONTRACT}")
    entry = code[code.index("segarecomp_m68k_exception_enter("):code.index("segarecomp_m68k_exception_return(")]
    assert "ACCESS_FAILED" not in entry, f"exception entry still has an ACCESS_FAILED path; {FRAME_WRITE_CONTRACT}"


def main() -> None:
    text = HEADER.read_text(encoding="utf-8")
    check_source(text)
    check_frame_write_contract(text)
    # Negative controls for the ADR 0043 §5 frame-write guard.
    for drift in (text.replace("void (*frame_write)", "int (*frame_write)", 1),
                  text.replace("  hooks->frame_write(hooks->context, frame_base, 2U",
                               "  if (!hooks->frame_write(hooks->context, frame_base, 2U", 1),
                  text.replace("  hooks->frame_write(hooks->context, frame_base + 2U, 4U, stacked_pc);",
                               "  hooks->frame_write(hooks->context, frame_base + 2U, 4U, stacked_pc);\n"
                               "  return SEGARECOMP_M68K_EXCEPTION_ACCESS_FAILED;", 1)):
        assert drift != text
        try:
            check_frame_write_contract(drift)
        except AssertionError:
            continue
        raise AssertionError("the frame-write guard accepted a fallible frame write")
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
