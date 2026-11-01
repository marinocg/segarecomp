#!/usr/bin/env python3
"""SEG-078-T001 (ADR 0204): the Sega CD program in the REAL shared SDL3 viewer (dummy video driver, dummy audio, no wall-clock waiting).

The live fixture (tools/sega_cd_bios_live_fixture.py) is built with its viewer variant (`sega_cd_bios_build.py --viewer`): the unmodified
generated C linked with platforms/sega-cd/viewer, the Genesis viewer's presenter and pacer and libs/viewer. Keyboard events are pushed onto
SDL's queue by the viewer's automation seam (SEGARECOMP_VIEWER_KEY_SCRIPT, the launcher GUI-script idea), so the real default key map, the
shared host's service loop, the F6/F7 pause and step keys and the window close are exercised. Checked: the viewer opens and presents every
frame; the arrow keys reach the pad (the captured frame is the colour of RIGHT); the recording replays headlessly to the identical report;
F6/F7 hold and step without changing the result; the window close leaves with exit 0 after a recording; the headless program is unchanged.

Skipped (exit 0, "SKIP") when SDL3 is not installed or the host is not Linux (the SDL3 CI job on Linux runs it).

usage: sega_cd_viewer_sdl_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_common as common  # noqa: E402
import sega_cd_bios_build as build_driver  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_live_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
FRAME = 262 * 3420 * 500_000
FIRST_ONSET = 111_858 * 500_000
BUDGET = 400_000_000
failures = []
RIGHT, UP = 0x08, 0x01


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def flat_digest(word):
    colour = (((word >> 9) & 7) << 6) | (((word >> 5) & 7) << 3) | ((word >> 1) & 7)
    return hashlib.sha256(struct.pack("<HH", 256, 224) + struct.pack("<H", (1 << 9) | colour) * (256 * 224)).hexdigest()




def backdrop(high, low):
    """The CRAM word the fixture's handler writes for the raw TH-high / TH-low bytes (the documented formula)."""
    p, q = (~high) & 0x3F, (~low) & 0x30
    return ((p & 7) << 1) | ((p & 0x38) << 2) | (q << 5)


def run(exe, args, env, timeout=300):
    out = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=timeout, env={**os.environ, **env})
    return out.returncode, out


