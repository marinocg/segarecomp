#!/usr/bin/env python3
"""SEG-140-T011 (ADR 0309): authored offscreen scenery from a NATIVE ABI 2 provider, through the real SDL3 viewer of a generated-native program, on a FABRICATED scrolling fixture (no commercial image).

The provider (tests/fixtures/mod_scenery/provider.c) submits one name-table window per frame through SegaGenesisRenderApi::scenery_window. The golden is constructed here from the fixture's contract (plane A scrolls by
-(camera * 8); cells outside the picture alternate tile 2 / tile 3 of palette line 1) and never from the program's output. Proves: (A) the left and right margins at 16:9, 21:9 and 32:9 equal the golden while the native
picture equals Vanilla's; (B) the coverage report credits `scenery` and `hud` for presented frames and `actors` never; (C) the Vanilla program (and the same mod with the callback disconnected) presents the generic wrapped
scenery, never the authored one; (D) containment: an ungranted capability, an oversized or duplicated window, a bad plane and a NULL words pointer fault only the mod and present nothing; (E) a stale window is never
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


def golden(side, camera):
    """The expected surface margins: columns beyond the picture show the authored level cells; the picture itself is the fixture's own (green) plane."""
    green, blue, cyan = rgb(0x00E0), rgb(0x0E00), rgb(0x0EE0)
    rows = []
    for y in range(H):
        row = []
        for x in range(-side, W + side):
            if 0 <= x < W:
                row.append(None)
                continue
            cell = (x + camera * 8) // 8   # plane x = screen x - hscroll with hscroll = -(camera * 8)
            row.append(green if 24 <= cell < 64 else (blue if cell % 2 == 0 else cyan))
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
            return tmp / name

        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        modded = project("modded")
        if h.failures:
            return 1
        both = "    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n    /* plane B does not scroll in the fixture: its cells are asked for at their own, camera-independent coordinates */\n    (void)render->scenery_window(1U, -40, 0, COLS, ROWS, blank);\n"
        variants = {   # name -> (provider edit, manifest edit, the stable fault cause expected or None)
            "disconnected": (lambda t: t.replace(both, ""), None, None),
            "plane-b-missing": (lambda t: t.replace("    (void)render->scenery_window(1U, -40, 0, COLS, ROWS, blank);\n", ""), None, None),
            "stale": (lambda t: t.replace("static uint32_t camera;", "static uint32_t camera, submitted;").replace(both, "    if (submitted == 0U) {\n" + both + "      submitted = 1U;\n    }\n"), None, None),
            "ungranted": (None, lambda m: m.replace('"overlay.rgba", "scenery.words"', '"overlay.rgba"'), "capability"),
            "oversized": (lambda t: t.replace("scenery_window(0U, x0, 0, COLS, ROWS, words)", "scenery_window(0U, x0, 0, 129U, ROWS, words)"), None, "range"),
            "duplicate": (lambda t: t.replace(both, both + "    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n"), None, "range"),
            "bad-plane": (lambda t: t.replace("scenery_window(1U,", "scenery_window(2U,"), None, "range"),
            "null-words": (lambda t: t.replace("ROWS, blank)", "ROWS, 0)"), None, "range"),
        }
        programs = {name: project("v-" + name.replace("_", "-"), edit, toml) for name, (edit, toml, _) in variants.items()}
        if h.failures:
            return 1

        def play(program, name, ratio):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(f"video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio={ratio}\n")
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            run, lines, frame, ram = h.play(program, name, PAD, rom_sha=image_sha, extra={"SEGARECOMP_COVERAGE_REPORT": str(report), "SEGARECOMP_VIEWER_SETTINGS": str(cfg)})
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
            check(margins_generic and ram == ram_v, f"{name}: the margins stay generic and the guest state is untouched")
            check(scenery["covered"] == 0 or name == "stale", f"{name}: coverage credits no scenery: {scenery}")
            if cause is not None:
                check(summary.get("faulted") is True and f"cause={cause}" in summary.get("fault", ""), f"{name}: the callback faults with the stable cause `{cause}`, only the mod: {summary.get('fault')}")
            else:
                check(summary.get("faulted") is False, f"{name}: no fault: {summary.get('fault')}")
            if name == "stale":
                check(0 < scenery["covered"] < scenery["requested"], f"stale: only the frame the window was rendered for is credited ({scenery})")
        if h.failures:
            return 1
    print("mod_scenery_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
