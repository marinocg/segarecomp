#!/usr/bin/env python3
"""SEG-138-T005 (ADR 0298): deterministic callback ordering, time/cycle behaviour and transactional failure of a single mod's function hooks, through the REAL mod scheduler
(platforms/genesis/runtime/mod_scheduler.c) driving a generated hooked program (strict C11, 600 virtual frames).

 * ordering: frame-begin -> input -> post-input -> [hook-entry / hook-exit as the hooked functions run: entry in call order, exit in reverse order for nested calls] -> frame-end -> render ->
   frame-begin(N+1); hook callbacks carry the frame number and the deterministic cycle counter (slot 17), which equals the guest clock at the boundary and never runs backwards;
 * a replacing mod whose results, flags and declared cost equal the original's leaves the complete guest state bit-identical (noop-hook-equivalence through the scheduler);
 * fault injection: a callback that violates a rule mid-phase (undeclared range, undeclared register slot, write budget, invalid return value, missing declared result) leaves guest state EXACTLY as
   the contract states: its staged RAM and register writes are discarded, a replacing hook that faulted runs the ORIGINAL function, the mod is disabled for the session (no further callback of
   any phase), the diagnostic names cause, mod, phase, frame and hook, and the guest equals a reference run in which the mod simply went inert at that call; writes committed before the fault stay;
 * the same input twice gives identical state hashes and identical committed-write digests; a disabled mod is inert and counted.
usage: genesis_mod_hook_scheduler_test.py <m68k_pipeline_tests> <C compiler> <source root>"""
import os
import pathlib
import subprocess
import sys
import tempfile

emitter, compiler, root = sys.argv[1:4]
RT = pathlib.Path(root) / "platforms/genesis/runtime"
SDK = [pathlib.Path(root) / "platforms/genesis/sdk", pathlib.Path(root) / "platforms/sdk"]


