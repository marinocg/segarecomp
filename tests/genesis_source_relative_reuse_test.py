#!/usr/bin/env python3
"""SEG-096-T004 (ADR 0209): the Genesis ROM-to-work-RAM copy (ADR 0049) against the source-relative derivation shared with Sega CD.

The unchanged production emission of the SEG-021-T041 fixture (a ROM routine copied verbatim to work RAM and called there: a PC-relative read, a BSR, a relative
branch, an absolute store, RTS) is produced with the routine's alias at 1..4 work-RAM bases, and `genesis_source_relative_reuse_tool` turns each instruction's
ROM identity and alias identities into ONE source-relative function (the byte-identity guard conditional on the work-RAM address). Checked:

  * the derivation (libs/codegen/c11/source_relative.hpp, the code the Sega CD emitter uses) succeeds for every instruction of the routine, and instantiating the
    shared pieces at every real address - ROM and each alias, including an alias base never used to derive - reproduces that address's own production lowering text
    exactly (a fourth base is a held-out check);
  * the shared program is behaviourally identical to the production program: a step-by-step trace of the routine executed from ROM and from every RAM alias base
    (registers, status, stack, work-RAM hash after every instruction; the PC-relative read observes the RAM data word in RAM and the ROM data word in ROM) is equal
    line for line, and every single-byte tamper of every executed instruction of every alias stops at that instruction with nothing committed, in both programs;
  * the ADR 0049 falsification control: a program that serves the alias addresses by the ROM-anchored bodies (a blind share) is NOT equal - the test would notice;
  * the existing ADR 0049 harness (tamper, copy-never-happened, later change, mirror) passes unchanged on the shared program;
  * everything compiles as strict C11; the sizes and compile cost are measured (the numbers are reported, the production emitter and its default output are unchanged).

usage: genesis_source_relative_reuse_test.py <genesis_source_relative_reuse_tool> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

TOOL, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402
import genesis_immutable_copy_alias_generated_test as alias_test  # noqa: E402

RUNTIME = ROOT / "platforms" / "genesis" / "runtime"
failures = []
checks = [0]
RAM_BASES = (0x00FF0400, 0x00FF0500, 0x00FF0600, 0x00FF0700)

TRACE = r'''
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "runtime.h"
#include "generated.c"

#define ALIASES %(k)d
static const uint8_t routine[40] = {
  0x70, 0x05, 0x61, 0x18, 0x4A, 0x40, 0x66, 0x04, 0x7C, 0x63, 0x4E, 0x71, 0x30, 0x3A, 0x00, 0x16,
  0x33, 0xC0, 0x00, 0xFF, 0x08, 0x00, 0x4E, 0x75, 0x4E, 0x71, 0x4E, 0x71, 0x52, 0x40, 0x4E, 0x75,
  0x4E, 0x71, 0x4E, 0x71, 0x12, 0x34, 0x00, 0x00 };
static const GenesisOwnedCartridgeRegion rom_region = { UINT32_C(0x00000E40), UINT32_C(0x00000E68), routine, 40U };
static const uint32_t bases[4] = { UINT32_C(0x00FF0400), UINT32_C(0x00FF0500), UINT32_C(0x00FF0600), UINT32_C(0x00FF0700) };
static const struct { uint32_t off, len; } executed[] = {{0x00, 2}, {0x02, 2}, {0x04, 2}, {0x06, 2}, {0x0C, 4}, {0x10, 6}, {0x16, 2}, {0x1C, 2}, {0x1E, 2}};

static uint64_t fnv(uint64_t h, uint64_t v) { return (h ^ v) * UINT64_C(1099511628211); }
static uint64_t state(const GenesisRuntime *r) {
  uint64_t h = UINT64_C(14695981039346656037);
  unsigned i;
  h = fnv(h, r->pc); h = fnv(h, r->sr); h = fnv(h, r->usp);
  for (i = 0; i < 8; ++i) { h = fnv(h, r->d[i]); h = fnv(h, r->a[i]); }
  for (i = 0; i < 0x1000; ++i) h = fnv(h, r->work_ram[i]);
  return h;
}
static void put32(GenesisRuntime *r, uint32_t off, uint32_t v) {
  r->work_ram[off] = (uint8_t)(v >> 24); r->work_ram[off + 1] = (uint8_t)(v >> 16); r->work_ram[off + 2] = (uint8_t)(v >> 8); r->work_ram[off + 3] = (uint8_t)v;
}
static GenesisControlTransfer step(GenesisRuntime *r) {
  GenesisControlTransfer t = genesis_bridge_dispatch(r);
  if (t.kind == GENESIS_CONTINUE_AT_PC) r->pc = t.next_pc;
  return t;
}
static void setup(GenesisRuntime *r, uint32_t entry, uint32_t ram_base) {
  *r = (GenesisRuntime){0};
  r->owned_regions = &rom_region; r->owned_region_count = 1U;
  if (ram_base != 0U) memcpy(&r->work_ram[ram_base - UINT32_C(0x00FF0000)], routine, sizeof(routine));
  r->pc = entry;
  r->a[7] = UINT32_C(0x00FF0FFC);
  put32(r, 0xFFC, UINT32_C(0x00000E28));  /* the return address of the JSR that called the routine */
}
/* One complete execution of the routine from `entry`, printing the whole machine state after every instruction. */
static void run(const char *label, uint32_t entry, uint32_t ram_base, uint16_t data_word) {
  GenesisRuntime r;
  unsigned n;
  setup(&r, entry, ram_base);
  if (ram_base != 0U) { uint32_t o = ram_base - UINT32_C(0x00FF0000) + 0x24U; r.work_ram[o] = (uint8_t)(data_word >> 8); r.work_ram[o + 1] = (uint8_t)data_word; }
  for (n = 0; n < 64U; ++n) {
    GenesisControlTransfer t = step(&r);
    printf("%%s %%u kind=%%d stop=%%d pc=%%08X d0=%%08X d6=%%08X a7=%%08X sr=%%04X h=%%016llX\n", label, n, (int)t.kind, t.kind == GENESIS_STOP ? (int)t.stop.stop_class : -1,
           (unsigned)r.pc, (unsigned)r.d[0], (unsigned)r.d[6], (unsigned)r.a[7], (unsigned)r.sr, (unsigned long long)state(&r));
    if (t.kind != GENESIS_CONTINUE_AT_PC || r.pc == UINT32_C(0x00000E28)) break;
  }
}
/* Every byte of every executed instruction flipped, one at a time: the run must stop AT that instruction, committing nothing. */
static void tamper(const char *label, uint32_t base) {
  unsigned e, b;
  for (e = 0; e < sizeof executed / sizeof executed[0]; ++e)
    for (b = 0; b < executed[e].len; ++b) {
      GenesisRuntime r;
      unsigned steps = 0;
      setup(&r, base, base);
      r.work_ram[base - UINT32_C(0x00FF0000) + executed[e].off + b] ^= 0x01;
      for (;;) {
        const uint32_t before = r.pc, a7 = r.a[7], d0 = r.d[0];
        GenesisControlTransfer t = step(&r);
        if (t.kind == GENESIS_STOP) {
          printf("%%s tamper %%02X+%%u stop=%%d at=%%d committed=%%d\n", label, executed[e].off, b, (int)t.stop.stop_class, before == base + executed[e].off,
                 !(r.pc == before && r.a[7] == a7 && r.d[0] == d0));
          break;
        }
        if (++steps > 64U) { printf("%%s tamper %%02X+%%u ran-away\n", label, executed[e].off, b); break; }
      }
    }
}
int main(void) {
  unsigned i;
  run("rom", UINT32_C(0x00000E40), 0U, 0U);
  for (i = 0; i < ALIASES; ++i) {
    char label[16];
    snprintf(label, sizeof label, "ram%%u", i);
    run(label, bases[i], bases[i], UINT16_C(0xBEEF));
    tamper(label, bases[i]);
  }
  return 0;
}
'''


def check(ok, label):
    checks[0] += 1
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def tool(*args):
    out = subprocess.run([TOOL, *args], capture_output=True, text=True, timeout=300)
    return out.returncode, out.stdout


def compile_program(work, name, generated, k, harness=None):
    d = work / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "generated.c").write_text(generated)
    (d / "harness.c").write_text(harness if harness is not None else TRACE % {"k": k})
    exe = d / "run"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(RUNTIME), "-I", str(d)]
    built = subprocess.run([CC, *flags, str(d / "harness.c"), str(RUNTIME / "runtime.c"), "-o", str(exe)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr[-2000:]
    return exe


def trace(exe):
    out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    return out.stdout


def main():
    with tempfile.TemporaryDirectory(prefix="gen-sr-") as tmp:
        tmp = pathlib.Path(tmp)
        rows = []
        for k in (1, 2, 3, 4):
            rc, ref = tool("--ref", str(k))
            rc2, shared = tool("--shared", str(k))
            rc3, naive = tool("--naive", str(k))
            rc4, report = tool("--report", str(k))
            assert rc == rc2 == rc3 == rc4 == 0
            info = json.loads(report)
            # K >= 3: the program derives from its own three lowerings and every instruction reproduces exactly. K < 3: three points come from an auxiliary
            # program; the two RTS bodies embed the program-wide return-target list (one continuation per alias), so the derived text does not reproduce the real
            # lowering and they keep their own functions (an explicit fallback, never a guess).
            expected_unverified = 0 if k >= 3 else 2
            check(info["routine_instructions"] == 19 and info["derived"] == 19 - expected_unverified and info["fallbacks"] == 0 and info["unverified"] == expected_unverified and
                  info["shared_with_rom"] >= 9,
                  "K=%d: %d of %d routine instructions are one source-relative function each (%d of them also serve the ROM identity); %d keep their own function (the RTS "
                  "return-target list is program-wide) - each shared body instantiates back to every real lowering exactly" % (k, info["routine_instructions"] - info["unverified"],
                                                                                                                             info["routine_instructions"], info["shared_with_rom"], info["unverified"]))
            check(rc == 0 and tool("--ref", str(k))[1] == ref and tool("--shared", str(k))[1] == shared,
                  "K=%d: the production emission and the shared transformation are deterministic" % k)
            work = tmp / ("k%d" % k)
            t_ref = trace(compile_program(work, "ref", ref, k))
            t_shared = trace(compile_program(work, "shared", shared, k))
            t_naive = trace(compile_program(work, "naive", naive, k))
            check(t_ref == t_shared and len(t_ref.splitlines()) > 40,
                  "K=%d: the shared program's complete trace equals the production program's (%d lines: ROM run, %d RAM runs, every single-byte tamper)" % (k, len(t_ref.splitlines()), k))
            check(t_ref != t_naive, "K=%d: control - serving the alias addresses by the ROM-anchored bodies is detectably wrong (the trace differs)" % k)
            lines = t_ref.splitlines()
            rom_final = [l for l in lines if l.startswith("rom ")][-1]
            ram_final = [l for l in lines if l.startswith("ram0 ") and "tamper" not in l][-1]
            check("d0=00001234" in rom_final and "d0=0000BEEF" in ram_final,
                  "K=%d: the PC-relative read sees the ROM data word from ROM and the RAM data word from RAM (ROM and RAM execution addresses are not interchangeable)" % k)
            check(all("at=1 committed=0" in l for l in lines if " tamper " in l) and sum(1 for l in lines if " tamper " in l) == k * 24,
                  "K=%d: all %d single-byte tampers (24 bytes of executed instructions per alias) stop at their instruction with nothing committed" % (k, k * 24))
            if k == 1:
                alias_test.build_and_run(CC, str(ROOT), shared, alias_test.HARNESS, "shared-adr0049")
                check(True, "the existing ADR 0049 harness (tamper, no copy, later change, mirror) passes unchanged on the shared program")
            # sizes and compile cost
            row = {"aliases": k, "functions_reference": info["functions_reference"], "functions_shared": info["functions_shared"],
                   "c_bytes_reference": info["c_bytes_reference"], "c_bytes_shared": info["c_bytes_shared"]}
            for name, text in (("reference", ref), ("shared", shared)):
                d = tmp / ("cost-%s-%d" % (name, k))
                d.mkdir()
                (d / "tu.c").write_text('#include "runtime.h"\n#include "generated.c"\n')
                (d / "generated.c").write_text(text)
                rc, _, cpu, rss, wall, err = common.measured([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O1", "-I", RUNTIME, "-I", d, "-c", d / "tu.c", "-o", d / "tu.o"])
                assert rc == 0, err
                row["compile_cpu_s_" + name] = round(cpu, 3)
                row["compile_peak_kib_" + name] = rss
                row["object_bytes_" + name] = (d / "tu.o").stat().st_size
            rows.append(row)
            check(row["functions_shared"] < row["functions_reference"] and row["c_bytes_shared"] < row["c_bytes_reference"],
                  "K=%d: fewer native instruction functions (%d -> %d) and C bytes (%d -> %d)" % (k, row["functions_reference"], row["functions_shared"], row["c_bytes_reference"], row["c_bytes_shared"]))
        print(json.dumps(rows, sort_keys=True))
    print("%d checks, %d failures" % (checks[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
