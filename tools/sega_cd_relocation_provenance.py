#!/usr/bin/env python3
"""SEG-110 (ADR 0254): offline, bounded, privacy-safe explanation of an unexplained RAM code frontier as a *relocated fragment* of delivered immutable bytes.

A build pass that stops at a RAM program counter no static case owns asks `segarecomp sega-cd resolve-source` for the disc span the live window is an exact copy of.
That resolver requires a 32-byte anchor and a 64-byte run, so a short fragment that the guest copied from an already-loaded image to another RAM address and then
runs there (a bootstrap stub, a bank-switch or DMA trampoline, a relocated routine) is refused as `no_source`. This diagnostic explains such a window from the
machine's own *writer evidence* instead of byte equality alone, and prints the `--disc-image` extent it would justify. It is a research/diagnostic tool, not a
build dependency: the runtime guard (every executed instruction's bytes are compared with the admitted bytes) is what keeps the result fail-closed.

Evidence chain, all required (each failure is a typed refusal, nothing is guessed):
  1. stream     the final bytes at the PC are owned by one *write stream* of the machine's RAM write log: ascending, contiguous, one writer program counter
                (a copy loop), at most MAX_STREAM bytes. `no_stream` when none owns the PC; `stream_overwritten` when later writes changed the PC's byte or
                left fewer than MIN_RUN bytes (the run is cut to the bytes still owned and equal to the live window).
  2. writer     the writer's program counter lies in verified code: an admitted image extent or the main BIOS window. `writer_not_verified` otherwise
                (a stream written by unadmitted code, e.g. a decoder running from unexplained RAM, explains nothing).
  3. source     the run's bytes occur exactly once in the *delivered* disc blocks (the pass's delivery ledger): `no_source` for none (generated or transformed
                code: values computed by instructions are not a delivered run), `ambiguous_source` for several (never decided by order). Equality alone never
                classifies anything: the stream and the verified writer say where the bytes came from, uniqueness says whether the answer is single.
  4. bounds     an even base and length of at least MIN_RUN bytes; the extent never exceeds the stream.
Only main Work RAM and sub PRG RAM are covered (the write log carries no Word RAM writes): `unsupported_region` otherwise.

usage: sega_cd_relocation_provenance.py --cue DISC.cue --capture CAPTURE.json --region REGION.bin --write-log W.bin [--verified cpu:base:length]...
Inputs: the pass capture (cpu, pc, region, delivered ledger), the live window it wrote, and the program's `--ram-write-log` of the same deterministic run.
Output (stdout, JSON): {"ok": true, "extent": "cpu:offset:base:length", ...} or {"ok": false, "error": CLASS}. Exit 0 / 8.
"""
import argparse
import json
import os
import re
import struct
import sys

MAX_STREAM = 4096     # a produced fragment, not a program: larger streams are not claimed by this diagnostic
MIN_RUN = 8           # four words
MAX_LOG_BYTES = 1 << 30
WORK_RAM = 0xFF0000


def fail(error, **extra):
    print(json.dumps(dict(ok=False, error=error, **extra), sort_keys=True))
    return 8


class Track1:
    def __init__(self, cue):
        text = open(cue, encoding="latin-1").read()
        self.path = os.path.join(os.path.dirname(cue), re.search(r'FILE\s+"([^"]+)"', text).group(1))
        raw = "MODE1/2352" in text   # MODE2 is not supported by the product either
        self.stride, self.start = (2352, 16) if raw else (2048, 0)

    def blocks(self, first, count):
        out = bytearray()
        with open(self.path, "rb") as fh:
            for lba in range(first, first + count):
                fh.seek(lba * self.stride + self.start)
                block = fh.read(2048)
                if len(block) != 2048:
                    return None
                out += block
        return bytes(out)


