#!/usr/bin/env python3
"""SEG-149-T001 (delivery F29): the INTEGRATED synthetic acceptance of one schema 3 / native ABI 2 mod. The original fixture (platforms/genesis/sdk/examples/widescreen-native/fixture_rom.py, no vendor code) scrolls a
tilemap, moves an actor across the right edge of the picture and keeps a power value. The ONE example package authors the offscreen scenery, draws the actor as a virtual sprite in the widescreen margin, shows a HUD and
restores the power value when C is pressed. The whole chain runs on the unmodified product:

  mod check -> mod pack -> launcher install (exact-ROM check, native approval) -> select -> build (reverified) -> the launcher-built program PLAYS (SDL3 viewer, dummy drivers)
  and the SAME package built with the frame-capture seam is played at 4:3, 16:9, 21:9 and 32:9 against INDEPENDENT golden constructions; save / restore / rewind / replay; Vanilla is unchanged;
  fail-closed negatives (wrong image, invalid geometry declaration, a scene the declaration does not list).

The launcher-built program is the product build without the test-only frame seam, so the frame comparison uses the same package built by the same CLI with `--test-frame-seam`.
usage: mod_widescreen_example_test.py <segarecomp-cli> <launcher_schema3_flow_test> <C compiler> <source root> <SDL3 prefix>"""
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import H, W, rgb, side_of, surface_from_bmp  # noqa: E402
from mod_scenery_abi2_test import golden  # noqa: E402

PAD = "0 RB\n2 R\n10 RC\n11 R\n20 -\n"   # Right for 20 turns, B ("damage") for the first turns (power 5 -> somewhere in 1..4), C pressed at turn 10 (the mod restores power to 5)
HUD_BOX = (24, 10)               # surface pixels (x < 24, y < 10): the HUD panel and its counter bar are the one drawn thing over the golden
SPRITE_COLOUR = rgb(0x0E0E)
GOLD = (255, 208, 64)


def words(ram):
    return {"score": int.from_bytes(ram[0x10:0x12], "big"), "camera": int.from_bytes(ram[0x12:0x14], "big"), "actor": int.from_bytes(ram[0x14:0x16], "big"), "power": int.from_bytes(ram[0x16:0x18], "big")}


