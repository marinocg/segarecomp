#!/usr/bin/env python3
"""SEG-086 post-freeze amendment: the registry/Image protection layer (synthetic datasets and a stand-in loader module; no ROM, no model)."""
import json
import pathlib
import sys
import tempfile
import types

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_sample_guard as sg  # noqa: E402
import segarecomp_sample_roles as sr  # noqa: E402

FAILURES = []
SHA_D, SHA_E, SHA_RES = "d" * 64, "e" * 64, "f" * 64
H01_SHA = "511f35cc11f88316f8b8940e28ab298bd75a4da193672a80172884d6eb913b6a"


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def raises(fn, exc=sr.RoleError):
    try:
        fn()
    except exc:
        return True
    return False


def make_tr(root: pathlib.Path):
    """A stand-in for segarecomp_ml_v3_train: Image records every file it would read; registry enumerates meta.json files."""
    log = []

    class Image:
        def __init__(self, meta_dir):
            log.append(("construct", pathlib.Path(meta_dir).name))
            for f in ("classes.bin", "tokens.bin"):
                log.append(("read", f))

    def registry(ids=None):
        out = []
        for m in sorted(root.rglob("meta.json")):
            meta = json.loads(m.read_text())
            if "audit" in meta and (ids is None or meta["id"] in ids or meta["lineage"] in ids):
                out.append(m.parent)
        return out

    return types.SimpleNamespace(Image=Image, registry=registry), log


def sample(root, name, **kw):
    d = root / name
    d.mkdir(parents=True)
    m = {"id": f"x/{name}", "sha256": SHA_D, "lineage": "D01", "audit": {"starts": 1}}
    m.update(kw)
    (d / "meta.json").write_text(json.dumps(m))
    return d


def guarded(root, manifests=(), reserved=()):
    tr, log = make_tr(root)
    un = sg.install(tr, reserved=set(reserved), manifests=list(manifests))
    return tr, log, un


def test_training_and_calibration_reject_evaluation_only():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        ev = sample(root, "ev", sha256=SHA_E, lineage="Z01", role="evaluation-only: x, never training", role_class="evaluation-only", evaluation_only=True)
        tr, log, un = guarded(root)
        check(raises(lambda: tr.Image(ev)), "training load of an evaluation-only sample")
        check(raises(lambda: [tr.Image(d) for d in tr.registry({"Z01"})]), "calibration/registry enumeration of an evaluation-only sample")
        check(log == [], f"nothing was constructed or read: {log}")
        un()


def test_legacy_development_accepted_and_legacy_frozen_identity_rejected():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        ok = sample(root, "legacy")                                             # no role fields: legacy development
        bad = sample(root, "legacy_eval", sha256=SHA_E, lineage="Z01")           # no role fields, but a frozen evaluation identity
        man = root / "m.json"
        man.write_text(json.dumps({"entries": [{"image_sha256": SHA_E, "lineage": "Z01"}]}))
        tr, log, un = guarded(root, [man])
        tr.Image(ok)
        check(log[:1] == [("construct", "legacy")], "legacy development sample refused")
        n = len(log)
        check(raises(lambda: tr.Image(bad)) and len(log) == n, "legacy metadata with a frozen evaluation identity accepted or read")
        un()


def test_contradictory_unknown_exploratory_roles_rejected():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        cases = {"contra": dict(role="development: x", role_class="evaluation-only"), "unknown": dict(role="sealed blind"), "flag": dict(evaluation_only=True),
                 "explor": dict(role="exploratory diagnostic-only", role_class="exploratory", evaluation_only=False)}
        dirs = {k: sample(root, k, **v) for k, v in cases.items()}
        tr, log, un = guarded(root)
        for k, d in dirs.items():
            check(raises(lambda d=d: tr.Image(d)), f"{k}: accepted")
        check(log == [], "something was read before the role check")
        un()


def test_reserved_identity_checked_before_any_content_is_read():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        r = sample(root, "res", sha256=SHA_RES, role="development: x", role_class="development", evaluation_only=False)
        tr, log, un = guarded(root, reserved={SHA_RES})
        check(raises(lambda: tr.Image(r)) and log == [], "reserved identity not refused first")
        check(raises(lambda: tr.registry(None)), "registry enumeration returned a reserved identity")
        un()
        check(not hasattr(tr.Image, "_guarded"), "uninstall")


def test_unreadable_reserved_list_fails_closed():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        d = sample(root, "s")
        tr, log = make_tr(root)

        def boom():
            raise RuntimeError("reserved list unreadable")
        un = sg.install(tr, reserved=boom)
        check(raises(lambda: tr.Image(d), RuntimeError) and log == [], "unreadable reserved list must refuse, not allow")
        un()


def test_evaluation_context_h01_member_and_unexpected_members():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        h01 = sample(root, "h01", id="h01/ps4", sha256=H01_SHA, lineage="H01", role="evaluation-only: SEG-085 independent historical cohort (H01), never training",
                     role_class="evaluation-only", evaluation_only=True)
        legacy_h01 = sample(root, "h01_legacy", id="h01/ps4b", sha256=H01_SHA, lineage="H01")
        stray = sample(root, "stray", sha256=SHA_E, lineage="Z09", role="evaluation-only: x, never training", role_class="evaluation-only", evaluation_only=True)
        manifest = json.loads((ROOT / "tools" / "segarecomp_seg085_cohort.manifest.json").read_text())
        ctx = sr.EvaluationContext(manifest, "unit test", lambda s: None)
        tr, log, un = guarded(root, [ROOT / "tools" / "segarecomp_seg085_cohort.manifest.json"])
        tr.Image(h01, evaluation=ctx)
        check(("construct", "h01") in log, "legitimate H01 evaluation refused through the EvaluationContext")
        tr.Image(legacy_h01, evaluation=ctx)                                       # restored before role preservation: admitted via the manifest only
        check(raises(lambda: tr.Image(h01)), "H01 admitted without an evaluation context")
        n = len(log)
        check(raises(lambda: tr.Image(stray, evaluation=ctx)) and len(log) == n, "a non-member admitted into the evaluation context")
        check(raises(lambda: ctx.discover(root)), "unexpected evaluation-directory member / duplicate H01 member not rejected")
        un()
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        sample(root, "h01", id="h01/ps4", sha256=H01_SHA, lineage="H01", role_class="evaluation-only", role="evaluation-only: x, never training", evaluation_only=True)
        ctx = sr.EvaluationContext(json.loads((ROOT / "tools" / "segarecomp_seg085_cohort.manifest.json").read_text()), "unit", lambda s: None)
        check(len(ctx.discover(root)) == 1, "exact membership refused")
        check(raises(lambda: ctx.discover(root / "missing_dir_is_empty") if (root / "missing_dir_is_empty").mkdir() is None else None), "missing manifest member not rejected")


def test_install_is_idempotent_and_wraps_registry():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        ok = sample(root, "ok")
        tr, log = make_tr(root)
        u1 = sg.install(tr, reserved=set(), manifests=[])
        u2 = sg.install(tr, reserved=set(), manifests=[])
        check(tr.registry({"D01"}) == [ok], "registry")
        tr.Image(ok)
        check(log.count(("construct", "ok")) == 1, "double install must not double-wrap")
        u1()
        u2()


def main() -> int:
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        fn()
    if FAILURES:
        print("\n".join(FAILURES))
        return 1
    print("segarecomp_seg086_guard_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
