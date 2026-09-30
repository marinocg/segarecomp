#!/usr/bin/env python3
"""SEG-009-T009: viewer/headless equivalence on a generated-native fixture (hermetic, no window, no SDL).

usage: sms_viewer_equivalence_test.py <sms_image_emitter> <cc> <product-root>

The project-authored `machine_e2e` fixture is emitted through the SMS generation route and linked twice against the same
generated objects: once with the real headless driver and once with the viewer core driven by a fake host (fake clock,
sleeper, input poll, presenter and audio sink: tests/sms_viewer_fake_main.c). Checked:

  * the input stream the viewer recorded, replayed through the headless driver, reproduces the viewer's state digest, its
    per-frame framebuffer hashes (record list and presented frames) and its PCM hash (run and total sample count);
  * a failing audio device and an absent audio sink leave the digest identical and do not stop the run;
  * a mid-run reset restarts the recording: the post-reset run equals a headless run of the recorded script;
  * pacing with the fake clock accumulates the exact rational frame period; the wall clock never enters guest state;
  * no SDL reference exists in the headless, runtime or viewer core sources.
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

import sms_e2e_common as c
from sms_e2e_common import builder

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
PLATFORM = ROOT / "platforms" / "master-system"
FAILED = []
N = builder.E2E_FRAMES


def check(cond, message):
    if not cond:
        FAILED.append(message)
        print("FAIL:", message)


def run(cmd, timeout=300):
    return subprocess.run([str(a) for a in cmd], capture_output=True, text=True, timeout=timeout)


def parse_viewer(out):
    res = {"records": [], "present": []}
    for line in out.splitlines():
        f = line.split()
        if f[0] == "outcome":
            res["outcome"], res["frames"], res["presented"], res["resets"] = int(f[1]), int(f[3]), int(f[5]), int(f[7])
        elif f[0] == "cycles":
            res["cycles"], res["digest"] = int(f[1]), f[3]
        elif f[0] == "record":
            res["records"].append((int(f[1]), f[2]))
        elif f[0] == "present":
            res["present"].append(f[2])
        elif f[0] == "pcm":
            res["pcm_samples"], res["pcm_sha"], res["audio_failures"], res["slept"], res["sleeps"] = int(f[1]), f[2], int(f[4]), int(f[6]), int(f[8])
    return res


def headless_view(res):
    frames = [l.split() for l in (res["dir"] / "frames.txt").read_text().splitlines()]
    run_line = [l.split() for l in (res["dir"] / "audio.sha256").read_text().splitlines() if l.startswith("run ")][0]
    return {"digest": res["digest"], "records": [(int(f[0]), f[3]) for f in frames], "pcm_samples": int(run_line[1]), "pcm_sha": run_line[2],
            "cycles": res["status"]["cycles"]}


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms_viewer_eq_"))
    _tmp_holder.append(tmp)
    native, exe = c.build_native(EMITTER, CC, ROOT, tmp)
    if exe is None:
        print("\n".join(native.failures))
        return 1
    out = tmp / ("gen_" + c.FIXTURE)
    viewer_sources = [PLATFORM / "viewer" / "sms_viewer.c", HERE / "sms_viewer_fake_main.c"]
    native.include.append(PLATFORM / "viewer")
    objs = [out / (pathlib.Path(u).stem + ".o") for u in (out / "sms.units").read_text().split()]
    shared = tmp / "viewer_shared"
    shared.mkdir()
    for src in [s for s in native.sources if s.stem not in ("sms_headless", "sms_audio", "sms_devices_vdp")] + viewer_sources:
        obj = native.compile(src, shared)
        if obj is None:
            print("\n".join(native.failures))
            return 1
        objs.append(obj)
    vexe = tmp / "sms_viewer_fake.exe"
    link = run([CC, *objs, "-o", vexe])
    if link.returncode != 0:
        print("link failed:", link.stderr[:1500])
        return 1

    poll = tmp / "poll.txt"
    poll.write_text(builder.E2E_SCRIPT, encoding="utf-8")

    def viewer(tag, *args):
        r = run([vexe, "--poll-script", poll, *args])
        check(r.returncode == 0, "%s: viewer exit %d: %s" % (tag, r.returncode, r.stderr[:300]))
        return parse_viewer(r.stdout)

    def headless(tag, script, frames):
        art = tmp / ("art_h_" + tag)
        art.mkdir()
        res = native.execute(exe, "h_" + tag, "--frames", str(frames), "--input", str(script))
        check(res["code"] == 0, "%s: headless exit %d" % (tag, res["code"]))
        res["dir"] = tmp / ("art_h_" + tag)
        return headless_view(res)

    # 1. equivalence on the recorded stream
    rec = tmp / "recorded.txt"
    v = viewer("main", "--frames", str(N), "--record", rec)
    script = rec.read_text()
    events = [l.split() for l in script.splitlines()]
    check(v.get("outcome") == 1 and v.get("frames") == N, "viewer ran %d guest frames to the frame limit" % N)
    check(len(events) >= 8 and any(e[3] == "P" for e in events) and any(e[1] != "------" for e in events) and any(e[2] != "------" for e in events),
          "the recorded stream carries pad 1, pad 2 and pause events (%d events)" % len(events))
    check(all(int(a[0]) <= int(b[0]) for a, b in zip(events, events[1:])), "recorded frames are non-decreasing")
    h = headless("main", rec, N)
    check(v["cycles"] == h["cycles"], "same stop cycle (%s vs %s)" % (v["cycles"], h["cycles"]))
    check(v["digest"] == h["digest"], "state digest: viewer %s, headless replay %s" % (v.get("digest"), h["digest"]))
    check(len(v["records"]) >= builder.E2E_LOGGED_FRAMES and v["records"] == h["records"], "per-frame framebuffer hashes (%d frames)" % len(v["records"]))
    check([r[1] for r in v["records"]] == v["present"][:len(v["records"])] and v["presented"] == len(v["records"]),
          "every completed frame was presented exactly once, in order")
    check(v["pcm_samples"] == h["pcm_samples"] > 0 and v["pcm_sha"] == h["pcm_sha"], "PCM hash and sample count (%d samples)" % v["pcm_samples"])
    check(len(set(r[1] for r in v["records"])) > 4, "the run shows more than a few distinct frames")

    # 2. the scripted input is what matters: a different stream gives a different digest (the comparison is not vacuous)
    other = viewer("quiet", "--frames", str(N))  # poll script still applies; compare against an empty-stream headless run
    empty = tmp / "empty.txt"
    empty.write_text("", encoding="utf-8")
    check(headless("empty", empty, N)["digest"] != h["digest"], "an empty stream differs from the recorded one")
    check(other["digest"] == v["digest"], "the viewer is deterministic across runs")

    # 3. audio device failure and absence cannot change the guest
    failing = viewer("audio_fail", "--frames", str(N), "--audio", "fail")
    none = viewer("audio_none", "--frames", str(N), "--audio", "none")
    check(failing["digest"] == v["digest"] and none["digest"] == v["digest"], "failing/absent audio leaves the digest identical")
    check(failing["audio_failures"] > 0 and failing["pcm_sha"] == v["pcm_sha"], "failures were counted and the produced PCM is unchanged")
    check(none["records"] == v["records"], "an absent sink leaves the frames identical")

    # 4. reset: the recording restarts and the post-reset run equals a headless replay
    rec2 = tmp / "recorded_reset.txt"
    k = 20
    r = viewer("reset", "--frames", str(N), "--reset-at", str(k), "--record", rec2)
    check(r["resets"] == 1 and r["frames"] == N, "reset was performed once")
    hr = headless("reset", rec2, N - k)
    check(r["digest"] == hr["digest"] and r["cycles"] == hr["cycles"], "post-reset state equals the headless replay of the post-reset recording")
    check(r["records"] == hr["records"] and r["pcm_sha"] == hr["pcm_sha"] and r["pcm_samples"] == hr["pcm_samples"],
          "post-reset frames and PCM equal the headless replay")

    # 5. pacing: fake clock, exact rational period, no guest influence
    expected = (N - 1) * 59736 * 11 * 10**9 / 39375000
    check(v["sleeps"] == N - 1 and abs(v["slept"] - expected) <= 2, "%d waits slept %d ns, rational expectation %.1f" % (v["sleeps"], v["slept"], expected))

    # 6. headless/runtime/core never reference SDL
    for d in (PLATFORM / "headless", PLATFORM / "runtime", PLATFORM / "viewer" / "sms_viewer.c", PLATFORM / "viewer" / "sms_viewer.h"):
        files = [d] if d.is_file() else list(d.glob("*"))
        for f in files:
            check(not re.search(r"SDL3?/|SDL_|SDL3::", f.read_text(encoding="utf-8", errors="ignore")), "%s must not use SDL" % f.name)
    cm = (PLATFORM / "viewer" / "CMakeLists.txt").read_text()
    check("SDL3::" in cm.split("if(SEGARECOMP_ENABLE_SDL3_VIEWER)")[1] and "SDL3::" not in cm.split("if(SEGARECOMP_ENABLE_SDL3_VIEWER)")[0],
          "SDL3 is linked only inside the optional viewer gate")

    if native.failures:
        print("\n".join(native.failures))
    for m in FAILED:
        print("FAIL:", m)
    print("sms_viewer_equivalence_test:", "FAILED" if FAILED or native.failures else "all passed")
    return 1 if FAILED or native.failures else 0


_tmp_holder = []
try:
    status = main()
finally:
    for d in _tmp_holder:
        shutil.rmtree(d, ignore_errors=True)
sys.exit(status)
