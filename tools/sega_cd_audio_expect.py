#!/usr/bin/env python3
"""SEG-107 (ADR 0251): an independent expectation of the Sega CD mixed audio stream, written without the runtime's code.

From external evidence only - the runtime's own register-access trace (`--pcm-trace`: every chip write with its machine tick), the CD-DA run log
(`--cdda-log`: run starts and stops, fader writes) and the disc itself - it rebuilds the RF5C164 output from the published chip behavior (GPGX pcm_run / MAME rf5c68:
sign-magnitude bytes with bit 7 = positive, $FF loop marker, (value x ENV x PAN-nibble) >> 5, summed and clamped), places it on the 44.1 kHz guest-time
timeline (frame n at floor(n x ticks_per_second / 44100), zero-order hold), places each CD-DA run at its first delivery (contiguous at the DAC rate after, the
drive tick being slower than 588 frames the sectors beyond it come from the disc), applies the fader, and mixes them with saturation.
Negative PCM products round toward minus infinity in the runtime (GPGX) and toward zero here (MAME's rule): the two differ by at most one LSB per
running negative channel and side, so a stream with negative samples is compared within `tolerance`.

usage: sega_cd_audio_expect.py --mix FILE --trace FILE --log FILE --cue FILE [--tolerance N] [--from-frame N] [--frames N]   (exit 0 when every frame agrees)
"""
import argparse
import os
import re
import struct
import sys

TPS = 26846587500000          # machine ticks per second
SUB_CYCLE = 2148241           # ticks per sub cycle, BIOS-image profile
CDD_TICK = 357954500000       # one 75 Hz drive tick = one CD-DA sector period
PERIOD = 384 * SUB_CYCLE      # one RF5C164 sample