def main():
    cli, flow, compiler, root, sdl = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve(), pathlib.Path(sys.argv[5]).resolve()
    if sys.platform != "linux":
        print("mod_widescreen_example_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    example = root / "platforms/genesis/sdk/examples/widescreen-native"
    spec = importlib.util.spec_from_file_location("fixture_rom", example / "fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    keep = os.environ.get("SEGARECOMP_TEST_KEEP")
    with tempfile.TemporaryDirectory(prefix="segarecomp-ws-example-") as directory:
        tmp = pathlib.Path(keep or directory)
        tmp.mkdir(exist_ok=True)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "fixture.rom"
        rom.write_bytes(image)
        image_sha = sha(image)
        other = tmp / "other.rom"
        other.write_bytes(image + b"\x60\xfe")

        def project(name, image_sha=image_sha, edit=None, toml=None):
            d = tmp / (name + "-src")
            shutil.copytree(example, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
            (d / "mod.toml").write_text((toml((d / "mod.toml").read_text()) if toml else (d / "mod.toml").read_text()).replace(sha(image), image_sha))
            if edit:
                text = (d / "src/provider.c").read_text()
                new = edit(text)
                check(new != text, f"{name}: the edit changed the source")
                (d / "src/provider.c").write_text(new)
            return d

        def pack(d):
            out = tmp / (d.name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {d.name}: {r.stdout[:300]}")
            return out, doc.get("package_identity")

        # ---- 1. mod check -> pack ----
        d = project("example")
        r = h.run_cli("mod", "check", d, "--cc", compiler, "--sdk-dir", root / "platforms/genesis")
        check(r.returncode == 0 and json.loads(r.stdout).get("ok") and json.loads(r.stdout)["trust_class"] == "native", f"mod check passes: {r.stdout[:300]}")
        pkg, ident = pack(d)
        wrong, _ = pack(project("wrong", image_sha=sha(other.read_bytes())))

        # ---- 2. the launcher flow (install, exact-ROM check, approval, select, build, reverify, uninstall) on the real build driver; its programs play ----
        install = tmp / "install"
        (install / "bin").mkdir(parents=True)
        (install / "toolchain").mkdir()
        (install / "sdl3").mkdir()
        os.symlink(pathlib.Path(cli).resolve(), install / "bin" / "segarecomp")
        os.symlink(pathlib.Path(cli).resolve().parent / "segarecomp-luau-compile", install / "bin" / "segarecomp-luau-compile")
        os.symlink(root, install / "runtime")
        os.symlink(sdl / "include", install / "sdl3" / "include")
        os.symlink(sdl / "lib", install / "sdl3" / "lib")
        zig = install / "toolchain" / "zig"
        zig.write_text(f'#!/bin/sh\ndriver="$1"\nshift 3\ncase "$driver" in c++) exec "{shutil.which("c++") or "c++"}" "$@" ;; *) exec "{compiler}" "$@" ;; esac\n')
        zig.chmod(0o755)
        report = tmp / "flow.report"
        env = h.env(SEGARECOMP_S3_REPORT=str(report))
        r = subprocess.run([flow, str(install), str(rom), str(other), str(pkg), str(wrong)], text=True, capture_output=True, env=env, timeout=1500)
        check(r.returncode == 0 and "launcher_schema3_flow_test: ok" in r.stdout, f"the launcher flow passes (install, approval, select, build, tamper/identity refusals, uninstall): {r.stdout[-300:]}{r.stderr[-800:]}")
        if h.failures:
            return 1
        built = dict(line.split("=", 1) for line in report.read_text().splitlines() if "=" in line)
        check(built.get("identity") == ident, "the launcher built exactly the packed package")
        for label in ("vanilla", "mod"):
            program = pathlib.Path(built[label]).parent
            pad = tmp / (label + ".launcher.pad")
            pad.write_text(PAD)
            run = subprocess.run([str(program / "game"), "--instruction-budget", "4000000"], text=True, capture_output=True, timeout=240, cwd=tmp,
                                 env=h.env(SEGARECOMP_VIEWER_PAD_SCRIPT=str(pad), SEGARECOMP_VIEWER_SETTINGS=str(tmp / "launcher-settings.cfg")))
            ended = "guest_stop" in run.stderr
            check(ended and (("MOD_LOADED" in run.stderr and '"faulted":false' in run.stderr) if label == "mod" else "MOD_LOADED" not in run.stderr),
                  f"the launcher-built {label} program plays to its own guest stop ({'with the native mod loaded and healthy' if label == 'mod' else 'with no mod'}): {run.stderr[-300:]}")

        # ---- 3. the same package, built with the frame seam, at four aspects ----
        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        r = h.build(rom, tmp / "modded", pkg, ident, native=True)
        check(r.returncode == 0, f"the mod builds: {r.stdout[-300:]}{r.stderr[-300:]}")
        if h.failures:
            return 1

        def play(program, name, ratio, pad=PAD, save=None, extra=None, identity=None):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(f"video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio={ratio}\n")
            rep = tmp / (name + ".coverage.json")
            rep.unlink(missing_ok=True)
            e = {"SEGARECOMP_COVERAGE_REPORT": str(rep), "SEGARECOMP_VIEWER_SETTINGS": str(cfg)}
            e.update(extra or {})
            run, lines, frame, ram = h.play(program, name, pad, save=save, identity=identity, rom_sha=image_sha, extra=e)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: runs to its own guest stop: {run.stderr[-200:]}")
            side = side_of(*map(int, ratio.split(":")))
            return surface_from_bmp(read_bmp(frame), side), ram.read_bytes(), lines, (json.loads(rep.read_text()) if rep.is_file() else None)

        results = {}
        for ratio in ("4:3", "16:9", "21:9", "32:9"):
            side = side_of(*map(int, ratio.split(":")))
            tag = ratio.replace(":", "x")
            base, ram_v, lines_v, rep_v = play(tmp / "vanilla", "v-" + tag, ratio)
            surf, ram, lines, rep = play(tmp / "modded", "m-" + tag, ratio)
            wv, wm = words(ram_v), words(ram)
            check(1 <= wv["power"] <= 4 and wm["power"] == 5 and wv["camera"] == wm["camera"] and wv["actor"] == wm["actor"] and wv["score"] == wm["score"] and "MOD_SUMMARY" not in lines_v,
                  f"{ratio}: the gameplay effect: Vanilla ends with power {wv['power']}, the mod restores it to {wm['power']}; nothing else in the guest differs; Vanilla runs no mod")
            ms = lines["MOD_SUMMARY"][0]
            check(ms["faulted"] is False and ms["committed_writes"] == 1, f"{ratio}: no fault and exactly ONE guest write (the restore): {ms}")
            ax, cam = wm["actor"] - 8 * wm["camera"], wm["camera"]
            want = golden(side, cam) if side else [[None] * W for _ in range(H)]
            sprite_visible = 0 <= ax and ax + 8 <= W + side and ax >= W
            wrong_px = 0
            for y in range(H):
                for xi in range(W + 2 * side):
                    if xi < HUD_BOX[0] and y < HUD_BOX[1]:
                        continue
                    x = xi - side   # picture coordinate
                    expected = want[y][xi]
                    if side and expected is not None:
                        pass
                    elif 0 <= x < W:
                        expected = base[y][xi]   # the native picture is Vanilla's
                    if sprite_visible and ax <= x < ax + 8 and 150 <= y < 158:
                        expected = SPRITE_COLOUR
                    if surf[y][xi] != expected:
                        wrong_px += 1
            check(wrong_px == 0, f"{ratio}: the presented surface equals the independent construction (authored scenery, the actor sprite where it is presented, Vanilla's picture) ({wrong_px} pixels differ)")
            scenes = rep["scenes"][0]["components"] if rep and rep.get("scenes") else {}
            if side:
                check(scenes.get("scenery", {}).get("covered", 0) > 0 and scenes["scenery"]["covered"] <= scenes["scenery"]["requested"] and scenes.get("hud", {}).get("covered", 0) > 0, f"{ratio}: scenery and hud coverage are credited for presented frames: {scenes}")
            results[ratio] = (surf, scenes, rep)
        check(results["32:9"][1].get("actors", {}).get("covered", 0) > 0, "32:9: the actor is credited where it was presented in the margin")
        right = lambda ratio: (results[ratio][2] or {}).get("virtual_sprites", {}).get("pixels_right", 0)   # noqa: E731
        check(right("32:9") > right("21:9") >= right("16:9") >= right("4:3") == 0, f"the actor reaches the margin only where the aspect presents it ({[right(r) for r in ('4:3', '16:9', '21:9', '32:9')]})")

        # ---- 4. save / restore / rewind / replay (32:9) ----
        base, ram_v, _, _ = play(tmp / "vanilla", "n-vanilla", "32:9")
        first = play(tmp / "modded", "n-first", "32:9")
        again = play(tmp / "modded", "n-replay", "32:9")
        check(first[0] == again[0] and first[1] == again[1] and first[3] == again[3], "a replay reproduces the surface, the guest state and the accounting")
        save = tmp / "save"
        _, _, ls, _ = play(tmp / "modded", "n-save", "32:9", "0 RB\n2 R\n10 RC\n11 R\n14 RK\n15 R\n20 -\n", save=save, identity=ident)
        check(ls["VIEWER_STATE"][0]["status"] == "ok", "a quick save succeeds with the mod, its sprite and its scenery in flight")
        surf, ram, ll, rep = play(tmp / "modded", "n-load", "32:9", "0 -\n2 M\n3 -\n", save=save, identity=ident)
        check(ll["VIEWER_STATE"][0]["status"] == "ok" and words(ram)["power"] == 5 and ll["MOD_SUMMARY"][0]["faulted"] is False and surf[8][0] == GOLD,
              f"a NEW process restores the state: the guest power (5) and the mod's own counter (its HUD bar) both come back: {words(ram)}")
        surf, ram, lr, rep = play(tmp / "modded", "n-rewind", "32:9", "0 RB\n2 R\n10 RC\n11 R\n14 RY\n15 R\n20 -\n", save=tmp / "rewind", identity=ident)
        check([(e["event"], e["status"]) for e in lr.get("VIEWER_STATE", [])] == [("rewind", "ok")] and lr["MOD_SUMMARY"][0]["faulted"] is False, f"a scripted rewind is accepted and the mod stays healthy: {lr.get('VIEWER_STATE')}")

        # ---- 5. fail-closed negatives ----
        bad = project("bad-geometry", toml=lambda t: t.replace('aspect_max = "32:9"', 'aspect_max = "64:9"'))
        r = h.run_cli("mod", "check", bad, "--cc", compiler, "--sdk-dir", root / "platforms/genesis")
        check(r.returncode != 0, "an invalid geometry declaration is rejected by `mod check`")
        pkg_u, id_u = pack(project("undeclared-scene", edit=lambda t: t.replace('render->scene("main", 4U)', 'render->scene("other", 5U)')))
        r = h.build(rom, tmp / "undeclared", pkg_u, id_u, native=True)
        check(r.returncode == 0, "the undeclared-scene package builds")
        surf, ram, lines, rep = play(tmp / "undeclared", "u-32x9", "32:9")
        scn = next((x for x in rep["scenes"] if x["id"] == "*"), {"components": {"scenery": {"covered": -1, "undeclared": 0}}})
        check(scn["components"]["scenery"]["covered"] == 0 and scn["components"]["scenery"]["undeclared"] > 0 and all(x["components"]["scenery"]["covered"] == 0 for x in rep["scenes"]), f"a scene the declaration does not list is accounted under the undeclared scene (`*`), never as covered: {scn}")
        r = h.build(rom, tmp / "refused", pkg, "00" * 32, native=True)
        check(r.returncode != 0 and not (tmp / "refused/game").exists(), "an identity that was not approved never builds")
        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
    print("mod_widescreen_example_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
