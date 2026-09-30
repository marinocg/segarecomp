#!/usr/bin/env python3
"""SEG-008-T009: secondary independent oracle (kosarev/z80, ADR 0057 decision 2) over every synthetic form vector.

usage: z80_secondary_oracle_test.py <z80_image_emitter> <cc> <product-root>

Skips (exit 0, "skipped: ...") without SEGARECOMP_Z80_ORACLE_CHECKOUT holding the pinned kosarev_z80 clone. Otherwise the
generated-native side must agree with kosarev on every vector outside the committed deviation mask; the mask itself is
falsified by a mutant (a mutated generated result stream must surface as an unexplained difference).
"""
import copy
import os
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as z  # noqa: E402

root = os.environ.get(z.CHECKOUT_ENV)
if not root or z.secondary_checkout(root) is None:
    print("skipped: pinned kosarev/z80 checkout unavailable (%s)" % z.CHECKOUT_ENV)
    sys.exit(0)

tc = z.Toolchain(sys.argv[2], sys.argv[1], None)
by_form = z.form_vectors()
vectors = [v for f in sorted(by_form) for v in by_form[f]]
failed = []
with tempfile.TemporaryDirectory() as tmp:
    work = pathlib.Path(tmp)
    exe = z.build_secondary(z.secondary_checkout(root), work)
    total, masked_seen = 0, set()
    for batch in z.slot_batches(vectors):
        bdir = work / batch.name
        built = z.build_generated(tc, batch.spec_text, bdir, stem="z80_" + batch.name)
        if built["error"]:
            failed.append(built["error"][:200])
            continue
        generated = z.run_exe(built["exe"], batch.text, bdir)
        secondary = z.run_exe(exe, batch.text, bdir)
        vecs = [v for v, _ in batch.vectors]
        total += len(vecs)
        failed += ["unexplained %s" % (d,) for d in z.secondary_unexplained(generated, secondary, vecs)[:3]]
        if batch.name == "slots00":  # mutant: a flipped flag in the generated stream must be reported
            mutant = copy.deepcopy(generated)
            first = next(iter(mutant))
            mutant[first][0]["cpu"]["f"] = "%02X" % (int(mutant[first][0]["cpu"]["f"], 16) ^ 0x01)
            if not z.secondary_unexplained(mutant, secondary, vecs):
                failed.append("mask mutant not detected")
    print("z80 secondary oracle: %d vectors" % total)
if failed:
    print("FAIL:", failed[:5])
    sys.exit(1)
print("z80 secondary oracle: ok")
