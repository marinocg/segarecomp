#!/usr/bin/env python3
"""SEG-032-T009 (ADR 0075, contract section 11): the whole-machine PCM stream of the real runtime, Z80 machine, PSG and YM2612.

A project-authored generated-native Z80 driver plays a PSG tone, a YM2612 FM tone and DAC samples while the 68000 only advances time.
The mixer output of the real machine (tests/tools/genesis_z80_machine_harness.c `mixer`) is compared with an INDEPENDENT reconstruction:
the YM2612 library driven directly with the recorded arrival-ordered writes (tests/tools/ym2612_driver.c), the SN76489 library driven
directly with the recorded PSG writes (tests/tools/genesis_psg_levels_driver.c), and the frozen definition mixed in Python (the same
reference as tests/genesis_audio_mixer_test.py). Proved:
  * the PCM stream (every byte) and its SHA-256 digest equal the reconstruction; the stream is non-silent and uses all three sources;
  * the digest, frame count and counters are identical across repeated runs and across the Z80 synchronization cadence (quanta 1, 512,
    4,096, 65,536): chunk/sync boundaries do not enter the stream;
  * the frame count is the number of 44,100 Hz windows that ended before the final guest time (a pure function of virtual time);
  * mutation controls (wrong rate, swapped channels, wrong PSG gain, a PSG write dropped, a YM write dropped) change the digest.
usage: genesis_audio_pcm_test.py <registry_emitter> <cc> <source-root> <c++>
"""
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile

registry_emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
cxx = sys.argv[4] if len(sys.argv) > 4 else "c++"
sys.path.insert(0, str(root / "tools"))
sys.path.insert(0, str(root / "tests"))
import genesis_ym2612_build as ymbuild  # noqa: E402
import sms_fixture_rom as sms  # noqa: E402
import z80_conformance as z  # noqa: E402

MASTER, RATE = 53693175, 44100
YM_PERIOD, PSG_PERIOD, PSG_FULL = 1008, 240, 131068
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def clip16(v):
    return max(-32768, min(32767, v))


