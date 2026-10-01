#!/usr/bin/env python3
"""SEG-009-T007: Sega PSG device differential against the pinned PSG references (ADR 0062) and an independent PCM reference.

usage: sms_psg_differential_test.py <cc> <c++> <product-root>

Always (hermetic): the device's PCM from the real catch-up path (advance to each write, apply it) equals a Python
decimation of the device's own tick levels written straight from the machine contract (section 10), for directed and
seeded-random write scripts whose timestamps straddle chip-tick (16 T) and scanline (228 T) boundaries.

With SEGARECOMP_SMS_ORACLE_CHECKOUT holding the pinned checkouts (skipped cleanly otherwise):
  * ares SN76489 (primary): every chip field of every tick of every script is identical (tone counters/outputs/periods,
    noise counter/flip-flop/LFSR for every rate and mode, attenuations), under instruction-start ordering (a write with
    clock T is applied after floor(T/16) ticks); the PCM equals the Python decimation of ares' levels. Known ares
    deviations (ADR 0062) are avoided by construction (no data byte after an attenuation latch; noise output phase is
    compared through the LFSR).
  * Blargg Sms_Apu 0.1.4 (secondary): registers after every write, noise bit sequences and shift intervals for every
    rate and mode, tone edge times swept across chip-tick and scanline boundaries (classified: Blargg applies a write at
    1 T resolution, the device at chip-tick resolution, so every edge differs by exactly T mod 16, below one tick and
    far below one PCM sample of 81 T), and the PCM of a sustained tone by normalized cross-correlation.
  * mutation controls: wrong taps, wrong LFSR width, wrong divider and a wrong attenuation table each disagree with ares.
"""
import math
import os
import pathlib
import random
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "sms_oracle"))
import pins  # noqa: E402
import sms_psg_model as model  # noqa: E402
from sms_psg_model import TABLE, decimate  # noqa: E402

CC = sys.argv[1]
CXX = sys.argv[2] if len(sys.argv) > 2 else "c++"
ROOT = pathlib.Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else HERE.parent
sys.path.insert(0, str(HERE.parent / "tools"))
import host_cc  # noqa: E402
STRICT = host_cc.STRICT_C11
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)
        print("FAIL:", message)


def script_text(writes, end):
    return "".join("w %d %d\n" % w for w in writes) + "end %d\n" % end


