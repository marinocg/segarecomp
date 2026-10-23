#!/usr/bin/env python3
"""Bounded, sanitized provenance classification of a materialized Sega CD sub-CPU epoch image (SEG-079, ADR 0178/0179).

For every byte of a materialized image: is it a verbatim copy of an immutable input, a deterministic transform of one,
initialised/generated, or unexplained - and does a *writer* explain it (not just byte equality)?

Inputs (all local, caller supplied): the epoch image; named immutable sources (e.g. the BIOS image); the writer log of the
generated program (`program --ram-write-log FILE`, 16-byte little-endian records {kind, width, cpu, 0, offset, value, pc};
kind 1 = PRG RAM write, 2 = work RAM write, 3 = main BIOS-window read); optionally the program's `--coverage` file.

Method (constants below are the bounds):
  1. Streams. The main processor's PRG RAM writes before the first sub write are split into *streams*: maximal runs of writes
     whose destinations are consecutive. A decoder or a copy loop is one stream.
  2. Explanation, strongest first, all against the *final* image bytes the stream still owns:
       zero fill   - every write value is zero (the BIOS's PRG RAM clear)                              -> class 5
       source copy - writes pair with source reads at constant (source - destination) deltas and the
                     copied bytes equal the source; one delta -> class 1, several -> class 2          -> supported
       decoded     - an independent registered decoder, started at the contiguous ascending run of source reads that ends at
                     the stream's first write, reproduces exactly the bytes the stream wrote            -> class 4, supported
       otherwise   - unexplained                                                                      -> class 6
     Bytes no stream wrote are class 6 unless zero (class 5, reported as unsupported: no writer evidence).
  3. Equality-only evidence never classifies anything: `equality_only_bytes` counts image bytes that lie in long exact source
     runs but whose stream is *not* a copy (coincidence / transform side effect), to show what a search-only method would claim.
Classes: 1 verbatim, 2 reordered/segmented, 3 relocated/patched, 4 deterministically transformed, 5 initialised/generated, 6 unknown.
Class 3 is reserved: it needs a deterministic patch rule with an independent implementation and no stream here required one.

The report contains classes, region/byte counts, percentages and support flags only: no offsets, addresses or bytes. `--detail`
writes the private per-stream detail and must point into ignored storage.
"""
import argparse
import collections
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_epoch_recipe as recipe  # noqa: E402

K = 16                 # seed length of the equality search
MIN_COPY = 32          # shortest equal run that is even reported as equality-only evidence
MAX_CANDIDATES = 64
MAX_RECORDS = 1 << 26
CLASSES = {1: "verbatim_copy", 2: "reordered_or_segmented_copy", 3: "relocated_or_patched_copy",
           4: "deterministically_transformed", 5: "initialised_or_generated", 6: "unknown_provenance"}
KIND_PRG, KIND_WORK, KIND_ROM_READ = 1, 2, 3
MAIN, SUB = 0, 1


class ProvenanceError(Exception):
    pass


def read_log(path, limit=MAX_RECORDS):
    raw = pathlib.Path(path).read_bytes()
    if len(raw) % 16 != 0 or len(raw) // 16 > limit:
        raise ProvenanceError("write_log_malformed")
    return [struct.unpack_from("<BBBBIII", raw, o) for o in range(0, len(raw), 16)]


def uniform(data):
    return len(data) > 0 and data.count(data[0]) == len(data)


def equality_runs(image, source):
    """Maximal exact runs (>= MIN_COPY, not a single repeated value) of `image` found in `source`: [(image_off, length)]."""
    index = collections.defaultdict(list)
    for i in range(len(source) - K + 1):
        index[source[i:i + K]].append(i)
    runs, i = [], 0
    while i <= len(image) - K:
        best = 0
        for j in index.get(image[i:i + K], ())[:MAX_CANDIDATES]:
            n = K
            while i + n < len(image) and j + n < len(source) and image[i + n] == source[j + n]:
                n += 1
            best = max(best, n)
        if best >= MIN_COPY and not uniform(image[i:i + best]):
            runs.append((i, best))
        i += max(best, 1)
    return runs


def streams(log):
    """Main PRG writes before the first sub write -> [dict(start, end, first, last, values_zero)] (consecutive-destination runs)."""
    out, expected = [], None
    for n, (kind, width, cpu, _, offset, value, pc) in enumerate(log):
        if cpu == SUB:
            break
        if kind != KIND_PRG:
            continue
        if out and expected == offset:
            s = out[-1]
            s["end"], s["last"] = offset + width, n
            s["zero"] = s["zero"] and value == 0
        else:
            out.append({"start": offset, "end": offset + width, "first": n, "last": n, "zero": value == 0})
        expected = offset + width
    return out


def source_start(log, first):
    """Offset of the contiguous ascending run of source reads that ends at record `first` (the stream's first write)."""
    start, expect = None, None
    for r in reversed(log[:first + 1]):
        if r[0] != KIND_ROM_READ:
            continue
        if expect is not None and r[4] != expect:
            break
        start, expect = r[4], r[4] - 1
    return start


