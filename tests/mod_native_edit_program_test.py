#!/usr/bin/env python3
"""A native ABI 2 package that ALSO carries a typed ROM edit, EXECUTED through the real SDL3 viewer of a generated-native program on the fabricated scrolling scenery fixture (no commercial image).

The package is the scenery fixture's provider plus ONE typed edit (the fixture's palette colour 1, word at 0x244: 0x00e0 green -> 0x0e00 blue). Proves in one played frame:
 (A) the effective image digest `mod apply` reports equals the digest of bytes constructed HERE from the original image (one word replaced, the Genesis header checksum repaired by the documented rule when the
     original verifies), and the edit changes exactly those bytes;
 (B) the program runs over that edited image: the native picture's colour-1 columns are blue where the unedited program shows green, and nothing else in the picture differs (Vanilla's picture with green -> blue);
 (C) at the same time the native provider contributes: its authored scenery fills the margins at 32:9 (checked against the fixture's independently constructed golden, with the edited colour where the tile uses it)
     and its HUD panel is drawn; coverage credits scenery;
 (D) the guest state of the edited program equals the unedited program's (the edit changes a palette word of the image, not the game's behaviour), and the unedited native package is untouched.
usage: mod_native_edit_program_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import hashlib
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

PAD = "0 R\n20 -\n"
GREEN, BLUE = rgb(0x00E0), rgb(0x0E00)
EDIT_SITE, EDIT_OLD, EDIT_NEW = 0x244, 0x00E0, 0x0E00


def checksum_ok(image):
    return sum((image[i] << 8) | image[i + 1] for i in range(0x200, len(image) - 1, 2)) & 0xFFFF == (image[0x18E] << 8) | image[0x18F]


def expected_effective(original):
    """The edited image built from the ORIGINAL bytes by the documented rules, never by the product's tool."""
    assert (original[EDIT_SITE] << 8) | original[EDIT_SITE + 1] == EDIT_OLD, "the fixture's colour word is not the expected original"
    out = bytearray(original)
    out[EDIT_SITE], out[EDIT_SITE + 1] = EDIT_NEW >> 8, EDIT_NEW & 0xFF
    if checksum_ok(original) and len(out) % 2 == 0:   # ADR 0293: an original that verifies is repaired when the build changed the image
        total = sum((out[i] << 8) | out[i + 1] for i in range(0x200, len(out) - 1, 2)) & 0xFFFF
        out[0x18E], out[0x18F] = total >> 8, total & 0xFF
    return bytes(out)


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_native_edit_program_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    spec = importlib.util.spec_from_file_location("scenery_fixture", fixtures / "scenery_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="segarecomp-native-edit-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "fixture.rom"
        rom.write_bytes(image)
        image_sha = sha(image)

        def project(name, edit):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            shutil.copy(fixtures / "provider.c", d / "src/provider.c")
            manifest = (fixtures / "mod.toml").read_text().replace("@@SHA@@", image_sha)
            if edit:
                (d / "edits").mkdir()
                (d / "edits/colour.edits").write_text(f"fixture u16 0x{EDIT_SITE:08x} 0x{EDIT_OLD:04x} 0x{EDIT_NEW:04x}\n")
                manifest = manifest.replace('id = "scenery-fixture-native"', 'id = "scenery-fixture-native-edit"').replace('[[source]]\nkind = "native-c11"\npath = "src/provider.c"\n', '[[source]]\nkind = "native-c11"\npath = "src/provider.c"\n\n[[source]]\nkind = "rom-edit-set"\npath = "edits/colour.edits"\n')
                assert "rom-edit-set" in manifest
            (d / "mod.toml").write_text(manifest)
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            built = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-300:]}{built.stderr[-300:]}")
            return out, tmp / name, doc

        plain_pkg, plain, plain_doc = project("plain", False)
        edit_pkg, edited, edit_doc = project("edited", True)
        if h.failures:
            return 1

        # (A) the effective image: the product's digest against bytes constructed independently
        r = h.run_cli("mod", "apply", edit_pkg, "--rom", rom, "--out", tmp / "effective.rom")
        doc = json.loads(r.stdout)
        effective = (tmp / "effective.rom").read_bytes()
        want = expected_effective(image)
        diff = [i for i in range(len(image)) if image[i] != effective[i]]
        check(r.returncode == 0 and doc.get("ok") and doc.get("edits_applied") == 1, f"mod apply applies the one edit: {r.stdout[:300]}")
        check(effective == want and hashlib.sha256(want).hexdigest() == doc.get("effective_sha256") == sha(effective), "the effective image equals the independently constructed bytes and its digest is the one reported")
        check(set(diff) <= {EDIT_SITE, EDIT_SITE + 1, 0x18E, 0x18F} and EDIT_SITE in diff, f"only the edited word (and the header checksum field, when the original verified) changed: {[hex(i) for i in diff]}")
        r2 = h.run_cli("mod", "apply", plain_pkg, "--rom", rom, "--out", tmp / "plain.rom")
        check(r2.returncode != 0 or (tmp / "plain.rom").read_bytes() == image, "the unedited package leaves the image as it is (or refuses to apply: it has no edit)")

        def play(program, name, identity):
            cfg = tmp / (name + ".cfg")
            cfg.write_text("video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio=32:9\n")
            report = tmp / (name + ".coverage.json")
            run, lines, frame, ram = h.play(program, name, PAD, identity=identity, rom_sha=image_sha, extra={"SEGARECOMP_COVERAGE_REPORT": str(report), "SEGARECOMP_VIEWER_SETTINGS": str(cfg)})
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: the program runs to its own guest stop: {run.stderr[-300:]}")
            side = side_of(32, 9)
            return surface_from_bmp(read_bmp(frame), side), ram.read_bytes(), json.loads(report.read_text()) if report.is_file() else None, lines

        side = side_of(32, 9)
        plain_surf, plain_ram, plain_rep, _ = play(plain, "plain", plain_doc["package_identity"])
        edit_surf, edit_ram, edit_rep, edit_lines = play(edited, "edited", edit_doc["package_identity"])
        # (B) the native picture: the unedited program's picture with colour 1 (green) replaced by blue, and nothing else changed
        wrong = changed = 0
        for y in range(H):
            for x in range(W):
                p, e = plain_surf[y][side + x], edit_surf[y][side + x]
                want_px = BLUE if p == GREEN else p
                if e != want_px and not (x < 24 and y < 8):   # the provider's HUD panel covers the corner
                    wrong += 1
                changed += p == GREEN and e == BLUE
        check(wrong == 0 and changed > 1000, f"the edited program's picture is the unedited one with green -> blue ({changed} pixels recoloured, {wrong} differ otherwise)")
        check(not any(px == GREEN for row in edit_surf for px in row[side:side + W]), "no green is left in the edited picture")
        # (C) the native provider's contribution in the same frame: authored scenery in the margins, the HUD panel, coverage
        margin_ok = all(plain_surf[y][x] == edit_surf[y][x] or (plain_surf[y][x] == GREEN and edit_surf[y][x] == BLUE) for y in range(H) for x in list(range(side)) + list(range(side + W, W + 2 * side)) if not (x < 24 and y < 8))
        check(margin_ok, "the authored margins are the unedited ones with the edited colour applied (the provider's contribution is intact over the edited image)")
        hud_differs = any(edit_surf[y][x] != edit_surf[y][side + 40] for y in range(8) for x in range(2, 20))
        check(hud_differs, "the provider's HUD panel is drawn in the same frame")
        sc = edit_rep["scenes"][0]["components"] if edit_rep and edit_rep["scenes"] else {}
        check(sc.get("scenery", {}).get("covered", 0) > 0 and sc["scenery"]["covered"] == sc["scenery"]["requested"], f"coverage credits the provider's scenery over the edited image: {sc.get('scenery')}")
        check(edit_lines["MOD_SUMMARY"][0]["faulted"] is False, "the provider is healthy over the edited image")
        # (D) the guest state is the unedited program's: the edit is a palette word of the image
        check(edit_ram == plain_ram, "the edited program's guest work RAM equals the unedited program's")
        if h.failures:
            return 1
    print("mod_native_edit_program_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
