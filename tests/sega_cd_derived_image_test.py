#!/usr/bin/env python3
"""SEG-110 (ADR 0254): minimal reproducible proof of the *relocated fragment* producer class (the Earthworm Jim SE frontier class).

Project-authored fixture (tools/sega_cd_bios_loader_fixture.py, cases relocated*): a loaded module copies a 24-byte fragment of its own admitted image to PRG RAM and
runs it there. The window after the fragment is not the source's, so the exact-copy resolver (32-byte anchor, 64-byte run) refuses it (`no_source`) and the
program stops at the fragment. The diagnostic tools/sega_cd_relocation_provenance.py explains the window from the machine's own write stream, a verified writer and
a unique delivered source, and prints the `--disc-image` extent; the existing admission/emitter/C11 path compiles it and the generated code's per-instruction byte
guard keeps every wrong case fail-closed.

  relocated            refusal today -> derived extent equals ground truth (judge only) -> rebuild runs to the end; repeated derivation and build are identical
  guard                wrong source, an extent over a patched copy, an over-long claim, a disc byte changed after admission: nothing wrong ever executes
  emitter              a derived extent under another pass identity is refused (incorrect capture epoch); under the right one it re-validates
  relocated_short      incomplete output: the derived extent is the produced prefix; the program stops at the first uncompiled instruction (typed)
  relocated_patched    the copy was rewritten after it was produced: the diagnostic refuses (stream_overwritten)
  relocated_duplicate  two identical delivered sources: ambiguous_source
  refusals             a window nobody wrote from a stream (generated), an unverified writer, a missing ledger

usage: sega_cd_derived_image_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_loader_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
TOOL = ROOT / "tools" / "sega_cd_relocation_provenance.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def fixture(tmp, case):
    out = tmp / case
    truth = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--case", case, "--out", str(out)], check=True, capture_output=True, text=True).stdout)
    return out, truth


def build(tmp, out, name, extra=()):
    done = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(out / "disc" / "disc.cue"), "--bios", str(out / "bios.bin"),
                           "--work", str(tmp / name), "--discover-extents", "--timeout", "300", *extra], capture_output=True, text=True, timeout=1800)
    return json.loads(done.stdout) if done.stdout.strip() else {"ok": False}


def run_program(summary, out, *extra):
    done = subprocess.run([summary["program"], "--disc-dir", str(out / "disc"), "--bios", str(out / "bios.bin"), "--instruction-budget", "3000000", *extra], capture_output=True, text=True, timeout=300)
    if not done.stdout.strip():
        return done.returncode, {}, None
    report = json.loads(done.stdout)
    if "ga" not in report:      # a refusal before the first instruction (e.g. the disc is not the admitted disc)
        return done.returncode, report, None
    return done.returncode, report, report["ga"]["comstat"][2]      # the module's marker word (COMSTAT2)


def derive(tmp, out, summary, name, verified=True, ledger=True):
    """Run the stopped program once more with its RAM write log (deterministic: the same run as the pass) and ask the diagnostic."""
    work = tmp / name
    log = tmp / (name + ".wlog")
    run_program(summary, out, "--ram-write-log", str(log))
    capture = json.loads((work / "pass" / "capture.json").read_text())
    if not ledger:
        capture["region"]["delivered"] = []
        (tmp / (name + ".capture.json")).write_text(json.dumps(capture))
        cap_path = tmp / (name + ".capture.json")
    else:
        cap_path = work / "pass" / "capture.json"
    cmd = [sys.executable, str(TOOL), "--cue", str(out / "disc" / "disc.cue"), "--capture", str(cap_path), "--region", str(work / "pass" / "region.bin"), "--write-log", str(log)]
    if verified:
        cmd += ["--verified", "sub:%d:%d" % (524288, 4096)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    return done.returncode, json.loads(done.stdout)


def spec_args(spec):
    return ["--disc-image", spec]


def main():
    with tempfile.TemporaryDirectory(prefix="scd-derived-") as tmp:
        tmp = pathlib.Path(tmp)
        out, truth = fixture(tmp, "relocated")
        rel = truth["relocation"]
        want = "sub:%d:%d:%d" % (rel["offset"], rel["base"], rel["length"])
        base = build(tmp, out, "w-base")
        code, report, marker = run_program(base, out)
        check(base.get("ok") and base["frontier"] and base["frontier"]["refusal"] == "no_source" and report["stop"]["class"] == "entry_not_compiled" and
              report["stop"]["pc"] == rel["base"] and marker == 0,
              "refusal today: the exact-copy resolver refuses the relocated fragment (no_source) and the program stops at its first instruction")

        rc, answer = derive(tmp, out, base, "w-base")
        check(rc == 0 and answer.get("ok") and answer["extent"] == want and answer["occurrences"] == 1 and answer["stream_bytes"] == rel["length"],
              "derivation: the write stream, the verified writer and the unique delivered source give the extent equal to the ground truth (used only to judge)")
        rc2, again = derive(tmp, out, base, "w-base")
        check(again == answer, "derivation: repeated runs of the diagnostic give the identical answer")

        derived = build(tmp, out, "w-derived", spec_args(answer["extent"]))
        code, report, marker = run_program(derived, out)
        check(derived.get("ok") and derived["frontier"] is None and report["result"] == "idle" and marker == 0xA001,
              "reconstruction: with the derived extent the generated program runs the relocated fragment to the end and writes its marker")
        derived2 = build(tmp, out, "w-derived2", spec_args(answer["extent"]))
        check(derived2.get("program_identity") == derived.get("program_identity") and derived2["extents"] == derived["extents"],
              "determinism: the same inputs and the same derived extent give the same program identity")

        # ---- the runtime byte guard: nothing wrong ever executes ----
        wrong = ":".join(want.split(":")[:1] + [str(rel["offset"] + 2)] + want.split(":")[2:])
        w = build(tmp, out, "w-wrong", spec_args(wrong))
        code, report, marker = run_program(w, out)
        check(w.get("ok") and marker == 0 and report["stop"]["class"] in ("code_bytes_mismatch", "entry_not_compiled"),
              "wrong source: bytes that are not the live bytes never execute (typed %s, marker unchanged)" % report["stop"]["class"])
        longer = ":".join(want.split(":")[:3] + [str(rel["length"] + 4)])
        w = build(tmp, out, "w-longer", spec_args(longer))
        code, report, marker = run_program(w, out)
        check(w.get("ok") and marker == 0xA001 and report["result"] == "idle",
              "over-long claim: bytes beyond the produced fragment are admitted but not executed by this program; the guard compares what does execute")
        track = out / "disc" / "track01.bin"
        raw = bytearray(track.read_bytes())
        raw[rel["offset"] + 3] ^= 0x01        # one bit of the fragment's source, changed after the extent was admitted
        saved = track.read_bytes()
        track.write_bytes(bytes(raw))
        code, report, marker = run_program(derived, out)
        check(code != 0 and marker != 0xA001, "source changed after admission: the disc identity no longer matches the program; it does not execute (exit %d, %s)" % (code, report.get("error") or report.get("result")))
        track.write_bytes(saved)
        code, report, marker = run_program(derived, out)
        check(marker == 0xA001, "source restored: the same program runs again")

        # ---- the emitter: capture epoch ----
        epochs = [a for a in base["admissions"] if a.startswith("epoch:")]
        modules = [a for a in base["admissions"] if a.startswith("extent:")]
        cue = str(out / "disc" / "disc.cue")

        def emit(admits):
            cmd = [SEGARECOMP, "sega-cd", "emit", "--cue", cue, "--bios", str(out / "bios.bin"), "--out", str(tmp / "x.c")]
            for a in admits:
                cmd += ["--admit", a]
            return subprocess.run(cmd, capture_output=True, text=True)

        identity = base["history"][-1]["program_identity"]
        done = emit(epochs + modules[:1] + ["extent:%s:%s" % (identity, answer["extent"])])
        check(done.returncode == 0, "capture epoch: the derived extent under the identity of the pass that produced it re-validates in the emitter")
        done = emit(epochs + modules[:1] + ["extent:%s:%s" % ("0" * 64, answer["extent"])])
        check(done.returncode != 0 and "provenance_mismatch" in done.stdout, "capture epoch: the same extent under another pass identity is refused (provenance_mismatch)")

        # ---- incomplete output ----
        out_s, truth_s = fixture(tmp, "relocated_short")
        short = build(tmp, out_s, "w-short")
        rc, ans = derive(tmp, out_s, short, "w-short")
        check(rc == 0 and ans["extent"].split(":")[3] == "16", "incomplete output: the derived extent is the produced prefix (16 of 24 bytes), not the intended fragment")
        s2 = build(tmp, out_s, "w-short2", spec_args(ans["extent"]))
        code, report, marker = run_program(s2, out_s)
        check(report["stop"]["class"] == "entry_not_compiled" and report["stop"]["pc"] == 0x30000 + 16 and marker == 0xA001,
              "incomplete output: the program runs the produced prefix and stops with a typed entry_not_compiled at the first byte never produced")

        # ---- changed after production ----
        out_p, truth_p = fixture(tmp, "relocated_patched")
        patched = build(tmp, out_p, "w-patched")
        rc, ans = derive(tmp, out_p, patched, "w-patched")
        check(rc == 8 and ans["error"] == "stream_overwritten", "transformed image: a copy rewritten after it was produced is not claimed (stream_overwritten)")
        pr = truth_p["relocation"]
        forced = build(tmp, out_p, "w-forced", spec_args("sub:%d:%d:%d" % (pr["offset"], pr["base"], pr["length"])))
        code, report, marker = run_program(forced, out_p)
        check(report["stop"]["class"] == "code_bytes_mismatch" and marker == 0,
              "transformed image: even forced as an extent, the patched instruction's bytes differ from the admitted bytes and it never executes (code_bytes_mismatch)")

        # ---- ambiguity ----
        out_d, _ = fixture(tmp, "relocated_duplicate")
        dup = build(tmp, out_d, "w-dup")
        rc, ans = derive(tmp, out_d, dup, "w-dup")
        check(rc == 8 and ans["error"] == "ambiguous_source" and ans["occurrences"] == 2, "ambiguity: two identical delivered sources are refused (ambiguous_source), never chosen by order")

        # ---- never observed / not proven ----
        rc, ans = derive(tmp, out, base, "w-base", verified=False)
        check(rc == 8 and ans["error"] == "writer_not_verified", "unproven producer: a stream written by code that is not an admitted image is refused (writer_not_verified)")
        rc, ans = derive(tmp, out, base, "w-base", ledger=False)
        check(rc == 8 and ans["error"] == "no_delivery_evidence", "unproven source: without a delivery ledger nothing is claimed (no_delivery_evidence)")
        out_g, _ = fixture(tmp, "generated")
        gen = build(tmp, out_g, "w-gen")
        rc, ans = derive(tmp, out_g, gen, "w-gen")
        check(rc == 8 and ans["error"] in ("no_stream", "too_short", "no_source", "writer_not_verified", "stream_overwritten"),
              "generated code: instructions that compute their own immediates are not a delivered run (%s)" % ans.get("error"))
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
