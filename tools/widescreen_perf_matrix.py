#!/usr/bin/env python3
"""SEG-139-T005: measure the Genesis widescreen producer cost per aspect with the existing performance report (tools/perf_report.py).

Builds the packaged viewer once for the synthetic display ROM of tests/perf_viewer_integration_test.py (no commercial content), runs it unthrottled with the dummy video
and audio drivers under each widescreen configuration, and prints the per-frame `vdp_compose` / `texture_upload` time read from the runtime reports. Producer cost per frame is
the `emulation` span (all guest execution of a slice, which contains the extension producer; `vdp_compose` covers only the native raster producer) with the extension minus
the figure without it. Nothing here is a new profiler: it only drives the viewer and reads its own report.

  widescreen_perf_matrix.py --cli build/dev/apps/segarecomp/segarecomp --cc cc --tree . --sdl3-include DIR --sdl3-lib DIR [--runs 5] [--legacy-only]
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import genesis_z80_fixture_rom as fx  # noqa: E402
import perf_report as pr  # noqa: E402

FRAME_BUDGET_NS = 16_666_667   # native NTSC frame period


def display_rom() -> bytes:
    m = fx.M68k()
    m.w(0x7000)
    m.branch(0x67, "real")
    m.w(0x4E70)
    m.label("real")
    for word in (0x8144, 0x8B00, 0x8C81, 0x8230, 0x8407):   # display on, H40, scroll plane addresses
        m.w(0x33FC)
        m.w(word)
        m.w(0x00C0)
        m.w(0x0004)
    m.label("spin")
    m.branch(0x60, "spin")
    return fx.build_rom(m.resolve())


CONFIGS = [
    ("none", "video.widescreen=original\n"),
    ("legacy-16:9", "video.widescreen=extended_backgrounds\nvideo.widescreen_width=100\n"),
    ("ratio-16:9", "video.widescreen=extended_backgrounds\nvideo.widescreen_ratio=16:9\n"),
    ("ratio-21:9", "video.widescreen=extended_backgrounds\nvideo.widescreen_ratio=21:9\n"),
    ("ratio-32:9", "video.widescreen=extended_backgrounds\nvideo.widescreen_ratio=32:9\n"),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--tree", required=True, help="product tree whose platforms/genesis runtime and libs/viewer are compiled")
    ap.add_argument("--sdl3-include", required=True)
    ap.add_argument("--sdl3-lib", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--budget", type=int, default=2_000_000)
    ap.add_argument("--optimize", default="2")
    ap.add_argument("--legacy-only", action="store_true", help="only the configurations a pre-SEG-139 tree understands (none, legacy-16:9)")
    ap.add_argument("--json", help="write the result table here")
    a = ap.parse_args()
    configs = [c for c in CONFIGS if not (a.legacy_only and c[0].startswith("ratio-"))]
    tree = pathlib.Path(a.tree).resolve()
    with tempfile.TemporaryDirectory(prefix="segarecomp-wsperf-") as d:
        tmp = pathlib.Path(d)
        (tmp / "display.bin").write_bytes(display_rom())
        built = subprocess.run([a.cli, "build", "--rom", str(tmp / "display.bin"), "--output", str(tmp / "out"), "--cc", a.cc, "--runtime-dir", str(tree / "platforms" / "genesis"),
                                "--optimize", a.optimize, "--runtime-optimize", a.optimize, "--sdl3-include", a.sdl3_include, "--sdl3-lib", a.sdl3_lib], text=True, capture_output=True)
        if built.returncode != 0:
            sys.stderr.write(built.stdout + built.stderr)
            return 1
        base = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LD_LIBRARY_PATH": a.sdl3_lib, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
                "SEGARECOMP_VIEWER_MUTE": "1", "SEGARECOMP_VIEWER_UNTHROTTLED": "1", "SEGARECOMP_PERF_MODE": "detailed"}
        samples: dict[str, list[dict]] = {name: [] for name, _ in configs}
        for run in range(a.runs):                       # interleaved so host drift hits every configuration alike
            for name, text in configs:
                cfg = tmp / f"{name}.cfg"
                cfg.write_text(text)
                report = tmp / f"{name}-{run}.json"
                subprocess.run([str(tmp / "out" / "game"), "--instruction-budget", str(a.budget)], text=True, capture_output=True, timeout=300,
                               env={**base, "SEGARECOMP_VIEWER_SETTINGS": str(cfg), "SEGARECOMP_PERF_REPORT": str(report)})
                rep = pr.load(report)
                der = pr.runtime_derived(rep)["per_frame_ns"]
                samples[name].append({"emulation": der["emulation"], "vdp_compose": der["vdp_compose"], "texture_upload": der.get("texture_upload", 0.0), "frames": rep["runtime"]["frames"]["virtual"]})
        table = {}
        none = statistics.median(s["emulation"] for s in samples["none"])
        for name, _ in configs:
            vc = statistics.median(s["emulation"] for s in samples[name])
            vd = statistics.median(s["vdp_compose"] for s in samples[name])
            tu = statistics.median(s["texture_upload"] for s in samples[name])
            table[name] = {"emulation_ns": round(vc), "vdp_compose_ns": round(vd), "texture_upload_ns": round(tu), "producer_ns": round(vc - none), "producer_share_of_budget": round((vc - none) / FRAME_BUDGET_NS, 4),
                           "runs": a.runs, "frames": samples[name][0]["frames"]}
            print(f"{name:12s} emulation {vc:12.0f} ns/frame  vdp_compose {vd:10.0f}  texture_upload {tu:10.0f}  producer {vc - none:12.0f} ns  = {100 * (vc - none) / FRAME_BUDGET_NS:6.2f}% of a 60 Hz frame")
        if a.json:
            pathlib.Path(a.json).write_text(json.dumps(table, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
