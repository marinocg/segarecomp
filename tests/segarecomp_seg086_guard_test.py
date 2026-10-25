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
import segarecomp_seg086_guarded as gd  # noqa: E402

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


def fake_cal(events, results_path, nested_digest="D1", load=None):
    def load_records():
        events.append("load_records")
        return [], [{"id": "a", "lineage": "L", "scores": [1.0], "labels": [1], "seeds": {0}, "classes": b"I"}]
    return types.SimpleNamespace(verify_plan=lambda: events.append("verify_plan") or "PLAN", RESULTS=results_path, load_records=load or load_records,
                                 run_nested=lambda v, r: events.append("run_nested") or {"n": 1}, canonical=lambda o: nested_digest, main=lambda a: events.append(("cal.main", a)) or 7,
                                 SELECTION=None)


def env_for(events, cal, **kw):
    return gd.Env(guard_install=lambda: events.append("guard_install"), cal=cal, **kw)


def results_file(root, digest="D1"):
    f = root / "results.json"
    f.write_text(json.dumps({"determinism_sha256": digest, "development_decision": {"decision": "CALIBRATION_INEFFECTIVE"}, "chosen_normaliser": "N2",
                             "tables_all_images": {"primary_N2": {"all": {"fallback_images": 40, "domain_images": 40}}}}))
    return f


def test_verify_frozen_never_loads_before_the_guard_and_fails_closed():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        ev = []
        cal = fake_cal(ev, results_file(root))
        old = gd.REFERENCE_RECORDS_SHA256
        gd.REFERENCE_RECORDS_SHA256 = gd.records_digest(cal.load_records()[1])
        ev.clear()
        try:
            check(gd.verify_frozen(env_for(ev, cal)) == 0, "verify-frozen should pass for matching digests")
            check(ev == ["verify_plan", "guard_install", "load_records", "run_nested"], f"order {ev}")
            check(ev.count("load_records") == 1, "exactly one load, and it is the guarded one")
            ev.clear()
            check(gd.verify_frozen(env_for(ev, fake_cal(ev, results_file(root), nested_digest="OTHER"))) == 1, "a nested-digest mismatch must fail")
            gd.REFERENCE_RECORDS_SHA256 = "0" * 64
            check(gd.verify_frozen(env_for(ev, fake_cal(ev, results_file(root)))) == 1, "a records-digest mismatch must fail")
            ev.clear()
            bad = fake_cal(ev, results_file(root))
            bad.verify_plan = lambda: (_ for _ in ()).throw(SystemExit("frozen plan missing or modified"))
            check(raises(lambda: gd.verify_frozen(env_for(ev, bad)), SystemExit) and "load_records" not in ev and "guard_install" not in ev, "a failed plan check must stop before any load")
        finally:
            gd.REFERENCE_RECORDS_SHA256 = old


def test_dispatcher_installs_guard_before_delegating_and_preserves_outputs():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        ev = []
        cal = fake_cal(ev, results_file(root))
        diag = types.SimpleNamespace(RESULTS=None, main=lambda a: ev.append(("diag.main", a)) or 0)
        rep = types.SimpleNamespace(main=lambda: ev.append("report.main") or 3)
        e = env_for(ev, cal, diag=diag, report=rep, h01=types.SimpleNamespace(main=lambda: ev.append("h01.main")))
        check(gd.run(["nested", "--out", str(root / "n.json"), "--variants", "N2"], e) == 7, "exit status of the delegate is preserved")
        check(ev[0] == "guard_install" and ev[1] == ("cal.main", ["nested", "--out", str((root / "n.json").resolve()), "--variants", "N2"]), f"guard first, arguments passed through: {ev}")
        ev.clear()
        check(gd.run(["diag", "--out", str(root / "d.json")], e) == 0 and ev[0] == "guard_install" and diag.RESULTS == (root / "d.json").resolve(), "diag: guard first, output redirected")
        ev.clear()
        check(gd.run(["select", "--out", str(root / "s.json")], e) == 7 and ev[0] == "guard_install" and cal.SELECTION == (root / "s.json").resolve(), "select: guard first")
        ev.clear()
        check(gd.run(["report"], e) == 3 and ev == ["guard_install", "report.main"], "report")
        ev.clear()
        check(gd.run(["h01"], e) == 0 and ev == ["guard_install", "h01.main"], "h01 (None return maps to 0)")