def frame_time(n):
    return (n // 44100) * TPS + ((n % 44100) * TPS) // 44100


def clamp16(v):
    return max(-32768, min(32767, v))


class Chip:
    """The published RF5C164 behavior, written independently of the runtime."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.ram = bytearray(65536)
        self.env = [0] * 8
        self.pan = [0xFF] * 8
        self.st = [0] * 8
        self.fd = [0] * 8
        self.ls = [0] * 8
        self.addr = [0] * 8
        self.running = [False] * 8
        self.idx = self.bank = self.enabled = 0

    def write(self, reg, v):
        c = self.idx
        if reg >= 0x1000:
            self.ram[(self.bank << 12) | (reg & 0xFFF)] = v
        elif reg == 0:
            self.env[c] = v
        elif reg == 1:
            self.pan[c] = v
        elif reg == 2:
            self.fd[c] = (self.fd[c] & 0xFF00) | v
        elif reg == 3:
            self.fd[c] = (self.fd[c] & 0x00FF) | (v << 8)
        elif reg == 4:
            self.ls[c] = (self.ls[c] & 0xFF00) | v
        elif reg == 5:
            self.ls[c] = (self.ls[c] & 0x00FF) | (v << 8)
        elif reg == 6:
            self.st[c] = v
            if not self.running[c]:
                self.addr[c] = v << 19
        elif reg == 7:
            if v & 0x40:
                self.idx = v & 7
            else:
                self.bank = v & 15
            self.enabled = (v >> 7) & 1
        elif reg == 8:
            for k in range(8):
                self.running[k] = not (v >> k) & 1
                if not self.running[k]:
                    self.addr[k] = self.st[k] << 19

    def sample(self):
        left = right = 0
        if not self.enabled:
            return 0, 0
        for c in range(8):
            if not self.running[c]:
                continue
            b = self.ram[(self.addr[c] >> 11) & 0xFFFF]
            if b == 0xFF:
                self.addr[c] = self.ls[c] << 11
                b = self.ram[self.ls[c]]
                if b == 0xFF:
                    continue
            else:
                self.addr[c] = (self.addr[c] + self.fd[c]) & 0x7FFFFFF
            mag = b & 0x7F
            sign = 1 if b & 0x80 else -1
            left += sign * ((mag * (self.pan[c] & 15) * self.env[c]) >> 5)
            right += sign * ((mag * (self.pan[c] >> 4) * self.env[c]) >> 5)
        return clamp16(left), clamp16(right)


def parse_trace(trace_text):
    ops = []
    for line in trace_text.splitlines():
        p = line.split()
        if p and p[0] == "W":
            ops.append((int(p[2]), 0, int(p[3], 16), int(p[4], 16)))
        elif p and p[0] == "Z":
            ops.append((int(p[2]), 1, 0, 0))
    return ops


def pcm_stream(ops):
    """Yield the PCM (L, R) of output frame 0, 1, 2, ...: the latest sample event at or before the frame's guest time (zero-order hold)."""
    chip, origin, k, oi, hold, n = Chip(), 0, 0, 0, (0, 0), 0
    while True:
        t = frame_time(n)
        while True:
            e = origin + k * PERIOD
            while oi < len(ops) and ops[oi][0] <= e:  # an access at an event instant lands before that event
                when, kind, reg, v = ops[oi]
                if kind:
                    chip.reset()
                    origin, k, e = when, 0, when
                else:
                    chip.write(reg, v)
                oi += 1
            if e > t:
                break
            hold = chip.sample()
            k += 1
        yield hold
        n += 1


def pcm_reference(trace_text, frames):
    stream = pcm_stream(parse_trace(trace_text))
    return [next(stream) for _ in range(frames)]


def parse_cdda_log(log_text):
    """Runs and fader events from the CD-DA diagnostic log: ([{tick, lba, stop, next_lba}], [(tick, target or None for a reset)])."""
    runs, faders, cur = [], [], None
    for line in log_text.splitlines():
        m = re.match(r"(start|jump) frame=\d+ tick=(\d+) lba=(-?\d+)", line)
        if m:
            if cur is not None:
                runs.append(cur)
            cur = {"tick": int(m.group(2)), "lba": int(m.group(3)), "stop": None, "next_lba": None}
            continue
        m = re.match(r"stop frame=\d+ tick=(\d+) next_lba=(-?\d+)", line)
        if m and cur is not None:
            cur["stop"], cur["next_lba"] = int(m.group(1)), int(m.group(2))
            runs.append(cur)
            cur = None
            continue
        m = re.match(r"fader frame=\d+ tick=(\d+) target=(\d+)", line)
        if m:
            faders.append((int(m.group(1)), int(m.group(2))))
            continue
        m = re.match(r"reset frame=\d+ tick=(\d+)", line)
        if m:
            faders.append((int(m.group(1)), None))
    if cur is not None:
        runs.append(cur)
    return runs, faders


def first_frame_at_or_after(tick):
    lo, hi = 0, tick // (TPS // 44100) + 4
    while lo < hi:
        mid = (lo + hi) // 2
        if frame_time(mid) >= tick:
            hi = mid
        else:
            lo = mid + 1
    return lo


def place_runs(runs, cap_fn, frames):
    """Segments [(first frame, first lba, sectors)] of CD-DA on the timeline. A run starts at the first frame at or after its first delivery (behind any
    sector still queued) and then advances one frame per output frame. Sector j begins at frame start + 588 j and is heard if the drive had delivered it
    (j < delivered) or the drive still reports the run at that instant and the track goes on (it is read ahead from the disc)."""
    segments, free = [], 0
    for r in runs:
        start = max(first_frame_at_or_after(r["tick"]), free)
        delivered = (r["next_lba"] - r["lba"]) if r["next_lba"] is not None else 0
        j = 0
        while start + 588 * j < frames:
            if j >= delivered and not ((r["stop"] is None or frame_time(start + 588 * j) < r["stop"]) and r["lba"] + j < cap_fn(r["lba"])):
                break
            j += 1
        segments.append((start, r["lba"], j))
        free = start + 588 * j
    return segments


def scale(sample, volume):
    mult = (volume & 0x7FC) or (volume & 3)
    return int(sample * mult / 1024)  # truncation toward zero


def cdda_stream(segments, faders, sector_fn, frames):
    """Yield the CD-DA (L, R) of output frames 0..frames-1: the segments' sectors, scaled by the fader (one step per CD-DA frame toward the target)."""
    vol = target = 1024
    fi, si, cache = 0, 0, (None, b"")
    for n in range(frames):
        t = frame_time(n)
        while fi < len(faders) and faders[fi][0] <= t:
            if faders[fi][1] is None:
                vol = target = 1024
            else:
                target = min(faders[fi][1], 1024)
            fi += 1
        while si < len(segments) and n >= segments[si][0] + 588 * segments[si][2]:
            si += 1
        if si < len(segments) and segments[si][0] <= n:
            first, lba0, _ = segments[si]
            j, f = divmod(n - first, 588)
            if cache[0] != lba0 + j:
                cache = (lba0 + j, sector_fn(lba0 + j))
            l, r = struct.unpack_from("<hh", cache[1], 4 * f)
            l, r = scale(l, vol), scale(r, vol)
            if vol < target:
                vol += 1
            elif vol > target:
                vol -= 1
            yield (l, r)
        else:
            yield (0, 0)


def cdda_placement(log_text, sector_fn, cap_fn, frames):
    runs, faders = parse_cdda_log(log_text)
    return list(cdda_stream(place_runs(runs, cap_fn, frames), faders, sector_fn, frames))


class CueDisc:
    """Sector source and track ends of a single-BIN CUE disc (file offset = LBA x 2352), read with code written here."""

    def __init__(self, cue_path):
        import pathlib
        cue = pathlib.Path(cue_path)
        text = cue.read_text()
        self.bin = cue.parent / re.search(r'FILE "([^"]+)"', text).group(1)
        self.size = self.bin.stat().st_size // 2352
        tracks, cur = [], None
        for line in text.splitlines():
            m = re.match(r"\s*TRACK (\d+)", line)
            if m:
                cur = {"n": int(m.group(1)), "i0": None, "i1": None}
                tracks.append(cur)
            m = re.match(r"\s*INDEX 0([01]) (\d+):(\d+):(\d+)", line)
            if m and cur is not None:
                cur["i" + m.group(1)] = (int(m.group(2)) * 60 + int(m.group(3))) * 75 + int(m.group(4))
        self.tracks = tracks
        self._fh = open(self.bin, "rb")

    def sector(self, lba):
        self._fh.seek(lba * 2352)
        return self._fh.read(2352)

    def cap(self, lba):
        """End (exclusive) of the track holding `lba`: the next track's INDEX 00 (or INDEX 01), or the end of the BIN."""
        for i, t in enumerate(self.tracks):
            nxt = self.tracks[i + 1] if i + 1 < len(self.tracks) else None
            end = (nxt["i0"] if nxt["i0"] is not None else nxt["i1"]) if nxt else self.size
            if t["i1"] <= lba < end:
                return end
        return lba


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--mix", required=True)
    ap.add_argument("--trace", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--cue", required=True, help="the disc's CUE (single BIN): the CD-DA source and the track ends")
    ap.add_argument("--tolerance", type=int, default=0)
    ap.add_argument("--from-frame", type=int, default=0, help="compare only frames from here (the chip is still replayed from the start)")
    ap.add_argument("--frames", type=int, default=0, help="compare up to this frame count (0 = the whole artifact)")
    args = ap.parse_args(argv)
    disc = CueDisc(args.cue)
    total = os.path.getsize(args.mix) // 4
    end = total if args.frames <= 0 else min(args.frames, total)
    runs, faders = parse_cdda_log(open(args.log).read())
    cd_stream = cdda_stream(place_runs(runs, disc.cap, total), faders, disc.sector, total)
    stream = pcm_stream(parse_trace(open(args.trace).read()))
    differing = pcm_audible = cd_audible = both = compared = 0
    first_bad = None
    with open(args.mix, "rb") as fh:
        chunk, base = b"", 0
        for n in range(end):
            p = next(stream)
            c = next(cd_stream)
            if n < args.from_frame:
                continue
            if n - base >= len(chunk) // 4 or n < base:
                fh.seek(4 * n)
                chunk, base = fh.read(4 * 65536), n
            got = struct.unpack_from("<hh", chunk, 4 * (n - base))
            want = (clamp16(p[0] + c[0]), clamp16(p[1] + c[1]))
            compared += 1
            pcm_audible += p != (0, 0)
            cd_audible += c != (0, 0)
            both += p != (0, 0) and c != (0, 0)
            if max(abs(got[0] - want[0]), abs(got[1] - want[1])) > args.tolerance:
                differing += 1
                if first_bad is None:
                    first_bad = (n, got, want)
    print("frames compared %d (of %d); pcm-audible %d; cdda-audible %d; both %d; differing %d" % (compared, total, pcm_audible, cd_audible, both, differing))
    if first_bad:
        print("first differing frame %d: got %s want %s" % first_bad)
    return 1 if differing else 0


if __name__ == "__main__":
    sys.exit(main())
