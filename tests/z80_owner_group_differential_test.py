#!/usr/bin/env python3
"""SEG-033-T003: grouped Z80 owners are observably identical to the one-function-per-start reference (hermetic).

usage: z80_owner_group_differential_test.py <z80_image_emitter> <cc> <product-root> [quick|full [batch,batch,...]]

The reference is the merged broad AOT: owner group size 1 (one C function per instruction start, the historical emission).
The candidates are grouped emissions (bounded multi-entry owners that select the entry from the PC or the window offset).
The same vectors run natively against every emission and every step result (CPU/internal state, outcome, T-states, memory
writes, I/O) must be identical:

  * the whole legal-form matrix (every form row of the canonical dataset, 45,948 vectors in 64 KiB slot images);
  * every conformance scenario (prefix chains and locks, direct branches, calls/returns, bank/window-relative owners, the
    same PC under different image identities and remap, INT/NMI/EI delay/HALT/IM0-2, block repeats, deadline/resumable
    runs, fail-closed outcomes, entry-table chunking);
  * seeded random programs over an invariant image and over an SMS-shaped banked map with remap steps and random INT/NMI.

Every candidate emission must also keep the exact entry set and the owner classification of the reference, create strictly
fewer host owners and respect the group bound. Candidates vary the group bound (odd small bounds 2, 3, 7 put group boundaries at
every kind of position; the production default) and the shared PC-independent effect bodies (SEG-033-T005) independently.
`quick` (default) compares the scenarios and random programs; `full` adds the form matrix.

CI wall-clock: the optional fifth argument restricts the run to the named quick batches (scenario and random batch names); CTest
registers disjoint slices whose union is every batch, and a name that matches no batch fails the run, so a slice can neither be
empty nor silently skip a renamed batch. Without it every batch runs.
"""
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402
from z80_random_programs import random_documents  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
MODE = sys.argv[4] if len(sys.argv) > 4 else "quick"
assert MODE in ("quick", "full"), MODE
ONLY = set(sys.argv[5].split(",")) if len(sys.argv) > 5 else None
# (group bound, shared bodies): the reference is (1, 0) = one function per start with every effect inline (the merged emission).
# None selects the emitter default (the production setting).
REFERENCE = (1, 0)
CONFIGS = ((2, None), (3, None), (7, None), (None, None), (None, 0), (1, None))
FAILED = []
COMPARED = 0
SEEN = set()


def check(cond, message):
    if not cond:
        FAILED.append(message)


def comparable(stats):
    """The classification counts, which an emission layout must not change (shape_* and the TU count are layout)."""
    return {k: v for k, v in stats.items() if not k.startswith("shape_") and k != "units"}


def toolchain(config):
    group, share = config
    return z.Toolchain(CC, EMITTER, None, owner_group=group, share_bodies=share)


def tag_of(config):
    return "group=%s share=%s" % ("default" if config[0] is None else config[0], "default" if config[1] is None else config[1])


def compare_batches(label, batches, work):
    global COMPARED
    for batch in batches:
        if ONLY is not None:
            if batch.name not in ONLY:
                continue
            SEEN.add(batch.name)
        outputs = {}
        for config in (REFERENCE, *CONFIGS):
            bdir = work / label / batch.name / ("g%s_s%s" % config)
            built = z.build_generated(toolchain(config), batch.spec_text, bdir, stem="z80_" + batch.name, list_owners=True)
            if built["error"]:
                check(False, "%s/%s %s: %s" % (label, batch.name, tag_of(config), built["error"][:300]))
                break
            outputs[config] = (built, z.run_exe(built["exe"], batch.text, bdir))
        else:
            ref_built, ref_results = outputs[REFERENCE]
            ref_shape = ref_built["stats"]
            check(ref_shape["shape_owners"] == ref_shape["shape_entries"] and ref_shape["shape_max_group"] == 1 and
                  ref_shape["shape_shared_bodies"] == 0, "%s/%s: the reference must be one function per start, no shared body" % (label, batch.name))
            if label == "random":  # the random programs must really execute: not stop at the first fetch
                steps = [s for v in ref_results.values() for s in v]
                check(len(steps) >= 150 and len({s["cpu"]["pc"] for s in steps}) >= 60,
                      "%s/%s: the random programs execute too little (%d steps)" % (label, batch.name, len(steps)))
            for config in CONFIGS:
                built, results = outputs[config]
                tag = "%s/%s %s" % (label, batch.name, tag_of(config))
                stats = built["stats"]
                check(comparable(stats) == comparable(ref_shape), "%s: classification stats differ from the reference" % tag)
                check(stats["shape_entries"] == ref_shape["shape_entries"], "%s: the exact entry set changed" % tag)
                check(built["owners"] == ref_built["owners"], "%s: per-entry owner records differ from the reference" % tag)
                if config[0] is not None:
                    check(stats["shape_max_group"] <= config[0], "%s: a group exceeds its bound (%d)" % (tag, stats["shape_max_group"]))
                if stats["shape_entries"] > 1 and config[0] != 1:
                    check(stats["shape_owners"] < stats["shape_entries"], "%s: nothing was grouped" % tag)
                if config[1] == 0:
                    check(stats["shape_shared_bodies"] == 0, "%s: bodies shared although sharing is off" % tag)
                elif stats.get("full", 0) > 1:  # an image of only locks/stubs has no effect to share
                    check(stats["shape_shared_bodies"] > 0, "%s: no effect body was shared" % tag)
                check(set(results) == set(ref_results), "%s: vector set differs" % tag)
                for name in sorted(ref_results):
                    COMPARED += 1
                    if results.get(name) != ref_results[name]:
                        check(False, "%s: vector %s diverges from the reference" % (tag, name))
                        if len(FAILED) > 20:
                            return


def main():
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        compare_batches("scenario", z.scenario_batches(z.load_scenarios()), work)
        compare_batches("random", z.scenario_batches(random_documents()), work)
        if ONLY is not None:
            check(SEEN == ONLY, "batch selection %s matched %s" % (sorted(ONLY), sorted(SEEN)))
        if MODE == "full":
            by_form = z.form_vectors()
            compare_batches("forms", z.slot_batches([v for f in sorted(by_form) for v in by_form[f]]), work)
    if FAILED:
        print("\n".join(FAILED[:20]))
        return 1
    print("grouped-owner differential (%s): %d vector results identical between the reference %s and %s" % (MODE, COMPARED, REFERENCE, list(CONFIGS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
