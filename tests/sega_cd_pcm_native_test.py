#!/usr/bin/env python3
"""SEG-102 (ADR 0233, 0235): the RF5C164 channel pointers on the generated-native CPU path.

The project-authored fixture `pcm` (tools/sega_cd_fixture_disc.py) restarts the chip with RES0 = 0, starts channel 0 at a fixed step and reads
its pointer registers (byte low, byte high, word) at instants derived from the MC68000 cycle table alone: exactly on a sample event, just
before and just after one. The test checks

  * the pointer values the CPU receives against those hand-derived expectations;
  * the diagnostic trace (--pcm-trace) against an independent closed-form oracle written here: for every recorded read, the pointer equals the
    number of grid events in [ON write, read) (the events at multiples of 384 sub cycles from the reset instant);
  * determinism: two process runs and eight deadline partitions give identical reports and identical traces;
The typed stop that must remain (a read of a register that is not a pointer) is covered by sega_cd_subside_test (negative case 3) and
sega_cd_pcm_test.

usage: sega_cd_pcm_native_test.py <segarecomp> <c-compiler> <source-root>
"""
import pathlib
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []
SUB_CYCLE_TICKS = 2147727  # the synthetic profile's exact 12.5 MHz sub cycle (SCD_SUB_CYCLE_TICKS)
PERIOD = 384 * SUB_CYCLE_TICKS


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def trace_lines(path):
    return [line.split() for line in pathlib.Path(path).read_text().splitlines()]


def main():
    with tempfile.TemporaryDirectory(prefix="scd-pcm-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        disc, exe, expected, emitted = builder.program("pcm", "pcm")
        check(all(p["unsupported"] == 0 for p in emitted["processors"]), "pcm: every reached instruction compiled")
        trace = tmp / "pcm.trace"
        code, report = common.run(exe, disc, 6000000, "--pcm-trace", str(trace))
        check(code == 0 and report["result"] == "idle", "pcm: the run ends idle (exit %d, %s)" % (code, report.get("result")))
        d = report["cpus"][1]["d"]
        for register, value in sorted(expected["d"].items()):
            check(d[int(register[1])] == value, "pointer read on the CPU: %s = %d (hand-derived from the MC68000 cycle table), got %d" % (
                register, value, d[int(register[1])]))
        pcm = report["sub_devices"]["pcm"]
        check(pcm["enabled"] == 1 and pcm["running"] == 1 and pcm["onoff"] == 0xFE and pcm["addr"][0] == 40 << 11,
              "pcm: the final chip state (enabled, channel 0 running, address 40 bytes, fraction 0)")

        # --- independent oracle over the trace: pointer = grid events in [ON write, read) ---
        lines = trace_lines(trace)
        resets = [int(l[2]) for l in lines if l[0] == "Z"]
        writes = [(int(l[2]), int(l[3], 16), int(l[4], 16)) for l in lines if l[0] == "W"]
        reads = [(int(l[2]), int(l[3], 16), int(l[4]), int(l[5], 16)) for l in lines if l[0] == "R"]
        check(len(resets) == 1 and len(reads) == 6, "trace: one chip reset and six pointer reads")
        origin = resets[0]
        on_tick = next(t for (t, off, val) in writes if off == 8 and val == 0xFE)
        ok = True
        for tick, off, width, value in reads:
            # events at origin + k x PERIOD, k >= 0, strictly before `tick`, and at or after the ON write
            events = [k for k in range(0, 100) if on_tick <= origin + k * PERIOD < tick]
            count = len(events)
            channel_low = (off >> 1) & 0x1FFF == 0x10
            want = (count & 0xFF) if channel_low else (count >> 8) & 0xFF
            ok &= value == want
        check(ok, "trace: every pointer read equals the number of grid events between the ON write and the read (closed form)")
        gaps = sorted(set(((tick - origin) % PERIOD) for tick, _, _, _ in reads))
        check(0 in gaps and any(0 < g < 20 * SUB_CYCLE_TICKS for g in gaps) and any(g > PERIOD - 8 * SUB_CYCLE_TICKS for g in gaps),
              "trace: the reads cover an instant exactly on an event, shortly after one and shortly before one")

        # --- determinism ---
        check(common.run(exe, disc, 6000000)[1] == report, "pcm: two process runs give identical reports")
        partitions = [["--slice-ticks", str(t)] for t in (100000000, 777777777, 50000000000)]
        partitions += [["--slice-seed", str(s)] for s in range(1, 9)]
        same = []
        for number, p in enumerate(partitions):
            other = tmp / ("part%d.trace" % number)
            same.append(common.run(exe, disc, 6000000, *p, "--pcm-trace", str(other))[1] == report and other.read_text() == trace.read_text())
        check(all(same), "pcm: identical state and trace for %d deadline partitions (%s)" % (len(partitions), same))
    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_pcm_native_test: OK")
    return 0


sys.exit(main())
