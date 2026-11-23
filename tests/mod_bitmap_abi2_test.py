#!/usr/bin/env python3
"""SEG-152-T005/T006 (ADR 0320 decisions 6 and 7): the FINAL presented image of ABI 2 bitmaps and of a refused HUD through the real SDL3 viewer and its one compositor, on the FABRICATED mid-frame fixture (no commercial image).

Programs built from one fixture image:
  vanilla   no package (the generic picture);
  bitmap    the ABI 2 probe (fixtures/mod_bitmap/probe.c, exact-frame): a 40 x 16 bitmap in the fixed overlay with the ABI 1 fallback position, a 64 x 48 bitmap LAYER (top-right, picture reference,
            dx/dy) with a clip rectangle, and a placement;
  twin      the equivalent ABI 1 provider (twin_abi1.c) writing the same art straight into GenesisCompatOverlay (the established oracle);
  refuse    the probe plus a HUD decline and one authored virtual sprite (an independent component).
Proved: (1) the ABI 2 frame equals the ABI 1 twin's frame byte for byte at 32:9 with a placement, in Original and in exact 4:3; (2) in exact 4:3 the pixels equal an INDEPENDENT golden recomputed here from the
art formula (alpha-0 raw RGB shows the picture, opaque pixels exact, the layer drawn only inside its clip, the fixed overlay at the fallback position); (3) a refused HUD presents no HUD pixels while the independent
sprite is drawn, and the picture elsewhere is Vanilla's.
usage: mod_bitmap_abi2_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mod_exact_frame_abi2_test import ENHANCED, PAD  # noqa: E402
from viewer_mod_support import Harness, sha  # noqa: E402
from vsprite_model import H, W, rgb, surface_from_bmp  # noqa: E402

SPRITE = """  vs.count = 1U;
  vs.sprites[0].provenance = GENESIS_VSPRITE_MOD_AUTHORED;
  vs.sprites[0].layer = GENESIS_VSPRITE_LAYER_ABOVE_ALL;
  vs.sprites[0].width_cells = 1U;
  vs.sprites[0].height_cells = 1U;
  vs.sprites[0].tile_index = 1U;
  vs.sprites[0].x = 100;
  vs.sprites[0].y = 150;
  for (c = 0U; c < 32U; ++c) vs.authored_vram[32U + c] = 0x11U;
  vs.authored_cram[2] = 0x0EU;
  vs.authored_cram[3] = 0x0EU;
  (void)r->virtual_sprites(&vs);"""
REFUSE = "(void)r->decline(SEGA_GEN_COMPONENT_HUD, SEGA_GEN_REASON_MISMATCH);"


def art(x, y, k):
    """The independent copy of bitmap_art.h: (r, g, b, a) of pixel (x, y) of the art with seed k."""
    kind = (x // 8 + y // 8 + k) % 3
    if kind == 0:
        return (200, 100, 50, 0)
    if kind == 1:
        return ((x * 6 + k) & 255, (y * 14) & 255, 64 + ((x ^ y) & 63), 255)
    return ((x * 5) & 255, (255 - y * 3) & 255, 77, 128)


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_bitmap_abi2_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    bdir = root / "tests/fixtures/mod_bitmap"
    spec = importlib.util.spec_from_file_location("scenery_fixture", fixtures / "scenery_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    sps = importlib.util.spec_from_file_location("segapatch", root / "tools" / "segapatch.py")
    segapatch = importlib.util.module_from_spec(sps)
    sps.loader.exec_module(segapatch)
    art_h = (bdir / "bitmap_art.h").read_text()
    inline = lambda text: text.replace('#include "bitmap_art.h"', art_h)  # noqa: E731
    keep = os.environ.get("SEGARECOMP_TEST_KEEP")
    with tempfile.TemporaryDirectory(prefix="segarecomp-bitmap-") as directory:
        tmp = pathlib.Path(keep or directory)
        tmp.mkdir(exist_ok=True)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom(mid_frame=True)
        rom = tmp / "bitmap.rom"
        rom.write_bytes(image)
        image_sha = sha(image)

        def project(name, edit=None):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            source = inline((bdir / "probe.c").read_text())
            source = edit(source) if edit else source
            (d / "src/provider.c").write_text(source)
            (d / "mod.toml").write_text((bdir / "mod.toml").read_text().replace("@@SHA@@", image_sha))
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            built = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-400:]}{built.stderr[-300:]}")
            return tmp / name

        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        bitmap = project("bitmap", lambda t: t.replace("/*@@SPRITE@@*/", "").replace("/*@@REFUSE@@*/", ""))
        refuse = project("refuse", lambda t: t.replace("/*@@SPRITE@@*/", SPRITE).replace("/*@@REFUSE@@*/", REFUSE).replace("  if (v->side > 0U) (void)r->view_left(v->side / 2U, v->side);\n", ""))   # no placement: the picture stays centred like Vanilla's
        tw = tmp / "twin.segapatch"
        segapatch.create(image, None, tw, id_="bitmap-twin", name="Bitmap twin", version="1.0.0", native_source=inline((bdir / "twin_abi1.c").read_text()).encode(),
                         native_caps=["frame.vdp", "overlay.rgba"], author="Test")
        ident = json.loads(h.run_cli("patch", "inspect", tw).stdout)["package_identity"]
        r = h.run_cli("build", "--rom", rom, "--output", tmp / "twin", "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1", "--sdl3-include", pathlib.Path(sdl) / "include",
                      "--sdl3-lib", pathlib.Path(sdl) / "lib", "--test-frame-seam", "1", "--patch-package", tw, "--patch-identity", ident)
        check(r.returncode == 0, f"twin builds: {r.stdout[-300:]}{r.stderr[-300:]}")
        if h.failures:
            return 1

        def play(program, name, cfg_text, schedule=None):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(cfg_text)
            env = {"SEGARECOMP_VIEWER_SETTINGS": str(cfg), "SEGARECOMP_VIEWER_FINAL_FRAME_NO_NOTICES": "1"}
            if schedule:
                env["SEGARECOMP_VIEWER_TEST_VIDEO_AT"] = schedule
            run, lines, frame, _ram = h.play(program, name, PAD, rom_sha=image_sha, extra=env, budget=20_000_000)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: runs to its own guest stop: {run.stderr[-300:]}")
            from viewer_mod_support import read_bmp
            return {"bmp": read_bmp(frame), "summary": (lines.get("MOD_SUMMARY") or [{}])[0]}

        # ---- (1) ABI 2 == ABI 1 twin through the compositor: 32:9 with a placement, Original, exact 4:3 ----
        modes = {"wide": (ENHANCED % "32:9", None), "orig": (ENHANCED % "32:9", "10:orig"), "43": (ENHANCED % "32:9", "10:4:3")}
        res = {}
        for key, (cfg, sched) in modes.items():
            res[key] = {n: play(p, f"{n}-{key}", cfg, sched) for n, p in (("bitmap", bitmap), ("twin", tw and tmp / "twin"), ("vanilla", tmp / "vanilla"), ("refuse", refuse))}
            check(res[key]["bitmap"]["summary"].get("faulted") is False, f"{key}: the bitmap provider is healthy")
            check(res[key]["bitmap"]["bmp"] == res[key]["twin"]["bmp"], f"{key}: the presented frame of the ABI 2 bitmaps equals the ABI 1 twin's byte for byte")
            check(res[key]["bitmap"]["bmp"] != res[key]["vanilla"]["bmp"], f"{key}: the HUD is actually drawn (the frame differs from Vanilla's)")
        # ---- (2) independent golden in exact 4:3 (side 0, picture reference, fallback position) ----
        van = surface_from_bmp(res["43"]["vanilla"]["bmp"], 0)
        got = surface_from_bmp(res["43"]["bitmap"]["bmp"], 0)
        bad = []
        expect = {}
        for y in range(16):                       # the fixed overlay sits at the fallback position (30, 60) inside the picture
            for x in range(40):
                expect[(30 + x, 60 + y)] = art(x, y, 0)
        for y in range(48):                       # layer: top-right of the picture, dx -4, dy 100: left edge 320 - 64 - 4, only its clip rectangle is drawn
            for x in range(64):
                if 8 <= x < 40 and 8 <= y < 32:
                    expect[(252 + x, 100 + y)] = art(x, y, 1)
        opaque = alpha0 = clipped = 0
        for y in range(H):
            for x in range(W):
                e = expect.get((x, y))
                if e is None:
                    if got[y][x] != van[y][x]:
                        bad.append((x, y, "outside the art"))
                elif e[3] == 255:
                    opaque += 1
                    if got[y][x] != (e[0], e[1], e[2]):
                        bad.append((x, y, "opaque"))
                elif e[3] == 0:
                    alpha0 += 1
                    if got[y][x] != van[y][x]:
                        bad.append((x, y, "alpha 0 must show the picture"))
        for y in range(48):                       # layer pixels outside the clip are not drawn
            for x in range(64):
                if not (8 <= x < 40 and 8 <= y < 32):
                    clipped += 1
                    if (252 + x, 100 + y) not in expect and got[100 + y][252 + x] != van[100 + y][252 + x]:
                        bad.append((252 + x, 100 + y, "outside the clip"))
        check(opaque > 300 and alpha0 > 300 and clipped > 1000 and not bad, f"exact 4:3: opaque ({opaque}), alpha-0 ({alpha0}) and clipped ({clipped}) pixels equal the independent golden ({len(bad)} differences, first {bad[:3]})")
        # ---- (3) a refused HUD: no HUD pixels, the independent sprite stays, the picture elsewhere is Vanilla's ----
        for key, side in (("43", 0),):
            rv = surface_from_bmp(res[key]["refuse"]["bmp"], side)
            vv = surface_from_bmp(res[key]["vanilla"]["bmp"], side)
            magenta = rgb(0x0E0E)
            diff = [(x, y) for y in range(H) for x in range(W) if rv[y][x] != vv[y][x]]
            check(diff and all(100 <= x < 108 and 150 <= y < 158 and rv[y][x] == magenta for x, y in diff), f"{key}: a refused HUD presents no HUD pixels; only the independent sprite (8 x 8 at 100,150) differs from Vanilla ({len(diff)} pixels, first {diff[:3]})")
            check(len(diff) == 64, f"{key}: the whole sprite is drawn ({len(diff)} of 64 pixels)")
        wr = surface_from_bmp(res["wide"]["refuse"]["bmp"], 238)
        wv = surface_from_bmp(res["wide"]["vanilla"]["bmp"], 238)
        wdiff = [(x, y) for y in range(H) for x in range(len(wr[0])) if wr[y][x] != wv[y][x]]
        check(len(wdiff) == 64 and all(wr[y][x] == rgb(0x0E0E) for x, y in wdiff), f"32:9: a refused HUD leaves only the sprite ({len(wdiff)} pixels differ from Vanilla)")
        check(res["wide"]["refuse"]["summary"].get("faulted") is False, "a refusal is not a fault: the mod stays enabled")
        if h.failures:
            return 1
    print("mod_bitmap_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
