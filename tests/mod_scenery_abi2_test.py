#!/usr/bin/env python3
"""SEG-140-T011 (ADR 0309): authored offscreen scenery from a NATIVE ABI 2 provider, through the real SDL3 viewer of a generated-native program, on a FABRICATED scrolling fixture (no commercial image).

The provider (tests/fixtures/mod_scenery/provider.c) submits one name-table window per frame through SegaGenesisRenderApi::scenery_window. The golden is constructed here from the fixture's contract (plane A scrolls by
-(camera * 8); cells outside the picture alternate tile 2 / tile 3 of palette line 1) and never from the program's output. Proves: (A) the left and right margins at 16:9, 21:9 and 32:9 equal the golden while the native
picture equals Vanilla's; (B) the coverage report credits `scenery` and `hud` for presented frames and `actors` never; (C) the Vanilla program (and the same mod with the callback disconnected) presents the generic wrapped
scenery, never the authored one; (D) containment: an ungranted capability, an oversized window, a third window on one plane (two are allowed, ADR 0313), a bad plane and a NULL words pointer fault only the mod and present nothing; (E) a stale window is never
carried to another frame (a provider that submits only on the first frame leaves the later frames generic).
usage: mod_scenery_abi2_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import os
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import H, W, rgb, side_of, surface_from_bmp  # noqa: E402

CAMERA_FINAL = 20
PAD = "0 R\n20 -\n"   # Right held for 20 turns, then released: the last 19 presented frames share camera 20 (plane A scroll -160)


def golden(side, camera, alternating=True):
    """The expected surface margins: columns beyond the picture show the authored level cells; the picture itself is the fixture's own plane (alternating green / red columns)."""
    green, red, blue, cyan = rgb(0x00E0), rgb(0x000E), rgb(0x0E00), rgb(0x0EE0)
    rows = []
    for y in range(H):
        row = []
        for x in range(-side, W + side):
            if 0 <= x < W:
                row.append(None)
                continue
            cell = (x + camera * 8) // 8   # plane x = screen x - hscroll with hscroll = -(camera * 8)
            row.append((green if cell % 2 == 0 or not alternating else red) if 24 <= cell < 64 else (blue if cell % 2 == 0 else cyan))
        rows.append(row)
    return rows


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_scenery_abi2_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    spec = importlib.util.spec_from_file_location("scenery_fixture", fixtures / "scenery_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    keep = os.environ.get("SEGARECOMP_TEST_KEEP")   # debugging aid: reuse a directory that survives the run
    with tempfile.TemporaryDirectory(prefix="segarecomp-scenery-") as directory:
        tmp = pathlib.Path(keep or directory)
        tmp.mkdir(exist_ok=True)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "scenery.rom"
        rom.write_bytes(image)
        image_sha = sha(image)

        identities = {}

        def project(name, edit=None, toml=None):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            source = (fixtures / "provider.c").read_text()
            new = edit(source) if edit else source
            if edit:
                check(new != source, f"{name}: the edit changed the source")
            (d / "src/provider.c").write_text(new)
            manifest = (fixtures / "mod.toml").read_text().replace("@@SHA@@", image_sha)
            (d / "mod.toml").write_text(toml(manifest) if toml else manifest)
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            built = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-400:]}{built.stderr[-300:]}")
            identities[name] = doc.get("package_identity")
            return tmp / name

        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        modded = project("modded")
        if h.failures:
            return 1
        both = "    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n    /* plane B does not scroll in the fixture: its cells are asked for at their own, camera-independent coordinates */\n    (void)render->scenery_window(1U, -40, 0, COLS, ROWS, blank);\n"
        variants = {   # name -> (provider edit, manifest edit, the stable fault cause expected or None)
            "disconnected": (lambda t: t.replace(both, ""), None, None),
            "narrow-window": (lambda t: t.replace("scenery_window(0U, x0, 0, COLS, ROWS, words)", "scenery_window(0U, x0, 0, COLS, ROWS / 2U, words)"), None, None),   # half of plane A's rows have no window: unanswered cells
            "stale": (lambda t: t.replace("static uint32_t camera;", "static uint32_t camera, submitted;").replace(both, "    if (submitted == 0U) {\n" + both + "      submitted = 1U;\n    }\n"), None, None),
            "ungranted": (None, lambda m: m.replace('"overlay.rgba", "scenery.words"', '"overlay.rgba"'), "capability"),
            "oversized": (lambda t: t.replace("scenery_window(0U, x0, 0, COLS, ROWS, words)", "scenery_window(0U, x0, 0, 129U, ROWS, words)"), None, "range"),
            "third-window": (lambda t: t.replace(both, both + "    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n"), None, "range"),   # ADR 0313: two windows per plane are allowed, a third is a range fault
            "bad-plane": (lambda t: t.replace("scenery_window(1U,", "scenery_window(2U,"), None, "range"),
            "null-words": (lambda t: t.replace("ROWS, blank)", "ROWS, 0)"), None, "range"),
        }
        programs = {name: project("v-" + name.replace("_", "-"), edit, toml) for name, (edit, toml, _) in variants.items()}
        # ADR 0314: plane B submits no window at all (the fixture's plane B is empty, so the VDP-derived extension is exactly what the provider would have sent): plane A is authored, plane B is generic, nothing fails
        plane_b_generic = project("v-plane-b-generic", lambda t: t.replace("    (void)render->scenery_window(1U, -40, 0, COLS, ROWS, blank);\n", ""))
        split_windows = project("v-split-windows", lambda t: t.replace("    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n",
                                "    (void)render->scenery_window(0U, x0, 0, COLS, ROWS / 2U, words);\n    (void)render->scenery_window(0U, x0, (int32_t)(ROWS / 2U), COLS, ROWS - ROWS / 2U, words + (ROWS / 2U) * COLS);\n"))   # the same cells as two windows
        if h.failures:
            return 1

        def play(program, name, ratio, pad=PAD, save=None, identity=None):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(f"video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio={ratio}\n")
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            run, lines, frame, ram = h.play(program, name, pad, save=save, identity=identity, rom_sha=image_sha, extra={"SEGARECOMP_COVERAGE_REPORT": str(report), "SEGARECOMP_VIEWER_SETTINGS": str(cfg)})
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: the program runs to its own guest stop: {run.stderr[-300:]}")
            side = side_of(*map(int, ratio.split(":")))
            return surface_from_bmp(read_bmp(frame), side), lines, (json.loads(report.read_text()) if report.is_file() else None), ram.read_bytes()

        for ratio in ("16:9", "21:9", "32:9"):
            side = side_of(*map(int, ratio.split(":")))
            base, _, _, ram_v = play(tmp / "vanilla", "v-" + ratio.replace(":", "x"), ratio)
            surf, lines, rep, ram = play(modded, "m-" + ratio.replace(":", "x"), ratio)
            camera = int.from_bytes(ram[0x12:0x14], "big")   # the guest's own camera word (final work RAM of the run), never the program's picture
            check(CAMERA_FINAL <= camera <= CAMERA_FINAL + 1 and ram_v[0x12:0x14] == ram[0x12:0x14], f"{ratio}: the guest camera is the scripted one in both programs ({camera})")
            want = golden(side, camera)
            wrong = sum(1 for y in range(H) for xi in range(W + 2 * side) if want[y][xi] is not None and surf[y][xi] != want[y][xi] and not (xi < 24 and y < 8))   # the 24 x 8 HUD panel is the one drawn thing over the golden
            picture_same = all(surf[y][side + x] == base[y][side + x] for y in range(H) for x in range(W))
            check(wrong == 0, f"{ratio}: the authored left/right columns equal the independent golden ({wrong} pixels differ)")
            check(picture_same and ram == ram_v, f"{ratio}: the native picture and the guest state are unchanged")
            generic_equal = all(surf[y][xi] == base[y][xi] for y in range(H) for xi in range(0, side)) if side else True
            check(side == 0 or not generic_equal, f"{ratio}: the margins differ from the generic wrapped scenery of the Vanilla program")
            scenes = rep["scenes"] if rep else []
            comps = scenes[0]["components"] if scenes else {}
            check(bool(scenes) and scenes[0].get("id") == "main" and comps.get("scenery", {}).get("covered", 0) == comps.get("scenery", {}).get("requested", -1) > 0 and comps.get("hud", {}).get("covered", 0) >= 0 and comps.get("actors", {}).get("covered", 0) == 0,
                  f"{ratio}: coverage credits scenery for presented frames and never actors: {scenes[:1]}")
        # ---- negatives: every one of them presents the GENERIC scenery (never an authored one), faults only the mod where the contract was broken, and never reports scenery it did not present ----
        base, _, _, ram_v = play(tmp / "vanilla", "n-vanilla", "32:9")
        side = side_of(32, 9)
        for name, (_, _, cause) in variants.items():
            surf, lines, rep, ram = play(programs[name], "n-" + name, "32:9")
            summary = lines.get("MOD_SUMMARY", [{}])[0]
            margins_generic = all(surf[y][x] == base[y][x] for y in range(H) for x in list(range(side)) + list(range(side + W, W + 2 * side)) if not (x < 24 and y < 8))
            scenes = rep["scenes"] if rep else []
            scenery = scenes[0]["components"]["scenery"] if scenes else {"covered": 0}
            check((margins_generic or name == "narrow-window") and ram == ram_v, f"{name}: the margins stay generic (the answered rows of narrow-window are authored) and the guest state is untouched")
            check(scenery["covered"] == 0 or name == "stale", f"{name}: coverage credits no scenery: {scenery}")
            if name == "narrow-window":   # ADR 0313: unanswered scenery cells fail only the authored-scenery claim; the provider's independent HUD is still delivered for the very same frames
                hud = scenes[0]["components"]["hud"] if scenes else {"covered": 0, "requested": -1}
                check(hud["covered"] > 0 and hud["covered"] == hud["requested"] and scenery["covered"] == 0, f"{name}: the independent HUD stays credited while the scenery is not ({hud})")
            if cause is not None:
                check(summary.get("faulted") is True and f"cause={cause}" in summary.get("fault", ""), f"{name}: the callback faults with the stable cause `{cause}`, only the mod: {summary.get('fault')}")
            else:
                check(summary.get("faulted") is False, f"{name}: no fault: {summary.get('fault')}")
            if name == "stale":
                check(0 < scenery["covered"] < scenery["requested"], f"stale: only the frame the window was rendered for is credited ({scenery})")
        # ---- ADR 0313: two windows per plane. The same cells submitted as two windows give exactly the frame of the single window (a cell is answered by the first window of its plane that contains it) ----
        side = side_of(32, 9)
        surf_one, _, rep_one, ram_one = play(modded, "w-one", "32:9")
        surf_two, _, rep_two, ram_two = play(split_windows, "w-two", "32:9")
        c_one = rep_one["scenes"][0]["components"]["scenery"] if rep_one and rep_one["scenes"] else {"covered": 0}
        c_two = rep_two["scenes"][0]["components"]["scenery"] if rep_two and rep_two["scenes"] else {"covered": 0}
        check(surf_one == surf_two and ram_one == ram_two and c_two["covered"] == c_one["covered"] > 0, f"two windows holding the same cells present the identical frame and the identical coverage ({c_one} vs {c_two})")
        surf_g, lines_g, rep_g, ram_g = play(plane_b_generic, "g-one", "32:9")
        cam_g = int.from_bytes(ram_g[0x12:0x14], "big")
        want_g = golden(side, cam_g)
        wrong_g = sum(1 for y in range(H) for xi in range(W + 2 * side) if want_g[y][xi] is not None and surf_g[y][xi] != want_g[y][xi] and not (xi < 24 and y < 8))
        sc_g = rep_g["scenes"][0]["components"]["scenery"] if rep_g and rep_g["scenes"] else {"covered": 0, "requested": -1}
        hud_g = rep_g["scenes"][0]["components"]["hud"] if rep_g and rep_g["scenes"] else {"covered": 0, "requested": -1}
        check(wrong_g == 0 and sc_g["covered"] == sc_g["requested"] > 0 and hud_g["covered"] > 0 and lines_g["MOD_SUMMARY"][0]["faulted"] is False and surf_g == surf_one,
              f"ADR 0314: a plane without a window is left to the generic extension: plane A authored, equal to the golden ({wrong_g} pixels differ), frame equal to the two-plane frame, scenery credited, no fault ({sc_g})")
        # ---- SEG-152-T007 (ADR 0320): the REAL trusted attachment. A scheduled ABI 2 minor 2 provider marks every row of both planes through scenery_generic_rows (no window): the viewer's trusted attach honours the host-private
        # code 3, so the rows are credited as authored (covered == requested) and show the VDP's own frame-start plane (equal to Vanilla's generic wrap for this fixture, whose plane is never rewritten); no fault ----
        def mask_edit(t):
            t = t.replace("SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM", "SEGA_MOD_ABI_MAJOR, 2U, SEGA_MOD_PLATFORM")
            t = t.replace(both, "    {\n      static const uint8_t all_rows[28] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};\n      (void)render->scenery_generic_rows(0U, all_rows, 224U, 28U);\n      (void)render->scenery_generic_rows(1U, all_rows, 224U, 28U);\n    }\n")
            return t
        masked = project("v-mask-rows", mask_edit, lambda m: m.replace("[abi]\nnative = 2", "[abi]\nnative = 2\nminor = 2"))
        if h.failures:
            return 1
        surf_m, lines_m, rep_m, ram_m = play(masked, "mask-rows", "32:9")
        sc_m = rep_m["scenes"][0]["components"]["scenery"] if rep_m and rep_m["scenes"] else {"covered": 0, "requested": -1}
        check(lines_m["MOD_SUMMARY"][0]["faulted"] is False and sc_m["covered"] == sc_m["requested"] > 0 and ram_m == ram_v,
              f"trusted attach: a real scheduled provider's row mask is credited as authored (covered == requested), no fault: {sc_m} {lines_m['MOD_SUMMARY'][0].get('fault')}")
        check(all(surf_m[y][x] == base[y][x] for y in range(H) for x in list(range(side)) + list(range(side + W, W + 2 * side)) if not (x < 24 and y < 8)),
              "trusted attach: the masked rows present the VDP's own frame-start plane (the Vanilla generic margins)")
        # ---- ACTIVE scrolling: Right is held until the program ends, so the presented frame is captured WHILE the camera changes on consecutive frames ----
        MOVING = "0 R\n"
        mutant = project("t-shifted-cell", lambda t: t.replace("const int32_t cell = x0 + (int32_t)c;", "const int32_t cell = x0 + (int32_t)c + 1;"))   # a window whose words belong to the NEXT cell: the one-cell / one-frame misassociation
        if h.failures:
            return 1
        for ratio in ("16:9", "32:9"):
            sd = side_of(*map(int, ratio.split(":")))
            tag = ratio.replace(":", "x")
            for label, program in (("modded", modded), ("shifted", mutant)):
                surf, lines, rep, ram = play(program, f"mv-{label}-{tag}", ratio, MOVING)
                cam = int.from_bytes(ram[0x12:0x14], "big")
                _, _, _, ram_v = play(tmp / "vanilla", f"mv-vanilla-{tag}", ratio, MOVING)
                vsurf = play(tmp / "vanilla", f"mv-vanilla-{tag}", ratio, MOVING)[0]
                # the camera the PRESENTED frame used is read off the native picture itself (alternating green / red columns: its parity), never off the mod: the guest's final word or the one before it
                def picture_ok(k):
                    return all(vsurf[y][sd + x] == (rgb(0x00E0) if ((x + 8 * k) // 8) % 2 == 0 else rgb(0x000E)) for y in (40, 120, 200) for x in range(0, W, 3))
                used = [k for k in (cam, cam - 1) if picture_ok(k)]
                check(cam >= 30 and len(used) == 1 and surf[100][sd + 160] == vsurf[100][sd + 160] and ram == ram_v, f"{label} {ratio}: the camera is still moving at the end ({cam}), the native picture reveals exactly one camera for the presented frame {used} and equals Vanilla's")
                if not used:
                    continue
                want = golden(sd, used[0])
                wrong = sum(1 for y in range(H) for xi in range(W + 2 * sd) if want[y][xi] is not None and surf[y][xi] != want[y][xi] and not (xi < 24 and y < 8))
                sc = rep["scenes"][0]["components"]["scenery"]
                if label == "modded":
                    check(wrong == 0 and sc["covered"] == sc["requested"] > 0, f"moving camera {ratio}: the authored margins equal the independent golden for the camera of the presented frame ({wrong} pixels differ, {sc})")
                else:
                    check(wrong > 0, f"moving camera {ratio}: a window whose words are misassociated by one cell is DETECTED by the golden ({wrong} pixels differ)")
        # ---- frame contract (off-by-one-frame hazards): a fault midway, a scene change while a window is pending, save/restore and rewind across a pending submission ----
        mid = project("t-fault-midway", lambda t: t.replace(both, "    if (camera == 10U) {\n      (void)render->scenery_window(0U, x0, 0, 129U, ROWS, words);\n    } else {\n" + both + "    }\n"))
        scene = project("t-scene-change", lambda t: t.replace('render->scene("main", 4U);', 'render->scene(camera >= 10U ? "other" : "main", camera >= 10U ? 5U : 4U);'))
        if h.failures:
            return 1
        surf, lines, rep, ram = play(mid, "t-fault", "32:9")
        sc = rep["scenes"][0]["components"]["scenery"]
        check(lines["MOD_SUMMARY"][0]["faulted"] is True and 0 < sc["covered"] < rep["frames"] - 1, f"a fault midway: the frames before it are credited, none after, nothing stale is carried: {sc}")
        check(all(surf[y][x] == base[y][x] for y in range(H) for x in list(range(side)) + list(range(side + W, W + 2 * side)) if not (x < 24 and y < 8)), "after the fault the final frame is the generic one (no leftover authored column)")
        surf, lines, rep, ram = play(scene, "t-scene", "32:9")
        main = next(x for x in rep["scenes"] if x["id"] == "main")["components"]["scenery"]
        undeclared = next(x for x in rep["scenes"] if x["id"] == "*")["components"]["scenery"]
        check(0 < main["covered"] < rep["frames"] - 1 and undeclared["covered"] == 0 and undeclared["undeclared"] > 0, f"a scene change while windows are pending: `main` is credited only before it, the new undeclared scene never: {main} {undeclared}")
        ident = identities["modded"]
        save = tmp / "save"
        _, ls, _, _ = play(modded, "t-save", "32:9", "0 R\n12 RK\n13 R\n20 -\n", save=save, identity=ident)
        check(ls["VIEWER_STATE"][0]["status"] == "ok", "a quick save succeeds with scenery in flight")
        surf, lines, rep, ram = play(modded, "t-load", "32:9", "0 -\n3 M\n4 -\n", save=save, identity=ident)
        sc = rep["scenes"][0]["components"]["scenery"]
        cam = int.from_bytes(ram[0x12:0x14], "big")
        want = golden(side, cam)
        wrong = sum(1 for y in range(H) for xi in range(W + 2 * side) if want[y][xi] is not None and surf[y][xi] != want[y][xi] and not (xi < 24 and y < 8))
        check(lines["VIEWER_STATE"][0]["status"] == "ok" and sc["covered"] < sc["requested"] and wrong == 0 and lines["MOD_SUMMARY"][0]["faulted"] is False,
              f"after a restore the window of the pre-load timeline is never served (the first frame is generic), later frames are exact ({wrong} pixels differ, {sc})")
        surf, lines, rep, ram = play(modded, "t-rewind", "32:9", "0 R\n14 RY\n15 R\n20 -\n", save=tmp / "rewind", identity=ident)
        sc = rep["scenes"][0]["components"]["scenery"]
        check([(e["event"], e["status"]) for e in lines.get("VIEWER_STATE", [])] == [("rewind", "ok")] and sc["covered"] < sc["requested"] and lines["MOD_SUMMARY"][0]["faulted"] is False,
              f"a rewind across a pending submission leaves that frame generic and the mod healthy: {sc}")
        # ---- ADR 0313: the same save / restore / rewind contract with two windows per plane (both are staged, committed and discarded together with the frame they belong to) ----
        ident2 = identities.get("v-split-windows")
        if ident2:
            save2 = tmp / "save2"
            _, ls, _, _ = play(split_windows, "t2-save", "32:9", "0 R\n12 RK\n13 R\n20 -\n", save=save2, identity=ident2)
            check(ls["VIEWER_STATE"][0]["status"] == "ok", "two windows: a quick save succeeds with scenery in flight")
            surf, lines, rep, ram = play(split_windows, "t2-load", "32:9", "0 -\n3 M\n4 -\n", save=save2, identity=ident2)
            sc = rep["scenes"][0]["components"]["scenery"]
            cam = int.from_bytes(ram[0x12:0x14], "big")
            want = golden(side, cam)
            wrong = sum(1 for y in range(H) for xi in range(W + 2 * side) if want[y][xi] is not None and surf[y][xi] != want[y][xi] and not (xi < 24 and y < 8))
            check(lines["VIEWER_STATE"][0]["status"] == "ok" and sc["covered"] < sc["requested"] and wrong == 0 and lines["MOD_SUMMARY"][0]["faulted"] is False,
                  f"two windows: after a restore the pre-load windows are never served, later frames are exact ({wrong} pixels differ, {sc})")
            surf, lines, rep, ram = play(split_windows, "t2-rewind", "32:9", "0 R\n14 RY\n15 R\n20 -\n", save=tmp / "rewind2", identity=ident2)
            sc = rep["scenes"][0]["components"]["scenery"]
            check([(e["event"], e["status"]) for e in lines.get("VIEWER_STATE", [])] == [("rewind", "ok")] and sc["covered"] < sc["requested"] and lines["MOD_SUMMARY"][0]["faulted"] is False,
                  f"two windows: a rewind across a pending submission leaves that frame generic and the mod healthy: {sc}")
        if h.failures:
            return 1
    print("mod_scenery_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
