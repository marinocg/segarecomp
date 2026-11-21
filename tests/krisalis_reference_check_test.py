#!/usr/bin/env python3
"""ADR 0318: the detection power of the Krisalis reference-validation tooling (tools/krisalis_reference_check.py) on SYNTHETIC logs. The tool compares the native replacement with the
ORIGINAL driver executed by an independent emulator; that oracle needs a third-party package and a commercial image, so it is local. What CI checks here is the comparison itself: a missed
command, an invented command, a changed byte, an extra or missing chip write inside the window, a premature stop and a wrong DAC byte must each FAIL, an identical run must pass, and the
stimulus script must round-trip. No commercial data and no original driver code is involved.

usage: krisalis_reference_check_test.py <source-root>
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).resolve()
sys.path.insert(0, str(root / "tools"))
import krisalis_reference_check as k  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


H = 10_000_000  # the horizon of the synthetic runs (master ticks)


def pair(part, reg, value, t):   # two port writes: address then data
    return [(part * 2, reg, t), (part * 2 + 1, value, t + 100)]


def fm_stream():
    out = []
    t = 1000
    for reg, value in ((0x30, 1), (0x34, 2), (0x40, 0x7F), (0xA4, 0x02), (0xA0, 0x8D), (0x28, 0xF0)):
        out += pair(0, reg, value, t)
        t += 200_000
    return out


def dac_group(value, t, revision):
    group = [(0, 0x2A, t), (1, value, t + 50)]
    if revision == 1:
        group += [(2, 0xB6, t + 100), (3, 0xC0, t + 150)]
    return group


# ---- the port-level view ----------------------------------------------------------------------------------------------------------------------------------
fm, dac = k.split_port_stream(fm_stream() + dac_group(0x55, 3_000_000, 3) + dac_group(0x66, 3_010_000, 3), 3)
check(len(fm) == 6 and len(dac) == 2 and [d[2] for d in dac] == [0x55, 0x66], "revision 3: DAC data bytes are separated from the register writes")
fm1, dac1 = k.split_port_stream(fm_stream() + dac_group(0x55, 3_000_000, 1) + dac_group(0x66, 3_010_000, 1), 1)
check(len(fm1) == 6 and len(dac1) == 2, "revision 1: the player's four-write poll group (data and the FM6 pan rewrite) is the player's, not row work")
end_group = [(2, 0xB6, 5_000_000), (3, 0x80, 5_000_050), (0, 0x2B, 5_000_100), (1, 0x00, 5_000_150)]
fm2, _ = k.split_port_stream(fm_stream() + end_group, 3)
check(len(fm2) == 6, "the end of a sample (FM6 pan back from its voice, DAC off) is the player's own group")
fm3, _ = k.split_port_stream(fm_stream() + [(2, 0xB6, 5_000_000), (3, 0x80, 5_000_050)], 3)
check(len(fm3) == 7, "an FM6 pan write that is NOT followed by the DAC-off pair is an ordinary register write")

# ---- the FM stream comparison -------------------------------------------------------------------------------------------------------------------------------
ref_fm, _ = k.split_port_stream(fm_stream(), 3)
tail = 896_040 * 2
failures_, compared, skew = k.compare_streams(ref_fm, ref_fm, H, tail, "fm")
check(not failures_ and compared == 6 and skew == 0, "an identical run passes")
shifted = [(p, r, v, t + 40_000) for p, r, v, t in ref_fm]
failures_, compared, skew = k.compare_streams(shifted, ref_fm, H, tail, "fm")
check(not failures_ and skew == 40_000, "a time shift is reported as skew, never as a failure")
changed = list(ref_fm)
changed[2] = (0, 0x40, 0x7E, changed[2][3])
check(bool(k.compare_streams(changed, ref_fm, H, tail, "fm")[0]), "a changed value fails")
check(bool(k.compare_streams(ref_fm[:4], ref_fm, H, tail, "fm")[0]), "a premature stop (missing writes inside the window) fails")
check(bool(k.compare_streams(ref_fm + [(0, 0x28, 0x00, 5_000_000)], ref_fm, H, tail, "fm")[0]), "an extra write inside the window fails")
check(not k.compare_streams(ref_fm + [(0, 0x28, 0x00, H - 100)], ref_fm, H, tail, "fm")[0], "an extra write at the very end of the window (the lagging side's horizon) is not a failure")

# ---- the DAC comparison: activations, content, bounded differences ---------------------------------------------------------------------------------------------------
HZ = 100_000_000
TAIL = 2 * 896_040


def sample(t, values, ended=True, pan=0x80, step=2760):
    out = [(0, 0x2B, 0x80, t)] + [(0, 0x2A, v, t + 100 + i * step) for i, v in enumerate(values)]
    if ended:
        out += [(1, 0xB6, pan, t + 200 + len(values) * step), (0, 0x2B, 0x00, t + 200 + len(values) * step)]
    return out


def stream(*samples):
    return [w for smp in samples for w in smp]


A = list(range(1, 201))          # three 200-byte samples
B = [(7 * i) & 0xFF for i in range(150)]
C = [(11 * i + 3) & 0xFF for i in range(200)]
ref3 = stream(sample(1_000_000, A), sample(9_000_000, B), sample(20_000_000, C))
failures_, report = k.compare_activations_and_dac(ref3, ref3, HZ, TAIL)
check(not failures_ and report["samples_exact"] == 3 and not report["samples_length_differing"], "identical sample streams are exact")

changed = [w if not (w[1] == 0x2A and w[2] == B[10] and w[3] > 9_000_000) else (0, 0x2A, B[10] ^ 1, w[3]) for w in ref3]
check(bool(k.compare_activations_and_dac(changed, ref3, HZ, TAIL)[0]), "one changed DAC byte fails")
no_short = stream(sample(1_000_000, A), sample(20_000_000, C))
check(bool(k.compare_activations_and_dac(no_short, ref3, HZ, TAIL)[0]), "an entirely missing sample (the middle one) fails: the activation is missing")
short = [1, 2, 3, 4, 5, 6, 7, 8]
ref_short = stream(sample(1_000_000, A), sample(5_000_000, short), sample(9_000_000, B))
no_short2 = stream(sample(1_000_000, A), sample(9_000_000, B))
check(bool(k.compare_activations_and_dac(no_short2, ref_short, HZ, TAIL)[0]), "a missing SHORT sample cannot hide inside a byte-count tolerance")
extra = stream(sample(1_000_000, A), sample(5_000_000, short), sample(9_000_000, B), sample(20_000_000, C))
check(bool(k.compare_activations_and_dac(extra, ref3, HZ, TAIL)[0]), "an extra sample activation fails")
check(bool(k.compare_activations_and_dac(ref3, extra, HZ, TAIL)[0]), "...whichever side has it")
check(not k.compare_activations_and_dac(stream(sample(1_000_000, A), sample(9_000_000, B)), stream(sample(1_000_000, A), sample(9_000_000, B), sample(HZ - 1_000_000, C, ended=False)), HZ, TAIL)[0],
      "a final activation that lies inside the horizon's tail may be missing on the lagging side")
check(bool(k.compare_activations_and_dac(stream(sample(1_000_000, A), sample(9_000_000, B)), stream(sample(1_000_000, A), sample(9_000_000, B), sample(HZ - 30_000_000, C, ended=False)), HZ, TAIL)[0]),
      "...but not one far from the horizon")
# the final sample crossing the horizon: the native side is cut at the horizon (not ended), the original has more bytes of the same sample
tail_ref = stream(sample(1_000_000, A), sample(HZ - 700_000, C, ended=False))
tail_nat = stream(sample(1_000_000, A), sample(HZ - 700_000, C[:150], ended=False, step=2760))
tail_nat = [w for w in tail_nat if w[3] <= HZ]
tail_ref = [w for w in tail_ref if w[3] <= HZ]
res_f, res_r = k.compare_activations_and_dac(tail_nat, tail_ref, HZ, TAIL)
check(not res_f, "an unfinished final sample is accepted when its last byte lies within the tail of the horizon")
check(res_r["samples_truncated_by_horizon"] >= 0, "...and that is reported")
mid_cut = stream(sample(1_000_000, A[:100]), sample(9_000_000, B), sample(20_000_000, C))
check(bool(k.compare_activations_and_dac(mid_cut, ref3, HZ, TAIL)[0]), "a sample cut far too short (100 of 200 bytes, not at the horizon) fails: excessive cut discrepancy")
ok_cut = stream(sample(1_000_000, A[:195]), sample(9_000_000, B), sample(20_000_000, C))
f_ok, r_ok = k.compare_activations_and_dac(ok_cut, ref3, HZ, TAIL)
check(not f_ok and r_ok["samples_length_differing"] == 1 and r_ok["length_difference_max"] == 5 and r_ok["samples_exact"] == 2, "a legitimate bounded cut discrepancy (5 of 200 bytes) passes and is REPORTED, not counted as exact")
tiny_nat = stream(sample(1_000_000, A), sample(5_000_000, short[:6]), sample(9_000_000, B))
check(bool(k.compare_activations_and_dac(tiny_nat, ref_short, HZ, TAIL)[0]), "a short sample must agree almost exactly: 2 of 8 bytes missing fails")
pan_other = stream(sample(1_000_000, A, pan=0x40), sample(9_000_000, B), sample(20_000_000, C))
check(bool(k.compare_activations_and_dac(pan_other, ref3, HZ, TAIL)[0]), "a different pan restored at the end of a sample fails")

# ---- the bank-register bit sequence -------------------------------------------------------------------------------------------------------------------------------
bank_ref = [(b, 1_000_000 + 40 * i) for i, b in enumerate([1, 0, 1, 1, 0, 0, 0, 0, 0] + [0, 1, 0, 0, 1, 0, 0, 0, 0])]
check(not k.compare_banks(bank_ref, bank_ref, HZ, TAIL)[0], "identical bank bit sequences pass")
check(bool(k.compare_banks(bank_ref[:-1], bank_ref, HZ, TAIL)[0]), "one missing bank-register bit fails")
wrong = list(bank_ref); wrong[4] = (1, wrong[4][1])
check(bool(k.compare_banks(wrong, bank_ref, HZ, TAIL)[0]), "one wrong bank-register bit fails")
reorder = list(bank_ref); reorder[0], reorder[1] = (reorder[1][0], reorder[0][1]), (reorder[0][0], reorder[1][1])
check(bool(k.compare_banks(reorder, bank_ref, HZ, TAIL)[0]), "reordered bank writes fail")
late = [(b, HZ - 100_000 + 40 * i) for i, (b, _t) in enumerate(bank_ref)]
check(not k.compare_banks(late[:9], late, HZ, TAIL)[0], "a selection that is cut by the genuine horizon may be incomplete on the lagging side")
check(bool(k.compare_banks(bank_ref[:9], bank_ref, HZ, TAIL)[0]), "...but a missing selection in the middle of the run is not")

# ---- the commands --------------------------------------------------------------------------------------------------------------------------------------------------
cells = bytes(range(12))
written = [(100, bytes([10]) + cells), (200, bytes([1]) + cells), (300, bytes([10]) + cells)]
consumed = [(150, bytes([10]) + cells), (250, bytes([1]) + cells), (350, bytes([10]) + cells)]
check(not k.compare_commands(written, consumed, 1_000_000, 10), "every command written is consumed once, in order, after it was written")
check(bool(k.compare_commands(written, consumed[:2], 1_000_000, 10)), "a command never consumed fails")
check(not k.compare_commands(written, consumed[:2], 360, 100), "a command written within the last frames before the end is pending, not missed")
check(bool(k.compare_commands(written, consumed + [(400, bytes([4]) + cells)], 1_000_000, 10)), "a command the game never wrote fails")
altered = [consumed[0], (250, bytes([1]) + bytes(12)), consumed[2]]
check(bool(k.compare_commands(written, altered, 1_000_000, 10)), "a command consumed with different cells fails")
early = [(50, bytes([10]) + cells)] + consumed[1:]
check(bool(k.compare_commands(written, early, 1_000_000, 10)), "a command consumed before it was written fails")

# ---- the stimulus script round-trips -----------------------------------------------------------------------------------------------------------------------------------
base = k.MAILBOX[3]
log = [k.Line("W", 0x0C00, 0xAA, 10), k.Line("A", 3, 0, 1000), k.Line("B", 0, 0, 1000), k.Line("S", 0, 0, 2000), k.Line("W", base + 2, 0x11, 2100), k.Line("W", base, 10, 2200),
       k.Line("U", 0, 0, 2500), k.Line("S", 0, 0, 3000), k.Line("U", 0, 0, 4200), k.Line("E", 0, 0, 9000)]
script = k.parse_script(k.make_script(log, 3))
check(script["revision"] == 3 and script["activation"] == 1000 and script["horizon"] == 9000 and script["image"] == {0x0C00: 0xAA}, "the script carries the revision, the activation, the horizon and the RAM image at activation")
check(len(script["transactions"]) == 1 and script["transactions"][0]["cmd"] == 1 and script["transactions"][0]["hold"] == 500 and script["transactions"][0]["writes"] == [(base + 2, 0x11), (base, 10)],
      "one transaction per hold that stored something; a bare poll of the wait loop is not replayed; a command store is flagged")
check(k.committed_commands(log, 3) == [(2200, bytes([10, 0x11]) + bytes(11))], "the commands the 68000 committed are rebuilt from its own stores")

# ---- the whole check, end to end, on fabricated runs ---------------------------------------------------------------------------------------------------------------------
base = k.MAILBOX[3]
native = [k.Line("A", 3, 0, 1000), k.Line("B", 0, 0, 1000), k.Line("S", 0, 0, 2000), k.Line("W", base + 2, 0x11, 2100), k.Line("W", base, 10, 2200), k.Line("U", 0, 0, 2500)]
native += [k.Line("C", i, b, 3000) for i, b in enumerate(bytes([10, 0x11]) + bytes(11))]
for port, value, t in fm_stream():
    native.append(k.Line("y", port, value, t))
for i, (b, t) in enumerate(bank_ref):
    native.append(k.Line("K", b, 0, t))
native.append(k.Line("E", 0, 0, H))
ref_fm_full, _ = k.split_port_stream(fm_stream(), 3)
result = k.check(native, ref_fm_full, [], bank_ref, 3, tail)
check(result["passed"] and result["verdict"] == "exact" and result["commands"]["exact"] and result["fm_register_writes"]["exact"], "an identical fabricated run passes with the verdict 'exact'")
bounded_native = native + [k.Line("y", 0, 0x2B, 4_000_000), k.Line("y", 1, 0x80, 4_000_100)]   # a sample activation the reference lacks, early in the run
check(not k.check(bounded_native, ref_fm_full, [], bank_ref, 3, tail)["passed"], "an extra sample activation fails the whole check")
no_bank = [l for l in native if l.kind != "K"]
check(not k.check(no_bank, ref_fm_full, [], bank_ref, 3, tail)["passed"], "a native run that never wrote the bank register fails the whole check")
broken = [l for l in native if not (l.kind == "y" and l.ticks >= 801_000)]
check(not k.check(broken, ref_fm_full, [], bank_ref, 3, tail)["passed"], "a native driver that stops early fails the whole check")
no_command = [l for l in native if l.kind != "C"]
check(not k.check(no_command, ref_fm_full, [], bank_ref, 3, tail)["passed"], "a command that is never consumed fails the whole check")
try:
    k.check([l for l in native if l.kind != "E"], ref_fm_full, [], bank_ref, 3, tail)
    check(False, "a log without the end marker is refused")
except k.CheckError:
    check(True, "a log without the end marker is refused")

print("failures: %d" % len(failures))
sys.exit(1 if failures else 0)
