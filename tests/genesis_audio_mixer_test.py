#!/usr/bin/env python3
"""SEG-032-T009 (ADR 0075, contract section 11): the deterministic Genesis audio mixer against an INDEPENDENT reference.

The real genesis_mixer.c is driven with scripted native deliveries (tests/tools/genesis_mixer_harness.c). The expected stream is computed
here by a separate, brute-force implementation of the frozen definition (per-window scan, Python integers) and by hand-computed
constants. Proved:
  * silence, a constant PSG level, a PSG square tone, a constant and a varying FM level, the DAC step, saturation (with the clip
    counter), the empty-window rule, the DC policy (a constant FM offset is kept: no DC removal), and the window boundaries
    (T_k = ceil(k x 53,693,175 / 44,100); a native exactly at T_k belongs to window k) equal the reference sample for sample;
  * the digest, frame count and every frame are independent of the order and chunking of the deliveries (YM-first, PSG-first,
    interleaved, shuffled in blocks) and of when the consumer reads the ring;
  * the ring drops the OLDEST frames on overrun, counts them, and neither the digest nor the observer sink sees the drop;
  * the frame-range digest equals a digest over that range alone; non-monotonic input and a source running too far ahead fault closed;
  * mutation controls (wrong output rate, swapped channels, wrong PSG gain, truncation replaced by rounding) are told apart by the digest.
usage: genesis_audio_mixer_test.py <cc> <source-root>
"""
import bisect
import hashlib
import pathlib
import random
import subprocess
import sys
import tempfile

cc, root = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
failures = []

MASTER, RATE = 53693175, 44100
YM_PERIOD, PSG_PERIOD, PSG_FULL, PSG_SCALE = 1008, 240, 131068, 8192


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


