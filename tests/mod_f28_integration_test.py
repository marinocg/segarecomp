#!/usr/bin/env python3
"""F28 integration (SEG-138-T010, SEG-138-T011, SEG-140-T008, SEG-140-T009, SEG-140-T010): ONE generated-native Genesis program per authoring route runs a scheduled gameplay mod (guest-RAM effect), shows its HUD
and draws authored virtual sprites in the 32:9 side columns, all through the real SDL3 viewer: the Luau mod through the one scheduler, and its NATIVE ABI 2 twin through the typed render feature. The fabricated
gameplay fixture (platforms/genesis/sdk/examples/gameplay) is the guest; no commercial input.

Proves together: (A) Luau gameplay + HUD + side-column sprites; (B) native ABI 2 gameplay + HUD + typed sprites; (C) the two routes present the identical surface and report identical accounting; (D) the sprites in the
left and right 32:9 margins equal the independent golden on the FINAL surface; (E) replay, save/restore and rewind keep the guest effect, the mod state, the HUD and the margin sprites; (F) fail-closed negatives
(unauthorized guest read, malformed descriptor, out-of-range geometry, an identity that was not approved); the Vanilla program is unchanged by all of it.
usage: mod_f28_integration_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import *  # noqa: E402,F401,F403

LEFT, RIGHT = sp(x=-60, y=150, tile=1), sp(x=360, y=150, tile=2)
SPRITES = [LEFT, RIGHT]
WRITE_DIGEST = "194aba886c6e5d5006f0167d766991397ba0996e7767dd9bdb9e201a8e10336d"   # 91 committed bonus writes, C held for the whole program (the gameplay example's pinned witness)
HUD_BOX = (0, 0, 120, 40)   # surface pixels (x0, y0, x1, y1) where the HUD panel is drawn; the golden skips it


def derived_ram(score, position):
    ram = bytearray(65536)
    ram[0x10:0x14] = score.to_bytes(2, "big") + position.to_bytes(2, "big")
    return bytes(ram)


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_f28_integration_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    luau_example, native_example = root / "platforms/genesis/sdk/examples/gameplay", root / "platforms/genesis/sdk/examples/gameplay-native"
    spec = importlib.util.spec_from_file_location("fixture_rom", luau_example / "fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="segarecomp-f28-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "gameplay.rom"
        rom.write_bytes(image)
        image_sha = sha(image)
        side = side_of(32, 9)

        sprite_lua = ('  sega.sprite.space("presentation")\n  sega.sprite.tile(1, "%s")\n  sega.sprite.tile(2, "%s")\n  sega.sprite.palette(0, "%s")\n' % (hexpattern(POOL_TILES[1]), hexpattern(POOL_TILES[2]), "".join("%04X" % w for w in AUTHORED_PALETTE))
                      + '  sega.sprite.add("authored", "above_all", -60, 150, 1, 1, 1, 0, 0)\n  sega.sprite.add("authored", "above_all", 360, 150, 1, 1, 2, 0, 0)\n')
        decl = '\n[geometry]\naspect_max = "32:9"\n\n[[scene]]\nid = "main"\ncomponents = ["actors", "hud"]\n'

        def luau_project(name, edit=None, sprites=sprite_lua):
            d = tmp / (name + "-src")
            shutil.copytree(luau_example, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
            script = (d / "scripts/main.luau").read_text().replace("function render(frame: number)\n", 'function render(frame: number)\n  sega.scene("main")\n' + sprites, 1)
            (d / "scripts/main.luau").write_text(edit(script) if edit else script)
            (d / "mod.toml").write_text((d / "mod.toml").read_text() + decl)
            return d

        def native_project(name, edit=None, with_sprites=True):
            d = tmp / (name + "-src")
            shutil.copytree(native_example, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
            src = (d / "src/provider.c").read_text()
            fill = "".join('  tile(%dU, "%s");\n' % (i, hexpattern(POOL_TILES[i])) for i in (1, 2)) + "".join("  colour(0U, %dU, 0x%04XU);\n" % (i, w) for i, w in enumerate(AUTHORED_PALETTE))
            helpers = f'''static GenesisVirtualSpriteSet set;
static unsigned nibble(char c) {{ return (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); }}
static void tile(unsigned index, const char *hex) {{ unsigned i; for (i = 0U; i < 32U; ++i) set.authored_vram[index * 32U + i] = (uint8_t)(nibble(hex[2U * i]) << 4 | nibble(hex[2U * i + 1U])); }}
static void colour(unsigned line, unsigned entry, unsigned word) {{ set.authored_cram[line * 32U + entry * 2U] = (uint8_t)(word >> 8); set.authored_cram[line * 32U + entry * 2U + 1U] = (uint8_t)word; }}
static void add(int x, unsigned tile_index) {{ GenesisVirtualSprite *c = &set.sprites[set.count++]; c->provenance = 2U; c->layer = 2U; c->width_cells = 1U; c->height_cells = 1U; c->tile_index = (uint16_t)tile_index; c->x = (int16_t)x; c->y = 150; }}
static void submit_sprites(const SegaGenesisRenderApi *render) {{
  set.count = 0U; set.space = 1U;
{fill}  add(-60, 1U); add(360, 2U);
  (void)render->virtual_sprites(&set);
}}
'''
            src = src.replace("static uint32_t granted;", "static uint32_t granted;\n" + helpers, 1)
            if with_sprites:
                src = src.replace("    render->overlay_rect(0, 0, 76, 24, PANEL);", '    (void)render->scene("main", 4U);\n    submit_sprites(render);\n    render->overlay_rect(0, 0, 76, 24, PANEL);', 1)
            (d / "src/provider.c").write_text(edit(src) if edit else src)
            (d / "mod.toml").write_text((d / "mod.toml").read_text() + decl)
            return d

        def pack(d):
            out = tmp / (d.name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {d.name}: {r.stdout[:300]}")
            return out, doc.get("package_identity")

        luau_pkg, luau_id = pack(luau_project("f28-luau"))
        native_pkg, native_id = pack(native_project("f28-native"))
        for name, pkg, ident, native in (("vanilla", None, None, False), ("luau", luau_pkg, luau_id, False), ("native", native_pkg, native_id, True)):
            r = h.build(rom, tmp / name, pkg, ident, native=native)
            check(r.returncode == 0, f"{name} builds: {r.stdout[-300:]}{r.stderr[-300:]}")
        if h.failures:
            return 1

        def play(program, name, pad="0 C\n", save=None, identity=None, extra=None):
            cfg = tmp / (name + ".cfg")
            cfg.write_text("video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio=32:9\n")
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            env = {"SEGARECOMP_COVERAGE_REPORT": str(report), "SEGARECOMP_VIEWER_SETTINGS": str(cfg)}
            env.update(extra or {})
            run, lines, frame, ram = h.play(program, name, pad, save=save, identity=identity, rom_sha=image_sha, extra=env)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: the program runs to its own guest stop: {run.stderr[-200:]}")
            return surface_from_bmp(read_bmp(frame), side), ram.read_bytes(), lines, (json.loads(report.read_text()) if report.is_file() else None)

        base, ram_v, lines_v, rep_v = play(tmp / "vanilla", "f28-vanilla")
        check(ram_v == derived_ram(0, 0) and "MOD_SUMMARY" not in lines_v and "virtual_sprites" not in rep_v, "Vanilla: no guest effect, no mod, no sprite accounting")

        def outside_hud(x, y):
            return not (HUD_BOX[0] <= x < HUD_BOX[2] and HUD_BOX[1] <= y < HUD_BOX[3])

        results = {}
        for route, program in (("luau", tmp / "luau"), ("native", tmp / "native")):
            surf, ram, lines, rep = play(program, "f28-" + route)
            results[route] = (surf, ram, lines, rep)
            ms = lines["MOD_SUMMARY"][0]
            check(ram == derived_ram(900, 0) and ms["faulted"] is False and ms["write_digest"] == WRITE_DIGEST and ms["overlay_ops"] == 4, f"{route}: the gameplay effect (score 900 while C is held), no fault, the pinned replay witness and the HUD: {ms['write_digest']}")
            want, counts, rej = model_wide(base, side, SPRITES)
            wrong = sum(1 for y in range(H) for x in range(320 + 2 * side) if outside_hud(x, y) and surf[y][x] != want[y][x])
            check(rej is None and wrong == 0 and counts["authored"]["left"] == 64 and counts["authored"]["right"] == 64, f"{route}: the sprites in the left and right 32:9 margins equal the independent golden on the presented surface ({wrong} pixels differ)")
            check(any(surf[y][x] == (255, 255, 255) for y in range(0, 40) for x in range(0, 120)) or any(surf[y][x] != base[y][x] for y in range(0, 40) for x in range(0, 120)), f"{route}: the HUD reached the presented frame")
            block = rep["virtual_sprites"]
            actors = rep["scenes"][0]["components"]["actors"]
            hud = rep["scenes"][0]["components"]["hud"]
            check(block["pixels_left"] == 64 * rep["frames"] and block["pixels_right"] == 64 * rep["frames"] and block["pixels_picture"] == 0 and actors["covered"] == rep["frames"] and hud["offered"] > 0 and block["rejected"] == 0,
                  f"{route}: F15 coverage credits `actors` for the pixels that reached the left and right margins (and none in the picture): {block} {actors}")
        check(results["luau"][0] == results["native"][0] and results["luau"][1] == results["native"][1] and results["luau"][3]["virtual_sprites"] == results["native"][3]["virtual_sprites"] and results["luau"][3]["scenes"] == results["native"][3]["scenes"],
              "the Luau and native routes present the IDENTICAL surface (HUD, margins) and report identical accounting and guest state")

        # ---- E: replay, save / restore, rewind ----
        for route in ("luau", "native"):
            program = tmp / route
            ident = luau_id if route == "luau" else native_id
            first = results[route]
            again = play(program, f"f28-{route}-replay")
            check(again[0] == first[0] and again[1] == first[1] and again[3] == first[3], f"{route}: a replay reproduces the surface, the guest state and the accounting")
            save = tmp / f"{route}-save"
            _, _, ls, _ = play(program, f"f28-{route}-save", "0 C\n8 CK\n9 C\n", save=save, identity=ident)
            check(ls["VIEWER_STATE"][0]["status"] == "ok", f"{route}: a quick save succeeds with the mod and its sprites in flight")
            surf, ram, ll, rep = play(program, f"f28-{route}-load", "0 C\n2 CM\n3 C\n", save=save, identity=ident)
            check(ll["VIEWER_STATE"][0]["status"] == "ok" and ram == derived_ram(900, 0) and ll["MOD_SUMMARY"][0]["write_digest"] == WRITE_DIGEST, f"{route}: a NEW process restores the state: the identical guest state and replay witness (mod state included)")
            want, _, _ = model_wide(base, side, SPRITES)
            check(all(surf[y][x] == want[y][x] for y in range(H) for x in range(320 + 2 * side) if outside_hud(x, y)) and rep["virtual_sprites"]["pixels_left"] > 0, f"{route}: after the restore the margin sprites are drawn again, for the current frame only")
            surf, ram, lr, rep = play(program, f"f28-{route}-rewind", "0 C\n14 CY\n15 C\n", save=tmp / f"{route}-rewind", identity=ident)
            check([(e["event"], e["status"]) for e in lr.get("VIEWER_STATE", [])] == [("rewind", "ok")] and ram == derived_ram(900, 0) and lr["MOD_SUMMARY"][0]["write_digest"] == WRITE_DIGEST and surf[154][side - 60 + 3] == rgb(AUTHORED_PALETTE[1]),
                  f"{route}: a scripted rewind keeps the effect, the witness and the margin sprites")

        # ---- F: fail-closed negatives, each through the same programs' machinery ----
        bad_read = 'if frame == 40 then local v = sega.mem.read("m68k.work_ram", 0x20, 2) end\n'
        luau_bad, bad_id = pack(luau_project("f28-luau-read", lambda s: s.replace("function post_input(frame: number)\n", "function post_input(frame: number)\n  " + bad_read, 1)))
        r = h.build(rom, tmp / "luau-read", luau_bad, bad_id)
        check(r.returncode == 0, "the unauthorized-read mod builds (the violation is a run-time fault)")
        surf, ram, lines, rep = play(tmp / "luau-read", "f28-luau-read")
        ms = lines["MOD_SUMMARY"][0]
        check(ms["faulted"] is True and "cause=range" in ms["fault"] and 0 < ms["committed_writes"] < 91 and ram == derived_ram(10 * (ms["committed_writes"] - 1), 0),
              f"Luau: a read outside the declared range faults only the mod: the bonuses committed before the fault stay (the guest clears its score word once at start-up, hence one write fewer than the writes committed), no later write happens, the guest runs to its stop: {ms['fault']} {ms['committed_writes']}")
        check(ms["faulted"] is True and surf[154][side - 60 + 3] == base[154][side - 60 + 3], "...and a disabled mod draws nothing in the margins")
        native_bad, nbad_id = pack(native_project("f28-native-read", lambda s: s.replace("    uint32_t pad = 0U, score = 0U;", "    uint32_t pad = 0U, score = 0U, probe = 0U;\n    if (host->frame == 40U) (void)host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x20U, 2U, &probe);", 1)))
        r = h.build(rom, tmp / "native-read", native_bad, nbad_id, native=True)
        check(r.returncode == 0, "the native unauthorized-read provider builds")
        surf, ram, lines, rep = play(tmp / "native-read", "f28-native-read")
        ms = lines["MOD_SUMMARY"][0]
        check(ms["faulted"] is True and "cause=range" in ms["fault"] and 0 < ms["committed_writes"] < 91 and ram == derived_ram(10 * (ms["committed_writes"] - 1), 0) and surf[154][side - 60 + 3] == base[154][side - 60 + 3],
              f"native: the same read is the same range fault; nothing is drawn afterwards: {ms['fault']}")
        # malformed descriptor and out-of-range geometry: whole-set rejections with the stable reasons; the HUD (a separate output) is unaffected
        for label, edit, reason in (("size", lambda s: s.replace('"above_all", 360, 150, 1, 1, 2, 0, 0)', '"above_all", 360, 150, 5, 1, 2, 0, 0)'), SIZE), ("position", lambda s: s.replace('"above_all", 360, 150, 1, 1, 2, 0, 0)', '"above_all", 2000, 150, 1, 1, 2, 0, 0)'), POSITION)):
            pkg, ident = pack(luau_project("f28-bad-" + label, edit))
            r = h.build(rom, tmp / ("bad-" + label), pkg, ident)
            check(r.returncode == 0, f"the {label} package builds")
            surf, ram, lines, rep = play(tmp / ("bad-" + label), "f28-bad-" + label)
            check(rep["last_vsprite_reason"] == reason and rep["virtual_sprites"]["rejected"] == rep["frames"] and surf[154][side - 60 + 3] == base[154][side - 60 + 3] and ram == derived_ram(900, 0),
                  f"a {label} violation rejects the whole set (reason {reason}): no sprite in either margin, the gameplay effect untouched: {rep['virtual_sprites']}")
        # an identity that was not approved never builds, on either route
        r1 = h.build(rom, tmp / "refused-luau", luau_pkg, "00" * 32)
        r2 = h.build(rom, tmp / "refused-native", native_pkg, "00" * 32, native=True)
        check(r1.returncode != 0 and r2.returncode != 0 and not (tmp / "refused-luau/game").exists() and not (tmp / "refused-native/game").exists(), "an identity that is not the approved package's is refused on both routes: no executable")

        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_f28_integration_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
