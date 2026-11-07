#!/usr/bin/env python3
"""SEG-131 Checkpoint C (ADR 0278): automatic SRAM and X24C01 EEPROM persistence, end to end.

Project-authored synthetic cartridges only. Each one goes through the production `emit-general-startup-bridge-c` route into generated C, is built
as a REAL viewer executable (SDL3 dummy video driver, scripted quit, no real-time waiting) and is run in separate processes against temporary
save directories -- the same SEGARECOMP_SAVE_DIR contract the launcher uses.

Proved: a guest write survives the process and is read back by the next one (SRAM and EEPROM, the committed cell); a fresh cartridge starts at the
deterministic 0xFF fill; an existing all-zero save stays zero; a save of the wrong size, a directory or a symbolic link in its place is refused
before any guest instruction and left untouched; a typed guest stop, an exhausted budget and a killed process never touch the canonical save; a
failed candidate keeps it; a rebuilt program, other directories and sessions that carry no save directory (replays, the build pass) never see or
change the data; the previous generation is kept.
"""
import importlib.util
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent


def load(name, path):
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


sys.path.insert(0, str(ROOT))
bridge = load("genesis_startup_bridge", ROOT.parent / "tools" / "genesis_startup_bridge.py")
import genesis_cartridge_sram_generated_test as sram  # noqa: E402  (shared synthetic-ROM, emit and encoding helpers)
import genesis_cartridge_eeprom_generated_test as eeprom  # noqa: E402

PATTERN = [0x40 + i for i in range(16)]
failures = []


def check(ok, label):
  print("%-5s %s" % ("ok" if ok else "FAIL", label))
  if not ok:
    failures.append(label)


def move_w_imm_abs(value, address):  # MOVE.W #value,address.L
  return f"33FC{value:04X}{address:08X}"


def vdp_setup():
  """Configures the VDP so that the guest publishes frames (the scripted quit counts published frames)."""
  return "".join(move_w_imm_abs(word, 0xC00004) for word in (0x8124, 0x8230, 0x8404, 0x8528, 0x8B04, 0x8C81, 0x9001))


def sram_guest(stop_after=False):
  code = vdp_setup()
  for i in range(16):  # D-less read-back: copy SRAM byte i to work RAM $FF0000+i
    code += f"13F9{sram.SRAM_START + 2 * i:08X}{0xFF0000 + i:08X}"
  for i in range(16):
    code += sram.move_b_imm_abs(PATTERN[i], sram.SRAM_START + 2 * i)
  if stop_after:
    code += sram.movea_l_imm(0, 0x500000) + sram.move_b_ind_dn(0, 1)  # a runtime read of unmapped space: a typed guest stop after the guest has already written its data
  return code


def eeprom_guest():
  g = eeprom.Guest()
  g.start(); ack = g.send_byte(eeprom.command(5, False)); g.start()
  g.send_byte(eeprom.command(5, True)); read = g.receive_byte(False); g.stop()
  g.start(); g.send_byte(eeprom.command(5, False)); g.send_byte(0xA5); g.stop()
  return vdp_setup() + g.hex(), g, read, ack


def build_viewers(tmp, sources, sdl3, flags):
  """Compiles the shared viewer objects ONCE and links each generated main against them."""
  runtime_dir = ROOT.parent / "platforms/genesis/runtime"
  viewer_dir = ROOT.parent / "platforms/genesis/viewer"
  sv_dir = ROOT.parent / "libs/viewer"
  includes = ["-I", str(runtime_dir), "-I", str(viewer_dir), "-I", str(sv_dir / "include"), "-I", str(sv_dir / "src")] + sdl3[0]
  others = [runtime_dir / "runtime.c", runtime_dir / "vdp_render.c", runtime_dir / "genesis_persistence.c", viewer_dir / "viewer.c", viewer_dir / "viewer_sdl3.c",
            viewer_dir / "viewer_main_hook.c"] + [sv_dir / "src" / n for n in ("sv_settings.c", "sv_layout.c", "sv_menu.c", "sv_input_state.c", "sv_input_diag.c", "sv_font.c",
                                                                               "sv_font_data.c", "sv_ui.c", "sv_host.c", "sv_pages.c", "sv_draw.c")]
  (tmp / "shared").mkdir(exist_ok=True)
  objects, failure = bridge.compile_objects([(flags + includes, src) for src in others], ROOT.parent, tmp / "shared")
  assert failure is None, failure
  exes = {}
  for name, source in sources.items():
    (tmp / ("main-" + name)).mkdir(exist_ok=True)
    mains, failure = bridge.compile_objects([(flags + ["-Dgenesis_runtime_run=genesis_viewer_hook_run"] + includes, source)], ROOT.parent, tmp / ("main-" + name))
    assert failure is None, failure
    exe = tmp / (name + ".exe")
    done = subprocess.run([flags[0], "-o", str(exe)] + mains + objects + sdl3[1] + ([] if os.name == "nt" else ["-lm"]), text=True, capture_output=True)
    assert done.returncode == 0, done.stderr
    exes[name] = exe
  return exes


