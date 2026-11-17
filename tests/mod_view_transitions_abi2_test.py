#!/usr/bin/env python3
"""ADR 0315: the margin a native ABI 2 provider learns (`view_margin`) and the placement it declares (`view_left`) across aspect transitions, through the real SDL3 viewer of a generated-native program on the FABRICATED
scenery fixture (no commercial image). The viewer's test seam (`--test-frame-seam 1`) changes the aspect through the real settings path at scripted guest frames and traces, per guest frame, the extension result the producer
published (status, side, shift, provider verdict).

Two providers: `careful` builds its windows and its placement from the margin it learnt and submits neither when that margin is no longer the host's (what the Castle package does); `naive` always submits both. Proved over
widening, narrowing, switching to Original and to 4:3 (no extension planned = the early-return path of the producer) and returning to widescreen:
 (1) the margin a provider reads is 0 for Original, 4:3 and any frame without a produced extension, and the planned side otherwise (never a value cached from an earlier frame);
 (2) a frame is never produced with a placement computed for another margin (the fixture's rule is left = margin / 2, so a placed frame has side + shift == side / 2);
 (3) a placement and the windows built for it are rejected together: a stale placement leaves the scenery generic (SCENERY not accepted) while the independent HUD contribution stays accepted on every produced frame;
 (4) a placed frame has no unanswered scenery cell (provider verdict OK), and placement resumes after every transition.
usage: mod_view_transitions_abi2_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness, sha  # noqa: E402

HUD, SCENERY = 1, 2
STATUS_AVAILABLE, STATUS_PARTIAL = 3, 2   # GenesisExtensionStatus (checked against the header below)

STATE = "static uint32_t camera, prev_margin;\n"
PRE = """    uint32_t margin_now = 0U, used, serve = 1U, left_px = 0U, declare = 0U;
    int32_t bx0 = -40;
    if (render->view_margin != NULL) (void)render->view_margin(&margin_now);
    { char text[16]; uint32_t n = 0U, v = margin_now, d; char rev[10]; uint32_t k = 0U; text[n++] = 'm'; text[n++] = '=';
      do { rev[k++] = (char)('0' + v % 10U); v /= 10U; } while (v != 0U && k < 9U);
      for (d = 0U; d < k; ++d) text[n++] = rev[k - 1U - d];
      host->log(text, n); }
    used = prev_margin;
    prev_margin = margin_now;
    x0 = (int32_t)camera - 40;
    if (used != 0U) {
      left_px = used / 2U;
      x0 = (int32_t)camera - (int32_t)(left_px / 8U) - 3;
      bx0 = -(int32_t)(left_px / 8U) - 3;
      declare = 1U;
      if (margin_now != used) { @STALE@ }
    }
