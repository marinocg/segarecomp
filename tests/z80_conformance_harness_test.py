#!/usr/bin/env python3
"""SEG-008-T003: Z80 differential-harness self-tests (hermetic: no ROM, no oracle).

usage: z80_conformance_harness_test.py <z80_image_emitter> <cc> <product-root>

  * table rows expand deterministically to unique vectors owned by their family table;
  * the generated side of every seed row agrees with an independent Python model derived only from the T001 dataset
    (T-states from the dataset timing, R from its M1 count, PC from its length, register effects from the opcode fields);
  * the first-divergence comparator reports the exact vector, step, domain and field, and catches an injected fault
    in a flag, a memory write (value, address, order, count), an I/O transaction, R, PC and the T-state count;
  * the validation manifests are fresh (digest-attributed) and stale or unattributed credit is not credit;
    updating a manifest can only add credit.
The oracle side runs in z80_conformance_oracle_test.py (skips without the pinned checkout).
"""
import copy
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


data, forms = z.load_dataset()


def expected_model(vec, pc):
    """Independent model of the seed forms from the dataset alone. Returns the expected step-0 result."""
    form = forms[vec.form]
    prefix_count = len(vec.code) - form_len(form)
    timing = form["timing"]
    t = (timing["t_states"] if "t_states" in timing else timing["not_taken"]) + 4 * prefix_count
    exp = {"t": t, "m1": form["m1_fetches"] + prefix_count, "length": len(vec.code)}
    state = dict(vec.state)
    op = vec.code[prefix_count + form["layout"].split().index("op")]
    regs = {0: "b", 1: "c", 2: "d", 3: "e", 4: "h", 5: "l", 7: "a"}
    if form["mnemonic"] == "LD" and form["dst"] == "r" and form["src"] == "r":
        state[regs[(op >> 3) & 7]] = state[regs[op & 7]]
    elif form["mnemonic"] == "LD" and form["dst"] == "r" and form["src"] == "n":
        state[regs[(op >> 3) & 7]] = vec.code[-1]
    exp["state"] = state
    return exp


def form_len(form):
    return form["length"]


def seed_rows_against_model(tc, work):
    by_form = z.form_vectors()
    vecs = [v for f in sorted(by_form) for v in by_form[f]]
    batches = z.slot_batches(vecs)
    report = z.run_batches(tc, batches, work / "rows", with_oracle=False)
    total = 0
    for batch in batches:
        r = report[batch.name]
        check(not r.get("error"), "row batch failed: %s" % r.get("error"))
        if r.get("error"):
            continue
        for vec, _ in batch.vectors:
            steps = r["generated"].get(vec.name)
            check(steps is not None and len(steps) == len(vec.steps), "%s: missing steps" % vec.name)
            if not steps:
                continue
            total += 1
            pc = batch.slots[vec.code]
            if not (forms[vec.form]["mnemonic"] in ("NOP", "HALT") or (forms[vec.form]["mnemonic"] == "LD" and
                    forms[vec.form]["dst"] == "r" and forms[vec.form]["src"] in ("r", "n"))):
                continue  # the seed model covers only the T003 seed forms; the other families are oracle-validated
            exp = expected_model(vec, pc)
            first = steps[0]
            form = forms[vec.form]
            if form["mnemonic"] == "HALT":
                check(first["out"] == "halted" and first["cpu"]["halted"] == "1", "%s: HALT is not a resumable halted outcome" % vec.name)
                check(first["t"] == exp["t"], "%s: HALT T-states %d != %d" % (vec.name, first["t"], exp["t"]))
                for later in steps[1:]:  # halted cycles: 4 T and R + 1 each, PC stays after HALT
                    check(later["t"] == 4 and later["out"] == "halted", "%s: halted cycle accounting" % vec.name)
                    check(later["cpu"]["pc"] == "%04X" % (pc + exp["length"]), "%s: PC while halted" % vec.name)
                continue
            check(first["out"] == "deadline", "%s: outcome %s" % (vec.name, first["out"]))
            check(first["t"] == exp["t"], "%s: T-states %d != model %d" % (vec.name, first["t"], exp["t"]))
            check(first["cpu"]["pc"] == "%04X" % (pc + exp["length"]), "%s: PC %s" % (vec.name, first["cpu"]["pc"]))
            r0 = vec.state["r"]
            want_r = (r0 & 0x80) | ((r0 + exp["m1"]) & 0x7F)
            check(int(first["cpu"]["r"], 16) == want_r, "%s: R %s != %02X" % (vec.name, first["cpu"]["r"], want_r))
            for k in ("a", "b", "c", "d", "e", "h", "l", "f", "ix", "iy", "sp", "wz", "a2", "f2", "i", "im", "iff1", "iff2"):
                got = int(first["cpu"][k], 16) if k not in z.DECIMAL else int(first["cpu"][k])
                check(got == exp["state"][k], "%s: %s = %s, model %X" % (vec.name, k, first["cpu"][k], exp["state"][k]))
            check(int(first["cpu"]["q"], 16) == 0, "%s: Q after a non-flag instruction" % vec.name)
            check(first["w"] == [] and first["io"] == [], "%s: unexpected bus events" % vec.name)
    check(total == sum(len(v) for v in by_form.values()), "not every row vector produced a result")
    return total


