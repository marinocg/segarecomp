#!/usr/bin/env python3
"""W04 gap B: game-art virtual sprites follow the frame's OWN art and are never served stale. A FABRICATED animated fixture (tests/tools/vsprite_fixture_rom.py, animate=True) rewrites pattern 2 and CRAM colour 2 in every vertical blank
(checker/red <-> solid/cyan) and shows pattern 2 on hardware sprite 0, so the frame's native picture states which art the frame has. A native ABI 2 provider granted frame.work_ram submits GAME_STATE_VERIFIED sprites of pattern 2
(in the picture, beyond the left and beyond the right hardware limit of the 32:9 presentation). Proves, frame by frame, that every virtual block equals the native hardware block of the SAME frame (animated pattern AND palette), that the
art really changes across frames, and, across a save and a restore by a NEW process and a scripted rewind, that no frame ever shows a virtual block that differs from its own frame's art (a refused set draws nothing).
usage: mod_vsprite_state_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import *  # noqa: E402,F401,F403
from mod_vsprite_abi2_test import c_source, cfg_text  # noqa: E402

FRAMES = ",".join(str(i) for i in range(4, 20))


def block(surface, x, y):
    return tuple(tuple(surface[y + j][x + i] for i in range(8)) for j in range(8))


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_vsprite_state_test: skipped (the generated-native build is exercised on Linux)")
        return 0
    spec = importlib.util.spec_from_file_location("vsprite_fixture", root / "tests/tools/vsprite_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    side = side_of(32, 9)
    with tempfile.TemporaryDirectory(prefix="segarecomp-vsprite-state-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom(animate=True)
        rom = tmp / "animated.rom"
        rom.write_bytes(image)
        image_sha = sha(image)
        sprites = [sp("verified", x=200, y=150, tile=2), sp("verified", x=-24, y=150, tile=2), sp("verified", x=330, y=150, tile=2)]
        d = tmp / "src"
        (d / "src").mkdir(parents=True)
        (d / "mod.toml").write_text(f"""[mod]
id = "vsprite-state"
author = "test"
version = "0.1.0"
name = "VSprite state"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "fixture"
sha256 = "{image_sha}"

[[source]]
kind = "native-c11"
path = "src/provider.c"

[abi]
native = 2

[capabilities]
required = ["overlay.rgba", "frame.work_ram", "state.mod"]

