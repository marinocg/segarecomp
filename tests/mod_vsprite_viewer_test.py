#!/usr/bin/env python3
"""SEG-140-T008 (ADR 0306): live virtual sprites in the REAL generated-native SDL3 viewer program, authored through the ABI 1 native provider seam and through Luau (`sega.sprite.*`, both Luau routes of
the played program). A FABRICATED runnable fixture image (tests/tools/vsprite_fixture_rom.py: a plane with low- and high-priority pixels, 24 hardware sprites on one line; no commercial input) is built
into Vanilla and modded programs by the unmodified product and run under the real viewer (dummy video/audio, explicit --instruction-budget, wall-clock timeouts).

The expected pixels come from an INDEPENDENT golden model written in this file (pattern, flip, palette decoding, native priority classes, the original per-line budget) applied to the Vanilla frame: nothing
is derived from the compositor under test. Proves: authored art is independent of the guest's VRAM and verified art is the frame's own VRAM and palette; a verified set without the host's current-state
gate is rejected as a whole (UNVERIFIED_STATE) on every route; every invalid command fails closed with the stable F14 reason and draws NOTHING (a valid sibling included); the three native priorities, flips,
a column-major block, clip rectangles and both budget modes match the model; a set never outlives its frame; the coverage report distinguishes submitted, composed, rejected and presented and credits `actors`
only for sprites that really reached the presented frame (fully transparent, fully clipped, occluded, budget-dropped and out-of-range sets deliver nothing); runs are deterministic; Vanilla is untouched.
usage: mod_vsprite_viewer_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402

W, H = 320, 224
FIXTURE_PALETTE = (0x0420, 0x00E0, 0x000E, 0x0E00)
AUTHORED_PALETTE = (0x0000, 0x0E0E, 0x00EE)
UNVERIFIED, TOO_MANY, PROVENANCE, SIZE, TILE_RANGE, PALETTE, LAYER, POSITION, CLIP, RESERVED, BUDGET_MODE = 3, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11
BEHIND, BETWEEN, ABOVE = 0, 1, 2


# ---------------------------------------------------------------- the independent golden model
def rgb(word):
    return ((word >> 1 & 7) * 255 // 7, (word >> 5 & 7) * 255 // 7, (word >> 9 & 7) * 255 // 7)


def solid(c):
    return [[c] * 8 for _ in range(8)]


def checker(a, b):
    return [[a if (x + y) % 2 == 0 else b for x in range(8)] for y in range(8)]


def corners():
    p = [[0] * 8 for _ in range(8)]
    p[0][0], p[7][7] = 1, 2
    return p


FIXTURE_TILES = {1: solid(1), 2: checker(1, 2), 3: solid(3)}
POOL_TILES = {1: solid(1), 2: solid(2), 3: corners(), 6: solid(1), 7: solid(2), 8: checker(1, 2), 9: solid(1)}


def native_class(x, y):
    """The fixture's native priority class of picture pixel (x, y): plane A pattern 1 on cell rows 0 (low) and 1 (high); 20 low-priority hardware sprite cells on rows 96-103 (x < 160)."""
    if y < 8:
        return "low"
    if y < 16:
        return "high"
    if 96 <= y < 104 and x < 160:
        return "low"
    return "backdrop"


def hw_row_sprites(y):
    return 20 if 96 <= y < 104 else 0


