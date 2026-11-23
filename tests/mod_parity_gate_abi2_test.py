#!/usr/bin/env python3
"""SEG-152-T008 (Exit C), the viewer-level half of the ABI 1 / ABI 2 parity gate (its producer-level half is abi_parity_gate_tests, which extends the T001 harness): the FINAL presented image of an ABI 1 provider and an ABI 2
twin written to the same logic, through the real SDL3 viewer and its one compositor, on the FABRICATED mid-frame fixture (no commercial image), built as the two package generations of the same game:
  schema 2   `segapatch.create`: a BPS payload (the effective image) plus the ABI 1 C11 twin (tests/fixtures/mod_parity/twin_abi1.c);
  schema 3   `segarecomp mod pack` of a project with a `bps-image` and a `native-c11` source (tests/fixtures/mod_parity/provider.c), abi.minor 2, capabilities frame.vdp, frame.work_ram, overlay.rgba, scenery.words.
Proved: (1) the schema 3 package is accepted by the packager with exactly those capabilities, minor 2 and the two payload kinds, and `pack` is byte-deterministic; the schema 2 package of the same game still inspects and
builds unchanged; (2) over the variants serve / per-row generic rows / refused HUD / refused frame and the modes 32:9 with an asymmetric placement, 16:9, Original and exact 4:3, the presented frame of the ABI 2 package equals
the ABI 1 twin's byte for byte and matches a committed golden digest (synthetic content only, exact equality); (3) falsification: a stale scroll, a skipped callback, a missing refusal and an oversized bitmap each leave the
ABI 2 run different from the ABI 1 twin's for a named reason, an oversized bitmap faulting the callback with cause range and discarding everything staged; (4) per-frame producer verdicts (frame, status, side, accepted components, refusal reason, authored status, placement) and the coverage report's accounting equal frame by frame, with the one declared ADR 0315 difference (no placement can be requested when the view reports side 0), and a scripted transition run (32:9, 21:9, 16:9, Original, exact 4:3), its replay, a quick save + load and a rewind equal between the twins. The existing viewport suites run unmodified and are part of the same CI job.
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
MODES = {"wide": ("32:9", None), "w219": ("21:9", None), "w169": ("16:9", None), "orig": ("32:9", "10:orig"), "43": ("32:9", "10:4:3")}
# Golden digests of the presented frame (sha256 over the RGB rows) of the synthetic fixture, exact equality. `--print-goldens` prints them.
GOLDEN = {
    "refuse-all:w219": "494a30015d06708e2bbcd3d1b9139668eec855beeafbf262a7ed683f407f6b79",
    "refuse-hud:w219": "6a7d4c6c3e0fbbb0f0958cf2d18e6aaa4d5ffbd8afd920bd63664ef704777907",
    "rows:w219": "924b0f44614f39e04260c3539db49579c6ec607adeb7ea31cac5c2029eee9572",
    "serve:w219": "f41efa576f051d2a6f686ff08ad484c7d96c46e1e38ce840245530aa21772393",
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
        class Ctx:   # one fixture image with its effective (BPS-patched) image: the matrix runs on the short program (40 turns), the lifecycle on a long one (110 turns, like the exact-frame suite) so scripted transitions, a save, a load and a rewind all fit
            pass

        def make_ctx(tag, turns):
            c = Ctx()
            fixture.TURNS = turns
            c.image = fixture.make_rom(mid_frame=True)
            eff = bytearray(c.image)
            eff[0x7F9] ^= 0x01   # one byte of unused padding (`bra.s *` words no program path reads): the BPS payload is real, the program is unchanged
            c.effective = bytes(eff)
            c.rom = tmp / f"parity{tag}.rom"
            c.rom.write_bytes(c.image)
            c.effective_rom = tmp / f"parity-effective{tag}.rom"   # a schema 2 build is given the image the launcher already patched (the package declares its BPS target); a schema 3 build is given the original and applies its bps-image
            c.effective_rom.write_bytes(c.effective)
            c.image_sha, c.effective_sha = sha(c.image), sha(c.effective)
            c.bps = segapatch.bps_create(c.image, c.effective)
            c.tag = tag
            return c

        C = make_ctx("", 40)
        shared = (bdir / "bitmap_art.h").read_text()
        world = (pdir / "parity_world.h").read_text()

        ids = {}

        def inline(text, variant):
            decline, mask, left = VARIANTS[variant]
            text = text.replace('#include "bitmap_art.h"', shared).replace('#include "parity_world.h"', world)
            return text.replace("@@DECLINE@@", str(decline)).replace("@@MASK_FROM@@", str(mask)).replace("@@LEFT_DIV@@", str(left))

        def abi2_project(name, variant, edit=None, manifest=None):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            (d / "data").mkdir()
            source = inline((pdir / "provider.c").read_text(), variant)
            source = edit(source) if edit else source
            (d / "src/provider.c").write_text(source)
            (d / "data/game.bps").write_bytes(C.bps)
            (d / "mod.toml").write_text((pdir / "mod.toml").read_text().replace("@@SHA@@", C.image_sha).replace("@@TARGET_SHA@@", C.effective_sha) if manifest is None else manifest((pdir / "mod.toml").read_text().replace("@@SHA@@", C.image_sha).replace("@@TARGET_SHA@@", C.effective_sha)))
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:400]}")
            return d, out, doc

        def abi2_program(name, variant, edit=None, manifest=None):
            _, out, doc = abi2_project(name, variant, edit, manifest=manifest)
            built = h.build(C.rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-400:]}{built.stderr[-300:]}")
            ids[name] = doc.get("package_identity")
            return tmp / name

        def abi1_program(name, variant):
            pkg = tmp / (name + ".segapatch")
            segapatch.create(C.image, C.effective, pkg, id_="parity-twin", name="Parity twin", version="1.0.0", native_source=inline((pdir / "twin_abi1.c").read_text(), variant).encode(),
                             native_caps=CAPABILITIES, author="Test")
            ident = json.loads(h.run_cli("patch", "inspect", pkg).stdout)["package_identity"]
            r = h.run_cli("build", "--rom", C.effective_rom, "--output", tmp / name, "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1", "--sdl3-include", pathlib.Path(sdl) / "include",
                          "--sdl3-lib", pathlib.Path(sdl) / "lib", "--test-frame-seam", "1", "--patch-package", pkg, "--patch-identity", ident)
            check(r.returncode == 0, f"{name} builds: {r.stdout[-300:]}{r.stderr[-300:]}")
            ids[name] = ident
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
        check(h.build(C.rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        if h.failures:
            return 1

        def play(program, name, mode, schedule=None, pad=PAD, save=None, identity=None, sched_aspect=None):
            aspect, mode_schedule = MODES[mode]
            cfg = tmp / (name + ".cfg")
            cfg.write_text(ENHANCED % (sched_aspect or aspect))
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            env = {"SEGARECOMP_VIEWER_SETTINGS": str(cfg), "SEGARECOMP_VIEWER_FINAL_FRAME_NO_NOTICES": "1", "SEGARECOMP_VIEWER_TEST_TRACE": "1", "SEGARECOMP_COVERAGE_REPORT": str(report)}
            if schedule or mode_schedule:
                env["SEGARECOMP_VIEWER_TEST_VIDEO_AT"] = schedule or mode_schedule
            run, lines, frame, ram = h.play(program, name, pad, save=save, identity=identity, rom_sha=C.image_sha, extra=env, budget=20_000_000)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: runs to its own guest stop: {run.stderr[-300:]}")
            trace = {t["f"]: t for t in lines.get("VIEWER_TEST_TRACE", [])}
            cov = json.loads(report.read_text()) if report.is_file() else None
            scene = cov["scenes"][0]["components"] if cov and cov.get("scenes") else None
            return {"bmp": read_bmp(frame), "summary": (lines.get("MOD_SUMMARY") or [{}])[0], "trace": trace, "events": [tuple(t[k] for k in ("f",) + VERDICT) for t in lines.get("VIEWER_TEST_TRACE", [])], "coverage": scene, "lines": lines, "ram": ram.read_bytes() if ram.is_file() else b""}

        VERDICT = ("status", "side", "shift", "acc", "reason", "auth")

        def verdicts(run):
            return {f: tuple(t[k] for k in VERDICT) for f, t in run["trace"].items()}

        def diff_verdicts(a, b):
            va, vb = verdicts(a), verdicts(b)
            return [(f, va.get(f), vb.get(f)) for f in sorted(set(va) | set(vb)) if va.get(f) != vb.get(f)]

        ACCOUNT = ("offered", "accepted", "presented", "rejected")   # the coverage report's per-component accounting (declared/covered/requested depend on whether the package declares a geometry, which the schema 2 twin does not)
        OFF_FROM = 10   # SEGARECOMP_VIEWER_TEST_VIDEO_AT "10:orig" / "10:4:3": the first frame whose presenter shows no extension

        def compare_contract(key, a, b, mode, off=None):
            """Frame by frame, the producer's verdict of the ABI 1 twin and of the ABI 2 package must be equal: frame identity, status, side, accepted components, refusal reason, authored status and placement shift. The ONE declared
            intended difference (ADR 0315): on frames where the presenter shows no extension (Original, exact 4:3; `off` = [(first, last)] frame ranges, exclusive/inclusive) the ABI 2 view reports side 0, so a provider cannot request
            a placement: the ABI 2 `shift` is 0 where the ABI 1 twin, which is handed the planned side, requests one; the picture is identical because the presenter ignores it. Nothing else may differ."""
            if off is None:
                off = [(OFF_FROM, 10 ** 9)] if mode in ("orig", "43") else []
            is_off = lambda f: any(lo < f <= hi for lo, hi in off)  # noqa: E731
            si = VERDICT.index("shift")
            va, vb = verdicts(a), verdicts(b)
            check(sorted(va) == sorted(vb) and len(va) > 30, f"{key}: both packages publish a verdict for the same frames ({len(va)} / {len(vb)})")
            strict = [(f, va[f], vb.get(f)) for f in sorted(va) if not is_off(f) and va[f] != vb.get(f)]
            check(not strict, f"{key}: per-frame verdicts (status, side, shift, accepted components, reason, authored) equal frame by frame ({len(strict)} differ, first {strict[:2]})")
            late = [f for f in sorted(va) if is_off(f)]
            if off:
                odd = [(f, va[f], vb.get(f)) for f in late if vb.get(f) is None or va[f][:si] + va[f][si + 1:] != vb[f][:si] + vb[f][si + 1:] or vb[f][si] != 0]
                check(late and not odd, f"{key}: with no extension shown the verdicts equal the ABI 1 twin's except the placement shift, which the ABI 2 view cannot request (declared ADR 0315 difference; {len(odd)} other differences, first {odd[:2]})")
                moved = sum(1 for f in late if va[f][si] != vb[f][si])
                print(f"DECLARED DIFFERENCE {key}: the ABI 1 twin requested a placement the presenter ignored on {moved} of {len(late)} frames; the ABI 2 view (side 0) requested none; pictures equal")
            ca, cb = a["coverage"], b["coverage"]
            check(ca is not None and cb is not None and all({k: ca[c][k] for k in ACCOUNT} == {k: cb[c][k] for k in ACCOUNT} for c in ("scenery", "hud")), f"{key}: the coverage report's offered/accepted/presented/rejected accounting equals the ABI 1 twin's ({ca and ca['scenery']} / {cb and cb['scenery']})")

        def named_failures(ref, bad):
            """The named reasons a mutant's run differs from the ABI 1 reference run: per-frame verdict fields and the presented picture."""
            va, vb = verdicts(ref), verdicts(bad)
            names = set()
            for f in set(va) | set(vb):
                x, y = va.get(f), vb.get(f)
                if x is None or y is None:
                    names.add("frame-identity")
                    continue
                for i, label in enumerate(("status", "side", "placement", "accepted-components", "reason", "authored")):
                    if x[i] != y[i]:
                        names.add(label)
            if ref["bmp"] != bad["bmp"]:
                names.add("picture")
            return names

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
                compare_contract(key, a, b, mode)
                digest = frame_digest(b["bmp"])
                produced[key] = digest
                if not printing:
                    check(GOLDEN[key] == digest, f"{key}: the presented frame equals its committed golden digest (got {digest})")
                nothing = variant == "refuse-all" or (variant == "refuse-hud" and mode in ("orig", "43"))   # a refused frame is generic; Original / 4:3 present no extension, so a refused HUD leaves nothing to show
                if nothing:
                    check(b["bmp"] == vanilla[mode]["bmp"], f"{key}: nothing of the provider is presented: Vanilla's generic frame")
                else:
                    check(b["bmp"] != vanilla[mode]["bmp"], f"{key}: the provider's output is really presented")
        # ---- (3) falsification: each mutant of the ABI 2 provider must fail the differential for a NAMED reason (a per-frame verdict field or the picture), never merely "something faulted" ----
        def big(t):   # an oversized bitmap layer (257 > 256): the production range rule refuses the call, which FAULTS the callback (SEGA_MOD_E_RANGE) and discards everything it staged
            return t.replace("/*@@BIG@@*/", "(void)r->overlay_layer_bitmap(2U, 2U, 0U, 0, 0, 257U, 16U, 1U); (void)r->overlay_target(2U); (void)r->overlay_bitmap(0, 0, 257U, 16U, big_art, 1028U); (void)r->overlay_target(0U);")
        mutants = [
            ("stale-scroll", lambda t: t.replace("camera = -hs / 8;", "camera = -hs / 8 - 20;"), None, {"picture"}),   # windows placed from a scroll twenty cells (160 px) behind: the far right cells fall outside every window
            ("skipped-callback", lambda t: t.replace("/*@@SKIP@@*/", "return;"), None, {"accepted-components", "picture"}),
            ("oversized-bitmap", big, None, {"accepted-components", "picture"}),
        ]
        runs = {}
        for name, edit, manifest, want in mutants:
            program = abi2_program(f"mut-{name}", "serve", edit, manifest)
            bad, ref = play(program, f"mut-{name}-wide", "wide"), play(twin_serve, f"mut-ref-{name}-wide", "wide")
            runs[name] = bad
            names = named_failures(ref, bad)
            check(want <= names, f"FALSIFY {name}: the differential fails it for the named reasons {sorted(want)} (got {sorted(names)})")
        ob = runs["oversized-bitmap"]
        vwide = vanilla["wide"]
        logs = [m.get("line") for m in ob["lines"].get("MOD_LOG", [])]
        faults = [l for l in logs if l and l.startswith("E_MOD_FAULT")]
        check(len(faults) == 1 and "cause=range" in faults[0] and "phase=render" in faults[0], f"FALSIFY oversized-bitmap: the refused call faults the render callback with cause range (SEGA_MOD_E_RANGE is the status the adapter's overlay_layer_bitmap returns for it: genesis_mod_overlay_script_tests, `a bitmap layer wider than 256 is a range fault`): {faults}")
        fault = ob["summary"].get("fault") or {}
        check(ob["summary"].get("faulted") is True and "range" in json.dumps(fault), f"FALSIFY oversized-bitmap: the callback faulted with cause range ({fault})")
        check(ob["bmp"] == vwide["bmp"], "FALSIFY oversized-bitmap: no staged HUD, layer or scenery survives the fault (the presented frame is Vanilla's generic frame)")
        check(ob["ram"] == vwide["ram"] and len(ob["trace"]) == len(vwide["trace"]), "FALSIFY oversized-bitmap: the guest progressed normally (same work RAM and the same number of produced frames as Vanilla)")
        after = {f: v for f, v in verdicts(ob).items() if f >= 2}
        check(after and all(v == (2, 238, 0, 0, 5, 0) for v in after.values()), f"FALSIFY oversized-bitmap: from the faulted frame on the producer's verdict is accepted 0, reason FAULT (5), no placement, authored 0, on every frame (the discarded callback leaves nothing): {sorted(set(after.values()))}")
        # a missing refusal: the provider ignores the decision the ABI 1 twin takes (DECLINE 0 where the twin refuses)
        for ref_variant in ("refuse-all", "refuse-hud"):
            ref_run = play(abi1_program(f"mutref-{ref_variant}", ref_variant)[0], f"mutref-run-{ref_variant}", "wide")
            bad = play(abi2_program(f"mut-norefuse-{ref_variant}", "serve"), f"mut-norefuse-{ref_variant}", "wide")
            names = named_failures(ref_run, bad)
            check({"accepted-components", "picture"} <= names, f"FALSIFY missing-refusal ({ref_variant}): the differential fails a provider that serves what the ABI 1 twin refuses ({sorted(names)})")
        # ---- (4) lifecycle through the real viewer: a scripted transition (32:9 -> 21:9 -> 16:9 -> Original -> exact 4:3 -> 32:9), replay, save/load and rewind, ABI 1 twin against ABI 2, frame by frame ----
        C = make_ctx("-long", 110)
        twin_long = abi1_program("abi1-long", "serve")[0]
        abi2_long = abi2_program("abi2-long", "serve")
        if h.failures:
            return 1
        sched = "20:21:9,40:16:9,60:orig,80:4:3,100:32:9"
        OFF = [(60, 100)]   # frames after "60:orig" up to "100:32:9": the presenter shows no extension
        seq = {k: play(p, f"lc-{k}", "wide", schedule=sched) for k, p in (("a1", twin_long), ("a2", abi2_long))}
        again = {k: play(p, f"lc-{k}-2", "wide", schedule=sched) for k, p in (("a1", twin_long), ("a2", abi2_long))}
        check(len(seq["a1"]["trace"]) > 100, f"the scripted run presents more than one frame through several transitions ({len(seq['a1']['trace'])} frames)")
        check(len({t["side"] for t in seq["a2"]["trace"].values()}) >= 3, "the aspect changed several times (32:9, 21:9, 16:9: three different planned sides)")
        check(seq["a1"]["bmp"] == seq["a2"]["bmp"], "transition run: the final presented frame equals the ABI 1 twin's")
        compare_contract("transition-run", seq["a1"], seq["a2"], "wide", off=OFF)
        for k in ("a1", "a2"):
            check(again[k]["events"] == seq[k]["events"] and again[k]["bmp"] == seq[k]["bmp"], f"replay determinism ({k}): the second run reproduces every per-frame verdict and the final frame")
        lifecycle = {}
        compared = {}
        for k, (prog, ident) in (("a1", (twin_long, ids["abi1-long"])), ("a2", (abi2_long, ids["abi2-long"]))):
            base = play(prog, f"lcb-{k}", "wide")
            sv = play(prog, f"lcs-{k}", "wide", pad="0 -\n30 K\n31 -\n", save=tmp / f"save-{k}", identity=ident)
            ld = play(prog, f"lcl-{k}", "wide", pad="0 -\n3 M\n4 -\n", save=tmp / f"save-{k}", identity=ident)
            rw = play(prog, f"lcr-{k}", "wide", pad="0 -\n40 Y\n41 -\n", save=tmp / f"save-rw-{k}", identity=ident)
            check(sv["lines"]["VIEWER_STATE"][0]["status"] == "ok" and ld["lines"]["VIEWER_STATE"][0]["status"] == "ok", f"({k}) the quick save and the load succeed")
            rev = [e for e in rw["lines"].get("VIEWER_STATE", []) if e.get("event") == "rewind"]
            check(rev and rev[0].get("status") == "ok", f"({k}) the rewind took effect")
            check(sv["events"] == base["events"] and sv["bmp"] == base["bmp"], f"({k}) taking a save changes nothing (every trace occurrence equal)")
            def replayed(run, kind):
                """Walks the run's COMPLETE ordered trace (repeated frame numbers preserved: the frame-keyed dict keeps only the last occurrence). Every occurrence must equal the uninterrupted run's verdict for that frame number,
                except the one first newly produced frame after the restore (the frame right after the one the discontinuity lands on), which has no previous-onset snapshot: the host reports STALE_STATE (accepted 0, reason 1, no placement), for ABI 1 and ABI 2 alike."""
                events, bv = run["events"], verdicts(base)
                frames = [e[0] for e in events]
                jumps = [i for i in range(1, len(frames)) if frames[i] != frames[i - 1] + 1]
                wrong = [(i, e) for i, e in enumerate(events) if e[1:] != bv.get(e[0]) and not (i - 1 in jumps and e[1:][3] == 0 and e[1:][4] == 1 and e[1:][2] == 0)]
                if kind == "load":
                    shape = len(jumps) == 1 and frames[jumps[0]] >= 31 and frames[jumps[0]] > frames[jumps[0] - 1] + 1   # a forward jump into the saved timeline (the save was taken near frame 30)
                else:
                    shape = len(jumps) == 1 and frames[jumps[0]] <= frames[jumps[0] - 1] and len(frames) > len(set(frames))   # a backward jump: frame numbers repeat
                check(shape and not wrong, f"({k}) {kind}: a real frame-number discontinuity (jumps at {[(frames[i - 1], frames[i]) for i in jumps]}) and all {len(events)} trace occurrences ({len(events) - len(set(frames))} repeated frame numbers) equal the uninterrupted run's verdicts but the one STALE_STATE restore frame (wrong: {wrong[:2]})")
                check(run["bmp"] == base["bmp"] and run["ram"] == base["ram"], f"({k}) {kind}: the final presented frame and guest RAM equal the uninterrupted run's")
                return len(events)
            compared[k] = (replayed(ld, "load"), replayed(rw, "rewind"))
            lifecycle[k] = (base, ld, rw)
        for i, label in ((0, "uninterrupted"), (1, "load"), (2, "rewind")):
            check(lifecycle["a1"][i]["events"] == lifecycle["a2"][i]["events"] and lifecycle["a1"][i]["bmp"] == lifecycle["a2"][i]["bmp"], f"save/load/rewind ({label}): the ABI 2 package's per-frame verdicts and final frame equal the ABI 1 twin's")
        if printing:
            print("GOLDEN = {")
            for key in sorted(produced):
                print(f'    "{key}": "{produced[key]}",')
            print("}")
        if h.failures:
            return 1
    print(f"mod_parity_gate_abi2_test: replayed trace occurrences compared (load, rewind) per package: {compared}")
    print("mod_parity_gate_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
