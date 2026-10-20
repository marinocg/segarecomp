#!/usr/bin/env python3
"""SEG-075 C1 (ADR 0160): tests of tools/segarecomp_xplat_handoff.py (standard library + git, synthetic data, no network/Docker).

Covers the artifact classification, the content-addressed store, physical verification (object corruption, truncation and loss are
detected; a dependency on a task worktree is refused; a fresh isolated process reaches the same verdict), restore, and the T001 audit
functions on a synthetic cache: declared outputs against record digests, a missing or corrupted build output, a changed link map, cross-run
identity, an incorrect source revision, a modified checkout file, and drift of a toolchain input, staged Dockerfile or embedded helper.
"""

import copy
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_xplat_handoff as h  # noqa: E402
import segarecomp_xplat_qualify as q  # noqa: E402

FAILURES = []


def write_fixture(path: pathlib.Path, text: str) -> None:
    """Write a synthetic fixture as exact UTF-8 bytes with LF line ends on every platform (Windows text mode would write CRLF, and the
    digest comparisons under test would then - correctly - report drift). Production code under test keeps its own I/O behaviour."""
    path.write_bytes(text.encode("utf-8"))


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def git(*args, cwd):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True,
                          env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
                               "GIT_COMMITTER_EMAIL": "t@t"}).stdout.strip()


def test_classify():
    check(h.classify_present("a", "a", "t001_record") == ("PRESENT_AND_VERIFIED", "t001_record"), "matching digest verified")
    check(h.classify_present("a", "b", "t001_record") == ("PRESENT_BUT_CHANGED", "t001_record"), "different digest changed")
    check(h.classify_present("a", None, "t001_record") == ("PRESENT_AND_VERIFIED", "baseline"), "no evidence -> baseline basis only")
    check(h.classify_missing(True) == "MISSING_REBUILD_REQUIRED" and h.classify_missing(False) == "MISSING_UNRECOVERABLE", "missing classes")
    for s in ("PRESENT_AND_VERIFIED", "PRESENT_BUT_CHANGED", "REPRODUCIBLE_BUT_NOT_PRESERVED", "MISSING_REBUILD_REQUIRED",
              "MISSING_UNRECOVERABLE", "NOT_REQUIRED_FOR_SUCCESSOR"):
        check(s in h.STATUSES, f"status {s} defined")
    es = [h.entry("x", "g", True, "PRESENT_BUT_CHANGED", "t001_record", None, "0" * 64, 3),
          h.entry("y", "g", False, "PRESENT_BUT_CHANGED", "baseline", None, "1" * 64, 4),
          h.entry("z", "g", True, "MISSING_REBUILD_REQUIRED", None)]
    s = h.summarize(es)
    check(s["required_problems"] == ["x", "z"], "only required problems are reported as problems")
    check(s["physical_files"] == 2 and s["physical_bytes"] == 7, "physical counts")


def make_store(td: pathlib.Path):
    src = td / "cache" / "seg075" / "files"
    src.mkdir(parents=True)
    data = {"a.bin": b"\x00\x01" * 100, "b.lst": b"listing\n", "c.bin": b"\x00\x01" * 100}  # c duplicates a: one object
    entries = []
    for name, b in data.items():
        p = src / name
        p.write_bytes(b)
        entries.append(h.entry(str(p.relative_to(td / "cache")), "test", True, "PRESENT_AND_VERIFIED", "baseline", p, sha(b), len(b)))
    store = td / "cache" / "seg075-handoff"
    stats = h.snapshot(entries, store)
    body = {"schema": h.SCHEMA, "label": "t", "entries": entries, "image_archives": []}
    mpath, digest = h.write_manifest(store, "t", body)
    return store, mpath, digest, entries, stats


