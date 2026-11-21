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

# ---- the DAC comparison -------------------------------------------------------------------------------------------------------------------------------------------
starts = []
d_ref = [(0, 0x2B, 0x80, 1_000_000)] + [(0, 0x2A, v, 1_100_000 + i * 1000) for i, v in enumerate((1, 2, 3, 4))] + [(0, 0x2B, 0x80, 4_000_000)] + [(0, 0x2A, v, 4_100_000 + i * 1000) for i, v in enumerate((9, 8))]
check(not k.compare_dac(d_ref, starts, d_ref, starts, 4)[0], "identical DAC bytes pass")
d_bad = list(d_ref)
d_bad[1] = (0, 0x2A, 7, d_bad[1][3])
check(bool(k.compare_dac(d_bad, starts, d_ref, starts, 4)[0]), "a wrong DAC byte fails")
d_cut = d_ref[:4] + d_ref[5:]
check(not k.compare_dac(d_cut, starts, d_ref, starts, 4)[0], "a sample cut one byte earlier is within the polling tolerance")
check(bool(k.compare_dac(d_cut, starts, d_ref, starts, 0)[0]), "...and fails when no tolerance is allowed")
check(bool(k.compare_dac(d_ref[:2] + d_ref[5:], starts, d_ref, starts, 1)[0]), "a sample cut far too early fails")
check(bool(k.compare_dac(d_ref[:5], starts, d_ref, starts, 1)[0]), "a sample the native driver never started fails")

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
native = [k.Line("A", 3, 0, 1000), k.Line("B", 0, 0, 1000), k.Line("S", 0, 0, 2000), k.Line("W", base + 2, 0x11, 2100), k.Line("W", base, 10, 2200), k.Line("U", 0, 0, 2500)]
native += [k.Line("C", i, b, 3000) for i, b in enumerate(bytes([10, 0x11]) + bytes(11))]
for port, value, t in fm_stream():
    native.append(k.Line("y", port, value, t))
native.append(k.Line("E", 0, 0, H))
result = k.check(native, ref_fm, [], [], 3, tail)
check(result["identical"] and result["fm_writes_compared"] == 6 and result["commands_consumed"] == 1, "an identical fabricated run passes the whole check")
broken = [l for l in native if not (l.kind == "y" and l.ticks >= 801_000)]   # the native driver stopped early: the last writes are missing
result = k.check(broken, ref_fm, [], [], 3, tail)
check(not result["identical"], "a native driver that stops early fails the whole check")
no_command = [l for l in native if l.kind != "C"]
check(not k.check(no_command, ref_fm, [], [], 3, tail)["identical"], "a command that is never consumed fails the whole check")
try:
    k.check([l for l in native if l.kind != "E"], ref_fm, [], [], 3, tail)
    check(False, "a log without the end marker is refused")
except k.CheckError:
    check(True, "a log without the end marker is refused")

print("failures: %d" % len(failures))
sys.exit(1 if failures else 0)