def session(exe, save_dir, work, budget=5000000, quit_frame=3, script=True, timeout=120, extra_env=None):
  """One interactive session in its own process. The scripted pad quits at `quit_frame` published frames (a clean player exit)."""
  env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_VIEWER_UNTHROTTLED="1", SEGARECOMP_VIEWER_NO_STOP_SCREEN="1",
             SEGARECOMP_VIEWER_SETTINGS=str(work / "viewer-settings.cfg"))
  for name in ("SEGARECOMP_SAVE_DIR", "SEGARECOMP_VIEWER_PAD_SCRIPT"):
    env.pop(name, None)
  if save_dir is not None:
    env["SEGARECOMP_SAVE_DIR"] = str(save_dir)
  if script:
    pad = work / "pad.txt"
    pad.write_text("%d Q\n" % quit_frame)
    env["SEGARECOMP_VIEWER_PAD_SCRIPT"] = str(pad)
  env.update(extra_env or {})
  full = work / "full.json"
  if full.exists():
    full.unlink()
  run = subprocess.run([str(exe), "--instruction-budget", str(budget), "--full-report-path", str(full)], text=True, capture_output=True, env=env, timeout=timeout, cwd=ROOT.parent)
  return run, full


def persist_events(run):
  import json
  out = []
  for line in run.stderr.splitlines():
    if line.startswith("VIEWER_PERSIST "):
      out.append(json.loads(line.split(" ", 1)[1]))
  return out


def work_ram(full):
  import base64
  import json
  return base64.b64decode(json.loads(full.read_text())["runtime"]["work_ram_base64"])