def test_store_and_verify():
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        store, mpath, digest, entries, stats = make_store(td)
        check(stats["dedup"] == 1 and stats["copy"] == 2, "content-addressed dedup of identical files")
        obj0 = h.cas_path(store, entries[0]["sha256"])
        check(obj0.stat().st_ino != pathlib.Path(entries[0]["origin"]).stat().st_ino, "store objects are independent copies, not hardlinks")
        # an in-place write to a live file (truncate + write on the same inode) never reaches the preserved object
        with open(entries[1]["origin"], "r+b") as f:
            f.write(b"LISTING")
        check(h.verify_manifest(mpath)["verdict"] == "PASS", "in-place rewrite of the live cache leaves the store intact")
        check(h.verify_manifest(mpath, check_origin=True)["origin_drift"] == 1, "the live rewrite is reported as origin drift")
        # re-snapshot over an existing corrupted object replaces it instead of keeping it
        os.chmod(obj0, 0o644)
        obj0.write_bytes(b"\xff" * 200)
        check(h.put(store, pathlib.Path(entries[0]["origin"]), entries[0]["sha256"]) == "replaced", "corrupted object replaced on re-store")
        check(h.verify_manifest(mpath)["verdict"] == "PASS", "replaced object verifies")
        os.chmod(obj0, 0o644)
        obj0.unlink()
        os.link(entries[0]["origin"], obj0)
        check(h.put(store, pathlib.Path(entries[0]["origin"]), entries[0]["sha256"]) == "replaced", "an object sharing the live inode is replaced")
        check(obj0.stat().st_nlink == 1, "replaced object is independent")
        check(mpath.with_suffix(".sha256").read_text().split()[0] == digest == sha(mpath.read_bytes()), "manifest identity recorded")
        ok = h.verify_manifest(mpath)
        check(ok["verdict"] == "PASS" and ok["verified_files"] == 3 and ok["verified_bytes"] == 408, "clean store verifies physically")
        fresh = h.verify_fresh(mpath)
        check(fresh["verdict"] == "PASS" and fresh["fresh_process"]["cwd"] == "/", "fresh isolated process reaches the same verdict")
        # the store does not depend on the origin files: delete them, verification still passes, restore recreates them
        for e in entries:
            pathlib.Path(e["origin"]).unlink()
        check(h.verify_manifest(mpath)["verdict"] == "PASS", "verification needs only the store, not the origin tree")
        check(h.verify_manifest(mpath, check_origin=True)["origin_drift"] == 3, "origin loss is reported as drift")
        dest = td / "restored"
        n = h.restore(mpath, dest)
        check(n == 3 and (dest / entries[1]["logical"]).read_bytes() == b"listing\n", "restore materializes the logical tree")
        (dest / entries[1]["logical"]).write_bytes(b"rebuilt\n")
        check(h.verify_manifest(mpath)["verdict"] == "PASS", "writing a restored (copied) file leaves the store intact")
        try:
            h.restore(mpath, dest)
            check(False, "restore into a non-empty directory refused")
        except h.HandoffError:
            pass
        # corruption of the object itself (in place, same inode, same size)
        obj = h.cas_path(store, entries[1]["sha256"])
        os.chmod(obj, 0o644)
        with open(obj, "r+b") as f:
            f.write(b"L")
        bad = h.verify_manifest(mpath)
        check(bad["verdict"] == "FAIL" and any("bytes changed" in p for p in bad["problems"]), "byte corruption detected")
        obj.write_bytes(b"listing")
        check(any("size" in p for p in h.verify_manifest(mpath)["problems"]), "truncation detected")
        obj.unlink()
        check(any("object missing" in p for p in h.verify_manifest(mpath)["problems"]), "missing object detected")
        check(h.verify_fresh(mpath)["verdict"] == "FAIL", "fresh process also fails on a missing object")
        obj.write_bytes(b"listing\n")
        # the manifest digest must be present and may be bound to an independently recorded value
        check(h.verify_manifest(mpath, expect_sha256=digest)["verdict"] == "PASS", "expected manifest digest accepted")
        check(any("expected" in p for p in h.verify_manifest(mpath, expect_sha256="0" * 64)["problems"]), "unexpected manifest digest refused")
        sums = mpath.with_suffix(".sha256")
        saved_sums = sums.read_text()
        sums.unlink()
        check(any(".sha256 file missing" in p for p in h.verify_manifest(mpath)["problems"]), "missing digest file fails")
        write_fixture(sums, saved_sums)
        # manifest tampering is detected by its own digest file
        man = json.loads(mpath.read_bytes())
        man["entries"][0]["note"] = "edited"
        mpath.write_bytes(h.canonical(man))
        check(any("manifest digest" in p for p in h.verify_manifest(mpath)["problems"]), "manifest edit detected")
        # a dependency on a task worktree is refused
        man["entries"][0]["origin"] = "/x/.worktrees/seg-075-t002/tools/a.bin"
        _, d2 = h.write_manifest(store, "t", man)
        check(any("task worktree" in p for p in h.verify_manifest(mpath)["problems"]), "worktree origin refused")
        # a recorded required problem fails verification even when every object is intact
        man["entries"][0]["origin"] = "/x/a.bin"
        man["entries"][0]["status"] = "PRESENT_BUT_CHANGED"
        h.write_manifest(store, "t", man)
        check(any("recorded required problem" in p for p in h.verify_manifest(mpath)["problems"]), "recorded changed artifact fails verify")