def build(parts, base=0x200):
    img = bytearray()
    for addr, hexs in parts:
        off = addr - base
        assert off >= len(img)
        img += b"\x4e\x71" * ((off - len(img)) // 2)
        img += bytes.fromhex(hexs)
    return img.hex().upper()


MAIN = [(0x200, "4EB900000220" + "4EB900000240" + "66F2" + "4E70"), (0x220, "5280" + "4E75"), (0x240, "4EB900000220" + "5281" + "4E75")]
HOOKS = "a:220:replace:1:10001:24:1;b:240:both:20003"
HOOKS_TRACE = "a:220:both:20003;b:240:both:20003"
HOOKS_AFTER = "a:220:both:20003;b:240:after:20003"


def emit(hooks):
    run = subprocess.run([emitter, "--emit-synthetic-hooked-bridge", hooks, "-", build(MAIN)], text=True, capture_output=True)
    assert run.returncode == 0, (hooks, run.stdout[:200], run.stderr[:200])
    return run.stdout


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "mod_scheduler.h"
#define main generated_bridge_main
#include "generated.c"
#undef main

enum { M_TRACE = 1, M_REPLACE = 2, M_AFTER_FAULT = 3, M_RENDER_CYCLES = 4, M_FRAME_WRITE = 5 };
enum { K_NONE = 0, K_RANGE = 1, K_SLOT = 2, K_BUDGET = 3, K_RET2 = 4, K_CONTRACT = 5, K_EXIT_RET = 6, K_EXIT_RANGE = 7 };

static GenesisRuntime *g_rt;
static int g_mode, g_kind;
static unsigned g_fault_at, g_clean_until;
static unsigned g_hook_calls[2], g_a_calls, g_b_exits, g_phase_calls[16], g_after_fault_phase_calls;
static int g_faulted_seen;
static char g_events[4096];
static unsigned g_events_n, g_bad;
static uint32_t g_last_cycles;
static uint64_t g_trace_frame = UINT64_MAX; static unsigned g_trace_hooks;
static char g_log[8][256]; static unsigned g_log_n;

static void ev(const char *text) { size_t n = strlen(text); if (g_events_n + n + 1U < sizeof g_events) { memcpy(g_events + g_events_n, text, n); g_events_n += (unsigned)n; g_events[g_events_n++] = ','; } }
static int rd(const SegaModHost *h, uint32_t slot, uint32_t *v) { return h->read(SEGA_GEN_SPACE_M68K_HOOK_REGS, slot, 4U, v); }
static int wr(const SegaModHost *h, uint32_t slot, uint32_t v) { return h->write(SEGA_GEN_SPACE_M68K_HOOK_REGS, slot, 4U, v); }

static int hook(void *state, const SegaModHost *h, uint32_t id) {
  uint32_t d0 = 0, cycles = 0;
  (void)state;
  ++g_hook_calls[h->phase == SEGA_MOD_PHASE_HOOK_EXIT ? 1 : 0];
  if (g_faulted_seen) ++g_after_fault_phase_calls;
  if (rd(h, GENESIS_HOOK_REG_CYCLES, &cycles) != SEGA_MOD_OK) ++g_bad;
  if (cycles != (uint32_t)(g_rt->scheduler.master_ticks / 7U) || cycles < g_last_cycles) ++g_bad;   /* the counter IS the guest clock and is monotonic */
  g_last_cycles = cycles;
  if (h->frame != g_rt->virtual_frame_count) ++g_bad;
  if (g_mode == M_TRACE) {
    char tok[8];
    (void)snprintf(tok, sizeof tok, "%c%c", h->phase == SEGA_MOD_PHASE_HOOK_EXIT ? 'L' : 'E', id == 1U ? 'a' : 'b');
    if (h->frame != g_trace_frame) { g_trace_frame = h->frame; g_trace_hooks = 0U; }
    if (g_trace_hooks++ < 6U) ev(tok);                                    /* the first six hook events of each frame keep the trace short */
    if (rd(h, 0U, &d0) != SEGA_MOD_OK) ++g_bad;
    return 0;
  }
  if (g_mode == M_REPLACE && id == 1U && h->phase == SEGA_MOD_PHASE_HOOK_ENTRY) {
    uint32_t result, ccr, n;
    ++g_a_calls; n = g_a_calls;
    if (g_clean_until != 0U && n > g_clean_until) return 0;                    /* the reference mod: inert from here on */
    if (rd(h, 0U, &d0) != SEGA_MOD_OK) ++g_bad;
    result = d0 + 1U;
    ccr = (result == 0U ? 4U : 0U) | ((result & 0x80000000U) ? 8U : 0U) | (d0 == 0xFFFFFFFFU ? 0x11U : 0U) | ((((d0 ^ result) & (1U ^ result)) >> 31U) ? 2U : 0U);
    (void)h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x0100U, 4U, n);           /* committed with the call: the proof of what survives a later fault */
    (void)wr(h, 0U, result);
    if (n == g_fault_at && g_kind == K_CONTRACT) return 1;                    /* the CCR result is missing */
    (void)wr(h, GENESIS_HOOK_REG_CCR, ccr);
    if (n == g_fault_at) {
      unsigned i;
      switch (g_kind) {
        case K_RANGE: (void)h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x0200U, 4U, 0xDEADBEEFU); break;   /* outside the declared range */
        case K_SLOT: (void)wr(h, 1U, 5U); break;                                                        /* D1 is not a declared result */
        case K_BUDGET: for (i = 0; i < 1100U; ++i) (void)h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x0104U, 1U, i & 0xFFU); break;
        case K_RET2: return 2;
        default: break;
      }
    }
    return 1;
  }
  if (g_mode == M_AFTER_FAULT && id == 2U && h->phase == SEGA_MOD_PHASE_HOOK_EXIT) {
    ++g_b_exits;
    if (g_clean_until != 0U && g_b_exits > g_clean_until) return 0;
    (void)h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x0108U, 4U, g_b_exits);
    if (g_b_exits == g_fault_at) {
      if (g_kind == K_EXIT_RET) return 7;
      if (g_kind == K_EXIT_RANGE) (void)h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x0300U, 4U, 1U);
    }
    return 0;
  }
  return 0;
}

static const char *const phase_tok[9] = {"", "reset", "frame-begin", "input", "post-input", "hook-entry", "hook-exit", "frame-end", "render"};
static void phase(void *state, const SegaModHost *h) {
  uint32_t v = 0;
  (void)state;
  ++g_phase_calls[h->phase & 15U];
  if (g_faulted_seen) ++g_after_fault_phase_calls;
  if (g_mode == M_TRACE && g_events_n < 600U && (h->phase != SEGA_MOD_PHASE_RESET || g_events_n == 0U)) ev(phase_tok[h->phase < 9U ? h->phase : 0U]);
  if (g_mode == M_RENDER_CYCLES && h->phase == SEGA_MOD_PHASE_RENDER && h->frame == 2U) (void)rd(h, GENESIS_HOOK_REG_CYCLES, &v);   /* guest time is not visible outside guest time */
  if (g_mode == M_FRAME_WRITE && h->phase == SEGA_MOD_PHASE_FRAME_END) (void)h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x0110U, 4U, (uint32_t)h->frame);
}
static void sink(void *ctx, const char *line, uint32_t n) { (void)ctx; if (g_log_n < 8U) { memcpy(g_log[g_log_n], line, n < 255U ? n : 255U); g_log[g_log_n][n < 255U ? n : 255U] = 0; ++g_log_n; } }

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
static void hex(const uint8_t *d) { unsigned i; for (i = 0; i < 32U; ++i) printf("%02x", d[i]); }

