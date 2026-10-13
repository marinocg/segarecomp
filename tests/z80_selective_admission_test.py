#!/usr/bin/env python3
"""SEG-057: Z80 selective admission is structurally correct (hermetic; ADR 0118).

usage: z80_selective_admission_test.py <z80_image_emitter> <cc> <product-root> [batch,batch,...|static]

The emitter accepts an optional, explicitly supplied admission plan (a set of instruction starts per code image). This test checks the
STRUCTURAL claim only: the emission contains exactly the admitted starts and rejects every other exposed start with the distinct
`unadmitted_pc` outcome; it does not (and cannot) claim that a plan admits every start a program executes.

The reference is the broad immutable-image AOT (no plan) over the same hermetic scenario corpus and seeded random programs the
grouped-owner differential gate uses (prefix chains and locks, direct branches, calls/returns, bank/window-relative owners, remap,
INT/NMI/IM0-2, HALT, block repeats, deadline resumes, grouped owners with shared effect bodies, direct-bound fall-through):

  * completeness: a plan admitting every exposed offset emits byte-identical owner and body translation units;
  * positive: a plan derived from the broad run (the hull of every visited start plus a margin, closed by the exact PCs the emission
    itself reports as unadmitted) reproduces every step of the broad run (CPU/internal state, outcome, T-states, writes, I/O) and
    is a strict subset of the exposed offsets;
  * negative: omitting visited starts makes each affected vector stop with `unadmitted_pc` at an omitted start. For single-instruction
    steps without interrupts or HALT the stop is exact: `state.pc` is the omitted start, the machine state equals the previous step's,
    and no T-state, write or I/O event happened (the instruction had no effect);
  * the plan contract fails closed (unknown/duplicate/missing image, different bytes, unsorted/duplicate/out-of-range/unexposed offsets)
    leaving nothing behind, and the emission is deterministic.
"""
import filecmp
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402
from z80_random_programs import random_documents  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
# CI wall-clock: the optional fourth argument restricts the run to the named scenario/random batches; the token `static` selects the
# image-independent checks (all-admitted byte identity, determinism, plan contract). CTest registers disjoint slices whose union is
# everything, and a name that matches nothing fails the run. Without it everything runs.
ONLY = set(sys.argv[4].split(",")) if len(sys.argv) > 4 else None
SEEN = set()
FAILED = []
MARGIN = 128
MAX_ROUNDS = 8
TOTALS = {"vectors": 0, "unadmitted_stops": 0, "exact_stops": 0, "admitted": 0, "exposed": 0}


def check(cond, message):
    if not cond:
        FAILED.append(message)


def parse_spec(spec_text):
    """image id -> {kind, windows [(base, first, length)], size}"""
    images = {}
    for line in spec_text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "image":
            images[int(parts[1])] = {"kind": parts[2], "windows": [], "size": 0}
        elif parts[0] == "window":
            images[int(parts[1])]["windows"].append((int(parts[2], 16), int(parts[3], 16), int(parts[4], 16)))
        elif parts[0] == "bytes":
            images[int(parts[1])]["size"] += len(parts[2]) // 2
        elif parts[0] == "fill":
            images[int(parts[1])]["size"] += int(parts[3], 16)
    return images


def exposed_offsets(image):
    if "exposed" not in image:
        out = set()
        for _base, first, length in image["windows"]:
            out.update(range(first, min(first + length, image["size"])))
        image["exposed"] = out
    return image["exposed"]


def locate(vec, images, map_id, pc):
    """(identity, image offset) the emission keys the start at logical `pc` under the vector's map `map_id`, or None."""
    ranges = vec.maps.get(str(map_id)) or [[0, 0x10000, 1, 0]]
    for lo, hi, identity, base in ranges:
        if lo <= pc < hi and identity in images:
            image = images[identity]
            reference = image["windows"][0][0] if image["kind"] == "invariant" or len(image["windows"]) == 1 else base
            offset = (pc - reference) & 0xFFFF
            return (identity, offset) if offset in exposed_offsets(image) else None
    return None


