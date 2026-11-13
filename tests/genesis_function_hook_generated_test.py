#!/usr/bin/env python3
"""SEG-138-T003/T004 (ADR 0298): statically resolved M68K function hooks in generated-native programs.

A project-authored synthetic image (a vector table and code at $200: a main loop that calls function A ($220: ADDQ.L #1,D0 ; RTS) and function B ($240: JSR A ; ADDQ.L #1,D1 ; RTS)) is built
into a generated bridge program with and without hook sites, compiled as strict C11 and run for 600 virtual frames. Proves:

 * noop-hook-equivalence: before/after/before-after hooks with no-op callbacks, a hooked program with no hook host attached, and a declined replacement leave the complete guest state
   (registers, SR, PC, work RAM, master clock, frame count) bit-identical to the unhooked program over 600 frames, including nested calls;
 * callback order: entry callbacks in call order, exit callbacks in reverse order for nested functions, each at the right architectural state;
 * replacement: an exact replacement (results + flags + declared cost) is bit-identical to the original; a wrong cost changes the clock; a different result changes D0 (the callback is the authority);
 * BSR and JSR call forms; calls from a caller the build did not prove are skipped with a counter and the guest is unchanged;
 * unhooked functions' generated text is byte-identical (only the dispatch wrapper differs); two emissions are identical; an unrelated edit does not move the site table entry;
 * the dispatch-boundary step itself (frame stack): exits, abandoned frames, a full frame stack, a stack outside work RAM, an unproven caller.
usage: genesis_function_hook_generated_test.py <m68k_pipeline_tests> <C compiler> <source root>"""
import difflib
import os
import pathlib
import subprocess
import sys
import tempfile

emitter, compiler, root = sys.argv[1:4]
RUNTIME = pathlib.Path(root) / "platforms/genesis/runtime"


