#!/usr/bin/env python3
"""SEG-021-T023: the M68K reuse boundary (docs/architecture/m68k-reuse-boundary.md).

Guards that the MC68000 CPU library and its generated support carry no Genesis globals or identifiers and that
direct-route generated code can be instantiated with independent state (two live CPUs interleaved in one process).

usage: m68k_reuse_boundary_test.py <m68k_conformance_emitter> <c-compiler> <product-root>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

EMITTER, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
CPU = ROOT / "libs" / "cpu" / "m68k"
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror", "-Wno-type-limits", "-O0"]
# Machine-owned vocabulary that must not appear in CPU-owned code or in generated direct-route function bodies.
MACHINE_TOKENS = re.compile(r"\b(?:[Gg]enesis\w*|GENESIS\w*|runtime_state|work_ram|vdp\w*|ym2612\w*|z80\w*)\b")


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# 1. The CPU library is machine-independent: links only against base, and its code (comments aside) names no machine.
cmake = (CPU / "CMakeLists.txt").read_text(encoding="utf-8")
links = re.findall(r"target_link_libraries\(segarecomp_cpu_m68k\s+(?:PUBLIC|PRIVATE)\s+([^)]*)\)", cmake)
check(links and all(item == "segarecomp::base" for entry in links for item in entry.split()),
      "libs/cpu/m68k must link only segarecomp::base: %s" % links)
for path in sorted(CPU.rglob("*")):
    if path.suffix in {".cpp", ".hpp", ".h"}:
        code = strip_comments(path.read_text(encoding="utf-8"))
        check('#include "segarecomp/machine' not in code and '#include "segarecomp/platform' not in code
              and "platforms/genesis" not in code, "%s includes machine-owned code" % path)
        # The one recorded exception: the `M68kDecodeProfile::genesis_startup` profile NAME (a decode-policy
        # selector enumerator; it holds no Genesis state or code). See the reuse-boundary document.
        found = [t for t in MACHINE_TOKENS.findall(code) if t != "genesis_startup"]
        check(not found, "%s names machine-owned identifiers in code: %s" % (path, sorted(set(found))[:5]))

with tempfile.TemporaryDirectory(prefix="m68k-reuse-") as tmp:
    tmp = pathlib.Path(tmp)
    # 2. Header-only generated support units are stateless: strict C11, no external or defined data symbols.
    for header in ("timing_core.h", "exception_core.h"):
        unit = tmp / (header + ".c")
        unit.write_text('#include "%s"\nint reuse_unit_anchor(void) { return 0; }\n' % header, encoding="utf-8")
        obj = tmp / (header + ".o")
        subprocess.run([CC, *CFLAGS, "-I", str(CPU / "include" / "segarecomp" / "cpu" / "m68k"), "-c", str(unit),
                        "-o", str(obj)], check=True)
        symbols = subprocess.run(["nm", "-g", str(obj)], check=True, capture_output=True, text=True).stdout.split("\n")
        data = [line for line in symbols if re.match(r"^\S*\s+[BDCS]\s", line)]
        check(not data, "%s defines or requires data symbols: %s" % (header, data))
        undefined = [line for line in symbols if re.match(r"^\s*U\s", line)]
        check(not undefined, "%s requires external symbols: %s" % (header, undefined))

    # 3. Direct-route generated functions reference only their caller-supplied state and no machine vocabulary.
    encodings = ["D081", "22C0", "C0C1", "4E56FFFC", "4E5E"]
    tu = tmp / "cpu.c"
    result = subprocess.run([EMITTER, "--out", str(tu)], input="\n".join(encodings) + "\n", check=True,
                            capture_output=True, text=True)
    check(all(line.endswith(" ok") for line in result.stdout.strip().split("\n")), "emission failed: " + result.stdout)
    text = tu.read_text(encoding="utf-8")
    bodies = re.findall(r"static int cf_[0-9A-F]+\(cap_state \*s\) \{.*?\n\}\n", text, flags=re.S)
    check(len(bodies) == len(encodings), "expected one generated function per encoding")
    for body in bodies:
        check(not MACHINE_TOKENS.findall(body), "generated body names machine vocabulary: %s" % body[:80])
        check("frame_" not in body, "these forms need no call-frame state: %s" % body[:80])

    # 4. Two independent live CPUs, interleaved, must not influence each other (no shared hidden state).
    runner = tmp / "runner.c"
    runner.write_text('''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.c"
uint32_t frame_ids[64], frame_continuations[64], frame_depth; /* the harness's own frame slots; unused by these forms */
static uint32_t be32(const cap_state *s, uint32_t a) {
  return ((uint32_t)s->ram[a] << 24) | ((uint32_t)s->ram[a + 1] << 16) | ((uint32_t)s->ram[a + 2] << 8) | s->ram[a + 3];
}
static void prime(cap_state *s, uint32_t d0, uint32_t d1, uint32_t a1, uint32_t a6, uint32_t a7) {
  memset(s, 0, sizeof *s);
  s->d[0] = d0; s->d[1] = d1; s->a[1] = a1; s->a[6] = a6; s->a[7] = a7; s->sr = 0x2700U; s->pc = 0x2000U;
  s->ram[a7 - 4] = 0; /* the LINK frame lives below A7 */
}
static int fail(const char *what) { printf("FAIL: %s\\n", what); return 1; }
int main(void) {
  cap_state *a = malloc(sizeof *a), *b = malloc(sizeof *b);
  if (!a || !b) return 2;
  prime(a, 5U, 7U, 0x100U, 0x1234U, 0x800U);
  prime(b, 100U, 3U, 0x200U, 0xABCDU, 0x900U);
  for (int step = 0; cf_table[step].code != NULL; ++step) {
    if (cf_table[step].fn(a) != 0 || cf_table[step].fn(b) != 0) return fail("step stopped");
    if (step == 0 && (a->d[0] != 12U || b->d[0] != 103U)) return fail("ADD.L");
    if (step == 1 && (be32(a, 0x100U) != 12U || be32(b, 0x200U) != 103U || a->a[1] != 0x104U || b->a[1] != 0x204U))
      return fail("MOVE.L (A1)+");
    if (step == 2 && (a->d[0] != 84U || b->d[0] != 309U)) return fail("MULU.W");
    if (step == 3 && (a->a[6] != 0x7FCU || b->a[6] != 0x8FCU || a->a[7] != 0x7F8U || b->a[7] != 0x8F8U ||
                      be32(a, 0x7FCU) != 0x1234U || be32(b, 0x8FCU) != 0xABCDU)) return fail("LINK");
    if (step == 4 && (a->a[6] != 0x1234U || b->a[6] != 0xABCDU || a->a[7] != 0x800U || b->a[7] != 0x900U))
      return fail("UNLK");
  }
  if (a->pc != 0x2000U + 2U + 2U + 2U + 4U + 2U || a->pc != b->pc) return fail("program counters");
  if (be32(a, 0x200U) != 0U || be32(b, 0x100U) != 0U) return fail("cross-instance memory bleed");
  free(a); free(b);
  puts("OK");
  return 0;
}
''', encoding="utf-8")
    exe = tmp / "runner"
    subprocess.run([CC, *CFLAGS, "-I", str(tmp), str(runner), "-o", str(exe)], check=True)
    ran = subprocess.run([str(exe)], capture_output=True, text=True)
    check(ran.returncode == 0 and ran.stdout.strip() == "OK", "independent-instance run failed: " + ran.stdout + ran.stderr)

print("m68k reuse boundary OK")
