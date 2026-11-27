#!/usr/bin/env python3
"""SEG-147-T004/T005 (ADR 0325): a scheduled Master System mod inside the REAL played program (SDL3 dummy video and audio drivers): live input, quick save / quick load, visual rewind and replay.

usage: sms_viewer_mod_test.py <segarecomp-cli> <cc> <product-root>

The synthetic gameplay example is built through the production `segarecomp build` route with the SDL3 viewer. The viewer's scripted pad presses button 2 while the mod runs, so the mod's own state changes
every frame. Proved: the played program is deterministic with the mod; its recorded PHYSICAL input replayed by the headless build of the same mod reproduces the mod's write witness; a state saved in one
process and loaded in another resumes the identical canonical state (MODSTATE included); a rewind across mod-state changes is deterministic and cancelling it changes nothing; a Vanilla program and a
modded program refuse each other's states."""
import hashlib
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import genesis_startup_bridge as bridge  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def main():
    cli, cc = sys.argv[1], sys.argv[2]
    if sys.platform != "linux" or bridge.find_sdl3() is None:
        print("SKIP: SDL3 or a Linux host is not available (the SDL3 CI job on Linux runs this test)")
        return 0
    include, lib = bridge.find_sdl3()[0][1], bridge.find_sdl3()[1][1]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms_viewer_mod_"))
    example = ROOT / "platforms" / "master-system" / "sdk" / "examples" / "gameplay"
    spec = importlib.util.spec_from_file_location("sms_runner_fixture", example / "fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    rom = tmp / "runner.sms"
    rom.write_bytes(fixture.build())
    package = tmp / "runner.segapatch"
    r = subprocess.run([cli, "mod", "pack", str(example), "--out", str(package)], text=True, capture_output=True, timeout=120)
    identity = json.loads(r.stdout)["package_identity"]

    def build(name, play, mod):
        out = tmp / name
        args = [cli, "build", "--rom", str(rom), "--output", str(out), "--cc", cc, "--optimize", "1", "--runtime-dir", str(ROOT / "platforms" / "master-system"), "--platform", "master-system"]
        args += ["--luau-package", str(package), "--luau-identity", identity] if mod else ["--mapper", "rom_only"]
        if play:
            args += ["--sdl3-include", include, "--sdl3-lib", lib]
        p = subprocess.run(args, text=True, capture_output=True, timeout=1200)
        if p.returncode != 0:
            print(p.stdout[-1500:], p.stderr[-1500:], (out / "build.log").read_text()[-3000:])
            raise SystemExit("build failed: " + name)
        return out / "game"

    played, vanilla, headless = build("played", True, True), build("vanilla", True, False), build("headless", False, True)
    work = tmp / "work"
    work.mkdir()

    def session(program, frames, script, save=None, interval=None, record=None, identity_env=None):
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_VIEWER_SETTINGS=str(work / "viewer-settings.cfg"), SEGARECOMP_ROM_SHA256="11" * 32)
        for name in ("SEGARECOMP_SAVE_DIR", "SEGARECOMP_SAVE_VARIANT", "SEGARECOMP_REWIND", "SEGARECOMP_REWIND_INTERVAL_MS"):
            env.pop(name, None)
        if identity_env is not None:   # exactly these identity variables, nothing else
            env.pop("SEGARECOMP_ROM_SHA256")
            env.update(identity_env)
        (work / "pad.txt").write_text(script)
        report = work / "report.json"
        report.unlink(missing_ok=True)
        env["SEGARECOMP_VIEWER_PAD_SCRIPT"] = str(work / "pad.txt")
        env["SEGARECOMP_VIEWER_STATE_REPORT"] = str(report)
        if save is not None:
            env["SEGARECOMP_SAVE_DIR"] = str(save)
        if interval is not None:
            env["SEGARECOMP_REWIND_INTERVAL_MS"] = interval
        args = [str(program), "--viewer-unthrottled", "--viewer-mute", "--viewer-frames", str(frames)]
        if record is not None:
            args += ["--viewer-record", str(record)]
        r = subprocess.run(args, text=True, capture_output=True, env=env, timeout=180)   # bounded: frame limit and timeout
        rep = json.loads(report.read_text()) if report.exists() else None
        lines = {}
        for line in r.stderr.splitlines():
            key, _, rest = line.partition(" ")
            if key in ("VIEWER_STATE", "MOD_SUMMARY", "MOD_LOADED"):
                lines.setdefault(key, []).append(json.loads(rest))
        return r, rep, lines.get("VIEWER_STATE", []), (lines.get("MOD_SUMMARY") or [None])[0]

    held = "20 12\n"   # buttons 1 and 2 held from poll 20: the mod runs, scores and counts every frame
    r, plain, _, mod = session(played, 120, held)
    check(r.returncode == 0 and plain is not None and mod is not None and mod["faulted"] is False and mod["committed_writes"] >= 119, "the played program runs the mod: %s %s" % (r.returncode, mod))
    check(session(played, 120, held)[1] == plain, "the played program with a mod is deterministic")
    record = work / "record.txt"
    _, _, _, recorded = session(played, 120, held, record=record)
    text = record.read_text()
    check("----12" in text and "---R" not in text, "the recording holds the PHYSICAL input (the mod's held Right is not recorded): %r" % text)
    replay = subprocess.run([str(headless), "--frames", "120", "--cycle-budget", "20000000", "--input", str(record)], text=True, capture_output=True, timeout=120)
    summary = [json.loads(l.split(" ", 1)[1]) for l in replay.stderr.splitlines() if l.startswith("MOD_SUMMARY ")]
    check(replay.returncode == 0 and summary and recorded is not None and summary[0]["write_digest"] == recorded["write_digest"] and summary[0]["committed_writes"] == recorded["committed_writes"],
          "replaying the recording in the headless build of the same mod reproduces the write witness")

    # ---- save in one process, load in another: the identical future, mod state included ----
    save = tmp / "save"
    r, rep, ev, _ = session(played, 120, "20 12\n60 12K\n61 12\n", save)
    check(rep == plain and ev and ev[0]["event"] == "save" and ev[0]["status"] == "ok", "saving does not perturb the run: %s" % ev)
    r, rep, ev, loaded = session(played, 70, "10 12M\n11 12\n", save)
    check(ev and ev[0]["event"] == "load" and ev[0]["status"] == "ok", "a separate process loads the state: %s" % ev)
    check(rep is not None and rep["canonical_sha256"] == plain["canonical_sha256"] and loaded["write_digest"] == mod["write_digest"],
          "... and reaches the uninterrupted run's canonical state and write witness (the mod's run counter was restored with the guest)")

    # ---- the embedded identity is authoritative: no external value changes what a modded program records or accepts ----
    effective = hashlib.sha256(fixture.build()).digest()   # a script-only mod: the effective cartridge is the fixture itself
    environments = {"agreeing": {"SEGARECOMP_ROM_SHA256": effective.hex(), "SEGARECOMP_SAVE_VARIANT": identity}, "conflicting": {"SEGARECOMP_ROM_SHA256": "22" * 32, "SEGARECOMP_SAVE_VARIANT": "33" * 32},
                    "vanilla-claim": {"SEGARECOMP_ROM_SHA256": "11" * 32, "SEGARECOMP_SAVE_VARIANT": "vanilla"}, "malformed": {"SEGARECOMP_ROM_SHA256": "zz", "SEGARECOMP_SAVE_VARIANT": "zz"}, "absent": {}}
    digests = set()
    for label, environment in environments.items():
        folder = tmp / ("identity-" + label)
        r, rep, ev, _ = session(played, 120, "20 12\n60 12K\n61 12\n", folder, identity_env=environment)
        data = (folder / "states" / "quick.sgstate").read_bytes()
        check(ev and ev[0]["status"] == "ok" and data[32:64] == effective and data[64:96] == bytes.fromhex(identity),
              "%s environment: the state records the program's own cartridge and package identity (%s %s)" % (label, data[32:36].hex(), data[64:68].hex()))
        digests.add(rep["canonical_sha256"])
    check(len(digests) == 1, "the canonical machine state (identity included) does not depend on the environment: %s" % digests)
    r, rep, ev, loaded = session(played, 70, "10 12M\n11 12\n", tmp / "identity-conflicting", identity_env=environments["agreeing"])
    check(ev and ev[0]["status"] == "ok" and rep["canonical_sha256"] in digests, "a state written under a conflicting environment loads under the right one: %s" % ev)
    foreign = tmp / "identity-foreign"
    (foreign / "states").mkdir(parents=True)
    forged = bytearray((save / "states" / "quick.sgstate").read_bytes())
    forged[32:64] = bytes.fromhex("22" * 32)   # a state that claims another cartridge, resealed
    forged[-32:] = hashlib.sha256(bytes(forged[:-32])).digest()
    (foreign / "states" / "quick.sgstate").write_bytes(bytes(forged))
    r, rep, ev, _ = session(played, 70, "10 M\n", foreign, identity_env={"SEGARECOMP_ROM_SHA256": "22" * 32})
    check(ev and ev[0]["status"] == "failed" and "different game or version" in ev[0]["detail"], "an environment cannot make the program accept another cartridge's state: %s" % ev)

    # ---- rewind across mod-state changes ----
    quiet = tmp / "rewind"
    r, cancel, ev, _ = session(played, 130, "20 12\n100 12Z\n101 12\n", quiet, "100")
    _, plain130, _, mod130 = session(played, 130, held)
    check(cancel is not None and cancel["canonical_sha256"] == plain130["canonical_sha256"], "cancelling a rewind changes nothing")
    r, back, ev, bm = session(played, 130, "20 12\n100 12Y\n101 12\n", quiet, "100")
    check(any(e["event"] == "rewind" and e["status"] == "ok" for e in ev) and back is not None and back["cycles"] < plain130["cycles"] and bm["faulted"] is False and bm["committed_writes"] > 0,
          "a rewind restores an earlier moment with its mod state and the game runs on: %s" % ev)
    _, back2, _, bm2 = session(played, 130, "20 12\n100 12Y\n101 12\n", quiet, "100")
    check(back2 == back and bm2["write_digest"] == bm["write_digest"], "the rewound future is deterministic, witness included")

    # ---- the two worlds refuse each other's states ----
    r, rep, ev, _ = session(vanilla, 70, "10 M\n", save)
    check(ev and ev[0]["status"] == "failed" and "different game or version" in ev[0]["detail"], "a Vanilla program refuses a modded state (the variant identity already differs): %s" % ev)
    vsave = tmp / "vsave"
    session(vanilla, 70, "60 K\n", vsave)
    r, rep, ev, _ = session(played, 70, "10 M\n", vsave)
    check(ev and ev[0]["status"] == "failed" and "different game or version" in ev[0]["detail"] and rep is not None, "a modded program refuses a Vanilla state and keeps running: %s" % ev)

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