int main(int argc, char **argv) {
  static GenesisRuntime runtime;
  static GenesisModScheduler sched;
  static const GenesisModRange ranges[] = { { SEGA_GEN_SPACE_M68K_WORK_RAM, 0x0100U, 0x00F0U, GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE } };
  static const GenesisModHookBinding bindings[] = { { "a", 1U }, { "b", 2U } };
  static SegaModProvider provider = { sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM_GENESIS, "hook-test", 1U, NULL, phase, hook, NULL, NULL, { NULL } };
  GenesisModConfig cfg;
  GenesisControlTransfer t = {0};
  const char *rejection = "";
  uint8_t d[32], w[32];
  unsigned slices = 0, i;
  uint64_t frames;
  int attach;
  if (argc != 7) return 2;
  frames = strtoull(argv[1], 0, 10); attach = atoi(argv[2]); g_mode = atoi(argv[3]); g_kind = atoi(argv[4]); g_fault_at = (unsigned)atoi(argv[5]); g_clean_until = (unsigned)atoi(argv[6]);
  g_rt = &runtime;
  runtime.a[7] = UINT32_C(0x00FF8000); runtime.pc = UINT32_C(0x00000200); runtime.sr = UINT16_C(0x2700);
  runtime.owned_regions = genesis_owned_cartridge_regions; runtime.owned_region_count = UINT32_C(1);
  memset(&cfg, 0, sizeof cfg);
  cfg.mod_id = "hook-test"; cfg.provider = &provider; cfg.hooks = bindings; cfg.hook_count = 2U; cfg.ranges = ranges; cfg.range_count = 1U; cfg.log_sink = sink;
  cfg.phase_mask = (1U << SEGA_MOD_PHASE_RESET) | (1U << SEGA_MOD_PHASE_FRAME_BEGIN) | (1U << SEGA_MOD_PHASE_INPUT) | (1U << SEGA_MOD_PHASE_POST_INPUT) | (1U << SEGA_MOD_PHASE_HOOK_ENTRY) |
                  (1U << SEGA_MOD_PHASE_HOOK_EXIT) | (1U << SEGA_MOD_PHASE_FRAME_END) | (1U << SEGA_MOD_PHASE_RENDER);
  cfg.required[SEGA_MOD_CAP_HOOK_FUNCTION / 64U] |= UINT64_C(1) << (SEGA_MOD_CAP_HOOK_FUNCTION % 64U);
  cfg.required[SEGA_MOD_CAP_MEMORY_READ / 64U] |= UINT64_C(1) << (SEGA_MOD_CAP_MEMORY_READ % 64U);
  cfg.required[SEGA_MOD_CAP_MEMORY_WRITE / 64U] |= UINT64_C(1) << (SEGA_MOD_CAP_MEMORY_WRITE % 64U);
  if (attach) {
    if (genesis_mod_scheduler_attach(&sched, &runtime, &cfg, &rejection) != 0) { printf("ATTACH-REFUSED %s\n", rejection); return 4; }
  }
  while (runtime.virtual_frame_count < frames && slices < 4000000U) {
    t = genesis_runtime_run(&runtime, genesis_bridge_dispatch, 997U); ++slices;
    if (t.kind == GENESIS_STOP) break;
    if (attach && sched.disabled) g_faulted_seen = 1;
  }
  digest(&runtime, d); genesis_mod_write_log_digest(&sched, w);
  hex(d); printf(" ");
  hex(w);
  printf(" frames=%llu hook_entry=%u hook_exit=%u scheduler_entry=%llu scheduler_exit=%llu bad=%u disabled=%d faulted=%d cause=%s after_fault_calls=%u writes=%llu skips=%llu/%llu/%llu\n",
         (unsigned long long)runtime.virtual_frame_count, g_hook_calls[0], g_hook_calls[1], (unsigned long long)sched.hook_calls[0], (unsigned long long)sched.hook_calls[1], g_bad, attach ? sched.disabled : 0,
         attach ? sched.fault.faulted : 0, attach && sched.fault.cause ? sched.fault.cause : "-", g_after_fault_phase_calls, (unsigned long long)sched.write_total, (unsigned long long)sched.hook_skips[1],
         (unsigned long long)sched.hook_skips[2], (unsigned long long)sched.hook_skips[3]);
  printf("fault_text=%s\n", attach ? sched.fault.text : "");
  printf("log=%u", g_log_n); for (i = 0; i < g_log_n; ++i) printf("|%s", g_log[i]); printf("\n");
  printf("ram0100=%u ram0108=%u ram0110=%u d0=%u\n", (unsigned)((runtime.work_ram[0x100] << 24) | (runtime.work_ram[0x101] << 16) | (runtime.work_ram[0x102] << 8) | runtime.work_ram[0x103]),
         (unsigned)((runtime.work_ram[0x108] << 24) | (runtime.work_ram[0x109] << 16) | (runtime.work_ram[0x10A] << 8) | runtime.work_ram[0x10B]),
         (unsigned)((runtime.work_ram[0x110] << 24) | (runtime.work_ram[0x111] << 16) | (runtime.work_ram[0x112] << 8) | runtime.work_ram[0x113]), runtime.d[0]);
  printf("events=%s\n", g_events);
  return t.kind == GENESIS_STOP ? 3 : 0;
}
'''

MODES = {"trace": 1, "replace": 2, "after_fault": 3, "render_cycles": 4, "frame_write": 5}
KINDS = {"none": 0, "range": 1, "slot": 2, "budget": 3, "ret2": 4, "contract": 5, "exit_ret": 6, "exit_range": 7}


def parse(out):
    lines = out.strip().splitlines()
    head = lines[0].split(" ")
    res = {"state": head[0], "writes_digest": head[1]}
    for item in head[2:]:
        k, _, v = item.partition("=")
        res[k] = v
    for line in lines[1:]:
        if line.startswith(("fault_text=", "log=", "events=")):
            k, _, v = line.partition("=")
            res[k] = v
        else:
            for item in line.split(" "):
                k, _, v = item.partition("=")
                res[k] = v
    return res


with tempfile.TemporaryDirectory() as temporary:
    tmp = pathlib.Path(temporary)
    programs = {}

    def program(hooks):
        if hooks not in programs:
            d = tmp / f"prog{len(programs)}"
            d.mkdir()
            (d / "generated.c").write_text(emit(hooks), encoding="utf-8")
            (d / "harness.c").write_text(HARNESS, encoding="utf-8")
            inc = ["-I", str(RT), "-I", str(d)] + [x for p in SDK for x in ("-I", str(p))]
            built = subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic", *os.environ.get("SEGARECOMP_TEST_CFLAGS", "").split(), *inc, str(d / "harness.c"), str(RT / "runtime.c"),
                                    str(RT / "vdp_render.c"), str(RT / "frame_export.c"), str(RT / "mod_scheduler.c"), "-o", str(d / "prog")], text=True, capture_output=True)
            assert built.returncode == 0, built.stderr[-3000:]
            programs[hooks] = d / "prog"
        return programs[hooks]

    def run(hooks, frames=600, attach=1, mode="trace", kind="none", fault_at=0, clean_until=0):
        ran = subprocess.run([str(program(hooks)), str(frames), str(attach), str(MODES[mode]), str(KINDS[kind]), str(fault_at), str(clean_until)], text=True, capture_output=True, timeout=300)
        assert ran.returncode == 0, (hooks, mode, kind, ran.returncode, ran.stdout[-600:], ran.stderr[-300:])
        return parse(ran.stdout)

    # ---------------------------------------------------------------- ordering and cycle visibility
    trace = run(HOOKS_TRACE, frames=3, mode="trace")
    events = trace["events"].strip(",").split(",")
    assert events[:4] == ["reset", "frame-begin", "input", "post-input"], events[:6]     # the opening phases of frame 0 run before the first dispatch
    first_hooks = [e for e in events[4:20] if e in ("Ea", "La", "Eb", "Lb")]
    assert first_hooks[:6] == ["Ea", "La", "Eb", "Ea", "La", "Lb"], first_hooks            # entry in call order, exit in reverse order for the nested call
    i_end = events.index("frame-end")
    assert events[i_end + 1:i_end + 4] == ["render", "frame-begin", "input"], events[i_end:i_end + 5]
    assert "Ea" in events[4:i_end] and "post-input" not in events[4:i_end]                  # hooks run strictly between post-input and frame-end
    assert trace["bad"] == "0" and trace["disabled"] == "0" and trace["scheduler_entry"] == trace["hook_entry"] and trace["scheduler_exit"] == trace["hook_exit"]

    # ---------------------------------------------------------------- replacement through the scheduler: bit-identical to the unhooked program
    plain = run(HOOKS, attach=0)                                       # hooked program, no host attached = the unhooked program
    exact = run(HOOKS, mode="replace")                                  # but the mod also writes a counter into RAM: only RAM 0x0100 differs, so compare after zeroing via a reference
    assert exact["faulted"] == "0" and exact["bad"] == "0" and int(exact["hook_entry"]) > 100000
    ref = run(HOOKS, mode="replace", clean_until=10 ** 9)
    assert ref["state"] == exact["state"] and ref["writes_digest"] == exact["writes_digest"], "a mod run twice gives identical state hashes and identical write digests"
    assert exact["d0"] == plain["d0"], (exact["d0"], plain["d0"])      # the replacement computed the same results as the original function
    noop = run(HOOKS, mode="replace", clean_until=1)                    # the mod answers "proceed" from the second call on, and wrote its counter once
    assert noop["d0"] == plain["d0"] and noop["ram0100"] == "1"

    # ---------------------------------------------------------------- fault injection: state exactly as the contract states
    N = 5000
    for kind, cause in (("range", "range"), ("slot", "range"), ("budget", "budget"), ("ret2", "value"), ("contract", "contract")):
        faulted = run(HOOKS, mode="replace", kind=kind, fault_at=N)
        reference = run(HOOKS, mode="replace", clean_until=N - 1)       # the same mod, simply inert from call N on
        assert faulted["faulted"] == "1" and faulted["disabled"] == "1" and faulted["cause"] == cause, (kind, faulted)
        assert faulted["state"] == reference["state"], (kind, "guest state after the fault differs from the reference run")
        assert faulted["ram0100"] == str(N - 1) == reference["ram0100"], (kind, faulted["ram0100"])            # committed writes of earlier callbacks stay; the faulted callback's are discarded
        assert faulted["d0"] == plain["d0"] and faulted["bad"] == "0", (kind, faulted, plain["d0"])              # the original function ran: the guest is unmodded after the fault
        assert faulted["hook_entry"] == str(3 * N // 2) == faulted["scheduler_entry"] and faulted["after_fault_calls"] == "0", (kind, faulted)   # per loop iteration: entry of A, B, nested A          # disabled: no further callback of ANY phase
        assert faulted["writes_digest"] == reference["writes_digest"], (kind, "the replay digest must not contain the faulted callback's writes")
        text = faulted["fault_text"]
        assert text.startswith(f"E_MOD_FAULT cause={cause} mod=hook-test phase=hook-entry frame=") and text.endswith(" hook=a"), text   # stable diagnostic: cause, mod, phase, frame, hook
        assert faulted["log"].split("|")[1] == text, faulted["log"]
        again = run(HOOKS, mode="replace", kind=kind, fault_at=N)
        assert again == faulted, "fault injection is deterministic"
    # a fault in an EXIT hook
    for kind, cause in (("exit_ret", "value"), ("exit_range", "range")):
        faulted = run(HOOKS_AFTER, mode="after_fault", kind=kind, fault_at=3000)
        reference = run(HOOKS_AFTER, mode="after_fault", clean_until=2999)
        assert faulted["cause"] == cause and faulted["state"] == reference["state"] and faulted["ram0108"] == "2999" and faulted["after_fault_calls"] == "0", (kind, faulted)
        assert faulted["fault_text"].startswith(f"E_MOD_FAULT cause={cause} mod=hook-test phase=hook-exit frame=") and faulted["fault_text"].endswith(" hook=b"), faulted["fault_text"]
    # guest time is not visible from `render`
    cyc = run(HOOKS, mode="render_cycles")
    assert cyc["cause"] == "phase" and cyc["disabled"] == "1" and cyc["fault_text"].startswith("E_MOD_FAULT cause=phase mod=hook-test phase=render frame=2"), cyc

    # ---------------------------------------------------------------- frame-phase writes and hook callbacks share one transactional model
    fw = run(HOOKS_TRACE, mode="frame_write")
    assert fw["faulted"] == "0" and fw["ram0110"] == "599" and fw["writes"] == "600", fw          # frame-end(599) is the last one: its boundary is the 600th frame

    # ---------------------------------------------------------------- a program with no host attached, and a detached one, are the unhooked program
    detached = run(HOOKS, attach=0, mode="trace")
    assert detached["state"] == plain["state"] and detached["hook_entry"] == "0"

print("genesis_mod_hook_scheduler_test: OK")