def rows_are_deterministic():
    first = z.form_vectors()
    second = z.form_vectors()
    check(sorted(first) == sorted(second), "row expansion is not deterministic")
    names = set()
    for form_id, vecs in first.items():
        check([v.digest_material() for v in vecs] == [v.digest_material() for v in second[form_id]], "digest material differs: " + form_id)
        check(z.form_digest(vecs) == z.form_digest(second[form_id]), "digest differs: " + form_id)
        for v in vecs:
            check(v.name not in names, "duplicate vector name " + v.name)
            names.add(v.name)
            check(forms[v.form]["family"] == v.family, "row in the wrong family table: " + v.name)
    return first


# -------------------------------------------------------------------------------- comparator and fault injection
def mini_results(tc, work):
    """Generated results of an IM1 interrupt followed by a NOP: writes (push), an acknowledge transaction, T-states."""
    state = z.profile_state("mixed", "mini")
    state.update(sp=0x8000, im=1, iff1=1, iff2=1, deferral=0, ldair=0, halted=0, prefix_run=0, nmireject=0)
    steps = [{"mode": "i", "budget": 1, "int": 1, "nmi": 0, "map": 0}, {"mode": "i", "budget": 1, "int": 0, "nmi": 0, "map": 0}]
    vec = z.Vec("mini/int", b"\x00", state, steps)
    batch = z.slot_batches([vec])[0]
    report = z.run_batches(tc, [batch], work / "mini", with_oracle=False)
    r = report[batch.name]
    check(not r.get("error"), "mini batch failed: %s" % r.get("error"))
    return r["generated"], vec.name


def fault_injection(results, name):
    good = results[name]
    check(len(good) == 2 and len(good[0]["w"]) == 2 and good[0]["w"][0].startswith("7FFF:") and good[0]["w"][1].startswith("7FFE:"),
          "mini vector does not record the push writes (high byte first)")
    check(len(good[0]["io"]) == 1 and good[0]["io"][0].startswith("A:"), "mini vector does not record the acknowledge")
    check(z.first_divergence(results, results, [name]) is None, "control: identical streams diverge")

    def mutate(step_index, edit):
        bad = copy.deepcopy(results)
        edit(bad[name][step_index])
        return z.first_divergence(bad, results, [name])

    def flip(field, mask):
        def edit(s):
            s["cpu"][field] = "%0*X" % (len(s["cpu"][field]), int(s["cpu"][field], 16) ^ mask)
        return edit

    def expect(label, report, step, domain, fields):
        check(report is not None, "fault not detected: " + label)
        if report is None:
            return
        check(report["vector"] == name and report["step"] == step and report["domain"] == domain,
              "%s: reported %s step %s domain %s" % (label, report["vector"], report["step"], report["domain"]))
        check([f["field"] for f in report["fields"]] == fields, "%s: fields %s" % (label, [f["field"] for f in report["fields"]]))

    expect("flag", mutate(0, flip("f", 0x40)), 0, "cpu", ["f"])
    expect("flag at a later step", mutate(1, flip("f", 0x01)), 1, "cpu", ["f"])
    expect("R", mutate(0, flip("r", 0x01)), 0, "cpu", ["r"])
    expect("PC", mutate(1, flip("pc", 0x0100)), 1, "cpu", ["pc"])
    expect("MEMPTR", mutate(0, flip("wz", 0x0001)), 0, "cpu", ["wz"])
    expect("internal state bit", mutate(0, flip("ldair", 1)), 0, "cpu", ["ldair"])
    expect("memory write value", mutate(0, lambda s: s["w"].__setitem__(0, s["w"][0][:5] + "%02X" % (int(s["w"][0][5:], 16) ^ 1))), 0, "memory", ["writes"])
    expect("memory write address", mutate(0, lambda s: s["w"].__setitem__(1, "7FFC" + s["w"][1][4:])), 0, "memory", ["writes"])
    expect("memory write order", mutate(0, lambda s: s["w"].reverse()), 0, "memory", ["writes"])
    expect("extra memory write", mutate(0, lambda s: s["w"].append("0000:00")), 0, "memory", ["writes"])
    expect("missing memory write", mutate(0, lambda s: s["w"].pop()), 0, "memory", ["writes"])
    expect("I/O value", mutate(0, lambda s: s["io"].__setitem__(0, "A:0000:00")), 0, "io", ["io"])
    expect("I/O transaction dropped", mutate(0, lambda s: s["io"].clear()), 0, "io", ["io"])
    expect("I/O out transaction added", mutate(0, lambda s: s["io"].append("O:0010:AA")), 0, "io", ["io"])
    expect("T-states", mutate(0, lambda s: s.__setitem__("t", s["t"] + 1)), 0, "timing", ["t_states"])
    expect("T-states at a later step", mutate(1, lambda s: s.__setitem__("t", s["t"] - 1)), 1, "timing", ["t_states"])

    # SEG-020 domain order: when several domains differ the first reported one is cpu, then memory, io, timing.
    def many(s):
        flip("f", 1)(s)
        s["w"].pop()
        s["io"].clear()
        s["t"] += 1
    expect("domain order", mutate(0, many), 0, "cpu", ["f"])
    # The first diverging step wins even when a later step also differs; the report is deterministic JSON.
    bad = copy.deepcopy(results)
    flip("a", 1)(bad[name][1])
    flip("b", 1)(bad[name][0])
    first = z.first_divergence(bad, results, [name])
    check(first["step"] == 0 and first["fields"][0]["field"] == "b", "first diverging step is not reported first")
    check(json.dumps(first, sort_keys=True) == json.dumps(z.first_divergence(bad, results, [name]), sort_keys=True), "report is not deterministic")
    # A generated stream that stops early (fail-closed error) or is missing is an outcome divergence.
    short = copy.deepcopy(results)
    short[name] = short[name][:1]
    check(z.first_divergence(short, results, [name])["domain"] == "outcome", "early stop is not reported")
    check(z.first_divergence({}, results, [name])["domain"] == "outcome", "missing generated vector is not reported")
    # Bounded lockstep: the step limit stops the comparison.
    bad = copy.deepcopy(results)
    flip("a", 1)(bad[name][1])
    check(z.first_divergence(bad, results, [name], limit=1) is None, "the step limit is not honoured")


