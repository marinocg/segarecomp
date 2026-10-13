#!/usr/bin/env python3
"""SEG-056: refit the HISTORICAL SEG-051 C1 recipe (unchanged trainer, 3 seeds) on the SEG-056 development folds.

Gives baselines A/B on exactly the same leave-one-lineage-out folds as the new candidates. Private research tooling; writes
private 32-byte-cell logits only. Sealed lineages are never loaded.
"""
import argparse
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80 as z1  # noqa: E402
import segarecomp_ml_region_z80_gen2_train as T  # noqa: E402
import segarecomp_ml_region_z80_train as old  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img-dirs", nargs="+", required=True)
    ap.add_argument("--truth-dirs", nargs="+", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--held", required=True)
    ap.add_argument("--threads", type=int, default=2)
    a = ap.parse_args()
    plan = T.check_plan()
    np, torch = T.setup(a.threads)
    dev = T.dev_ids(plan)
    if a.held not in dev:
        raise SystemExit("held-out lineage must be a development lineage")

    def find(lid):
        for idir, tdir in zip(a.img_dirs, a.truth_dirs):
            if (pathlib.Path(idir) / f"{lid}.sms").exists():
                return old.load(np, lid, idir, tdir)
        raise SystemExit(f"missing private inputs for {lid}")

    train = [find(i) for i in dev if i != a.held]
    held = find(a.held)
    models = [old.fit(np, torch, train, False, s) for s in T.SEEDS]
    np.save(pathlib.Path(a.state) / f"c1cell_{a.held}.npy", old.score(np, torch, models, held))
    print("c1 refit", a.held, flush=True)


if __name__ == "__main__":
    main()
