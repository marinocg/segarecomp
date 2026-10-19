#!/usr/bin/env python3
"""SEG-075-T001 (ADR 0159): tests of tools/segarecomp_xplat_qualify.py (standard library only, synthetic data, no network/Docker).

Covers the frozen plan's structural invariants (eleven Tier A candidates, full SHAs, pinned toolchain inputs, an undeclared licence never
passing silently), executable-container probes on synthetic headers, the source CPU-variant scan, deterministic archive-pilot
selection, the privacy guard (a non-ignored path inside a git work tree is refused), recipe evaluation (expected failures need their
signature, author comparisons must be byte-identical, determinism) and the contradiction check of the sanitized manifest.
"""

import copy
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_xplat_qualify as q  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def raises(fn, *args):
    try:
        fn(*args)
    except q.QualifyError:
        return True
    return False


def test_plan():
    plan = q.load_plan()
    ids = [c["id"] for c in plan["candidates"]]
    check(len([c for c in plan["candidates"] if c["tier"] == "A"]) == 11, "eleven Tier A candidates")
    for must in ("emutos", "toslibc", "atarist-toolkit-docker", "space-invaders", "perihelion-m68k-tutorials", "AtariST", "pacman500",
                 "scramble500", "amiga_asmdev_workflow", "amiga-realtime3d", "GODLIB"):
        check(must in ids, f"Tier A candidate {must} present")
    pinned = {c["id"]: c["commit"] for c in plan["candidates"]}
    check(pinned["emutos"] == "02c2db75fc875265c1449efea014450466cf20b7", "EmuTOS pin unchanged")
    check(pinned["GODLIB"] == "ae600aae192f35c8fe8a7e8164ec0e4caaa7e6e9", "GODLIB pin unchanged")
    for c in plan["candidates"]:
        if c["licence_status"] == "undeclared" and c["technical_class"] in ("QUALIFIED", "PARTIALLY_QUALIFIED"):
            check(c["class"] == "LICENSING_REVIEW_REQUIRED", f"{c['id']}: undeclared licence carries the review class")
    # the GNU m68k-linux C compiler (always 68020) is never used to produce corpus C code
    for c in plan["candidates"]:
        for r in c["recipes"]:
            if "m68k-linux-gnu-gcc" in r["script"] or "TARGET_COMPILE=m68k-linux-gnu-" in r["script"]:
                check(r.get("expect") == "failure", f"{c['id']}/{r['name']}: m68k-linux-gnu-gcc only as an expected rejection")

    bad = copy.deepcopy(plan)
    bad["candidates"][0]["commit"] = "02c2db7"
    check(raises(q.validate_plan, bad), "short commit rejected")
    bad = copy.deepcopy(plan)
    perihelion = next(c for c in bad["candidates"] if c["id"] == "perihelion-m68k-tutorials")
    perihelion["class"] = "QUALIFIED"
    check(raises(q.validate_plan, bad), "undeclared licence cannot be QUALIFIED")
    bad = copy.deepcopy(plan)
    bad["toolchains"]["tc-gnu"]["inputs"][0]["sha256"] = "latest"
    check(raises(q.validate_plan, bad), "unpinned toolchain input rejected")
    bad = copy.deepcopy(plan)
    r = next(c for c in bad["candidates"] if c["id"] == "pacman500")["recipes"][0]
    del r["failure_signature"]
    check(raises(q.validate_plan, bad), "expected failure needs a signature")
    bad = copy.deepcopy(plan)
    bad["candidates"] = [c for c in bad["candidates"] if c["id"] != "GODLIB"]
    check(raises(q.validate_plan, bad), "dropping a Tier A candidate is rejected")
    bad = copy.deepcopy(plan)
    hello = next(c for c in bad["candidates"] if c["id"] == "HelloAmi")
    hello["recipes"] = [dict(hello["recipes"][0], expect="failure", failure_signature="x")]
    check(raises(q.validate_plan, bad), "usable class needs a successful recipe")
    bad = copy.deepcopy(plan)
    next(c for c in bad["candidates"] if c["id"] == "galaga").update({"licence_status": "declared", "class": "PARTIALLY_QUALIFIED"})
    check(raises(q.validate_plan, bad), "commercial-derived source never becomes usable, even with a licence")
    bad = copy.deepcopy(plan)
    del bad["candidates"][0]["provenance"]
    check(raises(q.validate_plan, bad), "provenance required")
    for cid in ("galaga", "xevious", "mpatrol", "pacman500", "scramble500"):
        check(next(c for c in plan["candidates"] if c["id"] == cid)["provenance"] == "commercial-derived", f"{cid} commercial-derived")
    bad = copy.deepcopy(plan)
    bad["candidates"][0]["recipes"][0]["toolchain"] = "tc-nope"
    check(raises(q.validate_plan, bad), "unknown toolchain rejected")