def copy_segments(log, stream, previous_last):
    """A pure copy loop: the source reads in the stream's window (after the previous stream) equal, in order, the stream's writes.
    -> [(dest, source_offset, length)] segments (maximal runs contiguous in both source and destination), else None."""
    reads = [r for r in log[previous_last + 1:stream["last"] + 1] if r[0] == KIND_ROM_READ]
    writes = [r for r in log[stream["first"]:stream["last"] + 1] if r[0] == KIND_PRG and r[2] == MAIN]
    if not writes or len(reads) < len(writes):
        return None
    reads = reads[-len(writes):]
    segs = []
    for r, w in zip(reads, writes):
        if r[1] != w[1] or r[5] != w[5]:
            return None
        if segs and segs[-1][1] + segs[-1][2] == r[4] and segs[-1][0] + segs[-1][2] == w[4]:
            segs[-1] = (segs[-1][0], segs[-1][1], segs[-1][2] + w[1])
        else:
            segs.append((w[4], r[4], w[1]))
    return segs


def classify(image, sources, log, executed_starts=None):
    """-> (sanitized report, private detail). `sources` = {name: bytes}; the first sorted name is the copy-pairing source."""
    size = len(image)
    primary = sources[sorted(sources)[0]]
    cls = [6] * size
    support = {c: False for c in CLASSES}
    unresolved = {c: False for c in CLASSES}
    detail = []
    previous_last = -1
    all_streams = streams(log)
    owner = [None] * size   # the last stream to write each byte: earlier streams (e.g. the PRG RAM clear) keep only what is left
    for i, s in enumerate(all_streams):
        for b in range(s["start"], min(s["end"], size)):
            owner[b] = i
    claimed = [o is not None for o in owner]
    for i, s in enumerate(all_streams):
        lo, hi = s["start"], min(s["end"], size)
        if lo >= size:
            continue
        owned = [b for b in range(lo, hi) if owner[b] == i]
        rec = {"start": s["start"], "length": s["end"] - s["start"], "owned": len(owned), "kind": "unexplained"}
        if s["zero"] and all(image[b] == 0 for b in owned):
            c, rec["kind"] = 5, "zero_fill"
            support[5] = True
        else:
            segs = copy_segments(log, s, previous_last)
            ok_copy = bool(segs) and sum(n for _, _, n in segs) == s["end"] - s["start"] and \
                all(primary[so + (b - d)] == image[b] for d, so, n in segs for b in range(d, d + n) if b < size and owner[b] == i)
            start = source_start(log, s["first"])
            decoded = None
            if start is not None and start < len(primary):
                try:
                    data, _ = recipe.kosinski(primary, start)
                    if len(data) == s["end"] - s["start"] and all(image[b] == data[b - lo] for b in owned):
                        decoded = start
                except recipe.RecipeError:
                    pass
            if decoded is not None and not (ok_copy and len(segs) == 1 and segs[0][1] == decoded):
                c, rec["kind"] = 4, "decoded:kosinski"
                support[4] = True
            elif ok_copy:
                c, rec["kind"] = (1, "copy") if len(segs) == 1 else (2, "segmented_copy")
                support[c] = True
            else:
                c = 6
                unresolved[6] = True
        for b in owned:
            cls[b] = c
        previous_last = s["last"]
        detail.append(rec)
    unwritten_zero = False
    for b in range(size):
        if not claimed[b]:
            if image[b] == 0:
                cls[b] = 5
                unwritten_zero = True   # machine-initial zero with no writer: counted in class 5 but never 'supported'
            else:
                cls[b] = 6
    eq = set()
    for off, n in equality_runs(image, primary):
        eq.update(range(off, off + n))
    equality_only = sum(1 for b in eq if cls[b] not in (1, 2))
    counts = collections.Counter(cls)
    regions = collections.Counter()
    prev = None
    for c in cls:
        if c != prev:
            regions[c] += 1
        prev = c
    exe = collections.Counter()
    if executed_starts:
        for pc in executed_starts:
            if pc < size:
                exe[cls[pc]] += 1
    table = []
    for c in sorted(CLASSES):
        n = counts.get(c, 0)
        table.append({"class": c, "name": CLASSES[c], "regions": regions.get(c, 0), "bytes": n,
                      "percent": round(100.0 * n / size, 3) if size else 0.0,
                      "independently_supported": bool(n and support[c] and not unresolved[c] and not (c == 5 and unwritten_zero)),
                      "unresolved_dynamic_behaviour": bool(n and (unresolved[c] or c == 6)),
                      "executed_instruction_starts": exe.get(c, 0) if executed_starts else None})
    report = {"image_bytes": size, "classes": table, "equality_only_bytes": equality_only,
              "fully_explained": counts.get(6, 0) == 0 and not any(unresolved.values()),
              "supported_bytes": sum(row["bytes"] for row in table if row["independently_supported"])}
    return report, detail


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--epoch", required=True)
    ap.add_argument("--source", action="append", default=[], help="NAME=FILE (an immutable input)")
    ap.add_argument("--write-log", required=True)
    ap.add_argument("--coverage", help="program --coverage file (sub instruction starts)")
    ap.add_argument("--detail", help="private per-stream detail output (ignored storage only)")
    args = ap.parse_args(argv)
    sources = {}
    for spec in args.source:
        name, _, path = spec.partition("=")
        sources[name] = pathlib.Path(path).read_bytes()
    if not sources:
        raise SystemExit("at least one --source is required")
    starts = None
    if args.coverage:
        starts = {int(f[1], 16) for f in (line.split() for line in pathlib.Path(args.coverage).read_text().splitlines()) if len(f) >= 2 and f[0] == "1"}
    image = pathlib.Path(args.epoch).read_bytes()
    report, detail = classify(image, sources, read_log(args.write_log), starts)
    if args.detail:
        pathlib.Path(args.detail).write_text(json.dumps(detail))
    json.dump(report, sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
