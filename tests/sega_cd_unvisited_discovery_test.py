#!/usr/bin/env python3
"""SEG-097-T001 (ADR 0206): static discovery of disc-loaded modules that no run has loaded - the pre-registered matrix.

The project-authored `unvisited` fixture disc (tools/sega_cd_unvisited_fixture.py) boots a loader that, chosen by a runtime machine input,
loads module A (with a non-code header), B (same address as A) or C (two bases). `segarecomp sega-cd discover-modules` is given the disc and
nothing else; the fixture's expected layout (the oracle) is read only here and never reaches the tool. The discovered module list is admitted
by the unchanged `emit --modules`, the program is compiled once, and the SAME binary then runs every route.

usage: sega_cd_unvisited_discovery_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []
checks = [0]
BUDGET = 2_000_000


def check(ok, label):
    checks[0] += 1
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def discover(cue, out=None):
    args = [SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(cue)] + (["--out", str(out)] if out else [])
    proc = subprocess.run(args, capture_output=True, text=True, timeout=120)
    return proc.returncode, json.loads(proc.stdout)


def build_case(tmp, case):
    directory = tmp / ("disc-" + case)
    expected = common.build_disc(directory, "unvisited", "cooked", "--unvisited-case", case)
    return directory, expected


def route(exe, disc, choice, tmp, label):
    extra = ["--set", "main:d7=%d" % choice] if choice else []
    coverage = tmp / ("coverage-%s.txt" % label)
    code, report = common.run(exe, disc, BUDGET, *extra, "--coverage", str(coverage))
    words = common.result_words(report, 6) if "result_block" in report else None
    executed = set()
    if coverage.exists():
        for line in coverage.read_text().splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[0] == "1":
                executed.add(int(fields[1], 16) if not fields[1].isdigit() else int(fields[1]))
    return code, report, words, executed


def main():
    with tempfile.TemporaryDirectory(prefix="scd-unvisited-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        disc, oracle = build_case(tmp, "base")
        cue = disc / "disc.cue"
        sha = {k: v["sha256"] for k, v in oracle["layout"].items()}

        # -- 1/3/4/5/6/7: discovery from the disc alone ---------------------------------------------------------------------------
        manifest = tmp / "modules.txt"
        code, found = discover(cue, manifest)
        mods = {m["block"]: m for m in found["modules"]}
        check(code == 0 and found["ok"] and found["complete"] and len(mods) == 3, "base: three modules discovered from the disc alone (exit 0, complete)")
        for kind in "ABC":
            want = oracle["modules"][kind]
            got = mods.get(want["lba"], {})
            check(got.get("sha256") == sha[kind] and got.get("bases") == want["bases"] and got.get("entries") == want["entries"] and
                  [got.get("range", {}).get("offset"), got.get("range", {}).get("length")] == want["range"],
                  "module %s: source sector, execution bases, entry roots and executable subrange equal the oracle (%s)" % (kind, want["range"]))
        check(oracle["modules"]["A"]["range"][0] == 0x40 and mods[oracle["lba"]["A"]]["range"]["offset"] == 0x40,
              "A: the 64-byte non-code header is outside the executable subrange (range starts at the entry, not at 0)")
        check(len(mods[oracle["lba"]["C"]]["bases"]) == 2, "C: one image with two verified bases")
        check(oracle["lba"]["D"] not in mods and any(r["reason"] == "loaded_not_executed" for r in found["refusals"]),
              "D (data loaded, never executed) is reported loaded_not_executed and admits nothing")
        check(oracle["lba"]["X"] not in mods and all(o["lba"] != oracle["lba"]["X"] for o in found["operations"]),
              "decoy X (a valid instruction stream no loader names) is neither an operation nor a module")
        text = manifest.read_text()
        check("file " not in text and ".BIN" not in text and not re.search(r"(?i)magic", text),
              "the discovered list names no ISO9660 path or file name: the disc has no ISO9660 volume at all")

        # -- 14: determinism --------------------------------------------------------------------------------------------------------
        code2, found2 = discover(cue)
        check(found2 == found and (tmp / "modules.txt").read_text() == text, "two discoveries of one disc are byte-identical (list, identity, report)")

        # -- 15: nothing but the boot images and the proven sectors influences the result -----------------------------------------
        other = tmp / "disc-unrelated"
        shutil.copytree(disc, other)
        track = other / "track01.bin"
        data = bytearray(track.read_bytes())
        for lba in (20, 33, 34):  # sectors no loader names, including the neighbours of the decoy
            data[lba * 2048:(lba + 1) * 2048] = bytes((lba * 7 + i) & 0xFF for i in range(2048))
        data[oracle["lba"]["X"] * 2048 + 100] ^= 0xFF  # and the decoy itself
        track.write_bytes(bytes(data))
        _, found3 = discover(other / "disc.cue")
        def strip(f):
            return f["modules"], f["operations"], f["refusals"]
        check(strip(found3) == strip(found) and found3["disc_identity"] != found["disc_identity"],
              "altering sectors that no loader operation names changes nothing but the disc identity (discovery reads only boot images and proven sectors)")

        # -- build once, run every route with the SAME binary (2/3) -----------------------------------------------------------------
        c_path = tmp / "program.c"
        rc, emitted = builder.emit(cue, c_path, "--modules", str(manifest))
        check(rc == 0 and emitted.get("ok") and emitted.get("overlay_groups", 0) >= 1, "the discovered list is admitted unchanged by `emit --modules`")
        rc2, emitted2 = builder.emit(cue, tmp / "program2.c", "--modules", str(manifest))
        check(emitted2.get("program_identity") == emitted.get("program_identity") and (tmp / "program2.c").read_bytes() == c_path.read_bytes(),
              "repeat builds from the discovered list give the identical program identity and byte-identical C")
        exe = tmp / ("program" + common.EXE)
        builder.compile(c_path, exe)
        before = hashlib.sha256(exe.read_bytes()).hexdigest()
        code_a, rep_a, words_a, ex_a = route(exe, disc, 0, tmp, "a")
        check(code_a == 0 and words_a == [1, 0, 0, 0xAAAA, 0, 0], "default route (the build-time route): A runs, B and C never execute: %s" % words_a)
        code_b, rep_b, words_b, ex_b = route(exe, disc, 2, tmp, "b")
        check(code_b == 0 and words_b == [0, 1, 0, 0, 0xBBBB, 0], "the previously unvisited module B runs from the same binary: %s" % words_b)
        code_c, rep_c, words_c, ex_c = route(exe, disc, 3, tmp, "c")
        check(code_c == 0 and words_c == [0, 0, 2, 0, 0, 0xCCCC], "the previously unvisited module C runs at both of its bases (count 2): %s" % words_c)
        check(hashlib.sha256(exe.read_bytes()).hexdigest() == before, "the binary is unchanged by running: nothing is compiled after the run starts")
        b_range = range(0x080000, 0x080000 + 0x1C)
        check(ex_a and not any(pc in b_range for pc in ex_a) and any(pc in b_range for pc in ex_b),
              "coverage: B's instructions execute only on the route that selects B (they were compiled before they were ever executed)")

        # -- 11: wrong digest -------------------------------------------------------------------------------------------------------
        bad = tmp / "modules-bad-digest.txt"
        bad.write_text(text.replace(sha["B"], sha["C"], 1), newline="\n")
        rc, out = builder.emit(cue, tmp / "bad.c", "--modules", str(bad))
        check(rc == 5 and out.get("error") == "digest_mismatch", "a discovered list whose digest names other bytes is refused at admission: digest_mismatch")
        other_id = tmp / "modules-bad-disc.txt"
        other_id.write_text(re.sub(r"disc [0-9a-f]{64}", "disc " + "0" * 64, text), newline="\n")
        rc, out = builder.emit(cue, tmp / "bad2.c", "--modules", str(other_id))
        check(rc == 5 and out.get("error") == "disc_identity_mismatch", "a list discovered on another disc is refused: disc_identity_mismatch")

        # -- 4b/12: the executable subrange is the authority, not the whole extent --------------------------------------------------
        for case, expect in (("header_patched", "runs"), ("overwritten", "stops")):
            d, o = build_case(tmp, case)
            m = tmp / ("modules-%s.txt" % case)
            rc, f = discover(d / "disc.cue", m)
            exe_case = tmp / ("program-%s%s" % (case, common.EXE))
            rc, emitted_case = builder.emit(d / "disc.cue", tmp / ("p-%s.c" % case), "--modules", str(m))
            builder.compile(tmp / ("p-%s.c" % case), exe_case)
            code, report, words, _ = route(exe_case, d, 0, tmp, case)
            if expect == "runs":
                check(f["ok"] and code == 0 and words == [1, 0, 0, 0xAAAA, 0, 0],
                      "header_patched: a write into the non-executable header does not disturb liveness (the header is outside the mapping): %s" % words)
            else:
                stop = report.get("stop", {}).get("class")
                check(f["ok"] and code != 0 and stop in ("image_bytes_mismatch", "code_bytes_mismatch", "image_partially_overwritten"),
                      "overwritten: a word changed inside the executable range ends in a typed stop, the patched instruction never runs (%s)" % stop)
                check(words is None or words[0] == 0, "overwritten: A's counter stays 0 (no instruction of the wrong image retired)")

        # -- 8/9/10: refusals -------------------------------------------------------------------------------------------------------
        for case, reason in (("unknown_lba", "lba_unknown"), ("unknown_dest", "destination_unknown"), ("prg_dest", "destination_unsupported")):
            d, o = build_case(tmp, case)
            rc, f = discover(d / "disc.cue", tmp / ("m-%s.txt" % case))
            names = {m["block"] for m in f["modules"]}
            check(any(r["reason"] == reason for r in f["refusals"]), "%s: the unprovable load is refused as %s" % (case, reason))
            check(o["lba"]["A"] in names and o["lba"]["B"] in names, "%s: the independent loads (A, B) are still discovered" % case)
            c_bases = [m["bases"] for m in f["modules"] if m["block"] == o["lba"]["C"]]
            want = {"unknown_lba": [], "unknown_dest": [[0x0A0000]], "prg_dest": []}[case]
            check(c_bases == want, "%s: the mappings of C that are proven are exactly %s (got %s)" % (case, want, c_bases))
        d, o = build_case(tmp, "recursion")
        rc, f = discover(d / "disc.cue")
        check(rc == 9 and not f["ok"] and not f["complete"] and any(r["reason"] == "scan_limit_exceeded" for r in f["refusals"]) and not f["modules"],
              "bounded: unbounded recursion ends in scan_limit_exceeded, complete=false, no module proposed")
        rc, out = (lambda p: (p.returncode, p.stdout))(subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(cue), "--modules", str(manifest)],
                                                                      capture_output=True, text=True))
        check(rc == 2, "discovery accepts no module manifest, coordinates or input script (usage error)")
    print("%d checks, %d failures" % (checks[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
