#!/usr/bin/env python3
"""SEG-122-T011 (ADR 0271): static work-RAM `JMP (xxx).L` thunks with a live operand, on project-authored synthetic Genesis images.

Every image is generated here; no commercial data is involved. Generated programs run only through a C harness that includes the
emitted program, with explicit instruction budgets. Fixtures (lettered as in the task record):

A  V-interrupt thunk built by two immutable constant stores: its entry exists in the first emission, before any guest instruction runs.
B  H-interrupt thunk retargeted between two ROM handlers during execution; both run through the one precompiled body and the level-4
   vector is delivered (a real scheduler run).
C  late construction: the construction code is unreached by any preparation route; the entry still exists and works once built.
D  not-yet-installed thunk: the guard refuses, nothing committed.
E  unsupported mutation: another opcode (JSR, JMP.W, NOP) over the thunk refuses with nothing committed.
F  invalid live operands: odd, unmapped, uncompiled targets fail with the typed stop, nothing committed; 24-bit masking.
G  ambiguous provenance: a conflicting reachable writer, an unreachable construction, no operand constructor, a register-indirect
   writer alone, and an unrelated RAM address never yield an entry.
H  legacy compatibility: an exact-byte materialized thunk over the same address is subsumed (no conflict); a different form is refused;
   an unrelated materialized thunk is unchanged.
Also: deterministic emission, strict C11 compile, equal timing to a ROM `JMP (xxx).L`.
"""
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile

V_STUB, H_STUB = 0x00FFFFF0, 0x00FFFFF6
V_HANDLER, H_HANDLER_A, H_HANDLER_B = 0x1000, 0x2000, 0x3000
LOOP = 0x400  # ROM JMP $00002000.L used as the timing reference


def words(rom, at, *values):
    for value in values:
        rom[at:at + 2] = struct.pack(">H", value)
        at += 2
    return at


def image(code, vectors=True, extra=None):
    rom = bytearray(0x4000)
    rom[0:4] = struct.pack(">I", 0x00FFFE00)
    rom[4:8] = struct.pack(">I", 0x200)
    if vectors:
        rom[0x70:0x74] = struct.pack(">I", H_STUB)
        rom[0x78:0x7C] = struct.pack(">I", V_STUB)
    rom[0x100:0x110] = b"SEGA GENESIS    "
    words(rom, 0x200, *code)
    words(rom, V_HANDLER, 0x52B8, 0xFF00, 0x4E73)                                   # ADDQ.L #1,($FF00).W ; RTE
    words(rom, H_HANDLER_A, 0x52B8, 0xFF04, 0x21FC, 0x0000, H_HANDLER_B, 0xFFF8, 0x4E73)   # count; retarget to B; RTE
    words(rom, H_HANDLER_B, 0x52B8, 0xFF08, 0x21FC, 0x0000, H_HANDLER_A, 0xFFF8, 0x4E73)   # count; retarget to A; RTE
    words(rom, LOOP, 0x4EF9, 0x0000, H_HANDLER_A)                                   # JMP $00002000.L (timing reference)
    for at, values in (extra or {}).items():
        words(rom, at, *values)
    return bytes(rom)


def stores(opcode_v=0x4EF9, operand_v=True, opcode_h=0x4EF9, operand_h=True):
    code = [0x31FC, opcode_v, 0xFFF0]
    if operand_v:
        code += [0x21FC, 0x0000, V_HANDLER, 0xFFF2]
    code += [0x31FC, opcode_h, 0xFFF6]
    if operand_h:
        code += [0x21FC, 0x0000, H_HANDLER_A, 0xFFF8]
    return code


KEEP_ON_C4 = [0x207C, 0x0000, 0x0200, 0x4ED0]  # MOVEA.L #$200,A0 ; JMP (A0): keeps the program on the generated C4 route
RUN_INTERRUPTS = [0x33FC, 0x8014, 0x00C0, 0x0004,   # VDP register 0: H-interrupt enable
                  0x33FC, 0x8164, 0x00C0, 0x0004,   # register 1: display and V-interrupt enable
                  0x33FC, 0x8A00, 0x00C0, 0x0004,   # register 10: H counter 0 (every line)
                  0x46FC, 0x2000]                    # MOVE #$2000,SR: all interrupt levels open


