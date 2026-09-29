#!/usr/bin/env python3
"""SEG-008-T008: Z80 T-state timing validation and deterministic deadline/resume contract closure.

usage: z80_timing_closure_test.py <z80_image_emitter> <cc> <product-root>

Hermetic (generated-native only); with SEGARECOMP_Z80_ORACLE_CHECKOUT the same vectors also run on the pinned oracle
and its T-states must agree too (skipped cleanly otherwise). Every generated program is bounded by finite deadlines.

  A. per-form / per-outcome T-states against the PUBLISHED table (the independent legal-forms dataset), with the
     outcome derived from an independent model of the condition, not from the timing: fixed, both outcomes of JR cc /
     CALL cc / RET cc / DJNZ, the identical cost of both JP cc outcomes, the repeating and final iteration of every
     block operation, HALT, prefix chains (4 T and R+1 per extra prefix) and R accounting.
  B. interrupt-entry cost (IM0 RST 13, IM1 13, IM2 19, NMI 11) and halted-cycle accounting on T007's vectors.
  C. deadline/resume property: straight-through == split at random seeded absolute-deadline schedules, no instruction
     is ever split (every stop is an instruction boundary of the single-stepped run), including INT/NMI raised between
     splits, wrapped-fetch instructions, and splits inside a prefix_lock run where INT/NMI are never accepted.
  D. outcome classes: resumable and fail-closed outcomes are disjoint; an error is always terminal and never a
     scheduling point.
"""
import json
import pathlib
import random
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
FAILED = []
RESUMABLE = {"deadline", "halted", "prefix_lock"}
ERRORS = {"no_owner", "mutable_code", "unresolved_fetch_mapping", "unknown_image_identity", "excluded_form",
          "im0_unsupported_acknowledge_byte"}
SAMPLE_PER_FORM = 16
SCHEDULES_PER_PROGRAM = 40


def check(cond, message):
    if not cond:
        FAILED.append(message)


# ----------------------------------------------------------------------------------------------------- A. tables
def condition(cc, f):
    z_, c_, pv, s_ = f & 0x40, f & 0x01, f & 0x04, f & 0x80
    return [not z_, bool(z_), not c_, bool(c_), not pv, bool(pv), not s_, bool(s_)][cc]


def taken_model(form_id, code, prefix_count, state):
    op = code[prefix_count]
    if form_id == "djnz.e.base":
        return ((state["b"] - 1) & 0xFF) != 0
    if form_id == "jr.cc4_e.base":
        return condition((op >> 3) & 3, state["f"])
    return condition((op >> 3) & 7, state["f"])  # ret cc / call cc / jp cc


def prefix_extra(form, code):
    """Superseded/ignored DD/FD prefixes before the form's own bytes (4 T and one M1 each)."""
    leading = 0
    while leading < len(code) and code[leading] in (0xDD, 0xFD):
        leading += 1
    return leading - (1 if form["space"] in ("dd", "fd", "ddcb", "fdcb") else 0)


def repeat_vectors(forms):
    """Block operations with BC/B = 1 (final iteration at once) and 2 (one repeating iteration, then final)."""
    data, _ = z.load_dataset()
    vecs = []
    for form_id, form in sorted(forms.items()):
        if form["timing"]["class"] != "repeat":
            continue
        io = form_id.split(".")[0] in ("inir", "indr", "otir", "otdr")
        for n in (1, 2):
            state = z.profile_state("zero", "t008-%s-%d" % (form_id, n))
            state.update(a=0x55, h=0x20, l=0x40, d=0x30, e=0x40, c=0x34 if io else n, b=n if io else 0, f=0)
            code = z.encoding_code(form, next(z.expand_ranges(form["byte_ranges"])), b"")
            vecs.append(z.Vec("t008/%s/%d" % (form_id, n), code, state, [step("i", 1)] * 3, form=form_id,
                              family=form["family"], oracle=True))
    return vecs


def expected_step(form, vec, step, start_pc):
    """(expected T-states, outcome label) of a single-instruction vector from the published timing only."""
    extra = prefix_extra(form, vec.code)
    t = form["timing"]
    klass = t["class"]
    if klass == "fixed":
        return t["t_states"] + 4 * extra, "fixed"
    if klass == "halt":
        return t["t_states"] + 4 * extra, "halt"
    if klass == "conditional":
        if form["id"] == "jp.cc_nn.base":
            raise AssertionError("jp.cc_nn is classed fixed in the dataset")
        taken = taken_model(form["id"], vec.code, extra, vec.state)
        return (t["taken"] if taken else t["not_taken"]) + 4 * extra, "taken" if taken else "not_taken"
    repeating = int(step["cpu"]["pc"], 16) == start_pc  # a repeating iteration rewinds PC to the instruction
    return (t["repeating"] if repeating else t["final"]) + 4 * extra, "repeating" if repeating else "final"


