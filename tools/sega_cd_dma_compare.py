#!/usr/bin/env python3
"""SEG-103-T004: compare the product's 68K->VDP DMA transfers with the private reference's, transfer by transfer (aggregate, privacy-safe output).

Product side: the runtime's `--event-log` records `dma_src` (source word address incl. register #23), `dma_len` (words transferred, after the Word RAM
source delay) and `dma_dur` (the main processor's wait in master clocks) per transfer. Reference side: the private Genesis Plus GX driver's DMA log
(arm lines `frame r23 src len code addr cycle`, the length logged after the source delay; end lines `E frame cycle`). Transfers are paired in order; sources below the 68K-bus family
(fill/copy lines, r23 >= 0x80) are skipped. The duration of a reference transfer is (end - arm) in master clocks over 896,040-clock frames.
Nothing printed contains an address: counts, classes and equality only. usage: sega_cd_dma_compare.py PRODUCT_EVENTS REFERENCE_DMA [--page 0x10]
"""
import argparse
import json
import sys

FRAME = 3420 * 262


def product(path):
    rows, cur = [], {}
    for line in open(path):
        parts = line.split()
        if len(parts) != 3 or parts[1] not in ("dma_src", "dma_len", "dma_dur"):
            continue
        cur[parts[1]] = int(parts[2], 16)
        if parts[1] == "dma_dur":
            rows.append((int(parts[0]), cur["dma_src"], cur["dma_len"], cur["dma_dur"]))
            cur = {}
    return rows


def reference(path):
    arms, ends = [], []
    for line in open(path):
        p = line.split()
        if p[0] == "E":
            ends.append((int(p[1]), int(p[2])))
        else:
            arms.append((int(p[0]), int(p[1], 16), int(p[2], 16), int(p[3]), int(p[4], 16), int(p[5], 16), int(p[6])))
    if len(arms) != len(ends):
        raise SystemExit("reference arm/end counts differ: %d / %d" % (len(arms), len(ends)))
    rows = []
    for (frame, r23, src, length, code, addr, cyc), (ef, ec) in zip(arms, ends):
        if r23 >= 0x80:
            continue
        rows.append((frame, (r23 << 16) | src, length, (ef * FRAME + ec) - (frame * FRAME + cyc), r23))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("product_events")
    ap.add_argument("reference_dma")
    ap.add_argument("--page", type=lambda x: int(x, 0), default=0x10, help="register #23 value of the class whose durations must be exact")
    ap.add_argument("--skip-reference", type=int, default=0, help="reference transfers to skip (the product log starts later or the guest differs)")
    ap.add_argument("--prefix", type=int, default=None,
                    help="compare exactly this many leading transfers (fail if either stream is shorter); use it to evaluate an accepted prefix and to "
                         "keep a known divergent suffix (e.g. R2 after its idle-transition divergence) out of the verdict")
    ap.add_argument("--allow-product-suffix", action="store_true",
                    help="the product ran past the end of the reference stream: the unpaired product transfers are reported, not failed")
    a = ap.parse_args()
    P, R = product(a.product_events), reference(a.reference_dma)[a.skip_reference:]
    n = min(len(P), len(R)) if a.prefix is None else a.prefix
    failures = []
    if a.prefix is not None and min(len(P), len(R)) < a.prefix:
        failures.append("a stream is shorter than the declared prefix (%d product, %d reference, %d declared)" % (len(P), len(R), a.prefix))
        n = min(len(P), len(R))
    if n == 0:
        failures.append("empty comparison: nothing was paired")
    if a.prefix is None and len(P) > len(R) and not a.allow_product_suffix:
        failures.append("the product has %d transfers the reference stream cannot verify (truncated reference); declare --prefix or --allow-product-suffix" % (len(P) - len(R)))
    out = {"product_transfers": len(P), "reference_transfers": len(R), "paired": n}
    for name, pred in (("all", lambda r: True), ("page", lambda r: r[4] == a.page)):
        tot = srcs = lens = durs = 0
        first_bad = first_bad_duration = None
        hist = {}
        for i in range(n):
            p, r = P[i], R[i]
            if not pred(r):
                continue
            tot += 1
            srcs += p[1] == r[1]
            lens += p[2] == r[2]  # the reference logs the length after its source delay
            durs += p[3] == r[3]
            d = p[3] - r[3]
            hist[d] = hist.get(d, 0) + 1
            if (p[1] != r[1] or p[2] != r[2]) and first_bad is None:
                first_bad = i
            if p[3] != r[3] and first_bad_duration is None:
                first_bad_duration = i
        out[name] = {"compared": tot, "source_equal": srcs, "length_equal_after_delay": lens, "duration_equal": durs,
                     "duration_difference_histogram": dict(sorted(hist.items(), key=lambda kv: -kv[1])[:6]), "first_mismatch_index": first_bad,
                     "first_duration_mismatch_index": first_bad_duration}
    if out["all"]["source_equal"] != out["all"]["compared"] or out["all"]["length_equal_after_delay"] != out["all"]["compared"]:
        failures.append("a source or length differs (first at index %s)" % out["all"]["first_mismatch_index"])
    if out["page"]["compared"] == 0:
        failures.append("no transfer of the register-#23 class %#x was compared" % a.page)
    elif out["page"]["duration_equal"] != out["page"]["compared"]:
        failures.append("a duration of the class %#x differs (first at index %s)" % (a.page, out["page"]["first_duration_mismatch_index"]))
    out["failures"] = failures
    print(json.dumps(out, indent=1))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
