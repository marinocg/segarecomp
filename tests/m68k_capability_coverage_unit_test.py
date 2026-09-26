#!/usr/bin/env python3
"""SEG-021-T002: focused controls for the coverage tool's measurement rules (no probe, no ROM, no Musashi).

usage: m68k_capability_coverage_unit_test.py <c-compiler> <product-root>
"""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

CC, ROOT = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
spec = importlib.util.spec_from_file_location("cov", ROOT / "tools" / "m68k_capability_coverage.py")
cov = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cov)
_, FORMS = cov.load_forms()
BY_ID = {f["id"]: f for f in FORMS}


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def forms_of(mnemonic, **match):
    return [f for f in FORMS if f["mnemonic"] == mnemonic and all(f[k] == v for k, v in match.items())]


# --- exception / privilege classification -------------------------------------------------------------
trap = forms_of("TRAP")[0]
trap_only = dict(trap, exceptions=["trap_vector_32_47"], privilege="user")  # isolate the vector-range rule
for n in (0, 5, 15):
    word = 0x4E40 | n
    check(word in set(cov.expand(trap)), "TRAP #%d word is in the TRAP form" % n)
    check(cov.needed_vectors(trap_only, word) == {32 + n}, "TRAP #%d must require vector %d" % (n, 32 + n))
    # An effect contract naming vector 32 for every TRAP proves only TRAP #0; the true vector proves TRAP #n.
    check(cov.exception_modeled({32 + n}, 32) == (n == 0), "single vector 32 must not prove TRAP #%d" % n)
    check(cov.exception_modeled({32 + n}, 32 + n), "the encoded vector proves TRAP #%d" % n)
    # SEG-021-T032: the dataset also lists address error for TRAP; that class is a declared deferred
    # disposition (ADR 0043 section 4), so the real form requires exactly the TRAP vector.
    check("address_error_vector_3" in trap["exceptions"], "the dataset still lists address error for TRAP")
    check(cov.needed_vectors(trap, word) == {32 + n} and cov.exception_modeled(cov.needed_vectors(trap, word), 32 + n),
          "a deferred class is never required; the real TRAP form needs only its own vector")
privileged = [f for f in FORMS if f["privilege"] != "user" and not f["exceptions"] or f["mnemonic"] == "RESET"][0]
word = next(iter(cov.expand(privileged)))
check(cov.needed_vectors(privileged, word) >= {8}, "a privileged form requires vector 8")
check(cov.exception_modeled({8}, 8) and not cov.exception_modeled({8}, 0), "vector 8 only when modeled")
# SEG-021-T032 negative controls: a form listing only the deferred address-error class is not applicable to
# exception_privilege_modeled; a form listing vector 3 plus vector 5 still requires vector 5.
base_form = dict(forms_of("ADD")[0], privilege="user")
only_deferred = dict(base_form, exceptions=["address_error_vector_3"])
check(not cov.exception_row_applicable(only_deferred), "a deferred-only form is not applicable")
check(cov.needed_vectors(only_deferred, next(iter(cov.expand(only_deferred)))) == set(),
      "a deferred-only form requires no vector")
mixed = dict(base_form, exceptions=["address_error_vector_3", "zero_divide_vector_5"])
check(cov.exception_row_applicable(mixed), "a mixed form stays applicable")
check(cov.needed_vectors(mixed, next(iter(cov.expand(mixed)))) == {5}, "a mixed form still requires vector 5")
check(not cov.exception_modeled({5}, 0) and cov.exception_modeled({5}, 5), "vector 5 is required for the mixed form")
split = cov.deferred_disposition(FORMS)
check(split["forms"] == split["only_deferred_forms"] + split["mixed_forms"], "deferred bucket arithmetic")
check(split["forms"] == sum(1 for f in FORMS if "address_error_vector_3" in f["exceptions"]),
      "deferred bucket counts every form listing a deferred class")
multi = dict(base_form, exceptions=["zero_divide_vector_5", "chk_vector_6"])
need = cov.needed_vectors(multi, next(iter(cov.expand(multi))))
check(len(need) > 1, "a form listing several classes requires several vectors")
check(not any(cov.exception_modeled(need, v) for v in range(0, 48)), "one effect vector must not prove several exception classes")
check(cov.exception_modeled(set(), 0), "forms with no exception class are not penalised")