def main():
    if sys.platform != "linux" or build_driver.find_sdl3() is None:
        print("SKIP: SDL3 or a Linux host is not available")
        return 0
    with tempfile.TemporaryDirectory(prefix="scd-sdl-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        common.build_disc(disc, "comm")
        fx = tmp / "fx"
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx)], check=True, capture_output=True, text=True)
        bios = fx / "bios.bin"
        built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"),
                                "--bios", str(bios), "--work", str(tmp / "work"), "--pass-set", "main:d6=3", "--viewer"],
                               capture_output=True, text=True, timeout=1800)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok") and summary["viewer"]["host"] == "sdl3", "the viewer variant builds against SDL3" + ("" if summary.get("ok") else ": %s" % built.stdout[-300:]))
        if not summary.get("ok"):
            return 1
        exe, vexe = pathlib.Path(summary["program"]), pathlib.Path(summary["viewer"]["program"])
        env = {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SEGARECOMP_VIEWER_UNTHROTTLED": "1", "SEGARECOMP_VIEWER_NO_STOP_SCREEN": "1",
               "SEGARECOMP_VIEWER_SETTINGS": str(tmp / "settings.cfg"), "SEGARECOMP_SCREENSHOT_DIR": str(tmp)}
        base = ["--disc-dir", disc, "--bios", bios, "--instruction-budget", BUDGET, "--set", "main:d6=40"]
        capdir = tmp / "cap"
        capdir.mkdir()
        keys = tmp / "keys.txt"
        # RIGHT (arrow key) held from poll 3 to poll 11; UP from poll 14 to 18
        keys.write_text("3 down Right\n11 up Right\n14 down Up\n18 up Up\n")

        def live(name, key_text, extra=()):
            (tmp / (name + ".keys")).write_text(key_text)
            rec, rep = tmp / (name + ".rec"), tmp / (name + ".json")
            code, out = run(vexe, base + ["--record-input", rec, "--report", rep, "--capture-dir", capdir, "--capture-onset", 8, "--capture-onset", 17, *extra],
                            {**env, "SEGARECOMP_VIEWER_KEY_SCRIPT": str(tmp / (name + ".keys"))})
            return code, json.loads(rep.read_text()) if rep.exists() else {}, rec.read_text() if rec.exists() else None, out

        code, rep, rec, out = live("a", keys.read_text())
        ok = code == 0 and rep["result"] == "idle" and rep["live"]["outcome"] == "machine_stop"
        check(ok, "viewer: opens a window, runs to the end of the program, exit 0" + ("" if ok else " (%s)" % out.stderr[-300:]))
        check(rep["live"]["rendered"] == rep["live"]["presented"] == 39 and rep["live"]["skipped"] == 1 and rep["live"]["rejected"] == 0,
              "viewer: every frame but the power-on one presented to the window (39 of 40 onsets)")
        right = backdrop(0x77, 0x33)
        up = backdrop(0x7E, 0x33)
        caps = {c["onset"]: c["sha256"] for c in rep["frame_captures"]}
        check(caps[8] == flat_digest(right) and caps[17] == flat_digest(up) and right != up,
              "viewer: the arrow keys reach the pad - the frame after the RIGHT key is the RIGHT backdrop, the one after UP the UP backdrop")
        lines = rec.splitlines()
        want = ["%d 1 press RIGHT" % (FIRST_ONSET + 3 * FRAME), "%d 1 release RIGHT" % (FIRST_ONSET + 11 * FRAME),
                "%d 1 press UP" % (FIRST_ONSET + 14 * FRAME), "%d 1 release UP" % (FIRST_ONSET + 18 * FRAME)]
        check(lines[0] == "segarecomp-input-script 1" and lines[1:] == want, "viewer: the recording holds the four key transitions at their frame boundaries")
        (tmp / "a.rec").write_text(rec)
        code, rrep = run(exe, base + ["--input-script", tmp / "a.rec", "--capture-dir", capdir, "--capture-onset", 8, "--capture-onset", 17, "--report", tmp / "r.json"], {})
        replay = json.loads((tmp / "r.json").read_text())
        # SEG-107: the viewer mixes PCM and CD-DA by default (an `audio` block; the chip is synchronized to every video frame, so its lazily synchronized position
        # fields differ from a headless replay without the mix); everything the guest can observe is equal
        def guest_view(report):
            view = {k: v for k, v in report.items() if k not in ("live", "cdda", "audio")}
            if "sub_devices" in view and "pcm" in view["sub_devices"]:
                view = json.loads(json.dumps(view))
                for key in ("samples", "channel_steps", "max_catchup", "addr"):
                    view["sub_devices"]["pcm"].pop(key, None)
            return view

        check(code == 0 and guest_view(rep) == guest_view(replay), "viewer: the recording replays headlessly to the identical report and captured frames")

        # SEG-117: the Sega CD viewer reads the same shared Widescreen Mode setting as the Genesis one. A saved Level 3 request is shown as Level 2 (the
        # extension is presentation only), so it leaves the run (reports, presented frames, captured frames, the recording) identical to Original and the viewer does not rewrite the file.
        ws_file = tmp / "settings.cfg"
        ws_text = "video.widescreen=enhanced_gameplay\n"
        ws_file.write_text(ws_text)
        wcode, wrep, wrec, wout = live("w", keys.read_text())
        check(wcode == 0 and guest_view(wrep) == guest_view(rep) and wrep["live"]["presented"] == rep["live"]["presented"] and wrec == rec,
              "widescreen: a saved Level 3 request leaves the Sega CD run, frames and input recording identical to Original" + ("" if wcode == 0 else " (%s)" % wout.stderr[-300:]))
        check(ws_file.read_text() == ws_text, "widescreen: the Sega CD viewer reads the shared setting and does not rewrite it")
        # SEG-119 (ADR 0262): Level 2 on the real driver. The extension is produced by the Sega CD frame path (scd_present_frame) for every presented
        # frame (the viewer reports its counters on exit, only for a run that requested it); the Sega CD machine is untouched: the whole report (guest state,
        # scheduler counters, CD-DA / PCM / audio blocks and the live counters), the recorded input and the headless replay equal the Original run.
        ws_file.write_text("video.widescreen=extended_backgrounds\n")
        l2code, l2rep, l2rec, l2out = live("l2", keys.read_text())
        check(l2code == 0 and l2rep == rep and l2rec == rec,
              "level 2: report (guest, scheduler, audio, live counters) and input recording identical to Original" + ("" if l2code == 0 else " (%s)" % l2out.stderr[-300:]))
        stats = [ln for ln in l2out.stderr.splitlines() if ln.startswith("viewer: level2 extension")]
        check(len(stats) == 1 and "produced=%d failed=0" % rep["live"]["presented"] in stats[0], "level 2: one extension per presented frame, none failed (%s)" % stats)
        check(not [ln for ln in out.stderr.splitlines() if ln.startswith("viewer: level2 extension")], "original: no extension work reported")
        check(guest_view(l2rep) == guest_view(rep), "level 2: the machine's observable state equals Original")
        (tmp / "l2.rec").write_text(l2rec)
        code, _ = run(exe, base + ["--input-script", tmp / "l2.rec", "--capture-dir", capdir, "--capture-onset", 8, "--capture-onset", 17, "--report", tmp / "r2.json"], {})
        check(code == 0 and guest_view(json.loads((tmp / "r2.json").read_text())) == guest_view(replay), "level 2: the recording replays headlessly to the Original report")
        ws_file.unlink()

        # F6 holds, F7 steps: same recording and report as the same keys without the pause
        pk = "3 down Right\n4 down F6\n4 up F6\n6 down F7\n6 up F7\n8 down F7\n8 up F7\n9 down F6\n9 up F6\n13 up Right\n14 down Up\n18 up Up\n"
        code, prep, prec, _ = live("b", pk)
        check(code == 0 and prep["live"]["steps"] == 2 and prep["live"]["paused_polls"] >= 2, "pause: F6 holds, F7 steps (%s)" % {k: prep["live"][k] for k in ("steps", "paused_polls")})
        # the held polls advance no frame: 4 -> 9 polls but two frames; compare to the unpaused run of equivalent key schedule
        check(prec is not None and prec.startswith("segarecomp-input-script 1") and prep["result"] == "idle", "pause: the run still ends idle with a valid recording")

        # window close mid-run: exit 0, recording written, outcome quit
        code, qrep, qrec, _ = live("c", "3 down Right\n9 quit x\n")
        check(code == 0 and qrep["live"]["outcome"] == "quit" and qrec.startswith("segarecomp-input-script 1") and qrep["live"]["frames"] == 9,
              "window close: exit 0, outcome quit at the poll of the close, recording written (frames %s)" % qrep.get("live", {}).get("frames"))

        # a typed stop is reported (budget exhausted -> typed result), exit status of the program
        code, out = run(vexe, ["--disc-dir", disc, "--bios", bios, "--instruction-budget", 20000, "--set", "main:d6=40", "--report", tmp / "s.json"], env)
        srep = json.loads((tmp / "s.json").read_text())
        check(code == 11 and srep["result"] == "budget_exhausted" and srep["live"]["outcome"] == "machine_stop", "budget: a typed stop of the machine is reported and exits 11")

        # a refused BIOS never opens a window and keeps the program's typed exit
        wrong = tmp / "wrong.bin"
        wrong.write_bytes(bios.read_bytes()[:-1] + b"\xff")
        code, out = run(vexe, ["--disc-dir", disc, "--bios", wrong, "--instruction-budget", 1000], env)
        check(code == 21 and "bios_identity_mismatch" in out.stdout, "refusal: a different BIOS is the program's typed exit 21 before any window or guest instruction")
        # the headless program of the same build has no viewer
        code, out = run(exe, base + ["--report", tmp / "h.json"], {})
        check(code == 0 and "live" not in json.loads((tmp / "h.json").read_text()), "headless: unchanged by the viewer build")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
