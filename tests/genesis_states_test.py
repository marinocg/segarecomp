#!/usr/bin/env python3
"""SEG-131 Checkpoint D (ADR 0278): manual save states, end to end, across separate REAL viewer processes.

A synthetic cartridge (a counting loop in work RAM) goes through the production `emit-general-startup-bridge-c` route into a real viewer executable
(SDL3 dummy video driver). The pad script presses the quick-save (K = F5) and quick-load (M = F8) hotkeys at chosen published frames and quits (Q).

Proved: a state saved by one process is loaded by a NEW process (and by a rebuilt program for the same cartridge) and the guest continues from it, not from
power-on; two independent loads give identical futures; corrupt, truncated, oversized, foreign (another cartridge or version), wrongly-typed (a directory) and
unreadable slot files are refused with the running session intact and the file untouched; a state is never loaded at boot; a failed save keeps the previous
state; a session with no save location has no states.
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
import genesis_persistence_test as pt  # noqa: E402  (shared real-viewer build and session helpers)

sram = pt.sram
bridge = pt.bridge
failures = []


def check(ok, label):
  print("%-5s %s" % ("ok" if ok else "FAIL", label))
  if not ok:
    failures.append(label)


def counter_guest():
  """VDP setup (so frames publish), then forever: counter += 1 in work RAM at $FF0000."""
  setup = pt.vdp_setup()
  loop = "2039" + "%08X" % 0xFF0000 + "5280" + "23C0" + "%08X" % 0xFF0000 + "4ED6"
  loop_address = 0x200 + len(setup) // 2 + 6
  return setup + sram.movea_l_imm(6, loop_address) + loop


def counter_of(full):
  return struct.unpack(">I", base64.b64decode(json.loads(full.read_text())["runtime"]["work_ram_base64"])[:4])[0]


def state_events(run):
  out = []
  for line in run.stderr.splitlines():
    if line.startswith("VIEWER_STATE "):
      out.append(json.loads(line.split(" ", 1)[1]))
  return out


def main():
  cli, compiler, root = sys.argv[1:4]
  sdl3 = bridge.find_sdl3()
  if sdl3 is None:
    print("SKIP: SDL3 not available")
    return 0
  flags = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
  with tempfile.TemporaryDirectory(dir=pathlib.Path(root) / "build", prefix="genesis-states-") as t:
    tmp = pathlib.Path(t)
    gen = tmp / "gen"
    gen.mkdir()
    rom = sram.build_rom(counter_guest(), header=None)
    source = sram.emit(cli, rom, gen, "counter", aot=True)
    exes = pt.build_viewers(tmp, {"counter": source}, sdl3, flags)
    rebuilt = tmp / "rebuilt"
    rebuilt.mkdir()
    exes["rebuilt"] = pt.build_viewers(rebuilt, {"rebuilt": source}, sdl3, flags)["rebuilt"]
    work = tmp / "work"
    work.mkdir()
    counter = exes["counter"]

    def fresh(name):
      d = tmp / name
      d.mkdir()
      return d

    def run(exe, save, script, **kw):
      r, full = pt.session(exe, save, work, script_text=script, **kw)
      return r, (counter_of(full) if full.exists() else None)

    # Calibration: the guest is deterministic, so the counter at a quit frame is a fixed number. x4 < x5 < x8; a power-on start quits at x4 at clock 4.
    x = {}
    for clock in (2, 4, 5, 8):
      r, x[clock] = run(counter, None, "%d Q\n" % clock)
      assert r.returncode == 0 and x[clock] is not None, r.stderr[-300:]
    check(x[2] < x[4] < x[5] < x[8], "calibration: the counting guest is deterministic and monotone (%s)" % x)

    save = fresh("save-a")
    r, a_final = run(counter, save, "5 K\n8 Q\n")
    ev = state_events(r)
    check(ev and ev[0]["event"] == "save" and ev[0]["status"] == "ok", "process 1: quick save (K) succeeds: %s" % ev)
    check(a_final == x[8], "process 1: saving does not disturb the guest (same counter as an unsaved run)")
    quick = save / "states" / "quick.sgstate"
    check(quick.is_file() and quick.stat().st_size > 100000, "quick.sgstate exists (%d bytes)" % (quick.stat().st_size if quick.exists() else 0))
    check(not any(p.suffix in (".next", ".tmp") for p in (save / "states").iterdir()), "no candidate file is left behind")

    # A NEW process loads it at clock 2 and quits at clock 4: the guest continues from the saved moment (counter >= the saved one), not from power-on (x4 < x5).
    r, b4 = run(counter, save, "2 M\n4 Q\n")
    ev = state_events(r)
    check(ev and ev[0]["event"] == "load" and ev[0]["status"] == "ok" and b4 is not None, "process 2 (new): quick load (M) succeeds: %s" % ev)
    check(b4 is not None and b4 >= x[5], "process 2: the guest continues from the saved moment, not from power-on (counter %s >= %s)" % (b4, x[5]))
    r, b4_again = run(counter, save, "2 M\n4 Q\n")
    check(b4_again == b4, "an independent third process loading the same state reaches the identical counter (deterministic restore)")
    r, b5 = run(counter, save, "2 M\n5 Q\n")
    check(b5 is not None and b5 > b4, "and keeps running after the load")
    r, c4 = run(exes["rebuilt"], save, "2 M\n4 Q\n")
    check(c4 == b4 and state_events(r)[0]["status"] == "ok", "a rebuilt program for the same cartridge loads the state and reaches the same future")

    # never auto-loaded at boot
    r, boot = run(counter, save, "4 Q\n")
    check(boot == x[4] and not state_events(r), "a state is never loaded at boot (a plain session equals the unsaved run)")

    # ================= refused slots: the session continues untouched =================
    good = quick.read_bytes()

    def refused(label, mutate, expect_detail=None):
      d = fresh("refuse-%d" % len(list(tmp.iterdir())))
      (d / "states").mkdir()
      target = d / "states" / "quick.sgstate"
      mutate(target, good)
      def signature(path):
        if path.is_symlink():
          return ("link", os.readlink(path))
        return "dir" if path.is_dir() else path.read_bytes()
      before = signature(target)
      r, final = run(counter, d, "2 M\n4 Q\n")
      ev = state_events(r)
      after = signature(target)
      check(ev and ev[0]["event"] == "load" and ev[0]["status"] == "failed" and final == x[4] and before == after and r.returncode == 0,
            "%s: refused (%s), the session runs on exactly as a plain one, the file is untouched" % (label, ev[0]["detail"] if ev else "?"))

    refused("a corrupt payload byte", lambda p, g: p.write_bytes(g[:5000] + bytes([g[5000] ^ 0x5A]) + g[5001:]))
    refused("a corrupt header byte", lambda p, g: p.write_bytes(g[:12] + bytes([g[12] ^ 0xFF]) + g[13:]))
    refused("a truncated file", lambda p, g: p.write_bytes(g[: len(g) // 2]))
    refused("an empty file", lambda p, g: p.write_bytes(b""))
    refused("a trailing byte", lambda p, g: p.write_bytes(g + b"\0"))
    refused("an oversized file", lambda p, g: p.write_bytes(g + bytes(5 * 1024 * 1024)))
    refused("garbage that is not a state", lambda p, g: p.write_bytes(bytes(range(256)) * 600))
    refused("a directory in place of the file", lambda p, g: p.mkdir())
    if os.name != "nt":
      def make_link(p, g):
        (p.parent.parent / "elsewhere.bin").write_bytes(g)
        p.symlink_to(p.parent.parent / "elsewhere.bin")
      refused("a symbolic link to a valid state", make_link)

    # another cartridge / another version / another region: the identity in the state does not match the running machine
    save2 = fresh("save-other")
    (save2 / "states").mkdir()
    (save2 / "states" / "quick.sgstate").write_bytes(good)
    for label, env in (("another cartridge (different image digest)", {"SEGARECOMP_ROM_SHA256": "22" * 32}),
                       ("another version (a patch identity)", {"SEGARECOMP_SAVE_VARIANT": "33" * 32}),
                       ("another region profile", {"SEGARECOMP_GENESIS_REGION": "japan"})):
      r, final = run(counter, save2, "2 M\n4 Q\n", extra_env=env)
      ev = state_events(r)
      check(ev and ev[0]["status"] == "failed" and final == x[4], "%s: refused, the session is intact (%s)" % (label, ev[0]["detail"] if ev else "?"))
    check((save2 / "states" / "quick.sgstate").read_bytes() == good, "the foreign state file is left untouched")

    # no save location / no identity: no states, and nothing is written anywhere
    r, final = run(counter, None, "2 K\n3 M\n4 Q\n")
    ev = state_events(r)
    check(final == x[4] and not ev, "without a save location both hotkeys are refused by the host (the provider reports itself unavailable) and the session is untouched")
    r, final = run(counter, save, "2 K\n4 Q\n", extra_env={"SEGARECOMP_ROM_SHA256": "not-a-digest"})
    ev = state_events(r)
    check(not ev and final == x[4] and quick.read_bytes() == good, "a session without a verified game identity cannot write a state (the existing state is unchanged)")

    # a failed save keeps the previous valid state: the candidate path is occupied by a directory
    save3 = fresh("save-fail")
    r, _ = run(counter, save3, "5 K\n6 Q\n")
    first = (save3 / "states" / "quick.sgstate").read_bytes()
    (save3 / "states" / "quick.sgstate.next").mkdir()
    r, _ = run(counter, save3, "7 K\n8 Q\n")
    ev = state_events(r)
    check(ev and ev[0]["status"] == "failed" and (save3 / "states" / "quick.sgstate").read_bytes() == first, "a failed save reports the failure and keeps the previous valid state (%s)" % (ev[0]["detail"] if ev else "?"))
    if os.name != "nt" and os.geteuid() != 0:
      save4 = fresh("save-readonly")
      (save4 / "states").mkdir()
      os.chmod(save4 / "states", 0o555)
      try:
        r, _ = run(counter, save4, "5 K\n6 Q\n")
        ev = state_events(r)
        check(ev and ev[0]["status"] == "failed" and not list((save4 / "states").iterdir()), "a write-denied state folder fails the save and leaves nothing behind")
      finally:
        os.chmod(save4 / "states", 0o755)

  print("%d failure(s)" % len(failures))
  return 1 if failures else 0


if __name__ == "__main__":
  sys.exit(main())
