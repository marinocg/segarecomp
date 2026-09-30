"""SEG-009-T004: shared generated-native build/run helpers for the SMS VDP tests (sms_vdp_test.py, sms_vdp_oracle_test.py).

Builds a project-authored fixture through the SMS generation route (tests/tools/sms_image_emitter), compiles the emitted
image with the SMS runtime, the PSG device library, the real headless driver (platforms/master-system/headless) and the
VDP device wiring `sms_devices_vdp.c` as strict C11, and runs it with a finite --frames bound.
"""
import concurrent.futures
import json
import os
import pathlib
import subprocess

STRICT = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-D_CRT_SECURE_NO_WARNINGS"]


class Native:
    def __init__(self, emitter, cc, root, tmp):
        self.emitter, self.cc, self.root, self.tmp = emitter, cc, pathlib.Path(root).resolve(), pathlib.Path(tmp)
        platform = self.root / "platforms" / "master-system"
        self.include = [self.root / "libs" / "codegen" / "c11" / "include", platform / "runtime", platform / "headless",
                        self.root / "libs" / "device" / "sega" / "psg" / "include"]
        self.sources = ([platform / "runtime" / n for n in ("sms_memory.c", "sms_sha256.c", "sms_input.c", "sms_machine.c",
                                                            "sms_psg.c", "sms_pad.c", "sms_vdp.c", "sms_render.c")]
                        + [self.root / "libs" / "device" / "sega" / "psg" / "src" / "sn76489.c"]
                        + [platform / "headless" / n for n in ("sms_audio.c", "sms_headless.c", "sms_devices_vdp.c")])
        self.failures = []
        self.objects = {}

    def run(self, cmd, timeout=600):
        return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout)

    def compile(self, src, out, extra=()):
        obj = out / (src.stem + ".o")
        c = self.run([self.cc, *STRICT, "-O0", *[a for d in self.include for a in ("-I", d)], "-I", out, *extra, "-c", src, "-o", obj])
        if c.returncode != 0:
            self.failures.append("%s did not compile as strict C11: %s" % (src.name, c.stderr[:1200]))
            return None
        return obj

    def build(self, roms, name):
        """Returns the executable of fixture `name` (ROM + mapper manifest in `roms`), or None after recording a failure."""
        out = self.tmp / ("gen_" + name)
        r = self.run([self.emitter, roms / (name + ".sms"), out, "sms", "--manifest", roms / (name + ".mapper.json")])
        if r.returncode != 0:
            self.failures.append("%s: emission failed: %s" % (name, (r.stdout + r.stderr)[:300]))
            return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, os.cpu_count() or 1))) as pool:
            objs = list(pool.map(lambda u: self.compile(out / u, out), (out / "sms.units").read_text().split()))
        if any(o is None for o in objs):
            return None
        for src in self.sources:  # runtime objects are identical for every fixture: compile once
            if src not in self.objects:
                shared = self.tmp / "shared"
                shared.mkdir(exist_ok=True)
                self.objects[src] = self.compile(src, shared)
            if self.objects[src] is None:
                return None
            objs.append(self.objects[src])
        exe = out / "sms_vdp.exe"
        link = self.run([self.cc, *objs, "-o", exe])
        if link.returncode != 0:
            self.failures.append("%s: link failed: %s" % (name, link.stderr[:800]))
            return None
        return exe

    def execute(self, exe, tag, *args, timeout=300):
        artifacts = self.tmp / ("art_" + tag)
        artifacts.mkdir(exist_ok=True)
        r = self.run([exe, *args, "--artifacts", artifacts], timeout=timeout)
        res = {"code": r.returncode, "stdout": r.stdout, "dir": artifacts}
        if (artifacts / "status.json").exists():
            res["status"] = json.loads((artifacts / "status.json").read_text())
            res["digest"] = (artifacts / "state.sha256").read_text().strip()
            res["irq"] = [tuple(l.split()) for l in (artifacts / "irq.trace").read_text().splitlines()]
            res["ram"] = (artifacts / "ram.bin").read_bytes()
            if (artifacts / "vdp.trace").exists():
                res["vdp_trace"] = [tuple(l.split()) for l in (artifacts / "vdp.trace").read_text().splitlines()]
                res["vram"] = (artifacts / "vram.bin").read_bytes()
                res["cram"] = (artifacts / "cram.bin").read_bytes()
                res["vdp"] = json.loads((artifacts / "vdp.json").read_text())
        return res


def write_straddle_flip(ram, offset, steps=64):
    """Analysis of the `vdp_straddle_write` fixture (tools/sms_fixture_rom.py). `ram` holds $C100 at `offset`. Trial n logs the
    V counter of the two interrupts after its R10 write: a gap of 4 lines means the write reached R10 before the next
    line's underflow reload, a gap of 1 line after it. Trials whose interrupts straddle the frame end are skipped.
    Returns (first n with gap 1, True when every valid trial is a clean 4...4 1...1 step)."""
    gaps = []
    for n in range(steps):
        first, second = ram[offset + n], ram[offset + 0x40 + n]
        gaps.append(second - first if 0 < first <= 186 else None)
    valid = [(n, g) for n, g in enumerate(gaps) if g is not None]
    flip = next((n for n, g in valid if g == 1), None)
    clean = flip is not None and all(g == (1 if n >= flip else 4) for n, g in valid)
    return flip, clean
