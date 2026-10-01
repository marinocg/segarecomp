#!/usr/bin/env python3
"""SEG-009-T007: generated-native PSG fixture: a Z80 program writes PSG registers and the headless program's PCM digest
equals the digest of an independent model fed the statically known instruction-start T-states.

usage: sms_psg_native_test.py <sms_image_emitter> <cc> <product-root>

The fixture ROM is built here from the public Assembler of tools/sms_fixture_rom.py (read-only use); it is project-authored
synthetic code. Expected write times come from the published Z80 T-states (DI 4, LD r,n 7, OUT (n),A 11, OUT (C),A 12,
DJNZ 13/8, NOP 4), expected PCM from tests/sms_psg_model.py. The test checks audio.pcm bytes, the run and per-frame
SHA-256 lines, byte-identical artifacts across runs and across fixed/pseudo-random slicing, PSG port mirrors, and the typed
stop for a data byte before any latch byte.
"""
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sms_cc_cache  # noqa: E402
sys.path.insert(0, str(HERE))
import sms_fixture_rom as builder  # noqa: E402
import sms_psg_model as model  # noqa: E402

PLATFORM = ROOT / "platforms" / "master-system"
RUNTIME = PLATFORM / "runtime"
PSG_LIB = ROOT / "libs" / "device" / "sega" / "psg"
Z80_INCLUDE = ROOT / "libs" / "codegen" / "c11" / "include"
STRICT = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-D_CRT_SECURE_NO_WARNINGS"]
SOURCES = [RUNTIME / n for n in ("sms_memory.c", "sms_sha256.c", "sms_input.c", "sms_machine.c", "sms_psg.c", "sms_pad.c", "sms_vdp.c")] + [
    PSG_LIB / "src" / "sn76489.c", PLATFORM / "headless" / "sms_audio.c", PLATFORM / "headless" / "sms_headless.c", PLATFORM / "headless" / "sms_devices_none.c"]
FRAME = 59736
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)
        print("FAIL:", message)


def run(cmd, timeout=300):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout)


class Program:
    """Straight-line Z80 source with the instruction-start T-state of every PSG write."""

    def __init__(self):
        self.lines, self.t, self.writes = ["di"], 4, []

    def psg(self, byte, port=0x7F):
        self.lines.append("ld a,%d" % byte)
        self.t += 7
        self.lines.append("out (0x%02X),a" % port)
        self.writes.append((self.t, byte))
        self.t += 11

    def psg_c(self, byte, port):  # OUT (C),A: 12 T, port from the C register
        self.lines += ["ld c,%d" % port, "ld a,%d" % byte, "out (c),a"]
        self.t += 14
        self.writes.append((self.t, byte))
        self.t += 12

    def delay(self, n):  # LD B,n (7) + DJNZ loop (13 x (n - 1) + 8)
        label = "d%d" % len(self.lines)
        self.lines += ["ld b,%d" % n, "%s: djnz %s" % (label, label)]
        self.t += 7 + 13 * (n - 1) + 8

    def nop(self):
        self.lines.append("nop")
        self.t += 4

    def source(self):
        return ".org 0x0000\n" + "\n".join("        " + l if ":" not in l else l for l in self.lines) + "\ndone:   halt\n        jr done\n"


def tone_program():
    p = Program()
    p.psg(0x90)                     # tone 0 attenuation 0
    p.psg(0x8E); p.psg(0x0F)        # tone 0 period $0FE (440 Hz)
    p.delay(200)
    p.psg(0xB4)                     # tone 1 attenuation 4
    p.psg(0xA3); p.psg(0x1F)        # tone 1 period $1F3
    p.nop(); p.psg(0xD8)            # tone 2 attenuation 8 (a write one nop later: a different tick phase)
    p.psg(0xC1); p.psg(0x00)        # tone 2 period 1: toggles every tick
    p.delay(120)
    p.psg(0xF2)                     # noise attenuation 2
    p.psg(0xE7)                     # white noise, tone 2 coupled (rate 3)
    p.delay(250)
    p.psg_c(0xE5, 0x7E)             # white noise rate 1 through the mirror $7E
    p.delay(250)
    p.psg_c(0xE0, 0x41)             # periodic noise rate 0 through the mirror $41
    p.delay(255)
    p.psg(0x9F); p.psg(0xBF)        # silence tones 0 and 1
    p.delay(255)
    p.psg(0xFF)                     # silence the noise
    p.delay(255)
    return p


def build_rom(tmp, name, program):
    image, _ = builder.Assembler(program.source()).assemble()
    rom = builder.build_rom(0x8000, image, {})
    (tmp / (name + ".sms")).write_bytes(rom)
    (tmp / (name + ".mapper.json")).write_text(json.dumps({"fixture": name, "size": len(rom), "mapper": "rom_only",
                                                           "sha256": hashlib.sha256(rom).hexdigest(),
                                                           "declaration_source": "fixture_builder"}), encoding="utf-8")