def test_probe():
    prg = b"\x60\x1a" + struct.pack(">IIII", 8, 4, 2, 0) + b"\0" * 10 + b"\x4e\x75" * 4 + b"\0" * 4
    p = q.probe_format(prg)
    check(p["format"] == "tos-prg" and p["text"] == 8 and p["data"] == 4 and p["bss"] == 2, "TOS PRG header parsed")
    check(q.probe_format(b"\x00\x00\x03\xf3" + b"\0" * 20)["format"] == "amiga-hunk-exe", "Amiga hunk executable")
    check(q.probe_format(b"\x00\x00\x03\xe7" + b"\0" * 20)["format"] == "amiga-hunk-object", "Amiga hunk object")
    elf = b"\x7fELF" + bytes([1, 2, 1]) + b"\0" * 9 + struct.pack(">HH", 1, 4) + b"\0" * 32
    check(q.probe_format(elf)["format"] == "elf-m68k-relocatable", "big-endian m68k ELF relocatable")
    elf_x86 = b"\x7fELF" + bytes([1, 2, 1]) + b"\0" * 9 + struct.pack(">HH", 2, 3) + b"\0" * 32
    check(q.probe_format(elf_x86)["format"] == "elf-other", "non-m68k ELF not counted as m68k")
    check(q.probe_format(b"!<arch>\nxxxx")["format"] == "ar-archive", "ar archive")
    check(q.probe_format(b":100000000010F300\r\n")["format"] == "intel-hex", "Intel HEX")
    check(q.probe_format(b"HU\0\0" + b"\0" * 60)["format"] == "x68k-x", "X68000 .X")
    check(q.probe_format(struct.pack(">I", 0x00000107) + b"\0" * 28)["format"] == "aout", "a.out OMAGIC")
    check(q.probe_format(b"\x4e\x71\x4e\x75")["format"] == "raw", "raw fallback")
    sym1 = prg + b"SYM1"
    sym2 = prg + b"SYM2"
    check(q.code_digest(sym1) == q.code_digest(sym2), "TOS code digest ignores the symbol table")
    check(q.code_digest(prg) != q.code_digest(prg[:28] + b"\x4e\x71" + prg[30:]), "TOS code digest covers text")
    check(q.code_digest(b"abc") != q.code_digest(b"abd"), "raw code digest covers every byte")
    data_changed = bytearray(prg)
    data_changed[28 + 8] ^= 0xFF
    check(q.code_digest(prg) != q.code_digest(bytes(data_changed)), "TOS code digest covers the data segment")


