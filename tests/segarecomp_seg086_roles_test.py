#!/usr/bin/env python3
"""SEG-086: sample-role enforcement (synthetic datasets only; no ROM, no emulator, no model fit)."""
import json
import pathlib
import sys
import tempfile
import types

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_sample_roles as sr  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def raises(fn, exc=sr.RoleError):
    try:
        fn()
    except exc:
        return True
    return False


SHA_E = "e" * 64
SHA_D = "d" * 64
SHA_RES = "f" * 64


def manifest(extra=()):
    return {"entries": [{"image_sha256": SHA_E, "lineage": "Z01"}, *extra]}


def reserved(sha):
    if sha == SHA_RES:
        raise ValueError("reserved identity (synthetic)")


def meta(id_="s/e", sha=SHA_E, lineage="Z01", **kw):
    return {"id": id_, "sha256": sha, "lineage": lineage, "audit": {"starts": 1}, **kw}


def write(root, name, m):
    d = root / name
    d.mkdir(parents=True)
    (d / "meta.json").write_text(json.dumps(m))
    return d


def test_role_records_and_legacy():
    check(sr.role_of(meta()) == sr.LEGACY, "legacy sample has no role fields")
    check(sr.role_of(meta(role="development: x", role_class="development", evaluation_only=False)) == sr.DEVELOPMENT, "development")
    check(sr.role_of(meta(role="evaluation-only: x, never training", role_class="evaluation-only", evaluation_only=True)) == sr.EVALUATION_ONLY, "eval")
    check(sr.role_of(meta(role="exploratory diagnostic-only")) == sr.EXPLORATORY, "exploratory by role string only")
    for bad in (dict(role="sealed"), dict(role_class="blind"), dict(evaluation_only=True), dict(role="development", evaluation_only=True),
                dict(role="evaluation-only: never training", role_class="development"), dict(role="evaluation-only: never training", evaluation_only="yes"),
                dict(role="evaluation-only: never training", role_class="evaluation-only", evaluation_only=False)):
        check(raises(lambda b=bad: sr.role_of(meta(**b))), f"contradictory or unknown role accepted: {bad}")


def test_training_entry_points_reject_evaluation_only():
    ev = meta(role="evaluation-only: x, never training", role_class="evaluation-only", evaluation_only=True)
    check(raises(lambda: sr.require_trainable(ev, "calibration", manifests=[])), "evaluation-only sample accepted for calibration")
    check(raises(lambda: sr.require_trainable(meta(role="exploratory diagnostic-only"), manifests=[])), "exploratory accepted for training")
    check(sr.require_trainable(meta(sha=SHA_D, lineage="D01", role="development: x"), manifests=[]) == sr.DEVELOPMENT, "development refused")
    check(sr.require_trainable(meta(sha=SHA_D, lineage="D01"), manifests=[]) == sr.LEGACY, "backward compatibility: legacy development sample refused")
    with tempfile.TemporaryDirectory() as t:
        mp = pathlib.Path(t) / "m.json"
        mp.write_text(json.dumps(manifest()))
        check(raises(lambda: sr.require_trainable(meta(), "training", manifests=[mp])), "legacy sample with a frozen evaluation identity accepted")
        check(raises(lambda: sr.require_trainable(meta(sha=SHA_D, lineage="Z01"), "training", manifests=[mp])), "frozen evaluation lineage accepted")
        check(raises(lambda: sr.require_trainable_lineages(["D01", "Z01"], manifests=[mp])), "evaluation lineage accepted in a fold definition")
        sr.require_trainable_lineages(["D01", "D02"], manifests=[mp])
        check(raises(lambda: sr.require_trainable(meta(), manifests=[pathlib.Path(t) / "missing.json"])), "unreadable manifest not fail-closed")


