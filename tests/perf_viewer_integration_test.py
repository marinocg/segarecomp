#!/usr/bin/env python3
"""SEG-125 (ADR 0273): the profiler in a generated Genesis SDL3 viewer program (dummy video/audio drivers, software renderer; a project-authored image that enables the
display and spins). A requested report is valid and names the render backend, the host's present/upload spans and the VDP composition inside emulation; a paced session
reports waits and ~native speed; and the guest-visible result (viewer and audio summaries) is identical with the report on, with the overlay on, and with neither.
"""
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402
import perf_report as pr  # noqa: E402


def require(cond, message):
    if not cond:
        raise RuntimeError(message)


def display_rom():
    m = fx.M68k()
    m.w(0x7000)
    m.branch(0x67, "real")
    m.w(0x4E70)
    m.label("real")
    for word in (0x8144, 0x8B00, 0x8C81, 0x8230, 0x8407):   # VDP registers: display on, H40, scroll plane addresses
        m.w(0x33FC)
        m.w(word)
        m.w(0x00C0)
        m.w(0x0004)
    m.label("spin")
    m.branch(0x60, "spin")
    return fx.build_rom(m.resolve())


def summaries(stderr):
    keep = [l for l in stderr.splitlines() if l.startswith(("VIEWER_SUMMARY", "VIEWER_AUDIO_SUMMARY"))]
    # the audio summary carries device-queue counters that depend on the (muted) sink only; the digest, frame count and outcome are the guest-visible parts
    return [re.sub(r'"device":\w+', "", l) for l in keep]