def test_symlinks():
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td) / "c"
        root.mkdir()
        write_fixture((root / "f"), "x")
        try:
            os.symlink("f", root / "inside")
            os.symlink("/tmp", root / "outside")
            os.symlink("/r/.worktrees/seg-075-t002/x", root / "wt")
        except OSError:
            return  # no symlink privilege (Windows): the guard is exercised on POSIX CI
        bad = h.symlinks_escaping(root)
        check("inside" not in " ".join(bad), "relative in-tree symlink accepted")
        check(any(b.startswith("outside") for b in bad), "symlink leaving the cache reported")
        check(any(b.startswith("wt") and "worktree" in b for b in bad), "symlink into a task worktree reported")


def fake_plan(commit: str) -> dict:
    return {"toolchains": {"tc-x": {"dockerfile": "tools/tc-x.Dockerfile", "inputs": [{"name": "in.tar", "sha256": sha(b"input")}]}},
            "candidates": [{"id": "HelloAmi", "commit": commit,
                            "recipes": [{"name": "units", "toolchain": "tc-x", "outputs": ["out/*"]},
                                        {"name": "bad", "toolchain": "tc-x", "expect": "failure", "failure_signature": "x"}]},
                           {"id": "emutos", "commit": commit,
                            "recipes": [{"name": r, "toolchain": "tc-x", "outputs": ["emutos.map"]} for r in ("rom-192",)]},
                           {"id": "galaga", "commit": commit, "recipes": []}]}


def make_cache(td: pathlib.Path):
    cache = td / "cache" / "seg075" / "q"
    src = cache / "src" / "HelloAmi"
    src.mkdir(parents=True)
    git("init", "-q", cwd=src)
    write_fixture((src / "a.s"), "rts\n")
    write_fixture((src / ".gitattributes"), "*.bat text eol=crlf\n")
    (src / "run.bat").write_bytes(b"echo\r\n")
    git("add", "-A", cwd=src)
    git("commit", "-q", "-m", "x", cwd=src)
    commit = git("rev-parse", "HEAD", cwd=src)
    for run in ("a", "b"):
        d = cache / "build" / "HelloAmi" / "units" / run / "out"
        d.mkdir(parents=True)
        (d / "HelloAmi").write_bytes(b"\x00\x00\x03\xf3exe")
        (d / "HelloAmi.lst").write_bytes(b"00:00000000 4E75\t1: rts\n")
        write_fixture((cache / "build" / "HelloAmi" / "units" / f"{run}.log"), "ok\n")
        e = cache / "build" / "emutos" / "rom-192" / run
        (e / "obj").mkdir(parents=True)
        write_fixture((e / "emutos.map"), "map\n")
        (e / "obj" / "x.o").write_bytes(b"\x7fELFobj")
    (cache / "build" / "HelloAmi" / "bad" / "a").mkdir(parents=True)
    (cache / "records").mkdir()
    write_fixture((cache / "records" / "HelloAmi.json"), json.dumps({"id": "HelloAmi", "commit": commit, "recipes": [
        {"name": "units", "byte_identical_runs": True, "outputs": [{"path": "out/HelloAmi", "sha256": sha(b"\x00\x00\x03\xf3exe")}]},
        {"name": "bad"}]}))
    write_fixture((cache / "records" / "emutos.json"), json.dumps({"id": "emutos", "commit": commit, "recipes": [
        {"name": "rom-192", "byte_identical_runs": True, "outputs": [{"path": "emutos.map", "sha256": sha(b"map\n")}]}]}))
    return cache, commit


