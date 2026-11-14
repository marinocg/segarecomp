"""Shared helpers of the F28 viewer-integration tests (SEG-138-T010/T011, SEG-140-T008/T009/T010): build a generated-native Genesis program of a FABRICATED fixture (Vanilla, headless or SDL3 viewer,
with or without a package), run it under the real SDL3 viewer with dummy video/audio, an explicit instruction budget and a wall-clock timeout, and read back the final frame and work RAM through the
test-only seams (SEGARECOMP_VIEWER_FINAL_FRAME / SEGARECOMP_VIEWER_FINAL_RAM). Nothing here reads a commercial image."""
import hashlib
import json
import os
import pathlib
import subprocess

TAGS = ("VIEWER_SUMMARY", "VIEWER_NATIVE_SUMMARY", "VIEWER_STATE", "MOD_LOADED", "MOD_SUMMARY", "MOD_LOG", "MOD_REFUSED", "RUNTIME_NOTES", "VIEWER_FINAL_FRAME", "VIEWER_FINAL_RAM", "VIEWER_COVERAGE")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_bmp(path):
    data = pathlib.Path(path).read_bytes()
    offset = int.from_bytes(data[10:14], "little")
    width, height = int.from_bytes(data[18:22], "little", signed=True), int.from_bytes(data[22:26], "little", signed=True)
    bpp = int.from_bytes(data[28:30], "little")
    assert data[:2] == b"BM" and bpp in (24, 32), (bpp, data[:2])
    step = bpp // 8
    stride = (width * step + 3) & ~3
    rows = []
    for y in range(abs(height)):
        src = (abs(height) - 1 - y) if height > 0 else y
        raw = data[offset + src * stride: offset + src * stride + width * step]
        rows.append([(raw[x * step + 2], raw[x * step + 1], raw[x * step]) for x in range(width)])   # BGR(A) -> RGB
    return rows


class Harness:
    def __init__(self, cli, compiler, root, sdl, tmp):
        self.cli, self.compiler, self.root, self.sdl, self.tmp = cli, compiler, pathlib.Path(root), pathlib.Path(sdl), pathlib.Path(tmp)
        self.home = self.tmp / "home"
        self.home.mkdir(exist_ok=True)
        self.failures = []

    def check(self, cond, what):
        if not cond:
            self.failures.append(what)
            print("FAIL:", what)
        return cond

    def run_cli(self, *args, timeout=900):
        return subprocess.run([self.cli, *map(str, args)], text=True, capture_output=True, timeout=timeout)

    def build(self, rom, out, package=None, identity=None, viewer=True, extra=(), cc=None):
        args = ["build", "--rom", rom, "--output", out, "--cc", cc or self.compiler, "--runtime-dir", self.root / "platforms/genesis", "--optimize", "0", "--runtime-optimize", "1"]
        if viewer:
            args += ["--sdl3-include", self.sdl / "include", "--sdl3-lib", self.sdl / "lib", "--test-frame-seam", "1"]
        if package is not None:
            args += ["--luau-package", package, "--luau-identity", identity]
        return self.run_cli(*args, *extra)

    def status(self, out):
        return json.loads((pathlib.Path(out) / "status.json").read_text())

    def env(self, **more):
        e = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_VIEWER_UNTHROTTLED="1", SEGARECOMP_VIEWER_MUTE="1",
                 SEGARECOMP_VIEWER_NO_STOP_SCREEN="1", SEGARECOMP_REWIND="off", HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / "config"), XDG_DATA_HOME=str(self.home / "data"),
                 LD_LIBRARY_PATH=str(self.sdl / "lib") + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""), SEGARECOMP_VIEWER_SETTINGS=str(self.tmp / "viewer-settings.cfg"))
        e.update(more)
        return e

    def play(self, out, name, pad=None, save=None, identity=None, rom_sha=None, budget=4_000_000, extra=None):
        """Runs the viewer program once. `pad` = text of SEGARECOMP_VIEWER_PAD_SCRIPT ("<poll> <keys>" lines). Returns (process, parsed tagged lines, frame bmp path, ram bin path)."""
        frame, ram = self.tmp / (name + ".bmp"), self.tmp / (name + ".ram")
        for p in (frame, ram):
            p.unlink(missing_ok=True)
        extras = dict(SEGARECOMP_VIEWER_FINAL_FRAME=str(frame), SEGARECOMP_VIEWER_FINAL_RAM=str(ram))
        if pad is not None:
            pad_file = self.tmp / (name + ".pad")
            pad_file.write_text(pad)
            extras["SEGARECOMP_VIEWER_PAD_SCRIPT"] = str(pad_file)
        if save is not None:
            pathlib.Path(save).mkdir(parents=True, exist_ok=True)
            extras.update(SEGARECOMP_SAVE_DIR=str(save), SEGARECOMP_ROM_SHA256=rom_sha, REWIND_INTERVAL="100")
            extras["SEGARECOMP_REWIND_INTERVAL_MS"] = "100"
            extras.pop("REWIND_INTERVAL")
            extras["SEGARECOMP_REWIND"] = "on"
            if identity:
                extras["SEGARECOMP_SAVE_VARIANT"] = identity
        extras.update(extra or {})
        run = subprocess.run([str(pathlib.Path(out) / "game"), "--instruction-budget", str(budget)], text=True, capture_output=True, env=self.env(**extras), timeout=240, cwd=self.tmp)
        lines = {}
        for line in run.stderr.splitlines():
            tag, _, rest = line.partition(" ")
            if tag in TAGS:
                try:
                    lines.setdefault(tag, []).append(json.loads(rest))
                except ValueError:
                    lines.setdefault(tag, []).append({"raw": rest})
        return run, lines, frame, ram