def sample_vectors(by_form, forms):
    picked = []
    for form_id in sorted(by_form):
        vecs = [v for v in by_form[form_id] if all(st["mode"] == "i" and not st["int"] and not st["nmi"] for st in v.steps)]
        klass = forms[form_id]["timing"]["class"]
        if klass in ("fixed",) and len(vecs) > SAMPLE_PER_FORM:
            stride = len(vecs) / SAMPLE_PER_FORM
            vecs = [vecs[int(i * stride)] for i in range(SAMPLE_PER_FORM)]
        picked.extend(vecs)
    return picked


def table_test(tc, work, root):
    _, forms = z.load_dataset()
    by_form = z.form_vectors()
    vecs = sample_vectors(by_form, forms) + repeat_vectors(forms)
    covered = {v.form for v in vecs}
    check(covered == set(forms), "forms without a single-instruction timing vector: %s" % sorted(set(forms) - covered)[:8])
    batches = z.slot_batches(vecs)
    report = z.run_batches(tc, batches, work / "tables", with_oracle=root is not None)
    seen, checked = {}, 0
    for batch in batches:
        r = report[batch.name]
        if r.get("error"):
            check(False, "table batch %s: %s" % (batch.name, str(r["error"])[:400]))
            continue
        for vec, _ in batch.vectors:
            form = forms[vec.form]
            steps = r["generated"].get(vec.name, [])
            if len(steps) != len(vec.steps):
                check(False, "%s: missing result" % vec.name)
                continue
            start = batch.slots[vec.code]
            extra = prefix_extra(form, vec.code)
            repeat = form["timing"]["class"] == "repeat"
            for k, got in enumerate(steps):
                if k and not repeat:
                    break
                want_t, label = expected_step(form, vec, got, start)
                if repeat:
                    # a step after the final iteration is a plain fetch of the following byte: stop there
                    if k and seen_final:
                        break
                seen_final = label == "final"
                checked += 1
                seen.setdefault(vec.form, set()).add(label)
                check(got["t"] == want_t, "%s step %d: %s T-states %d, published table says %d" % (
                    vec.name, k, label, got["t"], want_t))
                check(got["out"] in ("deadline", "halted"), "%s: unexpected outcome %s" % (vec.name, got["out"]))
                if k == 0 and not vec.form.startswith("ld.r_refresh_a"):
                    r0 = vec.state["r"]
                    want_r = (r0 & 0x80) | ((r0 + form["m1_fetches"] + extra) & 0x7F)
                    check(int(got["cpu"]["r"], 16) == want_r, "%s: R %s, expected %02X (m1 %d + %d prefixes)" % (
                        vec.name, got["cpu"]["r"], want_r, form["m1_fetches"], extra))
                if r["oracle"] is not None and vec.oracle:
                    o = r["oracle"].get(vec.name, [])
                    check(len(o) > k and o[k]["t"] == want_t, "%s step %d: oracle T-states differ from the published table" % (
                        vec.name, k))
    for form_id, form in forms.items():
        klass = form["timing"]["class"]
        if klass == "conditional":
            check(seen.get(form_id) == {"taken", "not_taken"}, "%s: outcomes exercised %s" % (form_id, seen.get(form_id)))
        elif klass == "repeat":
            check(seen.get(form_id) == {"repeating", "final"}, "%s: outcomes exercised %s" % (form_id, seen.get(form_id)))
    return checked