def by_logical(entries):
    return {e["logical"]: e for e in entries}


def test_audit_builds_and_sources():
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        cache, commit = make_cache(td)
        parent = td / "cache"
        plan = fake_plan(commit)
        es = by_logical(h.audit_builds(q, plan, cache, parent))
        exe_a = "seg075/q/build/HelloAmi/units/a/out/HelloAmi"
        check(es[exe_a]["status"] == "PRESENT_AND_VERIFIED" and es[exe_a]["basis"] == "t001_record", "declared output verified by record")
        lst = "seg075/q/build/HelloAmi/units/b/out/HelloAmi.lst"
        check(es[lst]["basis"] == "cross_run_identical", "listing verified across the two runs")
        check(es["seg075/q/build/emutos/rom-192/a/emutos.map"]["basis"] == "t001_record", "link map verified by record")
        check(any(e["group"] == "t002.truth_listing" and e["status"] == "REPRODUCIBLE_BUT_NOT_PRESERVED" for e in es.values()),
              "never-generated compiler listings are REPRODUCIBLE_BUT_NOT_PRESERVED")
        # corrupt the declared output of run a; change the link map of run b; delete run b's output
        (cache / "build" / "HelloAmi" / "units" / "a" / "out" / "HelloAmi").write_bytes(b"\x00\x00\x03\xf3exE")
        write_fixture((cache / "build" / "emutos" / "rom-192" / "b" / "emutos.map"), "map2\n")
        (cache / "build" / "HelloAmi" / "units" / "b" / "out" / "HelloAmi").unlink()
        es = by_logical(h.audit_builds(q, plan, cache, parent))
        check(es[exe_a]["status"] == "PRESENT_BUT_CHANGED", "corrupted output detected")
        check(es["seg075/q/build/emutos/rom-192/b/emutos.map"]["status"] == "PRESENT_BUT_CHANGED", "changed link map detected")
        check(es["seg075/q/build/HelloAmi/units/b/out/HelloAmi"]["status"] == "MISSING_REBUILD_REQUIRED", "missing build output detected")
        # a successor intermediate pattern that matches nothing is a missing required artifact
        for f in (cache / "build" / "emutos" / "rom-192" / "a" / "obj").iterdir():
            f.unlink()
        es = by_logical(h.audit_builds(q, plan, cache, parent))
        check(es["seg075/q/build/emutos/rom-192/a/obj/*.o"]["status"] == "MISSING_REBUILD_REQUIRED", "missing objects for T002 detected")
        # sources: correct pin with an eol-filtered file verifies; a wrong pin or a modified file does not
        ss = by_logical(h.audit_sources(q, plan, cache, parent))
        check(ss["seg075/q/src/HelloAmi/run.bat"]["status"] == "PRESENT_AND_VERIFIED", "eol-converted working copy verifies via filters")
        check(ss["seg075/q/src/HelloAmi/.git#HEAD"]["status"] == "PRESENT_AND_VERIFIED", "pinned clean checkout verified")
        check(ss["seg075/q/src/emutos"]["status"] == "MISSING_REBUILD_REQUIRED", "missing required checkout")
        check(ss["seg075/q/src/galaga"]["status"] == "NOT_REQUIRED_FOR_SUCCESSOR", "non-required candidate not required")
        wrong = fake_plan("0" * 40)
        ss = by_logical(h.audit_sources(q, wrong, cache, parent))
        check(ss["seg075/q/src/HelloAmi/.git#HEAD"]["status"] == "PRESENT_BUT_CHANGED", "incorrect source revision detected")
        check(ss["seg075/q/src/HelloAmi/a.s"]["status"] == "PRESENT_BUT_CHANGED", "files of a wrong revision are not verified")
        write_fixture((cache / "src" / "HelloAmi" / "a.s"), "nop\n")
        ss = by_logical(h.audit_sources(q, plan, cache, parent))
        check(ss["seg075/q/src/HelloAmi/a.s"]["status"] == "PRESENT_BUT_CHANGED", "modified checkout file detected")