def locate_range(vec, images, map_id, lo, hi):
    """[(identity, set of exposed image offsets)] for the logical addresses lo..hi (inclusive) under the vector's map."""
    out = []
    for r_lo, r_hi, identity, base in vec.maps.get(str(map_id)) or [[0, 0x10000, 1, 0]]:
        a, b = max(lo, r_lo), min(hi, r_hi - 1)
        if a > b or identity not in images:
            continue
        image = images[identity]
        reference = image["windows"][0][0] if image["kind"] == "invariant" or len(image["windows"]) == 1 else base
        start = (a - reference) & 0xFFFF
        out.append((identity, {(start + n) & 0xFFFF for n in range(b - a + 1)} & exposed_offsets(image)))
    return out


def selective_spec(spec_text, admitted):
    """Append a plan: `admitted` is {identity: set(offsets)}. Every image is listed, an unmentioned one admits nothing."""
    lines = [spec_text.rstrip("\n"), "selective 0"]
    for identity, offsets in sorted(admitted.items()):
        run = None
        for offset in sorted(offsets):
            if run is not None and offset == run[1]:
                run[1] += 1
                continue
            if run is not None:
                lines.append("admit %d %X %X" % (identity, run[0], run[1]))
            run = [offset, offset + 1]
        if run is not None:
            lines.append("admit %d %X %X" % (identity, run[0], run[1]))
    return "\n".join(lines) + "\n"


def pcs_of(steps, vec):
    pre = [vec.state["pc"]] + [int(s["cpu"]["pc"], 16) for s in steps[:-1]]
    return pre, int(steps[-1]["cpu"]["pc"], 16) if steps else vec.state["pc"]


def visited(batch, images, reference):
    """Per vector: [(step index, identity, offset)] pre-start of every step, and the hull-derived admitted set."""
    paths, hull = {}, {identity: set() for identity in images}
    for vec, _text in batch.vectors:
        steps = reference.get(vec.name, [])
        if not steps:
            continue
        pre, last = pcs_of(steps, vec)
        path = []
        for k, pc in enumerate(pre):
            where = locate(vec, images, vec.steps[k]["map"], pc)
            if where:
                path.append((k, *where))
        paths[vec.name] = path
        points = pre + [last]
        for k in range(len(steps)):
            low, high = sorted((points[k], points[k + 1]))
            if high - low > 0x8000:  # a wrapped range: keep to the endpoints
                low = high = points[k]
            for identity, offsets in locate_range(vec, images, vec.steps[k]["map"], max(low - MARGIN, 0), min(high + MARGIN, 0xFFFF)):
                hull[identity].update(offsets)
    return paths, hull


def build(tc, spec, directory, stem):
    built = z.build_generated(tc, spec, directory, stem=stem)
    check(not built["error"], "%s: %s" % (stem, (built["error"] or "")[:300]))
    return built


def run_vectors(built, batch, directory):
    return z.run_exe(built["exe"], batch.text, directory)