# --- SEG-021-T025: justified restrictions must name a currently failing, applicable stage ---------------------
stages_all = cov.ALL_STAGES
def fake_rows(passing):
    return {fid: (BY_ID[fid], {s: passing for s in stages_all}, {s: True for s in stages_all}, 1)
            for fid, _stage in cov.JUSTIFIED_RESTRICTIONS}
check(len(cov.justified_restrictions(fake_rows(False))) == len(cov.JUSTIFIED_RESTRICTIONS),
      "justified restrictions resolve against failing stages")
try:
    cov.justified_restrictions(fake_rows(True))
    check(False, "a justified restriction whose stage now passes must be reported as stale")
except ValueError:
    pass

# --- CCR/SR expectation --------------------------------------------------------------------------------
check(all(cov.ccr_expected(f) for f in forms_of("ADD")), "ADD modifies CCR")
check(not any(cov.ccr_expected(f) for f in forms_of("MOVEA")), "MOVEA does not modify CCR")
check(not any(cov.ccr_expected(f) for f in forms_of("LEA")), "LEA does not modify CCR")
check(not any(cov.ccr_expected(f) for f in forms_of("ADDQ", dst="an")), "ADDQ to An does not modify CCR")
check(all(cov.ccr_expected(f) for f in forms_of("ADDQ") if f["dst"] != "an"), "ADDQ to other destinations modifies CCR")
check(cov.ea_effect_expected(forms_of("CMPM")[0]), "CMPM has auto-update side effects")

# --- ratchet comparison controls (drops fail for every corrected dimension) ---------------------------
stages = cov.ALL_STAGES
base = {"f": "1" * len(stages)}
for stage in ("ccr_sr_effect_declared", "exception_privilege_modeled", "ea_footprint_declared",
              "route_runtime_routed_executes", "ccr_sr_validated", "ea_side_effect_validated"):
    i = stages.index(stage)
    dropped = {"f": base["f"][:i] + "0" + base["f"][i + 1:]}
    drops, _, _ = cov.diff_masks(base, dropped, stages)
    check(len(drops) == 1 and stage in drops[0], "dropping %s must be reported" % stage)
    zero = {"f": "0" * len(stages)}
    _, improvements, tolerated = cov.diff_masks(zero, {"f": ("0" * i) + "1" + "0" * (len(stages) - i - 1)}, stages)
    check(bool(improvements) != bool(tolerated), "an improvement is classified exactly once")
    check(bool(tolerated) == (stage in cov.COMPILER_DEPENDENT), "only compiler-dependent improvements are tolerated")

# --- native execution starts from an identical state for every word -----------------------------------
def synthetic_chunk(workdir, order):
    """w_0001 writes memory; w_0002 reports (through its return code) whether it can see that write."""
    funcs = {
        0x0001: ["static int w_0001(cap_state *s) {", "  s->ram[0x100] = 0x55U; s->d[0] = 0xDEADBEEFU; s->sr = 0U; return 0;", "}"],
        0x0002: ["static int w_0002(cap_state *s) {",
                 "  return (s->ram[0x100] == 0x55U || s->d[0] == 0xDEADBEEFU || s->sr == 0U) ? 7 : 0;", "}"],
    }
    header = ["#include <stdint.h>", "#include <stddef.h>",
              "typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;"]
    tail = ["int cap_touch_000(void) { return 0; }", "typedef int (*cap_fn)(cap_state *);"]
    path = workdir / "chunk_000.c"
    cov.write(path, cov.build_chunk(header, [funcs[w] for w in order], tail, "000"))
    check(not cov.compile_chunk(CC, path), "synthetic chunk compiles")
    return path


results = []
for order in ([0x0001, 0x0002], [0x0002, 0x0001]):
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        chunk = synthetic_chunk(work, order)
        result, crashed = cov.native_exec(CC, work, [chunk])
        check(not crashed and result[0x0001] == 0, "writer probe runs")
        results.append(result[0x0002])
check(results == [0, 0], "a memory/register-writing probe leaked state into the next probe: %r" % results)