def test_cross_run():
    """A needed listing/object that differs between the two runs is a problem; a by-product difference is recorded, never verified."""
    st, basis = h.verify_build_file("out/x.lst", "1", {}, set(), True, "a", {"out/x.lst": "2"}, True, needed=True)
    check((st, basis) == ("PRESENT_BUT_CHANGED", "cross_run_differs"), "needed file differing across runs is changed")
    st, basis = h.verify_build_file("gen.h", "1", {}, set(), True, "a", {"gen.h": "2"}, True, needed=False)
    check((st, basis) == ("PRESENT_UNVERIFIED", "cross_run_differs"), "by-product difference is recorded, not verified")
    check(h.verify_build_file("out/x.lst", "1", {}, set(), True, "a", {"out/x.lst": "1"}, True, True)[0] == "PRESENT_AND_VERIFIED",
          "identical across runs verifies")
    check(h.verify_build_file("o", "1", {"o": "1"}, set(), False, "b", {"o": "1"}, True, True)[1] == "cross_run_identical",
          "run b is not checked against the record unless T001 recorded identical runs")
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        cache, commit = make_cache(td)
        plan = fake_plan(commit)
        (cache / "build" / "HelloAmi" / "units" / "a" / "out" / "HelloAmi.lst").write_bytes(b"00:00000000 4E71\t1: nop\n")
        write_fixture((cache / "build" / "HelloAmi" / "units" / "a" / "out" / "scratch.tmp"), "1")
        write_fixture((cache / "build" / "HelloAmi" / "units" / "b" / "out" / "scratch.tmp"), "2")
        es = by_logical(h.audit_builds(q, plan, cache, td / "cache"))
        lst = es["seg075/q/build/HelloAmi/units/a/out/HelloAmi.lst"]
        check(lst["status"] == "PRESENT_BUT_CHANGED" and lst["required"], "corrupted needed listing (one run) is a required problem")
        tmp = es["seg075/q/build/HelloAmi/units/a/out/scratch.tmp"]
        check(not tmp["required"] and tmp.get("verified") == "PRESENT_UNVERIFIED", "differing by-product kept, not required, not verified")
        check("seg075/q/build/HelloAmi/units/a/out/HelloAmi.lst" in h.summarize(list(es.values()))["required_problems"],
              "summary reports the needed-listing problem")