# ---------------------------------------------------------------- the independent reference
def trunc_div(a, n):
    return -((-a) // n) if a < 0 else a // n


def clip16(v):
    return max(-32768, min(32767, v))


def reference(ym, psg, rate=RATE, scale=PSG_SCALE, swap=False, rounding=False):
    """Returns (frames[(l, r)], clipped) for the natives (start, ...)."""
    def window(k):
        return -(-k * MASTER // rate)

    ym = [(s, clip16(l), clip16(r)) for s, l, r in ym]
    ym_starts, psg_starts = [e[0] for e in ym], [e[0] for e in psg]
    wm = min((ym[-1][0] + YM_PERIOD) if ym else 0, (psg[-1][0] + PSG_PERIOD) if psg else 0)
    frames, clipped = [], 0
    last_ym, last_psg = (0, 0), 0
    k = 0
    while window(k + 1) <= wm:
        lo, hi = window(k), window(k + 1)
        ys = [(l, r) for s, l, r in ym[bisect.bisect_left(ym_starts, lo):bisect.bisect_left(ym_starts, hi)]]
        ps = [lv for s, lv in psg[bisect.bisect_left(psg_starts, lo):bisect.bisect_left(psg_starts, hi)]]
        if ys:
            n = len(ys)
            if rounding:
                last_ym = ((sum(a for a, _ in ys) + n // 2) // n, (sum(b for _, b in ys) + n // 2) // n)
            else:
                last_ym = (trunc_div(sum(a for a, _ in ys), n), trunc_div(sum(b for _, b in ys), n))
        if ps:
            last_psg = trunc_div(sum(ps), len(ps)) * scale // PSG_FULL
        out = []
        for ch in (0, 1):
            v = last_ym[ch] + last_psg
            c = clip16(v)
            clipped += c != v
            out.append(c)
        frames.append(tuple(reversed(out)) if swap else tuple(out))
        k += 1
    return frames, clipped


def digest_of(frames):
    h = hashlib.sha256()
    for l, r in frames:
        h.update(l.to_bytes(2, "little", signed=True) + r.to_bytes(2, "little", signed=True))
    h.update(len(frames).to_bytes(8, "little"))
    return h.hexdigest()


# ---------------------------------------------------------------- the harness
def build(tmp):
    exe = tmp / "mixer_harness"
    rt = root / "platforms/genesis/runtime"
    done = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O1", "-I", str(rt),
                           "-I", str(root / "libs/device/sega/psg/include"), "-I", str(root / "libs/device/sega/ym2612/include"),
                           str(root / "tests/tools/genesis_mixer_harness.c"), str(rt / "genesis_mixer.c"),
                           str(rt / "runtime.c"), "-o", str(exe)], text=True, capture_output=True)
    assert done.returncode == 0, done.stderr
    return exe


class Harness:
    def __init__(self, tmp):
        self.tmp, self.exe, self.n = tmp, build(tmp), 0

    def run(self, lines, frames=True):
        self.n += 1
        path = self.tmp / ("s%d.txt" % self.n)
        path.write_text("\n".join(lines) + "\nend\n")
        out = subprocess.run([str(self.exe), str(path)] + (["frames"] if frames else []), text=True, capture_output=True, check=True).stdout
        res = {"F": [], "D": [], "W": {}}
        for ln in out.splitlines():
            parts = ln.split()
            if parts[0] == "F":
                res["F"].append((int(parts[1]), int(parts[2])))
            elif parts[0] == "D":
                res["D"].append((int(parts[1]), int(parts[2])))
            elif parts[0] == "W":
                res["W"][int(parts[1])] = int(parts[2])
            elif parts[0] == "MIX":
                res["mix"] = dict(p.split("=") for p in parts[1:])
        return res


def natives(span, ym_fn, psg_fn):
    ym = [(t, *ym_fn(t // YM_PERIOD)) for t in range(0, span, YM_PERIOD)]
    psg = [(t, psg_fn(t // PSG_PERIOD)) for t in range(0, span, PSG_PERIOD)]
    return ym, psg


def script(ym, psg, order="ym-first"):
    y = ["ym %d %d %d" % e for e in ym]
    p = ["psg %d %d" % e for e in psg]
    if order == "ym-first":
        return y + p
    if order == "psg-first":
        return p + y
    out, i, j = [], 0, 0
    while i < len(y) or j < len(p):  # interleaved by start time
        if j >= len(p) or (i < len(y) and ym[i][0] <= psg[j][0]):
            out.append(y[i]); i += 1
        else:
            out.append(p[j]); j += 1
    return out


def block_shuffled(ym, psg, seed):
    rnd = random.Random(seed)
    y = ["ym %d %d %d" % e for e in ym]
    p = ["psg %d %d" % e for e in psg]
    out, i, j = [], 0, 0
    while i < len(y) or j < len(p):
        take = rnd.randint(1, 9)
        if rnd.random() < 0.5 and i < len(y):
            out += y[i:i + take]; i += take
        elif j < len(p):
            out += p[j:j + take]; j += take
        else:
            out += y[i:i + take]; i += take
    return out


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-mixer-") as d:
        tmp = pathlib.Path(d)
        h = Harness(tmp)
        SPAN = 60000  # ~49 output frames

        # ---- hand-computed constants (independent of the reference code above) ----
        w = h.run(["window 0", "window 1", "window 2", "window %d" % RATE, "window 100000"], frames=False)["W"]
        check(w[0] == 0 and w[1] == 1218 and w[2] == 2436 and w[RATE] == MASTER and w[100000] == -(-100000 * MASTER // RATE),
              "window starts: T_0 = 0, T_1 = ceil(1217.5) = 1218, T_2 = ceil(2435.06) = 2436, T_44100 = one second of master ticks")
        cases = {
            "silence (YM idle 0, PSG level 0)": (lambda i: (0, 0), lambda i: 0, (0, 0)),
            "PSG constant full scale -> 8192 in both channels": (lambda i: (0, 0), lambda i: PSG_FULL, (8192, 8192)),
            "PSG half scale 65,534 -> 4096": (lambda i: (0, 0), lambda i: 65534, (4096, 4096)),
            "PSG level 1 -> floor(8192 / 131068) = 0": (lambda i: (0, 0), lambda i: 1, (0, 0)),
            "FM constant (a DC offset is kept: no DC removal)": (lambda i: (-310, 275), lambda i: 0, (-310, 275)),
            "FM + PSG add": (lambda i: (1000, -1000), lambda i: PSG_FULL, (9192, 7192)),
            "FM positive saturation (counted per channel sample)": (lambda i: (32767, 32000), lambda i: PSG_FULL, (32767, 32767)),
            "FM negative extreme has headroom for the unipolar PSG": (lambda i: (-32768, -32768), lambda i: PSG_FULL, (-24576, -24576)),
            "FM beyond s16 is clipped on arrival (not a mix clip)": (lambda i: (40000, -40000), lambda i: 0, (32767, -32768)),
        }
        for name, (yf, pf, expected) in cases.items():
            ym, psg = natives(SPAN, yf, pf)
            res = h.run(script(ym, psg))
            ref, clipped = reference(ym, psg)
            check(res["F"] == ref and len(ref) > 40 and set(res["F"]) == {expected} and int(res["mix"]["clipped"]) == clipped, name)
        ym, psg = natives(SPAN, lambda i: (32767, 32000), lambda i: PSG_FULL)
        res = h.run(script(ym, psg))
        check(int(res["mix"]["clipped"]) == 2 * int(res["mix"]["frames"]), "the clip counter counts every saturated channel sample (L and R)")
        ym, psg = natives(SPAN, lambda i: (1000, 1000), lambda i: 1)
        check(int(h.run(script(ym, psg))["mix"]["clipped"]) == 0, "no clip without saturation")

        # ---- tones, DAC steps, windows with several natives and empty windows ----
        def psg_square(i):
            return PSG_FULL if (i // 7) % 2 else 0           # a square tone
        def fm_wave(i):
            v = int(20000 * ((i % 11) - 5) / 5)
            return (v, -v // 2)
        def dac_step(i):
            return (-12000, -12000) if i < 20 else (9000, 9000)
        for name, yf, pf in (("PSG tone", lambda i: (0, 0), psg_square), ("FM tone", fm_wave, lambda i: 0),
                             ("DAC step", dac_step, lambda i: 0), ("FM tone + PSG tone (clipping likely)", fm_wave, psg_square)):
            ym, psg = natives(900000, yf, pf)
            res = h.run(script(ym, psg))
            ref, clipped = reference(ym, psg)
            check(res["F"] == ref and res["mix"]["sha"] == digest_of(ref) and int(res["mix"]["frames"]) == len(ref) and int(res["mix"]["clipped"]) == clipped
                  and res["mix"]["nonsilent"] == "1" and int(res["mix"]["changes"]) >= 1, "%s: stream, digest, frame count and clip counter equal the reference (non-silent)" % name)
        ym, psg = natives(SPAN, lambda i: (500, 500), lambda i: 0)
        check(h.run(script(ym, psg))["mix"]["nonsilent"] == "0", "a constant stream (even a non-zero DC offset) is not non-silent")

        # empty window: a source with a gap repeats its last decimated value
        ym = [(t, 100, 100) for t in range(0, 40000, YM_PERIOD)]
        psg = [(t, PSG_FULL) for t in range(0, 6000, PSG_PERIOD)] + [(t, 0) for t in range(30000, 40000, PSG_PERIOD)]
        res = h.run(script(ym, psg))
        ref, _ = reference(ym, psg)
        check(res["F"] == ref and ref[10][0] == 8192 + 100, "an empty window repeats the last decimated value of that source (PSG gap)")
        # natives exactly on window boundaries (source natives keep their own 1,008-tick spacing): T_1 - 1 is in window 0; T_2 is in
        # window 2, so window 1 is empty and repeats the last decimated value
        assert -(-2 * MASTER // RATE) == 2436 and -(-3 * MASTER // RATE) == 3653
        ym = [(0, 7, 7), (1217, 11, 11), (2436, 41, 41), (3653, 61, 61), (4661, 0, 0), (5669, 0, 0)]
        psg = [(t, 0) for t in range(0, 6000, PSG_PERIOD)]
        res = h.run(script(ym, psg))
        check(res["F"][:4] == [(9, 9), (9, 9), (41, 41), (30, 30)],
              "boundary membership: T_1 - 1 joins window 0 (mean 9), an empty window repeats it, T_2 opens window 2 (41), T_3 opens window 3 with (61 + 0) / 2 = 30")
        ym = [(0, -3, -3), (1008, -4, -4)] + [(t, 0, 0) for t in range(2016, 5000, 1008)]
        res = h.run(script(ym, psg))
        check(res["F"][0] == (-3, -3), "truncating division rounds toward zero (-7 / 2 = -3)")

        # ---- order / chunk independence ----
        ym, psg = natives(900000, fm_wave, psg_square)
        ref, clipped = reference(ym, psg)
        outs = {o: h.run(script(ym, psg, o), frames=False)["mix"] for o in ("ym-first", "psg-first", "interleaved")}
        for seed in range(6):
            outs["shuffled-%d" % seed] = h.run(block_shuffled(ym, psg, seed), frames=False)["mix"]
        check(len({(m["sha"], m["frames"], m["clipped"], m["changes"]) for m in outs.values()}) == 1 and outs["ym-first"]["sha"] == digest_of(ref),
              "digest, frame count and counters are independent of delivery order and chunking (%d variants)" % len(outs))
        # reading the ring in pieces changes nothing
        lines = script(ym, psg, "interleaved")
        mid = lines[: len(lines) // 2] + ["read 50"] + lines[len(lines) // 2:] + ["read 100000"]
        piece = h.run(mid, frames=False)
        check(piece["mix"]["sha"] == digest_of(ref) and piece["D"] == ref[: len(piece["D"])] and len(piece["D"]) == len(ref),
              "draining the consumer ring in pieces yields the whole stream in order and the same digest")

        # ---- frame count is a pure function of what was delivered ----
        ym, psg = natives(100000, lambda i: (0, 0), lambda i: 0)
        n_all = len(reference(ym, psg)[0])
        n_less = len(reference(ym[:-3], psg)[0])
        res = h.run(script(ym[:-3], psg), frames=False)
        check(int(res["mix"]["frames"]) == n_less and n_less < n_all, "a frame is produced only once both sources passed its window end (frames %d < %d)" % (n_less, n_all))

        # ---- ring overrun ----
        ym, psg = natives(MASTER, lambda i: (i % 3000, 0), lambda i: 0)  # one guest second
        res = h.run(script(ym, psg, "interleaved") + ["read 100000"])
        ref, _ = reference(ym, psg)
        dropped = len(ref) - 8192
        check(int(res["mix"]["dropped"]) == dropped and len(res["D"]) == 8192 and res["D"] == ref[dropped:] and len(res["F"]) == len(ref)
              and res["mix"]["sha"] == digest_of(ref), "ring overrun: the oldest %d frames are dropped and counted; the observer sink and the digest still see all %d" % (dropped, len(ref)))

        # ---- range digest ----
        ym, psg = natives(900000, fm_wave, psg_square)
        ref, _ = reference(ym, psg)
        res = h.run(["range 100 50"] + script(ym, psg, "interleaved"), frames=False)
        sub = digest_of(ref[100:150])
        check(res["mix"]["range_sha"] == sub and res["mix"]["range_frames"] == "50" and res["mix"]["sha"] == digest_of(ref),
              "the frame-range digest equals a digest over that range alone; the run digest is unaffected")

        # ---- faults ----
        res = h.run(["ym 0 0 0", "ym 1008 0 0", "ym 1008 1 1", "psg 0 0"], frames=False)
        check(res["mix"]["fault"] == "2" and res["mix"]["frames"] == "0", "a native that starts before the previous one ended faults closed (non-monotonic)")
        ym, psg = natives(MASTER, lambda i: (0, 0), lambda i: 0)
        res = h.run(["ym %d 0 0" % t for t, _, _ in ym], frames=False)
        check(res["mix"]["fault"] == "1" and res["mix"]["frames"] == "0", "a source that runs 2,048 windows ahead of the other faults closed (the host must synchronize)")

        # ---- mutation controls ----
        ym, psg = natives(900000, lambda i: (fm_wave(i)[0] + i % 7 - 3, fm_wave(i)[1] - i % 5), psg_square)
        real = h.run(script(ym, psg, "interleaved"), frames=False)["mix"]["sha"]
        check(real == digest_of(reference(ym, psg)[0]), "control baseline: the reference digest equals the mixer's")
        for label, kwargs in (("wrong output rate (48,000 Hz)", {"rate": 48000}), ("swapped channels", {"swap": True}),
                              ("wrong PSG gain (4096 instead of 8192)", {"scale": 4096}), ("rounding instead of truncating means", {"rounding": True})):
            check(digest_of(reference(ym, psg, **kwargs)[0]) != real, "mutation control fails the comparison: %s" % label)
        print("METRIC mixer frames=%s clipped=%s span=%s sha_stable=%s" % (outs["ym-first"]["frames"], outs["ym-first"]["clipped"], outs["ym-first"]["span"],
                                                                   len({m["sha"] for m in outs.values()}) == 1))
    print("genesis audio mixer: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