def main():
    cli, cc, _root, sdl_include, sdl_lib = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
    with tempfile.TemporaryDirectory(prefix="segarecomp-perf-viewer-") as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "display.bin"
        rom.write_bytes(display_rom())
        built = subprocess.run([cli, "build", "--rom", str(rom), "--output", str(tmp / "out"), "--cc", cc, "--runtime-dir", str(ROOT / "platforms" / "genesis"), "--optimize", "1",
                                "--runtime-optimize", "1", "--sdl3-include", sdl_include, "--sdl3-lib", sdl_lib], text=True, capture_output=True)
        require(built.returncode == 0, "viewer build: " + built.stdout + built.stderr)
        game = tmp / "out" / "game"
        base = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LD_LIBRARY_PATH": sdl_lib, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
                "SEGARECOMP_VIEWER_MUTE": "1", "SEGARECOMP_VIEWER_UNTHROTTLED": "1"}

        def run(budget, extra=None, settings=""):
            cfg = tmp / f"settings-{abs(hash(settings))}.cfg"
            cfg.write_text(settings)
            env = {**base, "SEGARECOMP_VIEWER_SETTINGS": str(cfg), **(extra or {})}
            return subprocess.run([str(game), "--instruction-budget", str(budget)], text=True, capture_output=True, env=env, timeout=120)

        plain = run(2_000_000)
        require(len(summaries(plain.stderr)) == 2, "plain viewer run: " + plain.stderr[-400:])   # a finished budget exits non-zero like every bounded generated program: compared, not assumed
        report = tmp / "viewer.json"
        profiled = run(2_000_000, {"SEGARECOMP_PERF_REPORT": str(report), "SEGARECOMP_PERF_MODE": "detailed"})
        require(summaries(profiled.stderr) == summaries(plain.stderr) and profiled.returncode == plain.returncode, "a report request changes nothing the guest or audio digest can see")
        overlay = run(2_000_000, settings="video.perf_overlay=2\n")
        require(summaries(overlay.stderr) == summaries(plain.stderr) and overlay.returncode == plain.returncode, "the overlay changes nothing the guest or audio digest can see")

        d = pr.load(report)
        r = d["runtime"]
        by_name = {s["name"]: s for s in r["spans"]}
        require(d["status"] == "complete" and d["config"]["entry"] == "viewer" and d["config"]["renderer"] == "software" and d["config"]["paced"] is False, "viewer report identity: " + json.dumps(d["config"]))
        require(r["frames"]["virtual"] > 50 and r["frames"]["presented"] > 40, "frames: " + json.dumps(r["frames"]))
        for name in ("emulation", "vdp_compose", "rgb_convert", "texture_upload", "present"):
            require(by_name[name]["count"] > 0 and by_name[name]["total_ns"] > 0, f"span {name} measured")
        require(by_name["vdp_compose"]["total_ns"] <= by_name["emulation"]["total_ns"], "vdp_compose is contained in emulation")
        require(by_name["texture_upload"]["total_ns"] <= by_name["present"]["total_ns"], "texture_upload is contained in present")
        require(by_name["vdp_compose"]["class"] == "inclusive" and by_name["pacing_wait"]["class"] == "exclusive", "span classes")
        require(any(u["metric"] == "gpu_time" for u in d["unavailable"]), "GPU time is declared unavailable")
        require(r["guest"]["master_ticks"] > 0 and r["counters"]["m68k_dispatches"] == 2_000_000, "guest counters stay separate")

        # paced: the pacer waits and the session runs at about the native rate (a 1.2 s guest span)
        paced_report = tmp / "paced.json"
        paced_env = {"SEGARECOMP_PERF_REPORT": str(paced_report), "SEGARECOMP_VIEWER_UNTHROTTLED": "0"}
        paced = run(1_300_000, paced_env)
        require(paced.returncode == plain.returncode, "paced run: " + paced.stderr[-300:])
        p = pr.load(paced_report)
        speed = pr.runtime_derived(p)["speed"]
        pacing, frames = p["runtime"]["pacing"], p["runtime"]["frames"]["virtual"]
        require(p["config"]["paced"] is True and frames > 50, "paced session identity")
        # every boundary either waited for its deadline or found it already past - on a busy CI host the latter happens (and is exactly what the collector must report)
        require(pacing["waits"] + pacing["missed_deadlines"] >= frames - 5, "every paced frame is accounted for as a wait or a missed deadline: " + json.dumps(pacing))
        if pacing["missed_deadlines"] == 0:
            require(0.8 < speed < 1.2, f"an on-time paced session runs at ~100%: {speed}")
        else:
            require(speed is not None and speed < 1.2, f"a session that missed deadlines never runs faster than native: {speed}")
        require(p["runtime"]["frame_work"]["avg_ns"] <= p["runtime"]["frame_interval"]["avg_ns"] and (pacing["waits"] == 0 or p["runtime"]["frame_work"]["avg_ns"] < p["runtime"]["frame_interval"]["avg_ns"]), "work excludes the pacing wait")
        # robustness: malformed option fails closed before the window opens
        bad = run(1000, {"SEGARECOMP_PERF_MODE": "turbo", "SEGARECOMP_PERF_REPORT": str(tmp / "bad.json")})
        require(bad.returncode == 3, "malformed viewer perf option exits 3")

        # ---- Master System: the same collector at its own frame loop, its own counter names (cross-platform fields)
        sys.path.insert(0, str(ROOT / "tests"))
        import sms_e2e_common as c   # noqa: E402
        from sms_e2e_common import builder   # noqa: E402
        builder.main(["--out", str(tmp / "sms"), "--fixture", c.FIXTURE])
        sms_rom = tmp / "sms" / (c.FIXTURE + ".sms")
        built = subprocess.run([cli, "build", "--rom", str(sms_rom), "--output", str(tmp / "sms-out"), "--cc", cc, "--optimize", "1", "--runtime-dir", str(ROOT / "platforms" / "master-system"),
                                "--mapper", "sega", "--sdl3-include", sdl_include, "--sdl3-lib", sdl_lib], text=True, capture_output=True)
        require(built.returncode == 0, "SMS viewer build: " + built.stdout + built.stderr)
        sms_report = tmp / "sms.json"

        def run_sms(extra=None):
            env = {**base, "SEGARECOMP_VIEWER_SETTINGS": str(tmp / "sms-settings.cfg"), **(extra or {})}
            return subprocess.run([str(tmp / "sms-out" / "game"), "--viewer-unthrottled", "--viewer-mute", "--viewer-frames", "150"], text=True, capture_output=True, env=env, timeout=120)
        sms_plain, sms_prof = run_sms(), run_sms({"SEGARECOMP_PERF_REPORT": str(sms_report), "SEGARECOMP_PERF_MODE": "detailed"})
        line = lambda r: next(l for l in r.stdout.splitlines() if l.startswith("viewer outcome"))   # noqa: E731
        require(sms_plain.returncode == sms_prof.returncode and line(sms_plain) == line(sms_prof), "SMS: the guest-visible result is identical with the collector on")
        sd = pr.load(sms_report)
        sr = sd["runtime"]
        require(sd["platform"] == "master_system" and sd["status"] == "complete" and sr["outcome"] == "frame_limit" and sd["config"]["entry"] == "viewer", "SMS report identity")
        require(sr["frames"]["virtual"] >= 140 and sr["frames"]["presented"] >= 140, "SMS frames: " + json.dumps(sr["frames"]))
        require(sr["counters"]["guest_frames"] == 150 and sr["counters"]["z80_cycles"] > 0, "SMS counters use SMS names: " + json.dumps(sr["counters"]))
        sspans = {x["name"]: x for x in sr["spans"]}
        require(sspans["emulation"]["count"] > 0 and sspans["present"]["count"] > 0 and sspans["texture_upload"]["count"] > 0, "SMS spans measured")
        require(any(u["metric"] == "guest_master_ticks" for u in sd["unavailable"]), "SMS: unavailable fields are explicit, not zero-filled")
        print("perf_viewer_integration_test: ok")


if __name__ == "__main__":
    main()
