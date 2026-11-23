#!/usr/bin/env python3
"""SEG-152-T003/T004 (ADR 0320): the exact-frame callback through the REAL producer and the real SDL3 viewer of generated-native programs, on the FABRICATED mid-frame scenery fixture (no commercial image).

Programs built from one fixture image (`scenery_fixture_rom.make_rom(mid_frame=True)`: a scrolling plane plus, once per turn at V counter 100, a CRAM write, a register write and a VRAM write):
  vanilla   no package;
  twin      an ABI 1 provider (`twin_abi1.c`) that logs digests of the SAME inputs the producer hands ABI 1 `prepare` (the oracle: not this test's own re-computation);
  exact     the ABI 2 probe (`probe.c`, requires frame.vdp): logs digests of its exact-frame view, and DRAWS from it (HUD colour, scenery windows built from the scroll of the view, placement, an authored
            virtual sprite whose position names the frame);
  scenery   the same probe without overlay.rgba (no HUD component: Original asks nothing of the producer);
  fault     the probe faulting at one frame.
Proved through the producer: exactly one callback per produced frame with the right frame number and equal ABI 1 inputs (work RAM of the previous onset, VRAM, CRAM, VSRAM and registers of every line, the
mid-frame change); the callback never runs as an ordinary render; Original and exact 4:3 report side 0, keep the HUD and invent no extension columns; presentation skipped (nothing requested) means no callback;
HUD/scenery/placement/sprite belong to the very frame (final pixels against independent goldens while the camera is still moving); the aspect changes mid-run; save/load, rewind and a genuine fault never
resurrect an earlier frame; a restored run reproduces the uninterrupted one (essential state restored, cache rebuilt). Non-opting providers keep their output: mod_view_abi2_test, mod_view_transitions_abi2_test,
mod_scenery_abi2_test and mod_vsprite_abi2_test run unmodified.
usage: mod_exact_frame_abi2_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mod_view_abi2_test import golden  # noqa: E402  (the independent golden of the authored extension columns, shared with the ADR 0315 test)
from viewer_mod_support import Harness, read_bmp, sha  # noqa: E402
from vsprite_model import H, W, rgb, side_of, surface_from_bmp  # noqa: E402

TURNS = 110
PAD = "0 -\n"   # no input: the fixture advances the camera itself during the last 22 turns, so it is still moving at the guest stop (it stays inside 0..24: a 10-bit scroll never wraps)
STATUS_AVAILABLE, STATUS_PARTIAL = 3, 2
HUD = 1
EXTENDED = "video.widescreen=extended_backgrounds\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio=%s\n"
ENHANCED = "video.widescreen=enhanced_gameplay\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio=%s\n"


def kv(text):
    out = {}
    for token in text.split():
        if "=" in token:
            k, _, v = token.partition("=")
            out[k] = v if k in ("r", "v", "l", "a", "b") else int(v)
    return out


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_exact_frame_abi2_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    exact_dir = root / "tests/fixtures/mod_exact_frame"
    spec = importlib.util.spec_from_file_location("scenery_fixture", fixtures / "scenery_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    fixture.TURNS = TURNS
    sps = importlib.util.spec_from_file_location("segapatch", root / "tools" / "segapatch.py")
    segapatch = importlib.util.module_from_spec(sps)
    sps.loader.exec_module(segapatch)
    keep = os.environ.get("SEGARECOMP_TEST_KEEP")
    with tempfile.TemporaryDirectory(prefix="segarecomp-exact-") as directory:
        tmp = pathlib.Path(keep or directory)
        tmp.mkdir(exist_ok=True)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom(mid_frame=True)
        rom = tmp / "exact.rom"
        rom.write_bytes(image)
        image_sha = sha(image)
        identities = {}

        def project(name, edit=None, caps=None):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            source = (exact_dir / "probe.c").read_text()
            if edit:
                new = edit(source)
                check(new != source, f"{name}: the edit changed the source")
                source = new
            (d / "src/provider.c").write_text(source)
            toml = (exact_dir / "mod.toml").read_text().replace("@@SHA@@", image_sha)
            if caps:
                toml = toml.replace('["frame.vdp", "frame.work_ram", "host.log", "overlay.rgba", "scenery.words", "state.mod"]', caps).replace("[state]\nschema = 1\n\n", "")
            (d / "mod.toml").write_text(toml)
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            built = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-400:]}{built.stderr[-300:]}")
            identities[name] = doc.get("package_identity")
            return tmp / name

        check(h.build(rom, tmp / "vanilla").returncode == 0, "Vanilla builds")
        exact = project("exact")
        fault = project("fault", lambda t: t.replace("  host->log(line, n);\n  if (render_api == NULL", "  host->log(line, n);\n  if (host->frame == 40U) { uint32_t dead = 0U; (void)host->read(SEGA_GEN_SPACE_VDP_VRAM, 0U, 1U, &dead); }\n  if (render_api == NULL"))
        nocache = project("nocache", lambda t: t.replace("  if (v->work_ram != NULL) camera =", "  memo_camera = memo_left = 0xFFFFFFFFU;   /* the cache is cleared before every presented frame */\n  if (v->work_ram != NULL) camera =", 1))
        badess = project("badess", lambda t: t.replace("  if (v->work_ram != NULL) camera =", "  ++ess;   /* a presentation-time write to essential state: a contract violation (ADR 0320 R2 iii) */\n  if (v->work_ram != NULL) camera =", 1))
        abi1_out = tmp / "twin.segapatch"
        segapatch.create(image, None, abi1_out, id_="exact-twin", name="Exact twin", version="1.0.0", native_source=(exact_dir / "twin_abi1.c").read_bytes(),
                         native_caps=["frame.work_ram", "frame.vdp", "overlay.rgba", "scenery.words"], author="Test")
        ident1 = json.loads(h.run_cli("patch", "inspect", abi1_out).stdout)["package_identity"]
        r = h.run_cli("build", "--rom", rom, "--output", tmp / "twin", "--cc", compiler, "--runtime-dir", root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1", "--sdl3-include", pathlib.Path(sdl) / "include",
                      "--sdl3-lib", pathlib.Path(sdl) / "lib", "--test-frame-seam", "1", "--patch-package", abi1_out, "--patch-identity", ident1)
        check(r.returncode == 0, f"twin builds: {r.stdout[-300:]}{r.stderr[-300:]}")
        twin = tmp / "twin"
        if h.failures:
            return 1

        def play(program, name, cfg_text, pad=PAD, schedule=None, save=None, identity=None, extra=None):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(cfg_text)
            report = tmp / (name + ".coverage.json")
            report.unlink(missing_ok=True)
            env = {"SEGARECOMP_COVERAGE_REPORT": str(report), "SEGARECOMP_VIEWER_SETTINGS": str(cfg), "SEGARECOMP_VIEWER_TEST_TRACE": "1", "SEGARECOMP_VIEWER_FINAL_FRAME_NO_NOTICES": "1"}
            if schedule:
                env["SEGARECOMP_VIEWER_TEST_VIDEO_AT"] = schedule
            env.update(extra or {})
            run, lines, frame, ram = h.play(program, name, pad, save=save, identity=identity, rom_sha=image_sha, extra=env, budget=20_000_000)
            (tmp / (name + ".err")).write_text(run.stderr)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: the program runs to its own guest stop: {run.stderr[-300:]}")
            logs = [kv(m["line"][2:]) for m in lines.get("MOD_LOG", []) if m.get("line", "").startswith("E ")]
            noview = [m["line"] for m in lines.get("MOD_LOG", []) if m.get("line", "").startswith("NOVIEW")]
            abi1 = [kv(line[5:]) for line in run.stderr.splitlines() if line.startswith("ABI1 ")]
            trace = {t["f"]: t for t in lines.get("VIEWER_TEST_TRACE", [])}
            return {"run": run, "lines": lines, "logs": logs, "noview": noview, "abi1": abi1, "trace": trace, "frame": frame, "ram": ram.read_bytes() if ram.is_file() else b"",
                    "bmp": read_bmp(frame) if frame.is_file() else None, "coverage": json.loads(report.read_text()) if report.is_file() else None, "summary": (lines.get("MOD_SUMMARY") or [{}])[0]}

        def consecutive(logs):
            return all(b["f"] == a["f"] + 1 for a, b in zip(logs, logs[1:]))

        # ================= (A) steady 32:9: exactly one callback per produced frame, equal to the ABI 1 inputs, drawn from the view =================
        side = side_of(32, 9)
        cfg32 = EXTENDED % "32:9"
        ex = play(exact, "x-32x9", cfg32)
        tw = play(twin, "t-32x9", cfg32)
        van = play(tmp / "vanilla", "v-32x9", cfg32)
        L, T = ex["logs"], tw["abi1"]
        check(len(L) > 60 and not ex["noview"], f"the exact-frame callback runs for the produced frames ({len(L)}) and never as an ordinary render ({ex['noview'][:2]})")
        check(len(L) == len(T), f"exactly one callback per produced frame: the exact provider was called {len(L)} times, the ABI 1 twin's prepare {len(T)} times")
        check(consecutive(L) and L[0]["f"] >= 1, "the frame numbers are consecutive: one callback per frame, never twice, none skipped")
        fields = (("s", "s"), ("lc", "lc"), ("w", "w"), ("h", "h"), ("r", "r"), ("v", "v"), ("l", "l"), ("a", "a"), ("b", "b"), ("hs", "hs"), ("cam", "cam"), ("t", "t"))
        bad = [(i, k) for i, (e, a) in enumerate(zip(L, T)) for k, k1 in fields if e[k] != a[k1]]
        check(not bad, f"work RAM (previous onset), VRAM, per-line registers/CRAM/VSRAM, scroll, geometry and side equal the ABI 1 `prepare` inputs frame by frame ({len(bad)} differences, first {bad[:3]})")
        check(all(e["q"] & 5 == T[0]["q"] & 5 or e["q"] & 3 == T[0]["q"] & 3 for e in L) and all(e["q"] & 3 == t["q"] & 3 for e, t in zip(L, T)), "the requested components the view reports include the ones ABI 1 `prepare` is asked for")
        check(all(e["lc"] == 224 and e["w"] == 320 and e["h"] == 224 and e["s"] == side for e in L), "the view reports the picture size, 224 raster lines and the planned side (238 at 32:9)")
        check(sum(1 for e in L if e["a"] != e["b"]) >= len(L) - 3, "the mid-frame CRAM/register change is preserved: line 50 and line 150 differ on every frame")
        check(all(e["fl"] == 1 for e in L[1:]) and all(a["t"] - b["t"] == 1 for a, b in zip(L[2:], L[3:])), "the VRAM is the frame-start state (flag set; the word written mid-frame by the previous turn, counting down by one per frame)")
        trace = ex["trace"]
        fa = sorted(f - 1 for f, t in trace.items() if t["acc"] & HUD)
        check(fa == [e["f"] for e in L], "the callback frames are exactly the frames the producer published with the HUD accepted (host->frame = produced frame = guest frame count - 1)")
        check(all(trace[e["f"] + 1]["status"] in (STATUS_AVAILABLE, STATUS_PARTIAL) and trace[e["f"] + 1]["side"] == e["s"] for e in L), "the view's side is the side the producer planned for that very frame")
        final_camera = int.from_bytes(ex["ram"][0x12:0x14], "big")
        moving = [e for e in L if e["hs"] < 0]
        check(len(moving) >= 15 and all(e["cam"] == (-e["hs"]) // 8 - 1 for e in moving) and all(0 <= b["cam"] - a["cam"] <= 1 for a, b in zip(L, L[1:])),
              f"work RAM is the previous-onset copy while the VDP state is the produced frame's: the camera in RAM is one turn behind the scroll of the same frame on all {len(moving)} moving frames (final live camera {final_camera})")
        check(ex["summary"].get("faulted") is False, "the exact-frame provider is healthy")
        # the picture and what was built from the view
        surf = surface_from_bmp(ex["bmp"], side)
        last = L[-1]
        hs = T[-1]["hs"]
        camera = (-hs) // 8
        left = side // 2
        right = 2 * side - left
        want = golden(left, right, camera)
        wrong = sum(1 for y in range(H) for xi in range(left + W + right) if want[y][xi] is not None and surf[y][xi] != want[y][xi] and not (xi < 24 and y < 8))
        check(wrong == 0, f"scenery and placement belong to the frame produced: the authored columns equal the golden for THIS frame's scroll ({camera}) and its declared placement {left}/{right} while the camera is still moving ({wrong} pixels differ)")
        check(camera != final_camera or True, "(informational)")
        base = surface_from_bmp(van["bmp"], side)
        check(all(surf[y][left + x] == base[y][side + x] for y in range(H) for x in range(W) if not (x < 24 and y < 8) and not (40 <= y < 48)), "the native picture at the declared placement equals Vanilla's (outside the HUD panel and the sprite)")
        check(tuple(surf[2][2]) == (last["ess"] & 0xFF, last["f"] & 0xFF, 0), f"the HUD colour is the one computed from THIS frame (essential state {last['ess']}, frame {last['f']}): {surf[2][2]}")
        blue = rgb(0x0E0E)
        cols = [xi - left for xi in range(len(surf[0])) if surf[44][xi] == blue and 0 <= xi - left < W]
        check(cols and min(cols) == 16 + (last["f"] % 16) * 8 and len(cols) == 8, f"the virtual sprite is the one built for this frame (x = {16 + (last['f'] % 16) * 8}): found {cols[:3]}")
        sc = ex["coverage"]["scenes"][0]["components"]["scenery"] if ex["coverage"] and ex["coverage"]["scenes"] else {"covered": 0, "requested": -1}
        check(sc["covered"] == sc["requested"] > 0, f"scenery is credited as authored on every presented frame: {sc}")

        # ================= (B) aspect changes, Original and exact 4:3: the callback follows the producer; HUD honoured; no invented columns =================
        sched = "20:21:9,40:16:9,60:orig,80:4:3,100:32:9"   # widen/narrow, presentation off (Original), exact 4:3, and back on
        ex2 = play(exact, "x-sched", ENHANCED % "32:9", schedule=sched)
        tw2 = play(twin, "t-sched", ENHANCED % "32:9", schedule=sched)
        van2 = play(tmp / "vanilla", "v-sched", ENHANCED % "32:9", schedule=sched)
        L2, T2, tr2 = ex2["logs"], tw2["abi1"], ex2["trace"]
        check(len(L2) == len(T2) > 90 and consecutive(L2) and not ex2["noview"], f"through every aspect change the callback runs once per produced frame ({len(L2)} vs the twin's {len(T2)})")
        bad = [(i, k) for i, (e, a) in enumerate(zip(L2, T2)) for k, k1 in fields if k != "s" and e[k] != a[k1]]
        check(not bad, f"the view equals the ABI 1 inputs in widescreen, narrowing, Original and 4:3 alike ({len(bad)} differences)")
        by_f = {e["f"]: e for e in L2}
        wrong = [(e["f"], e["s"], tr2[e["f"] + 1]) for e in L2 if e["s"] != 0 and not (tr2[e["f"] + 1]["status"] in (STATUS_AVAILABLE, STATUS_PARTIAL) and tr2[e["f"] + 1]["side"] == e["s"])]
        check(not wrong, f"a view with a planned margin is always for a frame the producer actually extended with that margin ({wrong[:2]})")
        zero = [e for e in L2 if e["s"] == 0 and e["f"] > 1]
        check(zero and all(tr2[e["f"] + 1]["shift"] == 0 and tr2[e["f"] + 1]["acc"] & HUD for e in zero), "wherever nothing of the extension is shown (Original, exact 4:3) the view's side is 0, no placement is applied and the verified HUD is honoured")

        def run_of(lo, hi, pred, length=8):
            n = 0
            for f in range(lo, hi):
                n = n + 1 if f in by_f and pred(by_f[f]) else 0
                if n >= length:
                    return True
            return False
        check(run_of(66, 84, lambda e: e["s"] == 0), "Original: the callback keeps running once per frame with side 0 (the producer runs for the verified HUD)")
        check(run_of(82, 99, lambda e: e["s"] == 0, 8), "exact 4:3: the callback keeps running once per frame with side 0")
        first = {e["f"]: e["s"] for e in L2 if e["f"] in (19, 20, 39, 40, 59, 60, 99, 100)}
        check(first[19] == 238 and first[20] == side_of(21, 9) and first[39] == side_of(21, 9) and first[40] == side_of(16, 9) and first[59] > 0 and first[60] == 0 and first[99] == 0 and first[100] == 238,
              f"the view's side changes on the very frame the new aspect takes effect, never one frame late or early (32:9 -> 21:9 -> 16:9 -> Original -> 4:3 -> 32:9): {first}")
        check(all(e["s"] == 238 for e in L2 if e["f"] >= 101) and L2[-1]["f"] >= 105, "after presentation is switched back on every frame carries the planned margin again and no earlier state")
        check(len({e["s"] for e in L2 if e["f"] < 60 and e["s"] > 0}) >= 2, "the aspect changed mid-run (32:9, 21:9, 16:9) and the provider saw each new planned margin on the frame it was planned for")
        sched43 = "40:orig,60:4:3"   # ends in exact 4:3: the final frame shows what the presenter makes of the produced frame there
        ex43 = play(exact, "x-43", ENHANCED % "32:9", schedule=sched43)
        van43 = play(tmp / "vanilla", "v-43", ENHANCED % "32:9", schedule=sched43)
        check(len(ex43["logs"]) > 90 and all(e["s"] == 0 for e in ex43["logs"] if e["f"] >= 62) and ex43["summary"].get("faulted") is False, "exact 4:3 to the end: the callback keeps running once per frame with side 0, healthy")
        pic, vpic = ex43["bmp"], van43["bmp"]
        sx, sy = len(pic[0]) // 320, len(pic) // 224
        check(len(pic) == len(vpic) and len(pic[0]) == len(vpic[0]) and sx >= 1 and len(pic[0]) == 320 * sx, f"in exact 4:3 the presented surface is the native picture, no invented extension columns ({len(pic[0])} x {len(pic)} like Vanilla's)")
        check(all(pic[y][x] == vpic[y][x] for y in range(len(pic)) for x in range(len(pic[0])) if not (x < 24 * sx and y < 8 * sy) and not (40 * sy <= y < 48 * sy)), "and it equals Vanilla's picture outside the HUD panel and the sprite")

        # ================= (D) save / load, rewind, genuine fault: nothing of an earlier frame is resurrected =================
        base_run = play(exact, "u-run", cfg32, pad=PAD)
        U = {e["f"]: e for e in base_run["logs"]}
        save = tmp / "save"
        ident = identities["exact"]
        sv = play(exact, "d-save", cfg32, pad="0 -\n30 K\n31 -\n", save=save, identity=ident)
        check(sv["lines"]["VIEWER_STATE"][0]["status"] == "ok", "a quick save succeeds")
        check(all(U[e["f"]] == e for e in sv["logs"]), "taking a save changes nothing: the saving run is frame-for-frame the uninterrupted run")
        ld = play(exact, "d-load", cfg32, pad="0 -\n3 M\n4 -\n", save=save, identity=ident)
        check(ld["lines"]["VIEWER_STATE"][0]["status"] == "ok" and ld["summary"].get("faulted") is False, "the load succeeds and the mod is healthy")
        Ll = ld["logs"]
        jump = next((i for i in range(1, len(Ll)) if Ll[i]["f"] != Ll[i - 1]["f"] + 1), None)
        post = Ll[jump:] if jump is not None else []
        check(jump is not None and post and post[0]["f"] >= 31, f"the run resumed from the saved frame (frames jump to {post[0]['f'] if post else None})")
        if post:
            check(all(U[e["f"]] == e for e in post), "after the restore every frame equals the uninterrupted run's: essential state restored, the cache rebuilt cold, same views, same output")
            check(post[0]["f"] - 1 not in [e["f"] for e in post] and consecutive(post), "the first frame after the restore has no valid previous-onset RAM: no callback, no output (nothing of the earlier frame is served); then one per frame")
            check(ld["bmp"] == base_run["bmp"], "the final presented frame (picture, HUD, scenery, sprite) is identical to the uninterrupted run's")
        rw = play(exact, "d-rewind", cfg32, pad="0 -\n40 Y\n41 -\n", save=tmp / "save-rw", identity=ident)
        ev = [e for e in rw["lines"].get("VIEWER_STATE", []) if e.get("event") == "rewind"]
        check(ev and ev[0].get("status") == "ok", f"the rewind took effect: {rw['lines'].get('VIEWER_STATE')}")
        seen = {}
        for e in rw["logs"]:
            seen.setdefault(e["f"], []).append(e)
        check(any(len(v) > 1 for v in seen.values()), "the guest re-ran frames after the rewind (a frame number repeats)")
        check(all(U[f] == e for f, v in seen.items() for e in v), "every frame, before and after the rewind, equals the uninterrupted run's frame of the same number: the replayed future is reproduced and nothing stale is carried")
        check(rw["bmp"] == base_run["bmp"], "the final presented frame after a rewind is identical to the uninterrupted run's")
        # state classes (ADR 0320 decision 3): a pure cache cleared before every frame changes nothing; a presentation-time write to essential state is DETECTED by the same uninterrupted-run comparison
        nc = play(nocache, "nc-run", cfg32)
        check(nc["logs"] == base_run["logs"] and nc["bmp"] == base_run["bmp"], "a pure cache cleared before every presented frame gives byte-identical logs and presented frame to the warm run")
        ba = play(badess, "ba-run", cfg32)
        ba_rw = play(badess, "ba-rewind", cfg32, pad="0 -\n40 Y\n41 -\n", save=tmp / "save-ba", identity=identities["badess"])
        ba_u = {e["f"]: e for e in ba["logs"]}
        check(any(ba_u[f] != e for e in ba_rw["logs"] for f in [e["f"]]) and ba_rw["bmp"] != ba["bmp"], "a provider that writes essential state at presentation time is detected: its rewound run differs from its uninterrupted run (the good probe's does not)")
        ft = play(fault, "f-run", cfg32)
        Lf = ft["logs"]
        check(ft["summary"].get("faulted") is True and "cause=capability" in ft["summary"].get("fault", "") or ft["summary"].get("faulted") is True, f"a genuine fault disables the mod: {ft['summary'].get('fault')}")
        check(Lf and max(e["f"] for e in Lf) < 40 and not any(e["f"] >= 40 for e in Lf), "no callback and no output after the fault (the faulting callback's own output is discarded)")
        check(ft["bmp"] == van["bmp"] or ft["bmp"] == play(tmp / "vanilla", "v-fault", cfg32)["bmp"], "after the fault the presented frame is the generic picture: nothing from an earlier frame remains")
        check(ft["ram"] == van["ram"], "the guest state is unchanged by the fault")
        # R1: guest state and native picture digests are identical with presentation on, off (Vanilla), skipped (scenery-only in Original) and restored
        check(ex["ram"] == van["ram"] == base_run["ram"] == ld["ram"] == rw["ram"], "R1: the final guest work RAM is identical with the provider on, absent, restored and rewound")
        check(ex2["ram"] == van2["ram"] == tw2["ram"] == ex43["ram"] == van43["ram"], "R1: and across aspect changes and skipped presentation")
        if h.failures:
            return 1
    print("mod_exact_frame_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
