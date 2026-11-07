#!/usr/bin/env python3
"""SEG-131 Checkpoint E (ADR 0278): the bounded in-memory rewind engine of the Genesis state service.

The REAL runtime, the generated-native Z80 (a registered synthetic sound driver), the YM2612/PSG/mixer, SRAM and VDP are driven by a script through the real
retirement hook; after every retirement the service gets its once-per-slice call, exactly where the viewer hook makes it. Proved: snapshots every 500 ms of
GUEST time, ordered; eviction by age and by byte budget with the budget never exceeded; a snapshot that cannot fit disables rewind with an explicit reason while the
game runs on bit-identically; enabling rewind never changes the guest; cancel (browse then close) mutates nothing; confirming an entry restores it, drops every newer
entry (a new branch) and the replayed future equals the straight run (full state hash and the mixed audio stream); a slot load starts a fresh history.
usage: genesis_rewind_test.py <registry_emitter> <cc> <source-root> <c++>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[3])
sys.path.insert(0, str(root / "tests"))
import genesis_snapshot_test as st  # noqa: E402  (the synthetic Z80 sound driver, the script builder and the assertions' helpers)

z = st.z
ymbuild = st.ymbuild
registry_emitter, cc = sys.argv[1], sys.argv[2]
cxx = sys.argv[4] if len(sys.argv) > 4 else "c++"
STEP = 50000                  # M68K cycles per retirement = 350,000 master ticks
TICKS_PER_STEP = STEP * 7
TICKS_PER_SECOND = 53693175
failures = []


def check(ok, label):
  print("%-5s %s" % ("ok" if ok else "FAIL", label))
  if not ok:
    failures.append(label)


def steps_for(seconds):
  return int(seconds * TICKS_PER_SECOND / TICKS_PER_STEP)


def prelude():
  s = st.Script()
  for word in (0x8124, 0x8230, 0x8404, 0x8528, 0x8B04, 0x8C81, 0x9001):
    s.w16(st.VDP_CTRL, word)
  s.epoch("X")
  return s


def run_steps(s, count):
  for _ in range(count):
    s.raw("retire %d" % STEP)


def run(exe, script, budget, interval=500, target=60000, state_dir=None):
  path = pathlib.Path(tempfile.mkdtemp(prefix="sgrw")) / "script.txt"
  path.write_text(script.text() if isinstance(script, st.Script) else script)
  cmd = [str(exe), str(path), str(budget), str(interval), str(target)] + ([str(state_dir)] if state_dir else [])
  done = subprocess.run(cmd, text=True, capture_output=True)
  assert done.returncode == 0, done.stderr[-400:] + done.stdout[-400:]
  return done.stdout, done.stderr


def hists(out):
  result = []
  for m in re.finditer(r"^HIST (\d+) (\d+) (\d+) (\d+) (\d+)$((?:\nENTRY .*)*)", out, re.M):
    entries = [(int(e.group(1)), int(e.group(2)), e.group(3)) for e in re.finditer(r"ENTRY (\d+) (\d+) (\d+x\d+)", m.group(6))]
    result.append({"count": int(m.group(1)), "bytes": int(m.group(2)), "captures": int(m.group(3)), "evictions": int(m.group(4)), "failures": int(m.group(5)), "entries": entries})
  return result


def full(out):
  return re.findall(r"^FULL ([0-9a-f]{64})$", out, re.M)


def mixer(out):
  return re.findall(r"^MIXER frames=\d+ sha=([0-9a-f]{64})$", out, re.M)


def main():
  with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O1", cache=False)
    rt = root / "platforms/genesis/runtime"
    viewer = root / "platforms/genesis/viewer"
    spec = tmp / "full.spec"
    spec.write_text(st.registry_spec(["X"]))
    done = subprocess.run([registry_emitter, str(spec), str(tmp / "full"), "genesis_z80"], text=True, capture_output=True)
    assert done.returncode == 0, done.stdout + done.stderr
    exe, message = z.compile_units(
        tc, tmp / "full", "genesis_z80",
        extra_sources=[root / "tests/tools/genesis_rewind_harness.c", rt / "runtime.c", rt / "vdp_render.c", rt / "z80_machine.c", rt / "genesis_audio.c", rt / "genesis_mixer.c",
                       rt / "genesis_snapshot.c", rt / "genesis_persistence.c", viewer / "genesis_viewer_state.c", root / "libs/device/sega/psg/src/sn76489.c"],
        extra_flags=["-I", str(rt), "-I", str(viewer), "-I", str(root / "libs/viewer/include"), "-I", str(root / "libs/device/sega/psg/include"),
                     "-I", str(root / "libs/device/sega/ym2612/include"), "-DSEGARECOMP_GENESIS_SOUND"],
        extra_objects=ymbuild.build_objects(cc, cxx, root, tmp / "ymobj"))
    assert exe is not None, message
    big = 96 * 1024 * 1024

    # ================= cadence and ordering (10.2 s of guest time) =================
    s = prelude()
    run_steps(s, steps_for(10.2))
    s.raw("hist")
    s.raw("fullhash")
    out, err = run(exe, s, big)
    h = hists(out)[0]
    ages = [a for _, a, _ in h["entries"]]
    check(19 <= h["count"] <= 21 and h["captures"] == h["count"] and h["evictions"] == 0, "a snapshot every 500 ms of guest time: %d entries in 10.2 s" % h["count"])
    check(all(ages[i] > ages[i + 1] for i in range(len(ages) - 1)) and all(480 <= ages[i] - ages[i + 1] <= 530 for i in range(len(ages) - 1)),
          "entries are ordered oldest first and spaced by the interval (%s...)" % ages[:4])
    check(all(dims == "160x112" for _, _, dims in h["entries"]), "each entry carries a 160x112 thumbnail of the guest's own frame")
    check(0 < h["bytes"] <= big and "anchor_moved" not in err, "history bytes are accounted (%d bytes for %d entries)" % (h["bytes"], h["count"]))
    straight = full(out)[0]

    # ================= enabling rewind never changes the guest =================
    s2 = prelude()
    run_steps(s2, steps_for(10.2))
    s2.raw("fullhash")
    out_off, _ = run(exe, s2, 0)
    check(full(out_off)[0] == straight, "the guest is bit-identical with rewind enabled and disabled (capture is const, nothing is fed back)")
    s3 = prelude()
    s3.raw("avail")
    out_off2, _ = run(exe, s3, 0)
    check("AVAIL 0 Rewind is turned off." in out_off2, "disabled rewind says so")

    # ================= age eviction =================
    s = prelude()
    run_steps(s, steps_for(10.2))
    s.raw("hist")
    h = hists(run(exe, s, big, target=3000)[0])[0]
    ages = [a for _, a, _ in h["entries"]]
    check(h["count"] <= 7 and max(ages) <= 3000 + 600 and h["evictions"] > 0, "entries older than the 3 s target are evicted: %d kept, oldest %d ms" % (h["count"], max(ages)))

    # ================= byte-budget eviction =================
    size = h["bytes"] // max(h["count"], 1)
    budget = int(size * 4.5)
    s = prelude()
    maxima = []
    for _ in range(8):
      run_steps(s, steps_for(1.3))
      s.raw("hist")
    out, _ = run(exe, s, budget, target=600000)
    hs = hists(out)
    check(all(x["bytes"] <= budget for x in hs) and hs[-1]["count"] <= 4 and hs[-1]["evictions"] > 0, "the byte budget is never exceeded (%s of %d)" % ([x["bytes"] for x in hs], budget))
    ages = [a for _, a, _ in hs[-1]["entries"]]
    check(all(ages[i] > ages[i + 1] for i in range(len(ages) - 1)) and ages[0] < 4000, "under pressure the OLDEST entries go first: the retained moments are the most recent")
    s = prelude()
    run_steps(s, steps_for(6.0))
    s.raw("avail")
    out, _ = run(exe, s, budget, target=600000)
    check(re.search(r"^NOTE 1 Memory limit reached: the last \d+ seconds are kept\.$", out, re.M) is not None, "the player is told the real, shorter span when the memory limit is what bounds the history")

    # ================= a snapshot that cannot fit =================
    s = prelude()
    run_steps(s, steps_for(10.2))
    s.raw("avail")
    s.raw("hist")
    s.raw("fullhash")
    out, _ = run(exe, s, 50000)
    check("AVAIL 0 Rewind is unavailable: one saved moment" in out and "does not fit" in out and hists(out)[0]["count"] == 0, "a snapshot larger than the budget disables rewind with an explicit reason and keeps no memory")
    check(full(out)[0] == straight, "and the game runs on, bit-identical to the unrewound run")

    # ================= cancel mutates nothing =================
    s = prelude()
    run_steps(s, steps_for(5.2))
    s.raw("fullhash")
    s.raw("open")
    s.raw("hist")
    s.raw("cancel")
    s.raw("fullhash")
    out, err = run(exe, s, big)
    fh = full(out)
    check(len(fh) == 2 and fh[0] == fh[1] and "anchor_moved" not in err, "browsing then cancelling changes nothing in the machine")

    # ================= confirm: restore, new branch, replay equivalence =================
    total = steps_for(10.2)
    s = prelude()
    run_steps(s, total)
    s.raw("ticks")
    s.raw("hist")
    s.raw("mixer")
    s.raw("fullhash")
    pick = 9
    s.raw("restore %d" % pick)
    s.raw("ticks")
    s.raw("hist")
    s.raw("fullhash")
    out, err = run(exe, s, big)
    t_end = int(re.search(r"^TICKS (\d+)", out, re.M).group(1))
    ticks_after = [int(m.group(1)) for m in re.finditer(r"^TICKS (\d+)", out, re.M)]
    hs = hists(out)
    check(re.search(r"^RESTORE %d 0  count=%d$" % (pick, pick + 1), out, re.M) is not None, "confirming entry %d restores it and drops every newer entry (a new branch): %d left" % (pick, hs[1]["count"]))
    remaining = (t_end - ticks_after[1]) // TICKS_PER_STEP
    check((t_end - ticks_after[1]) % TICKS_PER_STEP == 0 and 0 < remaining < total, "the restored moment is %d retirements before the end" % remaining)
    s = prelude()
    run_steps(s, total)
    s.raw("mixer")
    s.raw("fullhash")
    reference, _ = run(exe, s, big)
    s = prelude()
    run_steps(s, total)
    s.raw("restore %d" % pick)
    run_steps(s, remaining)
    s.raw("mixer")
    s.raw("fullhash")
    s.raw("hist")
    s.raw("resets")
    replay, err = run(exe, s, big)
    check(full(replay)[0] == full(reference)[0], "after a confirmed rewind the replayed future equals the straight run (CPU, memory, VDP, Z80, devices)")
    check(mixer(replay)[0] == mixer(reference)[0], "and so does the audio stream (SHA-256 over every sample, YM2612 and PSG included)")
    check("RESETS 1" in replay, "the sink reset (host audio queue, pacing, frame baseline, input latches) ran exactly once for the restore")
    hr = hists(replay)[0]
    ages = [a for _, a, _ in hr["entries"]]
    check(all(ages[i] > ages[i + 1] for i in range(len(ages) - 1)) and hr["count"] >= pick + 1, "the new branch continues the history in order")

    # restoring a very old entry then a newer one is impossible (it was dropped): the oldest-first indices stay valid and bounded
    s = prelude()
    run_steps(s, total)
    s.raw("restore 3")
    s.raw("restore 8")
    out, _ = run(exe, s, big)
    check("RESTORE 3 0" in out and re.search(r"^RESTORE 8 -1 that moment is no longer available count=4$", out, re.M) is not None, "an entry newer than the branch point no longer exists")

    # ================= slot load starts a fresh history =================
    state_dir = tmp / "states"
    s = prelude()
    run_steps(s, steps_for(3.2))
    s.raw("save 1")
    run_steps(s, steps_for(3.2))
    s.raw("hist")
    s.raw("load 1")
    s.raw("hist")
    s.raw("fullhash")
    out, _ = run(exe, s, big, state_dir=state_dir)
    hs = hists(out)
    check("SAVE 1 0" in out and "LOAD 1 0  count=0" in out and hs[0]["count"] > 0 and hs[1]["count"] == 0, "loading a slot discards the rolling history of the abandoned timeline")

    # ================= retained memory over a long session =================
    s = prelude()
    marks = []
    done_s = 0
    for second in (10, 30, 62):
      run_steps(s, steps_for(second - done_s))
      done_s = second
      s.raw("hist")
    out, _ = run(exe, s, big)
    hs = hists(out)
    print("INFO retained history: " + ", ".join("%ds: %d entries, %.1f MiB" % (sec, x["count"], x["bytes"] / 1048576.0) for sec, x in zip((10, 30, 62), hs)))
    check(hs[2]["count"] <= 121 and hs[2]["bytes"] < big and hs[2]["count"] >= 118, "60 s of retained history stays well inside the 96 MiB budget (%d entries)" % hs[2]["count"])

  print("%d failure(s)" % len(failures))
  return 1 if failures else 0


if __name__ == "__main__":
  sys.exit(main())