def test_loader_wrappers_do_not_read_samples_of_the_wrong_role():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        d1 = write(root, "dev", meta("d/1", SHA_D, "D01", role="development: x", role_class="development", evaluation_only=False))
        d2 = write(root, "ev", meta(role="evaluation-only: x, never training", role_class="evaluation-only", evaluation_only=True))
        calls = []
        tr = types.SimpleNamespace(registry=lambda ids: [d1, d2], Image=lambda d: calls.append(d) or d)
        check(raises(lambda: sr.load_trainable_images(tr, ["D01"], manifests=[])), "evaluation-only sample reached the training loader")
        check(not calls, "an Image was constructed before the role check failed")
        tr.registry = lambda ids: [d1]
        check(sr.load_trainable_images(tr, ["D01"], manifests=[]) == [d1], "trainable loader")


def test_evaluation_context_membership_and_reserved_first():
    ctx = sr.EvaluationContext(manifest(), "unit test", reserved)
    legacy_eval = meta()                                                         # restored before role preservation: admitted only through the manifest
    check(ctx.admit(legacy_eval) == (SHA_E, "Z01"), "frozen member refused")
    check(ctx.admit(meta(role="evaluation-only: x, never training", role_class="evaluation-only", evaluation_only=True)) == (SHA_E, "Z01"), "role-marked member refused")
    check(raises(lambda: ctx.admit(meta(sha=SHA_D))), "non-member admitted")
    check(raises(lambda: ctx.admit(meta(lineage="Z02"))), "lineage mismatch admitted")
    check(raises(lambda: ctx.admit(meta(role="development: x", role_class="development", evaluation_only=False))), "development sample admitted as evaluation material")
    order = []
    c2 = sr.EvaluationContext({"entries": [{"image_sha256": SHA_RES, "lineage": "Z09"}]}, "unit", lambda s: (order.append("reserved"), reserved(s)))
    check(raises(lambda: c2.admit(meta(sha=SHA_RES, lineage="Z09", role="garbage"))) and order == ["reserved"], "reserved identity not refused before the role is parsed")
    check(raises(lambda: sr.EvaluationContext(manifest(), "")), "context without purpose")
    check(raises(lambda: sr.EvaluationContext({"entries": []}, "x", reserved)), "empty manifest")
    check(raises(lambda: sr.EvaluationContext({"entries": [{"image_sha256": SHA_E, "lineage": "Z01"}] * 2}, "x", reserved)), "duplicate manifest entry")
    check(raises(lambda: sr.EvaluationContext(manifest(), "x", reserved, expected_digest="0" * 64)), "manifest digest mismatch accepted")
    check(sr.EvaluationContext(manifest(), "x", reserved, expected_digest=sr.manifest_digest(manifest())).purpose == "x", "digest match refused")


def test_discovery_is_manifest_driven():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        ctx = sr.EvaluationContext(manifest(), "unit", reserved)
        e = write(root, "a/ev", meta())
        check(ctx.discover(root) == [e], "member not discovered")
        write(root, "b/extra", meta("s/x", SHA_D, "D05"))                         # a stray directory sample is an error, not ignored
        check(raises(lambda: ctx.discover(root)), "stray non-member sample silently accepted")
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        ctx = sr.EvaluationContext(manifest(), "unit", reserved)
        check(raises(lambda: ctx.discover(root)), "missing member accepted")
        check(ctx.discover(root, require_complete=False) == [], "incomplete discovery with explicit allowance")
        write(root, "a", meta())
        write(root, "b", meta("s/dup"))
        check(raises(lambda: ctx.discover(root)), "duplicate member accepted")
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        write(root, "r", meta(sha=SHA_RES, lineage="Z01"))
        check(raises(lambda: sr.EvaluationContext(manifest(), "unit", reserved).discover(root)), "reserved identity discovered")
        calls = []
        tr = types.SimpleNamespace(Image=lambda d: calls.append(d))
        check(raises(lambda: sr.load_evaluation_images(tr, sr.EvaluationContext(manifest(), "unit", reserved), root)) and not calls, "Image built before admission")


def main() -> int:
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        fn()
    if FAILURES:
        print("\n".join(FAILURES))
        return 1
    print("segarecomp_seg086_roles_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