def differential(label, batches, work, tc):
    for batch in batches:
        if ONLY is not None:
            if batch.name not in ONLY:
                continue
            SEEN.add(batch.name)
        images = parse_spec(batch.spec_text)
        bdir = work / label / batch.name
        broad = build(tc, batch.spec_text, bdir / "broad", "z80_" + batch.name)
        if broad.get("error"):
            return
        reference = run_vectors(broad, batch, bdir / "broad")
        paths, admitted = visited(batch, images, reference)
        exposed = sum(len(exposed_offsets(i)) for i in images.values())
        # ---- positive: the plan derived from the broad run, closed by the emission's own unadmitted reports ----
        results = None
        for round_ in range(MAX_ROUNDS):
            spec = selective_spec(batch.spec_text, admitted)
            built = build(tc, spec, bdir / ("sel%d" % round_), "z80_" + batch.name)
            if built.get("error"):
                return
            results = run_vectors(built, batch, bdir / ("sel%d" % round_))
            grew = False
            for vec, _text in batch.vectors:
                got, want = results.get(vec.name, []), reference.get(vec.name, [])
                for k, (g, w) in enumerate(zip(got, want)):
                    if g == w:
                        continue
                    if g["out"] == "unadmitted_pc":
                        where = locate(vec, images, vec.steps[k]["map"], int(g["cpu"]["pc"], 16))
                        if where and where[1] not in admitted[where[0]]:
                            admitted[where[0]].add(where[1])
                            grew = True
                    break
            if not grew:
                break
        else:
            check(False, "%s/%s: the derived plan did not converge in %d rounds" % (label, batch.name, MAX_ROUNDS))
        for vec, _text in batch.vectors:
            TOTALS["vectors"] += 1
            check(results.get(vec.name) == reference.get(vec.name),
                  "%s/%s: vector %s differs from the broad AOT under its derived plan" % (label, batch.name, vec.name))
        count = sum(len(v) for v in admitted.values())
        TOTALS["admitted"] += count
        TOTALS["exposed"] += exposed
        check(built["stats"]["adm_admitted"] == count and built["stats"]["adm_omitted"] == exposed - count,
              "%s/%s: admitted/omitted statistics do not match the plan" % (label, batch.name))
        check(built["stats"]["shape_entries"] <= count, "%s/%s: more entries than admitted starts" % (label, batch.name))
        # ---- negative: omit visited starts; two disjoint samples of the distinct visited starts ----
        distinct = sorted({(i, o) for path in paths.values() for _k, i, o in path})
        for sample_no in range(2):
            omitted = set(distinct[sample_no::2][:64])
            if not omitted:
                continue
            plan = {i: set(offsets) for i, offsets in admitted.items()}
            for identity, offset in omitted:
                plan[identity].discard(offset)
            built = build(tc, selective_spec(batch.spec_text, plan), bdir / ("neg%d" % sample_no), "z80_" + batch.name)
            if built.get("error"):
                return
            negative = run_vectors(built, batch, bdir / ("neg%d" % sample_no))
            for vec, _text in batch.vectors:
                check_negative(label, batch, vec, images, reference, negative, paths, plan, omitted)


def check_negative(label, batch, vec, images, reference, negative, paths, plan, omitted):
    name, tag = vec.name, "%s/%s %s" % (label, batch.name, vec.name)
    want, got = reference.get(name, []), negative.get(name, [])
    first = next((k for k, (g, w) in enumerate(zip(got, want)) if g != w), None)
    on_path = [k for k, i, o in paths.get(name, []) if (i, o) in omitted]
    if first is None:
        check(len(got) == len(want) or got[-1]["out"] != want[-1]["out"], "%s: a shorter run without a divergence" % tag)
        return
    stop = got[first]
    check(stop["out"] == "unadmitted_pc", "%s: step %d diverges with %r, not unadmitted_pc" % (tag, first, stop["out"]))
    if stop["out"] != "unadmitted_pc":
        return
    TOTALS["unadmitted_stops"] += 1
    pc = int(stop["cpu"]["pc"], 16)
    where = locate(vec, images, vec.steps[first]["map"], pc)
    check(where is not None and where[1] not in plan[where[0]], "%s: the reported pc %04X is not an omitted start" % (tag, pc))
    check(len(got) == first + 1, "%s: the run continued after an unadmitted_pc stop" % tag)
    # An exact stop: single-instruction steps, no interrupt edge or HALT anywhere before it.
    simple = all(s["mode"] == "i" and not s["int"] and not s["nmi"] for s in vec.steps[:first + 1]) and \
        all(s["cpu"]["halted"] == "0" for s in want[:first + 1] if "halted" in s["cpu"])
    if simple and on_path and first == on_path[0]:
        pre, _last = pcs_of(want, vec)
        check(pc == pre[first], "%s: stop pc %04X is not the omitted pre-start %04X" % (tag, pc, pre[first]))
        check(stop["t"] == 0 and stop["w"] == [] and stop["io"] == [], "%s: the rejected start had an effect (t/writes/io)" % tag)
        if first > 0:
            check(stop["cpu"] == got[first - 1]["cpu"], "%s: the rejected start changed the machine state" % tag)
        TOTALS["exact_stops"] += 1


