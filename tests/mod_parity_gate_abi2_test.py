#!/usr/bin/env python3
"""SEG-152-T008 (Exit C), the viewer-level half of the ABI 1 / ABI 2 parity gate (its producer-level half is abi_parity_gate_tests, which extends the T001 harness): the FINAL presented image of an ABI 1 provider and an ABI 2
twin written to the same logic, through the real SDL3 viewer and its one compositor, on the FABRICATED mid-frame fixture (no commercial image), built as the two package generations of the same game:
  schema 2   `segapatch.create`: a BPS payload (the effective image) plus the ABI 1 C11 twin (tests/fixtures/mod_parity/twin_abi1.c);
  schema 3   `segarecomp mod pack` of a project with a `bps-image` and a `native-c11` source (tests/fixtures/mod_parity/provider.c), abi.minor 2, capabilities frame.vdp, frame.work_ram, overlay.rgba, scenery.words.
Proved: (1) the schema 3 package is accepted by the packager with exactly those capabilities, minor 2 and the two payload kinds, and `pack` is byte-deterministic; the schema 2 package of the same game still inspects and
builds unchanged; (2) over the variants serve / per-row generic rows / refused HUD / refused frame and the modes 32:9 with an asymmetric placement, 16:9, Original and exact 4:3, the presented frame of the ABI 2 package equals
the ABI 1 twin's byte for byte and matches a committed golden digest (synthetic content only, exact equality); (3) falsification: a stale scroll, a skipped callback, a missing refusal and an oversized bitmap each leave the
ABI 2 frame different from the ABI 1 twin's (the gate names the failing case). The existing viewport suites run unmodified and are part of the same CI job.
usage: mod_parity_gate_abi2_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import hashlib
import importlib.util
import json
import os
import pathlib
import sys
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mod_exact_frame_abi2_test import ENHANCED, PAD  # noqa: E402
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402

CAPABILITIES = ["frame.vdp", "frame.work_ram", "overlay.rgba", "scenery.words"]
# variant -> (DECLINE, MASK_FROM, LEFT_DIV): serve; per-row generic rows from row 100 (the ABI 1 own_plane shape); the HUD refused (BAD_STATE); the whole frame refused (unsupported scene)
VARIANTS = {"serve": (0, 224, 4), "rows": (0, 100, 4), "refuse-hud": (1, 224, 4), "refuse-all": (2, 224, 4)}
# mode -> (aspect, video schedule): 32:9 with an asymmetric placement, 16:9, Original (10:orig) and exact 4:3 (10:4:3)
MODES = {"wide": ("32:9", None), "w169": ("16:9", None), "orig": ("32:9", "10:orig"), "43": ("32:9", "10:4:3")}
# Golden digests of the presented frame (sha256 over the RGB rows) of the synthetic fixture, exact equality. `--print-goldens` prints them.
GOLDEN = {
    "refuse-all:43": "b09683a3e8a6da4af582181a5a415878dce57186269028fceaf9d5136d898e16",
    "refuse-all:orig": "b09683a3e8a6da4af582181a5a415878dce57186269028fceaf9d5136d898e16",
    "refuse-all:w169": "0f27bb51d386c9cd2a118202b494023c6e99b384cfc052ce0aef9f178c0d5e03",
    "refuse-all:wide": "beb9ae2f545cdd28329044037e37deae067202e3301060bd3378b63e2b269a53",
    "refuse-hud:43": "b09683a3e8a6da4af582181a5a415878dce57186269028fceaf9d5136d898e16",
    "refuse-hud:orig": "b09683a3e8a6da4af582181a5a415878dce57186269028fceaf9d5136d898e16",
    "refuse-hud:w169": "3b3caaacb5ae8c52e6c04c0948ff92ea6d0447a465a9a67aa3c1608ae4355b8a",
    "refuse-hud:wide": "367d72c823d8595c0b69cf3a53775f1ec60a855903f07bf89cb080f3bfcd0dc2",
    "rows:43": "6c665450383283934443f3f7602cfe5572624381fa9e260f9af4f8ad8fc73cf3",
    "rows:orig": "6c665450383283934443f3f7602cfe5572624381fa9e260f9af4f8ad8fc73cf3",
    "rows:w169": "d81412967afe07cae1df029294c65a80b60bc78ea42763e80f3f4b3bee2bb6fd",
    "rows:wide": "33c9480fe1beb5747fbfe102647df2853889c45dc4d70171edbe8a8395acf604",
    "serve:43": "6c665450383283934443f3f7602cfe5572624381fa9e260f9af4f8ad8fc73cf3",
    "serve:orig": "6c665450383283934443f3f7602cfe5572624381fa9e260f9af4f8ad8fc73cf3",
    "serve:w169": "574134f2ddaed09ffc0304947f9bee7c8b1699201533079b770841ef7912024f",
    "serve:wide": "b0e58d8040b4b7c51b551ac6248b89b21ecd9649da2a82dd86adca4f19da14bd",
}


def frame_digest(bmp):
    return hashlib.sha256(b"".join(bytes(c for px in row for c in px) for row in bmp)).hexdigest()


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    printing = "--print-goldens" in sys.argv
    if sys.platform != "linux":
        print("mod_parity_gate_abi2_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    pdir = root / "tests/fixtures/mod_parity"
    bdir = root / "tests/fixtures/mod_bitmap"
    fixture = load(fixtures / "scenery_fixture_rom.py", "scenery_fixture")
    segapatch = load(root / "tools" / "segapatch.py", "segapatch")
    keep = os.environ.get("SEGARECOMP_TEST_KEEP")
    produced = {}
    with tempfile.TemporaryDirectory(prefix="segarecomp-parity-") as directory:
        tmp = pathlib.Path(keep or directory)
        tmp.mkdir(exist_ok=True)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom(mid_frame=True)
        effective = bytearray(image)
        effective[0x7F9] ^= 0x01   # one byte of unused padding (`bra.s *` words no program path reads): the BPS payload is real, the program is unchanged
        effective = bytes(effective)
        rom = tmp / "parity.rom"
        rom.write_bytes(image)
        effective_rom = tmp / "parity-effective.rom"   # a schema 2 build is given the image the launcher already patched (the package declares its BPS target); a schema 3 build is given the original and applies its bps-image
        effective_rom.write_bytes(effective)
        image_sha, effective_sha = sha(image), sha(effective)
        bps = segapatch.bps_create(image, effective)
        shared = (bdir / "bitmap_art.h").read_text()
        world = (pdir / "parity_world.h").read_text()

        def inline(text, variant):
            decline, mask, left = VARIANTS[variant]
            text = text.replace('#include "bitmap_art.h"', shared).replace('#include "parity_world.h"', world)
            return text.replace("@@DECLINE@@", str(decline)).replace("@@MASK_FROM@@", str(mask)).replace("@@LEFT_DIV@@", str(left))

        def abi2_project(name, variant, edit=None, bps_bytes=bps):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            (d / "data").mkdir()
            source = inline((pdir / "provider.c").read_text(), variant)
            source = edit(source) if edit else source
            (d / "src/provider.c").write_text(source)
            (d / "data/game.bps").write_bytes(bps_bytes)
            (d / "mod.toml").write_text((pdir / "mod.toml").read_text().replace("@@SHA@@", image_sha).replace("@@TARGET_SHA@@", effective_sha))
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:400]}")
            return d, out, doc

        def abi2_program(name, variant, edit=None):
            _, out, doc = abi2_project(name, variant, edit)
            built = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-400:]}{built.stderr[-300:]}")
            return tmp / name

        def abi1_program(name, variant):
            pkg = tmp / (name + ".segapatch")
            segapatch.create(image, effective, pkg, id_="parity-twin", name="Parity twin", version="1.0.0", native_source=inline((pdir / "twin_abi1.c").read_text(), variant).encode(),
                             native_caps=CAPABILITIES, author="Test")
            ident = json.loads(h.run_cli("patch", "inspect", pkg).stdout)["package_identity"]
            r = h.run_cli("build", "--rom", effective_rom, "--output", tmp / name, "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1", "--sdl3-include", pathlib.Path(sdl) / "include",
                          "--sdl3-lib", pathlib.Path(sdl) / "lib", "--test-frame-seam", "1", "--patch-package", pkg, "--patch-identity", ident)
            check(r.returncode == 0, f"{name} builds: {r.stdout[-300:]}{r.stderr[-300:]}")
            return tmp / name, pkg

        # ---- (1) the schema 3 package: accepted with the declared capabilities, minor 2 and both payload kinds, byte-deterministic; the schema 2 package of the same game is unchanged ----
        d3, pkg3, doc3 = abi2_project("pkg3", "serve")
        again = tmp / "pkg3-again.segapatch"
        r = h.run_cli("mod", "pack", d3, "--out", again)
        check(r.returncode == 0 and again.read_bytes() == pkg3.read_bytes(), "`mod pack` of the schema 3 project is byte-deterministic")
        ins = json.loads(h.run_cli("mod", "inspect", pkg3).stdout)
        with zipfile.ZipFile(pkg3) as z:
            manifest = json.loads(z.read("manifest.json"))
        check(ins.get("ok") is True and ins.get("trust_class") == "native" and ins.get("capabilities") == CAPABILITIES, f"the packager accepts the schema 3 package with capabilities {CAPABILITIES}: {json.dumps(ins)[:300]}")
        check(manifest.get("schema_version") == 3 and manifest.get("abi") == {"native": 2, "minor": 2} and [p.get("kind") for p in manifest["payloads"]] == ["bps-image", "native-c11"],
              f"schema 3, abi.minor 2 (ADR 0320 decision 10) and the payloads bps-image + native-c11: {manifest.get('abi')} {[p.get('kind') for p in manifest['payloads']]}")
        if h.failures:
            return 1
        twin_serve, pkg2 = abi1_program("abi1-serve", "serve")
        with zipfile.ZipFile(pkg2) as z:
            check(json.loads(z.read("manifest.json")).get("schema_version") == 2, "the ABI 1 package of the same game is still a schema 2 package")
        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        if h.failures:
            return 1

        def play(program, name, mode):
            aspect, schedule = MODES[mode]
            cfg = tmp / (name + ".cfg")
            cfg.write_text(ENHANCED % aspect)
            env = {"SEGARECOMP_VIEWER_SETTINGS": str(cfg), "SEGARECOMP_VIEWER_FINAL_FRAME_NO_NOTICES": "1"}
            if schedule:
                env["SEGARECOMP_VIEWER_TEST_VIDEO_AT"] = schedule
            run, lines, frame, _ram = h.play(program, name, PAD, rom_sha=image_sha, extra=env, budget=20_000_000)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: runs to its own guest stop: {run.stderr[-300:]}")
            return {"bmp": read_bmp(frame), "summary": (lines.get("MOD_SUMMARY") or [{}])[0]}

        # ---- (2) the matrix: every variant x mode, ABI 2 == ABI 1 twin byte for byte, equal to the committed golden ----
        vanilla = {m: play(tmp / "vanilla", f"vanilla-{m}", m) for m in MODES}
        for variant in VARIANTS:
            abi1 = twin_serve if variant == "serve" else abi1_program(f"abi1-{variant}", variant)[0]
            abi2 = abi2_program(f"abi2-{variant}", variant)
            if h.failures:
                return 1
            for mode in MODES:
                key = f"{variant}:{mode}"
                a, b = play(abi1, f"a1-{variant}-{mode}", mode), play(abi2, f"a2-{variant}-{mode}", mode)
                check(b["summary"].get("faulted") is False, f"{key}: the ABI 2 provider is healthy (a refusal is not a fault)")
                check(a["bmp"] == b["bmp"], f"{key}: the presented frame of the ABI 2 package equals the ABI 1 twin's byte for byte")
                digest = frame_digest(b["bmp"])
                produced[key] = digest
                if not printing:
                    check(GOLDEN[key] == digest, f"{key}: the presented frame equals its committed golden digest (got {digest})")
                nothing = variant == "refuse-all" or (variant == "refuse-hud" and mode in ("orig", "43"))   # a refused frame is generic; Original / 4:3 present no extension, so a refused HUD leaves nothing to show
                if nothing:
                    check(b["bmp"] == vanilla[mode]["bmp"], f"{key}: nothing of the provider is presented: Vanilla's generic frame")
                else:
                    check(b["bmp"] != vanilla[mode]["bmp"], f"{key}: the provider's output is really presented")
        # ---- (3) falsification: each mutant of the ABI 2 provider must differ from the ABI 1 twin of the logic it claims to reproduce ----
        mutants = [
            ("stale-scroll", "serve", "wide", lambda t: t.replace("camera = -hs / 8;", "camera = -hs / 8 - 20;")),   # windows placed from a scroll twenty cells (160 px) behind: the far right cells fall outside every window
            ("skipped-callback", "serve", "wide", lambda t: t.replace("/*@@SKIP@@*/", "return;")),
            ("oversized-bitmap", "serve", "wide", lambda t: t.replace("/*@@BIG@@*/", "(void)r->overlay_layer_bitmap(2U, 2U, 0U, 0, 0, 257U, 16U, 1U); (void)r->overlay_target(2U); (void)r->overlay_bitmap(0, 0, 257U, 16U, big_art, 1028U); (void)r->overlay_target(0U);")),
        ]
        expectations = {}
        for name, variant, mode, edit in mutants:
            program = abi2_program(f"mut-{name}", variant, edit)
            expectations[name] = (play(program, f"mut-{name}-{mode}", mode), play(twin_serve, f"mut-ref-{name}-{mode}", mode))
        for name, (bad, ref) in expectations.items():
            check(bad["bmp"] != ref["bmp"] or bad["summary"].get("faulted") is True, f"FALSIFY {name}: the gate fails the mutant (its frame differs from the ABI 1 twin's, or it faulted)")
        # a missing refusal: the provider ignores the decision the ABI 1 twin takes (DECLINE 0 where the twin refuses)
        for variant, ref_variant in (("serve", "refuse-all"), ("serve", "refuse-hud")):
            ref_prog = abi1_program(f"mutref-{ref_variant}", ref_variant)[0]
            bad = play(abi2_program(f"mut-norefuse-{ref_variant}", variant), f"mut-norefuse-{ref_variant}", "wide")
            check(bad["bmp"] != play(ref_prog, f"mutref-run-{ref_variant}", "wide")["bmp"], f"FALSIFY missing-refusal ({ref_variant}): the gate fails a provider that serves a frame the ABI 1 twin refuses")
        if printing:
            print("GOLDEN = {")
            for key in sorted(produced):
                print(f'    "{key}": "{produced[key]}",')
            print("}")
        if h.failures:
            return 1
    print("mod_parity_gate_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
