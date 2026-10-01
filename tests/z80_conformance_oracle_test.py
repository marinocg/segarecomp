#!/usr/bin/env python3
"""SEG-008-T003: generated-native Z80 execution against the pinned redcode/Z80 oracle (ADR 0057).

usage: z80_conformance_oracle_test.py <z80_image_emitter> <cc> <product-root>

Skips cleanly (prints `skipped:` and exits 0) unless SEGARECOMP_Z80_ORACLE_CHECKOUT names the directory holding the
pinned redcode_Z80 and redcode_Zeta clones; a wrong or dirty pin is a hard failure (Musashi convention, ADR 0057).
With the oracle it
  * compares every table row (full state, ordered write log, ordered I/O log, T-states) and every oracle-flagged
    scenario (interrupts, HALT, deadline, prefix lock, 0xFFFF fetch wrap, two-image dispatch, window-relative owners);
  * proves the harness catches real faults: five mutants of the emitted C / ABI header (R, T-states, a flag, a memory
    write, an interrupt-acknowledge transaction) are each detected with the exact first divergence.
"""
import json
import pathlib
import shutil
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


def compare_batches(tc, batches, work, label):
    report = z.run_batches(tc, batches, work, with_oracle=True)
    total = 0
    for batch in batches:
        r = report[batch.name]
        check(not r.get("error"), "%s %s: %s" % (label, batch.name, str(r.get("error"))[:500]))
        if r.get("error"):
            continue
        order = [v.name for v, _ in batch.vectors if v.oracle]
        total += len(order)
        d = z.first_divergence(r["generated"], r["oracle"], order)
        check(d is None, "%s %s diverges from the oracle: %s" % (label, batch.name, json.dumps(d)[:600]))
        for vec, _ in batch.vectors:
            if vec.oracle:
                check(len(r["oracle"].get(vec.name, [])) == len(vec.steps), "oracle produced no result for " + vec.name)
    return total


def mutant_vectors():
    zero = z.profile_state("mixed", "mutant")
    zero.update(sp=0x8000, im=1, iff1=1, iff2=1, deferral=0, ldair=0, halted=0, prefix_run=0, nmireject=0)
    nop = z.Vec("mutant/nop", b"\x00", zero, [{"mode": "i", "budget": 1, "int": 0, "nmi": 0, "map": 0}])
    irq = z.Vec("mutant/int", b"\x00", zero, [{"mode": "i", "budget": 1, "int": 1, "nmi": 0, "map": 0}])
    return z.slot_batches([nop, irq])[0]


def mutated_run(tc, oracle_exe, work, name, owner_edit=None, header_edit=None):
    batch = mutant_vectors()
    bdir = work / name
    stats, _, error = z.emit_image(tc, batch.spec_text, bdir, "mut")
    check(error is None, "%s: emit failed" % name)
    mtc = tc
    if header_edit:
        inc = bdir / "inc" / "segarecomp" / "codegen" / "c11" / "runtime"
        inc.mkdir(parents=True)
        text = (z.INCLUDE_DIR / "segarecomp" / "codegen" / "c11" / "runtime" / "z80_runtime.h").read_text()
        edited = header_edit(text)
        check(edited != text, "%s: header mutation did not apply" % name)
        (inc / "z80_runtime.h").write_text(edited)
        mtc = z.Toolchain(tc.cc, tc.emitter, tc.oracle_checkout, include_dir=bdir / "inc")
    if owner_edit:
        applied = False
        # SEG-033: instruction effects (R, T-states, Q) live in the shared-body units, entries and prologues in the owner units
        for path in [*bdir.glob("*_owner_*.c"), *bdir.glob("*_body_*.c")]:
            text = path.read_text()
            edited = owner_edit(text)
            applied = applied or edited != text
            path.write_text(edited)
        check(applied, "%s: owner mutation did not apply" % name)
    exe, error = z.compile_units(mtc, bdir, "mut", extra_sources=[z.TOOLS_DIR / "z80_conformance_runner.c"])
    check(exe is not None, "%s: mutant does not compile: %s" % (name, str(error)[:300]))
    if exe is None:
        return None
    generated = z.run_exe(exe, batch.text, bdir)
    oracle = z.run_exe(oracle_exe, batch.text, bdir)
    return z.first_divergence(generated, oracle, [v.name for v, _ in batch.vectors])


def main():
    root = z.oracle_checkout()
    if root is None:
        print("skipped: " + z.skip_reason())
        return 0
    tc = z.Toolchain(CC, EMITTER, root)
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        by_form = z.form_vectors()
        rows = [v for f in sorted(by_form) for v in by_form[f]]
        total = compare_batches(tc, z.slot_batches(rows), work / "rows", "rows")
        total += compare_batches(tc, z.scenario_batches(z.load_scenarios()), work / "scenarios", "scenarios")

        oracle_exe = z.build_oracle(tc, work)
        control = mutated_run(tc, oracle_exe, work, "control")
        check(control is None, "control (unmutated) run diverges: %s" % control)
        cases = [
            ("R", dict(owner_edit=lambda t: t.replace("z80_r_add(s->r, 1u)", "z80_r_add(s->r, 2u)")),
             ("mutant/nop", 0, "cpu", ["r"])),
            ("T-states", dict(owner_edit=lambda t: t.replace("s->cycles += 4u;", "s->cycles += 5u;")),
             ("mutant/nop", 0, "timing", ["t_states"])),
            ("flag", dict(owner_edit=lambda t: t.replace("s->q = 0u;", "s->q = 0u;\n  s->f ^= 0x01u;")),
             ("mutant/nop", 0, "cpu", ["f"])),
            ("memory write", dict(header_edit=lambda t: t.replace("z80_write(rt, s->sp, (uint8_t)value);", "z80_write(rt, s->sp, (uint8_t)(value ^ 1u));")),
             ("mutant/int", 0, "memory", ["writes"])),
            ("acknowledge transaction", dict(header_edit=lambda t: t.replace(
                "const uint8_t byte = rt->host.interrupt_acknowledge(rt->host.context, s->cycles);", "const uint8_t byte = 0xFFu;")),
             ("mutant/int", 0, "io", ["io"])),
        ]
        for label, edits, (vector, step, domain, fields) in cases:
            d = mutated_run(tc, oracle_exe, work, label.replace(" ", "_"), **edits)
            check(d is not None, "harness missed the injected %s fault" % label)
            if d:
                check((d["vector"], d["step"], d["domain"], [f["field"] for f in d["fields"]]) == (vector, step, domain, fields),
                      "%s fault: reported %s" % (label, json.dumps(d)[:300]))
    if FAILED:
        print("\n".join("FAIL: " + m for m in FAILED[:30]))
        return 1
    print("z80 oracle conformance: ok (%d vectors, 5 injected faults detected)" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