def broad_equivalence(batches, work, tc):
    """Admitting every exposed offset emits byte-identical owner and body units (and only the rejection table is new)."""
    for batch in batches[:4]:
        images = parse_spec(batch.spec_text)
        a, b = work / "eq" / batch.name / "broad", work / "eq" / batch.name / "all"
        z.emit_image(tc, batch.spec_text, a, "eq")
        full = selective_spec(batch.spec_text, {i: exposed_offsets(img) for i, img in images.items()})
        z.emit_image(tc, full, b, "eq")
        names = sorted(p.name for p in a.iterdir() if re.search(r"eq_(owner|body)_\d+\.c$", p.name) or p.name == "eq.h")
        check(names, "%s: no owner translation units to compare" % batch.name)
        for n in names:
            check((b / n).exists() and filecmp.cmp(a / n, b / n, shallow=False),
                  "%s: %s differs between the broad emission and the all-admitted plan" % (batch.name, n))


def contract(work, tc):
    base = "image 1 invariant\nwindow 1 0000 0 400\nfill 1 00 400\nimage 2 banked\nwindow 2 4000 0 100\nwindow 2 8000 80 80\nfill 2 00 100\n"
    cases = {
        "unknown": (base + "selective 0\nadmit_extra_image 9\n", "unknown code-image identity"),
        "duplicate": (base + "selective 0\nadmit_duplicate_image 1\n", "twice"),
        "bad_digest": (base + "selective 0\nadmit_bad_digest 1\n", "different image bytes"),
        "missing_image": (base + "selective 0\nadmit_omit_image 2\n", "does not cover"),
        "out_of_range": (base + "selective 0\nadmit 1 3FF 401\n", "not exposed"),
        "unsorted": (base + "selective 0\nadmit_unsorted 1 10 5\n", "strictly increasing"),
        "duplicate_offset": (base + "selective 0\nadmit_unsorted 1 4 4\n", "strictly increasing"),
    }
    for name, (spec, needle) in cases.items():
        directory = work / "contract" / name
        _stats, _owners, error = z.emit_image(tc, spec, directory, "bad")
        check(error is not None and needle in error, "%s: expected a %r rejection, got %r" % (name, needle, error))
        left = [p.name for p in directory.iterdir() if p.suffix in (".c", ".h", ".units")] if directory.exists() else []
        check(not left, "%s: a rejected plan left files behind: %s" % (name, left))
    unexposed = "image 1 invariant\nwindow 1 0000 100 100\nfill 1 00 400\nselective 0\nadmit 1 0 1\n"
    _s, _o, error = z.emit_image(tc, unexposed, work / "contract" / "unexposed", "bad")
    check(error is not None and "not exposed" in error, "unexposed offset must be rejected, got %r" % error)
    # An admitted start whose form has no lowering row stays `no_owner` semantics (counted, no owner) and a valid plan succeeds.
    fine = base + "selective 0\nadmit 1 0 4\nadmit 2 10 20\nadmit 2 90 95\n"
    stats, _owners, error = z.emit_image(tc, fine, work / "contract" / "fine", "fine")
    check(error is None, "valid plan rejected: %s" % error)
    if stats:
        check(stats["adm_admitted"] == 4 + 16 + 5, "admitted statistic %s" % stats.get("adm_admitted"))
        check(stats["adm_omitted"] == 0x400 - 4 + 0x100 - 16 - 5, "omitted statistic %s" % stats.get("adm_omitted"))
        check(stats["shape_entries"] == 25, "only the admitted starts get an exact entry (%s)" % stats["shape_entries"])


