"""Shared helpers of the Genesis sound tests (SEG-145-T003 / SEG-145-T008): the project-authored fixture of platforms/genesis/sdk/examples/sound, real generated-native builds by the product CLI and
bounded headless runs under the frame capture (explicit instruction budget and wall-clock timeout) with the canonical PCM artifact. No commercial input."""
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path

from mod_test_support import sha, write  # noqa: F401  (re-exported)

PAD = "10:1:0x20,12:1:0x00,20:1:0x10,22:1:0x00,32:1:0x08,34:1:0x00"   # C at frame 10, B at 20, Right at 32
FRAMES = 64
BYTES_PER_FRAME = 4 * 735   # about one video frame of 44.1 kHz stereo s16 (used only to name a time, never for an exact boundary)


def load_fixture(root):
    path = Path(root) / "platforms/genesis/sdk/examples/sound/fixture_rom.py"
    spec = importlib.util.spec_from_file_location("sound_fixture_rom", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Programs:
    def __init__(self, cli, sdk, cc, work):
        self.cli, self.sdk, self.cc, self.work = cli, Path(sdk), cc, Path(work)

    def build(self, name, image, package=None, identity=None, native=False):
        out = self.work / name
        args = ["build", "--rom", image, "--output", out, "--cc", self.cc, "--runtime-dir", self.sdk / "genesis", "--optimize", "0", "--runtime-optimize", "1"]
        if package is not None:
            args += (["--native-package", package, "--native-identity", identity] if native else ["--luau-package", package, "--luau-identity", identity])
        p = subprocess.run([self.cli.exe, *map(str, args)], text=True, capture_output=True, timeout=900)
        status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
        return p, out, status

    def run(self, out, pad=PAD, frames=FRAMES, first=1, extra=None, tag="run"):
        """(exit status, {report tag: [records]}, canonical PCM bytes). The PCM artifact is checked against the digest the program reported."""
        work = self.work / ("cap-" + out.name + "-" + tag)
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir()
        pcm = work / "audio.pcm"
        env = dict(os.environ, SEGARECOMP_CAPTURE_DIR=str(work), SEGARECOMP_CAPTURE_FIRST=str(first), SEGARECOMP_CAPTURE_COUNT=str(frames), SEGARECOMP_CAPTURE_SLICE="16",
                   SEGARECOMP_CAPTURE_RAM="32:3", SEGARECOMP_PAD_SCRIPT=pad, SEGARECOMP_SOUND_SUMMARY="1", SEGARECOMP_AUDIO_PCM_OUT=str(pcm))
        env.update(extra or {})
        p = subprocess.run([str(out / "game"), "--instruction-budget", "20000000"], text=True, capture_output=True, env=env, timeout=120)
        lines = {}
        for line in p.stderr.splitlines():
            name, _, rest = line.partition(" ")
            if name in ("CAPTURE_SUMMARY", "SOUND_SUMMARY", "MOD_SUMMARY", "MOD_LOADED", "MOD_LOG", "MOD_REFUSED"):
                lines.setdefault(name, []).append(json.loads(rest))
        data = pcm.read_bytes() if pcm.exists() else b""
        if "SOUND_SUMMARY" in lines:
            reported = lines["SOUND_SUMMARY"][0]["audio"]["sha256"]
            self.cli.check(hashlib.sha256(data + (len(data) // 4).to_bytes(8, "little")).hexdigest() == reported, f"{out.name}: the PCM file matches the reported digest")
        return p.returncode, lines, data


def first_difference(a, b):
    """Byte offset of the first difference of two PCM streams, or None."""
    for at in range(0, min(len(a), len(b)), 4096):
        if a[at:at + 4096] != b[at:at + 4096]:
            return at + next(i for i in range(len(a[at:at + 4096])) if a[at + i] != b[at + i])
    return None if len(a) == len(b) else min(len(a), len(b))


def distinct(trace):
    """The sequence of distinct consecutive values of the third sampled byte (the game's "last accepted command") over a capture's per-frame RAM samples, with the index of each change."""
    out = []
    for index, sample in enumerate(trace):
        value = int(sample[4:6], 16)
        if not out or out[-1][0] != value:
            out.append((value, index))
    return out
