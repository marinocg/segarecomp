#!/usr/bin/env python3
"""SEG-107 (ADR 0251): RF5C164 PCM audio and the PCM + CD-DA mix on a generated-native program.

The project-authored fixture (tools/sega_cd_cdda_fixture.py build --pcm) is a BIOS-like image whose generated sub program programs the PCM chip through
real bus writes (a positive-only wave with $FF loop markers, two channels), switches the channels off and on while the CD-DA script plays, over a disc of
closed-form audio. The expectation is rebuilt from external evidence only, with code written here and independent of the runtime:

  * the PCM stream from the runtime's own register-access trace (`--pcm-trace`: every chip write with its machine tick) replayed through a Python model
    of the chip (published GPGX/MAME behavior) on the 44.1 kHz guest-time timeline (frame n at floor(n x ticks_per_second / 44100), zero-order hold);
  * the CD-DA stream from the run log (`--cdda-log`) and the fixture's closed-form sectors: a run starts at its first delivery and then advances one
    frame per output frame (the drive tick is slower than 588 frames, so later sectors come from the disc ahead of it), ending with the drive's stop;
  * mixed = saturating sum. The runtime's `--audio-pcm` artifact must equal it frame for frame.

Also: the legacy CD-DA digest and artifact are unchanged by the mix; the mix is deterministic under every scheduler partition; the machine report apart
from the `audio` block is identical with the mix on and off (audio never reaches the guest); the SDL3 viewer plays exactly the mixed stream (skipped
when SDL3 is unavailable). usage: sega_cd_pcm_audio_native_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_bios_build as build_driver  # noqa: E402
import sega_cd_cdda_fixture as fx  # noqa: E402
from sega_cd_audio_expect import cdda_placement, pcm_reference  # noqa: E402

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


def clamp16(v):
    return max(-32768, min(32767, v))


def main():
    with tempfile.TemporaryDirectory(prefix="scd-pcm-") as tmp:
        tmp = pathlib.Path(tmp)
        subprocess.run([sys.executable, str(FIXTURE), "build", "--pcm", "--out", str(tmp / "fx")], check=True, capture_output=True, text=True)
        bios, disc = tmp / "fx" / "bios.bin", tmp / "fx" / "disc"
        viewer_ok = sys.platform == "linux" and build_driver.find_sdl3() is not None
        built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios", str(bios),
                                "--work", str(tmp / "work"), "--pass-set", "main:d6=3", "--budget", "60000000", "--timeout", "600",
                                *(["--viewer"] if viewer_ok else [])], capture_output=True, text=True, timeout=3000)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok"), "the PCM fixture program builds" + ("" if summary.get("ok") else ": %s" % (built.stdout[-400:] + built.stderr[-400:])))
        if not summary.get("ok"):
            return 1
        exe = pathlib.Path(summary["program"])
        base = ["--disc-dir", disc, "--bios", bios, "--instruction-budget", BUDGET, "--set", "main:d6=%d" % FRAMES]

        mix, cdda_file, log, trace, rep = tmp / "mix.pcm", tmp / "cdda.pcm", tmp / "cdda.log", tmp / "pcm.trace", tmp / "mix.json"
        code, out = run(exe, base + ["--audio-pcm", mix, "--cdda-pcm", cdda_file, "--cdda-log", log, "--pcm-trace", trace, "--report", rep])
        report = json.loads(rep.read_text()) if rep.exists() else {}
        check(code == 0 and report.get("result") == "idle", "headless mix: the run ends idle (exit %d, %s)" % (code, report.get("result")))
        audio, cdda = report.get("audio", {}), report.get("cdda", {})
        data = mix.read_bytes()
        n_frames = len(data) // 4
        check(audio.get("frames") == n_frames and len(data) == 4 * n_frames and audio.get("sample_rate") == 44100, "the artifact holds exactly the reported frames")
        check(hashlib.sha256(data).hexdigest() == audio.get("pcm_sha256"), "the report digest is the digest of the artifact")
        check(audio.get("pcm_events", 0) > 1000 and audio.get("pcm_frames", 0) > 1000, "PCM was synthesized (events and audible frames)")
        check(audio.get("cdda_mixed", 0) > 0 and audio.get("cdda_dropped") == 0 and audio.get("cdda_gaps", 1 << 30) <= 588 and audio.get("sink_refused") == 0 and
              audio.get("io_errors") == 0 and audio.get("cdda_readahead", 0) > 0,
              "CD-DA was mixed without a drop or a refused chunk, with read-ahead covering the slower drive tick, and at most one sector of silence while a run waits for the drive's next tick (%s)" % {k: audio.get(k) for k in ("cdda_gaps", "cdda_readahead", "cdda_skipped")})
        # the legacy CD-DA artifact and digest are unchanged by the mix
        legacy = tmp / "legacy.pcm"
        rl = tmp / "legacy.json"
        c2, _ = run(exe, base + ["--cdda-pcm", legacy, "--report", rl])
        lr = json.loads(rl.read_text())
        check(c2 == 0 and legacy.read_bytes() == cdda_file.read_bytes() and lr["cdda"]["pcm_sha256"] == cdda["pcm_sha256"] and "audio" not in lr,
              "the CD-DA-only artifact and digest are identical with and without the mix; no audio block without it")

        # the independent expectation
        pcm_ref = pcm_reference(trace.read_text(), n_frames)
        def cap(lba):  # the end of the fixture track holding `lba` (the product's track end: first LBA + sectors)
            return fx.T2_FIRST + fx.T2_N if lba < fx.T3_FIRST else fx.T3_FIRST + fx.T3_N

        cd_ref = cdda_placement(log.read_text(), fx.sector, cap, n_frames)
        got = [struct.unpack_from("<hh", data, 4 * i) for i in range(n_frames)]
        want = [(clamp16(p[0] + c[0]), clamp16(p[1] + c[1])) for p, c in zip(pcm_ref, cd_ref)]
        bad = [i for i in range(n_frames) if got[i] != want[i]]
        check(not bad, "the artifact equals the independent expectation (PCM from the access trace + CD-DA runs on the timeline, saturating sum)" +
              ("" if not bad else ": %d differing frames, first at %d: %s vs %s" % (len(bad), bad[0], got[bad[0]], want[bad[0]])))
        check(any(p != (0, 0) for p in pcm_ref) and any(c != (0, 0) for c in cd_ref), "the expectation contains both PCM and CD-DA sound")
        # silence where nothing plays: the PCM-off interval inside a CD-DA run is CD-DA only; before the game programs the chip the stream is silent
        first_sound = next((i for i in range(n_frames) if got[i] != (0, 0)), None)
        check(first_sound is not None and first_sound > 100, "the stream is silent until the sub program starts sound")
        off_only = [i for i in range(n_frames) if pcm_ref[i] == (0, 0) and cd_ref[i] != (0, 0)]
        pcm_only = [i for i in range(n_frames) if pcm_ref[i] != (0, 0) and cd_ref[i] == (0, 0)]
        both = [i for i in range(n_frames) if pcm_ref[i] != (0, 0) and cd_ref[i] != (0, 0)]
        check(off_only and pcm_only and both, "all three conditions occur: CD-DA with PCM silent, PCM with CD-DA silent, both together")

        # determinism under scheduler partitions and independence of the guest
        for name, extra in (("slice-1e9", ["--slice-ticks", "1000000000"]), ("slice-7e11", ["--slice-ticks", "700000000000"]), ("seed-3", ["--slice-seed", "3"]),
                            ("seed-11", ["--slice-seed", "11"])):
            p2, r2 = tmp / (name + ".pcm"), tmp / (name + ".json")
            c3, _ = run(exe, base + extra + ["--audio-pcm", p2, "--report", r2])
            check(c3 == 0 and p2.read_bytes() == data and json.loads(r2.read_text()) == report, "deterministic under %s: identical mixed stream and report" % name)
        r3 = tmp / "off.json"
        c4, _ = run(exe, base + ["--report", r3])
        off = json.loads(r3.read_text())
        lazy = ("samples", "channel_steps", "max_catchup", "addr")  # the chip is synchronized lazily: with the mix on it is brought up to every video frame

        def guest_view(rep):
            rep = json.loads(json.dumps(rep))
            for key in ("audio", "cdda"):
                rep.pop(key, None)
            for key in lazy:
                rep["sub_devices"]["pcm"].pop(key, None)
            return rep

        check(c4 == 0 and "audio" not in off and "cdda" not in off and guest_view(off) == guest_view(report) and off["sub_devices"]["pcm"]["syncs"] == report["sub_devices"]["pcm"]["syncs"],
              "audio disabled: the identical report apart from the audio/cdda blocks and the chip's lazily synchronized position (device log, CPUs, memory, registers, wave RAM)")
        c5, _ = run(exe, base + ["--pcm-audio", "0", "--audio-digest", "0", "--report", tmp / "z.json"])
        check(c5 == 0 and "audio" not in json.loads((tmp / "z.json").read_text()), "--audio-digest 0 leaves the mix off")

        # ---- the real SDL3 viewer ----
        if not viewer_ok:
            print("SKIP: SDL3 or a Linux host is not available (viewer part)")
        else:
            vexe = pathlib.Path(summary["viewer"]["program"])
            dev = tmp / "device.raw"
            env = {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "disk", "SDL_AUDIO_DISK_OUTPUT_FILE": str(dev), "SEGARECOMP_VIEWER_NO_STOP_SCREEN": "1",
                   "SEGARECOMP_VIEWER_SETTINGS": str(tmp / "settings.cfg"), "SDL_AUDIO_DISK_TIMESCALE": "0.5"}
            vrep, vpcm = tmp / "v.json", tmp / "v.pcm"
            code, out = run(vexe, base + ["--audio-pcm", vpcm, "--report", vrep], env)
            vr = json.loads(vrep.read_text()) if vrep.exists() else {}
            check(code == 0 and vr.get("result") == "idle", "viewer: runs to the end of the program, exit 0 (%d)" % code)
            check(vr.get("audio", {}).get("pcm_sha256") == audio["pcm_sha256"] and vpcm.read_bytes() == data, "viewer: the machine produced the identical mixed stream and digest as the headless run")
            stat = re.search(r"cdda frames offered=(\d+) submitted=(\d+) overrun_dropped=(\d+) refused=(\d+) discarded=(\d+) underruns=(\d+) pending_dropped=(\d+)", out.stderr)
            check(stat is not None and int(stat.group(1)) == n_frames and int(stat.group(2)) == n_frames and int(stat.group(3)) == 0 and
                  int(stat.group(4)) == 0 and int(stat.group(5)) == 0 and int(stat.group(7)) == 0,
                  "viewer: every mixed frame was submitted to the SDL3 stream: %s" % (stat.group(0) if stat else out.stderr[-200:]))
            played = dev.read_bytes() if dev.exists() else b""
            pf = [played[i:i + 4] for i in range(0, len(played) - len(played) % 4, 4)]
            nonzero = b"".join(f for f in pf if f != b"\0\0\0\0")
            wanted = b"".join(data[i:i + 4] for i in range(0, len(data), 4) if data[i:i + 4] != b"\0\0\0\0")
            # the frames still queued in the device when the window closes are not played (at most the 100 ms queue limit)
            tail = len(wanted) - len(nonzero)
            check(len(played) > 0 and wanted.startswith(nonzero) and 0 <= tail <= 4 * 4410,
                  "viewer: the samples SDL played (device file, silence removed) are the mixed stream up to the frames still queued at exit (%d bytes played, %d bytes unplayed)" % (len(played), tail))
            dev2 = tmp / "device2.raw"
            code, out = run(vexe, base + ["--report", tmp / "m.json"], {**env, "SDL_AUDIO_DISK_OUTPUT_FILE": str(dev2), "SEGARECOMP_VIEWER_MUTE": "1"})
            played2 = dev2.read_bytes() if dev2.exists() else b""
            check(code == 0 and all(b == 0 for b in played2), "viewer: SEGARECOMP_VIEWER_MUTE=1 plays no sample")
            dev3 = tmp / "device3.raw"
            code, out = run(vexe, base + ["--pcm-audio", "0", "--cdda-pcm", tmp / "c3.pcm", "--report", tmp / "c3.json"], {**env, "SDL_AUDIO_DISK_OUTPUT_FILE": str(dev3)})
            c3r = json.loads((tmp / "c3.json").read_text()) if (tmp / "c3.json").exists() else {}
            p3 = dev3.read_bytes() if dev3.exists() else b""
            f3 = b"".join(p3[i:i + 4] for i in range(0, len(p3) - len(p3) % 4, 4) if p3[i:i + 4] != b"\0\0\0\0")
            cw = cdda_file.read_bytes()
            cwn = b"".join(cw[i:i + 4] for i in range(0, len(cw), 4) if cw[i:i + 4] != b"\0\0\0\0")
            check(code == 0 and "audio" not in c3r and f3 == cwn, "viewer: --pcm-audio 0 plays the CD-DA alone, as before")
    print("%d failure(s)" % len(failures) if failures else "sega_cd_pcm_audio_native_test: all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