def build_exe(tmp, name):
    out = tmp / ("gen_" + name)
    r = run([EMITTER, tmp / (name + ".sms"), out, "sms", "--manifest", tmp / (name + ".mapper.json")])
    check(r.returncode == 0, "%s: emission failed: %s" % (name, (r.stdout + r.stderr)[:300]))
    if r.returncode != 0:
        return None
    units = [out / u for u in (out / "sms.units").read_text().split()]
    objs = []
    for src in units + SOURCES:
        obj = out / (src.stem + ".o")
        c = sms_cc_cache.compile_object([CC, *STRICT, "-O0", "-I", Z80_INCLUDE, "-I", RUNTIME, "-I", PLATFORM / "headless", "-I", PSG_LIB / "include", "-I", out, "-c", src, "-o", obj])
        check(c.returncode == 0, "%s: %s did not compile as strict C11: %s" % (name, src.name, c.stderr[:1500]))
        if c.returncode != 0:
            return None
        objs.append(obj)
    exe = out / "psg.exe"
    link = run([CC, *objs, "-o", exe])
    check(link.returncode == 0, "%s: link failed: %s" % (name, link.stderr[:500]))
    return exe


def execute(exe, tmp, tag, *args):
    art = tmp / ("art_" + tag)
    art.mkdir(exist_ok=True)
    r = run([exe, *args, "--artifacts", art], timeout=120)
    return r, art


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms-psg-"))
    prog = tone_program()
    build_rom(tmp, "psg_tone", prog)
    bad = Program()
    bad.lines += ["ld a,0x0F", "out (0x7F),a"]   # a data byte before any latch byte
    bad.t = 4 + 7
    build_rom(tmp, "psg_bad", bad)
    exe, bad_exe = build_exe(tmp, "psg_tone"), build_exe(tmp, "psg_bad")
    if FAILED:
        return finish()

    frames = 4
    r, art = execute(exe, tmp, "a", "--frames", str(frames))
    check(r.returncode == 0, "fixture run failed: %s" % (r.stdout + r.stderr)[:300])
    status = json.loads((art / "status.json").read_text())
    end = status["cycles"]
    check(end >= frames * FRAME, "run did not reach the frame bound")
    pcm = (art / "audio.pcm").read_bytes()
    expected = model.pcm_for(prog.writes, end)
    expected_bytes = b"".join(struct.pack("<h", s) for s in expected)
    check(pcm == expected_bytes, "audio.pcm differs from the model (%d vs %d bytes)" % (len(pcm), len(expected_bytes)))
    check(len(set(expected)) > 20, "the fixture timeline must exercise many distinct sample values")
    sums = (art / "audio.sha256").read_text().splitlines()
    run_line = sums[-1].split()
    check(run_line == ["run", str(len(expected)), hashlib.sha256(expected_bytes).hexdigest()], "run digest line: %s" % run_line)
    by_frame = {}
    for k, s in enumerate(expected):
        by_frame.setdefault(model.sample_start(k) // FRAME, []).append(s)
    want = ["frame %d %d %d %s" % (f, sum(len(by_frame[g]) for g in by_frame if g < f), len(v),
                                   hashlib.sha256(b"".join(struct.pack("<h", s) for s in v)).hexdigest())
            for f, v in sorted(by_frame.items())]
    check(sums[:-1] == want, "per-frame digest lines differ from the model")
    check(len(want) in (frames, frames + 1), "per-frame lines cover the run: %d" % len(want))
    print("native: %d writes, %d samples, %d frame lines, PCM digest %s" % (len(prog.writes), len(expected), len(want),
                                                                          run_line[2][:16]))

    # determinism across runs and slicing: PCM, per-frame digests and the machine-state digest (which includes the PSG)
    ref = {n: (art / n).read_bytes() for n in ("audio.pcm", "audio.sha256", "state.sha256")}
    for tag, extra in (("b", []), ("c", ["--slice-cycles", "997"]), ("d", ["--slice-cycles", "16"]), ("e", ["--slice-seed", "7"]),
                       ("f", ["--slice-seed", "12345"])):
        r2, art2 = execute(exe, tmp, tag, "--frames", str(frames), *extra)
        check(r2.returncode == 0, "run %s failed" % tag)
        for n, data in ref.items():
            check((art2 / n).read_bytes() == data, "run %s: %s differs (runs/slices must be byte-identical)" % (tag, n))
    print("native: PCM, frame digests and state digest identical across 5 re-runs and slice splits")

    # cycle-budget stop: the stream is complete to the stop boundary
    r3, art3 = execute(exe, tmp, "g", "--cycle-budget", "9000")
    end3 = json.loads((art3 / "status.json").read_text())["cycles"]
    check(r3.returncode == 2 and (art3 / "audio.pcm").read_bytes() == b"".join(struct.pack("<h", s) for s in model.pcm_for(prog.writes, end3)),
          "cycle-budget run PCM differs from the model")

    # U5: data byte before any latch byte stops fail-closed with the typed error at its instruction-start T-state
    rb, artb = execute(bad_exe, tmp, "bad", "--frames", "2")
    st = json.loads((artb / "status.json").read_text())
    check(rb.returncode == 4 and st["sms_error"] == "SMS_ERROR_PSG_DATA_BEFORE_LATCH" and st["error_value"] == 0x0F and
          st["error_cycles"] == 11 and st["stop"] == "platform_error",
          "data before latch must stop with SMS_ERROR_PSG_DATA_BEFORE_LATCH at T 11: %s %s" % (rb.returncode, st))
    return finish()


def finish():
    if FAILED:
        print("sms psg native: %d failures" % len(FAILED))
        return 1
    print("sms psg native: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