BOUNDARY_C = r'''
#include "sel_main.c"
int main(void) {
  /* image 1 (invariant, base 0): exposed offsets [0x100, 0x200), admitted [0x100, 0x140): the omitted run is [0x140, 0x200). */
  static const struct { uint16_t pc; int omitted; } cases[] = {
    {0x00FF, 0}, {0x0100, 0}, {0x013F, 0}, {0x0140, 1}, {0x0141, 1}, {0x01FF, 1}, {0x0200, 0}, {0x0201, 0}, {0xFFFF, 0}};
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    if (z80_omitted_start(1U, cases[i].pc, 0U) != cases[i].omitted) return (int)(i + 1U);
  if (z80_omitted_start(77U, 0x0140, 0U) != 0) return 20;  /* an unknown identity is never "omitted" */
  return 0;
}
'''


def rejection_boundaries(work, tc):
    """z80_omitted_start is exact at both ends of an omitted run and beside unexposed addresses (those stay `no_owner`)."""
    spec = "image 1 invariant\nwindow 1 0000 100 100\nfill 1 00 400\nselective 0\nadmit 1 100 140\n"
    directory = work / "boundary"
    _s, _o, error = z.emit_image(tc, spec, directory, "sel")
    check(error is None, "boundary emit: %s" % error)
    if error:
        return
    (directory / "harness.c").write_text(BOUNDARY_C)
    sources = [directory / n for n in (directory / "sel.units").read_text().split() if n != "sel_main.c"]
    exe = directory / "harness"
    built = z.run([tc.cc, "-std=c11", "-w", "-I", str(z.INCLUDE_DIR), "-I", str(directory), str(directory / "harness.c"),
                   *map(str, sources), "-o", str(exe)])
    check(built.returncode == 0, "boundary harness does not build: " + (built.stderr or "")[:300])
    if built.returncode == 0:
        r = z.run([str(exe)], timeout=30)
        check(r.returncode == 0, "z80_omitted_start is wrong at case %d" % r.returncode)


def determinism(batches, work, tc):
    batch = batches[0]
    images = parse_spec(batch.spec_text)
    plan = {i: set(sorted(exposed_offsets(img))[::3]) for i, img in images.items()}
    spec = selective_spec(batch.spec_text, plan)
    a, b = work / "det" / "a", work / "det" / "b"
    z.emit_image(tc, spec, a, "det")
    z.emit_image(tc, spec, b, "det")
    names = sorted(p.name for p in a.iterdir())
    check(names == sorted(p.name for p in b.iterdir()), "the two emissions differ in their file sets")
    for n in names:
        check(filecmp.cmp(a / n, b / n, shallow=False), "selective emission is not deterministic: %s" % n)


def main():
    tc = z.Toolchain(CC, EMITTER, None)
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        scenario = z.scenario_batches(z.load_scenarios())
        differential("scenario", scenario, work, tc)
        differential("random", z.scenario_batches(random_documents()), work, tc)
        if ONLY is None or "static" in ONLY:
            broad_equivalence(scenario, work, tc)
            determinism(scenario, work, tc)
            contract(work, tc)
            rejection_boundaries(work, tc)
        if ONLY is not None:
            check(SEEN == ONLY - {"static"}, "batch selection %s matched %s" % (sorted(ONLY), sorted(SEEN)))
    if ONLY is None or ONLY - {"static"}:
        check(TOTALS["unadmitted_stops"] >= 1, "no fail-closed stop observed: %s" % TOTALS)
        check(TOTALS["admitted"] < TOTALS["exposed"], "the derived plans are not a strict subset: %s" % TOTALS)
    if ONLY is None or ONLY - {"static"}:
        check(TOTALS["unadmitted_stops"] >= 2 * TOTALS["vectors"] // 10 and TOTALS["exact_stops"] >= TOTALS["vectors"] // 20,
              "too few fail-closed stops for the vectors run: %s" % TOTALS)
    if FAILED:
        print("\n".join(FAILED[:25]))
        return 1
    print("selective admission: %(vectors)d vectors identical to the broad AOT under derived plans (%(admitted)d of %(exposed)d "
          "exposed offsets admitted); %(unadmitted_stops)d unadmitted_pc stops, %(exact_stops)d proven effect-free" % TOTALS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
