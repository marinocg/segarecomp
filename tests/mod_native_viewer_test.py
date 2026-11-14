#!/usr/bin/env python3
"""SEG-138-T011 (ADR 0305): a native ABI 2 provider in BUILT Genesis programs, headless and SDL3. The FABRICATED gameplay fixture (no commercial image) is built with the UNMODIFIED product into
Vanilla, Luau-modded and native-modded programs; the native provider (platforms/genesis/sdk/examples/gameplay-native, strict C11 compiled by the host) is driven by the SAME scheduler as the Luau mod.

Proves: (1) the native-versus-Luau TWIN: identical per-frame guest RAM, replay witness, HUD overlay hash, final guest state and played HUD pixels, headless and in the SDL3 viewer; (2) `mod test` passes the
native project and `mod diff-run` reports the same differences as the Luau example; (3) save/restore round trip, a FRESH-program load, a corrupted or foreign state refused, and a viewer quick-save/load/
rewind keep the identical end state; (4) typed ABI 2 positioned overlay layers anchor exactly like the Luau ones (frames equal at 4:3, 16:9 and 32:9), and a layer outside the limits faults the callback;
(5) the scheduler's containment: range, capability, budget and phase violations fault ONLY the mod (the guest ends in the Vanilla state, nothing staged is committed); (6) a package that is not the approved
identity, an unsafe or unsupported payload, a memory capability without a declared range, or the wrong trust class is refused at build with a stable class and no executable; (7) Vanilla builds contain
no scheduler. Native code is host code: approval is the boundary, nothing here claims a sandbox.
usage: mod_native_viewer_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402

FIXTURE_SHA = "fc7126ee2497cfbc7e1e3337c230a967d21c4c98e44493a4dfd01ae3ff6a5a34"
PAD_HEADLESS = "10:1:0x20,20:1:0x00,30:1:0x08,40:1:0x00"   # C held for frames 10..19, Right held for 30..39
WRITE_DIGEST_HELD = "194aba886c6e5d5006f0167d766991397ba0996e7767dd9bdb9e201a8e10336d"


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_native_viewer_test: skipped (the generated-native build is exercised on Linux)")
        return 0
    luau_example, native_example = root / "platforms/genesis/sdk/examples/gameplay", root / "platforms/genesis/sdk/examples/gameplay-native"
    spec = importlib.util.spec_from_file_location("fixture_rom", luau_example / "fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="segarecomp-native-mod-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        rom = tmp / "fixture.rom"
        rom.write_bytes(fixture.make_rom())
        check(sha(rom.read_bytes()) == FIXTURE_SHA, "the fixture image is the pinned one")

        def project(name, example, edit=None, toml=None):
            d = tmp / name
            shutil.copytree(example, d, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
            if edit is not None:
                src = d / "src/provider.c" if (d / "src/provider.c").exists() else d / "scripts/main.luau"
                text = src.read_text()
                new = edit(text)
                check(new != text, f"{name}: the edit changed the source")
                src.write_text(new)
            if toml is not None:
                (d / "mod.toml").write_text(toml((d / "mod.toml").read_text()))
            return d

        def pack(d):
            out = tmp / (d.name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {d.name}: {r.stdout[:300]}")
            return out, doc.get("package_identity")

        lpkg, lid = pack(project("luau", luau_example))
        npkg, nid = pack(project("native", native_example))
        check(lid != nid, "the twin packages are distinct packages")

        # ---- the programs ----
        builds = {}

        def build(name, pkg=None, ident=None, viewer=True, native=False, **kw):
            r = h.build(rom, tmp / name, pkg, ident, viewer=viewer, native=native, **kw)
            builds[name] = r
            return r

        check(build("vanilla").returncode == 0, "Vanilla viewer program builds: " + builds["vanilla"].stdout[-300:])
        for name, pkg, ident, viewer, native in (("luau-h", lpkg, lid, False, False), ("native-h", npkg, nid, False, True), ("luau-v", lpkg, lid, True, False), ("native-v", npkg, nid, True, True)):
            r = build(name, pkg, ident, viewer, native)
            check(r.returncode == 0 and (tmp / name / "game").is_file(), f"{name} builds: {r.stdout[-400:]}{r.stderr[-400:]}")
        if h.failures:
            return 1
        ns = h.status(tmp / "native-v")["native_mod"]
        check(ns["package_identity"] == nid and ns["abi"] == 2 and ns["effective_sha256"] == FIXTURE_SHA and ns["sources"][0]["path"] == "src/provider.c" and "luau" not in h.status(tmp / "native-v"),
              f"status.json records the approved native package, its sources and the effective image: {ns}")
        nbytes = (tmp / "native-v/game").read_bytes()
        check(b"lua_newstate" not in nbytes and b"scheduled-mod-host" in nbytes, "the native program links the scheduler presenter and no Luau VM")
        vbytes = (tmp / "vanilla/game").read_bytes()
        check(b"scheduled-mod-host" not in vbytes and b"E_MOD_FAULT" not in vbytes, "a Vanilla viewer program contains no scheduler")

        # ---- (1) the twin, headless: per-frame RAM, witness, overlay hash, final state ----
        pl, ll = h.headless(tmp / "luau-h", "luau", PAD_HEADLESS)
        pn, ln = h.headless(tmp / "native-h", "native", PAD_HEADLESS)
        cl, cn = ll["CAPTURE_SUMMARY"][0], ln["CAPTURE_SUMMARY"][0]
        ml, mn = ll["MOD_SUMMARY"][0], ln["MOD_SUMMARY"][0]
        check(cl["ram_frames"] == cn["ram_frames"] and cl["digests"] == cn["digests"], "twin: identical per-frame guest RAM and frame digests (headless)")
        check(ml["write_digest"] == mn["write_digest"] and ml["committed_writes"] == mn["committed_writes"] == 10 and ml["overlay_sha256"] == mn["overlay_sha256"] and ml["overlay_ops"] == mn["overlay_ops"] == 4,
              f"twin: identical replay witness, committed writes and HUD overlay hash: {ml['write_digest']} vs {mn['write_digest']}")
        check(mn["faulted"] is False and ln["MOD_LOADED"][0]["kind"] == "native" and ln["MOD_LOG"][0]["line"] == "gameplay example ready", f"the native provider ran without a fault: {mn}")
        stop_l = h.headless(tmp / "luau-h", "luau-stop", PAD_HEADLESS, frames="1", first="200")[1]["CAPTURE_SUMMARY"][0]
        stop_n = h.headless(tmp / "native-h", "native-stop", PAD_HEADLESS, frames="1", first="200")[1]["CAPTURE_SUMMARY"][0]
        check(stop_l["final_work_ram_sha256"] == stop_n["final_work_ram_sha256"] and stop_n["outcome"] == "incomplete_guest_stop", "twin: the final guest state at the program's own guest stop is identical")
        # replay: identical input, identical everything
        _, ln2 = h.headless(tmp / "native-h", "native2", PAD_HEADLESS)
        check(ln2["CAPTURE_SUMMARY"][0] == cn and ln2["MOD_SUMMARY"][0] == mn, "native replay: the same input reproduces frames, RAM, witness and HUD hash")
        _, ld = h.headless(tmp / "native-h", "native-diff", "10:1:0x20,25:1:0x00")
        check(ld["MOD_SUMMARY"][0]["write_digest"] != mn["write_digest"], "a different recording gives a different witness")

        # ---- (3) save / restore: round trip, FRESH program, corrupted and foreign states ----
        state = tmp / "mid.state"
        _, lsave = h.headless(tmp / "native-h", "native-save", PAD_HEADLESS, frames="1", first="200", extra={"SEGARECOMP_MOD_CHECKPOINT": "15", "SEGARECOMP_MOD_SAVE": str(state)})
        rs = lsave["MOD_SUMMARY"][0]
        check(rs["roundtrip"]["status"] == "ok" and state.is_file() and lsave["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == stop_n["final_work_ram_sha256"], f"native checkpoint round trip loses nothing: {rs['roundtrip']}")
        _, lload = h.headless(tmp / "native-h", "native-load", PAD_HEADLESS, frames="1", first="200", extra={"SEGARECOMP_MOD_LOAD": str(state)})
        check(lload["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == stop_n["final_work_ram_sha256"] and lload["MOD_SUMMARY"][0]["write_digest"] == rs["write_digest"] and lload["MOD_SUMMARY"][0]["overlay_sha256"] == mn["overlay_sha256"],
              "a FRESH native program restored from the frame-15 state ends in the identical state, witness and HUD")
        data = bytearray(state.read_bytes())
        data[len(data) // 2] ^= 0xFF
        bad = tmp / "corrupt.state"
        bad.write_bytes(bytes(data))
        pbad, lbad = h.headless(tmp / "native-h", "native-bad", PAD_HEADLESS, frames="1", first="200", extra={"SEGARECOMP_MOD_LOAD": str(bad)})
        check(lbad.get("MOD_REFUSED", [{}])[0].get("class") == "E_STATE_RESTORE" and pbad.returncode == 5, "a corrupted state is refused before the guest runs")
        lstate = tmp / "luau.state"
        h.headless(tmp / "luau-h", "luau-save", PAD_HEADLESS, frames="1", first="200", extra={"SEGARECOMP_MOD_CHECKPOINT": "15", "SEGARECOMP_MOD_SAVE": str(lstate)})
        pf, lf = h.headless(tmp / "native-h", "native-foreign", PAD_HEADLESS, frames="1", first="200", extra={"SEGARECOMP_MOD_LOAD": str(lstate)})
        check(lf.get("MOD_REFUSED", [{}])[0].get("class") == "E_STATE_RESTORE" and pf.returncode == 5, "the Luau mod's state is another mod's state: refused by the native program")

        # ---- (1) the twin in the played SDL3 program, and viewer save / load / rewind ----
        HOLD = "0 C\n"
        _, lv, flv, rlv = h.play(tmp / "luau-v", "pl-luau", HOLD)
        _, nv, fnv, rnv = h.play(tmp / "native-v", "pl-native", HOLD)
        check(nv["VIEWER_SUMMARY"][0]["outcome"] == "guest_stop" and nv["VIEWER_NATIVE_SUMMARY"][0]["hud_frames"] > 80, f"the native program is played to its guest stop with its HUD on every frame: {nv.get('VIEWER_NATIVE_SUMMARY')}")
        check(rnv.read_bytes() == rlv.read_bytes() and nv["MOD_SUMMARY"][0]["write_digest"] == lv["MOD_SUMMARY"][0]["write_digest"] == WRITE_DIGEST_HELD, "played twin: identical guest RAM and replay witness")
        a, b = read_bmp(flv), read_bmp(fnv)
        check(a == b and nv["MOD_SUMMARY"][0]["overlay_sha256"] == lv["MOD_SUMMARY"][0]["overlay_sha256"], "played twin: the presented frame (picture + HUD) is pixel-identical")
        check(any(b[y][x] == (255, 255, 255) for y in range(9, 33) for x in range(12, 120)) and b[3 * 25 + 1][3 * 2 + 1] == (0xFF, 0xD0, 0x40), "the native HUD is really in the frame (white title text, the opaque gold bar)")
        save = tmp / "nsave"
        _, ns1, _, r1 = h.play(tmp / "native-v", "n-save", "0 C\n8 CK\n9 C\n", save=save, identity=nid, rom_sha=FIXTURE_SHA)
        _, ns2, f2, r2 = h.play(tmp / "native-v", "n-load", "0 C\n2 CM\n3 C\n", save=save, identity=nid, rom_sha=FIXTURE_SHA)
        check(ns1["VIEWER_STATE"][0]["status"] == "ok" and ns2["VIEWER_STATE"][0]["status"] == "ok" and r2.read_bytes() == rnv.read_bytes() and ns2["MOD_SUMMARY"][0]["write_digest"] == WRITE_DIGEST_HELD,
              f"viewer save then load in a NEW process ends in the identical guest state and witness: {ns2.get('VIEWER_STATE')}")
        _, ns3, _, r3 = h.play(tmp / "native-v", "n-rewind", "0 C\n14 CY\n15 C\n", save=tmp / "nrw", identity=nid, rom_sha=FIXTURE_SHA)
        check([(e["event"], e["status"]) for e in ns3.get("VIEWER_STATE", [])] == [("rewind", "ok")] and r3.read_bytes() == rnv.read_bytes() and ns3["MOD_SUMMARY"][0]["write_digest"] == WRITE_DIGEST_HELD, "a scripted rewind with the native provider attached keeps the identical end state and witness")
        _, ns4, _, r4 = h.play(tmp / "native-v", "n-foreign", "0 C\n2 CM\n3 C\n", save=save, identity=lid, rom_sha=FIXTURE_SHA)
        check(ns4["VIEWER_STATE"][0]["status"] == "failed" and r4.read_bytes() == rnv.read_bytes(), "a state saved under another package identity is refused with the stable diagnostic")

        # ---- (2) mod test and mod diff-run accept the native project ----
        nproj, lproj = tmp / "native-proj", tmp / "luau-proj"
        shutil.copytree(native_example, nproj, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
        shutil.copytree(luau_example, lproj, ignore=shutil.ignore_patterns("fixture_rom.py", "README.md", "__pycache__"))
        sdk = root / "platforms"
        t = h.run_cli("mod", "test", nproj, "--rom", rom, "--cc", compiler, "--sdk-dir", sdk)
        doc = json.loads(t.stdout)
        check(t.returncode == 0 and doc["ok"] and len(doc["scenarios"]) == 2 and all(s["passed"] for s in doc["scenarios"]), f"mod test passes the native project: {t.stdout[:300]}")
        lt = json.loads(h.run_cli("mod", "test", lproj, "--rom", rom, "--cc", compiler, "--sdk-dir", sdk).stdout)
        check([s["observed"] for s in doc["scenarios"]] == [s["observed"] for s in lt["scenarios"]], "the native scenarios observe exactly what the Luau example's do (frames, RAM per frame, final state)")
        args = ["--rom", rom, "--cc", compiler, "--sdk-dir", sdk, "--frames", "30", "--budget", "4000000", "--timeout", "120", "--input", "10:1:0x20,20:1:0x00", "--ram", "16:4"]
        dn, dl = (json.loads(h.run_cli("mod", "diff-run", p, *args).stdout) for p in (nproj, lproj))
        for d in (dn, dl):
            d.pop("package_identity", None)
            d["modded"]["mod"].pop("id", None)         # the two mods have their own ids ...
            d["modded"]["mod"].pop("callbacks", None)  # ... and a native provider is offered every phase while the Luau adapter registers only the callbacks the script defines
        check(dn.get("ok") and dn == dl, "mod diff-run of the native project reports exactly what the Luau example's reports")

        # ---- (4) typed ABI 2 positioned layers anchor like the Luau ones ----
        luau_layer = project("luau-layer", luau_example, lambda s: s.replace("function render(frame: number)\n", 'function render(frame: number)\n  sega.overlay.layer(1, "bottom_right", "presentation", 0, 0, 16, 8, 3)\n  sega.overlay.target(1)\n  sega.overlay.rect(0, 0, 16, 8, 0xFF00FFFF)\n  sega.overlay.target(0)\n', 1))
        native_layer = project("native-layer", native_example, lambda s: s.replace("    render->overlay_rect(0, 0, 76, 24, PANEL);", "    render->overlay_layer(1U, 8U, 1U, 0, 0, 16U, 8U, 3U);\n    render->overlay_target(1U);\n    render->overlay_rect(0, 0, 16, 8, 0xFF00FFFFU);\n    render->overlay_target(0U);\n    render->overlay_rect(0, 0, 76, 24, PANEL);", 1))
        lyl, lyn = pack(luau_layer), pack(native_layer)
        check(h.build(rom, tmp / "layer-l", lyl[0], lyl[1]).returncode == 0 and h.build(rom, tmp / "layer-n", lyn[0], lyn[1], native=True).returncode == 0, "the layered Luau and native programs build")
        frames = {}
        for name, mode, ratio in (("43", "original", None), ("169", "enhanced_gameplay", "16:9"), ("329", "enhanced_gameplay", "32:9")):
            cfg = tmp / f"layer-{name}.cfg"
            cfg.write_text(f"video.widescreen={mode}\nvideo.widescreen_side_style=raw\nvideo.scale=integer\nvideo.aspect=original\n" + (f"video.widescreen_ratio={ratio}\n" if ratio else ""))
            outs = []
            for kind in ("l", "n"):
                _, _, fr, _ = h.play(tmp / f"layer-{kind}", f"layer-{kind}-{name}", HOLD, extra={"SEGARECOMP_VIEWER_SETTINGS": str(cfg)})
                outs.append(read_bmp(fr))
            frames[name] = outs
            check(outs[0] == outs[1], f"{name}: the native layer frame is pixel-identical to the Luau layer frame")
        px = [(x, y) for y, row in enumerate(frames["43"][1]) for x, c in enumerate(row) if c == (255, 0, 255)]
        check(len(px) == 16 * 8 * 9 and (min(p[0] for p in px), max(p[0] for p in px), min(p[1] for p in px), max(p[1] for p in px)) == (912, 959, 648, 671), f"4:3: the native layer is the exact 16x8 block (x3) at the picture's bottom-right corner ({len(px)} px)")
        px = [(x, y) for y, row in enumerate(frames["329"][1]) for x, c in enumerate(row) if c == (255, 0, 255)]
        check(px and max(p[0] for p in px) == 959 and min(p[0] for p in px) > 960 * (796 - 40) // 796 - 4, "32:9: the native layer is anchored to the presentation (flush with the right edge of the wide surface)")

        # ---- (5) containment: each violation faults ONLY the mod ----
        cases = (
            ("range", lambda s: s.replace("#define SCORE 0x10U", "#define SCORE 0x20U"), "range"),
            ("capability", lambda s: s.replace("uint32_t pad = 0U, score = 0U;", "uint32_t pad = 0U, score = 0U;\n    (void)host->input_set(1U, 0U);"), "capability"),
            ("budget", lambda s: s.replace("    render->overlay_rect(0, 0, 76, 24, PANEL);", "    { uint32_t i; for (i = 0U; i < 70U; ++i) render->overlay_rect(0, 0, 1, 1, PANEL); }"), "budget"),
            ("phase", lambda s: s.replace("uint32_t pad = 0U, score = 0U;", "uint32_t pad = 0U, score = 0U;\n    if (render_api(host) != NULL) (void)render_api(host)->overlay_rect(0, 0, 1, 1, PANEL);"), "phase"),
            ("layer", lambda s: s.replace("    render->overlay_rect(0, 0, 76, 24, PANEL);", "    render->overlay_layer(1U, 8U, 1U, 0, 0, 200U, 8U, 3U);"), "range"),
        )
        for name, edit, cause in cases:
            pkg, ident = pack(project("fault-" + name, native_example, edit))
            r = h.build(rom, tmp / ("fault-" + name), pkg, ident, viewer=False, native=True)
            if not check(r.returncode == 0, f"fault-{name}: the provider builds: {r.stdout[-300:]}{r.stderr[-300:]}"):
                continue
            _, lines = h.headless(tmp / ("fault-" + name), "fault-" + name, PAD_HEADLESS, frames="1", first="200")
            ms = lines["MOD_SUMMARY"][0]
            check(ms["faulted"] is True and f"cause={cause}" in ms["fault"], f"fault-{name}: the violation is a {cause} fault of the callback: {ms['fault']}")
            check(lines["CAPTURE_SUMMARY"][0]["outcome"] == "incomplete_guest_stop", f"fault-{name}: the guest still runs to its own guest stop")
            if name in ("range", "capability"):
                check(ms["committed_writes"] == 0 and lines["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] != stop_n["final_work_ram_sha256"], f"fault-{name}: the faulted callback's writes were discarded and the mod is disabled")
            if name in ("budget", "layer"):
                check(ms["overlay_ops"] == 0, f"fault-{name}: the faulted render committed no overlay")
        # without the overlay capability the render API is absent: the provider presents nothing and the guest effect stays
        no_overlay = project("no-overlay", native_example, toml=lambda t: t.replace('"overlay.rgba", ', ""))
        pkg, ident = pack(no_overlay)
        check(h.build(rom, tmp / "no-overlay", pkg, ident, viewer=False, native=True).returncode == 0, "a provider without overlay.rgba builds")
        _, lines = h.headless(tmp / "no-overlay", "no-overlay", PAD_HEADLESS, frames="1", first="200")
        check(lines["MOD_SUMMARY"][0]["faulted"] is False and lines["MOD_SUMMARY"][0]["overlay_ops"] == 0 and lines["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == stop_n["final_work_ram_sha256"],
              "without overlay.rgba the typed render feature is not published (features->render is NULL): no HUD, the gameplay effect is unchanged")

        # ---- (6) approval, identity, payload and trust refusals ----
        def refused(what, code, pkg, ident, native=True, rom_path=rom):
            out = tmp / ("refused-" + what)
            r = h.build(rom_path, out, pkg, ident, viewer=False, native=native)
            diag = h.status(out).get("diagnostic") if (out / "status.json").exists() else None
            check(r.returncode != 0 and not (out / "game").exists() and diag == code, f"{what}: refused with {code} and no executable: {diag} {(r.stdout + r.stderr)[-200:]}")

        refused("identity", "NATIVE_IDENTITY_CHANGED", npkg, "00" * 32)
        refused("approval-of-another-package", "NATIVE_IDENTITY_CHANGED", npkg, lid)
        refused("luau-package-as-native", "E_LIVE_TRUST", lpkg, lid)
        wrong = tmp / "wrong.rom"
        wrong.write_bytes(rom.read_bytes() + b"\0")
        refused("image", "NATIVE_IMAGE_MISMATCH", npkg, nid, rom_path=wrong)
        asset = project("with-asset", native_example, toml=lambda t: t + "\n[[source]]\nkind = \"asset\"\npath = \"assets/a.bin\"\nrole = \"data\"\n")
        (asset / "assets").mkdir()
        (asset / "assets/a.bin").write_bytes(b"x")
        apkg = h.run_cli("mod", "pack", asset, "--out", tmp / "asset.segapatch")
        if apkg.returncode == 0:
            refused("asset-payload", "E_LIVE_PAYLOAD", tmp / "asset.segapatch", json.loads(apkg.stdout)["package_identity"])
        norange = project("no-range", native_example, toml=lambda t: t[: t.index("# The score word")])
        r = h.run_cli("mod", "pack", norange, "--out", tmp / "norange.segapatch")
        if r.returncode == 0:
            refused("memory-without-range", "E_RANGE_DECLARATION", tmp / "norange.segapatch", json.loads(r.stdout)["package_identity"])
        else:
            check("E_" in r.stdout, f"a memory capability without any declared range is refused even earlier, by the package check: {r.stdout[:200]}")
        broken = project("broken-c", native_example, lambda s: s.replace("static uint32_t granted;", "static uint32_t granted;\nstatic int unused_but_declared;"))
        bpkg, bid = pack(broken)
        out = tmp / "refused-warning"
        r = h.build(rom, out, bpkg, bid, viewer=False, native=True)
        check(r.returncode != 0 and not (out / "game").exists(), "a unit the host's strict flags reject (-Werror) fails the build and leaves no executable")
        r = h.run_cli("build", "--rom", rom, "--output", tmp / "usage", "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--native-package", npkg)
        check(r.returncode != 0 and not (tmp / "usage/game").exists(), "a package reference without the approved identity is a usage error")

        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_native_viewer_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
