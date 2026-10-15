#!/usr/bin/env python3
"""SEG-059: generated-native synthetic evidence for the Genesis compatibility repairs.

Project-authored programs only. Every scenario is compiled by the production AOT frontend to strict C11 and executed; the
architectural result (D0-D7, A0-A7, CCR, selected RAM bytes) is checked against hand-derived expected values that never come
from the generated code, and additionally against the pinned Musashi MC68000 core when
SEGARECOMP_M68K_COMPAT_REPAIRS_MUSASHI_CHECKOUT (or SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT) points at the pinned checkout.

Covered:
  1. Absolute-long ownership: the decoder keeps the raw 32-bit operand, so LEA/PEA preserve $FFFF8000 architecturally while
     DATA accesses (BTST/MOVE source and destination) reach the 24-bit bus location $FF8xxx. A non-sign-extended high byte
     and JMP/JSR to an absolute-long control target stay fail-closed, failure-atomically.
  2. In-place return-address rewriting (`MOVE.L <src>,(A7)` ... RTS): inline-data skipping, the unchanged `-(A7)` form, ordinary
     JSR/RTS, and the fail-closed cases (uncompiled target, odd target, a non-seed write that must not authorize the RTS).
  3. IRQ6 delivered to an immutable-ROM AOT `MOVE.L #abs,-(A7); RTS` trampoline through the CPU-owned exception entry (frame SR/PC,
     RTE return) plus the fail-closed negatives.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
STRICT = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
RAM = 0x00FF0000
DEFAULT_REGS = {"a7": 0x00FF0100, "sr": 0x2700}


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stderr[-2000:])
    return result


def sc(base, code, stop, regs=None, ram=(), observe=((0xFF00F0, 0x20),), name="", start=None):
    """A scenario: code hex mapped at `base`, run from `base` until PC == `stop`."""
    r = dict(DEFAULT_REGS)
    r.update(regs or {})
    return {"name": name, "base": base, "start": base if start is None else start, "code": code, "stop": stop, "regs": r, "ram": list(ram), "observe": list(observe)}


# ---- scenarios ------------------------------------------------------------------------------------------------------------
ABS_LONG = sc(
    0xB00,
    "41F9FFFF8000"        # B00 LEA ($FFFF8000).L,A0       -> A0 = $FFFF8000 (architectural 32-bit value)
    "4879FFFF8000"        # B06 PEA ($FFFF8000).L           -> pushes $FFFF8000
    "08390000FFFF8001"    # B0C BTST #0,($FFFF8001).L       -> reads RAM $FF8001
    "33C1FFFF8002"        # B14 MOVE.W D1,($FFFF8002).L     -> writes RAM $FF8002
    "3439FFFF8004"        # B1A MOVE.W ($FFFF8004).L,D2     -> reads RAM $FF8004
    "4E70",               # B20 RESET
    0xB20,
    regs={"d1": 0x1234ABCD, "d2": 0x11110000},
    ram=[(0xFF8001, "01"), (0xFF8004, "8001")],
    observe=[(0xFF00F0, 0x10), (0xFF8000, 8)],
    name="absolute-long LEA/PEA/BTST/MOVE",
)
ABS_LONG_EXPECT = {
    "a0": 0xFFFF8000, "a7": 0x00FF00FC, "d2": 0x11118001, "ccr": 0x08,   # MOVE.W ($FF8004),D2 -> N set
    "ram": {0xFF00FC: "FFFF8000", 0xFF8002: "ABCD"},
}

# Inline data after the call. The routine is entered through a runtime-resolved `JSR (A0)`, so it is an AOT-only identity (the
# static graph never reaches its RTS): the RTS authority under test is exactly the push-then-RTS computed-jump proof. The real
# `JSR $B12` prefix (a statically-called RTS) is the "genuine partial program" shape that makes RTS identities admissible.
# B00 JSR $B12 ; B06 MOVEA.W (A1),A0 ; B08 JSR (A0) [run starts here] ; B0A inline data (never executed) ; B0E MOVEQ #7,D1 ;
# B10 RESET ; B12 RTS (static callee) ; B14 routine
INLINE_HEAD = "4EB900000B12" "3051" "4E90" "4E704E70" "7207" "4E70" "4E75"
ENTRY = {"a0": 0xB14}
START = 0xB08


def routine(code, stop, regs=None, **kw):
    return sc(0xB00, INLINE_HEAD + code, stop, regs={**ENTRY, **(regs or {})}, start=START, **kw)


REWRITE_IMM = routine("2EBC00000B0E" "4E75", 0xB10, name="MOVE.L #imm,(A7) ; RTS")
REWRITE_REG = routine("2E89" "4E75", 0xB10, regs={"a1": 0xB0E}, name="MOVE.L A1,(A7) ; RTS")
PUSH_FORM = routine("2F3C00000B0E" "4E75", 0xB10, name="MOVE.L #imm,-(A7) ; RTS (previous form)")
PLAIN_RTS = sc(0xB00, "4EB900000B12" "3051" "4E90" "7203" "4E70" "4E71" "4E75" "4E75", 0xB0C,
               regs={"a0": 0xB10}, start=START, name="ordinary JSR/RTS")
# fail-closed RTS cases (generated only; Musashi has no compiled-entry set); the second element is the RTS address
UNCOMPILED = (routine("2E89" "4E75", None, regs={"a1": 0x00000C00}, name="rewrite to uncompiled target"), 0xB16)
ODD_TARGET = (routine("2E89" "4E75", None, regs={"a1": 0x00000B0F}, name="rewrite to odd target"), 0xB16)
NON_SEED_WRITE = (routine("3F410002" "4E75", None, regs={"d1": 0x0B0E},
                          name="MOVE.W D1,2(A7) is not a seed; RTS keeps the continuation authority"), 0xB18)
WINDOW_ENDED = (routine("2E89" "588F" "4E75", None, regs={"a1": 0xB0E}, ram=[(0xFF0100, "00000B0E")],
                        name="ADDQ.L #4,A7 ends the proof window; the RTS is an ordinary one"), 0xB18)


def musashi_dir():
    for key in ("SEGARECOMP_M68K_COMPAT_REPAIRS_MUSASHI_CHECKOUT", "SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT"):
        if os.environ.get(key):
            return pathlib.Path(os.environ[key])
    return None


def c_hex_bytes(code):
    raw = bytes.fromhex(code)
    return ", ".join(f"0x{b:02x}" for b in raw), len(raw)


# ---- generated-native side ---------------------------------------------------------------------------------------------
def generated_harness(scn, max_steps=64):
    lines = ["#include <stdint.h>", "#include <stdio.h>", '#include "runtime.h"', '#include "generated.c"',
             "int main(void) {", "  GenesisRuntime r = {0}; GenesisControlTransfer t = {0}; int step; unsigned i;",
             f"  r.pc = UINT32_C(0x{scn['start']:08X});"]
    for k, v in scn["regs"].items():
        if k == "sr":
            lines.append(f"  r.sr = UINT32_C(0x{v:04X});")
        else:
            lines.append(f"  r.{k[0]}[{k[1]}] = UINT32_C(0x{v:08X});")
    for addr, hexbytes in scn["ram"]:
        for i, b in enumerate(bytes.fromhex(hexbytes)):
            lines.append(f"  r.work_ram[0x{addr + i - RAM:X}] = 0x{b:02X};")
    stop = "UINT32_C(0xFFFFFFFF)" if scn["stop"] is None else f"UINT32_C(0x{scn['stop']:08X})"
    lines += [f"  for (step = 0; step < {max_steps} && r.pc != {stop}; ++step) {{",
              "    t = genesis_bridge_dispatch(&r);",
              "    if (t.kind == GENESIS_CONTINUE_AT_PC) r.pc = t.next_pc; else break;",
              "  }",
              '  printf("stop=%d pc=%08X\\n", t.kind == GENESIS_STOP, (unsigned)r.pc);',
              '  for (i = 0; i < 8; ++i) printf("d%u=%08X\\n", i, (unsigned)r.d[i]);',
              '  for (i = 0; i < 8; ++i) printf("a%u=%08X\\n", i, (unsigned)r.a[i]);',
              '  printf("ccr=%02X\\n", (unsigned)(r.sr & 0x1F));']
    for addr, length in scn["observe"]:
        lines.append(f'  printf("ram_{addr:X}="); for (i = 0; i < {length}; ++i) printf("%02X", r.work_ram[0x{addr - RAM:X} + i]); printf("\\n");')
    lines += ["  return 0;", "}"]
    return "\n".join(lines) + "\n"


def run_generated(emitter, compiler, root, scn, temp, tag):
    emitted = checked([emitter, "--emit-synthetic-aot-source", f"{scn['base']:08X}", scn["code"]]).stdout
    again = checked([emitter, "--emit-synthetic-aot-source", f"{scn['base']:08X}", scn["code"]]).stdout
    assert emitted == again, "generated C must be deterministic"
    directory = pathlib.Path(temp) / tag
    directory.mkdir()
    (directory / "generated.c").write_text(emitted)
    (directory / "harness.c").write_text(generated_harness(scn))
    runtime = pathlib.Path(root) / "platforms/genesis/runtime"
    exe = directory / "run"
    checked([compiler, *STRICT, "-I", str(runtime), "-I", str(directory), str(directory / "harness.c"),
             str(runtime / "runtime.c"), "-o", str(exe)])
    out = checked([str(exe)]).stdout
    return emitted, out


def state_lines(text):
    rows = {}
    for line in text.splitlines():
        if line.startswith("stop="):
            m = re.match(r"stop=(\d) pc=([0-9A-F]+)", line)
            rows["stopped"], rows["pc"] = m.group(1) == "1", int(m.group(2), 16)
        else:
            k, _, v = line.partition("=")
            rows[k] = v
    return rows


# ---- Musashi side -----------------------------------------------------------------------------------------------------
ORACLE_TEMPLATE = r'''#include <stdint.h>
#include <stdio.h>
#include "m68k.h"
static const uint8_t code[] = {%(code)s};
static uint8_t ram[UINT32_C(0x10000)];
#define RAM_BEGIN UINT32_C(0x00FF0000)
#define CODE_BEGIN UINT32_C(0x%(base)08X)
static unsigned int read8(unsigned int a) { if (a >= CODE_BEGIN && a < CODE_BEGIN + sizeof(code)) return code[a-CODE_BEGIN]; if (a >= RAM_BEGIN && a < RAM_BEGIN + UINT32_C(0x10000)) return ram[a-RAM_BEGIN]; return 0U; }
unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return (read8(a)<<8U)|read8(a+1U); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a)<<16U)|m68k_read_memory_16(a+2U); }
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a,unsigned int v) { if(a>=RAM_BEGIN&&a<RAM_BEGIN+UINT32_C(0x10000)) ram[a-RAM_BEGIN]=(uint8_t)v; }
void m68k_write_memory_16(unsigned int a,unsigned int v) { m68k_write_memory_8(a,v>>8U);m68k_write_memory_8(a+1U,v); }
void m68k_write_memory_32(unsigned int a,unsigned int v) { m68k_write_memory_16(a,v>>16U);m68k_write_memory_16(a+2U,v); }
void m68k_write_memory_32_pd(unsigned int a,unsigned int v) { m68k_write_memory_32(a,v); }
int main(void) {
  int step; unsigned i;
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
%(setup)s
  m68k_set_reg(M68K_REG_PC, UINT32_C(0x%(start)08X));
  for (step = 0; step < 64 && m68k_get_reg(NULL, M68K_REG_PC) != UINT32_C(0x%(stop)08X); ++step) (void)m68k_execute(1);
  printf("stop=0 pc=%%08X\n", m68k_get_reg(NULL, M68K_REG_PC));
%(print)s
  return 0;
}
'''


def oracle_source(scn):
    code, _ = c_hex_bytes(scn["code"])
    setup = []
    for k, v in scn["regs"].items():
        if k == "sr":
            setup.append(f"  m68k_set_reg(M68K_REG_SR, UINT32_C(0x{v:04X}));")
        else:
            setup.append(f"  m68k_set_reg(M68K_REG_{k.upper()}, UINT32_C(0x{v:08X}));")
    for addr, hexbytes in scn["ram"]:
        for i, b in enumerate(bytes.fromhex(hexbytes)):
            setup.append(f"  ram[0x{addr + i - RAM:X}] = 0x{b:02X};")
    printing = []
    for i in range(8):
        printing.append(f'  printf("d{i}=%08X\\n", m68k_get_reg(NULL, M68K_REG_D{i}));')
    for i in range(8):
        printing.append(f'  printf("a{i}=%08X\\n", m68k_get_reg(NULL, M68K_REG_A{i}));')
    printing.append('  printf("ccr=%02X\\n", m68k_get_reg(NULL, M68K_REG_SR) & 0x1F);')
    for addr, length in scn["observe"]:
        printing.append(f'  printf("ram_{addr:X}="); for (i = 0; i < {length}; ++i) printf("%02X", ram[0x{addr - RAM:X} + i]); printf("\\n");')
    return ORACLE_TEMPLATE % {"code": code, "base": scn["base"], "start": scn["start"], "stop": scn["stop"], "setup": "\n".join(setup),
                              "print": "\n".join(printing)}


def build_oracle(compiler, checkout, temp):
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
    assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0
    work = pathlib.Path(temp) / "musashi"
    work.mkdir()
    env = os.environ.copy()
    if not env.get("SDKROOT") and shutil.which("xcrun"):
        env["SDKROOT"] = checked(["xcrun", "--show-sdk-path"]).stdout.strip()
    generator = work / "m68kmake"
    checked([compiler, "-std=gnu11", "-w", str(checkout / "m68kmake.c"), "-o", str(generator)], env=env)
    shutil.copyfile(checkout / "m68k_in.c", work / "m68k_in.c")
    checked([str(generator)], cwd=work, env=env)
    return work, env


def run_oracle(compiler, checkout, work, env, scn, tag):
    source = work / f"{tag}.c"
    source.write_text(oracle_source(scn))
    exe = work / tag
    checked([compiler, "-std=gnu11", "-w", "-I", str(work),
             "-I", str(checkout), "-I", str(checkout / "softfloat"), str(source), str(checkout / "m68kcpu.c"),
             str(work / "m68kops.c"), str(checkout / "softfloat/softfloat.c"), "-o", str(exe), "-lm"], env=env)
    return checked([str(exe)]).stdout


def main():
    emitter, compiler, root = sys.argv[1:]
    checkout = musashi_dir()
    with tempfile.TemporaryDirectory() as temp:
        work = env = None
        if checkout is not None:
            work, env = build_oracle(compiler, checkout, temp)

        def differential(scn, tag, expect):
            emitted, out = run_generated(emitter, compiler, root, scn, temp, tag)
            rows = state_lines(out)
            assert rows["pc"] == scn["stop"], (scn["name"], out)
            for key, value in expect.items():
                if key == "ram":
                    for addr, hexbytes in value.items():
                        for obs_addr, length in scn["observe"]:
                            if obs_addr <= addr < obs_addr + length:
                                offset = (addr - obs_addr) * 2
                                assert rows[f"ram_{obs_addr:X}"][offset:offset + len(hexbytes)] == hexbytes, (scn["name"], addr, rows)
                                break
                        else:
                            raise AssertionError(f"{scn['name']}: address {addr:X} is not observed")
                elif key == "ccr":
                    assert int(rows["ccr"], 16) == value, (scn["name"], rows["ccr"])
                else:
                    assert int(rows[key], 16) == value, (scn["name"], key, rows[key], f"{value:08X}")
            if checkout is not None:
                oracle = state_lines(run_oracle(compiler, checkout, work, env, scn, tag))
                # the generated program ends in its typed frontier stop at the RESET; Musashi simply stops there
                assert {k: v for k, v in oracle.items() if k != "stopped"} == {k: v for k, v in rows.items() if k != "stopped"}, \
                    (scn["name"], oracle, rows)
            return emitted

        # 1. absolute-long ownership
        emitted = differential(ABS_LONG, "abs-long", ABS_LONG_EXPECT)
        assert "runtime->a[0] = UINT32_C(0xFFFF8000);" in emitted, "LEA must keep the architectural 32-bit value"
        assert "const uint32_t pea_address = UINT32_C(0xFFFF8000);" in emitted, "PEA must push the architectural 32-bit value"
        assert "UINT32_C(0x00FF8001)) & UINT32_C(0x00FFFFFF)" in emitted, "BTST reaches the 24-bit bus address"

        # 2. in-place return rewrite and its neighbours (JSR (A0) pushes the continuation $B0A; the data is skipped to $B0E)
        rewrite_expect = {"d1": 7, "a7": 0x00FF0100, "ram": {0xFF00FC: "00000B0E"}}
        differential(REWRITE_IMM, "rewrite-imm", rewrite_expect)
        differential(REWRITE_REG, "rewrite-reg", rewrite_expect)
        differential(PUSH_FORM, "push-form", {"d1": 7, "a7": 0x00FF00FC, "ram": {0xFF00FC: "00000B0A", 0xFF00F8: "00000B0E"}})
        differential(PLAIN_RTS, "plain-rts", {"d1": 3, "a7": 0x00FF0100})

        # fail-closed cases: typed stop AT the RTS, failure-atomic (RTS not committed: A7 unchanged, nothing after it ran)
        for (scn, rts_pc), tag, a7, slot in ((UNCOMPILED, "uncompiled", 0xFF00FC, "00000C00"),
                                             (ODD_TARGET, "odd-target", 0xFF00FC, "00000B0F"),
                                             (NON_SEED_WRITE, "non-seed", 0xFF00FC, "00000B0E"),
                                             (WINDOW_ENDED, "window-ended", 0xFF0100, "00000B0E")):
            _, out = run_generated(emitter, compiler, root, scn, temp, tag)
            rows = state_lines(out)
            assert rows["stopped"] and rows["pc"] == rts_pc, (scn["name"], "must stop at the RTS", out)
            assert int(rows["a7"], 16) == a7, (scn["name"], out)
            offset = (a7 - 0xFF00F0) * 2
            assert rows["ram_FF00F0"][offset:offset + 8] == slot, (scn["name"], "the slot the RTS refused to pop", out)
            assert int(rows["d1"], 16) in (0, 0x0B0E), (scn["name"], "nothing after the refused RTS ran", out)

        # 3. absolute-long fail-closed shapes keep their typed whole-program rejection: a non-sign-extended high byte on a data
        #    operand, and JMP/JSR control targets (a separately pinned policy that the data-access canonicalization never reaches)
        for tag, code in {"high-byte-data": "7001" "3039" "01FF8000" "4E70",         # MOVE.W ($01FF8000).L,D0
                          "jmp-sign-extended": "7001" "4EF9FFFF8000" "4E70",         # JMP ($FFFF8000).L
                          "jsr-sign-extended": "7001" "4EB9FFFF8000" "4E70"}.items():  # JSR ($FFFF8000).L
            refused = subprocess.run([emitter, "--emit-synthetic-aot-source", "00000B00", code], text=True, capture_output=True)
            assert refused.returncode != 0 and refused.stdout == "", (tag, "must not produce a program", refused.stderr)
            assert "rejected: effective_address_not_24bit" in refused.stderr, (tag, refused.stderr)
        # the same data access spelled with the sign-extended top-half address is accepted, so the rejection above is the
        # policy and not an artifact of the fixture
        accepted = subprocess.run([emitter, "--emit-synthetic-aot-source", "00000B00", "7001" "3039" "FFFF8000" "4E70"],
                                  text=True, capture_output=True)
        assert accepted.returncode == 0 and "UINT32_C(0x00FF8000)) & UINT32_C(0x00FFFFFF)" in accepted.stdout

        # 4. IRQ6 -> immutable-ROM AOT trampoline
        irq6(emitter, compiler, root, temp, checkout, work, env)
    print("genesis_compat_repairs_generated_differential_test: OK" + (" (Musashi compared)" if checkout else " (no Musashi checkout)"))


# ---- IRQ6 --------------------------------------------------------------------------------------------------------------
# $200 JSR $210 (a real static call: the RTS identities need a static return frame) ; $206 MOVE.W #$1000,D0 ; $20A DBRA D0,* ;
# $20E RESET ; $210 RTS
IRQ_MAIN = "4EB900000210" "303C1000" "51C8FFFE" "4E70" "4E75"
# trampoline: $240 MOVE.L #$260,-(A7) ; RTS ; handler body $260: MOVE.W SR,D4 ; MOVE.W (A7),D6 ; MOVE.L 2(A7),D5 ; MOVEQ #$55,D7 ; RTE
IRQ_TRAMPOLINE = "2F3C" "00000260" "4E75"
IRQ_BODY = "40C4" "3C17" "2A2F0002" "7E55" "4E73"


def irq_image(main, trampoline, body):
    code = bytearray(0x80)
    for off, h in ((0, main), (0x40, trampoline), (0x60, body)):
        b = bytes.fromhex(h)
        code[off:off + len(b)] = b
    return code.hex()


def instrumented(source, seeded, anchor="runtime.irq6_handler_present = 1; "):
    if seeded:
        assert anchor in source
        source = source.replace(anchor, anchor + "runtime.sr = 0x2000; runtime.devices.vdp.registers[1] = 0x0020; "
                                "runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28U; ", 1)
    call = re.search(r"result = genesis_runtime_run\([^;]*\);", source)
    assert call is not None
    probe = (' fprintf(stderr, "d4=%08X d5=%08X d6=%08X d7=%08X a7=%08X sr=%04X\\n", (unsigned)runtime.d[4], (unsigned)runtime.d[5], '
             '(unsigned)runtime.d[6], (unsigned)runtime.d[7], (unsigned)runtime.a[7], (unsigned)runtime.sr);')
    return source[:call.end()] + probe + source[call.end():]


def run_bridge(compiler, root, source, temp, tag):
    directory = pathlib.Path(temp) / tag
    directory.mkdir()
    (directory / "generated.c").write_text(source)
    runtime = pathlib.Path(root) / "platforms/genesis/runtime"
    exe = directory / "run"
    checked([compiler, *STRICT, "-I", str(runtime), str(directory / "generated.c"), str(runtime / "runtime.c"), "-o", str(exe)])
    first = subprocess.run([str(exe), "--instruction-budget", "40000"], text=True, capture_output=True, timeout=120)
    second = subprocess.run([str(exe), "--instruction-budget", "40000"], text=True, capture_output=True, timeout=120)
    assert (first.stdout, first.stderr) == (second.stdout, second.stderr), "deterministic"
    return first


def irq6(emitter, compiler, root, temp, checkout, work, env):
    image = irq_image(IRQ_MAIN, IRQ_TRAMPOLINE, IRQ_BODY)
    bridge = checked([emitter, "--emit-synthetic-irq6-aot-bridge", "00000240", image]).stdout
    assert bridge == checked([emitter, "--emit-synthetic-irq6-aot-bridge", "00000240", image]).stdout
    assert "runtime.irq6_handler_entry = UINT32_C(0x00000240);" in bridge and "runtime.irq6_handler_present = 1;" in bridge
    assert "genesis_compiled_entry_lookup" in bridge
    # the trampoline entry exists in the final emitted compiled-entry table
    table = bridge.split("genesis_compiled_entry_addresses[] = {", 1)[1].split("};", 1)[0]
    assert "UINT32_C(0x00000240)" in table and "UINT32_C(0x00000260)" in table

    unseeded = run_bridge(compiler, root, instrumented(bridge, False), temp, "irq6-unseeded")
    assert "d7=00000000" in unseeded.stderr, ("no admission, no handler", unseeded.stderr)
    seeded = run_bridge(compiler, root, instrumented(bridge, True), temp, "irq6-seeded")
    m = re.search(r"d4=([0-9A-F]+) d5=([0-9A-F]+) d6=([0-9A-F]+) d7=([0-9A-F]+) a7=([0-9A-F]+) sr=([0-9A-F]+)", seeded.stderr)
    assert m is not None, seeded.stderr
    d4, d5, d6, d7, a7, sr = (int(g, 16) for g in m.groups())
    assert d7 == 0x55, ("the trampoline and handler body executed", seeded.stderr)
    assert d4 == 0x2600, "handler runs in supervisor mode with the level-6 interrupt mask"
    assert d6 == 0x2000, "the exception frame holds the pre-interrupt SR"
    assert d5 == 0x210, ("the exception frame holds the PC of the instruction boundary after the JSR (the callee RTS)", hex(d5))
    assert a7 == 0x00FF8000 and sr == 0x2000 | (sr & 0x1F), ("RTE pops the frame and restores SP", seeded.stderr)
    assert '"family":"reset"' in seeded.stdout, "the mainline resumed after RTE and reached its RESET terminal"
    if checkout is not None:
        irq6_oracle(compiler, checkout, work, env, image)

    # negatives: the handler is not (or cannot be) a compiled cartridge entry => never reported deliverable, never silent
    for tag, handler in (("unmapped", "00300000"), ("odd", "00000241")):
        res = subprocess.run([emitter, "--emit-synthetic-irq6-aot-bridge", handler, image], text=True, capture_output=True)
        assert res.returncode != 0 and "irq6_handler_present" not in res.stdout, (tag, "whole build rejected fail-closed")
    ram = checked([emitter, "--emit-synthetic-irq6-aot-bridge", "00FF0100", image]).stdout
    assert "runtime.irq6_vector_in_work_ram = 1;" in ram and "irq6_handler_present = 1" not in ram, \
        "an unguarded work-RAM handler keeps the typed interrupt stop"
    stopped = run_bridge(compiler, root, instrumented(ram, True, "runtime.irq6_vector_in_work_ram = 1; "), temp, "irq6-ram")
    assert "d7=00000000" in stopped.stderr and '"stop_class":"' in stopped.stdout and '"result":"stop"' in stopped.stdout, \
        ("an undeliverable IRQ6 is a typed stop, not a silent drop", stopped.stdout)


def irq6_oracle(compiler, checkout, work, env, image):
    """Pinned-Musashi exception-entry frame for the same handler body (level-6 autovector, mask 0)."""
    code = bytes.fromhex(image)
    rom = bytearray(0x200) + code
    rom[0:4] = (0x00FF8000).to_bytes(4, "big")
    rom[4:8] = (0x200).to_bytes(4, "big")
    rom[0x78:0x7C] = (0x240).to_bytes(4, "big")
    source = ORACLE_TEMPLATE % {
        "code": ", ".join(f"0x{b:02x}" for b in rom), "base": 0, "start": 0x200, "stop": 0x20E,
        "setup": "  m68k_set_reg(M68K_REG_SR, UINT32_C(0x2000)); m68k_set_reg(M68K_REG_A7, UINT32_C(0x00FF8000));",
        "print": ""}
    # run: (the first m68k_execute only eats the reset cycles) step the JSR (PC $210), raise IRQ6, then execute until the handler's RTE has run (D7 set).
    source = source.replace(
        "  for (step = 0;",
        "  (void)m68k_execute(1); (void)m68k_execute(1); m68k_set_irq(6); (void)m68k_execute(1); m68k_set_irq(0);\n"
        "  for (step = 0; step < 16 && m68k_get_reg(NULL, M68K_REG_D7) == 0U; ++step) (void)m68k_execute(1);\n"
        '  printf("d4=%04X d5=%08X d6=%04X d7=%02X\\n", m68k_get_reg(NULL, M68K_REG_D4) & 0xFFFF, m68k_get_reg(NULL, M68K_REG_D5), m68k_get_reg(NULL, M68K_REG_D6) & 0xFFFF, m68k_get_reg(NULL, M68K_REG_D7));\n'
        "  for (step = 0;", 1)
    path = work / "irq6_oracle.c"
    path.write_text(source)
    exe = work / "irq6_oracle"
    checked([compiler, "-std=gnu11", "-w", "-I", str(work),
             "-I", str(checkout), "-I", str(checkout / "softfloat"), str(path), str(checkout / "m68kcpu.c"),
             str(work / "m68kops.c"), str(checkout / "softfloat/softfloat.c"), "-o", str(exe), "-lm"], env=env)
    line = checked([str(exe)]).stdout.splitlines()[0]
    assert line == "d4=2600 d5=00000210 d6=2000 d7=55", ("Musashi exception frame differs from the documented expectation", line)


if __name__ == "__main__":
    main()
