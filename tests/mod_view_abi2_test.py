#!/usr/bin/env python3
"""ADR 0315: the asymmetric widescreen viewport from a NATIVE ABI 2 provider, through the real SDL3 viewer of a generated-native program, on the FABRICATED scrolling scenery fixture (no commercial image).

The provider is the scenery fixture's provider edited at the source level: it learns the host's planned margin through SegaGenesisRenderApi::view_margin, sizes its scenery window from the placement it chooses
(`left = min(camera * 10, 2 * margin)`) and declares it with view_left. The goldens are constructed HERE from the fixture's contract (the guest camera word, plane A at -(camera * 8), the authored cells) and
never from the program's output. Proves: (A) at 32:9 the picture is where the provider asked (left = 200) and the authored scenery fills both extension regions exactly (left 200, right 276) while the native
picture equals Vanilla's and the guest state is unchanged; (B) at 16:9 and 21:9 the same provider, with no code change, adapts to the margin the host plans there; (C) a placement that is out of range for
the plan is the centred layout (not a fault); one beyond the absolute bound is a range fault of the callback only; a provider that places only once leaves later frames centred; (D) a provider that
declares no placement is untouched (centred, byte-identical to the unmodified fixture); (E) restore across a pending placement never serves it.
usage: mod_view_abi2_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import H, W, rgb, side_of, surface_from_bmp  # noqa: E402

PAD = "0 R\n20 -\n"   # Right held for 20 turns, then released: the last presented frames share one camera


def golden(left, right, camera):
    """Expected extension columns of a surface whose picture starts at column `left` and which has `right` columns after it: screen x = column - left, plane x = screen x - hscroll (hscroll = -(camera * 8))."""
    green, red, blue, cyan = rgb(0x00E0), rgb(0x000E), rgb(0x0E00), rgb(0x0EE0)
    rows = []
    for y in range(H):
        row = []
        for x in range(-left, W + right):
            if 0 <= x < W:
                row.append(None)
                continue
            cell = (x + camera * 8) // 8
            row.append((green if cell % 2 == 0 else red) if 24 <= cell < 64 else (blue if cell % 2 == 0 else cyan))
        rows.append(row)
    return rows


PLACE = """    {
      uint32_t margin = 0U, left;
      (void)render->view_margin(&margin);
      if (margin != 0U) {
        left = camera * 10U < 2U * margin ? camera * 10U : 2U * margin;
        (void)render->view_left(left);
        x0 = (int32_t)camera - (int32_t)(left / 8U) - 3;   /* the windows follow the placement: the leftmost visible column is screen x = -left */
        bx0 = -(int32_t)(left / 8U) - 3;                    /* plane B does not scroll: its cell is screen x / 8 */
      }
    }