[state]
schema = 1
""")
        source = c_source(sprites, "limit", "presentation").replace("{sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, \"vsprite-abi2\", 1U, provider_init, provider_phase, NULL, NULL, NULL,",
                                                                    "{sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, \"vsprite-abi2\", 1U, provider_init, provider_phase, NULL, state_save, state_load,")
        source = source.replace("static const SegaModProvider provider", "static int state_save(void *s, uint8_t *o, uint32_t cap, uint32_t *len) { (void)s; if (cap < 1U) return 1; o[0] = 0U; *len = 1U; return 0; }\n"
                                "static int state_load(void *s, const uint8_t *i, uint32_t len) { (void)s; (void)i; return len == 1U ? 0 : 1; }\nstatic const SegaModProvider provider", 1)
        (d / "src/provider.c").write_text(source)
        pkg = tmp / "state.segapatch"
        r = h.run_cli("mod", "pack", d, "--out", pkg)
        doc = json.loads(r.stdout)
        check(r.returncode == 0 and doc.get("ok"), f"pack: {r.stdout[:300]}")
        r = h.build(rom, tmp / "program", pkg, doc.get("package_identity"), native=True)
        check(r.returncode == 0, f"builds: {r.stdout[-300:]}{r.stderr[-300:]}")
        r = h.build(rom, tmp / "vanilla")
        check(r.returncode == 0, "Vanilla builds")
        if h.failures:
            return 1
        (tmp / "cfg.cfg").write_text(cfg_text("32:9", "raw"))

        def shots(name, pad=None, save=None, frames=FRAMES, program="program"):
            out = tmp / ("cap-" + name)
            out.mkdir(exist_ok=True)
            env = {"SEGARECOMP_VIEWER_SETTINGS": str(tmp / "cfg.cfg"), "SEGARECOMP_VIEWER_CAPTURE_DIR": str(out), "SEGARECOMP_VIEWER_CAPTURE_FRAMES": frames}
            run, lines, _, _ = h.play(tmp / program, name, pad, save=save, identity=doc["package_identity"], rom_sha=image_sha, extra=env)
            frames_ = {int(q.stem.split("-")[1]): surface_from_bmp(read_bmp(q), side) for q in sorted(out.glob("frame-*.bmp"))}
            return frames_, lines

        backdrop = rgb(0x0420)
        want_a = tuple(tuple(rgb(0x00E0) if (i + j) % 2 == 0 else rgb(0x000E) for i in range(8)) for j in range(8))   # checker of colours 1 and 2 (red)
        want_b = tuple(tuple(rgb(0x0EE0) for _ in range(8)) for _ in range(8))                                            # solid colour 2 (cyan)
        got, lines = shots("live")
        check(len(got) >= 12 and lines["MOD_SUMMARY"][0]["faulted"] is False, f"live run: {len(got)} frames captured, no fault")
        seen = set()
        for n, surf in sorted(got.items()):
            hw = block(surf, side + 0, 96)
            seen.add(hw)
            virtual = [block(surf, side + 200, 150), block(surf, side - 24, 150), block(surf, side + 330, 150)]
            check(all(v == hw for v in virtual), f"frame {n}: the picture, left-margin and right-margin game-art sprites show exactly this frame's own pattern and palette")
            check(hw in (want_a, want_b), f"frame {n}: the art is one of the game's two animation states (pattern and colour)")
        check(seen == {want_a, want_b}, f"the art changes across frame boundaries (pattern AND palette): {len(seen)} distinct states")
        again, _ = shots("live2")
        check(again == got, "replay: identical frames")
        # a save, then a NEW process loads it, then a scripted rewind: a refused submission draws nothing, a served one is exactly the frame's art
        save = tmp / "save"
        _, ls = shots("save", "0 -\n10 K\n11 -\n", save=save, frames="12")
        check(ls["VIEWER_STATE"][0]["status"] == "ok", "quick save succeeds")
        loaded, ll = shots("load", "0 -\n2 M\n3 -\n", save=save, frames=",".join(str(i) for i in range(2, 14)))
        check(ll["VIEWER_STATE"][0]["status"] == "ok" and ll["MOD_SUMMARY"][0]["faulted"] is False, "a NEW process restores the state without a fault")
        stale = absent = 0
        for n, surf in sorted(loaded.items()):
            hw = block(surf, side + 0, 96)
            for v in (block(surf, side + 200, 150), block(surf, side - 24, 150), block(surf, side + 330, 150)):
                if v == hw:
                    continue
                if all(px == backdrop for row in v for px in row):
                    absent += 1
                else:
                    stale += 1
        check(stale == 0, f"after a restore no game-art sprite ever shows art that is not its own frame's ({stale} stale blocks, {absent} refused blocks drawn as nothing)")
        rewound, lr = shots("rewind", "0 -\n14 Y\n15 -\n", save=tmp / "rw", frames=",".join(str(i) for i in range(10, 30)))
        stale = sum(1 for surf in rewound.values() for v in (block(surf, side + 200, 150), block(surf, side - 24, 150), block(surf, side + 330, 150))
                    if v != block(surf, side + 0, 96) and not all(px == backdrop for row in v for px in row))
        check(lr["MOD_SUMMARY"][0]["faulted"] is False and stale == 0, f"a scripted rewind leaves no stale game-art sprite ({[(e['event'], e['status']) for e in lr.get('VIEWER_STATE', [])]}, {stale} stale)")
        van, _ = shots("vanilla", program="vanilla")
        check(all(block(s, side + 200, 150) == tuple(tuple(backdrop for _ in range(8)) for _ in range(8)) for s in van.values()), "Vanilla draws no virtual sprite")
        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_vsprite_state_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
