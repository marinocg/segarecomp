#!/usr/bin/env python3
"""SEG-032-T009 (ADR 0075): headless-vs-viewer-core equivalence of the audio stream, and the presentation policy, without a window.

tests/tools/genesis_viewer_audio_harness.c runs ONE scripted 68K workload (a PSG tone and a YM2612 patch through the real 68K ports, then
guest time) through (a) the headless sound route (one runner call, then the flush the program hook performs) and (b) the real viewer
core (viewer.c, fake clock, presenter and sink; no SDL, no window) in slices of 1, 7, 64 and 100,000 dispatches, draining the mixer ring after
every slice through genesis_audio_present.c. Proved:
  * the viewer stream (the bytes the fake sink accepted), the mixer digest, the frame count, the YM2612 sample stream and device state and the
    guest time are bit-identical to the headless run for every slice size, and repeated runs are identical;
  * a refusing sink, a missing sink, mute and a permanently full queue change only the presenter counters (refused / discarded /
    overrun-dropped equal the frame count): the digest and every device/guest aggregate equal the healthy run;
  * the stream is non-silent, has thousands of frames, and the mixer never faulted;
  * the audio core reads no wall clock: the mixer, devices, presenter and sink contract contain no time, sleep, SDL or environment access.
usage: genesis_audio_equivalence_test.py <registry_emitter> <cc> <source-root> <c++>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

registry_emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
cxx = sys.argv[4] if len(sys.argv) > 4 else "c++"
sys.path.insert(0, str(root / "tools"))
import genesis_ym2612_build as ymbuild  # noqa: E402
import z80_conformance as z  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def parse(out, tag):
    line = re.search(r"^(?:outcome=\S+ )?(?:slices=\d+ )?%s (.*)$" % tag, out, re.M)
    return dict(p.split("=") for p in line.group(1).split()) if line else {}


def main():
    rt = root / "platforms/genesis/runtime"
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "image.spec").write_text("epoch %s %s\n" % ((b"\x76" + bytes(8191)).hex(), (b"\x01" + bytes(1023)).hex()))
        done = subprocess.run([registry_emitter, str(tmp / "image.spec"), str(tmp / "gen"), "genesis_z80"], text=True, capture_output=True)
        assert done.returncode == 0, done.stdout + done.stderr
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        objs = ymbuild.build_objects(cc, cxx, root, tmp / "ymobj")
        exe, message = z.compile_units(
            tc, tmp / "gen", "genesis_z80",
            extra_sources=[root / "tests/tools/genesis_viewer_audio_harness.c", rt / "runtime.c", rt / "z80_machine.c", rt / "genesis_audio.c", rt / "genesis_mixer.c",
                           rt / "genesis_audio_present.c", rt / "genesis_sound.c", rt / "vdp_render.c", root / "platforms/genesis/viewer/viewer.c",
                           root / "libs/device/sega/psg/src/sn76489.c"],
            extra_flags=["-I", str(rt), "-I", str(root / "platforms/genesis/viewer"), "-I", str(root / "libs/device/sega/psg/include"),
                         "-I", str(root / "libs/device/sega/ym2612/include")],
            extra_objects=objs)
        assert exe is not None, message

        def run(*args):
            pcm = tmp / ("out-%d.pcm" % len(list(tmp.glob("out-*.pcm"))))
            out = subprocess.run([str(exe), args[0], str(pcm), *args[1:]], text=True, capture_output=True, check=True).stdout
            return out, pcm.read_bytes()

        out, head_pcm = run("headless")
        head = parse(out, "HEADLESS")
        frames = int(head["frames"])
        check(frames > 4000 and head["nonsilent"] == "1" and head["fault"] == "0" and len(head_pcm) == 4 * frames and int(head["psg_writes"]) == 6 and int(head["ym_writes"]) > 40,
              "headless: %d frames of a non-silent stream from 68K-written PSG and YM2612, no mixer fault" % frames)
        out2, head_pcm2 = run("headless")
        check(parse(out2, "HEADLESS") == head and head_pcm2 == head_pcm, "headless runs are bit-identical (aggregates and PCM bytes)")
        invariants = ("frames", "clipped", "nonsilent", "fault", "ticks", "psg_writes", "ym_writes", "ym_samples", "ym_fnv", "ym_state", "sha")
        for slice_size in (1, 7, 64, 100000):
            out, pcm = run("viewer", str(slice_size), "ok")
            v = parse(out, "VIEWER")
            p = parse(out, "PRESENTER")
            check(all(v[k] == head[k] for k in invariants) and pcm == head_pcm and int(p["submitted"]) == frames and int(p["overrun_dropped"]) == 0
                  and int(p["refused"]) == 0 and int(p["ring_dropped"]) == 0,
                  "viewer core, slice %d: the accepted stream (%d bytes), digest, YM2612 stream/state and guest time are bit-identical to headless%s"
                  % (slice_size, len(pcm), "" if slice_size != 1 else " (outcome complete, %s slices)" % re.search(r"slices=(\d+)", out).group(1)))
        for mode in ("fail", "none", "mute", "full"):
            out, pcm = run("viewer", "7", mode)
            v, p = parse(out, "VIEWER"), parse(out, "PRESENTER")
            counter = {"fail": "refused", "none": "discarded", "mute": "discarded", "full": "overrun_dropped"}[mode]
            others = [k for k in ("submitted", "overrun_dropped", "refused", "discarded") if k != counter]
            check(all(v[k] == head[k] for k in invariants) and int(p[counter]) == frames and all(int(p[k]) == 0 for k in others),
                  "sink mode %s: only the presenter counter %s = %d changes; digest and every device/guest aggregate equal the healthy run" % (mode, counter, frames))
            if mode in ("fail", "none", "mute", "full"):
                check(pcm == b"", "sink mode %s: nothing reached the host sink" % mode)
        # forbidden wall-clock / host access in the audio core
        core = ["genesis_mixer.c", "genesis_mixer.h", "genesis_audio.c", "genesis_audio.h", "genesis_audio_present.c", "genesis_audio_present.h",
                "genesis_audio_sink.h", "genesis_audio_format.h", "genesis_sound.c", "genesis_sound.h"]
        forbidden = re.compile(r"<time\.h>|<sys/time\.h>|\btime\s*\(|\bclock\s*\(|clock_gettime|gettimeofday|\bsleep\s*\(|usleep|nanosleep|SDL_|getenv|<stdio\.h>|fopen")
        hits = [(name, m.group(0)) for name in core for m in forbidden.finditer(re.sub(r"/\*.*?\*/|//[^\n]*", "", (rt / name).read_text(), flags=re.S))]
        check(not hits, "the audio core (mixer, devices, presenter, sink contract, attach point) has no wall-clock, sleep, SDL, environment or file access %s" % hits)
        print("METRIC equivalence frames=%d slices_checked=4 sink_modes_checked=4 identical=%s" % (frames, True))
    print("genesis audio equivalence: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