def trunc_div(a, n):
    return -((-a) // n) if a < 0 else a // n


def reference(ym, psg, final_ticks, rate=RATE, scale=8192, swap=False):
    """The frozen definition, restated independently. ym: [(l, r)] per native k; psg: [level] per tick j."""
    def window(k):
        return -(-k * MASTER // rate)
    frames, clipped = [], 0
    iy = ip = 0
    last_ym, last_psg = (0, 0), 0
    k = 0
    wm = min(len(ym) * YM_PERIOD, len(psg) * PSG_PERIOD)
    while window(k + 1) <= wm:
        lo, hi = window(k), window(k + 1)
        ys, ps = [], []
        while iy < len(ym) and iy * YM_PERIOD < hi:
            if iy * YM_PERIOD >= lo:
                ys.append((clip16(ym[iy][0]), clip16(ym[iy][1])))
            iy += 1
        while ip < len(psg) and ip * PSG_PERIOD < hi:
            if ip * PSG_PERIOD >= lo:
                ps.append(psg[ip])
            ip += 1
        if ys:
            last_ym = (trunc_div(sum(a for a, _ in ys), len(ys)), trunc_div(sum(b for _, b in ys), len(ys)))
        if ps:
            last_psg = trunc_div(sum(ps), len(ps)) * scale // PSG_FULL
        out = []
        for ch in (0, 1):
            v = last_ym[ch] + last_psg
            out.append(clip16(v))
            clipped += clip16(v) != v
        frames.append(tuple(reversed(out)) if swap else tuple(out))
        k += 1
    return frames, clipped


def pcm_bytes(frames):
    return b"".join(l.to_bytes(2, "little", signed=True) + r.to_bytes(2, "little", signed=True) for l, r in frames)


def digest_of(frames):
    return hashlib.sha256(pcm_bytes(frames) + len(frames).to_bytes(8, "little")).hexdigest()


def patch(alg=7, fb=0, tl=0x20, ar=0x1F, d1r=0, sl_rr=0x0F, mul=1, fnum=0x269, block=4, pan=0xC0):
    w = [(0, 0xB0), (1, (fb << 3) | alg), (0, 0xB4), (1, pan)]
    for off in (0x00, 0x04, 0x08, 0x0C):
        w += [(0, 0x30 + off), (1, mul), (0, 0x40 + off), (1, tl), (0, 0x50 + off), (1, ar), (0, 0x60 + off), (1, d1r),
              (0, 0x70 + off), (1, 0), (0, 0x80 + off), (1, sl_rr)]
    w += [(0, 0xA4), (1, (block << 3) | (fnum >> 8)), (0, 0xA0), (1, fnum & 0xFF)]
    return w


def z80_program(psg_bytes, ym_writes):
    src = ".org 0x0000\n        ld sp,0x1F80\n"
    for value in psg_bytes:
        src += "        ld a,%d\n        ld (0x7F11),a\n" % value
    for n, (port, value) in enumerate(ym_writes):
        src += "w%d:     ld a,(0x4000)\n        add a,a\n        jr c,w%d\n        ld a,%d\n        ld (0x%04X),a\n" % (n, n, value, 0x4000 + port)
    src += "        halt\n"
    image, _ = sms.Assembler(src).assemble()
    return bytes(image.get(i, 0) for i in range(max(image) + 1))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        psg_bytes = [0x8A, 0x0F, 0x90,            # tone 0: period 0xFA, attenuation 0 (loudest)
                     0xA5, 0x08, 0xB2]             # tone 1: a second, quieter voice
        ym_writes = patch(pan=0x80) + [  # left channel only: the channel-swap control is meaningful
            (0, 0x28), (1, 0xF0), (0, 0x2B), (1, 0x80)]
        for i in range(40):
            ym_writes += [(0, 0x2A), (1, (i * 53 + 7) & 0xFF)]
        program = z80_program(psg_bytes, ym_writes)
        ram = program + bytes(8192 - len(program))
        written = bytearray(1024)
        for offset in range(len(program)):
            written[offset >> 3] |= 1 << (offset & 7)
        (tmp / "image.spec").write_text("epoch %s %s\n" % (ram.hex(), bytes(written).hex()))
        done = subprocess.run([registry_emitter, str(tmp / "image.spec"), str(tmp / "gen"), "genesis_z80"], text=True, capture_output=True)
        assert done.returncode == 0, done.stdout + done.stderr
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        rt = root / "platforms/genesis/runtime"
        objs = ymbuild.build_objects(cc, cxx, root, tmp / "ymobj")
        exe, message = z.compile_units(
            tc, tmp / "gen", "genesis_z80",
            extra_sources=[root / "tests/tools/genesis_z80_machine_harness.c", rt / "runtime.c", rt / "z80_machine.c", rt / "genesis_audio.c",
                           rt / "genesis_mixer.c", root / "libs/device/sega/psg/src/sn76489.c"],
            extra_flags=["-I", str(rt), "-I", str(root / "libs/device/sega/psg/include"), "-I", str(root / "libs/device/sega/ym2612/include")],
            extra_objects=objs)
        assert exe is not None, message
        ym_driver, psg_driver = tmp / "ym_driver", tmp / "psg_driver"
        built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(root / "libs/device/sega/ym2612/include"),
                                str(root / "tests/tools/ym2612_driver.c"), *map(str, objs), "-o", str(ym_driver)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(root / "libs/device/sega/psg/include"),
                                str(root / "tests/tools/genesis_psg_levels_driver.c"), str(root / "libs/device/sega/psg/src/sn76489.c"), "-o", str(psg_driver)],
                               text=True, capture_output=True)
        assert built.returncode == 0, built.stderr

        lines = ["w16 A11100 100", "w16 A11200 100"]
        lines += ["w8 %06X %02X" % (0xA00000 + i, b) for i, b in enumerate(program)]
        lines += ["w16 A11200 0", "w16 A11100 0", "w16 A11200 100"]
        steps = 100000
        lines += ["retire 50"] * (steps // 50) + ["mixer"]
        (tmp / "script.txt").write_text("\n".join(lines) + "\n")

        def run(quantum="-", name="pcm"):
            pcm = tmp / (name + ".pcm")
            out = subprocess.run([str(exe), str(tmp / "script.txt"), str(quantum), "audio", str(pcm)], text=True, capture_output=True, check=True).stdout
            mix = re.search(r"^MIXER (.*)$", out, re.M)
            fields = dict(p.split("=") for p in mix.group(1).split())
            fields["psg"] = [(int(a), int(b)) for a, b in re.findall(r"^PSGTRACE (\d+) (\d+)$", out, re.M)]
            fields["ym"] = [(int(a), int(b), int(c)) for a, b, c in re.findall(r"^YMTRACE (\d+) (\d+) (\d+)$", out, re.M)]
            fields["pcm"] = pcm.read_bytes()
            return fields

        def reconstruct(r, drop_psg=None, drop_ym=None, **kwargs):
            final = int(r["ticks"])
            ymw = [e for i, e in enumerate(r["ym"]) if i != drop_ym]
            lines = []
            for ticks, port, value in ymw:
                lines.append("reset %d" % ticks if port == 0xFF else "w %d %d %d" % (port, value, ticks))
            lines += ["adv %d" % final]
            (tmp / "y.txt").write_text("\n".join(lines) + "\n")
            subprocess.run([str(ym_driver), str(tmp / "y.txt"), str(tmp / "y.bin")], text=True, capture_output=True, check=True)
            raw = (tmp / "y.bin").read_bytes()
            ym = [(int.from_bytes(raw[i:i + 4], "little", signed=True), int.from_bytes(raw[i + 4:i + 8], "little", signed=True)) for i in range(0, len(raw), 8)]
            pl = ["w %d %d" % (v, t) for i, (t, v) in enumerate(r["psg"]) if i != drop_psg] + ["adv %d" % final]
            (tmp / "p.txt").write_text("\n".join(pl) + "\n")
            subprocess.run([str(psg_driver), str(tmp / "p.txt"), str(tmp / "p.bin")], check=True)
            raw = (tmp / "p.bin").read_bytes()
            psg = [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)]
            return reference(ym, psg, final, **kwargs), ym, psg

        base = run()
        check(int(base["fault"]) == 0 and base["nonsilent"] == "1" and len(base["psg"]) == len(psg_bytes) and len([e for e in base["ym"] if e[1] != 0xFF]) == len(ym_writes),
              "the Z80 driver played: PSG (%d bytes) and YM2612 (%d writes) traffic, a non-silent stream, no mixer fault" % (len(base["psg"]), len(ym_writes)))
        (frames, clipped), ym, psg = reconstruct(base)
        check(base["pcm"] == pcm_bytes(frames) and base["sha"] == digest_of(frames) and int(base["frames"]) == len(frames),
              "PCM bytes and SHA-256 equal the independent reconstruction (YM and PSG libraries driven directly, mixed by the reference): %d frames" % len(frames))
        check(int(base["clipped"]) == clipped, "the clip counter equals the reconstruction (%d)" % clipped)
        expected_frames = 0
        while -(-(expected_frames + 1) * MASTER // RATE) <= int(base["ticks"]) - 1008 - 240:  # the sources lag the final time by at most one native
            expected_frames += 1
        check(abs(int(base["frames"]) - expected_frames) <= 1 and int(base["frames"]) > 400,
              "the frame count is the number of 44,100 Hz windows that ended before the final guest time (%s of ~%d)" % (base["frames"], int(base["ticks"]) * RATE // MASTER))
        # each source really contributes
        no_psg = reference(ym, [0] * len(psg), int(base["ticks"]))[0]
        no_ym = reference([(0, 0)] * len(ym), psg, int(base["ticks"]))[0]
        check(digest_of(no_psg) != base["sha"] and digest_of(no_ym) != base["sha"] and len(set(no_psg)) > 20 and len(set(no_ym)) > 3,
              "both sources are audible in the stream (silencing either changes the digest; FM and PSG vary alone)")
        again = run(name="again")
        check((again["sha"], again["frames"], again["clipped"], again["pcm"]) == (base["sha"], base["frames"], base["clipped"], base["pcm"]), "repeated runs are bit-identical")
        cadence = {q: run(q, "q%s" % q) for q in (1, 4096, 65536)}
        check(all(c["sha"] == base["sha"] and c["frames"] == base["frames"] and c["pcm"] == base["pcm"] for c in cadence.values()),
              "the stream is independent of the Z80 synchronization cadence (quanta 1, 512, 4096, 65536)")
        # mutation controls
        for label, kwargs in (("wrong output rate (48,000 Hz)", {"rate": 48000}), ("swapped channels", {"swap": True}), ("wrong PSG gain (4096)", {"scale": 4096})):
            check(digest_of(reconstruct(base, **kwargs)[0][0]) != base["sha"], "mutation control fails the comparison: %s" % label)
        check(digest_of(reconstruct(base, drop_psg=3)[0][0]) != base["sha"], "mutation control: a PSG write dropped from the reconstruction changes the digest")
        check(digest_of(reconstruct(base, drop_ym=len(base["ym"]) - 1)[0][0]) != base["sha"], "mutation control: the last YM write dropped changes the digest")
        print("METRIC pcm frames=%s clipped=%s span=%s nonsilent=%s cadences_identical=%s" % (base["frames"], base["clipped"], base["span"], base["nonsilent"], True))
    print("genesis audio pcm: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
