#!/usr/bin/env python3
"""SEG-140-T009 (ADR 0307): virtual sprites in the WIDESCREEN side columns, in the REAL generated-native SDL3 viewer program. The fabricated vsprite fixture (tests/tools/vsprite_fixture_rom.py) is built into Vanilla and
modded programs; sprites are authored through the ABI 1 native provider seam and through Luau (`sega.sprite.space("presentation")`) and presented at 4:3, 16:9, 21:9 and 32:9 (side widths 0, 39, 101 and 238
source pixels, derived here by hand from the planner rule), compared pixel for pixel on the PRESENTED surface with the independent golden model (tests/vsprite_model.py).

Proves: the coordinate space (same origin as the picture; x < 0 left, x >= 320 right; the PICTURE space stays clipped to the picture, so every T008 golden is unchanged); sprites in each margin, straddling the
native boundary, touching and crossing the outer boundary of the presented surface (clipped, never moved); the side priority policy (ABOVE_ALL draws, a BEHIND_PLANES/BETWEEN command that reaches a margin rejects the
whole set with SIDE_PRIORITY, inside-picture layers are unchanged); LIMIT and EXTENDED budgets count a margin sprite's rows like any other; verified and authored provenance; per-region coverage (picture, left, right
pixels that reached the presented frame); a side treatment that replaces the columns (Comfort, Borders), Original (no columns) and an aspect outside the declaration present nothing in a margin and credit nothing;
a set never outlives its frame. Two demonstrations draw a sprite in a 32:9 side column, one per authoring route.
usage: mod_vsprite_wide_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import *  # noqa: E402,F401,F403

RATIOS = {"16:9": (16, 9), "21:9": (21, 9), "32:9": (32, 9)}


def cfg_text(ratio=None, style="raw", mode="extended_backgrounds"):
    if ratio is None:
        return "video.widescreen=original\nvideo.scale=1\nvideo.aspect=0\n"
    return f"video.widescreen={mode}\nvideo.widescreen_side_style={style}\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio={ratio}\n"


def sprites_for(case, side):
    pres = [sp(x=-24, y=150, tile=1)]
    return {
        "wide_left": ([sp(x=-24, y=150, tile=1)], "limit", "presentation"),
        "wide_right": ([sp(x=336, y=150, tile=2)], "limit", "presentation"),
        "wide_straddle": ([sp(x=-4, y=150, tile=1), sp(x=316, y=170, tile=2)], "limit", "presentation"),
        "wide_outer": ([sp(x=-side, y=150, tile=1), sp(x=320 + side - 8, y=150, tile=2), sp(x=-side - 4, y=180, tile=1), sp(x=320 + side - 4, y=180, tile=2)], "limit", "presentation"),
        "wide_layers_above": ([sp(x=-4, y=150, tile=1)], "limit", "presentation"),
        "wide_layers_between": ([sp(x=100, y=150, tile=1), sp(layer=BETWEEN, x=-4, y=150, tile=1)], "limit", "presentation"),
        "wide_layers_behind": ([sp(layer=BEHIND, x=316, y=150, tile=1)], "limit", "presentation"),
        "wide_layers_inside": ([sp(layer=BEHIND, x=20, y=150, tile=1), sp(layer=BETWEEN, x=60, y=150, tile=2), sp(x=-8, y=150, tile=1)], "limit", "presentation"),
        "wide_picture_space": ([sp(x=-4, y=150, tile=1), sp(x=316, y=170, tile=2)], "limit", "picture"),
        "wide_budget_limit": ([sp(x=-24, y=96, tile=1), sp(x=336, y=150, tile=1)], "limit", "presentation"),
        "wide_budget_extended": ([sp(x=-24, y=96, tile=1), sp(x=336, y=150, tile=1)], "extended", "presentation"),
        "wide_verified": ([sp("verified", x=330, y=150, tile=2), sp(x=-24, y=150, tile=1)], "limit", "presentation"),
        "wide_clip": ([sp(x=-12, y=150, w=2, tile=1, clip=(-6, 150, 10, 4))], "limit", "presentation"),
    }[case] if case != "none" else ([], "limit", "presentation")


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_vsprite_wide_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    spec = importlib.util.spec_from_file_location("vsprite_fixture", root / "tests/tools/vsprite_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    sps = importlib.util.spec_from_file_location("segapatch", root / "tools" / "segapatch.py")
    segapatch = importlib.util.module_from_spec(sps)
    sps.loader.exec_module(segapatch)
    provider_source = (root / "tests/fixtures/mod_vsprite/provider_abi1.c").read_bytes()
    with tempfile.TemporaryDirectory(prefix="segarecomp-vsprite-wide-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "vsprite.rom"
        rom.write_bytes(image)
        image_sha = sha(image)
        check(side_of(16, 9) == 39 and side_of(21, 9) == 101 and side_of(32, 9) == 238 and side_of(4, 3) == 0, "the hand-derived side widths of a 320 x 224 picture: 4:3 0, 16:9 39, 21:9 101, 32:9 238 (796 pixels wide)")

        def play(program, name, case=None, ratio=None, style="raw", mode="extended_backgrounds", side=0, extra=None):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(cfg_text(ratio, style, mode))
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            env = {"SEGARECOMP_COVERAGE_REPORT": str(report), "SEGARECOMP_VIEWER_SETTINGS": str(cfg), "VSPRITE_SIDE": str(side)}
            if case is not None:
                env["VSPRITE_CASE"] = case
            env.update(extra or {})
            run, lines, frame, _ = h.play(program, name, None, extra=env)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: the program runs to its own guest stop: {run.stderr[-200:]}")
            return (surface_from_bmp(read_bmp(frame), side) if frame.is_file() else None), (json.loads(report.read_text()) if report.is_file() else None)

        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")

        def package(name, caps):
            out = tmp / (name + ".segapatch")
            segapatch.create(image, None, out, id_="vsprite-provider", name="VSprite", version="1.0.0", native_source=provider_source, native_caps=caps, author="Test")
            ident = json.loads(h.run_cli("patch", "inspect", out).stdout)["package_identity"]
            r = h.run_cli("build", "--rom", rom, "--output", tmp / name, "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1",
                          "--sdl3-include", pathlib.Path(sdl) / "include", "--sdl3-lib", pathlib.Path(sdl) / "lib", "--test-frame-seam", "1", "--patch-package", out, "--patch-identity", ident)
            check(r.returncode == 0, f"{name} builds: {r.stdout[-300:]}{r.stderr[-300:]}")
            return tmp / name

        plain, state = package("abi1-plain", ["overlay.rgba"]), package("abi1-state", ["overlay.rgba", "frame.work_ram"])
        if h.failures:
            return 1

        def vs(rep):
            return rep.get("virtual_sprites", {})

        def actors(rep):
            return rep["scenes"][-1]["components"]["actors"]

        def region(counts):
            return {k: sum(counts[o][k] for o in counts) for k in ("picture", "left", "right")}

        bases = {}
        for ratio, (n, d) in RATIOS.items():
            side = side_of(n, d)
            base, rep = play(tmp / "vanilla", "van-" + ratio.replace(":", "x"), None, ratio, side=side)
            bases[ratio] = base
            check(len(base[0]) == 320 + 2 * side and base[3][0] == (0, 255, 0) and base[3][-1] == (0, 255, 0) and base[150][5] == rgb(FIXTURE_PALETTE[0]) and base[150][-3] == rgb(FIXTURE_PALETTE[0]),
                  f"{ratio}: the Vanilla surface is {320 + 2 * side} wide, the plane scenery continues into both margins and the rest is backdrop")

        # ---- the coordinate golden at every ratio: a sprite in each margin, straddling the boundary, flush with and across the outer boundary ----
        for ratio, (n, d) in RATIOS.items():
            side = side_of(n, d)
            tag = ratio.replace(":", "x")
            for case in ("wide_left", "wide_right", "wide_straddle", "wide_outer", "wide_clip", "wide_budget_limit", "wide_budget_extended", "wide_layers_above", "wide_layers_inside", "wide_picture_space"):
                sprites, budget, space = sprites_for(case, side)
                img, rep = play(plain, f"abi1-{tag}-{case}", case, ratio, side=side)
                want, counts, rej = model_wide(bases[ratio], side, sprites, budget, space)
                wrong = sum(1 for y in range(H) for x in range(320 + 2 * side) if img[y][x] != want[y][x])
                check(rej is None and wrong == 0, f"ABI 1 {ratio} {case}: the presented surface equals the independent golden ({wrong} pixels differ, rejection {rej})")
                reg = region(counts)
                block = vs(rep)
                frames = rep["frames"]
                check(block.get("pixels_left") == reg["left"] * frames and block.get("pixels_right") == reg["right"] * frames and block.get("pixels_picture") == reg["picture"] * frames,
                      f"ABI 1 {ratio} {case}: the pixels that reached the presented frame, by region, are the model's {reg}: {block}")
            # the coordinate contract in numbers: the left margin sprite is at surface x = side - 24 .. side - 17
            img, _ = play(plain, f"abi1-{tag}-origin", "wide_left", ratio, side=side)
            check(img[154][side - 24 + 3] == rgb(AUTHORED_PALETTE[1]) and img[154][side - 25] == bases[ratio][154][side - 25] and img[154][side - 16] == bases[ratio][154][side - 16],
                  f"{ratio}: x = -24 is surface column {side - 24}: the sprite starts exactly there")

        # ---- the side priority policy (ADR 0307): a layer that must be ordered against a native class cannot reach a column ----
        side = 238
        for case in ("wide_layers_between", "wide_layers_behind"):
            sprites, budget, space = sprites_for(case, side)
            img, rep = play(plain, f"abi1-32x9-{case}", case, "32:9", side=side)
            want, _, rej = model_wide(bases["32:9"], side, sprites, budget, space)
            check(rej == SIDE_PRIORITY and img == bases["32:9"] and rep["last_vsprite_reason"] == SIDE_PRIORITY and vs(rep).get("rejected") == rep["frames"] and vs(rep).get("presented") == 0 and actors(rep)["presented"] == 0,
                  f"{case}: a BEHIND_PLANES/BETWEEN command that reaches a margin rejects the WHOLE set (reason {SIDE_PRIORITY}); even the valid above-all sprite beside it is not drawn: {vs(rep)}")
        img, rep = play(plain, "abi1-32x9-bad-space", "wide_bad_space", "32:9", side=side)
        check(img == bases["32:9"] and rep["last_vsprite_reason"] == RESERVED, f"an unknown coordinate space is rejected as a reserved value: {rep['last_vsprite_reason']}")
        # verified and authored provenance, side by side, with the host's snapshot gate
        sprites, budget, space = sprites_for("wide_verified", side)
        img, rep = play(state, "abi1-32x9-verified", "wide_verified", "32:9", side=side)
        want, counts, _ = model_wide(bases["32:9"], side, sprites, budget, space)
        check(img == want and vs(rep).get("pixels_verified") == sum(counts["verified"].values()) * rep["frames"] and vs(rep).get("pixels_authored") == sum(counts["authored"].values()) * rep["frames"] and counts["verified"]["right"] > 0,
              f"a verified sprite in the right margin (the guest's own art) and an authored one in the left margin: separate counts, golden match: {vs(rep)}")
        img, rep = play(plain, "abi1-32x9-verified-ungated", "wide_verified", "32:9", side=side)
        check(img == bases["32:9"] and rep["last_vsprite_reason"] == UNVERIFIED, "without the snapshot gate the same set is rejected whole (UNVERIFIED_STATE), margins included")

        # ---- LIMIT versus EXTENDED in the margin: the original per-line budget counts the sprite's rows wherever x is ----
        lim, rep_l = play(plain, "abi1-32x9-limit", "wide_budget_limit", "32:9", side=side)
        ext, rep_e = play(plain, "abi1-32x9-extended", "wide_budget_extended", "32:9", side=side)
        check(lim[100][side - 22] == bases["32:9"][100][side - 22] and ext[100][side - 22] == rgb(AUTHORED_PALETTE[1]) and lim[154][side + 320 + 16 + 2] == rgb(AUTHORED_PALETTE[1]),
              "LIMIT drops the margin sprite on the saturated rows (no per-line slot left, wherever x is); EXTENDED draws it; a free row draws in both")

        # ---- a side treatment that replaces the columns, Original and the planner: nothing presented in a margin, nothing credited ----
        for style in ("comfort", "borders"):
            img, rep = play(plain, f"abi1-32x9-{style}", "wide_straddle", "32:9", style, side=side)
            block = vs(rep)
            check(block.get("pixels_left") == 0 and block.get("pixels_right") == 0 and block.get("pixels_picture") == 16 * rep["frames"] * 0 + (4 * 8 + 4 * 8) * rep["frames"],
                  f"{style}: the host replaces the columns, so only the picture halves of the straddling sprites are presented and counted: {block}")
            img, rep = play(plain, f"abi1-32x9-{style}-left", "wide_left", "32:9", style, side=side)
            check(vs(rep).get("pixels_left") == 0 and vs(rep).get("presented") == 0 and actors(rep)["presented"] == 0 and rep["last_reason"] == "virtual_sprite_unpresented",
                  f"{style}: a sprite wholly in a replaced column delivers nothing and the host says why: {vs(rep)} {rep['last_reason']}")
        img, rep = play(plain, "abi1-original-left", "wide_left", None, side=0)
        check(len(img[0]) == 320 and vs(rep).get("pixels_left") == 0 and vs(rep).get("composed_without_pixels") == rep["frames"] and actors(rep)["presented"] == 0, f"Original: no column is presented; a margin sprite draws nothing: {vs(rep)}")

        # ---- frame-to-frame: a set never outlives its frame ----
        cap = tmp / "capture"
        cap.mkdir()
        play(plain, "abi1-32x9-lifetime", "wide_lifetime", "32:9", side=side, extra={"SEGARECOMP_VIEWER_CAPTURE_DIR": str(cap), "SEGARECOMP_VIEWER_CAPTURE_FRAMES": "20,21,22,23"})
        shots = sorted(cap.glob("*.bmp"))
        seen = [surface_from_bmp(read_bmp(q), side)[154][side - 24 + 3] == rgb(AUTHORED_PALETTE[1]) for q in shots]
        check(len(shots) == 4 and seen in ([True, False, True, False], [False, True, False, True]), f"a margin sprite submitted for one frame is gone on the next: {seen}")

        # ---- the picture space and every T008 golden are untouched (the whole picture is byte-identical to a run with no side) ----
        img, _ = play(plain, "abi1-32x9-picture-space", "wide_picture_space", "32:9", side=side)
        check(all(img[y][x] == bases["32:9"][y][x] for y in range(H) for x in list(range(0, side - 1)) + list(range(side + 320 + 1, 320 + 2 * side))), "the PICTURE space never draws in a margin, even for a sprite hanging off the picture")

        # ---- the Luau route: the two demonstrations in a 32:9 side column, both Luau routes, with the declaration of the scene ----
        decl = '\n[geometry]\naspect_max = "32:9"\n\n[[scene]]\nid = "main"\ncomponents = ["actors"]\n'
        left_right = [sp(x=-100, y=150, tile=1), sp(x=420, y=150, tile=2)]
        for route, scheduled in (("luau-hud-seam", False), ("luau-scheduler", True)):
            program, _ = h.luau_program(rom, image_sha, f"{route}-wide", lua_for(left_right, space="presentation", scene="main"), scheduled, decl)
            img, rep = play(program, f"{route}-wide", None, "32:9", side=side)
            want, counts, rej = model_wide(bases["32:9"], side, left_right, "limit", "presentation")
            check(rej is None and img == want and counts["authored"]["left"] == 64 and counts["authored"]["right"] == 64, f"{route}: a sprite in each 32:9 side column, pixel-identical to the golden")
            a = rep["scenes"][0]["components"]["actors"]
            check(vs(rep).get("pixels_left") == 64 * rep["frames"] and vs(rep).get("pixels_right") == 64 * rep["frames"] and vs(rep).get("pixels_picture") == 0 and a["covered"] == rep["frames"] and a["presented"] == rep["frames"],
                  f"{route}: F15 coverage credits `actors` with 64 left and 64 right pixels that reached the presented frame and none in the picture: {vs(rep)} {a}")
            ref, _ = play(plain, f"abi1-ref-{route}", "none", "32:9", side=side)
        abi_demo, rep_abi = play(plain, "abi1-demo", "wide_outer", "32:9", side=side)
        check(abi_demo is not None and vs(rep_abi).get("pixels_left", 0) > 0 and vs(rep_abi).get("pixels_right", 0) > 0, "the ABI 1 demonstration draws in both 32:9 side columns")
        # the declaration: a package that declares only 4:3 is run at 32:9: the host presents the native picture alone, so nothing counts
        narrow, _ = h.luau_program(rom, image_sha, "luau-narrow", lua_for(left_right, space="presentation", scene="main"), True, decl.replace("32:9", "4:3"))
        img, rep = play(narrow, "luau-narrow", None, "32:9", side=side)
        a = rep["scenes"][0]["components"]["actors"]
        check(rep["outside_range_frames"] == rep["frames"] and vs(rep).get("presented") == 0 and vs(rep).get("pixels_left") == 0 and a["covered"] == 0 and a["presented"] == 0 and img[154][side - 100 + 3] != rgb(AUTHORED_PALETTE[1]),
              f"outside the declared range the native picture alone is shown: nothing is drawn in a margin, nothing is covered: {vs(rep)} {a}")
        # an invalid or unsupported request fails closed
        bad, _ = h.luau_program(rom, image_sha, "luau-bad-space", 'function render(frame: number)\n  sega.sprite.space("sideways")\nend\n', True)
        img, rep = play(bad, "luau-bad-space", None, "32:9", side=side)
        check(img == bases["32:9"] and rep is not None and "virtual_sprites" not in rep, "sega.sprite.space with an unknown name faults the mod (range); the frame is the Vanilla frame")

        # ---- bounded cost: composing a margin set costs the same order as the picture-only set (linear in rows, no per-pixel search), and zero commands cost nothing ----
        t0 = time.time()
        play(plain, "cost-none", "none", "32:9", side=side)
        t_none = time.time() - t0
        t0 = time.time()
        play(plain, "cost-wide", "wide_outer", "32:9", side=side)
        t_wide = time.time() - t0
        check(t_wide < max(4 * t_none, t_none + 20), f"a margin set stays within a bounded multiple of the no-set run ({t_none:.1f}s vs {t_wide:.1f}s for 90 frames of 4 sprites)")

        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_vsprite_wide_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