# ------------------------------------------------------------------------------------ B. interrupts / halt / scenarios
def scenario_test(tc, work, root):
    doc = z.load_scenarios()
    batches = z.scenario_batches(doc)
    report = z.run_batches(tc, batches, work / "scen", with_oracle=root is not None)
    results = {}
    for batch in batches:
        r = report[batch.name]
        check(not r.get("error"), "scenario batch %s: %s" % (batch.name, str(r.get("error"))[:300]))
        if r.get("error"):
            continue
        results.update(r["generated"])
        if r["oracle"] is not None:
            names = [v.name for v, _ in batch.vectors if v.oracle]
            check(z.first_divergence(r["generated"], r["oracle"], names) is None, "%s: oracle divergence" % batch.name)
    first_t = lambda name: results[name][0]["t"]  # noqa: E731
    for name in results:
        if name.startswith("t007_im2_") and name != "t007_im2_handler_not_emitted":
            check(first_t(name) == 19, "%s: IM2 entry is %d T, expected 19" % (name, first_t(name)))
        if name.startswith("t007_im0_rst_"):
            check(first_t(name) == 13, "%s: IM0 RST entry is %d T, expected 13" % (name, first_t(name)))
    check(first_t("int_im1") == 13, "IM1 entry must be 13 T")
    check(first_t("int_im2_odd_vector") == 19, "IM2 entry must be 19 T")
    check(first_t("int_im0_rst") == 13, "IM0 RST entry must be 13 T")
    check(first_t("nmi_accept") == 11, "NMI entry must be 11 T")
    check(first_t("nmi_directly_after_ei") == 11, "NMI entry after EI must be 11 T")
    check(first_t("halt_then_int") == 13 and first_t("halt_then_nmi") == 11, "acceptance out of HALT costs the entry only")
    check(first_t("t007_im2_halted") == 19, "IM2 entry out of HALT must be 19 T")
    check(first_t("halt_int_masked") == 4 and results["halt_int_masked"][0]["out"] == "halted",
          "a masked INT leaves HALT accounting at 4 T per halted cycle")
    # halted-cycle accounting: 4 T and R+1 per halted M1 cycle, rounded up to the deadline in whole cycles
    halted = results["halt_whole"]
    check(halted[1]["t"] == 100 and halted[1]["cpu"]["r"] == "1A", "HALT: 25 halted cycles = 100 T, R+25")
    # class discipline over every scenario
    for name, steps in results.items():
        for k, s in enumerate(steps):
            check(s["out"] in RESUMABLE | ERRORS, "%s step %d: unknown outcome %s" % (name, k, s["out"]))
            if s["out"] in ERRORS:
                check(k == len(steps) - 1, "%s: fail-closed stop %s is not terminal" % (name, s["out"]))
    return len(results)


# --------------------------------------------------------------------------------------------- C. deadline property
PROGRAM = bytes.fromhex(
    "210080" "111080" "010500" "EDB0"          # 0400 LD HL / LD DE / LD BC / LDIR (5 iterations)
    "0603" "DD3C" "10FC"                       # 040B LD B,3 ; DD INC A (ignored prefix) ; DJNZ back
    "DD213412" "DDCB05C6"                      # 0411 LD IX,nn ; SET 0,(IX+5)
    "CD3004"                                   # 0419 CALL 0430
    "FDDD3E42"                                 # 041C chained-prefix LD A,n
    "2802" "3C" "C23004"                       # 0420 JR Z / INC A / JP NZ
    "76")                                      # 0426 HALT
SUB = bytes.fromhex("3E01" "87" "C9")          # 0430 LD A,1 ; ADD A,A ; RET


def program_image():
    return {"images": [{"identity": 1, "kind": "invariant", "windows": [["0000", "0000", "10000"]], "fill": "00",
                        "patch": [["0400", PROGRAM.hex()], ["0430", SUB.hex()], ["0038", "C9"], ["0066", "ED45"]]}]}


def wrap_image():
    return {"images": [{"identity": 1, "kind": "invariant", "windows": [["0000", "0000", "10000"]], "fill": "00",
                        "patch": [["FFFE", "FD"], ["FFFF", "DD"], ["0000", "3E"], ["0001", "42"], ["0002", "C3FEFF"],
                                  ["0038", "C9"], ["0066", "ED45"]]}]}


def lock_image():
    return {"images": [{"identity": 1, "kind": "invariant", "windows": [["0000", "0000", "10000"]], "fill": "DD"}]}


def mk_vec(name, image_name, state_over, steps, load=(), expect_free=True):
    state = z.profile_state("zero", name)
    state.update(state_over)
    vec = z.Vec(name, b"", state, steps, image=image_name, load=list(load), oracle=False)
    return vec


def step(mode, budget, i=0, n=0):
    return {"mode": mode, "budget": budget, "int": i, "nmi": n, "map": 0}


def run_text(exe, vecs, image_doc, work):
    text = "".join(z.vector_text(v, v.state["pc"], z.memory_lines_for_load(image_doc, v.load)) for v in vecs)
    return z.run_exe(exe, text, work)


def build_image(tc, image_doc, work, stem):
    built = z.build_generated(tc, z.image_spec_text(image_doc["images"]), work, stem=stem)
    if built["error"]:
        raise AssertionError(str(built["error"])[:600])
    return built["exe"]