def write_streams(log, kind, cpu, mask):
    """Ascending, contiguous, single-writer streams of one RAM, in log order: [{start, end, pc, data}] (work RAM offsets are masked to its 64 KiB)."""
    out, cur = [], None
    for k, width, c, _, off, value, pc in struct.iter_unpack("<BBBBIII", log):
        if k != kind or c != cpu:
            continue
        address = off & mask
        if cur is None or cur["end"] != address or cur["pc"] != pc:
            if cur is not None:
                out.append(cur)
            cur = {"start": address, "end": address, "pc": pc, "data": bytearray()}
        cur["data"] += (value & ((1 << (8 * width)) - 1)).to_bytes(width, "big")
        cur["end"] = address + width
    if cur is not None:
        out.append(cur)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cue", required=True)
    ap.add_argument("--capture", required=True)
    ap.add_argument("--region", required=True)
    ap.add_argument("--write-log", required=True)
    ap.add_argument("--verified", action="append", default=[], help="cpu:base:length of an admitted image extent; the main BIOS window is always verified")
    args = ap.parse_args(argv)
    cap = json.load(open(args.capture))
    cpu_name = "main" if cap["cpu"] == 0 else "sub"
    pc, window, base = cap["pc"], open(args.region, "rb").read(), cap["region"]["base"]
    if not (base <= pc < base + len(window)) or os.path.getsize(args.write_log) > MAX_LOG_BYTES:
        return fail("bad_evidence")
    if cpu_name == "main":
        kind, log_cpu, mask, to_ram = 2, 0, 0xFFFF, (lambda a: a & 0xFFFF)
        if not (0xE00000 <= pc <= 0xFFFFFF):
            return fail("unsupported_region")
    else:
        kind, log_cpu, mask, to_ram = 1, 1, 0xFFFFFFFF, (lambda a: a)
        if pc >= 0x80000:
            return fail("unsupported_region")
    window_ram = to_ram(base)                     # RAM offset of window[0]
    log = open(args.write_log, "rb").read()
    streams = write_streams(log[:len(log) // 16 * 16], kind, log_cpu, mask)
    last = {}                                     # RAM offset -> (stream index, byte) of the final write
    for index, s in enumerate(streams):
        for j, byte in enumerate(s["data"]):
            last[s["start"] + j] = (index, byte)

    def live(a):
        i = a - window_ram
        return window[i] if 0 <= i < len(window) else None

    at = to_ram(pc)
    if at not in last:
        return fail("no_stream")
    index = last[at][0]
    s = streams[index]
    if s["end"] - s["start"] > MAX_STREAM:
        return fail("stream_too_large", bytes=s["end"] - s["start"])

    def owned(a):
        return a in last and last[a][0] == index and live(a) == last[a][1]

    if not owned(at):
        return fail("stream_overwritten")
    lo = hi = at
    while lo > s["start"] and owned(lo - 1):
        lo -= 1
    while hi + 1 < s["end"] and owned(hi + 1):
        hi += 1
    cut = (hi + 1 - lo) < (s["end"] - s["start"])
    lo += lo & 1
    length = (hi + 1 - lo) & ~1
    if length < MIN_RUN:
        return fail("stream_overwritten" if cut else "too_short", bytes=max(length, 0))
    verified = [("main", 0, 0x20000)] + [(v.split(":")[0], int(v.split(":")[1], 0), int(v.split(":")[2], 0)) for v in args.verified]
    if not any(c == cpu_name and b <= s["pc"] < b + n for c, b, n in verified):
        return fail("writer_not_verified")
    fragment = bytes(live(a) for a in range(lo, lo + length))
    ledger = []                                   # merged like the product resolver: contiguous or overlapping ranges are one segment, so a copy straddling two entries is found
    for first, count in sorted((int(a), int(b)) for a, b in cap["region"]["delivered"]):
        if ledger and first <= ledger[-1][0] + ledger[-1][1]:
            ledger[-1] = (ledger[-1][0], max(ledger[-1][1], first + count - ledger[-1][0]))
        else:
            ledger.append((first, count))
    if cap["region"].get("delivered_overflow") or not ledger:
        return fail("no_delivery_evidence")
    track, found = Track1(args.cue), []
    for first, count in ledger:
        data = track.blocks(first, count)
        if data is None:
            return fail("disc_unreadable")
        k = data.find(fragment)
        while k >= 0:
            found.append(first * 2048 + k)
            k = data.find(fragment, k + 1)
    if not found:
        return fail("no_source", bytes=length)
    if len(found) > 1:
        return fail("ambiguous_source", occurrences=len(found))
    address = (WORK_RAM + lo) if cpu_name == "main" else lo
    print(json.dumps({"ok": True, "extent": "%s:%d:%d:%d" % (cpu_name, found[0], address, length), "stream_bytes": s["end"] - s["start"], "occurrences": 1, "cut_by_later_writes": cut},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