def run(cmd, stdin=None, timeout=300):
    r = subprocess.run([str(c) for c in cmd], input=stdin, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("%s failed: %s" % (cmd[0], (r.stdout + r.stderr)[:400]))
    return r.stdout


def gen_script(seed, count):
    rng = random.Random(seed)
    t, writes, last_att = 0, [], False
    gaps = [0, 0, 1, 1, 2, 15, 16, 17, 31, 32, 33, 227, 228, 229, 455, 456, 457]
    for n in range(count):
        t += rng.choice(gaps) if rng.random() < 0.8 else rng.randrange(1, 700)
        if n == 0 or rng.random() < 0.55 or last_att:
            channel, kind = rng.randrange(4), rng.randrange(2)
            low = rng.choice([0, 1, 2, 3, 5, 8, 15, rng.randrange(16)])
            byte = 0x80 | channel << 5 | kind << 4 | low
            last_att = bool(kind)
        else:
            byte = rng.choice([0, 0, 1, 2, 4, 5, rng.randrange(64)])
        writes.append((t, byte))
    return writes, t + 16 * 1500


def directed_scripts():
    """Writes straddling chip-tick and scanline boundaries for every noise control value and tone periods 0..3."""
    out = []
    for ctl in range(8):
        writes = [(0, 0xC2), (0, 0x00), (15, 0xE0 | ctl), (16, 0xF0), (16, 0x90), (17, 0x85), (17, 0x00)]
        out.append((writes, 16 * 700))
    for period in range(4):
        for off in (0, 1, 15, 16, 17, 227, 228, 229):
            writes = [(0, 0x90), (off, 0x80 | period), (off + 1, 0x00), (off + 40, 0x95), (off + 41, 0xA0 | (period ^ 1)),
                      (off + 42, 0x00), (off + 43, 0xB0)]
            out.append((writes, 16 * 120))
    return out


def derive_level(row):
    """Mixer level from a trace row `j o0 o1 o2 c0 c1 c2 p0 p1 p2 nc flip lfsr a0 a1 a2 a3 ...`."""
    outs, atts, lfsr = row[1:4], row[13:17], row[12]
    return sum(TABLE[atts[i]] for i in range(3) if outs[i]) + (TABLE[atts[3]] if lfsr & 1 else 0)


def parse(text):
    return [[int(x) for x in line.split()] for line in text.splitlines() if line.strip()]


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sn76489-"))
    trace = tmp / "trace"
    run([CC, *STRICT, "-I", ROOT / "libs/device/sega/psg/include", ROOT / "tests/tools/sn76489_trace.c",
         ROOT / "libs/device/sega/psg/src/sn76489.c", "-o", trace])
    scripts = directed_scripts() + [gen_script(seed, 70) for seed in range(40)]

    # hermetic: device PCM (catch-up path) == contract decimation of the device's tick levels
    for index, (writes, end) in enumerate(scripts):
        text = script_text(writes, end)
        levels = [row[-1] for row in parse(run([trace, "trace"], text))]
        pcm = [int(x) for x in run([trace, "pcm"], text).split()]
        check(pcm == decimate(levels), "script %d: device PCM differs from the contract decimation" % index)
        rows = parse(run([trace, "trace"], text))
        check(all(row[-1] == derive_level(row) for row in rows),
              "script %d: device level differs from the attenuation table mix" % index)
        check(model.levels_for(writes, end) == levels, "script %d: test-local model levels differ from the device" % index)
        check(model.pcm_for(writes, end) == pcm, "script %d: test-local model PCM differs from the device" % index)
    print("hermetic: %d scripts, device levels and PCM equal the independent test-local model and the contract decimation" % len(scripts))

    root = pins.checkout(["ares-emulator_ares", "drhelius_Gearsystem"])
    if root is None:
        print("skipped: " + pins.skip_reason(["ares-emulator_ares", "drhelius_Gearsystem"]) + " (oracle part)")
        return finish()
    if os.name != "posix":
        print("skipped: the observation adapters need a POSIX GCC/Clang-style compiler")
        return finish()

    ares, gs = root / "ares-emulator_ares", root / "drhelius_Gearsystem" / "src"
    shim = tmp / "shim" / "ares"
    shim.mkdir(parents=True)
    (shim / "ares.hpp").write_text(
        "#pragma once\n#include <nall/platform.hpp>\n#include <nall/memory.hpp>\n#include <nall/primitives.hpp>\n"
        "#include <nall/array.hpp>\n#include <nall/serializer.hpp>\nusing namespace nall;\nusing namespace nall::primitives;\n"
        "#include \"%s\"\n" % (ares / "ares" / "ares" / "types.hpp").as_posix(), encoding="utf-8")
    ares_exe, blargg_exe = tmp / "ares_trace", tmp / "blargg_trace"
    run([CXX, "-std=c++20", "-w", "-I%s" % shim.parent, "-I%s" % (ares / "ares"), "-I%s" % (ares / "nall"),
         HERE / "sms_oracle" / "psg_trace_ares.cpp", ares / "ares" / "component" / "audio" / "sn76489" / "sn76489.cpp",
         "-o", ares_exe], timeout=600)
    run([CXX, "-std=c++17", "-w", "-I%s" % (gs / "audio"), "-I%s" % gs, HERE / "sms_oracle" / "psg_trace_blargg.cpp",
         gs / "audio" / "Sms_Apu.cpp", gs / "audio" / "Blip_Buffer.cpp", "-o", blargg_exe], timeout=600)

    # ---- ares: tick-exact chip state and PCM --------------------------------------------------------------------
    ticks_compared = 0
    for index, (writes, end) in enumerate(scripts):
        text = script_text(writes, end)
        mine = parse(run([trace, "trace"], text))
        theirs = parse(run([ares_exe], text))
        bad = next((j for j, (m, a) in enumerate(zip(mine, theirs)) if m[:18] != a), None)
        check(len(mine) == len(theirs) and bad is None,
              "script %d: ares tick %s differs: device %s vs ares %s" % (index, bad, mine[bad][:18] if bad is not None else "",
                                                                         theirs[bad] if bad is not None else ""))
        ticks_compared += len(theirs)
        levels = [derive_level(row) for row in theirs]
        pcm = [int(x) for x in run([trace, "pcm"], text).split()]
        check(pcm == decimate(levels), "script %d: device PCM differs from the PCM derived from ares' chip state" % index)
    print("ares: %d scripts, %d ticks, chip state and PCM identical" % (len(scripts), ticks_compared))

    # ---- mutation controls: each wrong parameter must disagree with ares --------------------------------------------
    for label, flags in (("taps", ["--taps", "3"]), ("width", ["--bits", "15"]), ("divider", ["--divider", "8"]),
                         ("table", ["--bad-table"])):
        detected = False
        for writes, end in scripts:
            text = script_text(writes, end)
            mine = run([trace, *flags, "trace"], text).splitlines()
            theirs = run([ares_exe], text).splitlines()
            ref_levels = [derive_level(r) for r in parse("\n".join(theirs))]
            if flags[0] == "--bad-table" or flags[0] == "--divider":
                detected = detected or decimate(ref_levels) != [int(x) for x in run([trace, *flags, "pcm"], text).split()]
            else:
                detected = detected or any(parse(m)[0][:18] != parse(a)[0] for m, a in zip(mine, theirs))
            if detected:
                break
        check(detected, "mutation control '%s' was not detected against ares" % label)
    print("mutation controls: wrong taps, width, divider and attenuation table detected")

    # ---- Blargg: registers, noise sequences, edge timing classification, PCM tolerance ------------------------------
    for index, (writes, end) in enumerate(scripts):
        text = script_text(writes, end)
        mine = parse(run([trace, "trace"], text))
        regs = parse(run([blargg_exe, "regs"], text))
        noise_written, latch = False, 0
        for w, row in enumerate(regs):
            T = writes[w][0]
            if writes[w][1] & 0x80:
                latch = writes[w][1] >> 4 & 7
            noise_written = noise_written or latch == 6
            if w + 1 < len(writes) and writes[w + 1][0] // 16 == T // 16:
                continue  # the next write lands in the same tick: the tick line shows the later state
            line = mine[T // 16] if T // 16 < len(mine) else None
            if line is None:
                continue
            ok = row[1:4] == line[7:10] and row[4:8] == line[13:17] and row[8] == (line[17] & 3) and (not noise_written or row[9] == ((line[17] >> 2) & 1))
            check(ok, "script %d write %d: Blargg registers %s differ from the device %s" % (index, w, row[1:10], line[7:18]))
    print("blargg: registers identical after every write of %d scripts (classified: Blargg powers on with the white-noise "
          "feedback selected, the device with control 0; compared only after the first noise-register write)" % len(scripts))

    for ctl in range(8):
        text = script_text([(0, 0xC5), (0, 0x00), (0, 0xE0 | ctl), (0, 0xF0)], 16 * 6000)
        theirs = parse(run([blargg_exe, "noise"], text))
        mine_rows = parse(run([trace, "trace"], text))
        mine_shifts = [(row[0], row[12] & 1) for prev, row in zip([[0] * 19] + mine_rows, mine_rows) if row[11] == 1 and prev[11] == 0]
        n = min(len(theirs), len(mine_shifts), 48)
        check(n >= 20, "noise ctl %d: too few shifts observed" % ctl)
        check([b for _, b in theirs[:n]] == [b for _, b in mine_shifts[:n]], "noise ctl %d: bit sequence differs from Blargg" % ctl)
        gaps_b = {(theirs[i + 1][0] - theirs[i][0]) // 16 for i in range(n - 1)}
        gaps_m = {mine_shifts[i + 1][0] - mine_shifts[i][0] for i in range(n - 1)}
        check(gaps_b == gaps_m and len(gaps_m) == 1, "noise ctl %d: shift interval differs: %s vs %s" % (ctl, gaps_b, gaps_m))
    print("blargg: noise sequences and shift intervals identical for all 8 noise controls")

    classified = set()
    for T in list(range(0, 49)) + list(range(224, 236)) + list(range(452, 460)):
        reg = 254
        writes = [(0, 0xB0), (T, 0xA0 | (reg & 15)), (T, reg >> 4)]
        end = 16 * (T // 16 + reg * 7 + 4)
        rows = parse(run([trace, "trace"], script_text(writes, end)))
        mine_edges = [16 * j for j in range(T // 16, len(rows)) if rows[j][2] != rows[j - 1][2]][:6] if T >= 16 else \
            [16 * j for j in range(T // 16, len(rows)) if rows[j][2] != (rows[j - 1][2] if j else 0)][:6]
        theirs = [int(x) for x in run([blargg_exe, "edges", T, reg]).split()][:6]
        check(len(mine_edges) == 6 and len(theirs) == 6, "edge sweep T=%d: missing edges" % T)
        if len(theirs) == 6 and len(mine_edges) == 6:
            deltas = {b - a for a, b in zip(mine_edges, theirs)}
            check(deltas == {T % 16}, "edge sweep T=%d: Blargg-device edge delta %s != T mod 16 (%d)" % (T, deltas, T % 16))
            classified.add(T % 16)
    print("blargg: tone edges differ by exactly T mod 16 (%d residues 0..15 seen): classified as sub-tick write "
          "resolution, below one PCM sample" % len(classified))

    writes, end = [(0, 0xB0), (0, 0xA0 | (254 & 15)), (0, 254 >> 4)], 357955
    mine = [int(x) for x in run([trace, "pcm"], script_text(writes, end)).split()]
    theirs = [int(x) for x in run([blargg_exe, "pcm"], script_text(writes, end)).split()]
    def centered(v):
        m = sum(v) / len(v)
        return [x - m for x in v]
    span = min(len(mine), len(theirs)) - 400
    a, b = centered(mine[200:200 + span]), centered(theirs[200:200 + span])
    best = -1.0
    for lag in range(-100, 101):
        seg_a = a[max(0, lag):span + min(0, lag)]
        seg_b = b[max(0, -lag):span + min(0, -lag)]
        den = math.sqrt(sum(x * x for x in seg_a) * sum(x * x for x in seg_b))
        if den:
            best = max(best, sum(x * y for x, y in zip(seg_a, seg_b)) / den)
    check(best > 0.95, "Blargg PCM correlation %.4f is below the 0.95 tolerance" % best)
    print("blargg: PCM of a 440 Hz tone correlates %.4f (tolerance 0.95: Blargg is band-limited, the device a box filter)" % best)
    return finish()


def finish():
    if FAILED:
        print("sms psg differential: %d failures" % len(FAILED))
        return 1
    print("sms psg differential: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