def main():
  cli, compiler, root = sys.argv[1:4]
  sdl3 = bridge.find_sdl3()
  if sdl3 is None:
    print("SKIP: SDL3 not available")
    return 0
  flags = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
  with tempfile.TemporaryDirectory(dir=pathlib.Path(root) / "build", prefix="genesis-persist-") as t:
    tmp = pathlib.Path(t)
    gen = tmp / "gen"
    gen.mkdir()
    eeprom_code, eg, eeprom_read, eeprom_ack = eeprom_guest()
    roms = {
        "sram": sram.build_rom(sram_guest()),
        "sram_stop": sram.build_rom(sram_guest(stop_after=True)),
        "eeprom": sram.build_rom(eeprom_code, size=0x400, header=eeprom.HEADER),
    }
    sources = {name: sram.emit(cli, rom, gen, name, aot=True) for name, rom in roms.items()}
    exes = build_viewers(tmp, sources, sdl3, flags)
    # a second build of the SAME cartridge in a different directory ("rebuild")
    rebuild_dir = tmp / "rebuild"
    rebuild_dir.mkdir()
    exes["sram_rebuilt"] = build_viewers(rebuild_dir, {"sram_rebuilt": sources["sram"]}, sdl3, flags)["sram_rebuilt"]

    def fresh(name):
      d = tmp / name
      d.mkdir()
      return d

    # ================= SRAM: two independent sessions =================
    save = fresh("save-a")
    work = fresh("work-a")
    run, full = session(exes["sram"], save, work)
    events = persist_events(run)
    check(run.returncode == 0 and [e["status"] for e in events] == ["fresh", "stored"], "session 1 (fresh): loads nothing, commits at the clean quit: %s" % events)
    seen = work_ram(full)[:16]
    check(list(seen) == [0xFF] * 16, "a fresh cartridge starts at the deterministic 0xFF fill")
    srm = save / "cartridge.srm"
    data = srm.read_bytes() if srm.exists() else b""
    check(len(data) == 512 and list(data[:16]) == PATTERN and set(data[16:]) == {0xFF}, "cartridge.srm holds exactly the 512 dense bytes the guest wrote (lane layout: byte i at 2i+1)")
    run, full = session(exes["sram"], save, work)
    check([e["status"] for e in persist_events(run)] == ["loaded", "stored"] and list(work_ram(full)[:16]) == PATTERN,
          "session 2 (a new process) reads exactly what session 1 wrote, before its first instruction")
    check((save / "cartridge.srm.prev").exists() and (save / "cartridge.srm.prev").read_bytes() == data and not (save / "cartridge.srm.next").exists(),
          "the previous generation is kept as .prev and no candidate is left behind")

    # a rebuilt program for the same cartridge keeps the data
    run, full = session(exes["sram_rebuilt"], save, work)
    check(list(work_ram(full)[:16]) == PATTERN, "a rebuilt program for the same cartridge sees the same save")

    # ================= existing zero-filled save stays zero =================
    save = fresh("save-zero")
    (save / "cartridge.srm").write_bytes(bytes(512))
    run, full = session(exes["sram"], save, work)
    check(list(work_ram(full)[:16]) == [0] * 16, "an existing all-zero save is loaded unchanged (never reformatted to 0xFF)")

    # ================= refused saves: wrong size, directory, symlink =================
    for label, make in (("a 511-byte", lambda p: p.write_bytes(bytes(511))), ("a 513-byte", lambda p: p.write_bytes(bytes(513))), ("an empty", lambda p: p.write_bytes(b"")),
                        ("a directory in place of the", lambda p: p.mkdir()),
                        ("a symbolic link as the", lambda p: p.symlink_to(tmp / "elsewhere.bin"))):
      if "symbolic" in label and os.name == "nt":
        continue
      save = fresh("save-bad-%d" % len(list(tmp.iterdir())))
      if "symbolic" in label:
        (tmp / "elsewhere.bin").write_bytes(bytes(512))
      make(save / "cartridge.srm")
      before = None if (save / "cartridge.srm").is_dir() else ((save / "cartridge.srm").is_symlink() and "link" or (save / "cartridge.srm").read_bytes())
      run, full = session(exes["sram"], save, work)
      after = None if (save / "cartridge.srm").is_dir() else ((save / "cartridge.srm").is_symlink() and "link" or (save / "cartridge.srm").read_bytes())
      events = persist_events(run)
      check(run.returncode == 6 and events and events[0]["status"] == "refused" and not full.exists() and before == after,
            "%s cartridge.srm is refused before any guest instruction (status 6) and left untouched" % label)
    check((tmp / "elsewhere.bin").read_bytes() == bytes(512), "a symbolic link's target is never modified")

    # ================= sessions that must NOT commit =================
    save = fresh("save-keep")
    run, _ = session(exes["sram"], save, work)
    good = (save / "cartridge.srm").read_bytes()
    # typed guest stop: the guest wrote different data (the same pattern here) and then stopped; make the canonical file distinguishable first
    marker = bytes([0x11]) * 512
    (save / "cartridge.srm").write_bytes(marker)
    run, full = session(exes["sram_stop"], save, work, script=False)
    events = persist_events(run)
    check((save / "cartridge.srm").read_bytes() == marker and events and events[-1]["status"] == "not_clean_exit" and not (save / "cartridge.srm.next").exists(),
          "a typed guest stop never commits: the canonical save is unchanged (%s)" % (events[-1] if events else run.stderr[-200:]))
    run, full = session(exes["sram"], save, work, budget=40, script=False)
    events = persist_events(run)
    check((save / "cartridge.srm").read_bytes() == marker and events and events[-1]["status"] == "not_clean_exit" and events[-1]["detail"] == "runner_resource_limit",
          "an exhausted runner budget (a resource-limit exit) is not a clean session end: nothing committed (%s)" % (events[-1] if events else ""))
    # killed: start, wait until the guest has run, kill -9
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_VIEWER_NO_STOP_SCREEN="1", SEGARECOMP_VIEWER_SETTINGS=str(work / "viewer-settings.cfg"),
               SEGARECOMP_SAVE_DIR=str(save))
    env.pop("SEGARECOMP_VIEWER_PAD_SCRIPT", None)
    child = subprocess.Popen([str(exes["sram"]), "--instruction-budget", "4000000000"], env=env, stderr=subprocess.PIPE, text=True, cwd=ROOT.parent)
    time.sleep(2.0)
    child.send_signal(signal.SIGKILL if os.name != "nt" else signal.SIGTERM)
    child.wait(timeout=30)
    check((save / "cartridge.srm").read_bytes() == marker and not (save / "cartridge.srm.next").exists(), "a killed process never touches the canonical save")

    # a failed commit keeps the previous save: the candidate path is occupied by a directory
    (save / "cartridge.srm.next").mkdir()
    run, full = session(exes["sram"], save, work)
    events = persist_events(run)
    check((save / "cartridge.srm").read_bytes() == marker and events and events[-1]["status"] == "kept_previous",
          "an impossible candidate keeps the previous save and says so (%s)" % (events[-1] if events else ""))
    shutil.rmtree(save / "cartridge.srm.next")
    # write denied: a read-only save directory (root ignores permission bits, so only meaningful for an ordinary user)
    if os.name != "nt" and os.geteuid() != 0:
      ro = fresh("save-readonly")
      (ro / "cartridge.srm").write_bytes(marker)
      os.chmod(ro, 0o555)
      try:
        run, full = session(exes["sram"], ro, work)
        events = persist_events(run)
        check((ro / "cartridge.srm").read_bytes() == marker and events and events[-1]["status"] == "kept_previous", "a write-denied save directory keeps the previous save and reports it")
      finally:
        os.chmod(ro, 0o755)
    # a stale candidate from an earlier crash is replaced by this run's own, never committed as is
    (save / "cartridge.srm.next").write_bytes(b"stale")
    run, full = session(exes["sram"], save, work)
    check(list((save / "cartridge.srm").read_bytes()[:16]) == PATTERN and not (save / "cartridge.srm.next").exists(), "a stale candidate is never mistaken for the session's own")
    # the canonical path became a directory between sessions' end: the replace is refused and nothing is lost
    # (covered at load by the refusal cases above)

    # ================= sessions without a save directory: replays, recordings, the build pass =================
    save = fresh("save-replay")
    (save / "cartridge.srm").write_bytes(marker)
    run, full = session(exes["sram"], None, work)
    check(not persist_events(run) and (save / "cartridge.srm").read_bytes() == marker and list(work_ram(full)[:16]) == [0xFF] * 16,
          "a session without SEGARECOMP_SAVE_DIR runs on the scratch fill and neither reads nor writes any save")

    # ================= isolation between cartridges / variants =================
    save_a, save_b = fresh("iso-a"), fresh("iso-b")
    session(exes["sram"], save_a, work)
    run, full = session(exes["sram"], save_b, work)
    check(list(work_ram(full)[:16]) == [0xFF] * 16 and (save_b / "cartridge.srm").exists(), "a different save directory (another cartridge or variant) is isolated")

    # ================= EEPROM =================
    save = fresh("save-eep")
    work_e = fresh("work-e")
    run, full = session(exes["eeprom"], save, work_e)
    ram = work_ram(full)
    samples = [ram[i] for i in range(eg.samples)]
    check(eeprom.byte_of(samples, eeprom_read) == 0xFF, "EEPROM: a fresh cartridge reads the 0xFF power-on contents")
    eep = save / "cartridge.eep"
    data = eep.read_bytes() if eep.exists() else b""
    check(len(data) == 128 and data[5] == 0xA5 and set(data[:5] + data[6:]) == {0xFF}, "cartridge.eep holds exactly the 128-byte array with the committed cell")
    check([e["status"] for e in persist_events(run)] == ["fresh", "stored"], "EEPROM: first session reports fresh/stored")
    run, full = session(exes["eeprom"], save, work_e)
    ram = work_ram(full)
    samples = [ram[i] for i in range(eg.samples)]
    check(eeprom.byte_of(samples, eeprom_read) == 0xA5 and [e["status"] for e in persist_events(run)] == ["loaded", "stored"],
          "EEPROM: a new process reads the committed cell back through the serial protocol")
    check(len(list(save.iterdir())) == 2 or {p.name for p in save.iterdir()} <= {"cartridge.eep", "cartridge.eep.prev"}, "only the .eep (and its .prev) are written: the SRAM name is not used for an EEPROM cartridge")
    (save / "cartridge.eep").write_bytes(bytes(127))
    before = (save / "cartridge.eep").read_bytes()
    run, full = session(exes["eeprom"], save, work_e)
    check(run.returncode == 6 and (save / "cartridge.eep").read_bytes() == before, "EEPROM: a wrong-size array is refused and left untouched")

  print("%d failure(s)" % len(failures))
  return 1 if failures else 0


if __name__ == "__main__":
  sys.exit(main())
