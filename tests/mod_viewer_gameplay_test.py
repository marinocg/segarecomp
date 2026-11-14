#!/usr/bin/env python3
"""SEG-138-T010 (ADR 0305): the scheduled Luau gameplay mod in the PLAYED program. Builds generated-native programs of the FABRICATED gameplay fixture (platforms/genesis/sdk/examples/gameplay; no
commercial image) with the UNMODIFIED product: Vanilla and Modded (the gameplay package) SDL3 viewer programs plus the headless reference, runs them under the real SDL3 viewer (dummy video/audio,
explicit --instruction-budget, wall-clock timeouts, the scripted pad) and proves: (1) the guest-RAM effect while C is held equals an independent derivation AND the headless reference; (2) the mod's HUD
reaches the presented frame (pinned crop) and the rest of the picture is the Vanilla frame; (3) the run is deterministic; (4) a state saved mid-run (with the mod's own counter) and loaded into a NEW
process ends in the identical guest state, replay witness and HUD; rewind keeps the effect; a foreign state or a Vanilla state is refused; (5) a mod that writes outside its declared range faults only
itself; (6) packages outside the scheduled support set are refused at build with their class; (7) a Vanilla viewer program contains no scheduler.
usage: mod_viewer_gameplay_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402

FIXTURE_SHA = "fc7126ee2497cfbc7e1e3337c230a967d21c4c98e44493a4dfd01ae3ff6a5a34"
PINNED_IDENTITY = "09a6f3da161f156d4431b50137f8ac5eaea3b32b5bff8e671fdf722bdbb8ed1e"
PINNED_WRITE_DIGEST = "194aba886c6e5d5006f0167d766991397ba0996e7767dd9bdb9e201a8e10336d"   # replay witness of 91 committed bonus writes (C held for the whole program)


def derived_state(score, position):
    """The final work RAM, derived independently: all zero except the two words the fixture program owns."""
    ram = bytearray(65536)
    ram[0x10:0x14] = score.to_bytes(2, "big") + position.to_bytes(2, "big")
    return bytes(ram)


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_viewer_gameplay_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    example = root / "platforms/genesis/sdk/examples/gameplay"
    spec = importlib.util.spec_from_file_location("fixture_rom", example / "fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="segarecomp-gameplay-viewer-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        rom = tmp / "fixture.rom"
        rom.write_bytes(fixture.make_rom())
        check(sha(rom.read_bytes()) == FIXTURE_SHA, "the fixture image is the pinned one")

        def project(name, script=None, toml=None):
            d = tmp / name
            shutil.copytree(example, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
            if script is not None:
                (d / "scripts/main.luau").write_text(script)
            if toml is not None:
                (d / "mod.toml").write_text(toml(( d / "mod.toml").read_text()))
            return d

        def pack(d):
            out = tmp / (d.name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc["ok"], f"pack {d.name}: {r.stdout[:300]}")
            return out, doc["package_identity"]

        pkg, ident = pack(project("gameplay"))
        check(ident == PINNED_IDENTITY, f"the example package keeps its pinned identity: {ident}")

        # ---- the programs: Vanilla viewer, Modded viewer, headless modded reference ----
        r = h.build(rom, tmp / "vanilla")
        check(r.returncode == 0, "Vanilla viewer program builds: " + r.stdout[-300:] + r.stderr[-300:])
        r = h.build(rom, tmp / "modded", pkg, ident)
        check(r.returncode == 0, "the gameplay package builds into the SDL3 viewer program (scheduled path): " + r.stdout[-400:] + r.stderr[-400:])
        r = h.build(rom, tmp / "headless", pkg, ident, viewer=False)
        check(r.returncode == 0, "and into the headless reference program: " + r.stdout[-300:] + r.stderr[-300:])
        if h.failures:
            return 1
        st = h.status(tmp / "modded")["luau"]
        check(st["package_identity"] == ident and st["effective_sha256"] == FIXTURE_SHA and "memory.write.m68k.work_ram" in st["capabilities"], f"status.json records the verified package and its capabilities: {st}")
        vbytes = (tmp / "vanilla/game").read_bytes()
        check(b"luau-scheduled-host" not in vbytes and b"E_MOD_FAULT" not in vbytes and b"lua_newstate" not in vbytes, "a Vanilla viewer program contains no scheduler, no mod adapter and no Luau VM")
        check(b"luau-scheduled-host" in (tmp / "modded/game").read_bytes(), "the modded viewer program links the scheduled-mod presenter")

        HOLD_C = "0 C\n"
        # ---- the played gameplay effect ----
        pv, lv, fv, rv = h.play(tmp / "vanilla", "vanilla", HOLD_C)
        pm, lm, fm, rm = h.play(tmp / "modded", "modded", HOLD_C)
        check(lv["VIEWER_SUMMARY"][0]["outcome"] == "guest_stop" and lm["VIEWER_SUMMARY"][0]["outcome"] == "guest_stop" and lm["RUNTIME_NOTES"][0]["virtual_frames"] == lv["RUNTIME_NOTES"][0]["virtual_frames"],
              "both programs run to the program's own guest stop in the same number of frames: " + pm.stderr[-300:])
        check("MOD_SUMMARY" not in lv and "VIEWER_NATIVE_SUMMARY" not in lv, "the Vanilla program reports no mod and attaches no provider")
        check(rv.read_bytes() == derived_state(0, 0), "Vanilla: no button B or Right held, so both words stay zero")
        check(rm.read_bytes() == derived_state(900, 0), "Modded: C held for the 90 turns adds 10 per turn to the score (independent derivation 900); the position is untouched")
        ms = lm["MOD_SUMMARY"][0]
        check(ms["faulted"] is False and ms["nondeterministic"] is False and ms["committed_writes"] == 91 and ms["write_digest"] == PINNED_WRITE_DIGEST and ms["overlay_ops"] == 4, f"the mod ran under the scheduler without a fault: {ms}")
        loaded = lm["MOD_LOADED"][0]
        check(loaded["package_identity"] == ident and loaded["ranges"] == 1 and lm["MOD_LOG"][0]["line"] == "gameplay example ready", f"the program loaded the verified package and its declared range: {loaded}")
        # the headless reference of the SAME package and rule: pad held from the first frame
        env = h.env(SEGARECOMP_CAPTURE_DIR=str(tmp / "cap"), SEGARECOMP_CAPTURE_FIRST="200", SEGARECOMP_CAPTURE_COUNT="1", SEGARECOMP_CAPTURE_SLICE="16", SEGARECOMP_CAPTURE_RAM="16:4", SEGARECOMP_PAD_SCRIPT="0:1:0x20")
        (tmp / "cap").mkdir()
        ph = subprocess.run([str(tmp / "headless/game"), "--instruction-budget", "4000000"], text=True, capture_output=True, env=env, timeout=120)
        cap = next(json.loads(line.partition(" ")[2]) for line in ph.stderr.splitlines() if line.startswith("CAPTURE_SUMMARY "))
        hs = next(json.loads(line.partition(" ")[2]) for line in ph.stderr.splitlines() if line.startswith("MOD_SUMMARY "))
        check(cap["final_work_ram_sha256"] == sha(rm.read_bytes()), "the played program's guest RAM equals the headless reference's final work RAM")
        check(hs["write_digest"] == ms["write_digest"] and hs["committed_writes"] == ms["committed_writes"], "and the replay witness of its committed writes is the headless one")

        # ---- the HUD reaches the presented frame ----
        v, m = read_bmp(fv), read_bmp(fm)
        diff = [(x, y) for y in range(672) for x in range(960) if m[y][x] != v[y][x]]
        check(len(diff) > 2000, f"the modded frame differs from Vanilla's ({len(diff)} pixels)")
        if diff:
            xs, ys = [p[0] for p in diff], [p[1] for p in diff]
            check(max(xs) < 3 * 76 and max(ys) < 3 * 28, f"the frames differ only inside the HUD region (bbox {min(xs)},{min(ys)}-{max(xs)},{max(ys)})")
        check(all(m[y][x] == v[y][x] for y in range(300, 672) for x in range(0, 960, 7)), "the rest of the picture is the Vanilla frame")
        check(m[3 * 25 + 1][3 * 2 + 1] == (0xFF, 0xD0, 0x40), f"the opaque gold bar pixel is the script's colour: {m[3 * 25 + 1][3 * 2 + 1]}")
        px, py = 3 * 70 + 1, 3 * 1 + 1   # panel only (no text reaches overlay x = 70): the translucent 0x000000C0 panel over the Vanilla pixel
        for channel in range(3):
            want = (v[py][px][channel] * 63 + 127) // 255
            check(abs(m[py][px][channel] - want) <= 2, f"the panel pixel is the documented blend of the Vanilla pixel: channel {channel} {m[py][px][channel]} vs {want}")
        check(any(m[y][x] == (255, 255, 255) for y in range(3 * 3, 3 * 11) for x in range(3 * 4, 3 * 40)), "white title text pixels are present")
        crop = sha(b"".join(bytes(p) for row in m[:90] for p in row[:240]))
        golden = json.loads((root / "tests/fixtures/mod_viewer/gameplay_hud.json").read_text()) if (root / "tests/fixtures/mod_viewer/gameplay_hud.json").exists() else {}
        check(crop == golden.get("hud_crop_sha256"), f"the rendered HUD crop is the pinned one: {crop}")

        # ---- deterministic ----
        _, lm2, fm2, rm2 = h.play(tmp / "modded", "modded2", HOLD_C)
        check(fm2.read_bytes() == fm.read_bytes() and rm2.read_bytes() == rm.read_bytes() and lm2["MOD_SUMMARY"] == lm["MOD_SUMMARY"], "a second run reproduces the frame, the guest RAM and the mod summary byte for byte")
        _, ln, _, rn = h.play(tmp / "modded", "modded-nc", "0 C\n6 -\n")
        check(rn.read_bytes() != rm.read_bytes() and ln["MOD_SUMMARY"][0]["write_digest"] != ms["write_digest"], "a different input gives a different guest state and witness")

        # ---- save / restore / rewind ----
        save = tmp / "save"
        _, ls, fs, rs = h.play(tmp / "modded", "save", "0 C\n8 CK\n9 C\n", save=save, identity=ident, rom_sha=FIXTURE_SHA)
        check(ls["VIEWER_STATE"][0]["status"] == "ok" and ls["VIEWER_STATE"][0]["event"] == "save", f"a quick save succeeds with the mod attached: {ls.get('VIEWER_STATE')}")
        check(rs.read_bytes() == rm.read_bytes() and ls["MOD_SUMMARY"][0]["write_digest"] == PINNED_WRITE_DIGEST, "saving changes nothing the run can see")
        _, ll, fl, rl = h.play(tmp / "modded", "load", "0 C\n2 CM\n3 C\n", save=save, identity=ident, rom_sha=FIXTURE_SHA)
        check(ll["VIEWER_STATE"][0]["event"] == "load" and ll["VIEWER_STATE"][0]["status"] == "ok", f"a NEW process loads the state: {ll.get('VIEWER_STATE')}")
        check(rl.read_bytes() == rm.read_bytes() and ll["MOD_SUMMARY"][0]["write_digest"] == PINNED_WRITE_DIGEST, "the restored session ends in the identical guest state and replay witness (the mod's counter was restored)")
        ml = read_bmp(fl)
        check(all(ml[y][x] == m[y][x] for y in range(3, 90) for x in range(0, 228)), "and the HUD, which shows the restored counter, is the straight run's HUD")
        _, lrw, frw, rrw = h.play(tmp / "modded", "rewind", "0 C\n14 CY\n15 C\n", save=tmp / "save-rewind", identity=ident, rom_sha=FIXTURE_SHA)
        check([(e["event"], e["status"]) for e in lrw.get("VIEWER_STATE", [])] == [("rewind", "ok")], f"a scripted rewind with the mod attached restores a history entry: {lrw.get('VIEWER_STATE')}")
        check(rrw.read_bytes() == rm.read_bytes() and lrw["MOD_SUMMARY"][0]["write_digest"] == PINNED_WRITE_DIGEST, "after a rewind with the mod attached the session still ends in the straight run's guest state and replay witness (the mod's state was part of the entry)")
        _, lf, _, rf = h.play(tmp / "modded", "foreign", "0 C\n2 CM\n3 C\n", save=save, identity="11" * 32, rom_sha=FIXTURE_SHA)
        check(lf["VIEWER_STATE"][0]["status"] == "failed" and lf["VIEWER_STATE"][0]["detail"] == "the state belongs to a different game or version" and rf.read_bytes() == rm.read_bytes(), f"a state of another package identity is refused with the stable diagnostic: {lf.get('VIEWER_STATE')}")
        _, lv2, _, _ = h.play(tmp / "vanilla", "vanilla-load", "0 C\n2 CM\n3 C\n", save=save, identity=ident, rom_sha=FIXTURE_SHA)
        check(lv2["VIEWER_STATE"][0]["status"] == "failed", f"a Vanilla program refuses a state that carries a mod: {lv2.get('VIEWER_STATE')}")

        # ---- unauthorized access fails closed: only the mod is disabled ----
        rogue = project("rogue", script='function post_input(frame: number)\n  sega.mem.write("m68k.work_ram", 0x20, 2, 99)\nend\nfunction save() return {} end\nfunction load(s) end\n')
        rpkg, rid = pack(rogue)
        r = h.build(rom, tmp / "rogue", rpkg, rid)
        check(r.returncode == 0, "the rogue package builds (the range violation is a run-time fault)")
        _, lr, fr, rr = h.play(tmp / "rogue", "rogue", HOLD_C)
        check(lr["MOD_SUMMARY"][0]["faulted"] is True and "cause=range" in lr["MOD_SUMMARY"][0]["fault"] and lr["VIEWER_SUMMARY"][0]["outcome"] == "guest_stop" and rr.read_bytes() == rv.read_bytes(),
              f"the out-of-range write faults the mod; the guest ends in the Vanilla state: {lr['MOD_SUMMARY'][0]['fault']}")
        check(fr.read_bytes() == fv.read_bytes(), "a faulted mod presents nothing: the frame is the Vanilla frame")

        # ---- everything outside the scheduled support set is still refused at build ----
        refusals = (("frame-work-ram", lambda t: t.replace('"memory.read.m68k.work_ram"', '"memory.read.m68k.work_ram", "frame.work_ram"'), "E_LIVE_CAPABILITY"),
                    ("hooks-capability", lambda t: t.replace('"memory.read.m68k.work_ram"', '"memory.read.m68k.work_ram", "hook.function"'), None))
        for name, edit, code in refusals:
            d = project(name, toml=edit)
            r = h.run_cli("mod", "pack", d, "--out", tmp / (name + ".segapatch"))
            if r.returncode != 0:
                check(code is None, f"{name}: pack refused ({r.stdout[:200]})")   # a package that cannot even be packed is closed before any build
                continue
            doc = json.loads(r.stdout)
            r = h.build(rom, tmp / name, tmp / (name + ".segapatch"), doc["package_identity"])
            diag = h.status(tmp / name).get("diagnostic") if (tmp / name / "status.json").exists() else None
            check(r.returncode != 0 and not (tmp / name / "game").exists() and (code is None or diag == code), f"{name}: refused at build with {code}: {diag} {(r.stdout + r.stderr)[-200:]}")

        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_viewer_gameplay_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