def model(base, sprites, budget="limit"):
    """The expected picture: `base` (the Vanilla frame as rows of RGB) with `sprites` composed. Each sprite: dict(src='authored'|'verified', layer, x, y, w, h, tile, pal, hflip, vflip, clip=(x,y,w,h)|None).
    Returns (picture, drawn_authored, drawn_verified). First command wins; the tests avoid overlapping sprites except where stated."""
    out = [row[:] for row in base]
    claimed = set()
    per_row = {}
    drawn = {"authored": 0, "verified": 0}
    for s in sprites:
        tiles = FIXTURE_TILES if s["src"] == "verified" else POOL_TILES
        palette = FIXTURE_PALETTE if s["src"] == "verified" else AUTHORED_PALETTE
        for py in range(s["h"] * 8):
            y = s["y"] + py
            if not 0 <= y < H:
                continue
            if budget == "limit":   # the original per-line sprite limit (20 in a 40-cell picture), hardware sprites first; a command that finds no slot on a row is dropped on that row
                if hw_row_sprites(y) + per_row.get(y, 0) >= 20:
                    continue
            per_row[y] = per_row.get(y, 0) + 1
            for px in range(s["w"] * 8):
                x = s["x"] + px
                if not 0 <= x < W or (x, y) in claimed:
                    continue
                if s["clip"] is not None:
                    cx, cy, cw, ch = s["clip"]
                    if not (cx <= x < cx + cw and cy <= y < cy + ch):
                        continue
                sx, sy = (s["w"] * 8 - 1 - px) if s.get("hflip") else px, (s["h"] * 8 - 1 - py) if s.get("vflip") else py
                cell = (sx // 8) * s["h"] + sy // 8   # column-major cell order, exactly like a hardware sprite
                idx = tiles[s["tile"] + cell][sy % 8][sx % 8] if (s["tile"] + cell) in tiles else 0
                if idx == 0:
                    continue
                cls = native_class(x, y)
                if (s["layer"] == BEHIND and cls != "backdrop") or (s["layer"] == BETWEEN and cls == "high"):
                    continue
                out[y][x] = rgb(palette[idx])
                claimed.add((x, y))
                drawn[s["src"]] += 1
    return out, drawn["authored"], drawn["verified"]


def sp(src="authored", layer=ABOVE, x=0, y=0, w=1, h=1, tile=1, pal=0, hflip=False, vflip=False, clip=None):
    return dict(src=src, layer=layer, x=x, y=y, w=w, h=h, tile=tile, pal=pal, hflip=hflip, vflip=vflip, clip=clip)


TOAST = (200, 0, 320, 40)   # picture pixels where the host's once-per-change notice ("Presentation: ...") appears (x0, y0, x1, y1); the sprites of these tests are never placed there


def outside_toast(x, y):
    return not (TOAST[0] <= x < TOAST[2] and TOAST[1] <= y < TOAST[3])


def mismatches(img, want):
    return sum(1 for y in range(H) for x in range(W) if outside_toast(x, y) and img[y][x] != want[y][x])


def toast_shown(img, base):
    return any(img[y][x] != base[y][x] for y in range(TOAST[1], TOAST[3]) for x in range(TOAST[0], TOAST[2]))


def picture(bmp):
    """The 320 x 224 picture from a 960 x 672 window capture (an exact 3x picture in Original mode): the centre sample of every source pixel."""
    return [[bmp[3 * y + 1][3 * x + 1] for x in range(W)] for y in range(H)]


# the expected sprite lists of the named cases (also what the provider / scripts submit)
CASES = {
    "authored": ([sp(x=200, y=150, tile=1), sp(x=216, y=150, tile=2)], "limit"),
    "both": ([sp(x=200, y=150, tile=2), sp("verified", x=216, y=150, tile=2)], "limit"),
    "layers": ([sp(layer=BEHIND, x=20, y=12), sp(layer=BETWEEN, x=60, y=4), sp(layer=ABOVE, x=100, y=4)], "limit"),
    "budget_limit": ([sp(x=200, y=96), sp(x=200, y=150)], "limit"),
    "budget_extended": ([sp(x=200, y=96), sp(x=200, y=150)], "extended"),
    "flip": ([sp(x=40, y=150, tile=3), sp(x=60, y=150, tile=3, hflip=True), sp(x=80, y=150, tile=3, vflip=True), sp(x=100, y=150, tile=3, hflip=True, vflip=True), sp(x=140, y=150, w=2, h=2, tile=6)], "limit"),
    "clip": ([sp(x=200, y=150, w=2, tile=1, clip=(204, 150, 8, 4))], "limit"),
    "transparent": ([sp(x=200, y=150, tile=5)], "limit"),
    "fully_clipped": ([sp(x=200, y=150, tile=1, clip=(0, 0, 4, 4))], "limit"),
    "occluded": ([sp(layer=BEHIND, x=20, y=0)], "limit"),
    "limit_dropped": ([sp(x=200, y=96)], "limit"),
}
REJECTED = {"bad_size": SIZE, "bad_provenance": PROVENANCE, "bad_tile": TILE_RANGE, "bad_palette": PALETTE, "bad_layer": LAYER, "bad_position": POSITION, "bad_clip": CLIP, "bad_reserved": RESERVED,
            "bad_budget": BUDGET_MODE, "bad_too_many": TOO_MANY, "verified_gated": UNVERIFIED}


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_vsprite_viewer_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    spec = importlib.util.spec_from_file_location("vsprite_fixture", root / "tests/tools/vsprite_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    sps = importlib.util.spec_from_file_location("segapatch", root / "tools" / "segapatch.py")
    segapatch = importlib.util.module_from_spec(sps)
    sps.loader.exec_module(segapatch)
    provider_source = (root / "tests/fixtures/mod_vsprite/provider_abi1.c").read_bytes()
    with tempfile.TemporaryDirectory(prefix="segarecomp-vsprite-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "vsprite.rom"
        rom.write_bytes(image)
        image_sha = sha(image)

        def play(out, name, case=None, extra=None):
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            env = {"SEGARECOMP_COVERAGE_REPORT": str(report)}
            if case is not None:
                env["VSPRITE_CASE"] = case
            env.update(extra or {})
            run, lines, frame, _ = h.play(out, name, None, extra=env)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: the program runs to its own guest stop: {run.stderr[-200:]}")
            return picture(read_bmp(frame)) if frame.is_file() else None, (json.loads(report.read_text()) if report.is_file() else None), lines

        # ---- Vanilla ----
        r = h.build(rom, tmp / "vanilla")
        check(r.returncode == 0, "Vanilla builds: " + r.stdout[-300:])
        base, rep_v, _ = play(tmp / "vanilla", "vanilla")
        check(base[1][1] == (0, 255, 0) and base[100][4] == (0, 0, 255) and base[100][170] == rgb(FIXTURE_PALETTE[0]) and base[12][50] == (0, 255, 0), "the Vanilla frame is the fixture's: plane rows, 20 hardware sprite cells, backdrop")
        check(rep_v is not None and "virtual_sprites" not in rep_v, "a Vanilla report carries no virtual-sprite block")

        def package(name, caps):
            out = tmp / (name + ".segapatch")
            segapatch.create(image, None, out, id_="vsprite-provider", name="VSprite", version="1.0.0", native_source=provider_source, native_caps=caps, author="Test")
            ident = json.loads(h.run_cli("patch", "inspect", out).stdout)["package_identity"]
            return out, ident

        def build_abi1(name, caps):
            pkg, ident = package(name, caps)
            r = h.run_cli("build", "--rom", rom, "--output", tmp / name, "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1",
                          "--sdl3-include", pathlib.Path(sdl) / "include", "--sdl3-lib", pathlib.Path(sdl) / "lib", "--test-frame-seam", "1", "--patch-package", pkg, "--patch-identity", ident)
            check(r.returncode == 0, f"{name} builds: {r.stdout[-300:]}{r.stderr[-300:]}")
            return tmp / name

        def vs(rep):
            return rep.get("virtual_sprites", {})

        def actors(rep):
            return rep["scenes"][-1]["components"]["actors"]

        # ---- ABI 1: the native provider seam ----
        # caps are capability NAMES for segapatch.create: overlay.rgba only (no game-state access) and overlay.rgba + frame.work_ram (the host's snapshot gate)
        abi_plain = build_abi1("abi1-plain", ["overlay.rgba"])
        abi_state = build_abi1("abi1-state", ["overlay.rgba", "frame.work_ram"])
        if h.failures:
            return 1
        none_img, rep, lines = play(abi_plain, "abi1-none", "none")
        check(none_img == base and "virtual_sprites" not in rep, "ABI 1, zero commands: the presented frame is the Vanilla frame and no virtual-sprite accounting appears (the existing fast path)")
        for name, (sprites, budget) in CASES.items():
            program = abi_state if name == "both" else abi_plain
            img, rep, _ = play(program, "abi1-" + name, name)
            want, d_auth, d_ver = model(base, sprites, budget)
            wrong = mismatches(img, want)
            check(wrong == 0, f"ABI 1 {name}: the presented frame equals the independent golden ({wrong} pixels differ)")
            check(toast_shown(img, base) == (d_auth + d_ver == 0), f"ABI 1 {name}: the host says so (a visible notice) exactly when the set delivered nothing")
            block = vs(rep)
            check(block.get("pixels_authored") == d_auth * rep["frames"] and block.get("pixels_verified", 0) == d_ver * rep["frames"] and block.get("sets") == rep["frames"],
                  f"ABI 1 {name}: authored {d_auth} / verified {d_ver} pixels per frame are reported separately and match the model: {block}")
            delivered = d_auth + d_ver > 0
            a = actors(rep)
            check((a["presented"] == rep["frames"]) == delivered and (block.get("composed_without_pixels") == rep["frames"]) == (not delivered) and block.get("rejected") == 0,
                  f"ABI 1 {name}: `actors` is presented exactly when pixels were drawn; a composed set with none is only 'composed_without_pixels': {a} {block}")
        # authored art is the pool's, verified art is the frame's (the 'both' case: pool pattern 2 is solid yellow, VRAM pattern 2 is the green/red checker)
        both, _, _ = play(abi_state, "abi1-both-again", "both")
        check(both[154][204] == rgb(AUTHORED_PALETTE[2]) and both[150][216] == rgb(FIXTURE_PALETTE[1]) and both[150][217] == rgb(FIXTURE_PALETTE[2]), "authored pixels are the pool's solid yellow; verified pixels are the guest's own checker in the guest's own palette")
        # the verified-state gate: without frame.work_ram the host can establish no current snapshot (and every other invalid set is rejected the same, whole)
        for case, reason in REJECTED.items():
            program = abi_plain
            img, rep, _ = play(program, "abi1-" + case, case)
            check(mismatches(img, base) == 0 and toast_shown(img, base), f"ABI 1 {case}: a rejected set draws NOTHING (the frame is the Vanilla frame, a valid sibling command included) and the host says so")
            check(rep["last_vsprite_reason"] == reason and vs(rep).get("rejected") == rep["frames"] and vs(rep).get("composed") == 0 and vs(rep).get("presented") == 0 and actors(rep)["presented"] == 0 and actors(rep)["rejected"] == rep["frames"],
                  f"ABI 1 {case}: rejected as a whole with reason {reason} on every frame, nothing presented, `actors` rejected: {rep['last_vsprite_reason']} {vs(rep)}")
        gated, rep, _ = play(abi_state, "abi1-verified-with-gate", "verified_gated")
        want, _, d_ver = model(base, [sp(x=200, y=150, tile=2), sp("verified", x=216, y=150, tile=2)])
        check(mismatches(gated, want) == 0 and vs(rep).get("pixels_verified", 0) > 0, "with the host's current-snapshot gate (frame.work_ram, valid snapshot) the same verified set is composed")
        # one-frame lifetime
        cap = tmp / "capture"
        cap.mkdir()
        play(abi_plain, "abi1-lifetime", "lifetime", {"SEGARECOMP_VIEWER_CAPTURE_DIR": str(cap), "SEGARECOMP_VIEWER_CAPTURE_FRAMES": "20,21,22,23"})
        shots = sorted(cap.glob("*.bmp"))
        check(len(shots) == 4, f"four frames were captured: {[p.name for p in shots]}")
        if len(shots) == 4:
            seen = [picture(read_bmp(p))[154][204] == rgb(AUTHORED_PALETTE[1]) for p in shots]
            check(seen in ([True, False, True, False], [False, True, False, True]), f"a set submitted for one frame is gone on the next (alternating submission): {seen}")
        # determinism
        again, rep_again, _ = play(abi_plain, "abi1-authored-2", "authored")
        first, rep_first, _ = play(abi_plain, "abi1-authored-3", "authored")
        check(again == first and rep_again == rep_first, "two runs give identical frames and identical reports")

        # ---- Luau: `sega.sprite.*` through BOTH Luau routes of the played program (the ABI 1 HUD seam for an overlay.rgba-only package; the ONE scheduler for a package that needs more) ----
        def hexpattern(p):
            return "".join(str(p[y][x]) for y in range(8) for x in range(8))

        def lua_for(sprites, budget="limit", pool=True, extra_head="", loop_count=None, scene=None, parity=False):
            lines = ["function render(frame: number)"]
            if parity:
                lines.append("  if frame % 2 ~= 0 then return end")
            if scene:
                lines.append(f'  sega.scene("{scene}")')
            lines.append(extra_head)
            if budget == "extended":
                lines.append('  sega.sprite.budget("extended")')
            if pool:
                for index, pat in sorted(POOL_TILES.items()):
                    lines.append(f'  sega.sprite.tile({index}, "{hexpattern(pat)}")')
                lines.append('  sega.sprite.palette(0, "' + "".join("%04X" % w for w in AUTHORED_PALETTE) + '")')
            names = {BEHIND: "behind_planes", BETWEEN: "between", ABOVE: "above_all"}
            for c in sprites:
                flags = (1 if c["hflip"] else 0) | (2 if c["vflip"] else 0)
                lines.append(f'  sega.sprite.add("{c["src"]}", "{names[c["layer"]]}", {c["x"]}, {c["y"]}, {c["w"]}, {c["h"]}, {c["tile"]}, {c["pal"]}, {flags})')
                if c["clip"] is not None:
                    lines.append("  sega.sprite.clip(%d, %d, %d, %d)" % c["clip"])
            if loop_count:
                lines.append(f'  for i = 1, {loop_count} do sega.sprite.add("authored", "above_all", 100, 150, 1, 1, 1, 0, 0) end')
            lines.append("end")
            return "\n".join(lines) + "\n"

        def luau_pkg(name, script, scheduled=False, geometry=""):
            d = tmp / (name + "-src")
            (d / "scripts").mkdir(parents=True)
            caps = '"input.read", "overlay.rgba"' if scheduled else '"overlay.rgba"'
            (d / "mod.toml").write_text(f'''[mod]
id = "vsprite-luau"
version = "0.1.0"
name = "VSprite"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "fixture"
sha256 = "{image_sha}"

[[source]]
kind = "luau-source"
path = "scripts/main.luau"

[capabilities]
required = [{caps}]
''' + geometry)
            (d / "scripts/main.luau").write_text(script)
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            return out, doc.get("package_identity")

        def build_luau(name, script, scheduled=False, geometry=""):
            pkg, ident = luau_pkg(name, script, scheduled, geometry)
            r = h.build(rom, tmp / name, pkg, ident)
            check(r.returncode == 0, f"{name} builds: {r.stdout[-300:]}{r.stderr[-300:]}")
            return tmp / name

        abi_images = {}
        for case in ("authored", "layers", "flip", "clip", "budget_limit", "budget_extended", "transparent"):
            abi_images[case] = play(abi_plain, "ref-" + case, case)
        route_programs = {}
        for route, scheduled in (("luau-hud-seam", False), ("luau-scheduler", True)):
            for case, (sprites, budget) in CASES.items():
                if case in ("occluded", "fully_clipped", "limit_dropped", "both") or (route == "luau-hud-seam" and case not in ("authored", "budget_extended")):
                    continue
                if route == "luau-scheduler" and case in ("budget_extended",):
                    continue
                program = build_luau(f"{route}-{case}", lua_for(sprites, budget), scheduled)
                img, rep, lines = play(program, f"{route}-{case}", None)
                want, d_auth, _ = model(base, sprites, budget)
                check(mismatches(img, want) == 0, f"{route} {case}: the presented frame equals the independent golden ({mismatches(img, want)} pixels differ)")
                ref_img, ref_rep, _ = abi_images.get(case) or play(abi_plain, "ref-" + case, case)
                check(img == ref_img and vs(rep) == vs(ref_rep) and actors(rep) == actors(ref_rep), f"{route} {case}: identical pixels, virtual-sprite counters and `actors` accounting to the ABI 1 native route")
                route_programs[(route, case)] = program
        check(("luau-hud-seam", "authored") in route_programs and ("luau-scheduler", "layers") in route_programs, "both Luau routes were built and run")
        check(b"scheduled-mod-host" in (route_programs[("luau-scheduler", "authored")] / "game").read_bytes() and b"luau-hud-host" in (route_programs[("luau-hud-seam", "authored")] / "game").read_bytes(),
              "the overlay.rgba-only package runs through the ABI 1 HUD seam; the package that needs more runs through the one scheduler")
        # invalid commands and the verified-state gate on Luau: the host's whole-set validation, the same stable reasons, nothing drawn
        for label, sprites, kwargs, reason in (
            ("verified", [sp(x=200, y=150, tile=2), sp("verified", x=216, y=150, tile=2)], {}, UNVERIFIED),
            ("size", [sp(x=100, y=150), sp(x=120, y=150, w=5)], {}, SIZE),
            ("palette", [sp(x=100, y=150), sp(x=120, y=150, pal=4)], {}, PALETTE),
            ("tile", [sp(x=100, y=150), sp(x=120, y=150, tile=2048)], {}, TILE_RANGE),
            ("position", [sp(x=100, y=150), sp(x=2000, y=150)], {}, POSITION),
            ("too-many", [], {"loop_count": 65}, TOO_MANY),
        ):
            for route, scheduled in (("luau-hud-seam", False), ("luau-scheduler", True)):
                if route == "luau-hud-seam" and label not in ("verified", "size"):
                    continue
                program = build_luau(f"{route}-bad-{label}", lua_for(sprites, **kwargs), scheduled)
                img, rep, _ = play(program, f"{route}-bad-{label}", None)
                check(mismatches(img, base) == 0 and toast_shown(img, base) and rep["last_vsprite_reason"] == reason and vs(rep).get("rejected") == rep["frames"] and vs(rep).get("presented") == 0,
                      f"{route} {label}: rejected as a whole with reason {reason}, nothing drawn, the host says so: {rep['last_vsprite_reason']} {vs(rep)}")
        # a script cannot reach guest or VDP data through this API: a capability-less package cannot even name it
        nocap = tmp / "nocap"
        shutil.copytree(tmp / "luau-hud-seam-authored-src", nocap)
        (nocap / "mod.toml").write_text((nocap / "mod.toml").read_text().replace('required = ["overlay.rgba"]', 'required = ["host.log"]'))
        r = h.run_cli("mod", "check", nocap)
        check(r.returncode != 0 and "E_LUAU_CAPABILITY" in r.stdout, f"sega.sprite needs the overlay.rgba capability: {r.stdout[:200]}")
        # one-frame lifetime on Luau
        life = build_luau("luau-lifetime", lua_for([sp(x=200, y=150)], parity=True), True)
        cap2 = tmp / "capture2"
        cap2.mkdir()
        play(life, "luau-lifetime", None, {"SEGARECOMP_VIEWER_CAPTURE_DIR": str(cap2), "SEGARECOMP_VIEWER_CAPTURE_FRAMES": "20,21,22,23"})
        shots = sorted(cap2.glob("*.bmp"))
        seen = [picture(read_bmp(q))[154][204] == rgb(AUTHORED_PALETTE[1]) for q in shots]
        check(len(shots) == 4 and seen in ([True, False, True, False], [False, True, False, True]), f"Luau: a set submitted for one frame is gone on the next: {seen}")
        # coverage against a declared scene: actors are covered only for sprites that really reached the presented frame, and only inside the declared range
        decl = '\n[geometry]\naspect_max = "32:9"\n\n[[scene]]\nid = "main"\ncomponents = ["actors"]\n'
        shown = build_luau("cov-shown", lua_for([sp(x=200, y=150)], scene="main"), True, decl)
        empty = build_luau("cov-empty", lua_for([sp(x=200, y=150, tile=5)], scene="main"), True, decl)
        _, rep, _ = play(shown, "cov-shown", None)
        main_scene = rep["scenes"][0]["components"]["actors"]
        check(rep["scenes"][0]["id"] == "main" and main_scene["declared"] is True and main_scene["covered"] == rep["frames"] and main_scene["presented"] == rep["frames"], f"a declared `actors` component is covered by sprites that reached the frame: {main_scene}")
        _, rep, _ = play(empty, "cov-empty", None)
        main_scene = rep["scenes"][0]["components"]["actors"]
        check(main_scene["covered"] == 0 and main_scene["missing"] == rep["frames"] and main_scene["presented"] == 0 and vs(rep).get("composed_without_pixels") == rep["frames"],
              f"a composed set with zero drawn pixels leaves a declared `actors` component missing: {main_scene} {vs(rep)}")
        narrow = build_luau("cov-narrow", lua_for([sp(x=200, y=150)], scene="main"), True, decl.replace("32:9", "4:3"))
        cfg = tmp / "wide.cfg"
        cfg.write_text("video.widescreen=enhanced_gameplay\nvideo.widescreen_side_style=raw\nvideo.widescreen_ratio=16:9\n")
        _, rep, _ = play(narrow, "cov-narrow", None, {"SEGARECOMP_VIEWER_SETTINGS": str(cfg)})
        main_scene = rep["scenes"][0]["components"]["actors"]
        check(rep["outside_range_frames"] == rep["frames"] and main_scene["covered"] == 0 and main_scene["presented"] == 0 and vs(rep).get("presented") == 0 and vs(rep).get("pixels_authored") == 0,
              f"outside the declared range the native picture is presented alone: nothing is covered, no pixel is counted as presented: {main_scene} {vs(rep)}")
        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_vsprite_viewer_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