def test_cpu_scan():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "a.s").write_text("\tmove.l d0,d1\n\trts\n")
        (root / "b.S").write_text("\t.chip 68030\n\tpmove tc,(a0)\n")
        (root / "c.s").write_text("start:\tmovec vbr,d0\n")
        (root / "d.c").write_text("int movec_count; /* movec mentioned in a comment only */\n")
        (root / "e.s").write_text("\tfmove.x fp0,fp1\n")
        check(q.cpu_variant_files(root) == 3, f"CPU-variant scan counts directive/mnemonic files: {q.cpu_variant_files(root)}")
    positives = ["\tmuls.l d0,d1", "\tdivu.l d2,d3", "\textb.l d0", "\tmove.w (a0,d0.w*2),d1", "\tmove.l ([4,a0]),d0", "\tbra.l far",
                 "\tbkpt #0", "\tmove.w ccr,d0", "\tpack d0,d1,#0", "\ttrapne", "\topt p=68020", "lbl:\tmovec vbr,d0", "\t.chip 68030",
                 "\tfmove.x fp0,fp1", "\tcas.l d0,d1,(a0)", "\tbne.l x"]
    negatives = ["cas = 1", "faddress\tdc.l 0", "\tmove.l d0,d1 ; moves the bytes", "* 68030 users should", "fsubmit:\trts",
                 "\tmove.w d0,ccr", "\tmuls.w d0,d1", "\tbra.w x", "\tlea (a0,d0.w),a1", "\tdbra d0,loop", "\tmulu.w #2*4,d0",
                 "\tmove.l d0,d1 ; (a0,d0.w*2)", "fbase:\tdc.w 1"]
    for line in positives:
        check(q.CPU_VARIANT_RE.search(line) is not None, f"CPU heuristic finds {line!r}")
    for line in negatives:
        check(q.CPU_VARIANT_RE.search(line) is None, f"CPU heuristic ignores {line!r}")
    with tempfile.TemporaryDirectory() as d:
        (pathlib.Path(d) / "x.c").write_text('asm("movec vbr,d0");\n')
        check(q.cpu_variant_files(pathlib.Path(d)) == 0, "C sources are outside the assembly heuristic")


def test_pilot():
    tree = []
    for i in range(30):
        tree += [f"ASM/A{i}/P/x.S", f"ASM/A{i}/P/x.PRG"]
    tree += ["ASM/B/P/a.s", "ASM/B/P/b.s", "ASM/B/P/a.prg",       # two sources: excluded
             "ASM/C/P/a.s",                                       # no executable: excluded
             "C/D/P/a.s", "C/D/P/a.prg",                          # wrong root
             "ASM/E/P/a.s", "ASM/E/P/a.tos"] + [f"ASM/E/P/f{k}" for k in range(9)]  # too many files
    one = q.select_pilot(tree, "ASM", 10, 75, 6)
    two = q.select_pilot(list(reversed(tree)), "ASM", 10, 75, 6)
    check(one == two and len(one) == 10, "pilot selection is deterministic and order-independent")
    check(all(d.startswith("ASM/A") for d, _s, _e in one), "pilot excludes ambiguous subprojects")
    check(q.select_pilot(tree, "ASM", 10, 76, 6) != one, "seed changes the sample")


def test_guard():
    with tempfile.TemporaryDirectory() as d:
        repo = pathlib.Path(d) / "repo"
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        (repo / ".gitignore").write_text("private/\n")
        check(raises(q.require_private, repo / "work"), "non-ignored directory inside a work tree refused")
        try:
            q.require_private(repo / "private" / "cache")
            ok = True
        except q.QualifyError:
            ok = False
        check(ok, "ignored directory accepted")
        outside = pathlib.Path(d) / "elsewhere"
        try:
            q.require_private(outside)
            ok = True
        except q.QualifyError:
            ok = False
        check(ok, "directory outside any work tree accepted")
    check(raises(q.require_private, ROOT / "tools" / "xplat-cache"), "tracked product directory refused")


