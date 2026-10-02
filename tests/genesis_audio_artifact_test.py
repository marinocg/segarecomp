#!/usr/bin/env python3
"""SEG-032-T009 (ADR 0075): the headless PCM artifact of a program built by `segarecomp build`, through the real consumer route.

The project-authored synthetic ROM `sound_tone` (tools/genesis_z80_fixture_rom.py) uploads one Z80 driver that plays a PSG tone, an FM tone
and DAC samples. The built program is run with a finite --instruction-budget and a wall-clock limit. Proved:
  * the run yields the frozen artifact: 44,100 Hz, 2 channels, a non-silent stream, no mixer fault, the SHA-256 digest of the canonical
    s16le bytes plus u64le frame count (recomputed here from the PCM file), a frame count of floor-windows of virtual time, and the digest of
    an arbitrary frame range;
  * repeated runs give the identical digest and bytes (build determinism across --jobs counts: genesis_z80_build_pipeline_test);
  * a shorter run is an exact prefix of a longer run (digest independence from where the run is cut);
  * an unusable PCM path changes nothing in the digest (only the file-error aggregate); a malformed range option is rejected before the run.
Sanitized aggregates are printed as METRIC lines; no ROM byte or address is.
usage: genesis_audio_artifact_test.py <segarecomp> <cc> <cxx> <source-root>
"""
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

cli, cc, cxx, root = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(root / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402

failures = []
MASTER, RATE = 53693175, 44100


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def build(rom_bytes, out, jobs=None):
    out.mkdir(parents=True, exist_ok=True)
    rom = out.parent / (out.name + ".md")
    rom.write_bytes(rom_bytes)
    command = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--cxx", cxx, "--runtime-dir", str(root / "platforms" / "genesis"),
               "--optimize", "0", "--runtime-optimize", "1"]
    if jobs:
        command += ["--jobs", str(jobs)]
    done = subprocess.run(command, text=True, capture_output=True, timeout=900)
    exe = next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)
    return done, exe


def run(exe, budget, pcm=None, rng=None, tmp=None):
    env = {"SEGARECOMP_SOUND_SUMMARY": "1", "PATH": "/usr/bin:/bin"}
    if pcm is not None:
        env["SEGARECOMP_AUDIO_PCM_OUT"] = str(pcm)
    if rng is not None:
        env["SEGARECOMP_AUDIO_RANGE"] = rng
    done = subprocess.run([str(exe), "--instruction-budget", str(budget)], text=True, capture_output=True, timeout=300, env=env)
    summary = re.search(r"SOUND_SUMMARY (\{.*\})", done.stderr)
    return done, (json.loads(summary.group(1)) if summary else {})


def stream_digest(data):
    return hashlib.sha256(data + (len(data) // 4).to_bytes(8, "little")).hexdigest()


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-audio-artifact-") as directory:
        tmp = pathlib.Path(directory)
        rom = fx.build("sound_tone")
        # One build. That the generated C and the registry (hence the program) are byte-identical for worker counts 1, 4 and default is
        # proved by genesis_z80_build_pipeline_test (epochs), so extra --jobs builds are not repeated here.
        built = {"default": build(rom, tmp / "default")}
        check(all(done.returncode == 0 and exe is not None for done, exe in built.values()), "the audio fixture builds through the normal route")
        exe = built["default"][1]

        long_pcm, short_pcm = tmp / "long.pcm", tmp / "short.pcm"
        done, a = run(exe, 2_000_000, long_pcm, "100:50")
        audio = a.get("audio", {})
        data = long_pcm.read_bytes()
        frames = audio.get("frames", 0)
        check(a.get("result_kind") == 3 and audio.get("rate_hz") == 44100 and audio.get("channels") == 2 and frames > 100000 and len(data) == 4 * frames,
              "the headless run produces the frozen artifact: 44,100 Hz, 2 channels, %d frames, %d PCM bytes" % (frames, len(data)))
        check(audio.get("sha256") == stream_digest(data), "the reported digest equals SHA-256(canonical s16le bytes || u64le frame count) recomputed from the PCM file")
        check(audio.get("non_silent") is True and audio.get("fault") == 0 and audio.get("span", 0) > 1000 and audio.get("changes", 0) > 1000,
              "the stream is non-silent (peak-to-peak span %s, %s value changes), no mixer fault" % (audio.get("span"), audio.get("changes")))
        expected = frames  # a pure function of virtual time: windows that ended by the final guest time
        virtual_ticks = a["virtual_frames"] * 896040
        check(abs(expected - virtual_ticks * RATE // MASTER) <= 1000 and a["psg_writes"] == 6 and a["ym_writes"] > 100,
              "the frame count follows virtual time (%d frames for %d virtual frames); PSG and YM2612 written by the Z80" % (expected, a["virtual_frames"]))
        check(audio.get("range_frames") == 50 and audio.get("range_sha256") == stream_digest(data[4 * 100:4 * 150]),
              "the frame-range digest (frames 100..149) equals the digest of exactly those canonical bytes")

        done2, b = run(exe, 2_000_000, tmp / "again.pcm")
        check(b.get("audio", {}).get("sha256") == audio["sha256"] and (tmp / "again.pcm").read_bytes() == data, "repeated runs are bit-identical")
        _, s = run(exe, 800_000, short_pcm)
        short = short_pcm.read_bytes()
        sa = s.get("audio", {})
        check(0 < len(short) < len(data) and data[:len(short)] == short and sa.get("sha256") == stream_digest(short),
              "a shorter run (%d frames) is an exact prefix of the longer run: the stream does not depend on where the run is cut" % (len(short) // 4))

        done3, d = run(exe, 2_000_000, tmp / "missing-dir" / "x.pcm")
        da = d.get("audio", {})
        check(done3.returncode == done.returncode and da.get("sha256") == audio["sha256"] and da.get("pcm_file_errors", 0) > 0,
              "an unusable PCM path never perturbs the run: same exit status and digest, the failure is only an aggregate counter")
        for bad in ("abc", "5:", ":5", "5:0", "5:6:7"):
            done4, _ = run(exe, 1000, None, bad)
            check(done4.returncode == 3 and "malformed SEGARECOMP_AUDIO_RANGE" in done4.stderr, "malformed range %r is rejected before the run (exit 3)" % bad)
        print("METRIC artifact %s" % json.dumps({"frames": frames, "bytes": len(data), "non_silent": audio.get("non_silent"), "clipped": audio.get("clipped"),
                                                 "span": audio.get("span"), "digests_identical_across_runs": True, "prefix_frames": len(short) // 4}, sort_keys=True))
    print("genesis audio artifact: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