def test_dispatcher_rejects_unsafe_commands_and_frozen_outputs():
    ev = []
    e = env_for(ev, fake_cal(ev, pathlib.Path("/nonexistent")))
    for argv in (["freeze"], ["final"], ["bogus"], [], ["nested", "--variants", "N9"], ["diag"], ["nested", "--out", "tools/segarecomp_seg086_cal.results.json"],
                 ["select", "--out", str(gd.HERE / "segarecomp_seg086_cal.selection.json")], ["diag", "--out", str(gd.HERE / "segarecomp_seg086_diag.results.json")]):
        check(raises(lambda a=argv: gd.run(a, e), SystemExit) and ev == [], f"{argv}: accepted or guard/delegate reached")


def test_authorization_failure_stops_the_command_without_fallback():
    ev = []

    def deny():
        raise sr.RoleError("authorization metadata unreadable")
    cal = fake_cal(ev, pathlib.Path("/nonexistent"))
    e = gd.Env(guard_install=deny, cal=cal)
    check(raises(lambda: gd.run(["nested"], e)) and ev == [], "delegate ran after the guard failed to install")


def test_guarded_diag_and_calibration_entry_points_refuse_evaluation_only_samples():
    """The delegate stands for diag/cal: it enumerates the registry and builds Images. With the real Guard installed by the dispatcher, an evaluation-only sample is refused."""
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        sample(root, "ok", lineage="D01")
        sample(root, "ev", id="h01/ps4", sha256=H01_SHA, lineage="D02", role="evaluation-only: x, never training", role_class="evaluation-only", evaluation_only=True)
        sample(root, "legacy_h01", id="h01/legacy", sha256=H01_SHA, lineage="D03")
        man = ROOT / "tools" / "segarecomp_seg085_cohort.manifest.json"
        tr, log = make_tr(root)
        guard_env = lambda: sg.install(tr, reserved=set(), manifests=[man])
        for name in ("diag", "nested"):
            log.clear()
            ev = []

            def delegate(*_a, tr=tr):
                ev.append("delegate")
                return [tr.Image(d) for d in tr.registry({"D01", "D02", "D03"})]
            cal = fake_cal(ev, pathlib.Path("/nonexistent"))
            cal.main = delegate
            diag = types.SimpleNamespace(RESULTS=None, main=delegate)
            e = gd.Env(guard_install=guard_env, cal=cal, diag=diag)
            args = ["diag", "--out", str(root / "o.json")] if name == "diag" else ["nested"]
            check(raises(lambda: gd.run(args, e)) and log == [], f"{name}: evaluation-only / frozen-identity sample reached Image construction: {log}")
            sg._ACTIVE[:] = [g for g in sg._ACTIVE if g.tr is not tr]
            tr.Image.__init__ = tr.Image.__init__.__wrapped__ if hasattr(tr.Image.__init__, "__wrapped__") else tr.Image.__init__
            for g in list(sg._ACTIVE):
                g.uninstall()
            tr, log = make_tr(root)
            guard_env = lambda tr=tr: sg.install(tr, reserved=set(), manifests=[man])
        # eligible development input only: both entry points work and return the delegate's result unchanged
        with tempfile.TemporaryDirectory() as t2:
            root2 = pathlib.Path(t2)
            ok = sample(root2, "ok")
            tr2, log2 = make_tr(root2)
            ev = []
            cal = fake_cal(ev, pathlib.Path("/nonexistent"))
            cal.main = lambda a: [tr2.Image(d) for d in tr2.registry({"D01"})] and 0
            check(gd.run(["nested"], gd.Env(guard_install=lambda: sg.install(tr2, reserved=set(), manifests=[man]), cal=cal)) == 0 and log2 == [("construct", "ok"), ("read", "classes.bin"), ("read", "tokens.bin")],
                  "an eligible development sample is loaded normally through the dispatcher")


def test_corrupt_authorization_metadata_fails_closed():
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t)
        d = root / "bad"
        d.mkdir()
        (d / "meta.json").write_text("{not json")
        tr, log, un = guarded(root)
        check(raises(lambda: tr.Image(d), ValueError) and log == [], "unreadable meta.json must refuse before any read")
        e = root / "empty"
        e.mkdir()
        check(raises(lambda: tr.Image(e), OSError) and log == [], "missing meta.json must refuse")
        un()


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
