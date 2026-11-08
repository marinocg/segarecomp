#!/usr/bin/env python3
"""SEG-132 (ADR 0279) Checkpoints C/D: Master System save states and visual rewind inside a REAL generated-native viewer process (SDL3 dummy video and audio drivers).

usage: sms_viewer_states_test.py <segarecomp-cli> <cc> <product-root>

A project-authored counting guest (tools/sms_fixture_rom.py `state_counter`) is built through the production `segarecomp build` route with the SDL3 viewer. The scripted
input (SEGARECOMP_VIEWER_PAD_SCRIPT) presses the quick-save / quick-load / rewind actions at the frame barrier exactly as F5 / F8 / F7 do; Y and Z queue the real key events
the real surface loop consumes. The final guest state of every session is read from SEGARECOMP_VIEWER_STATE_REPORT. Proved:
  * quick save in one process and quick load in a SEPARATE process resume the identical future: the canonical machine state at the end equals the uninterrupted run's;
  * rewind: confirming goes back to an earlier moment and the new branch is deterministic; cancelling changes nothing at all (the final state equals the plain run's);
    rewind and cancelled browsing never write a file;
  * a state of another game (identity), of another build of the cartridge (mapper/ROM profile) and a corrupt file are refused with the guest running on unchanged;
  * a recording session offers no states.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else pathlib.Path(__file__).resolve().parent.parent
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import sms_fixture_rom as builder  # noqa: E402
import genesis_startup_bridge as bridge  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def build(cli, cc, tmp, name, mapper):
    roms = tmp / "roms"
    builder.main(["--out", str(roms), "--fixture", name])
    out = tmp / ("out-" + name)
    sdl = bridge.find_sdl3()
    include, lib = sdl[0][1], sdl[1][1]
    r = subprocess.run([cli, "build", "--rom", str(roms / (name + ".sms")), "--output", str(out), "--cc", cc, "--optimize", "1", "--runtime-dir", str(ROOT / "platforms" / "master-system"),
                        "--mapper", mapper, "--sdl3-include", include, "--sdl3-lib", lib], text=True, capture_output=True, timeout=1200)
    if r.returncode != 0:
        print(r.stdout[-1500:], r.stderr[-1500:])
        raise SystemExit("build failed: " + name)
    return out / "game"


def main():
    cli, cc = sys.argv[1], sys.argv[2]
    if bridge.find_sdl3() is None:
        print("SKIP: SDL3 not available")
        return 0
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms_viewer_states_"))
    exe = build(cli, cc, tmp, "state_counter", "rom_only")
    other = build(cli, cc, tmp, "machine_e2e", "sega")
    work = tmp / "work"
    work.mkdir()
    sha = "11" * 32

    def session(program, frames, script="", save=None, env_extra=None, args=()):
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_VIEWER_SETTINGS=str(work / "viewer-settings.cfg"), SEGARECOMP_ROM_SHA256=sha)
        for name in ("SEGARECOMP_SAVE_DIR", "SEGARECOMP_SAVE_VARIANT", "SEGARECOMP_VIEWER_PAD_SCRIPT", "SEGARECOMP_REWIND", "SEGARECOMP_REWIND_INTERVAL_MS"):
            env.pop(name, None)
        pad = work / "pad.txt"
        pad.write_text(script)
        report = work / "report.json"
        if report.exists():
            report.unlink()
        env["SEGARECOMP_VIEWER_PAD_SCRIPT"] = str(pad)
        env["SEGARECOMP_VIEWER_STATE_REPORT"] = str(report)
        if save is not None:
            env["SEGARECOMP_SAVE_DIR"] = str(save)
        env.update(env_extra or {})
        r = subprocess.run([str(program), "--viewer-unthrottled", "--viewer-mute", "--viewer-frames", str(frames), *args], text=True, capture_output=True, env=env, timeout=180)
        rep = json.loads(report.read_text()) if report.exists() else None
        events = [json.loads(l.split(" ", 1)[1]) for l in r.stderr.splitlines() if l.startswith("VIEWER_STATE ")]
        return r, rep, events

    # ---- calibration: the guest is deterministic and counts one per frame ------------------------------------------------------------------------------------------
    plain = {}
    for n in (60, 70, 120, 130):
        r, plain[n], _ = session(exe, n)
        check(r.returncode == 0 and plain[n] is not None, "plain run of %d frames ends cleanly (rc %d)" % (n, r.returncode))
    check(plain[60]["counter"] < plain[120]["counter"] < plain[130]["counter"], "the counter advances with guest time: %s" % [plain[n]["counter"] for n in (60, 120, 130)])
    again = session(exe, 120)[1]
    check(again == plain[120], "the plain run is deterministic (identical final canonical state)")

    # ---- save in one process, load in another: the identical future -----------------------------------------------------------------------------------------------
    save = tmp / "save"
    r, rep, ev = session(exe, 120, "60 K\n", save)
    check(rep == plain[120] and ev and ev[0]["event"] == "save" and ev[0]["status"] == "ok", "saving at frame 60 does not perturb the run: %s" % ev)
    check((save / "states" / "quick.sgstate").is_file(), "the quick slot file exists in the launcher-managed folder")
    head = (save / "states" / "quick.sgstate").read_bytes()[:160]
    tw, th, tbytes = int.from_bytes(head[144:146], "little"), int.from_bytes(head[146:148], "little"), int.from_bytes(head[148:152], "little")
    check(head[:8] == b"SMSSTAT\x00" and (tw, th) == (149, 111) and tbytes == 149 * 111 * 2, "the state carries a real thumbnail of the game's own 256x192 picture (%dx%d, %d bytes)" % (tw, th, tbytes))
    r, rep, ev = session(exe, 70, "10 M\n", save)
    check(ev and ev[0]["event"] == "load" and ev[0]["status"] == "ok", "a separate process loads the state: %s" % ev)
    check(rep is not None and rep["canonical_sha256"] == plain[120]["canonical_sha256"] and rep["counter"] == plain[120]["counter"],
          "load in a new process reproduces the uninterrupted run's final machine state exactly (counter %s vs %s)" % (rep and rep["counter"], plain[120]["counter"]))

    # ---- refusals: another game, another build of the cartridge, a corrupt file -----------------------------------------------------------------------------------------
    r, rep, ev = session(exe, 70, "10 M\n", save, {"SEGARECOMP_ROM_SHA256": "22" * 32})
    check(ev and ev[0]["status"] == "failed", "a state of another game is refused: %s" % ev)
    same_guest = lambda a, b: a is not None and {k: v for k, v in a.items() if k != "canonical_sha256"} == {k: v for k, v in b.items() if k != "canonical_sha256"}   # noqa: E731  (the digest carries the session's identity)
    check(same_guest(rep, plain[70]), "... and the running game continues exactly as if nothing had been tried")
    r, rep, ev = session(exe, 70, "10 M\n", save, {"SEGARECOMP_SAVE_VARIANT": "33" * 32})
    check(ev and ev[0]["status"] == "failed" and same_guest(rep, plain[70]), "a state of another version (variant identity) is refused: %s" % ev)
    r, rep, ev = session(other, 70, "10 M\n", save)
    check(ev and ev[0]["status"] == "failed" and "mapper" in ev[0]["detail"] + "build", "a state of a different cartridge profile under the same identity is refused: %s" % ev)
    damaged = tmp / "damaged"
    (damaged / "states").mkdir(parents=True)
    blob = bytearray((save / "states" / "quick.sgstate").read_bytes())
    blob[500] ^= 0x01
    (damaged / "states" / "quick.sgstate").write_bytes(bytes(blob))
    r, rep, ev = session(exe, 70, "10 M\n", damaged)
    check(ev and ev[0]["status"] == "failed" and rep is not None and rep["canonical_sha256"] == plain[70]["canonical_sha256"], "a corrupt state is refused and the game keeps running: %s" % ev)
    check(bytes(blob) == (damaged / "states" / "quick.sgstate").read_bytes(), "a refused load never rewrites the file")
    # a recording session offers no states and writes nothing
    rec = tmp / "rec"
    r, rep, ev = session(exe, 70, "10 K\n", rec, args=("--viewer-record", str(work / "rec.txt")))
    check(not ev and not (rec / "states").exists() and rep is not None, "a recording session offers no save states and writes none: %s" % ev)

    # ---- rewind ----------------------------------------------------------------------------------------------------------------------------------------------------------
    env = {"SEGARECOMP_REWIND_INTERVAL_MS": "100"}
    norewind = tmp / "norewind"
    r, cancel, ev = session(exe, 130, "100 Z\n", norewind, env)
    check(cancel is not None and cancel["canonical_sha256"] == plain[130]["canonical_sha256"] and not [e for e in ev if e["event"] == "rewind"],
          "browsing three moments back and cancelling changes nothing: the final machine state equals the plain run's")
    r, back, ev = session(exe, 130, "100 Y\n", norewind, env)
    check(len([e for e in ev if e["event"] == "rewind" and e["status"] == "ok"]) == 1, "confirming restores one moment: %s" % ev)
    check(back is not None and plain[60]["counter"] < back["counter"] < plain[130]["counter"] - 5, "the guest resumed from an EARLIER moment and kept running (counter %s vs %s)" % (back and back["counter"], plain[130]["counter"]))
    r, back2, ev = session(exe, 130, "100 Y\n", norewind, env)
    check(back2 == back, "the rewound future is deterministic (an identical second process)")
    r, twice, ev = session(exe, 130, "100 Y\n101 Y\n", norewind, env)
    check(twice is not None and back is not None and twice["counter"] < back["counter"], "rewinding twice goes back further (the new branch has its own history)")
    check(not (norewind / "states").exists(), "rewind and cancelled browsing wrote nothing to disk")
    stats = [l for l in r.stderr.splitlines() if l.startswith("VIEWER_REWIND_STATS ")]
    st = json.loads(stats[0].split(" ", 1)[1]) if stats else {}
    check(st.get("captures", 0) >= 10 and st.get("restores") == 2 and st.get("last_capture_bytes", 0) > 100000, "rewind statistics are reported: %s" % st)
    # shipped defaults (nothing overridden): 500 ms moments, so a rewind two seconds in has real history
    r, d, ev = session(exe, 170, "150 Y\n", norewind)
    check(d is not None and any(e["event"] == "rewind" and e["status"] == "ok" for e in ev), "default policy: a rewind at frame 150 restores a moment: %s" % ev)
    # rewind switched off
    r, off, ev = session(exe, 130, "100 Y\n", norewind, {"SEGARECOMP_REWIND": "0"})
    check(off is not None and off["canonical_sha256"] == plain[130]["canonical_sha256"], "with rewind off the rewind action changes nothing")

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
