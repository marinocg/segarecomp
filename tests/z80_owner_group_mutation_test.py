#!/usr/bin/env python3
"""SEG-033-T009: the grouped-owner gates catch deliberate mutations (extended tier).

usage: z80_owner_group_mutation_test.py <z80_image_emitter> <cc> <product-root> <segarecomp-cli> <sms_image_emitter>

Each mutant is an emitter defect injected into the generated C of a real emission (a text edit of the owner, entry-table or
dispatcher units), compiled and run on the same vectors as the differential gate; it is KILLED when some vector result differs
from the unmutated emission. A mutant that compiles and survives, or that never changes the text, fails the test (it would mean
the gate cannot see that defect). The mutants:

  wrong_window_key          window-relative owner selects by the absolute PC instead of the window offset
  wrong_entry_selected      two entries of a group swap their case labels
  foreign_address_accepted  an owner also accepts the neighbouring address (selector masked)
  cross_image_confusion     two images' group owners are swapped in the entry table
  direct_branch_wrong_entry an in-group fall-through jumps to another entry's label instead of its own successor
  drop_set_pc               a direct binding into another grouped owner leaves a stale PC (found by the independent validator)
  missing_exact_entry       rows of the exact entry table are removed
  nearby_entry_fallback     the exact lookup returns the next key's owner when the key is absent
  stale_image_after_remap   the dispatcher keeps the code image of the first lookup (a bank switch keeps running the old image)
  fail_closed_bypassed      typed fail-closed outcomes are silenced
  wrong_shared_body         two shared effect bodies are swapped

and the build-side controls: an emission with a group bound above 128, and the reference (one function per start) emission,
violate the shape budget; the same generated C gives identical results at -O0, -O1 and -O2; emission is deterministic and the
comparison notices a changed byte; and a compiler worker failure is never hidden by the build route (POSIX).
"""
import hashlib
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import sms_build_shape as shape_gate  # noqa: E402
import z80_conformance as z  # noqa: E402
from z80_random_programs import random_documents  # noqa: E402

EMITTER, CC, CLI, SMS_EMITTER = sys.argv[1], sys.argv[2], sys.argv[4], sys.argv[5]
FAILED = []
REPORT = []
# Equivalent by design: a lookup that wrongly returns a neighbouring group still fails closed because the group's own selector does
# not own the address (`no_owner`). The stacked mutant that also removes that second defence must be killed.
MASKED = {"nearby_entry_fallback"}
BATCHES = ("sms_map", "t007_prog", "two_image", "reloc", "rnd_sms", "group_cross")


def check(cond, message):
    if not cond:
        FAILED.append(message)


def doc_batches():
    batches = {b.name: b for b in z.scenario_batches(z.load_scenarios())}
    batches.update({b.name: b for b in z.scenario_batches(random_documents())})
    return {name: batches[name] for name in BATCHES}


def owner_files(directory):
    return sorted(p for p in directory.iterdir() if p.suffix == ".c" and "_owner_" in p.name)


def table_files(directory):
    return sorted(p for p in directory.iterdir() if p.suffix == ".c" and ("_entry_" in p.name or p.name.endswith("_main.c")))


def edit(path, fn):
    text = path.read_text()
    new = fn(text)
    if new != text:
        path.write_text(new)
        return True
    return False


# ---- mutators: (directory) -> bool (did the text change) ---------------------------------------------------------------
def wrong_window_key(d):
    return any([edit(p, lambda t: t.replace("switch ((uint16_t)(s->pc - window_base))", "switch (s->pc)")) for p in owner_files(d)])


def wrong_entry_selected(d):
    """Every group entry is selected by the key of its neighbour (case 0x0 <-> 0x1, 0x2 <-> 0x3, ...): a dispatch landing runs the wrong body."""
    def swap(t):
        return re.sub(r"^    case 0x([0-9A-F]{4})u:$", lambda m: "    case 0x%04Xu:" % (int(m.group(1), 16) ^ 1), t, flags=re.M)
    return any([edit(p, swap) for p in owner_files(d)])


def foreign_address_accepted(d):
    return any([edit(p, lambda t: t.replace("switch (s->pc) {", "switch ((uint16_t)(s->pc & 0xFFFEu)) {")) for p in owner_files(d)])


def cross_image_confusion(d):
    changed = False
    for p in table_files(d):
        text = p.read_text()
        if "static const Z80Owner z80_entry_owners" not in text:
            continue
        heads = sorted(set(re.findall(r"\bz80_o_([0-9A-F]{4})_([0-9A-F]{4})\b", text)))
        by_image = {}
        for image, key in heads:
            by_image.setdefault(image, key)
        images = sorted(by_image)
        if len(images) < 2:
            continue
        a, b = "z80_o_%s_%s" % (images[0], by_image[images[0]]), "z80_o_%s_%s" % (images[1], by_image[images[1]])
        changed |= edit(p, lambda t: re.sub(re.escape(a) + r"|" + re.escape(b), lambda m: b if m.group(0) == a else a, t))
    return changed


