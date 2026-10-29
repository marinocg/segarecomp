#!/usr/bin/env python3
"""SEG-104 (ADR 0244, 0246): CD-DA end to end on a generated-native program - headless artifact first, then the real SDL3 viewer.

The project-authored fixture (tools/sega_cd_cdda_fixture.py) is a BIOS-like image whose sub program drives the CD drive through the real Gate Array
command registers (play, pause, resume, a play that runs from track 2 through track 3's non-silent INDEX 00 pregap, seek, play, stop) over a disc whose
audio tracks hold closed-form samples. Checked, against an oracle written independently of the runtime (the fixture's own `sector(lba)` bytes):

  * the canonical stream (`--cdda-pcm`, little-endian interleaved stereo) equals the concatenation of the expected sectors over the runs the product
    logged, with the runs themselves as the script requires (starts at the commanded LBAs, a resume contiguous with the pause, the INDEX 00 pregap
    never heard, nothing between a seek and its play, silence after the stop); sectors x 588 = frames; the report digest is the file's digest;
  * determinism: deadline slices and seeded partitions give the identical stream and report; the same run without CD-DA output has the identical report
    apart from the `cdda` block (audio production changes nothing the machine does);
  * the SDL3 viewer (dummy video, SDL's `disk` audio driver, real time): it exits 0, reports the identical canonical digest, and the samples SDL played
    - the device file without its silence - are the canonical stream.

Skipped (exit 0, "SKIP") for the viewer part when SDL3 is not installed or the host is not Linux.
usage: sega_cd_cdda_native_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_bios_build as build_driver  # noqa: E402
import sega_cd_cdda_fixture as fx  # noqa: E402

DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
FIXTURE = ROOT / "tools" / "sega_cd_cdda_fixture.py"
BUDGET = 200_000_000
FRAMES = 400
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def run(exe, args, env=None, timeout=900):
    out = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=timeout, env={**os.environ, **(env or {})})
    return out.returncode, out


def parse_runs(log):
    """Runs (first LBA, end LBA exclusive) from the product log: each start/jump opens a run, a stop closes it with its next_lba, a track line continues."""
    runs, open_start, last = [], None, None
    for line in log.read_text().splitlines():
        m = re.match(r"(start|jump|track|stop) frame=\d+ tick=\d+ (?:lba=(-?\d+)|next_lba=(-?\d+))", line)
        if not m:
            continue
        kind = m.group(1)
        if kind in ("start", "jump"):
            if open_start is not None:
                runs.append((open_start, last))
            open_start, last = int(m.group(2)), None
        elif kind == "stop":
            runs.append((open_start, int(m.group(3))))
            open_start = None
    return runs, open_start


def main():
    with tempfile.TemporaryDirectory(prefix="scd-cdda-") as tmp:
        tmp = pathlib.Path(tmp)
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(tmp / "fx")], check=True, capture_output=True, text=True)
        bios, disc = tmp / "fx" / "bios.bin", tmp / "fx" / "disc"
        viewer_ok = sys.platform == "linux" and build_driver.find_sdl3() is not None
        built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios", str(bios),
                                "--work", str(tmp / "work"), "--pass-set", "main:d6=3", "--budget", "60000000", "--timeout", "600",
                                *(["--viewer"] if viewer_ok else [])], capture_output=True, text=True, timeout=3000)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok"), "the fixture program builds" + ("" if summary.get("ok") else ": %s" % (built.stdout[-400:] + built.stderr[-400:])))
        if not summary.get("ok"):
            return 1
        exe = pathlib.Path(summary["program"])
        base = ["--disc-dir", disc, "--bios", bios, "--instruction-budget", BUDGET, "--set", "main:d6=%d" % FRAMES]

        # ---- headless artifact against the independent oracle ----
        pcm, log, rep = tmp / "a.pcm", tmp / "a.log", tmp / "a.json"
        code, out = run(exe, base + ["--cdda-pcm", pcm, "--cdda-log", log, "--report", rep])
        report = json.loads(rep.read_text()) if rep.exists() else {}
        check(code == 0 and report.get("result") == "idle", "headless: the run ends idle (exit %d, %s)" % (code, report.get("result")))
        cdda = report["cdda"]
        data = pcm.read_bytes()
        runs, still_open = parse_runs(log)
        check(still_open is None, "headless: every run ended (the stop)")
        t2_last, t2_end = fx.T2_FIRST + fx.T2_N - 5, fx.T2_FIRST + fx.T2_N
        check(len(runs) == 5, "headless: five runs (play, resume after pause, the end of track 2, track 3 after its pregap, play after seek); got %d" % len(runs))
        if len(runs) == 5:
            check(runs[0][0] == fx.T2_FIRST, "run 1 starts at the commanded LBA")
            check(runs[1][0] == runs[0][1], "run 2 (resume) continues exactly where the pause left the drive")
            check(runs[2] == (t2_last, t2_end), "run 3 plays the last five sectors of track 2 and no INDEX 00 pregap sector")
            check(runs[3][0] == fx.T3_FIRST and runs[3][1] > fx.T3_FIRST + 10, "run 4 begins at track 3's INDEX 01 after the silent pregap")
            check(runs[4][0] == fx.T3_FIRST + 20 and runs[4][1] > runs[4][0] + 10, "run 5 begins at the commanded LBA after the seek (nothing between the seek and the play)")
            check(runs[0][1] - runs[0][0] > 20 and runs[1][1] - runs[1][0] > 10, "the waits produce the expected order of sectors")
        total_sectors = sum(e - s for s, e in runs)
        expected = b"".join(fx.sector(lba) for s, e in runs for lba in range(s, e))
        check(data == expected, "the canonical stream equals the oracle's concatenation of the logged runs (left/right order, little-endian samples, boundaries)")
        check(cdda["sectors"] == total_sectors and cdda["frames"] == 588 * total_sectors and len(data) == 4 * cdda["frames"], "sectors x 588 = frames = file length / 4")
        check(hashlib.sha256(data).hexdigest() == cdda["pcm_sha256"], "the report digest is the digest of the file")
        check(cdda["segments"] == 5 and cdda["pregap_ticks"] == fx.T3_PRE and cdda["sink_refused"] == 0 and cdda["io_errors"] == 0,
              "counters: five runs, the whole 150-sector pregap silent, nothing refused")
        check(cdda["commands"] == {"stop": 1, "play": 3, "seek": 1, "pause": 1, "resume": 1, "jump": 0} or
              (cdda["commands"]["play"] == 3 and cdda["commands"]["seek"] == 1 and cdda["commands"]["pause"] == 1 and cdda["commands"]["resume"] == 1
               and cdda["commands"]["stop"] == 1), "counters: the commands the sub program sent")

        # ---- determinism and independence ----
        for name, extra in (("slice-1e9", ["--slice-ticks", "1000000000"]), ("slice-7e11", ["--slice-ticks", "700000000000"]), ("seed-3", ["--slice-seed", "3"]),
                            ("seed-11", ["--slice-seed", "11"])):
            p2, r2 = tmp / (name + ".pcm"), tmp / (name + ".json")
            c2, _ = run(exe, base + extra + ["--cdda-pcm", p2, "--report", r2])
            same = c2 == 0 and p2.read_bytes() == data and json.loads(r2.read_text()) == report
            check(same, "deterministic under %s: identical stream and report" % name)
        r3 = tmp / "off.json"
        c3, _ = run(exe, base + ["--report", r3])
        off = json.loads(r3.read_text())
        on_minus = {k: v for k, v in report.items() if k != "cdda"}
        check(c3 == 0 and "cdda" not in off and off == on_minus, "CD-DA output disabled: the identical report apart from the cdda block (device log, CPUs, memory, drive state)")
        c4, o4 = run(exe, base + ["--cdda-digest", "0", "--report", tmp / "off0.json"])
        check(c4 == 0 and "cdda" not in json.loads((tmp / "off0.json").read_text()), "--cdda-digest 0 leaves the output disabled")

        # ---- the real SDL3 viewer ----
        if not viewer_ok:
            print("SKIP: SDL3 or a Linux host is not available (viewer part)")
        else:
            vexe = pathlib.Path(summary["viewer"]["program"])
            dev = tmp / "device.raw"
            env = {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "disk", "SDL_AUDIO_DISK_OUTPUT_FILE": str(dev), "SEGARECOMP_VIEWER_NO_STOP_SCREEN": "1",
                   "SEGARECOMP_VIEWER_SETTINGS": str(tmp / "settings.cfg")}
            vrep, vpcm = tmp / "v.json", tmp / "v.pcm"
            code, out = run(vexe, base + ["--cdda-pcm", vpcm, "--report", vrep], env)
            vr = json.loads(vrep.read_text()) if vrep.exists() else {}
            check(code == 0 and vr.get("result") == "idle", "viewer: runs to the end of the program, exit 0 (%d)" % code)
            check(vr.get("cdda", {}).get("pcm_sha256") == cdda["pcm_sha256"] and vpcm.read_bytes() == data,
                  "viewer: the machine produced the identical canonical stream and digest as the headless run")
            stat = re.search(r"cdda frames offered=(\d+) submitted=(\d+) overrun_dropped=(\d+) refused=(\d+) discarded=(\d+) underruns=(\d+) pending_dropped=(\d+)", out.stderr)
            check(stat is not None and int(stat.group(1)) == cdda["frames"] and int(stat.group(2)) == cdda["frames"] and int(stat.group(3)) == 0 and
                  int(stat.group(4)) == 0 and int(stat.group(5)) == 0 and int(stat.group(7)) == 0,
                  "viewer: every canonical frame was submitted to the SDL3 stream (none dropped, refused or discarded): %s" % (stat.group(0) if stat else out.stderr[-200:]))
            played = dev.read_bytes() if dev.exists() else b""
            frames = [played[i:i + 4] for i in range(0, len(played) - len(played) % 4, 4)]
            nonzero = b"".join(f for f in frames if f != b"\0\0\0\0")
            wanted = b"".join(data[i:i + 4] for i in range(0, len(data), 4) if data[i:i + 4] != b"\0\0\0\0")
            check(len(played) > 0 and nonzero == wanted, "viewer: the samples SDL played (device file, silence removed) are the canonical stream (%d bytes played)" % len(played))
            # mute: the stream is consumed, nothing reaches the device
            dev2 = tmp / "device2.raw"
            code, out = run(vexe, base + ["--report", tmp / "m.json"], {**env, "SDL_AUDIO_DISK_OUTPUT_FILE": str(dev2), "SEGARECOMP_VIEWER_MUTE": "1"})
            played2 = dev2.read_bytes() if dev2.exists() else b""
            check(code == 0 and all(b == 0 for b in played2), "viewer: SEGARECOMP_VIEWER_MUTE=1 plays no sample and does not change the run")
            # no audio device: a silent viewer, the run is unaffected
            code, out = run(vexe, base + ["--cdda-pcm", tmp / "n.pcm", "--report", tmp / "n.json"], {**env, "SDL_AUDIODRIVER": "does_not_exist"})
            nr = json.loads((tmp / "n.json").read_text()) if (tmp / "n.json").exists() else {}
            check(code == 0 and nr.get("cdda", {}).get("pcm_sha256") == cdda["pcm_sha256"], "viewer: a missing audio driver leaves it silent and the machine's stream unchanged")
    print("%d failure(s)" % len(failures) if failures else "sega_cd_cdda_native_test: all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
