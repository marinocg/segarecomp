#!/usr/bin/env python3
"""SEG-132 (ADR 0279) Checkpoint F: Sega CD save states, visual rewind and the relocated frame-step key inside the REAL SDL3 viewer process (dummy video and audio drivers).

usage: sega_cd_viewer_states_test.py <segarecomp> <c-compiler> <source-root>

The project-authored CD-DA fixture (tools/sega_cd_cdda_fixture.py: a BIOS-like main program releasing a sub CPU that drives the CD drive and audio) is built into its REAL viewer variant
(scd_viewer.c + the shared SDL3 host). The scripted keyboard (SEGARECOMP_VIEWER_KEY_SCRIPT) queues real SDL key events that the shared host consumes exactly as a person's keys:
F5 quick save, F8 quick load, F7 the rewind surface (Left / Return / Escape inside it), F6 hold, F9 single step. The final machine state of every session is read from
SEGARECOMP_VIEWER_STATE_REPORT (the canonical capture's digest, machine time and onset count). Proved:
  * F5 in one process, F8 in a SEPARATE process: the final machine state equals the uninterrupted run's;
  * F7 rewind: confirming goes back to an earlier moment (a deterministic new branch), cancelling changes nothing, nothing is written to disk;
  * a state of another disc / a corrupt file is refused and the game runs on unchanged; a replay / recording session offers no states;
  * F6 holds the machine and F9 -- not F7 -- advances exactly one frame while held; F7 never steps.
Skipped (exit 0, "SKIP") when SDL3 is not installed.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_bios_build as build_driver  # noqa: E402

DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
BUDGET = 400_000_000
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def keys(*events):
    """events: (poll, key[, 'quit']) -> script text: a press and a release of `key` at that poll."""
    out = []
    for e in events:
        if len(e) == 2 and e[1] in ("quit", "rewind_yes", "rewind_no"):
            out.append("%d %s x" % (e[0], e[1]))
        else:
            out.append("%d down %s" % (e[0], e[1]))
            out.append("%d up %s" % (e[0], e[1]))
    return "\n".join(out) + "\n"


def main():
    if build_driver.find_sdl3() is None:
        print("SKIP: SDL3 not available")
        return 0
    with tempfile.TemporaryDirectory(prefix="scd-viewer-states-") as tmp:
        tmp = pathlib.Path(tmp)
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_cdda_fixture.py"), "build", "--out", str(tmp / "fx")], check=True, capture_output=True, text=True)
        disc, bios = tmp / "fx" / "disc", tmp / "fx" / "bios.bin"
        built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios", str(bios), "--work", str(tmp / "work"),
                                "--pass-set", "main:d6=3", "--budget", "60000000", "--timeout", "900", "--viewer"], capture_output=True, text=True, timeout=3000)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok") and summary.get("viewer", {}).get("host") == "sdl3", "the real SDL3 viewer variant builds" + ("" if summary.get("ok") else ": " + (built.stdout + built.stderr)[-400:]))
        if not summary.get("ok"):
            return 1
        viewer = pathlib.Path(summary["viewer"]["program"])
        work = tmp / "run"
        work.mkdir()

        def session(script, save=None, env_extra=None, args=(), tag="s"):
            script_file, report = work / (tag + ".keys"), work / (tag + ".json")
            script_file.write_text(script)
            if report.exists():
                report.unlink()
            env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_VIEWER_UNTHROTTLED="1", SEGARECOMP_VIEWER_NO_STOP_SCREEN="1",
                       SEGARECOMP_VIEWER_SETTINGS=str(work / "viewer-settings.cfg"), SEGARECOMP_VIEWER_KEY_SCRIPT=str(script_file), SEGARECOMP_VIEWER_STATE_REPORT=str(report))
            for name in ("SEGARECOMP_SAVE_DIR", "SEGARECOMP_REWIND", "SEGARECOMP_REWIND_INTERVAL_MS"):
                env.pop(name, None)
            if save is not None:
                env["SEGARECOMP_SAVE_DIR"] = str(save)
            env.update(env_extra or {})
            r = subprocess.run([str(viewer), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", str(BUDGET), "--set", "main:d6=400", *map(str, args)],
                               capture_output=True, text=True, timeout=300, env=env)
            rep = json.loads(report.read_text()) if report.exists() else None
            events = [json.loads(l.split(" ", 1)[1]) for l in r.stderr.splitlines() if l.startswith("VIEWER_STATE ")]
            return r, rep, events

        # ---- calibration ----
        r, plain60, _ = session(keys((60, "quit")), tag="p60")
        check(r.returncode == 0 and plain60 is not None and plain60["onsets"] >= 59, "plain run: exit 0 at onset %s" % (plain60 and plain60["onsets"]))
        r, plain100, _ = session(keys((100, "quit")), tag="p100")
        check(plain100 is not None and plain100["onsets"] > plain60["onsets"], "plain run to 100 polls: onset %s" % (plain100 and plain100["onsets"]))
        r, again, _ = session(keys((100, "quit")), tag="p100b")
        check(again == plain100, "the plain run is deterministic (identical final canonical state)")

        # ---- F5 in one process, F8 in another ----
        save = tmp / "save"
        r, rep, ev = session(keys((30, "F5"), (100, "quit")), save, tag="save")
        check(rep == plain100 and ev and ev[0]["event"] == "save" and ev[0]["status"] == "ok", "F5 at poll 30 saves without perturbing the run: %s" % ev)
        check((save / "states" / "quick.sgstate").is_file(), "the quick slot file is in the game's folder")
        r, rep, ev = session(keys((5, "F8"), (75, "quit")), save, tag="load")
        check(ev and ev[0]["event"] == "load" and ev[0]["status"] == "ok", "F8 in a separate process loads the state: %s" % ev)
        check(rep is not None and rep["canonical_sha256"] == plain100["canonical_sha256"] and rep["onsets"] == plain100["onsets"],
              "load in a new process reproduces the uninterrupted run's final machine state exactly (onset %s vs %s)" % (rep and rep["onsets"], plain100["onsets"]))

        # ---- refusals ----
        damaged = tmp / "damaged"
        (damaged / "states").mkdir(parents=True)
        blob = bytearray((save / "states" / "quick.sgstate").read_bytes())
        blob[700] ^= 1
        (damaged / "states" / "quick.sgstate").write_bytes(bytes(blob))
        r, rep, ev = session(keys((5, "F8"), (60, "quit")), damaged, tag="corrupt")
        check(ev and ev[0]["status"] == "failed" and rep is not None and rep["canonical_sha256"] == plain60["canonical_sha256"], "a corrupt state is refused and the game runs on unchanged: %s" % ev)
        r, rep, ev = session(keys((10, "F5"), (60, "quit")), tmp / "rec", args=("--record-input", work / "rec.txt"), tag="rec")
        check(not ev and not (tmp / "rec" / "states").exists(), "a recording session offers no save states and writes none: %s" % ev)
        r, rep, ev = session(keys((10, "F5"), (60, "quit")), None, tag="nofolder")
        check(not ev and rep is not None and rep["canonical_sha256"] == plain60["canonical_sha256"], "without a save folder F5 does nothing (the toast says why)")

        # ---- rewind ----
        env = {"SEGARECOMP_REWIND_INTERVAL_MS": "100"}
        norewind = tmp / "norewind"
        rewind_yes = [(60, "rewind_yes")]   # the surface opens as F7 does; Left, Left, Return are real queued key events consumed by the real surface loop
        rewind_no = [(60, "rewind_no")]     # three moments back, then Escape
        r, cancel, ev = session(keys(*rewind_no, (100, "quit")), norewind, env, tag="cancel")
        check(cancel is not None and cancel["canonical_sha256"] == plain100["canonical_sha256"] and not [e for e in ev if e["event"] == "rewind"],
              "F7, three moments back, Escape: nothing changes (the final machine state equals the plain run's)")
        r, back, ev = session(keys(*rewind_yes, (100, "quit")), norewind, env, tag="back")
        check(len([e for e in ev if e["event"] == "rewind" and e["status"] == "ok"]) == 1, "F7, two moments back, Return restores one moment: %s" % ev)
        check(back is not None and back["onsets"] < plain100["onsets"] and back["onsets"] > 50, "the guest resumed from an EARLIER moment and kept running (onset %s vs %s)" % (back and back["onsets"], plain100["onsets"]))
        r, back2, ev = session(keys(*rewind_yes, (100, "quit")), norewind, env, tag="back2")
        check(back2 == back, "the rewound future is deterministic (an identical second process)")
        check(not (norewind / "states").exists(), "rewind and cancelled browsing wrote nothing to disk")
        stats = [l for l in r.stderr.splitlines() if l.startswith("VIEWER_REWIND_STATS ")]
        st = json.loads(stats[0].split(" ", 1)[1]) if stats else {}
        check(st.get("captures", 0) >= 5 and st.get("restores") == 1 and st.get("last_capture_bytes", 0) > 1_000_000, "rewind statistics: %s" % st)
        print("      measured: %s" % {k: st.get(k) for k in ("last_capture_bytes", "capture_ns_total", "capture_ns_max", "captures", "retained_span_ms")})

        # ---- F6 holds, F9 (not F7) steps ----
        r, held, _ = session(keys((10, "F6"), (20, "F9"), (30, "F6"), (40, "quit")), tag="step")
        check(held is not None and held["onsets"] == 21, "F6 holds, F9 steps exactly one frame, F6 resumes: 10 + 1 + 10 = 21 frames (onset %s)" % (held and held["onsets"]))
        r, f7, _ = session(keys((10, "F6"), (20, "F7"), (25, "F7"), (40, "quit")), None, {"SEGARECOMP_REWIND": "0"}, tag="f7")
        check(f7 is not None and f7["onsets"] == 10, "F7 never steps the held machine (rewind off: onset stays %s)" % (f7 and f7["onsets"]))

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
