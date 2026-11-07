#!/usr/bin/env python3
"""SEG-131 (ADR 0278): before/after measurements of save states and rewind on a project-authored synthetic workload. Reports observed medians and worst cases only: no invented
guarantees, nothing is asserted. Requires a built `segarecomp`, a C compiler, SDL3 and the repository's test fixtures (tools/genesis_z80_fixture_rom.py).

  genesis_state_perf.py <segarecomp> <cc> <c++> <source-root> [--runs N] [--seconds S]

Measures, on the real production viewer (unthrottled, SDL dummy driver, muted audio) running a generated-native 68000 program with a Z80 driver playing PSG, FM and DAC:
  * wall time per guest second with rewind DISABLED (SEGARECOMP_REWIND=0) and ENABLED (the default policy: a snapshot every 500 ms of guest time, 96 MiB budget);
  * the viewer's own report of retained history, and the hot paths of the runtime harness (snapshot capture / restore time and size) are measured by
    tests/tools/genesis_rewind_harness.c `bench` (see docs/user/save-states.md for the figures this PR recorded).
"""
import argparse
import json
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("cli")
  parser.add_argument("cc")
  parser.add_argument("cxx")
  parser.add_argument("root")
  parser.add_argument("--runs", type=int, default=5)
  parser.add_argument("--seconds", type=int, default=20, help="guest seconds per run")
  args = parser.parse_args()
  root = pathlib.Path(args.root).resolve()
  sys.path.insert(0, str(root / "tools"))
  sys.path.insert(0, str(root / "tests"))
  import genesis_z80_fixture_rom as fx
  import genesis_startup_bridge as bridge

  sdl = bridge.find_sdl3()
  if sdl is None:
    print("SDL3 not found")
    return 2

  def prelude(m):
    for word in (0x8124, 0x8230, 0x8404, 0x8528, 0x8B04, 0x8C81, 0x9001):
      m.w(0x33FC); m.w(word); m.w(0x00C0); m.w(0x0004)

  with tempfile.TemporaryDirectory(dir=root / "build", prefix="state-perf-") as t:
    tmp = pathlib.Path(t)
    out = tmp / "game"
    rom = tmp / "game.md"
    rom.write_bytes(fx.build_sound([("raw", "t")], {"t": fx.sound_tone_driver()}, prelude=prelude))
    built = subprocess.run([args.cli, "build", "--rom", str(rom), "--output", str(out), "--cc", args.cc, "--cxx", args.cxx, "--runtime-dir", str(root / "platforms" / "genesis"),
                            "--optimize", "1", "--runtime-optimize", "1", "--sdl3-include", sdl[0][1], "--sdl3-lib", sdl[1][1]], capture_output=True, text=True)
    game = next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)
    if built.returncode != 0 or game is None:
      print(built.stdout[-500:] + built.stderr[-500:])
      return 1
    frames = args.seconds * 60
    script = tmp / "pad.txt"
    script.write_text("%d Q\n" % frames)
    base = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LD_LIBRARY_PATH": sdl[1][1], "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
            "SEGARECOMP_VIEWER_MUTE": "1", "SEGARECOMP_VIEWER_UNTHROTTLED": "1", "SEGARECOMP_VIEWER_NO_STOP_SCREEN": "1", "SEGARECOMP_VIEWER_SETTINGS": str(tmp / "s.cfg"),
            "SEGARECOMP_VIEWER_PAD_SCRIPT": str(script)}

    def run(extra):
      times = []
      for _ in range(args.runs):
        t0 = time.perf_counter()
        done = subprocess.run([str(game), "--instruction-budget", "2000000000"], capture_output=True, text=True, env={**base, **extra})
        times.append(time.perf_counter() - t0)
        assert "VIEWER_AUDIO_SUMMARY" in done.stderr, done.stderr[-300:]
      return times

    off = run({"SEGARECOMP_REWIND": "0"})
    on = run({})
    print(json.dumps({
        "workload": "synthetic generated-native 68000 + Z80 sound driver (PSG, FM, DAC), %d guest seconds, unthrottled, software renderer, muted" % args.seconds,
        "runs": args.runs,
        "rewind_disabled_seconds": {"median": round(statistics.median(off), 3), "min": round(min(off), 3), "max": round(max(off), 3)},
        "rewind_enabled_seconds": {"median": round(statistics.median(on), 3), "min": round(min(on), 3), "max": round(max(on), 3)},
        "overhead_percent_of_median": round(100.0 * (statistics.median(on) / statistics.median(off) - 1.0), 2),
        "snapshots_taken_per_run": args.seconds * 2,
    }, indent=2))
  return 0


if __name__ == "__main__":
  sys.exit(main())