def random_schedule(rng, horizon, max_steps, allow_irq):
    """Absolute deadlines (ascending with occasional repeats / already-passed values) and INT/NMI flags per step."""
    n = rng.randint(1, max_steps)
    ds, cur = [], 0
    for _ in range(n):
        roll = rng.random()
        if roll < 0.12 and ds:
            d = rng.randint(0, cur)              # a deadline already reached: no work may be done
        else:
            d = cur + rng.randint(1, 40)
        d = min(d, horizon)
        ds.append(d)
        cur = max(cur, d)
    flags = [(int(allow_irq and rng.random() < 0.25), int(allow_irq and rng.random() < 0.12)) for _ in ds]
    return ds, flags


def split_vs_reference(name, tc, image_doc, state, work, rng):
    """Property: every stop of a random absolute-deadline schedule (deadlines may already be reached, repeated or far
    apart) is an instruction boundary of the single-stepped reference run, at the FIRST boundary at/after the deadline,
    with exactly the reference's state. Interrupts are not raised here (the prefix-lock property below and the
    scenario fixtures cover interrupts between splits)."""
    exe = build_image(tc, image_doc, work / name, "z80_" + name)
    ref = run_text(exe, [mk_vec(name + "_ref", "img", state, [step("i", 1)] * 64, load=[(1, "0000")])], image_doc,
                   work / name)[name + "_ref"]
    bounds, total = {0: None}, 0
    for s_ in ref:
        total += s_["t"]
        bounds[total] = s_
    starts = sorted(bounds)
    horizon = total
    vecs, plans = [], []
    for k in range(SCHEDULES_PER_PROGRAM):
        ds, _ = random_schedule(rng, horizon, 12, allow_irq=False)
        plans.append(ds)
        vecs.append(mk_vec("%s_split%d" % (name, k), "img", state, [step("a", d) for d in ds], load=[(1, "0000")]))
    res = run_text(exe, vecs, image_doc, work / name)
    checked = 0
    for k, ds in enumerate(plans):
        end = 0
        for j, s_ in enumerate(res["%s_split%d" % (name, k)]):
            check(s_["out"] in RESUMABLE, "%s schedule %d step %d: %s" % (name, k, j, s_["out"]))
            end += s_["t"]
            check(end in bounds, "%s schedule %d step %d stopped at cycle %d, not an instruction boundary" % (name, k, j, end))
            want = min([b_ for b_ in starts if b_ >= ds[j] and b_ >= end - s_["t"]], default=None)
            if want is not None:
                check(end == want, "%s schedule %d step %d: stopped at %d, first boundary >= deadline %d is %d" % (
                    name, k, j, end, ds[j], want))
            if end in bounds and bounds[end] is not None:
                check(s_["cpu"] == bounds[end]["cpu"], "%s schedule %d step %d: state differs from the reference" % (name, k, j))
            checked += 1
    return len(plans), checked


def straight_vs_split(name, tc, image_doc, state, work, rng, count=40, irq_after=False):
    """Straight-through == split for schedules without interrupts, on the final state, T-states and logs."""
    exe = build_image(tc, image_doc, work / name, "z80_" + name)
    vecs = []
    for k in range(count):
        ds, _ = random_schedule(rng, 900, 14, allow_irq=False)
        ds = sorted(ds)
        end = ds[-1]
        vecs.append(mk_vec("%s_whole%d" % (name, k), "img", state, [step("a", end)], load=[(1, "0000")]))
        vecs.append(mk_vec("%s_split%d" % (name, k), "img", state, [step("a", d) for d in ds], load=[(1, "0000")]))
    res = run_text(exe, vecs, image_doc, work / name)
    for k in range(count):
        whole, split = res["%s_whole%d" % (name, k)], res["%s_split%d" % (name, k)]
        check(z.final_signature(whole)["cpu"] == z.final_signature(split)["cpu"] or
              split[-1]["t"] == 0 and z.final_signature(whole)["cpu"] == split[-1]["cpu"],
              "%s schedule %d: split final state differs from straight-through" % (name, k))
        check(sum(s["t"] for s in whole) == sum(s["t"] for s in split), "%s schedule %d: T-states differ" % (name, k))
        check([w for s in whole for w in s["w"]] == [w for s in split for w in s["w"]], "%s schedule %d: writes differ" % (name, k))
    return count