def direct_branch_wrong_entry(d):
    """An in-group fall-through jumps to another entry of the same owner instead of its successor."""
    def retarget(t):
        parts = re.split(r"(?=^/\* image )", t, flags=re.M)
        for i, part in enumerate(parts):
            labels = re.findall(r"^(z80_e_[0-9A-F]{4}):$", part, re.M)
            gotos = re.findall(r"goto (z80_e_[0-9A-F]{4});", part)
            if len(set(labels)) >= 2 and gotos:
                wrong = next(label for label in labels if label != gotos[0])
                # `if (0) goto <old>` keeps the old target label referenced (-Werror -Wunused-label) without changing behaviour
                parts[i] = part.replace("goto %s;" % gotos[0], "goto %s;" % wrong, 1).replace("  switch (", "  if (0) goto %s;\n  switch (" % gotos[0], 1)
        return "".join(parts)
    return any([edit(p, retarget) for p in owner_files(d)])


def missing_exact_entry(d):
    changed = False
    for p in table_files(d):
        text = p.read_text()
        for m in re.finditer(r"static const uint32_t z80_entry_keys(\w*)\[\] = \{\n(.*?\n)\};\n.*?static const uint\d+_t z80_entry_owner_ids\1\[\] = \{\n(.*?\n)\};", text, re.S):
            keys, ids = m.group(2).splitlines(True), m.group(3).splitlines(True)
            if len(keys) != len(ids) or len(keys) < 40:
                continue
            drop = {i for i in range(len(keys)) if i % 53 == 17}
            new_keys = "".join(k for i, k in enumerate(keys) if i not in drop)
            new_ids = "".join(k for i, k in enumerate(ids) if i not in drop)
            text = text.replace(m.group(2), new_keys, 1).replace(m.group(3), new_ids, 1)
            changed = True
        if changed:
            p.write_text(text)
    return changed


def nearby_entry_fallback(d):
    pattern = re.compile(r"if \(low < (sizeof\(\w+\) / sizeof\(\w+\[0\]\)) && \w+\[low\] == address\) return")
    return any([edit(p, lambda t: pattern.sub(lambda m: "if (low >= %s) low = %s - 1U; if (1) return" % (m.group(1), m.group(1)), t))
                for p in table_files(d)])


def lenient_group_default(d):
    """A group no longer fails closed on a selector it does not own: it re-enters at its first entry instead."""
    def lenient(t):
        parts = re.split(r"(?=^/\* image )", t, flags=re.M)
        for i, part in enumerate(parts):
            first = re.search(r"^    case 0x([0-9A-F]{4})u:$", part, re.M)
            if first is None or "default:\n    {\n      rt->outcome = Z80_ERROR_NO_OWNER;\n      return Z80_OWNER_STOP;\n    }" not in part:
                continue
            relative = "switch ((uint16_t)(s->pc - window_base))" in part
            pc = "(uint16_t)(window_base + 0x%su)" % first.group(1) if relative else "0x%su" % first.group(1)
            part = part.replace("  switch (", "z80_again:\n  switch (", 1)
            parts[i] = part.replace("default:\n    {\n      rt->outcome = Z80_ERROR_NO_OWNER;\n      return Z80_OWNER_STOP;\n    }",
                                    "default:\n    {\n      s->pc = %s;\n      goto z80_again;\n    }" % pc)
        return "".join(parts)
    return any([edit(p, lenient) for p in owner_files(d)])


def nearby_fallback_with_lenient_owner(d):
    return nearby_entry_fallback(d) & lenient_group_default(d)


def drop_set_pc(d):
    """A direct binding into another grouped owner no longer stores the successor PC: the target selects its entry from a stale PC."""
    return any([edit(p, lambda t: re.sub(r"s->pc = [^;\n]+;\n(\s*return Z80_OWNER_NEXT\()", r"\1", t)) for p in owner_files(d)])


def stale_image_after_remap(d):
    def stale(t):
        t = t.replace("    Z80CodeImage image;\n", "    static Z80CodeImage image; static int have;\n")
        return t.replace("    if (!rt->host.code_image(rt->host.context, s->pc, &image)) return rt->outcome = Z80_ERROR_MUTABLE_CODE;",
                         "    if (!have && !rt->host.code_image(rt->host.context, s->pc, &image)) return rt->outcome = Z80_ERROR_MUTABLE_CODE;\n    have = 1;")
    return edit(next(p for p in d.iterdir() if p.name.endswith("_main.c")), stale)


