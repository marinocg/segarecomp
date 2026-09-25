#!/usr/bin/env python3
"""SEG-007-T254: end-to-end generated-program viewer integration.

Project-authored synthetic ROM, no commercial input, no real-time waiting
(--viewer-unthrottled semantics, SDL dummy video driver). Proves:
generated program -> real generated dispatcher/GenesisRuntime -> T255 live frame
observer -> T254 viewer runner -> SDL3 presenter receives a completed frame.
Also proves the headless build of the same generated program is unchanged
(no SDL/viewer symbols, normal sanitized report).
"""
import hashlib
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location("genesis_startup_bridge", ROOT.parent / "tools" / "genesis_startup_bridge.py")
_bridge = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_bridge)


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise RuntimeError(msg)


SEED = ("runtime.irq6_handler_present = 1; ")


def main() -> int:
    binary, compiler, root = (pathlib.Path(v).resolve() for v in sys.argv[1:4])
    sdl3 = _bridge.find_sdl3()
    if sdl3 is None:
        print("SKIP: SDL3 not available")
        return 0
    emitted = subprocess.run([str(binary), "--emit-general-startup-bridge-irq6-retirement-redirect"],
                             text=True, capture_output=True)
    require(emitted.returncode == 0 and SEED in emitted.stdout, "frontend fixture emitted")
    # Seed a renderable VDP and place the first retirement across the virtual VBlank onset.
    seeded = emitted.stdout.replace(SEED, SEED +
        "runtime.sr = 0x2000; { uint16_t *g = runtime.devices.vdp.registers; g[1] = 0x24; g[2] = 0x30; "
        "g[4] = 0x04; g[5] = 0x28; g[11] = 0x04; g[12] = 0x81; g[16] = 0x01; } "
        "runtime.devices.vdp.cram[0] = 0x0E; "
        "runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 56U; ", 1)
    flags = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-viewer-int-") as d:
        tmp = pathlib.Path(d)
        source = tmp / "generated.c"
        source.write_text(seeded)
        viewer_exe = tmp / "viewer"
        st, _, _ = _bridge._compile_viewer_executable(flags, sdl3, root, [source], viewer_exe, b"")
        require(st == 0, f"viewer build must succeed (status {st})")
        headless_exe = tmp / "headless"
        hb = subprocess.run(flags + ["-I", str(root / "platforms/genesis/runtime"), "-o", str(headless_exe), str(source),
                                     str(root / "platforms/genesis/runtime/runtime.c")], text=True, capture_output=True)
        require(hb.returncode == 0, "headless build must succeed: " + hb.stderr)

        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SEGARECOMP_VIEWER_UNTHROTTLED="1",
                   SEGARECOMP_VIEWER_SLICE="2")
        run = subprocess.run([str(viewer_exe), "--instruction-budget", "8"], text=True,
                             capture_output=True, cwd=root, env=env)
        line = [l for l in run.stderr.splitlines() if l.startswith("VIEWER_SUMMARY ")]
        require(len(line) == 1, "viewer summary emitted: " + run.stderr[-300:])
        summary = json.loads(line[0].split(" ", 1)[1])
        require(summary["viewer_opened"] and summary["frame_publication_observed"] and summary["frame_presented"],
                "frame must be published and presented: " + line[0])
        require(summary["outcome"] == "guest_stop", "guest stop stays a guest stop: " + line[0])
        require(summary["execution"] == "generated-native", "generated-native")

        bad = subprocess.run([str(viewer_exe), "--instruction-budget", "8"], text=True, capture_output=True,
                             cwd=root, env=dict(env, SEGARECOMP_VIEWER_SLICE="0"))
        require(bad.returncode == 3 and "VIEWER_SUMMARY" not in bad.stderr, "malformed slice rejected early")

        head = subprocess.run([str(headless_exe), "--instruction-budget", "8"], text=True,
                              capture_output=True, cwd=root)
        require("VIEWER_SUMMARY" not in head.stderr, "headless emits no viewer output")
        # Headless and viewer agree on the guest's sanitized stop.
        require(head.stdout == run.stdout, "viewer and headless sanitized guest report identical")
        nm = subprocess.run(["nm", str(headless_exe)], text=True, capture_output=True).stdout
        require("SDL" not in nm and "viewer" not in nm.lower(), "headless binary links no viewer/SDL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