def lock_property(tc, work, rng):
    """Splits inside a prefix_lock run with INT/NMI raised between them: never accepted, same final state as a straight
    run, every stop after entry is prefix_lock with in_prefix_run set; an entry with in_prefix_run clear is interruptible."""
    image_doc = lock_image()
    exe = build_image(tc, image_doc, work / "lock", "z80_lock")
    state = {"pc": 0xFFF0, "sp": 0x8000, "im": 1, "iff1": 1, "iff2": 1}
    vecs, plans = [], []
    for k in range(SCHEDULES_PER_PROGRAM):
        n = rng.randint(2, 12)
        ds, cur = [], 0
        for j in range(n):
            cur += rng.randint(1, 30) if j else rng.randint(4, 30)
            ds.append(cur)
        flags = [(0, 0)] + [(int(rng.random() < 0.6), int(rng.random() < 0.5)) for _ in ds[1:]]
        vecs.append(mk_vec("lock_whole%d" % k, "img", state, [step("a", ds[-1])], load=[(1, "0000")]))
        vecs.append(mk_vec("lock_split%d" % k, "img", state, [step("a", d, i, nm) for d, (i, nm) in zip(ds, flags)],
                           load=[(1, "0000")]))
        plans.append(ds)
    res = run_text(exe, vecs, image_doc, work / "lock")
    for k, ds in enumerate(plans):
        whole, split = res["lock_whole%d" % k], res["lock_split%d" % k]
        check(z.final_signature(whole)["cpu"] == z.final_signature(split)["cpu"], "lock schedule %d: final state differs" % k)
        for j, s in enumerate(split):
            check(s["out"] == "prefix_lock", "lock schedule %d step %d: outcome %s" % (k, j, s["out"]))
            check(s["cpu"]["prefix_run"] == "1" and s["cpu"]["sp"] == "8000" and s["cpu"]["iff1"] == "1" and not s["w"],
                  "lock schedule %d step %d: an interrupt was accepted inside the run" % (k, j))
        check(sum(s["t"] for s in split) == sum(s["t"] for s in whole), "lock schedule %d: T-states differ" % k)
    # entry into a run with in_prefix_run clear is an ordinary interruptible boundary (INT then NMI)
    entries = [mk_vec("lock_entry_int", "img", state, [step("a", 13, 1, 0)], load=[(1, "0000")]),
               mk_vec("lock_entry_nmi", "img", state, [step("a", 11, 0, 1)], load=[(1, "0000")])]
    er = run_text(exe, entries, image_doc, work / "lock")
    check(er["lock_entry_int"][0]["out"] == "deadline" and er["lock_entry_int"][0]["cpu"]["pc"] == "0038", "INT at lock entry")
    check(er["lock_entry_nmi"][0]["out"] == "deadline" and er["lock_entry_nmi"][0]["cpu"]["pc"] == "0066", "NMI at lock entry")
    # resume with in_prefix_run explicitly set skips acceptance even when the CPU state is injected
    inj = mk_vec("lock_injected", "img", dict(state, prefix_run=1), [step("a", 40, 1, 1)], load=[(1, "0000")])
    ir = run_text(exe, [inj], image_doc, work / "lock")["lock_injected"][0]
    check(ir["out"] == "prefix_lock" and ir["cpu"]["sp"] == "8000", "resume with in-prefix-run set must not accept")
    return len(plans)


def property_test(tc, work):
    rng = random.Random(0x5EC0008)
    total = 0
    base = {"pc": 0x0400, "sp": 0x8000, "im": 1, "iff1": 1, "iff2": 1}
    di = dict(base, iff1=0, iff2=0)
    wrap = {"pc": 0xFFFE, "sp": 0x8000, "im": 1, "iff1": 1, "iff2": 1}
    for name, image, state in (("prog", program_image(), base), ("wrap", wrap_image(), wrap)):
        rounds, checked = split_vs_reference(name, tc, image, state, work, rng)
        total += checked
        check(rounds == SCHEDULES_PER_PROGRAM, "%s: schedules run" % name)
    for name, image, state in (("prog_di", program_image(), di), ("wrap_di", wrap_image(), dict(wrap, iff1=0, iff2=0))):
        total += straight_vs_split(name, tc, image, state, work, rng)
    total += lock_property(tc, work, rng)
    return total


# ---------------------------------------------------------------------------------------------------------- main
def main():
    root = z.oracle_checkout()
    tc = z.Toolchain(CC, EMITTER, root)
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        n_table = table_test(tc, work, root)
        n_scen = scenario_test(tc, work, root)
        n_prop = property_test(tc, work)
    if root is None:
        print("oracle comparison skipped: " + z.skip_reason())
    if FAILED:
        print("\n".join(FAILED[:30]))
        print("... %d failures" % len(FAILED))
        return 1
    print("z80 timing closure: %d table vectors, %d scenarios, %d property checks: ok" % (n_table, n_scen, n_prop))
    return 0


if __name__ == "__main__":
    sys.exit(main())