"""


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_view_abi2_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    spec = importlib.util.spec_from_file_location("scenery_fixture", fixtures / "scenery_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    keep = os.environ.get("SEGARECOMP_TEST_KEEP")
    with tempfile.TemporaryDirectory(prefix="segarecomp-view-") as directory:
        tmp = pathlib.Path(keep or directory)
        tmp.mkdir(exist_ok=True)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "view.rom"
        rom.write_bytes(image)
        image_sha = sha(image)
        identities = {}
        marker = "    x0 = (int32_t)camera - 40;\n"

        def project(name, edit=None):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            source = (fixtures / "provider.c").read_text().replace("    int32_t x0;\n", "    int32_t x0, bx0 = -40;\n").replace("scenery_window(1U, -40, 0, COLS, ROWS, blank)", "scenery_window(1U, bx0, 0, COLS, ROWS, blank)")
            new = edit(source) if edit else source
            if edit:
                check(new != source, f"{name}: the edit changed the source")
            (d / "src/provider.c").write_text(new)
            (d / "mod.toml").write_text((fixtures / "mod.toml").read_text().replace("@@SHA@@", image_sha))
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            built = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-400:]}{built.stderr[-300:]}")
            identities[name] = doc.get("package_identity")
            return tmp / name

        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        unmodified = project("plain")   # the fixture's own provider: declares no placement
        placed = project("placed", lambda t: t.replace(marker, marker + PLACE))
        outofrange = project("out-of-range", lambda t: t.replace(marker, marker + "    (void)render->view_left(600U);\n"))   # valid for 32:9 (total 476) but beyond the 16:9 total (78)
        absolute = project("absolute", lambda t: t.replace(marker, marker + "    (void)render->view_left(1345U);\n"))      # above 2 * the maximum margin: a range fault
        once = project("once", lambda t: t.replace("static uint32_t camera;", "static uint32_t camera, placed_once;").replace(marker, marker + "    if (placed_once == 0U) {\n      uint32_t m = 0U;\n      (void)render->view_margin(&m);\n      if (m != 0U) { (void)render->view_left(m); placed_once = 1U; }\n    }\n"))
        no_follow = "\n".join(line for line in PLACE.splitlines() if "x0 =" not in line and "bx0 =" not in line) + "\n"
        mutant = project("no-follow", lambda t: t.replace(marker, marker + no_follow))   # placed 200/276 but with the centred layout's windows: the right-hand cells are unanswered
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

        def wrong_pixels(surf, left, right, camera):
            want = golden(left, right, camera)
            return sum(1 for y in range(H) for xi in range(left + W + right) if want[y][xi] is not None and surf[y][xi] != want[y][xi] and not (xi < 24 and y < 8))   # the 24 x 8 HUD panel is drawn over the corner

        def scenery(rep):
            return rep["scenes"][0]["components"]["scenery"] if rep and rep["scenes"] else {"covered": 0, "requested": -1}

        # ---- (A)/(B): the placement follows the provider's rule at every planned margin; the scenery window follows the placement ----
        for ratio in ("32:9", "21:9", "16:9"):
            side = side_of(*map(int, ratio.split(":")))
            tag = ratio.replace(":", "x")
            base, _, _, ram_v = play(tmp / "vanilla", "v-" + tag, ratio)
            surf, lines, rep, ram = play(placed, "p-" + tag, ratio)
            camera = int.from_bytes(ram[0x12:0x14], "big")
            left = min(camera * 10, 2 * side)
            right = 2 * side - left
            check(ram == ram_v, f"{ratio}: the guest state is unchanged (Vanilla's)")
            check(all(surf[y][left + x] == base[y][side + x] for y in range(H) for x in range(W)), f"{ratio}: the native picture is at column {left} and equals Vanilla's picture")
            wrong = wrong_pixels(surf, left, right, camera)
            sc = scenery(rep)
            check(wrong == 0, f"{ratio}: the authored columns equal the independent golden for a {left}/{right} split ({wrong} pixels differ)")
            check(sc["covered"] == sc["requested"] > 0 and lines["MOD_SUMMARY"][0]["faulted"] is False, f"{ratio}: scenery is credited for presented frames and the mod is healthy: {sc}")
            if side == side_of(32, 9):
                check(camera in (20, 21) and left == camera * 10 and right == 476 - left, f"32:9 is the {left}/{right} split of the 476-column total")
            check(left != side, f"{ratio}: the split is genuinely asymmetric")
        # ---- a provider whose windows do not follow its own placement: cells the windows do not hold are unanswered, so the host never presents that scenery as authored ----
        surf, lines, rep, ram = play(mutant, "m-32x9", "32:9")
        camera = int.from_bytes(ram[0x12:0x14], "big")
        left = min(camera * 10, 476)
        sc = scenery(rep)
        check(sc["covered"] < sc["requested"] and wrong_pixels(surf, left, 476 - left, camera) > 0 and lines["MOD_SUMMARY"][0]["faulted"] is False,
              f"windows that do not cover the placed view are withdrawn (not credited), the extension is generic, the mod is healthy: {sc}")
        # ---- (D): no placement declared: the frame is byte-identical to the unmodified fixture's and centred ----
        surf_plain, lines_plain, rep_plain, ram_plain = play(unmodified, "u-32x9", "32:9")
        cam_u = int.from_bytes(ram_plain[0x12:0x14], "big")
        side = side_of(32, 9)
        check(wrong_pixels(surf_plain, side, side, cam_u) == 0 and scenery(rep_plain)["covered"] == scenery(rep_plain)["requested"] > 0, "without a declared placement the picture stays centred with its authored scenery")
        # ---- (C): out of range for the plan is centred, not a fault; beyond the absolute bound is a range fault of the callback only ----
        base16, _, _, ram16 = play(tmp / "vanilla", "v-16x9-c", "16:9")
        surf, lines, rep, ram = play(outofrange, "o-16x9", "16:9")
        s16 = side_of(16, 9)
        cam = int.from_bytes(ram[0x12:0x14], "big")
        check(lines["MOD_SUMMARY"][0]["faulted"] is False and all(surf[y][s16 + x] == base16[y][s16 + x] for y in range(H) for x in range(W)) and ram == ram16,
              "a placement beyond the current plan's total is the centred layout: the picture is centred, no fault, the guest untouched")
        surf, lines, rep, ram = play(absolute, "a-32x9", "32:9")
        summary = lines["MOD_SUMMARY"][0]
        check(summary.get("faulted") is True and "cause=range" in summary.get("fault", ""), f"a placement above the absolute bound faults only the mod with the stable cause `range`: {summary.get('fault')}")
        base32, _, _, _ = play(tmp / "vanilla", "v-32x9-c", "32:9")
        check(all(surf[y][side + x] == base32[y][side + x] for y in range(H) for x in range(W)), "after that fault the picture is centred and generic")
        surf, lines, rep, ram = play(once, "once-32x9", "32:9")
        check(lines["MOD_SUMMARY"][0]["faulted"] is False and all(surf[y][side + x] == base32[y][side + x] for y in range(H) for x in range(W)),
              "a provider that placed the picture once leaves every later frame centred: a placement is never carried to another frame")
        # ---- (E): save / restore across a pending placement never serves it; later frames are exact ----
        ident = identities["placed"]
        save = tmp / "save"
        _, ls, _, _ = play(placed, "t-save", "32:9", "0 R\n12 RK\n13 R\n20 -\n", save=save, identity=ident)
        check(ls["VIEWER_STATE"][0]["status"] == "ok", "a quick save succeeds with a placement in flight")
        surf, lines, rep, ram = play(placed, "t-load", "32:9", "0 -\n3 M\n4 -\n", save=save, identity=ident)
        camera = int.from_bytes(ram[0x12:0x14], "big")
        left = min(camera * 10, 476)
        sc = scenery(rep)
        check(lines["VIEWER_STATE"][0]["status"] == "ok" and lines["MOD_SUMMARY"][0]["faulted"] is False and wrong_pixels(surf, left, 476 - left, camera) == 0 and sc["covered"] < sc["requested"],
              f"after a restore the pre-load placement is not served (the first frame is generic and centred), later frames are exact ({sc})")
        if h.failures:
            return 1
    print("mod_view_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