def test_pin_check():
    with tempfile.TemporaryDirectory() as d:
        repo = pathlib.Path(d)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        (repo / "a.s").write_text("\trts\n")
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        subprocess.run(["git", "-C", str(repo), "add", "a.s"], check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", "x"], check=True, env=env)
        head = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
        check(q.observed_source(repo, head) == head, "pinned clean checkout accepted")
        check(raises(q.observed_source, repo, "0" * 40), "different commit refused")
        (repo / "a.s").write_text("\tnop\n")
        check(raises(q.observed_source, repo, head), "modified checkout refused")


def test_date_shim():
    check("XPLAT_DATE" in q.DATE_SHIM and q.RUN_DATES["a"][:4] != q.RUN_DATES["b"][:4], "the two runs see different years")


def _run(rc, log, outputs, compares=()):
    return {"rc": rc, "log": log, "outputs": outputs, "author_compare": list(compares), "pilot": {}}


def test_evaluate():
    out = [{"path": "out/a.tos", "size": 10, "sha256": "1" * 64, "format": "tos-prg"}]
    r = {"name": "x", "toolchain": "tc-gnu", "script": "", "outputs": ["out/*.tos"]}
    res = q.evaluate_recipe(r, _run(0, "", out), _run(0, "", out))
    check(res["matches_expectation"] and res["deterministic"] and res["formats"] == {"tos-prg": 1}, "success recipe")
    other = [dict(out[0], sha256="2" * 64)]
    check(not q.evaluate_recipe(r, _run(0, "", out), _run(0, "", other))["deterministic"], "nondeterminism detected")
    check(not q.evaluate_recipe(r, _run(0, "", []), _run(0, "", []))["matches_expectation"], "missing outputs fail")
    check(not q.evaluate_recipe(r, _run(2, "", out), None)["matches_expectation"], "nonzero exit fails")
    f = {"name": "y", "toolchain": "tc-gnu", "script": "", "expect": "failure", "failure_signature": "could not open"}
    check(q.evaluate_recipe(f, _run(1, "fatal error 13: could not open <x.i>", []), None)["matches_expectation"], "expected failure")
    check(not q.evaluate_recipe(f, _run(1, "syntax error", []), None)["matches_expectation"], "failure needs its signature")
    check(not q.evaluate_recipe(f, _run(0, "could not open", []), None)["matches_expectation"], "unexpected success is a contradiction")
    a = dict(r, author_compare=[["out/a.tos", "bin/a.tos"]])
    good = q.evaluate_recipe(a, _run(0, "", out, [{"ours": "out/a.tos", "author": "bin/a.tos", "identical": True}]), _run(0, "", out))
    bad = q.evaluate_recipe(a, _run(0, "", out, [{"ours": "out/a.tos", "author": "bin/a.tos", "identical": False}]), _run(0, "", out))
    check(good["matches_expectation"] and good["author_identical"] == 1, "author comparison identical")
    check(not bad["matches_expectation"], "author comparison mismatch is a contradiction")
    two = out + [{"path": "out/t.o", "size": 4, "sha256": "3" * 64, "format": "aout"}]
    two_b = out + [{"path": "out/t.o", "size": 4, "sha256": "4" * 64, "format": "aout"}]
    v = dict(r, outputs=["out/*"], volatile_outputs={"out/t.o": "__TIME__"})
    check(q.evaluate_recipe(v, _run(0, "", two), _run(0, "", two_b))["deterministic"], "declared volatile output excluded")
    check(not q.evaluate_recipe(dict(v, volatile_outputs={}), _run(0, "", two), _run(0, "", two_b))["deterministic"], "undeclared drift found")
    m = dict(r, min_outputs=2)
    check(not q.evaluate_recipe(m, _run(0, "", out), _run(0, "", out))["matches_expectation"], "min_outputs enforced")


def test_manifest():
    plan = q.load_plan()
    records = {}
    for c in plan["candidates"]:
        recs = []
        for r in c["recipes"]:
            recs.append({"name": r["name"], "toolchain": r["toolchain"], "expect": r.get("expect", "success"),
                         "observed": r.get("expect", "success"), "matches_expectation": True, "deterministic": True})
        records[c["id"]] = {"id": c["id"], "recipes": recs}
    m = q.make_manifest(plan, records, {})
    check(m["summary"]["contradictions"] == [], "consistent records give no contradiction")
    check(sum(m["summary"]["by_class"].values()) == len(plan["candidates"]), "every candidate classified once")
    check("atarist-toolkit-docker" not in m["summary"]["usable_without_review"], "infrastructure is not a usable lineage")
    check("BattleIsleEditor" in m["summary"]["usable_without_review"], "qualified program usable")
    check("perihelion-m68k-tutorials" not in m["summary"]["usable_without_review"], "licence review excludes from usable set")
    text = json.dumps(m)
    check('"script"' not in text and '"log"' not in text, "manifest carries no scripts or logs")
    records["BattleIsleEditor"]["recipes"][0]["matches_expectation"] = False
    m2 = q.make_manifest(plan, records, {})
    check(any(x.startswith("BattleIsleEditor/") for x in m2["summary"]["contradictions"]), "contradiction reported")
    records["BattleIsleEditor"]["recipes"][0]["matches_expectation"] = True
    records["BattleIsleEditor"]["recipes"][0]["deterministic"] = False
    check(any("differ between the two runs" in x for x in q.make_manifest(plan, records, {})["summary"]["contradictions"]),
          "nondeterministic success is a contradiction")
    records["BattleIsleEditor"]["recipes"][0]["deterministic"] = True
    records["HelloAmi"]["commit"] = "0" * 40
    check(any(x.startswith("HelloAmi: built from") for x in q.make_manifest(plan, records, {})["summary"]["contradictions"]),
          "record from a different commit is a contradiction")
    records["HelloAmi"]["commit"] = next(c["commit"] for c in plan["candidates"] if c["id"] == "HelloAmi")
    check("galaga" not in q.make_manifest(plan, records, {})["summary"]["usable_without_review"], "commercial-derived never usable")
    base = q.make_manifest(plan, records, {})
    other = copy.deepcopy(base)
    other["candidates"][0]["recipes"].append({"name": "extra", "observed": "success", "outputs": []})
    check(q.compare_manifests(base, base) == [], "identical manifests compare equal")
    check(any("extra" in d for d in q.compare_manifests(base, other)), "recipe drift detected")
    o1 = copy.deepcopy(base); o2 = copy.deepcopy(base)
    o1["candidates"][0]["recipes"][0]["outputs"] = [{"path": "x", "sha256": "1" * 64, "code_sha256": "1" * 64}]
    o2["candidates"][0]["recipes"][0]["outputs"] = [{"path": "x", "sha256": "2" * 64, "code_sha256": "2" * 64}]
    check(any("output x differs" in d for d in q.compare_manifests(o1, o2)), "digest drift detected")
    o2["candidates"][0]["recipes"][0]["volatile_outputs"] = {"x": "date"}
    o1["candidates"][0]["recipes"][0]["volatile_outputs"] = {"x": "date"}
    check(q.compare_manifests(o1, o2) == [], "declared volatile output ignored")
    del records["emutos"]
    check(any(x.startswith("emutos:") for x in q.make_manifest(plan, records, {})["summary"]["contradictions"]), "missing record reported")


def test_committed_manifest():
    path = ROOT / "tools" / "segarecomp_xplat_qualify.manifest.json"
    if not path.exists():
        return
    m = json.loads(path.read_text(encoding="utf-8"))
    plan = q.load_plan()
    check(m["summary"]["contradictions"] == [], "committed manifest has no contradiction")
    check([c["id"] for c in m["candidates"]] == [c["id"] for c in plan["candidates"]], "manifest matches the plan inventory")
    for c, p in zip(m["candidates"], plan["candidates"]):
        check(c["commit"] == p["commit"] and c["class"] == p["class"], f"{c['id']}: manifest identity/class equals plan")
        for r in c["recipes"]:
            check(r["matches_expectation"], f"{c['id']}/{r['name']}: executed evidence matches the plan")
    text = path.read_text(encoding="utf-8")
    check('"log"' not in text and '"script"' not in text, "committed manifest is sanitized")


def main():
    for t in (test_plan, test_probe, test_cpu_scan, test_pilot, test_guard, test_pin_check, test_date_shim, test_evaluate, test_manifest,
              test_committed_manifest):
        t()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_qualify_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