def fail_closed_bypassed(d):
    return any([edit(p, lambda t: re.sub(r"rt->outcome = Z80_ERROR_(?:UNRESOLVED_FETCH_MAPPING|MUTABLE_CODE|EXCLUDED_FORM);", "/* silenced */", t))
                for p in owner_files(d)])


def wrong_shared_body(d):
    bodies = sorted(p for p in d.iterdir() if "_body_" in p.name)
    for p in bodies:
        text = p.read_text()
        defs = re.findall(r"^void (z80_fb_\d+)\(struct Z80Runtime \*rt\) \{$", text, re.M)
        if len(defs) >= 2:
            a, b = defs[0], defs[1]
            return edit(p, lambda t: re.sub(r"\b%s\b|\b%s\b" % (a, b), lambda m: b if m.group(0) == a else a, t))
    return False


MUTANTS = {"wrong_window_key": wrong_window_key, "wrong_entry_selected": wrong_entry_selected,
           "foreign_address_accepted": foreign_address_accepted, "cross_image_confusion": cross_image_confusion,
           "direct_branch_wrong_entry": direct_branch_wrong_entry, "missing_exact_entry": missing_exact_entry,
           "nearby_entry_fallback": nearby_entry_fallback, "nearby_fallback_lenient_owner": nearby_fallback_with_lenient_owner,
           "drop_set_pc": drop_set_pc, "stale_image_after_remap": stale_image_after_remap,
           "fail_closed_bypassed": fail_closed_bypassed, "wrong_shared_body": wrong_shared_body}


def build_and_run(tc, batch, directory, mutate=None):
    """Emits `batch`, applies `mutate` (returns whether the text changed), compiles and runs. -> (results|None, changed, error)"""
    spec = batch.spec_text
    stats, _, error = z.emit_image(tc, spec, directory, "m")
    if error:
        return None, False, "emit: " + error
    changed = mutate(directory) if mutate else False
    exe, error = z.compile_units(tc, directory, "m", extra_sources=[z.TOOLS_DIR / "z80_conformance_runner.c"])
    if error:
        return None, changed, "compile: " + error[:200]
    try:
        return z.run_exe(exe, batch.text, directory, timeout=90), changed, None
    except subprocess.TimeoutExpired:  # a mutant that hangs the dispatcher is as visible as a wrong result (the real runs take seconds)
        return "HANG", changed, None
    except AssertionError as failure:  # the harness executable crashed or exited non-zero
        return "CRASH: " + str(failure)[:80], changed, None


def main():
    tc = z.Toolchain(CC, EMITTER, None)
    batches = doc_batches()
    with tempfile.TemporaryDirectory(prefix="z80-mutants-") as tmp:
        work = pathlib.Path(tmp)
        baseline = {}
        for name, batch in batches.items():
            baseline[name], _, error = build_and_run(tc, batch, work / "base" / name)
            check(error is None, "baseline %s: %s" % (name, error))
        if FAILED:
            return finish()

        for mutant, mutate in MUTANTS.items():
            touched, killed, uncompiled = False, [], []
            for name, batch in batches.items():
                results, changed, error = build_and_run(tc, batch, work / mutant / name, mutate)
                touched |= changed
                if not changed:
                    continue
                if error:
                    uncompiled.append(name)
                    continue
                if results != baseline[name]:
                    killed.append(name)
            verdict = ("killed by " + ",".join(killed)) if killed else ("masked by the owner's own fail-closed default" if mutant in MASKED else "SURVIVED")
            REPORT.append("%-30s %s" % (mutant, verdict))
            check(touched, "mutant %s never changed the generated C (the mutation no longer applies)" % mutant)
            check(not uncompiled, "mutant %s did not compile on %s (a compile break is not a behavioural kill)" % (mutant, uncompiled))
            if mutant in MASKED:
                check(not killed, "mutant %s was expected to be masked by the owner's fail-closed default but changed behaviour on %s" % (mutant, killed))
                check(any(o["out"] == "no_owner" for steps in baseline["two_image"].values() for o in steps),
                      "the masked path (a typed no_owner stop) is not exercised by the baseline")
            else:
                check(bool(killed) or not touched, "mutant %s SURVIVED every vector" % mutant)

        shape_controls(work, tc)
        opt_levels(work, batches, baseline)
        determinism(work, tc, batches)
        worker_failure(work)
    return finish()