def test_record_code_basis():
    """Run b of an output T001 recorded deterministic only by loadable-code digest is checked against that digest."""
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        cache, commit = make_cache(td)
        plan = fake_plan(commit)
        prg_a = b"\x60\x1a" + struct_pack_prg(4, 0, 0, 4) + b"\x4e\x75\x4e\x71" + b"SYMA" + b"\0\0\0\0"
        prg_b = prg_a[:-8] + b"SYMB" + b"\0\0\0\0"
        rec = json.loads((cache / "records" / "HelloAmi.json").read_text())
        rec["recipes"][0]["byte_identical_runs"] = False
        rec["recipes"][0]["outputs"].append({"path": "out/x.prg", "sha256": sha(prg_a), "code_sha256": q.code_digest(prg_a)})
        write_fixture((cache / "records" / "HelloAmi.json"), json.dumps(rec))
        (cache / "build" / "HelloAmi" / "units" / "a" / "out" / "x.prg").write_bytes(prg_a)
        (cache / "build" / "HelloAmi" / "units" / "b" / "out" / "x.prg").write_bytes(prg_b)
        es = by_logical(h.audit_builds(q, plan, cache, td / "cache"))
        b = es["seg075/q/build/HelloAmi/units/b/out/x.prg"]
        check(b["status"] == "PRESENT_AND_VERIFIED" and b["basis"] == "t001_record_code", "symbol-only run-b difference verified by code digest")
        (cache / "build" / "HelloAmi" / "units" / "b" / "out" / "x.prg").write_bytes(prg_b.replace(b"\x4e\x71", b"\x4e\x70"))
        es = by_logical(h.audit_builds(q, plan, cache, td / "cache"))
        check(es["seg075/q/build/HelloAmi/units/b/out/x.prg"]["status"] == "PRESENT_BUT_CHANGED", "changed run-b code detected")


def struct_pack_prg(t, d, b, s):
    import struct
    return struct.pack(">IIIIIIH", t, d, b, s, 0, 0, 0)


def test_truth_listing_resolution():
    es = [h.entry("seg075/q/build/toslibc#assembler-listings", "t002.truth_listing", True, "REPRODUCIBLE_BUT_NOT_PRESERVED", None),
          h.entry("seg075/q/build/emutos#assembler-listings", "t002.truth_listing", True, "REPRODUCIBLE_BUT_NOT_PRESERVED", None),
          h.entry("seg075/t002/truth/toslibc/lib+examples/lib/a.o.lst", "t002.truth", True, "PRESENT_AND_VERIFIED", "baseline", None, "1" * 64, 3)]
    h.resolve_truth_listings(es)
    check(es[0]["status"] == "PRESENT_AND_VERIFIED" and es[0]["listings"] == 1 and len(es[0]["listings_sha256"]) == 64,
          "a program with preserved listings is resolved with count and digest")
    check(es[1]["status"] == "REPRODUCIBLE_BUT_NOT_PRESERVED", "a program without listings in the snapshot stays unresolved")


def test_audit_control():
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        cache, commit = make_cache(td)
        product = td / "product"
        (product / "tools").mkdir(parents=True)
        write_fixture((product / "tools" / "tc-x.Dockerfile"), "FROM x\n")
        tc = cache / "toolchains" / "tc-x"
        tc.mkdir(parents=True)
        (tc / "in.tar").write_bytes(b"input")
        write_fixture((tc / "Dockerfile"), "FROM x\n")
        xp = cache / "xplat"
        (xp / "bin").mkdir(parents=True)
        write_fixture((xp / "ci_alias.py"), q.CI_ALIAS)
        write_fixture((xp / "asts_pilot.py"), q.ASTS_PILOT)
        write_fixture((xp / "bin" / "date"), q.DATE_SHIM)
        plan = fake_plan(commit)
        es = h.audit_control(q, plan, cache, td / "cache", product)
        check(all(e["status"] == "PRESENT_AND_VERIFIED" for e in es), "inputs, Dockerfile and helpers verify")
        (tc / "in.tar").write_bytes(b"inpuT")
        write_fixture((tc / "Dockerfile"), "FROM y\n")
        write_fixture((xp / "bin" / "date"), q.DATE_SHIM + "#\n")
        es = {e["group"]: e for e in h.audit_control(q, plan, cache, td / "cache", product) if e["status"] == "PRESENT_BUT_CHANGED"}
        check(set(es) == {"t001.toolchain_input", "t001.dockerfile", "t001.helper"}, "input, Dockerfile and helper drift detected")
        (tc / "in.tar").unlink()
        es = [e for e in h.audit_control(q, plan, cache, td / "cache", product) if e["group"] == "t001.toolchain_input"]
        check(es[0]["status"] == "MISSING_UNRECOVERABLE", "a lost floating-URL input is unrecoverable by digest")


