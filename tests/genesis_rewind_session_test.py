#!/usr/bin/env python3
"""SEG-131 Checkpoint F (ADR 0278): the rewind surface inside a REAL viewer process (SDL3 dummy video driver), keyboard path end to end.

A counting guest runs; the scripted pad opens the rewind surface at a published frame (Y = Left, Left, Return; Z = Left x3, Escape) through real queued key events consumed
by the real surface loop. Proved: confirming restores the machine from the history (the future starts at an EARLIER guest moment and then continues: the final counter is
that of an earlier time, not the plain run's), cancelling changes nothing at all (the final counter equals the unrewound run's), and the guest keeps running afterwards.
"""
import base64
import json
import os
import pathlib
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
import genesis_persistence_test as pt  # noqa: E402
import genesis_states_test as gs  # noqa: E402

sram = pt.sram
failures = []


def check(ok, label):
  print("%-5s %s" % ("ok" if ok else "FAIL", label))
  if not ok:
    failures.append(label)


def main():
  cli, compiler, root = sys.argv[1:4]
  sdl3 = pt.bridge.find_sdl3()
  if sdl3 is None:
    print("SKIP: SDL3 not available")
    return 0
  flags = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
  with tempfile.TemporaryDirectory(dir=pathlib.Path(root) / "build", prefix="genesis-rewind-") as t:
    tmp = pathlib.Path(t)
    gen = tmp / "gen"
    gen.mkdir()
    source = sram.emit(cli, sram.build_rom(gs.counter_guest(), header=None), gen, "counter", aot=True)
    counter = pt.build_viewers(tmp, {"counter": source}, sdl3, flags)["counter"]
    work = tmp / "work"
    work.mkdir()
    env = {"SEGARECOMP_REWIND_INTERVAL_MS": "100"}

    def run(script, save=None):
      r, full = pt.session(counter, save, work, script_text=script, extra_env=env)
      return r, (gs.counter_of(full) if full.exists() else None)

    x = {}
    for clock in (20, 25, 30, 40):
      r, x[clock] = run("%d Q\n" % clock)
      assert r.returncode == 0 and x[clock] is not None, r.stderr[-300:]
    check(x[20] < x[25] < x[30] < x[40], "calibration: monotone and deterministic %s" % x)

    r, n = run("30 Z\n40 Q\n")
    events = [line for line in r.stderr.splitlines() if line.startswith("VIEWER_STATE")]
    check(n == x[40] and not [e for e in events if '"rewind"' in e], "browsing three moments back and cancelling changes nothing: the final state equals the unrewound run")

    r, b = run("30 Y\n40 Q\n")
    events = [json.loads(line.split(" ", 1)[1]) for line in r.stderr.splitlines() if line.startswith("VIEWER_STATE")]
    check(len(events) == 1 and events[0]["event"] == "rewind" and events[0]["status"] == "ok", "confirming reports the restore in the log: %s" % events)
    check(b is not None and b < x[40] and b > x[20], "the guest resumed from an EARLIER moment and kept running (counter %s between %s and %s)" % (b, x[20], x[40]))
    r, b_again = run("30 Y\n40 Q\n")
    check(b_again == b, "the rewound future is deterministic (an identical second process)")
    r, c = run("30 Y\n31 Y\n40 Q\n")
    check(c is not None and c < b, "rewinding twice goes back further (the new branch has its own history)")

    # the SHIPPED defaults (nothing overridden): moments are closely spaced (500 ms of guest time), so a rewind a couple of seconds into the game has real history to go back to
    for clock in (40, 130):
      r, x[clock] = run("%d Q\n" % clock)
    r = pt.session(counter, None, work, script_text="120 Y\n130 Q\n", extra_env={"SEGARECOMP_REWIND_INTERVAL_MS": ""})[0]
    full = work / "full.json"
    d = gs.counter_of(full)
    stats = json.loads(next(l for l in r.stderr.splitlines() if l.startswith("VIEWER_REWIND_STATS ")).split(" ", 1)[1])
    ev = [json.loads(l.split(" ", 1)[1]) for l in r.stderr.splitlines() if l.startswith("VIEWER_STATE")]
    check(stats["captures"] >= 3 and ev and ev[0]["event"] == "rewind" and ev[0]["status"] == "ok",
          "default policy: %d snapshots in the first ~2 s of guest time and a rewind at frame 120 restores one: %s" % (stats["captures"], ev))
    check(x[40] < d < x[130], "the default rewind goes back only a second or two (counter %s between %s and %s)" % (d, x[40], x[130]))

  print("%d failure(s)" % len(failures))
  return 1 if failures else 0


if __name__ == "__main__":
  sys.exit(main())