def shape_controls(work, tc):
    """The build-shape gate flags the defect SEG-033 fixed (one function per start) and an over-bound group."""
    rng = __import__("random").Random(512)
    sys.path.insert(0, str(ROOT / "tools"))
    import sms_fixture_rom as builder  # noqa: E402
    rom = bytearray(rng.randrange(256) for _ in range(0x20000))
    builder.write_header(rom, 0x20000)
    (work / "big.sms").write_bytes(bytes(rom))
    budget = {"entries": 1024 + 8 * 0x4000, "owners": 1100, "functions": 20000, "max_tu_mib": 4.0, "total_mib": 64.0}
    for label, args, expect_violation in (("default", [], False), ("reference", ["--owner-group", "1", "--share-bodies", "0"], True),
                                          ("group129", ["--owner-group", "129"], True), ("no_sharing", ["--share-bodies", "0"], False)):
        out = work / ("shape_" + label)
        run = subprocess.run([SMS_EMITTER, str(work / "big.sms"), str(out), "sms", "--mapper", "sega", *args], capture_output=True, text=True)
        check(run.returncode == 0, "shape control %s failed to emit" % label)
        if run.returncode != 0:
            continue
        shape = {k: int(v) for k, v in (kv.split("=") for kv in next(l for l in run.stdout.splitlines() if l.startswith("shape ")).split()[1:])}
        files = [p for p in out.iterdir() if p.suffix == ".c"]
        defined = sum(len(re.findall(r"^(?:static )?struct Z80OwnerRef z80_o_\w+\(struct Z80Runtime \*rt, uint16_t window_base\) \{$", p.read_text(), re.M))
                      for p in files)
        found = shape_gate.violations(shape, budget, max(p.stat().st_size for p in files) / 2**20,
                                      sum(p.stat().st_size for p in out.iterdir()) / 2**20, defined)
        if label == "no_sharing":  # grouping alone still keeps the shape; only the byte budget may notice the missing sharing
            found = [f for f in found if "generated C" not in f]
        check(bool(found) == expect_violation, "shape gate verdict for the %s emission is %s (violations: %s)" % (label, bool(found), found))
        REPORT.append("shape control %-10s %s" % (label, "violates " + str(len(found)) + " budget(s)" if found else "inside the budget"))


def opt_levels(work, batches, baseline):
    """The same generated C behaves identically at -O1 and -O2 (and -O0, the baseline)."""
    for opt in ("-O1", "-O2"):
        tc = z.Toolchain(CC, EMITTER, None, opt=opt)
        for name in ("sms_map", "rnd_sms", "t007_prog"):
            results, _, error = build_and_run(tc, batches[name], work / ("opt" + opt) / name)
            check(error is None and results == baseline[name], "%s differs from -O0 at %s: %s" % (name, opt, error))
        REPORT.append("generated C at %s: identical to -O0 on 3 batches" % opt)


def determinism(work, tc, batches):
    spec = batches["sms_map"].spec_text
    for name in ("a", "b"):
        z.emit_image(tc, spec, work / ("det_" + name), "m")

    def tree(directory):
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(directory.iterdir()) if p.suffix in (".c", ".h", ".units")}
    check(tree(work / "det_a") == tree(work / "det_b"), "two emissions of one image differ")
    victim = next(p for p in (work / "det_b").iterdir() if p.suffix == ".c")
    victim.write_text(victim.read_text() + "/* perturbed */\n")
    check(tree(work / "det_a") != tree(work / "det_b"), "the determinism comparison does not notice a changed byte")
    REPORT.append("determinism: byte-identical across emissions; a perturbed byte is noticed")


def worker_failure(work):
    """A compiler worker failing on one unit must fail the build (non-zero exit, compile stage, no executable)."""
    if sys.platform == "win32":
        return
    sms_emitter_rom = work / "big.sms"
    wrapper = work / "failing_cc.sh"
    wrapper.write_text("#!/bin/sh\ncase \"$*\" in *sms_owner_03.c*) echo 'injected worker failure' >&2; exit 1;; esac\nexec %s \"$@\"\n" % CC)
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IEXEC)
    out = work / "worker_fail"
    run = subprocess.run([CLI, "build", "--rom", str(sms_emitter_rom), "--output", str(out), "--cc", str(wrapper), "--mapper", "sega",
                          "--optimize", "0", "--runtime-dir", str(ROOT / "platforms" / "master-system")], capture_output=True, text=True, timeout=900)
    check(run.returncode != 0 and "@result failed stage=compile" in run.stdout, "a failing compiler worker was hidden by the build route: exit %d" % run.returncode)
    check(not (out / "game").exists(), "an executable exists after a compiler worker failure")
    REPORT.append("worker failure: build exits %d at the compile stage, no executable" % run.returncode)


def finish():
    print("\n".join(REPORT))
    if FAILED:
        print("\n".join("FAIL: " + m for m in FAILED))
        return 1
    print("grouped-owner mutation gate: all mutants killed, controls behave")
    return 0


if __name__ == "__main__":
    sys.exit(main())
