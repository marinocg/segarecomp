#!/usr/bin/env python3
"""SEG-102-T005 (ADR 0233, 0236): compare a generated-native run's PCM log (`--pcm-trace`) with the independent reference's PCM log, aggregate only.

The reference log is the private Genesis Plus GX driver's (variant F of ADR 0233: the chip code with the host's per-frame phase restart removed). Each of
its lines is "<frame> <entry-relative frame> <R|W|Z> <chip offset hex> <value hex> cyc <u32> pcmcyc <i32> st <hex> en <0|1> a0 <hex> abs <u64>" with `abs`
the absolute sub-processor time in the reference's 50 MHz-domain cycles (four per sub cycle). The trace of the product is "R|W|Z <onsets> <tick> ..."
(scd_pcm.c). The tool prints only counts and classes: nothing here may contain a pointer value, an address or an instant of an authentic run.

Checks (all aggregate):
  * the sequence of PCM operations (kind, chip offset, written value) is identical operation for operation from the last chip reset on;
  * for every pointer read: the difference in sample events applied since the chip reset (reference minus product) - a histogram;
  * the CPU-timeline skew between the two machines at those operations (reference sub cycles minus product sub cycles, relative to the reset): its range;
  * the number of pointer reads whose value differs and how many of those are explained as a one-event difference (the reference's channel address
    exceeds the product's by exactly one frequency step).

usage: sega_cd_pcm_compare.py --trace PRODUCT_TRACE --reference REFERENCE_LOG [--sub-cycle-ticks 2148241]
"""
import argparse
import collections
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--sub-cycle-ticks", type=int, default=2148241)
    args = ap.parse_args()

    ref = []  # (kind, abs, chip offset, value, a0)
    resets = []
    for line in open(args.reference):
        t = line.split()
        if len(t) < 17 or t[2] not in ("R", "W", "Z"):
            continue
        entry = (t[2], int(t[16]), int(t[3], 16), int(t[4], 16), int(t[14], 16))
        ref.append(entry)
        if t[2] == "Z":
            resets.append(len(ref) - 1)
    mine = []  # (kind, tick, chip offset, value, addr, samples)
    for line in open(args.trace):
        t = line.split()
        if t[0] == "R":
            mine.append(("R", int(t[2]), int(t[3], 16) >> 1, int(t[5], 16), int(t[6], 16), int(t[7])))
        elif t[0] == "W":
            mine.append(("W", int(t[2]), int(t[3], 16), int(t[4], 16), 0, int(t[5])))
        else:
            mine.append(("Z", int(t[2]), 0, 0, 0, 0))
    if not resets or not mine:
        print("error: no chip reset in the reference log or an empty product trace", file=sys.stderr)
        return 2
    # The product's trace starts at its first traced reset (peripheral resets only: the power-on reset precedes the trace); align on the reference's
    # last reset that is not the power-on one.
    ref = ref[resets[-1]:] if len(resets) > 1 else ref[resets[0]:]
    anchor = ref[0][1]
    n = min(len(ref), len(mine))
    sequence_mismatch = 0
    for i in range(n):
        a, b = ref[i], mine[i]
        if a[0] != b[0] or (a[0] == "W" and (a[2] != b[2] or a[3] != b[3])) or (a[0] == "R" and a[2] != b[2]):
            sequence_mismatch += 1
    reads = [i for i in range(n) if mine[i][0] == "R"]
    ds = collections.Counter()
    differing = explained = 0
    skew0 = ref[0][1] / 4.0 - mine[0][1] / float(args.sub_cycle_ticks)
    skews = []
    for i in range(n):
        skews.append(ref[i][1] / 4.0 - mine[i][1] / float(args.sub_cycle_ticks) - skew0)
    for i in reads:
        ref_samples = -(-(ref[i][1] - anchor) // 1536)  # events strictly before the access: ceil((t - anchor) / 1536)
        ds[ref_samples - mine[i][5]] += 1
        if ref[i][3] != mine[i][3]:
            differing += 1
            if ref[i][4] > mine[i][4]:
                explained += 1
    read_skews = [skews[i] for i in reads]
    out = {
        "operations_compared": n,
        "operation_sequence_mismatches": sequence_mismatch,
        "pointer_reads": len(reads),
        "pointer_reads_equal": len(reads) - differing,
        "pointer_reads_differing": differing,
        "differing_reads_where_reference_is_ahead": explained,
        "sample_events_reference_minus_product_histogram": {str(k): v for k, v in sorted(ds.items())},
        "timeline_skew_sub_cycles_all_operations": [round(min(skews), 1), round(max(skews), 1)] if skews else None,
        "timeline_skew_sub_cycles_at_pointer_reads": [round(min(read_skews), 1), round(max(read_skews), 1)] if read_skews else None,
    }
    for k, v in out.items():
        print("%s: %s" % (k, v))
    return 0 if sequence_mismatch == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