# --- emitted file-scope call-frame state and the routed runner are restored per word too ---------------
def frame_chunk(workdir, routed, order):
    if routed:
        header = ['#include "runtime.h"', "extern uint32_t frame_ids[64]; extern uint32_t frame_continuations[64]; extern uint32_t frame_depth;"]
        sig, ret = "static GenesisControlTransfer %s(GenesisRuntime *s) {", "GenesisControlTransfer t = {0}; t.kind = %s; return t;"
        funcs = {
            0x0001: [sig % "rw_0001", "  frame_depth += 1U; frame_ids[0] = 9U; s->d[0] = 0xDEADBEEFU; s->work_ram[0x100] = 0x55U;",
                     "  " + ret % "GENESIS_CONTINUE_AT_PC", "}"],
            0x0002: [sig % "rw_0002", "  const int leaked = frame_depth != 0U || frame_ids[0] != 0U || s->d[0] == 0xDEADBEEFU || s->work_ram[0x100] == 0x55U;",
                     "  " + ret % "(leaked ? GENESIS_STOP : GENESIS_CONTINUE_AT_PC)", "}"],
        }
        tail = ["int cap_touch_000(void) { return 0; }", "typedef GenesisControlTransfer (*cap_fn)(GenesisRuntime *);"]
        path = workdir / "rchunk_000.c"
    else:
        header = ["#include <stdint.h>", "typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;",
                  "extern uint32_t frame_ids[64]; extern uint32_t frame_continuations[64]; extern uint32_t frame_depth;"]
        funcs = {
            0x0001: ["static int w_0001(cap_state *s) {", "  (void)s; frame_depth += 1U; frame_ids[0] = 9U; return 0;", "}"],
            0x0002: ["static int w_0002(cap_state *s) {", "  (void)s; return (frame_depth != 0U || frame_ids[0] != 0U) ? 7 : 0;", "}"],
        }
        tail = ["int cap_touch_000(void) { return 0; }", "typedef int (*cap_fn)(cap_state *);"]
        path = workdir / "chunk_000.c"
    cov.write(path, cov.build_chunk(header, [funcs[w] for w in order], tail, "000"))
    check(not cov.compile_chunk(CC, path, cov.RUNTIME_DIR if routed else None), "frame control chunk compiles")
    return path


with tempfile.TemporaryDirectory() as tmp:
    runtime_obj = pathlib.Path(tmp) / "runtime.o"
    subprocess.run([CC, "-std=c11", "-O0", "-I", str(cov.RUNTIME_DIR), "-c", str(cov.RUNTIME_DIR / "runtime.c"),
                    "-o", str(runtime_obj)], check=True, capture_output=True)
    for routed in (False, True):
        for order in ([0x0001, 0x0002], [0x0002, 0x0001]):
            with tempfile.TemporaryDirectory() as sub:
                work = pathlib.Path(sub)
                chunk = frame_chunk(work, routed, order)
                result, crashed = cov.native_exec(CC, work, [chunk], routed=routed,
                                                  extra_objects=[runtime_obj] if routed else ())
                check(not crashed and result[0x0002] == 0,
                      "%s runner leaked frame/register/RAM state into the next probe: %r" % ("routed" if routed else "direct", result))

# --- compile bisection: a failing function is removed, the survivors recompile and run -------------
with tempfile.TemporaryDirectory() as tmp:
    work = pathlib.Path(tmp)
    header = ["#include <stdint.h>", "typedef struct { uint32_t d[8]; uint32_t a[8]; uint16_t sr; uint32_t pc; uint32_t usp; uint8_t ram[0x100000]; } cap_state;"]
    good = ["static int w_0001(cap_state *s) {", "  (void)s; return 0;", "}"]
    bad = ["static int w_0002(cap_state *s) {", "  return undeclared_symbol_for_control(s);", "}"]
    tail = ["int cap_touch_000(void) { return 0; }", "typedef int (*cap_fn)(cap_state *);"]
    path = work / "chunk_000.c"
    cov.write(path, cov.build_chunk(header, [good, bad], tail, "000"))
    check(cov.compile_chunk(CC, path) == {2}, "only the failing function is rejected")
    result, _ = cov.native_exec(CC, work, [path])
    check(result == {1: 0}, "the surviving function is recompiled and executed")
print("m68k capability coverage unit controls OK")