def spin(code):
    """`code`, then a MOVEA/JMP (A0) spin on itself: a generated C4 route that costs cycles so the scheduler raises interrupts."""
    head = 0x200 + 2 * len(code)
    return code + [0x207C, 0x0000, head, 0x4ED0]


def emit(cli, work, name, rom, *extra):
    path = work / f"{name}.bin"
    path.write_bytes(rom)
    sha = hashlib.sha256(rom).hexdigest()
    command = [cli, "emit-general-startup-bridge-c", "--rom", str(path), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot", *extra]
    result = subprocess.run(command, text=True, capture_output=True, timeout=240)
    return result


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "runtime.h"
#define main generated_bridge_main
#include "generated.c"
#undef main

static GenesisRuntime fresh(uint32_t pc) {
  GenesisRuntime r = {0};
  r.pc = pc; r.a[7] = UINT32_C(0x00FFFE00); r.sr = UINT16_C(0x2700);
  return r;
}
static void put(GenesisRuntime *r, uint32_t at, const uint8_t *bytes, uint32_t n) { memcpy(&r->work_ram[at - UINT32_C(0x00FF0000)], bytes, n); }
static void thunk(GenesisRuntime *r, uint32_t at, uint16_t opcode, uint32_t operand) {
  uint8_t b[6] = {(uint8_t)(opcode >> 8), (uint8_t)opcode, (uint8_t)(operand >> 24), (uint8_t)(operand >> 16), (uint8_t)(operand >> 8), (uint8_t)operand};
  put(r, at, b, 6);
}
static uint32_t counter(const GenesisRuntime *r, uint32_t off) {
  return ((uint32_t)r->work_ram[off] << 24) | ((uint32_t)r->work_ram[off + 1] << 16) | ((uint32_t)r->work_ram[off + 2] << 8) | r->work_ram[off + 3];
}
static void assert_untouched(const GenesisRuntime *r, const GenesisRuntime *before) {
  assert(r->pc == before->pc && r->sr == before->sr && memcmp(r->d, before->d, sizeof r->d) == 0 && memcmp(r->a, before->a, sizeof r->a) == 0);
  assert(r->scheduler.master_ticks == before->scheduler.master_ticks);
  assert(memcmp(r->work_ram, before->work_ram, sizeof r->work_ram) == 0);
}

int main(void) {
  GenesisRuntime r, before; GenesisControlTransfer t;
  const uint32_t V = UINT32_C(0x00FFFFF0), H = UINT32_C(0x00FFFFF6);

  /* A/B body: a verified thunk with two different live operands reaches two different compiled ROM targets, 12 cycles each. */
  r = fresh(V); thunk(&r, V, 0x4EF9, UINT32_C(0x00001000)); before = r;
  t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_CONTINUE_AT_PC && t.next_pc == UINT32_C(0x1000) && r.pc == UINT32_C(0x1000));
  const uint64_t thunk_ticks = r.scheduler.master_ticks - before.scheduler.master_ticks;
  assert(thunk_ticks != 0U);
  r = fresh(V); thunk(&r, V, 0x4EF9, UINT32_C(0x00003000));
  t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_CONTINUE_AT_PC && t.next_pc == UINT32_C(0x3000));
  assert(r.scheduler.master_ticks == thunk_ticks); /* the live operand does not change the timing */
  /* The same instruction as a ROM JMP (xxx).L retires with identical cost. */
  r = fresh(UINT32_C(0x400));
  t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_CONTINUE_AT_PC && t.next_pc == UINT32_C(0x2000) && r.scheduler.master_ticks == thunk_ticks);
  /* Same for the H stub, and a high-byte-garbage operand is masked to the 24-bit program bus. */
  r = fresh(H); thunk(&r, H, 0x4EF9, UINT32_C(0xAB002000));
  t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_CONTINUE_AT_PC && t.next_pc == UINT32_C(0x2000) && r.scheduler.master_ticks == thunk_ticks);

  /* D: not installed (RAM all zero). */
  r = fresh(V); before = r;
  t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_STOP && t.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
  assert_untouched(&r, &before);

  /* E: another opcode over the structural bytes: JSR (xxx).L, JMP (xxx).W, NOP, JMP (A0). Nothing committed. */
  const uint16_t other[] = {0x4EB9, 0x4EF8, 0x4E71, 0x4ED0, 0xCEF9};
  for (unsigned i = 0; i < sizeof other / sizeof other[0]; ++i) {
    r = fresh(H); thunk(&r, H, other[i], UINT32_C(0x00002000)); before = r;
    t = genesis_bridge_dispatch(&r);
    assert(t.kind == GENESIS_STOP && t.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET);
    assert_untouched(&r, &before);
  }

  /* F: invalid live operands: odd, unmapped device space, beyond the image, work RAM. Typed stop, nothing committed. */
  const uint32_t bad[] = {UINT32_C(0x00002001), UINT32_C(0x00A10000), UINT32_C(0x00500000), UINT32_C(0x00FF0100), UINT32_C(0x00000001), UINT32_C(0xFFFFFFFF)};
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
    r = fresh(V); thunk(&r, V, 0x4EF9, bad[i]); before = r;
    t = genesis_bridge_dispatch(&r);
    assert(t.kind == GENESIS_STOP && t.stop.stop_class == GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET);
    assert_untouched(&r, &before);
  }

  /* B: a real scheduler run. The mainline opens the interrupts; the V handler and the two retargeted H handlers all execute. */
  r = fresh(UINT32_C(0x200));
  r.irq6_handler_entry = V; r.irq6_handler_present = 1;                               /* exactly what the emitted main installs */
  r.software_exception_handler_entry[28] = H; r.software_exception_handler_present[28] = 1;
  r.owned_regions = genesis_owned_cartridge_regions; r.owned_region_count = UINT32_C(1);
  t = genesis_runtime_run(&r, genesis_bridge_dispatch, 400000U);
  const uint32_t v = counter(&r, 0xFF00), a = counter(&r, 0xFF04), b = counter(&r, 0xFF08);
  fprintf(stderr, "scheduler-run v=%u hA=%u hB=%u kind=%d\n", (unsigned)v, (unsigned)a, (unsigned)b, (int)t.kind);
  assert(v > 0U && a > 0U && b > 0U);
  assert(a == b || a == b + 1U);                         /* strict alternation through the one live operand */
  assert(t.kind != GENESIS_STOP);                        /* no typed stop: the interrupts were delivered, not dropped */
  return 0;
}
'''


SELECTIVE_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#define main generated_bridge_main
#include "generated.c"
#undef main
static GenesisRuntime fresh(uint32_t operand) {
  GenesisRuntime r = {0};
  r.pc = UINT32_C(0x00FFFFF0); r.a[7] = UINT32_C(0x00FFFE00); r.sr = UINT16_C(0x2700);
  r.work_ram[0xFFF0] = 0x4E; r.work_ram[0xFFF1] = 0xF9;
  r.work_ram[0xFFF2] = (uint8_t)(operand >> 24); r.work_ram[0xFFF3] = (uint8_t)(operand >> 16); r.work_ram[0xFFF4] = (uint8_t)(operand >> 8); r.work_ram[0xFFF5] = (uint8_t)operand;
  return r;
}
int main(void) {
  GenesisRuntime r = fresh(UINT32_C(0x00000200)); GenesisControlTransfer t = genesis_bridge_dispatch(&r);
  assert(t.kind == GENESIS_CONTINUE_AT_PC && t.next_pc == UINT32_C(0x200)); /* an admitted target */
  r = fresh(UINT32_C(0x00001000)); t = genesis_bridge_dispatch(&r);          /* a ROM handler the closed plan did not admit */
  assert(t.kind == GENESIS_STOP && t.stop.stop_class == GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET && r.pc == UINT32_C(0x00FFFFF0));
  return 0;
}
'''