def parse_roundtrip():
    text = ("S v 0 t=13 out=deadline " + " ".join("%s=%s" % (k, "0000" if k in z.PAIR_ORDER else "00" if k not in z.DECIMAL else "0")
            for k in z.CPU_FIELDS) + " w=7FFF:12,7FFE:34 io=A:0000:FF,O:0010:AA\nnot a result line\n")
    parsed = z.parse_results(text)
    check(parsed["v"][0]["w"] == ["7FFF:12", "7FFE:34"] and parsed["v"][0]["io"] == ["A:0000:FF", "O:0010:AA"] and parsed["v"][0]["t"] == 13,
          "result parser")


# -------------------------------------------------------------------------------- manifests
def manifests_are_fresh(by_form):
    manifests = z.load_manifests()
    check(manifests, "no committed validation manifest")
    credit = z.manifest_credit(by_form, manifests)
    for family, manifest in manifests.items():
        check(manifest["oracle"] == z.PINS, "%s manifest is not attributed to the pinned oracle" % family)
        for form_id, entry in manifest["forms"].items():
            check(form_id in credit, "stale or unattributed manifest credit: %s (regenerate with --update-manifest)" % form_id)
            check(set(entry["stages"]) <= {"oracle_state", "oracle_memory", "oracle_io", "timing_validated"}, "stage names: " + form_id)
            check(set(entry["stages"]) == set(z.applicable_oracle_stages(forms[form_id])), "%s credits the wrong applicable stages" % form_id)
    # stale digest and wrong family are not credit
    fam, manifest = next(iter(manifests.items()))
    form_id = next(iter(manifest["forms"]))
    stale = copy.deepcopy({fam: manifest})
    stale[fam]["forms"][form_id]["digest"] = "0" * 64
    check(form_id not in z.manifest_credit(by_form, stale), "a stale digest still earns credit")
    other = next(f for f in z.FAMILIES if f != fam)
    moved = {other: copy.deepcopy(manifest)}
    check(form_id not in z.manifest_credit(by_form, moved), "a manifest in the wrong family still earns credit")
    # only additions are allowed
    try:
        z.write_manifest(fam, {}, {form_id: manifest["forms"][form_id]["digest"]}, manifest)
        check(False, "write_manifest removed credit")
    except SystemExit:
        pass


def main():
    tc = z.Toolchain(CC, EMITTER)
    by_form = rows_are_deterministic()
    check({"nop.base", "ld.r_r.base", "ld.r_n.base", "halt.base"} <= set(by_form), "seed forms have no rows")
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        total = seed_rows_against_model(tc, work)
        results, name = mini_results(tc, work)
        if results:
            fault_injection(results, name)
    parse_roundtrip()
    manifests_are_fresh(by_form)
    if FAILED:
        print("\n".join("FAIL: " + m for m in FAILED[:40]))
        return 1
    print("z80 conformance harness: ok (%d seed-row vectors checked against the dataset model)" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