def build(parts, base=0x200):
    img = bytearray()
    for addr, hexs in parts:
        off = addr - base
        assert off >= len(img), hex(addr)
        img += b"\x4e\x71" * ((off - len(img)) // 2)
        img += bytes.fromhex(hexs)
    return img.hex().upper()


JSR_A, JSR_B = "4EB900000220", "4EB900000240"
MAIN = [(0x200, JSR_A + JSR_B + "66F2" + "4E70"), (0x220, "5280" + "4E75"), (0x240, JSR_A + "5281" + "4E75")]
BSR = [(0x200, "6100001E" + "6100003A" + "66F6" + "4E70"), (0x220, "5280" + "4E75"), (0x240, JSR_A + "5281" + "4E75")]
# A is also called through JSR (A0) with A0 = $220: that caller's continuation is not one the build proves for A.
INDIRECT = [(0x200, JSR_A + "41F900000220" + "4E90" + "66F0" + "4E70"), (0x220, "5280" + "4E75")]


def emit(parts, hooks="-", vectors="-"):
    run = subprocess.run([emitter, "--emit-synthetic-hooked-bridge", hooks, vectors, build(parts)], text=True, capture_output=True)
    return run


def emit_ok(parts, hooks="-", vectors="-"):
    run = emit(parts, hooks, vectors)
    assert run.returncode == 0, (hooks, run.stdout[:200], run.stderr[:200])
    assert not run.stdout.startswith("/* translation rejected:"), run.stdout[:200]
    return run.stdout


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "function_hooks.h"
#define main generated_bridge_main
#include "generated.c"
#undef main

typedef struct { unsigned enters, leaves, skipped[4]; char trace[96]; unsigned trace_n; unsigned bad; unsigned completed_a; int style; uint32_t last_cycles; } Probe;
static Probe probe;
enum { NOOP = 1, EXACT = 2, DECLINE = 3, COST = 4, WRONG = 5 };

static void note(char kind, const GenesisHookSiteInfo *site) {
  if (probe.trace_n + 2U < sizeof probe.trace) { probe.trace[probe.trace_n++] = kind; probe.trace[probe.trace_n++] = site->id[0]; }
}
static int cb_enter(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t entry_sp) {
  uint32_t value = 0, cycles = 0, d0 = rt->d[0];
  (void)ctx; ++probe.enters; note('E', site);
  if (!genesis_hook_reg_read(rt, entry_sp, 8U + 7U, &value) || value != entry_sp) ++probe.bad;           /* A7 at entry is the entry stack pointer */
  if (!genesis_hook_reg_read(rt, entry_sp, GENESIS_HOOK_REG_CYCLES, &cycles) || cycles < probe.last_cycles) ++probe.bad;   /* the cycle counter never runs backwards */
  probe.last_cycles = cycles;
  if (site->id[0] == 'a' && (site->mode & GENESIS_HOOK_MODE_AFTER) != 0U && d0 != probe.completed_a) ++probe.bad;   /* D0 counts completed calls of A at its entry */
  if ((site->mode & GENESIS_HOOK_MODE_REPLACE) != 0U) {
    uint32_t result, ccr;
    if (probe.style == DECLINE || probe.style == NOOP) return 0;
    result = d0 + 1U + (probe.style == WRONG ? 1U : 0U);
    ccr = (result == 0U ? 4U : 0U) | ((result & 0x80000000U) ? 8U : 0U) | (d0 == 0xFFFFFFFFU ? 0x11U : 0U) | ((((d0 ^ result) & (1U ^ result)) >> 31U) ? 2U : 0U);
    if (!genesis_hook_reg_write(rt, 0U, result) || !genesis_hook_reg_write(rt, GENESIS_HOOK_REG_CCR, ccr)) ++probe.bad;
    if (genesis_hook_reg_write(rt, 15U, 0U)) ++probe.bad;                       /* A7 is never writable */
    return 1;
  }
  return 0;
}
static void cb_leave(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t entry_sp) {
  (void)ctx; ++probe.leaves; note('L', site);
  if (rt->a[7] != entry_sp + 4U) ++probe.bad;                                    /* the return popped exactly the return address */
  if (site->id[0] == 'a') { ++probe.completed_a; if (rt->d[0] != probe.completed_a) ++probe.bad; }   /* the result is visible at exit */
}
static void cb_skipped(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t kind) { (void)ctx; (void)rt; (void)site; ++probe.skipped[kind & 3U]; }

static void put(GenesisSha256 *s, uint64_t v, unsigned bytes) { uint8_t b[8]; unsigned i; for (i = 0; i < bytes; ++i) b[i] = (uint8_t)(v >> (8U * (bytes - 1U - i))); genesis_sha256_update(s, b, bytes); }
static void digest(const GenesisRuntime *r, uint8_t out[32]) {
  GenesisSha256 s; unsigned i;
  genesis_sha256_init(&s);
  for (i = 0; i < 8; ++i) put(&s, r->d[i], 4);
  for (i = 0; i < 8; ++i) put(&s, r->a[i], 4);
  put(&s, r->sr, 2); put(&s, r->pc, 4);
  genesis_sha256_update(&s, r->work_ram, 65536U);
  put(&s, r->scheduler.master_ticks, 8); put(&s, r->virtual_frame_count, 8);
  genesis_sha256_final(&s, out);
}

int main(int argc, char **argv) {
  static GenesisRuntime runtime;
  static GenesisFunctionHooks hooks;
  GenesisControlTransfer t = {0};
  uint8_t d[32]; unsigned i, slices = 0;
  if (argc != 4) return 2;
  const uint64_t frames = strtoull(argv[1], 0, 10);
  const int attach = atoi(argv[2]);
  probe.style = atoi(argv[3]);
  runtime.a[7] = UINT32_C(0x00FF8000); runtime.pc = UINT32_C(0x00000200); runtime.sr = UINT16_C(0x2700);
  runtime.owned_regions = genesis_owned_cartridge_regions; runtime.owned_region_count = UINT32_C(1);
  if (attach) { hooks.enter = cb_enter; hooks.leave = cb_leave; hooks.skipped = cb_skipped; runtime.function_hooks = &hooks; }
  while (runtime.virtual_frame_count < frames && slices < 4000000U) { t = genesis_runtime_run(&runtime, genesis_bridge_dispatch, 997U); ++slices; if (t.kind == GENESIS_STOP) break; }
  digest(&runtime, d);
  for (i = 0; i < 32; ++i) printf("%02x", d[i]);
  probe.trace[probe.trace_n] = 0;
  printf(" frames=%llu ticks=%llu d0=%u d1=%u enters=%u leaves=%u bad=%u abandoned=%llu unproven=%llu overflow=%llu trace=%s\n", (unsigned long long)runtime.virtual_frame_count,
         (unsigned long long)runtime.scheduler.master_ticks, runtime.d[0], runtime.d[1], probe.enters, probe.leaves, probe.bad, (unsigned long long)hooks.abandoned,
         (unsigned long long)hooks.unproven_callers, (unsigned long long)hooks.overflowed, probe.trace);
  return t.kind == GENESIS_STOP ? 3 : 0;
}
'''

STEP_HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"
#include "function_hooks.h"

/* The step is exercised directly on hand-made runtime states: no program, only the const site table. */
static const uint32_t conts[] = { UINT32_C(0x00000206), UINT32_C(0x0000020C), UINT32_C(0x00000246) };
static const GenesisHookSiteInfo sites[] = {
  { UINT32_C(0x00000220), "a", GENESIS_HOOK_MODE_AFTER | GENESIS_HOOK_MODE_BEFORE, 0U, 0U, 0U, 0U, 2U },
  { UINT32_C(0x00000240), "b", GENESIS_HOOK_MODE_AFTER, 0U, 0U, 0U, 2U, 1U },
};
static char log_text[128]; static unsigned log_n; static unsigned skipped_kind[4];
static int enter(void *c, GenesisRuntime *r, const GenesisHookSiteInfo *s, uint32_t sp) { (void)c; (void)r; (void)sp; log_text[log_n++] = 'E'; log_text[log_n++] = s->id[0]; return 0; }
static void leave(void *c, GenesisRuntime *r, const GenesisHookSiteInfo *s, uint32_t sp) { (void)c; (void)r; (void)sp; log_text[log_n++] = 'L'; log_text[log_n++] = s->id[0]; }
static void skipped(void *c, GenesisRuntime *r, const GenesisHookSiteInfo *s, uint32_t kind) { (void)c; (void)r; (void)s; ++skipped_kind[kind & 3U]; }
static void put32(GenesisRuntime *r, uint32_t address, uint32_t v) { uint32_t o = address - 0x00FF0000U; r->work_ram[o] = (uint8_t)(v >> 24); r->work_ram[o + 1] = (uint8_t)(v >> 16); r->work_ram[o + 2] = (uint8_t)(v >> 8); r->work_ram[o + 3] = (uint8_t)v; }
static int step(GenesisRuntime *r) { GenesisControlTransfer t; memset(&t, 0, sizeof t); return genesis_function_hook_step(r, sites, 2U, conts, &t); }
static GenesisRuntime rt; static GenesisFunctionHooks fh;

int main(void) {
  unsigned i;
  fh.enter = enter; fh.leave = leave; fh.skipped = skipped; rt.function_hooks = &fh;
  /* NULL hooks: nothing at all happens */
  { GenesisFunctionHooks *keep = rt.function_hooks; rt.function_hooks = 0; rt.pc = 0x220; rt.a[7] = 0x00FF0100U; put32(&rt, 0x00FF0100U, 0x206U); assert(step(&rt) == 0); rt.function_hooks = keep; assert(log_n == 0U); }
  /* A called from $200 (continuation $206), then B from $20C... nested: B ($240) calls A ($220) whose continuation $246 belongs to B only */
  rt.pc = 0x240; rt.a[7] = 0x00FF0100U; put32(&rt, 0x00FF0100U, 0x20CU);                 /* B was called from $206? only $246 is B's: $20C is not a continuation of B */
  assert(step(&rt) == 0 && fh.unproven_callers == 1U && skipped_kind[2] == 1U && fh.depth == 0U);   /* unproven caller: skipped, guest untouched */
  put32(&rt, 0x00FF0100U, 0x246U);                                                          /* A's continuation set is {$206,$20C}; $246 is not one of them */
  rt.pc = 0x220; assert(step(&rt) == 0 && fh.unproven_callers == 2U);
  put32(&rt, 0x00FF0100U, 0x206U);
  rt.pc = 0x220; assert(step(&rt) == 0 && fh.depth == 1U);                                  /* A entered from its proven caller: before + frame */
  assert(strncmp(log_text, "Ea", 2) == 0);
  /* A's frame: entry_sp = $FF0100; it returns to $206 with A7 = $FF0104 */
  rt.pc = 0x206; rt.a[7] = 0x00FF0104U; assert(step(&rt) == 0 && fh.depth == 0U); assert(strcmp(log_text, "EaLa") == 0);
  /* nested: B enters (frame), calls A (frame), A returns, B returns: exits run in reverse order */
  log_n = 0U; memset(log_text, 0, sizeof log_text);
  rt.pc = 0x240; rt.a[7] = 0x00FF0200U; put32(&rt, 0x00FF0200U, 0x246U + 0U); /* $246 is a continuation of B? it is slot 2: sites[1] has begin 2 count 1 -> $246 */
  assert(step(&rt) == 0 && fh.depth == 1U);
  rt.pc = 0x220; rt.a[7] = 0x00FF01FCU; put32(&rt, 0x00FF01FCU, 0x206U);
  assert(step(&rt) == 0 && fh.depth == 2U);
  rt.pc = 0x206; rt.a[7] = 0x00FF0200U;     /* A returned (to $206 in this fabricated state) with A7 = A's entry_sp + 4 = B's entry_sp -- and B is not yet back at $246 */
  assert(step(&rt) == 0 && fh.depth == 1U);
  rt.pc = 0x246; rt.a[7] = 0x00FF0204U; assert(step(&rt) == 0 && fh.depth == 0U);
  assert(strcmp(log_text, "EaLaLb") == 0);   /* B has only an exit hook: no entry callback, but its frame still orders its exit after the nested A */
  /* an abandoned frame: A7 rises above entry_sp + 4 without the return */
  log_n = 0U; memset(log_text, 0, sizeof log_text); fh.depth = 0U;
  rt.pc = 0x220; rt.a[7] = 0x00FF0300U; put32(&rt, 0x00FF0300U, 0x206U); assert(step(&rt) == 0 && fh.depth == 1U);
  rt.pc = 0x1000; rt.a[7] = 0x00FF0310U; assert(step(&rt) == 0 && fh.depth == 0U && fh.abandoned == 1U && skipped_kind[1] == 1U);
  /* a return at the right address but the wrong SP is not the function's return, and the stack is not unwound: the frame stays */
  rt.pc = 0x220; rt.a[7] = 0x00FF0300U; assert(step(&rt) == 0 && fh.depth == 1U);
  rt.pc = 0x206; rt.a[7] = 0x00FF02F0U; assert(step(&rt) == 0 && fh.depth == 1U);
  fh.depth = 0U;
  /* a stack outside work RAM: skipped */
  rt.pc = 0x220; rt.a[7] = 0x00000100U; assert(step(&rt) == 0 && fh.depth == 0U); assert(skipped_kind[2] == 3U);
  rt.a[7] = 0x00FF0301U; assert(step(&rt) == 0 && skipped_kind[2] == 4U);                        /* odd stack pointer */
  /* a full frame stack drops the new frame, counts it and never corrupts the stack */
  for (i = 0; i < GENESIS_HOOK_FRAMES_MAX; ++i) { rt.pc = 0x220; rt.a[7] = 0x00FF1000U - 8U * i; put32(&rt, rt.a[7], 0x206U); assert(step(&rt) == 0); }
  assert(fh.depth == GENESIS_HOOK_FRAMES_MAX);
  rt.pc = 0x220; rt.a[7] = 0x00FF0800U; put32(&rt, rt.a[7], 0x206U); assert(step(&rt) == 0 && fh.overflowed == 1U && skipped_kind[3] == 1U && fh.depth == GENESIS_HOOK_FRAMES_MAX);
  /* a stale frame (function left without its original return, A7 left at entry + 4) is retired by the next call at that SP: no duplicate exit */
  fh.depth = 0U; log_n = 0U; memset(log_text, 0, sizeof log_text); fh.abandoned = 0U;
  rt.pc = 0x220; rt.a[7] = 0x00FF0400U; put32(&rt, 0x00FF0400U, 0x206U); assert(step(&rt) == 0 && fh.depth == 1U);
  rt.pc = 0x1000; rt.a[7] = 0x00FF0404U; assert(step(&rt) == 0 && fh.depth == 1U);              /* left through another PC, same SP: still pending */
  rt.pc = 0x220; rt.a[7] = 0x00FF0400U; assert(step(&rt) == 0 && fh.depth == 1U && fh.abandoned == 1U);   /* the new call retires it */
  rt.pc = 0x206; rt.a[7] = 0x00FF0404U; assert(step(&rt) == 0 && fh.depth == 0U); assert(strcmp(log_text, "EaEaLa") == 0);   /* exactly one exit for the one real return */
  fh.depth = 0U;
  /* the register view */
  rt.d[3] = 0xAABBCCDDU; rt.a[2] = 0x1234U; rt.sr = 0x271FU;
  { uint32_t v = 0; assert(genesis_hook_reg_read(&rt, 0x00FF0100U, 3U, &v) && v == 0xAABBCCDDU); assert(genesis_hook_reg_read(&rt, 0x00FF0100U, 10U, &v) && v == 0x1234U); assert(genesis_hook_reg_read(&rt, 0x00FF0100U, GENESIS_HOOK_REG_CCR, &v) && v == 0x1FU);
    assert(!genesis_hook_reg_read(&rt, 0x00FF0100U, 18U, &v) && !genesis_hook_reg_read(&rt, 0x00FF0100U, 23U, &v) && !genesis_hook_reg_read(&rt, 0x00FF0100U, 32U, &v));
    put32(&rt, 0x00FF0104U, 0xCAFE0001U); put32(&rt, 0x00FF0108U, 0xCAFE0002U);
    assert(genesis_hook_reg_read(&rt, 0x00FF0100U, GENESIS_HOOK_REG_STACK0, &v) && v == 0xCAFE0001U); assert(genesis_hook_reg_read(&rt, 0x00FF0100U, GENESIS_HOOK_REG_STACK0 + 1U, &v) && v == 0xCAFE0002U);
    assert(!genesis_hook_reg_read(&rt, 0x00000100U, GENESIS_HOOK_REG_STACK0, &v));                  /* a stack slot outside work RAM is not readable */
    assert(genesis_hook_reg_write(&rt, 1U, 7U) && rt.d[1] == 7U); assert(genesis_hook_reg_write(&rt, 14U, 9U) && rt.a[6] == 9U); assert(!genesis_hook_reg_write(&rt, 15U, 9U) && rt.a[7] != 9U);
    assert(genesis_hook_reg_write(&rt, GENESIS_HOOK_REG_CCR, 0xFFU) && rt.sr == 0x271FU); assert(!genesis_hook_reg_write(&rt, GENESIS_HOOK_REG_CYCLES, 1U)); assert(!genesis_hook_reg_write(&rt, GENESIS_HOOK_REG_STACK0, 1U)); }
  puts("step: ok");
  return 0;
}
'''


def compile_run(directory, generated, args, harness=HARNESS, name="program"):
    d = pathlib.Path(directory)
    (d / "generated.c").write_text(generated, encoding="utf-8")
    (d / f"{name}.c").write_text(harness, encoding="utf-8")
    exe = d / name
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic", *os.environ.get("SEGARECOMP_TEST_CFLAGS", "").split()]   # e.g. -fsanitize=address,undefined
    built = subprocess.run([compiler, *flags, "-I", str(RUNTIME), "-I", str(d), str(d / f"{name}.c"), str(RUNTIME / "runtime.c"), str(RUNTIME / "vdp_render.c"),
                            str(RUNTIME / "frame_export.c"), "-o", str(exe)], text=True, capture_output=True)
    assert built.returncode == 0, built.stderr[-3000:]
    ran = subprocess.run([str(exe), *map(str, args)], text=True, capture_output=True, timeout=300)
    assert ran.returncode == 0, (args, ran.stdout[-500:], ran.stderr[-500:])
    return ran.stdout.strip()


def fields(line):
    digest, *rest = line.split(" ")
    out = {"digest": digest}
    for item in rest:
        k, _, v = item.partition("=")
        out[k] = v
    return out


with tempfile.TemporaryDirectory() as temporary:
    tmp = pathlib.Path(temporary)

    def run(label, parts, hooks, attach, style, frames=600, vectors="-"):
        d = tmp / label
        d.mkdir()
        return fields(compile_run(d, emit_ok(parts, hooks, vectors), [frames, attach, style]))

    # ---- noop-hook-equivalence over 600 frames, including nested calls (B calls A)
    plain = run("plain", MAIN, "-", 0, 0)
    assert plain["frames"] == "600" and int(plain["enters"]) == 0
    both = "a:220:both:3:0:0;b:240:both:0:0:0"
    for label, hooks, attach, style in (("null-host", both, 0, 0), ("noop", both, 1, 1), ("before", "a:220:before:3:0:0", 1, 1), ("after", "a:220:after:3:0:0;b:240:after:0:0:0", 1, 1),
                                        ("declined", "a:220:replace:1:10001:24:1", 1, 3)):
        got = run(label, MAIN, hooks, attach, style)
        assert got["digest"] == plain["digest"] and got["ticks"] == plain["ticks"] and got["frames"] == "600", (label, got, plain)
        assert got["bad"] == "0", (label, got)
    noop = run("noop-counts", MAIN, both, 1, 1)
    assert int(noop["enters"]) > 100000 and abs(int(noop["enters"]) - int(noop["leaves"])) <= 2, noop        # one call may be in flight at the 600th frame boundary
    assert noop["abandoned"] == "0" and noop["unproven"] == "0" and noop["overflow"] == "0", noop
    # nested order: A (outer call from main), then B -> A nested; entries in call order, exits in reverse
    assert noop["trace"].startswith("EaLaEbEaLaLbEaLaEbEaLaLb"), noop["trace"]

    # ---- BSR as the call form
    bsr_plain = run("bsr-plain", BSR, "-", 0, 0)
    bsr_noop = run("bsr-noop", BSR, both, 1, 1)
    assert bsr_noop["digest"] == bsr_plain["digest"] and bsr_noop["bad"] == "0" and int(bsr_noop["enters"]) > 100000, (bsr_noop, bsr_plain)
    assert bsr_noop["trace"].startswith("EaLaEbEaLaLb"), bsr_noop["trace"]

    # ---- replacement: exact results + flags + declared cost is bit-identical; wrong cost / wrong result are visible
    replace = "a:220:replace:1:10001:24:1"
    exact = run("replace-exact", MAIN, replace, 1, 2)
    assert exact["digest"] == plain["digest"] and exact["ticks"] == plain["ticks"] and exact["bad"] == "0", (exact, plain)
    assert int(exact["enters"]) > 100000 and exact["leaves"] == "0"
    cost20 = run("replace-cost", MAIN, "a:220:replace:1:10001:20:1", 1, 2)
    assert cost20["digest"] != plain["digest"] and int(cost20["d0"]) > int(plain["d0"]), (cost20, plain)   # the declared cost IS the time charged: a cheaper replacement completes more calls in the same 600 frames
    wrong = run("replace-wrong", MAIN, replace, 1, 5)
    assert wrong["digest"] != plain["digest"] and wrong["d0"] != plain["d0"], (wrong, plain)                       # the callback is the authority on the result
    declined_b = run("replace-b-declined", MAIN, "b:240:replace:2:10003:60:1", 1, 3)                                # B declines every call: the original B (and the nested A) runs unchanged
    assert declined_b["digest"] == plain["digest"] and declined_b["bad"] == "0", (declined_b, plain)

    # ---- a caller the build did not prove is skipped (guest unchanged, counted)
    ind_plain = run("ind-plain", INDIRECT, "-", 0, 0)
    ind = run("ind-hooked", INDIRECT, "a:220:both:3:0:0", 1, 1)
    assert ind["digest"] == ind_plain["digest"], (ind, ind_plain)
    assert int(ind["unproven"]) > 100000 and int(ind["enters"]) > 100000, ind        # the direct JSR is hooked, the JSR (A0) is skipped and counted

    # ---- unhooked functions' generated text is unchanged: only the dispatch wrapper differs
    text_plain = emit_ok(MAIN)
    text_hooked = emit_ok(MAIN, "a:220:both:3:0:0")
    assert text_hooked == emit_ok(MAIN, "a:220:both:3:0:0"), "hooked output must be deterministic"
    assert text_plain == emit_ok(MAIN), "unhooked output must be deterministic"
    a, b = text_plain.splitlines(), text_hooked.splitlines()
    changed_plain = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        changed_plain.extend(a[i1:i2])
        inserted = "\n".join(b[j1:j2])
        assert "genesis_dispatch" in inserted or "function_hooks.h" in inserted or "genesis_hook" in inserted, inserted[:200]
    assert changed_plain == ["static GenesisControlTransfer genesis_dispatch(GenesisRuntime *runtime) {"], changed_plain
    assert "genesis_hook" not in text_plain and "function_hooks.h" not in text_plain

    # ---- entry-point stability: an unrelated function elsewhere does not move the site entry; a different hook set changes the table only
    more = MAIN + [(0x400, "5282" + "4E75")]
    row = [l for l in text_hooked.splitlines() if l.startswith('  { UINT32_C(0x00000220), "a"')]
    row_more = [l for l in emit_ok(more, "a:220:both:3:0:0").splitlines() if l.startswith('  { UINT32_C(0x00000220), "a"')]
    assert row and row == row_more, (row, row_more)

    # ---- the dispatch-boundary step in isolation
    step = compile_run(tmp, "", [], STEP_HARNESS, "step")
    assert step == "step: ok", step

print("genesis_function_hook_generated_test: OK")