def compile_and_run(cli_root, compiler, work, name, generated, harness=None):
    harness = HARNESS if harness is None else harness
    directory = work / name
    directory.mkdir(exist_ok=True)
    (directory / "generated.c").write_text(generated, encoding="utf-8")
    (directory / "harness.c").write_text(harness, encoding="utf-8")
    runtime_dir = pathlib.Path(cli_root) / "platforms/genesis/runtime"
    exe = directory / "run"
    built = subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O1",
                            "-I", str(runtime_dir), "-I", str(directory), str(directory / "harness.c"),
                            str(runtime_dir / "runtime.c"), "-o", str(exe)], text=True, capture_output=True)
    assert built.returncode == 0, built.stderr[:4000]
    return subprocess.run([str(exe)], text=True, capture_output=True, timeout=240)


def has_entry(text, address):
    return f"genesis_aot_{address:08X}(GenesisRuntime *runtime)" in text


def main():
    cli, compiler, root = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)

        # A/B/C: static recognition, first-emission entries, determinism.
        good = image(spin(stores() + RUN_INTERRUPTS))
        (work / "good.bin").write_bytes(good)
        first, second = emit(cli, work, "good", good), emit(cli, work, "good2", good)
        assert first.returncode == 0 and first.stdout == second.stdout, first.stderr
        assert has_entry(first.stdout, V_STUB) and has_entry(first.stdout, H_STUB), "both thunks exist in the first emission"
        assert "runtime.irq6_handler_present = 1;" in first.stdout and "software_exception_handler_present[28] = 1;" in first.stdout
        assert "irq6_vector_in_work_ram = 1" not in first.stdout
        # The guard compares the opcode word only; the operand is read live and checked against the compiled-entry lookup.
        body = first.stdout.split("genesis_aot_00FFFFF0(GenesisRuntime *runtime)")[1].split("genesis_aot_00FFFFF6")[0]
        assert "work_ram[UINT32_C(65520)] != UINT8_C(0x4E)" in body and "work_ram[UINT32_C(65521)] != UINT8_C(0xF9)" in body
        assert "work_ram[UINT32_C(65522)] != " not in body and "genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL" in body
        assert "pc = UINT32_C(" not in body, "the live target is never a fixed successor"

        # C: the construction is behind a branch the preparation never takes; the entries are still emitted statically.
        late = image([0x4A39, 0x00FF, 0x0000, 0x6704] + [0x60FE, 0x60FE] + stores() + KEEP_ON_C4)  # TST.B ($FF0000).L; BEQ over a spin
        late_out = emit(cli, work, "late", late)
        assert late_out.returncode == 0 and has_entry(late_out.stdout, V_STUB) and has_entry(late_out.stdout, H_STUB), late_out.stderr

        # Runtime semantics of A-F and the scheduler run of B.
        good_c4 = good
        out = emit(cli, work, "run", good_c4)
        ran = compile_and_run(root, compiler, work, "run_bin", out.stdout)
        assert ran.returncode == 0, (ran.stdout, ran.stderr)
        print(ran.stderr.strip())

        # Admission (ADR 0270/0271): a selective plan for the first emission holds both RAM identities as mandatory roots without any
        # preparation round, follows no representative operand, and leaves the live targets outside the admitted set (dynamic, unproven).
        sha = hashlib.sha256(good).hexdigest()
        regions = work / "regions.txt"
        regions.write_text(f"segarecomp.m68k_executable_regions.v1\nrom_sha256 {sha}\nrange 00000200 00000240\nend\n", newline="\n")
        rom_path = work / "good.bin"
        base = [cli, "emit-general-startup-bridge-c", "--rom", str(rom_path), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"]

        def plan(name, mode):
            path = work / name
            return path, subprocess.run(base + ["--immutable-aot-region-proposal", str(regions), "--region-admission-plan-output", str(path),
                                                "--region-proposal-closure", mode], text=True, capture_output=True, timeout=240)

        pruned_path, pruned = plan("p.plan", "prune")
        assert not pruned_path.exists() and "outside_region" in pruned.stderr, pruned.stderr
        closed_path, closed = plan("c1.plan", "close")
        again_path, again = plan("c2.plan", "close")
        assert closed.returncode == 0 and closed_path.read_bytes() == again_path.read_bytes(), closed.stderr
        covered = lambda address: any(int(b, 16) <= address < int(e, 16) for b, e in (l.split()[1:] for l in closed_path.read_text().splitlines() if l.startswith("range ")))
        assert covered(V_STUB) and covered(H_STUB) and covered(V_STUB + 5) and covered(0x200)
        assert not covered(0x0) and not covered(V_HANDLER) and not covered(H_HANDLER_A), "a live operand contributes no fixed successor"
        selective = subprocess.run(base + ["--immutable-rom-aot-admission", str(closed_path)], text=True, capture_output=True, timeout=240)
        assert selective.returncode == 0, selective.stderr
        assert has_entry(selective.stdout, V_STUB) and has_entry(selective.stdout, H_STUB)
        admission_run = compile_and_run(root, compiler, work, "selective_bin", selective.stdout, SELECTIVE_HARNESS)
        assert admission_run.returncode == 0, (admission_run.stdout, admission_run.stderr)

        # G: ambiguity and missing evidence never yield an entry.
        negatives = {
            "conflicting_writer": stores() + [0x31FC, 0x4E71, 0xFFF0] + KEEP_ON_C4,        # a second constant store over the opcode word
            "conflicting_long": stores() + [0x21FC, 0x4E71, 0x4E71, 0xFFF0] + KEEP_ON_C4,   # a long store overlapping the opcode word
            "different_opcode": stores(opcode_v=0x4EB9, opcode_h=0x4EB9) + KEEP_ON_C4,       # JSR is not a supported form
            "no_operand_constructor": stores(operand_v=False, operand_h=False) + KEEP_ON_C4,
            "conflicting_movem": stores() + [0x48F9, 0x0007, 0x00FF, 0xFFE8] + KEEP_ON_C4,     # MOVEM.L D0-D2,($FFFFE8).L covers $FFFFF0-$FFFFF3 only
            "opcode_only_clr": [0x4278, 0xFFF0, 0x4278, 0xFFF6] + KEEP_ON_C4,                 # CLR.W over the stub: not the opcode
        }
        for name, code in negatives.items():
            result = emit(cli, work, f"neg_{name}", image(code))
            assert result.returncode == 0, (name, result.stderr)
            # The conflicting writers name only the V stub; the independently evidenced H stub is unaffected.
            assert not has_entry(result.stdout, V_STUB), name
            assert has_entry(result.stdout, H_STUB) == name.startswith("conflicting"), name
        # Unreachable construction (a false decode in data never executed) is not evidence.
        data_only = image([0x60FE], extra={0x1800: [x for x in stores()]})
        result = emit(cli, work, "neg_unreachable", data_only)
        assert not has_entry(result.stdout, V_STUB) and not has_entry(result.stdout, H_STUB)
        # A register-indirect writer alone is not evidence of any form.
        result = emit(cli, work, "neg_indirect", image([0x207C, 0x00FF, 0xFFF0, 0x30BC, 0x4EF9] + KEEP_ON_C4))
        assert not has_entry(result.stdout, V_STUB) and not has_entry(result.stdout, H_STUB)
        # An unrelated RAM address (a store of the opcode that no immutable vector points at) acquires no entry.
        unrelated = image([0x31FC, 0x4EF9, 0xFF00, 0x21FC, 0x0000, 0x1000, 0xFF02] + KEEP_ON_C4, vectors=False)
        result = emit(cli, work, "neg_unrelated", unrelated)
        assert not has_entry(result.stdout, 0x00FFFF00) and not has_entry(result.stdout, V_STUB)

        # H: materialized thunks. The exact-byte descriptor over a static address is subsumed; another form conflicts; an unrelated one works.
        same = emit(cli, work, "legacy_same", good, "--ram-thunk", f"00FFFFF0:4EF900001000")
        assert same.returncode == 0 and same.stdout == first.stdout, "a subsumed materialization changes nothing"
        differing = emit(cli, work, "legacy_other_form", good, "--ram-thunk", "00FFFFF0:4EF81000")
        assert differing.returncode != 0, "a different form over a static thunk is refused"
        # (the same descriptor over an address with no static thunk is a valid legacy thunk, so the refusal is the precedence rule)
        legacy_word = emit(cli, work, "legacy_word_form", image(spin([0x31FC, 0x4EF8, 0xFF00] + [0x4E71])), "--ram-thunk", "00FF0100:4EF81000")
        assert legacy_word.returncode == 0, legacy_word.stderr
        unrelated_thunk = emit(cli, work, "legacy_other", good, "--ram-thunk", "00FF0100:4EF900001000")
        assert unrelated_thunk.returncode == 0 and has_entry(unrelated_thunk.stdout, 0x00FF0100) and has_entry(unrelated_thunk.stdout, V_STUB)
    print("genesis_static_ram_jmp_thunk_generated_test: OK")


if __name__ == "__main__":
    main()