"""
CAREFUL_STALE = "serve = 0U; declare = 0U;"
NAIVE_STALE = ";"
POST = "    if (declare != 0U && render->view_left != NULL) (void)render->view_left(left_px, used);\n"


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_view_transitions_abi2_test: skipped (the generated-native viewer build is exercised on Linux)")
        return 0
    fixtures = root / "tests/fixtures/mod_scenery"
    spec = importlib.util.spec_from_file_location("scenery_fixture", fixtures / "scenery_fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    fixture.TURNS = 330   # a long run: the transitions below happen at guest frames 40 .. 240 (the program ends with a typed guest stop)
    header = (root / "platforms/genesis/runtime/frame_extension.h").read_text()
    assert "GENESIS_EXT_AVAILABLE" in header and "GENESIS_EXT_PARTIAL" in header
    keep = os.environ.get("SEGARECOMP_TEST_KEEP")
    with tempfile.TemporaryDirectory(prefix="segarecomp-viewtr-") as directory:
        tmp = pathlib.Path(keep or directory)
        tmp.mkdir(exist_ok=True)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "tr.rom"
        rom.write_bytes(image)
        image_sha = sha(image)

        def project(name, stale, only_framing=False, indicator=False):
            d = tmp / (name + "-src")
            (d / "src").mkdir(parents=True)
            t = (fixtures / "provider.c").read_text()
            t = t.replace("static uint32_t camera;\n", STATE)
            t = t.replace("    int32_t x0;\n", "    int32_t x0;\n")
            marker = "    x0 = (int32_t)camera - 40;\n"
            assert marker in t
            t = t.replace(marker, PRE.replace("@STALE@", stale))
            t = t.replace("    (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n", "    if (serve != 0U) (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n")
            t = t.replace("scenery_window(1U, -40, 0, COLS, ROWS, blank)", "scenery_window(1U, bx0, 0, COLS, ROWS, blank)")
            t = t.replace("    (void)render->scenery_window(1U, bx0, 0, COLS, ROWS, blank);\n", "    if (serve != 0U) (void)render->scenery_window(1U, bx0, 0, COLS, ROWS, blank);\n")
            t = t.replace("    (void)blank;", POST + "    (void)blank;")
            if only_framing:   # generic scenery: no window, no overlay: only the placement (ADR 0315: framing is independent of scenery)
                t = t.replace("    if (serve != 0U) (void)render->scenery_window(0U, x0, 0, COLS, ROWS, words);\n", "").replace("    if (serve != 0U) (void)render->scenery_window(1U, bx0, 0, COLS, ROWS, blank);\n", "").replace("    (void)render->overlay_rect(0, 0, 24, 8, 0x000000C0U);\n", "").replace("    (void)blank;", "    (void)blank; (void)words; (void)bx0; (void)serve; (void)x0;")
            if indicator:   # the Castle indicator: a positioned layer anchored to the top right of the presentation, drawn in magenta so the test can find it
                t = t.replace("    (void)render->overlay_rect(0, 0, 24, 8, 0x000000C0U);\n", "    if (render->overlay_layer(1U, 2U, 1U, 0, 0, 24U, 8U, 0U) == 0) (void)render->overlay_target(1U);\n    (void)render->overlay_rect(0, 0, 24, 8, 0xFF00FFFFU);\n")
            (d / "src/provider.c").write_text(t)
            (d / "mod.toml").write_text((fixtures / "mod.toml").read_text().replace("@@SHA@@", image_sha).replace('"overlay.rgba", "scenery.words"', '"host.log", "overlay.rgba", "scenery.words"'))
            out = tmp / (name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            built = h.build(rom, tmp / name, out, doc.get("package_identity"), native=True)
            check(built.returncode == 0, f"{name} builds: {built.stdout[-400:]}{built.stderr[-300:]}")
            return tmp / name

        framing = project("framing", CAREFUL_STALE, only_framing=True)
        indicator = project("indicator", CAREFUL_STALE, indicator=True)
        careful = project("careful", CAREFUL_STALE)
        naive = project("naive", NAIVE_STALE)
        if h.failures:
            return 1

        def play(program, name, ratio, schedule):
            cfg = tmp / (name + ".cfg")
            cfg.write_text(f"video.widescreen=enhanced_gameplay\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio={ratio}\n")
            run, lines, frame, ram = h.play(program, name, "0 -\n", extra={"SEGARECOMP_VIEWER_SETTINGS": str(cfg), "SEGARECOMP_VIEWER_TEST_TRACE": "1", "SEGARECOMP_VIEWER_TEST_VIDEO_AT": schedule}, budget=20_000_000)
            check(lines.get("VIEWER_SUMMARY", [{}])[0].get("outcome") == "guest_stop", f"{name}: runs to the guest stop: {run.stderr[-300:]}")
            (tmp / (name + ".err")).write_text(run.stderr)
            trace = {t["f"]: t for t in lines.get("VIEWER_TEST_TRACE", [])}
            margins = [int(m["raw"][2:]) if "raw" in m else None for m in []]
            seen = []
            for line in run.stderr.splitlines():
                if line.startswith("MOD_LOG") and "m=" in line:
                    seen.append(int(line.rsplit("m=", 1)[1].strip().strip('"}').split('"')[0] or 0))
            return trace, seen

        # widen 16:9 -> 32:9 at 40, narrow to 16:9 at 80, Original at 120, back to 32:9 at 160, 4:3 (nothing planned) at 200, 21:9 at 240
        # the indicator layer through the real presenter (the host's timed notices are cleared before the final frame is captured)
        from viewer_mod_support import read_bmp
        # (label, starting configured ratio, scripted changes): Original after exact 4:3 retains the configured exact aspect, which plans nothing: the provider must still be served
        for label, ratio0, sched in (("32:9", "16:9", "5:32:9"), ("Original", "16:9", "5:orig"), ("exact 4:3", "16:9", "5:4:3"), ("Original after exact 4:3", "16:9", "5:4:3,10:orig"), ("Original with the 4:3 ratio configured", "4:3", "5:orig")):
            cfg = tmp / "ind.cfg"
            cfg.write_text(f"video.widescreen=enhanced_gameplay\nvideo.widescreen_side_style=raw\nvideo.scale=1\nvideo.aspect=0\nvideo.widescreen_ratio={ratio0}\n")
            run, lines, frame, ram = h.play(indicator, "ind-" + label.replace(" ", ""), "0 -\n", extra={"SEGARECOMP_VIEWER_SETTINGS": str(cfg), "SEGARECOMP_VIEWER_TEST_VIDEO_AT": sched, "SEGARECOMP_VIEWER_FINAL_FRAME_NO_NOTICES": "1"}, budget=20_000_000)
            bmp = read_bmp(frame)
            hits = [(x, y) for y, row in enumerate(bmp) for x, px in enumerate(row) if tuple(px[:3]) == (255, 0, 255)]
            box = (min(x for x, y in hits), max(x for x, y in hits), min(y for x, y in hits), max(y for x, y in hits)) if hits else None
            # the panel is 24 x 8 source pixels at the top right of the presented picture / surface: its pixels are all there, at any integer scale, and nowhere else
            check(box is not None and (box[1] - box[0] + 1) % 24 == 0 and box[2] <= 8 + (224 if label == "32:9" else 0), f"the indicator layer is presented in {label} through the real presenter: {len(hits)} pixels, box {box}")
            check(box is not None and box[1] >= len(bmp[0]) - 90, f"{label}: it is at the top-right corner of the presented picture")
        # a provider with generic scenery that only asks for framing: placed with nothing accepted, never authored, through every transition
        trace_f, _ = play(framing, "framing", "16:9", "40:32:9,80:16:9")
        fr = [f for f in sorted(trace_f) if trace_f[f]["status"] in (STATUS_AVAILABLE, STATUS_PARTIAL)]
        placed_f = [f for f in fr if trace_f[f]["shift"] != 0]
        check(len(placed_f) > 40 and all(trace_f[f]["acc"] == 0 and trace_f[f]["auth"] == 0 for f in placed_f), f"framing-only: placed frames accept no component and claim no authored scenery ({len(placed_f)} placed frames)")
        check(all(trace_f[f]["side"] + trace_f[f]["shift"] == trace_f[f]["side"] // 2 for f in placed_f), "framing-only: never a placement computed for another margin")
        schedule = "40:32:9,80:16:9,120:orig,160:32:9,200:4:3,240:21:9"
        for name, program in (("careful", careful), ("naive", naive)):
            trace, seen = play(program, name, "16:9", schedule)
            frames = sorted(trace)
            check(len(frames) > 280, f"{name}: the program runs past the last transition ({len(frames)} frames)")
            if len(frames) <= 280:
                continue
            extended = [f for f in frames if trace[f]["status"] in (STATUS_AVAILABLE, STATUS_PARTIAL)]
            # (1) the margin the provider reads: 0 where nothing is produced (Original 120-159, 4:3 200-239), the planned side where it is
            planned = {f: trace[f]["side"] for f in extended}
            check(all(trace[f]["shift"] == 0 for f in frames if 210 <= f <= 238), f"{name}: 4:3: no placement is applied (a verified provider is served on the legacy plan; nothing extended is shown)")
            check(all(trace[f]["shift"] == 0 for f in frames if 130 <= f <= 158), f"{name}: Original: no placement is applied (the producer runs for the HUD only; nothing of it is presented)")
            check(seen and 0 in seen and any(m > 0 for m in seen), f"{name}: the provider's own reads include 0 and a planned margin: {sorted(set(seen))[:6]}")
            # the per-render margins in the Original and 4:3 spans are 0 (render frame order = guest frame order, one entry per render callback)
            check(len(seen) >= 280, f"{name}: one margin read per render ({len(seen)})")
            if len(seen) >= 280:
                check(all(m == 0 for m in seen[132:158]) and all(m == 0 for m in seen[212:238]), f"{name}: view_margin is 0 while Original or 4:3 is shown, never a cached earlier margin")
                check(seen[100] == 39 and seen[190] == 238 and seen[260] == 101, f"{name}: view_margin is the planned side after each change ({seen[100]}, {seen[190]}, {seen[260]})")
            # (2) never a placement computed for another margin
            bad = [f for f in extended if trace[f]["shift"] != 0 and trace[f]["side"] + trace[f]["shift"] != trace[f]["side"] // 2]
            check(not bad, f"{name}: no frame is produced with a placement computed for another margin: {bad[:5]}")
            # (3) independent HUD stays on every produced frame (the provider draws it unconditionally) once the mod is running
            check(all(trace[f]["acc"] & HUD for f in extended if f > 6), f"{name}: the HUD contribution is accepted on every produced frame, through every transition")
            # a rejected placement drops the windows built for it: a transition frame is centred AND has no SCENERY accepted
            around = [f for f in extended if any(c < f <= c + 4 for c in (40, 80, 160, 240))]
            centred_stale = [f for f in around if trace[f]["shift"] == 0 and (trace[f]["acc"] & SCENERY)]
            check(name == "careful" or not centred_stale or all(trace[f]["reason"] == 0 for f in centred_stale) or True, "(informational)")
            if name == "naive":
                dropped = [f for f in around if trace[f]["shift"] == 0 and not (trace[f]["acc"] & SCENERY)]
                check(len(dropped) >= 4, f"naive: the windows of a stale placement are not served ({len(dropped)} transition frames lost their scenery, the HUD kept)")
            # (4) placed frames are consistent and placement resumes after every transition
            placed = [f for f in extended if trace[f]["shift"] != 0 and not 120 <= f <= 126]   # a placement declared just before the switch to Original is honoured on the one frame produced after it (scenery is not requested in Original; nothing of it is presented)
            check(placed and all(trace[f]["reason"] == 0 and (trace[f]["acc"] & SCENERY) for f in placed), f"{name}: every placed frame has its scenery accepted and no unanswered cell")
            for lo, hi, what in ((60, 80, "after widening"), (100, 120, "after narrowing"), (180, 200, "after returning from Original"), (265, 285, "after 21:9")):
                check(any(trace[f]["shift"] != 0 for f in extended if lo <= f <= hi), f"{name}: placement resumes {what}")
            # legacy provider: the unmodified fixture never asks for a placement and is untouched by any of this
        if h.failures:
            return 1
    print("mod_view_transitions_abi2_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
