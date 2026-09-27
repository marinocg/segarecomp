#!/usr/bin/env python3
"""SEG-021-T020 / ADR 0043 §3, §7: the machine-independent MC68000 interrupt acceptance contract
(``libs/cpu/m68k/include/segarecomp/cpu/m68k/exception_core.h``) against documented expected values and, when the
pinned checkout is configured, the pinned Musashi core.

A project-authored synthetic flat machine (64 KiB, vector table at 0 with every vector pointing at a distinct
handler, NOP code elsewhere) is scripted identically on both sides. Each step optionally rewrites SR (as an
instruction that lowers the mask would), sets the interrupt request level and the acknowledge answer, then crosses
one instruction boundary: the contract samples the level, applies the acceptance rule, maps the acknowledge to a
vector and enters through the shared six-byte entry; then one instruction executes (the scaffold models only the
two words the scripts place in memory: NOP and STOP #imm). Musashi performs the same step with ``m68k_set_irq`` /
its acknowledge callback / ``m68k_execute(1)`` (built with ``M68K_EMULATE_INT_ACK`` on, so a request stays asserted
until the script changes it, as the machine-owned request lines do in this project). Compared per step: PC, SR, A7,
USP and the six bytes at A7.

Covered: levels 1-6 against every mask 0-7; the level-7 lower->7 transition at every mask; level 7 held at mask 7
(not re-taken), then recognized by the comparator once the mask is lowered; a held level 7 that never transitions
again; 7 -> 3 -> 7 re-transition at mask 7; autovector, supplied, spurious and uninitialized acknowledges; user-mode
entry (frame on the SSP); T cleared on entry; STOP woken by an accepted level versus staying stopped on masked
levels, including a level-7 transition at mask 7. The refused supplied vectors (outside 64-255) are checked against
documented expected values only (Musashi would take them).

usage: m68k_interrupt_acceptance_test.py <cc>
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER_DIR = ROOT / "libs" / "cpu" / "m68k" / "include" / "segarecomp" / "cpu" / "m68k"
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENVS = ("SEGARECOMP_M68K_CONFORMANCE_MUSASHI_CHECKOUT", "SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT")
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror"]

AUTO, SUPPLIED, SPURIOUS, UNINIT = 0, 1, 2, 3
KEEP = -1


def scenarios():
    """(name, initial SR, STOP immediate or None, [(new SR or KEEP, level or KEEP, ack, supplied), ...])."""
    out = []
    for mask in range(8):
        for level in range(1, 7):
            out.append(("L%d_M%d" % (level, mask), 0x2000 | mask << 8, None, [(KEEP, level, AUTO, 0)]))
        out.append(("L7_edge_M%d" % mask, 0x2000 | mask << 8, None, [(KEEP, 7, AUTO, 0)]))
    # Level 7 held: taken once on the transition (mask becomes 7), not re-taken while held at mask 7, recognized by
    # the comparator when the mask is lowered from 7 while level 7 stays asserted, then held again.
    out.append(("L7_held", 0x2000, None, [(KEEP, 7, AUTO, 0), (KEEP, 7, AUTO, 0), (KEEP, 7, AUTO, 0),
                                          (0x2600, 7, AUTO, 0), (KEEP, 7, AUTO, 0), (0x2700, 7, AUTO, 0)]))
    # A level 7 asserted at mask 7 from the start is one transition (taken); it then never transitions again.
    out.append(("L7_never_again", 0x2700, None, [(KEEP, 7, AUTO, 0)] + [(KEEP, 7, AUTO, 0)] * 3))
    # 7 -> 3 -> 7 at mask 7: the second rise is a new transition.
    out.append(("L7_retransition", 0x2700, None, [(KEEP, 7, AUTO, 0), (KEEP, 3, AUTO, 0), (KEEP, 7, AUTO, 0)]))
    # Acknowledge answers (level 5, mask 0).
    out.append(("ack_auto", 0x2000, None, [(KEEP, 5, AUTO, 0)]))
    out.append(("ack_vec64", 0x2000, None, [(KEEP, 5, SUPPLIED, 64)]))
    out.append(("ack_vec255", 0x2000, None, [(KEEP, 5, SUPPLIED, 255)]))
    out.append(("ack_spurious", 0x2000, None, [(KEEP, 5, SPURIOUS, 0)]))
    out.append(("ack_uninit", 0x2000, None, [(KEEP, 5, UNINIT, 0)]))
    # User mode: the frame goes on the SSP; the USP is kept. T is cleared on entry.
    out.append(("user_mode", 0x0015, None, [(KEEP, 1, AUTO, 0)]))
    out.append(("trace_cleared", 0xA31F, None, [(KEEP, 4, AUTO, 0)]))
    # STOP #$2300: levels 2 and 3 are masked (stays stopped), level 4 wakes it (instruction after STOP stacked).
    out.append(("stop_wake", 0x2700, 0x2300, [(KEEP, 0, AUTO, 0), (KEEP, 2, AUTO, 0), (KEEP, 3, AUTO, 0),
                                              (KEEP, 4, AUTO, 0), (KEEP, 0, AUTO, 0)]))
    # STOP #$2700: level 6 never wakes it; a level-7 transition does.
    out.append(("stop_nmi", 0x2000, 0x2700, [(KEEP, 0, AUTO, 0), (KEEP, 6, AUTO, 0), (KEEP, 6, AUTO, 0),
                                             (KEEP, 7, AUTO, 0)]))
    return out


def c_table():
    rows, steps = [], []
    for name, sr, stop, script in scenarios():
        rows.append('  {"%s", 0x%04XU, %d, 0x%04XU, %d, %d},' % (name, sr, 1 if stop is not None else 0,
                                                               stop or 0, len(steps), len(script)))
        for new_sr, level, ack, supplied in script:
            steps.append("  {%d, %d, %d, %d}," % (new_sr, level, ack, supplied))
    return ("typedef struct { const char *name; unsigned sr; int has_stop; unsigned stop_imm; int first, count; } Scenario;\n"
            "typedef struct { int new_sr, level, ack, supplied; } Step;\n"
            "static const Scenario scenarios[] = {\n" + "\n".join(rows) + "\n};\n"
            "static const Step steps[] = {\n" + "\n".join(steps) + "\n};\n"
            "#define SCENARIO_COUNT (sizeof scenarios / sizeof scenarios[0])\n")


COMMON = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MEM 0x10000U
#define CODE 0x400U
#define SSP0 0x8000U
#define USP0 0x6000U
#define HANDLER(v) (0x3000U + (unsigned)(v) * 0x10U)
static uint8_t mem[MEM];
static void build_memory(const Scenario *s) {
  unsigned i;
  for (i = 0; i < MEM; i += 2U) { mem[i] = 0x4E; mem[i + 1U] = 0x71; }  /* NOP everywhere */
  for (i = 0; i < 256U; ++i) {
    const unsigned h = i == 0U ? SSP0 : i == 1U ? CODE : HANDLER(i);
    mem[i * 4U] = (uint8_t)(h >> 24); mem[i * 4U + 1U] = (uint8_t)(h >> 16);
    mem[i * 4U + 2U] = (uint8_t)(h >> 8); mem[i * 4U + 3U] = (uint8_t)h;
  }
  if (s->has_stop) { mem[CODE] = 0x4E; mem[CODE + 1U] = 0x72; mem[CODE + 2U] = (uint8_t)(s->stop_imm >> 8);
                     mem[CODE + 3U] = (uint8_t)s->stop_imm; }
}
static void print_state(const char *name, int step, unsigned pc, unsigned sr, unsigned a7, unsigned usp) {
  unsigned i;
  printf("%s %d pc=%05X sr=%04X a7=%05X usp=%05X top=", name, step, pc, sr & 0xFFFFU, a7, usp);
  for (i = 0; i < 6U; ++i) printf("%02X", (a7 + i) < MEM ? mem[a7 + i] : 0U);
  printf("\n");
}
'''

CONTRACT = r'''
#include "exception_core.h"
typedef struct { uint16_t sr; uint32_t a7, other, pc; SegarecompM68kInterruptState irq; } Cpu;
static int valid(void *c, uint32_t base, uint32_t length, SegarecompM68kStackDirection d) {
  (void)c; (void)d; return base >= 0x100U && base + length <= MEM; }
static int rd(void *c, uint32_t a, uint32_t size, uint32_t *v) {
  uint32_t i, x = 0; (void)c; if (a + size > MEM) return 0;
  for (i = 0; i < size; ++i) x = (x << 8) | mem[a + i]; *v = x; return 1; }
static void wr(void *c, uint32_t a, uint32_t size, uint32_t v) {
  uint32_t i; (void)c; for (i = 0; i < size; ++i) mem[a + i] = (uint8_t)(v >> (8U * (size - 1U - i))); }
static SegarecompM68kVectorResolution vec(void *c, uint32_t v, uint32_t *h) {
  uint32_t x = 0; (void)c; if (!rd(c, v * 4U, 4U, &x)) return SEGARECOMP_M68K_VECTOR_NOT_INSTALLED;
  *h = x; return SEGARECOMP_M68K_VECTOR_HANDLER; }
/* Test scaffold: the scripts only ever place NOP and STOP #imm (with S kept set) in memory. */
static void execute_one(Cpu *cpu) {
  if (cpu->irq.stopped) return;
  if (mem[cpu->pc] == 0x4E && mem[cpu->pc + 1U] == 0x72) {
    cpu->sr = (uint16_t)((((unsigned)mem[cpu->pc + 2U] << 8) | mem[cpu->pc + 3U]) & SEGARECOMP_M68K_SR_IMPLEMENTED);
    cpu->pc += 4U; cpu->irq.stopped = 1U; return;
  }
  cpu->pc += 2U;
}
int main(void) {
  unsigned n; int k, refused = 0;
  SegarecompM68kMachineHooks hooks = {0};
  hooks.validate_stack_extent = valid; hooks.stack_read = rd; hooks.frame_write = wr; hooks.resolve_vector = vec;
  for (n = 0; n < SCENARIO_COUNT; ++n) {
    const Scenario *s = &scenarios[n];
    Cpu cpu = {0};
    SegarecompM68kCpuBinding binding = {&cpu.sr, &cpu.a7, &cpu.other, &cpu.pc};
    build_memory(s);
    cpu.sr = (uint16_t)s->sr; cpu.pc = CODE;
    cpu.a7 = (s->sr & 0x2000U) ? SSP0 : USP0; cpu.other = (s->sr & 0x2000U) ? USP0 : SSP0;
    for (k = 0; k < s->count; ++k) {
      const Step *t = &steps[s->first + k];
      if (t->new_sr >= 0) cpu.sr = (uint16_t)t->new_sr;
      segarecomp_m68k_interrupt_sample(&cpu.irq, (uint32_t)t->level);
      {
        const uint32_t level = segarecomp_m68k_interrupt_recognized_level(&cpu.irq, cpu.sr);
        if (level != 0U) {
          const uint32_t vector = segarecomp_m68k_interrupt_vector((SegarecompM68kInterruptAcknowledge)t->ack, level,
                                                                  (uint32_t)t->supplied);
          if (segarecomp_m68k_interrupt_enter(&hooks, &binding, &cpu.irq, level, vector, cpu.pc, 0) !=
              SEGARECOMP_M68K_EXCEPTION_OK) { printf("%s %d entry-failed\n", s->name, k); continue; }
        }
      }
      execute_one(&cpu);
      { const int sup = (cpu.sr & 0x2000U) != 0U;
        print_state(s->name, k, cpu.pc, cpu.sr, cpu.a7, sup ? cpu.other : cpu.a7); }
    }
  }
  /* Documented expected values beyond the oracle: supplied vectors outside 64-255 are refused, levels outside 1-7 map
     to nothing, and a refused vector changes nothing. */
  refused |= segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_SUPPLIED_VECTOR, 5U, 63U) != 0U;
  refused |= segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_SUPPLIED_VECTOR, 5U, 5U) != 0U;
  refused |= segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_SUPPLIED_VECTOR, 5U, 256U) != 0U;
  refused |= segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR, 0U, 0U) != 0U;
  refused |= segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR, 8U, 0U) != 0U;
  refused |= segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR, 1U, 0U) != 25U;
  refused |= segarecomp_m68k_interrupt_vector(SEGARECOMP_M68K_INTERRUPT_ACK_AUTOVECTOR, 7U, 0U) != 31U;
  {
    Cpu cpu = {0};
    SegarecompM68kCpuBinding binding = {&cpu.sr, &cpu.a7, &cpu.other, &cpu.pc};
    cpu.sr = 0x2000; cpu.a7 = SSP0; cpu.pc = CODE; cpu.irq.stopped = 1U; cpu.irq.level7_edge_pending = 1U;
    refused |= segarecomp_m68k_interrupt_enter(&hooks, &binding, &cpu.irq, 7U, 0U, CODE, 0) !=
               SEGARECOMP_M68K_EXCEPTION_VECTOR_UNAVAILABLE;
    refused |= cpu.sr != 0x2000 || cpu.a7 != SSP0 || cpu.pc != CODE || !cpu.irq.stopped || !cpu.irq.level7_edge_pending;
    /* Wake possibility: a level above the loaded mask, a possible level-7 transition or a latched one. */
    cpu.irq.level7_edge_pending = 0U;
    refused |= segarecomp_m68k_stop_wake_possible(&cpu.irq, 6U, 0x2600) != 0;
    refused |= segarecomp_m68k_stop_wake_possible(&cpu.irq, 6U, 0x2500) != 1;
    refused |= segarecomp_m68k_stop_wake_possible(&cpu.irq, 0U, 0x2000) != 0;
    refused |= segarecomp_m68k_stop_wake_possible(&cpu.irq, 7U, 0x2700) != 1;
    cpu.irq.level7_edge_pending = 1U;
    refused |= segarecomp_m68k_stop_wake_possible(&cpu.irq, 0U, 0x2700) != 1;
  }
  printf("documented=%s\n", refused ? "FAIL" : "ok");
  return 0;
}
'''

ORACLE = r'''
#include "m68k.h"
static const Step *current;
static int ack(int level) {
  (void)level;
  if (current->ack == 1) return current->supplied;
  if (current->ack == 2) return (int)M68K_INT_ACK_SPURIOUS;
  if (current->ack == 3) return 15;
  return (int)M68K_INT_ACK_AUTOVECTOR;
}
static unsigned rd8(unsigned a) { return a < MEM ? mem[a] : 0U; }
unsigned int m68k_read_memory_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return (rd8(a) << 8U) | rd8(a + 1U); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16U) | m68k_read_memory_16(a + 2U); }
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { if (a < MEM) mem[a] = (uint8_t)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { m68k_write_memory_8(a, v >> 8U); m68k_write_memory_8(a + 1U, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16U); m68k_write_memory_16(a + 2U, v); }
void m68k_write_memory_32_pd(unsigned int a, unsigned int v) { m68k_write_memory_32(a, v); }
int main(void) {
  unsigned n; int k;
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_set_int_ack_callback(ack);
  for (n = 0; n < SCENARIO_COUNT; ++n) {
    const Scenario *s = &scenarios[n];
    build_memory(s);
    current = &steps[s->first];
    m68k_set_irq(0);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SR, s->sr);
    m68k_set_reg(M68K_REG_USP, USP0); m68k_set_reg(M68K_REG_ISP, SSP0);
    m68k_set_reg(M68K_REG_PC, CODE);
    (void)m68k_execute(1); /* drain the pending reset cycles; executes no instruction */
    for (k = 0; k < s->count; ++k) {
      const Step *t = &steps[s->first + k];
      current = t;
      if (t->new_sr >= 0) m68k_set_reg(M68K_REG_SR, (unsigned)t->new_sr);
      m68k_set_irq((unsigned)t->level);
      (void)m68k_execute(1);
      { const unsigned sr = m68k_get_reg(NULL, M68K_REG_SR);
        const unsigned a7 = m68k_get_reg(NULL, M68K_REG_A7);
        print_state(s->name, k, m68k_get_reg(NULL, M68K_REG_PC), sr, a7,
                    (sr & 0x2000U) ? m68k_get_reg(NULL, M68K_REG_USP) : a7); }
    }
  }
  return 0;
}
'''


def env():
    result = os.environ.copy()
    if not result.get("SDKROOT") and shutil.which("xcrun"):
        result["SDKROOT"] = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True,
                                           check=True).stdout.strip()
    return result


def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, env=env(), **kwargs)
    assert result.returncode == 0, (args, result.stdout[-2000:], result.stderr[-4000:])
    return result


def expected_documented(lines):
    """Hand-derived MC68000 expectations for representative steps (independent of any emulator)."""
    by_key = {}
    for line in lines:
        parts = line.split()
        if len(parts) == 7:
            by_key[(parts[0], int(parts[1]))] = dict(p.split("=") for p in parts[2:])
    handler = lambda vector, executed=True: "%05X" % (0x3000 + vector * 0x10 + (2 if executed else 0))
    for mask in range(8):
        for level in range(1, 7):
            state = by_key[("L%d_M%d" % (level, mask), 0)]
            if level > mask:  # accepted: autovector 24 + level, SR mask = level, next instruction (0x400) stacked
                assert state["pc"] == handler(24 + level) and state["sr"] == "%04X" % (0x2000 | level << 8), state
                assert state["a7"] == "07FFA" and state["top"] == "%04X00000400" % (0x2000 | mask << 8), state
            else:  # masked: the NOP executes, nothing is stacked
                assert state["pc"] == "00402" and state["a7"] == "08000", state
        state = by_key[("L7_edge_M%d" % mask, 0)]
        assert state["pc"] == handler(31) and state["sr"] == "2700", state  # non-maskable transition, any mask
    held = [by_key[("L7_held", i)] for i in range(6)]
    assert held[0]["pc"] == handler(31) and held[1]["pc"] == "031F4" and held[2]["pc"] == "031F6", held
    assert held[3]["pc"] == handler(31) and held[3]["a7"] == "07FF4" and held[3]["top"] == "2600000031F6", held
    assert held[4]["pc"] == "031F4" and held[5]["pc"] == "031F6", held
    never = [by_key[("L7_never_again", i)] for i in range(4)]
    assert never[0]["pc"] == handler(31) and [s["pc"] for s in never[1:]] == ["031F4", "031F6", "031F8"], never
    again = [by_key[("L7_retransition", i)] for i in range(3)]
    assert again[1]["pc"] == "031F4" and again[2]["pc"] == handler(31) and again[2]["a7"] == "07FF4", again
    for name, vector in (("ack_auto", 29), ("ack_vec64", 64), ("ack_vec255", 255), ("ack_spurious", 24),
                         ("ack_uninit", 15)):
        state = by_key[(name, 0)]
        assert state["pc"] == handler(vector) and state["sr"] == "2500", (name, state)
    user = by_key[("user_mode", 0)]
    assert user["pc"] == handler(25) and user["sr"] == "2115" and user["a7"] == "07FFA" and user["usp"] == "06000"
    assert user["top"] == "001500000400", user
    trace = by_key[("trace_cleared", 0)]
    assert trace["sr"] == "241F" and trace["top"] == "A31F00000400", trace
    stop = [by_key[("stop_wake", i)] for i in range(5)]
    assert [s["pc"] for s in stop[:3]] == ["00404"] * 3 and stop[3]["pc"] == handler(28), stop
    assert stop[3]["top"] == "230000000404" and stop[3]["sr"] == "2400", stop
    nmi = [by_key[("stop_nmi", i)] for i in range(4)]
    assert [s["pc"] for s in nmi[:3]] == ["00404"] * 3 and nmi[3]["pc"] == handler(31), nmi
    assert nmi[3]["top"] == "270000000404", nmi


def main():
    compiler = sys.argv[1]
    table = c_table()
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)
        (temp / "contract.c").write_text(table + COMMON + CONTRACT)
        checked([compiler, *CFLAGS, "-I", str(HEADER_DIR), str(temp / "contract.c"), "-o", str(temp / "contract")])
        contract = checked([str(temp / "contract")]).stdout
        lines = contract.splitlines()
        assert lines[-1] == "documented=ok", lines[-1]
        assert not any("entry-failed" in line for line in lines), [l for l in lines if "entry-failed" in l]
        expected_documented(lines[:-1])
        checkout = next((os.environ[name] for name in CHECKOUT_ENVS if os.environ.get(name)), None)
        if not checkout:
            print("m68k_interrupt_acceptance_test: %d steps match documented values; pinned Musashi oracle unavailable"
                  % (len(lines) - 1))
            return 0
        checkout = pathlib.Path(checkout)
        assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN
        assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0
        checked([compiler, "-std=c11", str(checkout / "m68kmake.c"), "-o", str(temp / "m68kmake")])
        shutil.copyfile(checkout / "m68k_in.c", temp / "m68k_in.c")
        checked([str(temp / "m68kmake")], cwd=temp)
        (temp / "oracle.c").write_text(table + COMMON + ORACLE)
        checked([compiler, "-std=c11", "-DM68K_EMULATE_INT_ACK=M68K_OPT_ON", "-I", str(temp), "-I", str(checkout),
                 "-I", str(checkout / "softfloat"), str(temp / "oracle.c"), str(checkout / "m68kcpu.c"),
                 str(temp / "m68kops.c"), str(checkout / "softfloat/softfloat.c"), "-o", str(temp / "oracle")])
        oracle = checked([str(temp / "oracle")]).stdout.splitlines()
        mismatches = [(a, b) for a, b in zip(lines[:-1], oracle) if a != b]
        assert len(oracle) == len(lines) - 1 and not mismatches, mismatches[:10]
        print("m68k_interrupt_acceptance_test: %d steps match documented values and the pinned Musashi core"
              % len(oracle))
    return 0


if __name__ == "__main__":
    sys.exit(main())
