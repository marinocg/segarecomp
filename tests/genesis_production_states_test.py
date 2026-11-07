#!/usr/bin/env python3
"""SEG-131 Checkpoint G (ADR 0278): the whole production chain -- project-authored synthetic ROMs -> `segarecomp build` (generated C11, native compile, the sound viewer with
the generated-native Z80, YM2612, PSG and mixer) -> real viewer processes (SDL3 dummy video, scripted keys) -- for save states, rewind and cartridge persistence.

Proved: a state saved while the Z80/YM2612/PSG are playing is loaded by a NEW process and the resumed sample stream equals the straight run's (the mixer's SHA-256 over every
produced sample, at identical frame counts); a rewind is deterministic and keeps the audio non-silent and fault-free; SRAM written by one process is read by the next, also
through a rebuild of the same cartridge in another directory; a session with no save location, and a budget-limited one, neither read nor write player data.
usage: genesis_production_states_test.py <segarecomp> <cc> <cxx> <source-root>
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

cli, cc, cxx, root = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(root / "tools"))
sys.path.insert(0, str(root / "tests"))
import genesis_z80_fixture_rom as fx  # noqa: E402
import genesis_persistence_test as pt  # noqa: E402

sram = pt.sram
failures = []


def check(ok, label):
  print("%-5s %s" % ("ok" if ok else "FAIL", label))
  if not ok:
    failures.append(label)


def vdp_prelude(m):
  for word in (0x8124, 0x8230, 0x8404, 0x8528, 0x8B04, 0x8C81, 0x9001):   # frames publish: the scripted clock counts them
    m.w(0x33FC)
    m.w(word)
    m.w(0x00C0)
    m.w(0x0004)


def build(rom_bytes, out, sdl_include, sdl_lib):
  out.mkdir(parents=True, exist_ok=True)
  rom = out.parent / (out.name + ".md")
  rom.write_bytes(rom_bytes)
  done = subprocess.run([cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--cxx", cxx, "--runtime-dir", str(root / "platforms" / "genesis"),
                         "--optimize", "0", "--runtime-optimize", "1", "--sdl3-include", sdl_include, "--sdl3-lib", sdl_lib], text=True, capture_output=True, timeout=900)
  exe = next((p for p in out.iterdir() if p.stem == "game" and p.is_file()), None)
  assert done.returncode == 0 and exe is not None, done.stdout[-600:] + done.stderr[-600:]
  return exe


def session(exe, work, save=None, script=None, budget=50_000_000, sdl_lib="", extra=None):
  env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LD_LIBRARY_PATH": sdl_lib, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
         "SEGARECOMP_VIEWER_MUTE": "1", "SEGARECOMP_VIEWER_UNTHROTTLED": "1", "SEGARECOMP_VIEWER_NO_STOP_SCREEN": "1", "SEGARECOMP_VIEWER_SETTINGS": str(work / "settings.cfg"),
         "SEGARECOMP_ROM_SHA256": "7a" * 32, "SEGARECOMP_REWIND_INTERVAL_MS": "100"}
  if save is not None:
    env["SEGARECOMP_SAVE_DIR"] = str(save)
  if script is not None:
    pad = work / "pad.txt"
    pad.write_text(script)
    env["SEGARECOMP_VIEWER_PAD_SCRIPT"] = str(pad)
  env.update(extra or {})
  full = work / "full.json"
  if full.exists():
    full.unlink()
  done = subprocess.run([str(exe), "--instruction-budget", str(budget), "--full-report-path", str(full)], text=True, capture_output=True, env=env, timeout=240)
  return done, full


def audio(done):
  for line in done.stderr.splitlines():
    if line.startswith("VIEWER_AUDIO_SUMMARY "):
      return json.loads(line.split(" ", 1)[1])
  return {}


def events(done, tag):
  return [json.loads(line.split(" ", 1)[1]) for line in done.stderr.splitlines() if line.startswith(tag + " ")]


def main():
  sdl = pt.bridge.find_sdl3()
  if sdl is None:
    print("SKIP: SDL3 not available")
    return 0
  sdl_include = sdl[0][1]
  sdl_lib = sdl[1][1]
  with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-production-") as t:
    tmp = pathlib.Path(t)
    work = tmp / "work"
    work.mkdir()

    # ================= audio-bearing cartridge: save state, rewind =================
    rom = fx.build_sound([("raw", "t")], {"t": fx.sound_tone_driver()}, prelude=vdp_prelude)
    game = build(rom, tmp / "sound", sdl_include, sdl_lib)
    plain = {}
    for clock in range(4, 15):
      done, _ = session(game, work, None, "%d Q\n" % clock, sdl_lib=sdl_lib)
      a = audio(done)
      assert a.get("frames", 0) > 0, done.stderr[-400:]
      plain[clock] = (a["frames"], a["sha256"])
    check(all(audio(session(game, work, None, "9 Q\n", sdl_lib=sdl_lib)[0]).get("sha256") == plain[9][1] for _ in range(2)), "the production viewer's audio stream is deterministic across processes")
    check(len({v[0] for v in plain.values()}) == len(plain) and all(a for a in plain.values()), "calibration: every quit clock ends at its own frame count")
    save = tmp / "save-sound"
    save.mkdir()
    done, _ = session(game, work, save, "6 K\n9 Q\n", sdl_lib=sdl_lib)
    ev = events(done, "VIEWER_STATE")
    check(ev and ev[0]["status"] == "ok" and audio(done)["sha256"] == plain[9][1], "saving while the YM2612, PSG and Z80 play changes nothing the stream can see (%s)" % ev)
    a = audio(done)
    check(a["non_silent"] and a["fault"] == 0 and a["clipped"] == 0, "the stream is non-silent, fault-free and unclipped: %s" % {k: a[k] for k in ("frames", "non_silent", "fault", "clipped")})
    matches = 0
    for quit_clock in range(3, 13):
      done, _ = session(game, work, save, "2 M\n%d Q\n" % quit_clock, sdl_lib=sdl_lib)
      lo = events(done, "VIEWER_STATE")
      b = audio(done)
      assert lo and lo[0]["event"] == "load" and lo[0]["status"] == "ok", done.stderr[-400:]
      partner = next((c for c, v in plain.items() if v[0] == b["frames"]), None)
      if partner is not None:
        matches += 1
        check(b["sha256"] == plain[partner][1], "a NEW process that loaded the state: its resumed sample stream (%d frames) equals the straight run's byte for byte" % b["frames"])
    check(matches >= 1, "at least one loaded run ended at the same frame count as a straight run (%d)" % matches)
    done, _ = session(game, work, save, "10 Y\n14 Q\n", sdl_lib=sdl_lib)
    r1 = audio(done)
    ev = events(done, "VIEWER_STATE")
    done2, _ = session(game, work, save, "10 Y\n14 Q\n", sdl_lib=sdl_lib)
    check(ev and ev[0]["event"] == "rewind" and ev[0]["status"] == "ok", "rewind through the production viewer restores a moment: %s" % ev)
    check(audio(done2) == r1 and r1["non_silent"] and r1["fault"] == 0 and r1["frames"] < plain[14][0], "the rewound future is deterministic, the audio non-silent and fault-free, and earlier in guest time than the straight run")
    done, _ = session(game, work, save, "10 Z\n14 Q\n", sdl_lib=sdl_lib)
    check(audio(done)["sha256"] == plain[14][1] and not events(done, "VIEWER_STATE"), "cancelling the rewind leaves the stream exactly as the straight run's")
    done, _ = session(game, work, None, "6 K\n7 M\n9 Q\n", sdl_lib=sdl_lib)
    check(audio(done)["sha256"] == plain[9][1] and not events(done, "VIEWER_STATE") and not events(done, "VIEWER_PERSIST"),
          "legacy execution without a save location: the state hotkeys do nothing, no persistence, the stream equals the plain run")

    # ================= SRAM cartridge: persistence through the production build, and through a rebuild =================
    srom = sram.build_rom(pt.sram_guest())
    first = build(srom, tmp / "sram-a", sdl_include, sdl_lib)
    rebuilt = build(srom, tmp / "sram-b", sdl_include, sdl_lib)
    save = tmp / "save-sram"
    save.mkdir()
    done, full = session(first, work, save, "3 Q\n", sdl_lib=sdl_lib)
    pe = events(done, "VIEWER_PERSIST")
    check([e["status"] for e in pe] == ["fresh", "stored"], "production build, session 1: loads nothing, stores at the clean quit: %s" % pe)
    check((save / "cartridge.srm").is_file() and (save / "cartridge.srm").stat().st_size == 512 and list((save / "cartridge.srm").read_bytes()[:16]) == pt.PATTERN, "cartridge.srm holds the 512 dense bytes the guest wrote")
    done, full = session(rebuilt, work, save, "3 Q\n", sdl_lib=sdl_lib)
    import base64
    ram = base64.b64decode(json.loads(full.read_text())["runtime"]["work_ram_base64"])
    check([e["status"] for e in events(done, "VIEWER_PERSIST")] == ["loaded", "stored"] and list(ram[:16]) == pt.PATTERN, "a rebuilt program (another build directory) reads the data the first build's session stored")
    before = (save / "cartridge.srm").read_bytes()
    marker = bytes([0x22]) * 512
    (save / "cartridge.srm").write_bytes(marker)
    done, _ = session(first, work, save, None, budget=3000, sdl_lib=sdl_lib)
    check((save / "cartridge.srm").read_bytes() == marker and [e["status"] for e in events(done, "VIEWER_PERSIST")][-1:] == ["not_clean_exit"], "a runner-limit exit (budget) never commits")
    done, _ = session(first, work, None, "3 Q\n", sdl_lib=sdl_lib)
    check((save / "cartridge.srm").read_bytes() == marker and not events(done, "VIEWER_PERSIST"), "a session without a save location (replay / build pass) neither reads nor writes the player's data")
    check(before != marker, "(control) the stored data differed from the marker")

  print("%d failure(s)" % len(failures))
  return 1 if failures else 0


if __name__ == "__main__":
  sys.exit(main())