def test_portability():
    """Windows CI regression (PR 133 run 39418624402): worktree markers and logical paths must not depend on the host separator."""
    check(pathlib.Path.write_text.__module__.startswith("pathlib"), "the standard library Path.write_text is not replaced")
    with tempfile.TemporaryDirectory() as td:
        f = pathlib.Path(td) / "x"
        write_fixture(f, "a\nb\n")
        check(f.read_bytes() == b"a\nb\n", "fixtures are exact LF bytes")
    # a CRLF copy of an embedded helper (what a Windows text-mode write produces) is real drift and must be reported, not tolerated
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        cache, commit = make_cache(td)
        product = td / "product"
        (product / "tools").mkdir(parents=True)
        write_fixture(product / "tools" / "tc-x.Dockerfile", "FROM x\n")
        tc = cache / "toolchains" / "tc-x"
        tc.mkdir(parents=True)
        (tc / "in.tar").write_bytes(b"input")
        write_fixture(tc / "Dockerfile", "FROM x\n")
        xp = cache / "xplat"
        (xp / "bin").mkdir(parents=True)
        write_fixture(xp / "ci_alias.py", q.CI_ALIAS)
        write_fixture(xp / "asts_pilot.py", q.ASTS_PILOT)
        (xp / "bin" / "date").write_bytes(q.DATE_SHIM.replace("\n", "\r\n").encode())
        es = {e["group"]: e for e in h.audit_control(q, fake_plan(commit), cache, td / "cache", product) if e["status"] == "PRESENT_BUT_CHANGED"}
        check(set(es) == {"t001.helper"}, "a CRLF helper is detected as changed (byte-exact comparison preserved)")
    check(h.under_worktree("D:\\a\\segarecomp\\.worktrees\\seg-075-t002\\tools\\x.py"), "Windows worktree path detected")
    check(h.under_worktree("/r/.worktrees-harness/control-x/f") and h.under_worktree(pathlib.PureWindowsPath("C:/r/.worktrees/t")),
          "POSIX / PureWindowsPath worktree paths detected")
    check(not h.under_worktree("D:\\a\\worktrees\\x") and not h.under_worktree("/r/.worktreesX/y"), "near-miss names not flagged")
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        cache, commit = make_cache(td)
        es = h.audit_builds(q, fake_plan(commit), cache, td / "cache") + h.audit_sources(q, fake_plan(commit), cache, td / "cache")
        check(all("\\" not in e["logical"] for e in es), "logical paths are POSIX on every platform")


def test_add_refuses_worktree():
    with tempfile.TemporaryDirectory() as td:
        bad = pathlib.Path(td) / ".worktrees" / "seg-075-t002"
        bad.mkdir(parents=True)
        write_fixture((bad / "f"), "x")
        rc = h.main(["add", "--store", str(pathlib.Path(td) / "s"), "--cache-parent", td, "--label", "x", "--tree", f"g={bad}"])
        check(rc == 2, "snapshotting a task-worktree tree is refused")


def test_receipt_is_sanitized():
    """The committed C1 receipt holds aggregates only (no file names of third-party content, no digests of individual files)."""
    p = ROOT / "tools" / "segarecomp_xplat_handoff.c1_receipt.json"
    rec = json.loads(p.read_text())
    check(rec["schema"] == h.RECEIPT_SCHEMA and rec["verification"]["verdict"] == "PASS", "committed C1 receipt passed")
    check(rec["required_problems"] == 0, "no required artifact problem at C1")
    text = json.dumps(rec)
    check("q/src/" not in text and "q/build/" not in text and ".lst" not in text, "receipt carries no per-file paths")
    check(len(rec["manifest_sha256"]) == 64, "receipt names the manifest identity")


def main():
    for t in (test_classify, test_store_and_verify, test_symlinks, test_audit_builds_and_sources, test_cross_run, test_record_code_basis, test_truth_listing_resolution, test_portability, test_audit_control, test_add_refuses_worktree,
              test_receipt_is_sanitized):
        t()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_handoff_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
