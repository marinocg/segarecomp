#!/usr/bin/env python3
"""SEG-070-T002 (ADR 0149): the timed standard-pad machine input of the Sega CD BIOS-image profile.

  * tests/tools/sega_cd_input_table.c: the pad protocol of every button in both TH states, the data/control register semantics, the
    event timing at and around the exact transition ticks, same-tick events, the partition independence of the timeline, every
    script refusal class and typed stops of unsupported accesses - all against hand-derived values;
  * a generated-native fixture (tools/sega_cd_bios_input_fixture.py): ordinary MC68000 instructions poll both ports with the TH
    multiplex; the polling results the program itself stored must equal the machine's own read log and the hand-derived expectations;
    a press placed exactly on the tick of a read is seen by that read, one tick later it is not; two events at one tick;
    byte-identical outcomes over repeats and at least eight deadline partitions; no-script reports unchanged;
    refusals before any guest instruction; typed stops unchanged with a script present.

usage: sega_cd_input_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_input_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
BUDGET = 3_000_000
HEADER = "segarecomp-input-script 1\n"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def run_program(exe, disc, bios, *extra, budget=BUDGET):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", str(budget), *map(str, extra)],
                         capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


def read_log(path):
    rows = []
    for line in pathlib.Path(path).read_text().splitlines():
        tick, onset, port, value = line.split()
        rows.append((int(tick), int(onset), int(port), int(value, 16)))
    return rows


def main():
    with tempfile.TemporaryDirectory(prefix="scd-input-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / ("input_table" + common.EXE)
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        str(ROOT / "tests" / "tools" / "sega_cd_input_table.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        out = subprocess.run([str(table)], capture_output=True, text=True, timeout=120)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        check(out.returncode == 0 and len(rows) >= 140 and not any(r.startswith("FAIL") for r in rows),
              "input table: %d rows" % len(rows))
        for line in rows:
            if line.startswith("FAIL"):
                print("      " + line)

        # ---- the onset-schedule author: ticks derived by hand (ONSET = 224 lines * 3420 * 500,000; FRAME = 262 lines) ----
        tool = ROOT / "tools" / "sega_cd_input_script.py"
        text = subprocess.run([sys.executable, str(tool), "onset", "--base-onsets", "1197", "--press", "480", "--hold", "6"],
                              capture_output=True, text=True).stdout
        onset = 224 * 3420 * 500_000
        frame = 262 * 3420 * 500_000
        want = "segarecomp-input-script 1\n%d 1 press START\n%d 1 release START\n" % (onset + 1676 * frame, onset + 1682 * frame)
        check(frame == 448_020_000_000 and onset == 383_040_000_000 and text == want,
              "the onset-schedule author: press at onset 1677, release at onset 1683 (%s)" % text.replace("\n", " | "))
        check(subprocess.run([sys.executable, str(tool), "onset", "--base-onsets", "0", "--press", "0", "--hold", "6"],
                             capture_output=True).returncode == 2, "the author refuses an onset number below 1")

        # ---- the generated-native fixture ----
        disc = tmp / "disc"
        common.build_disc(disc, "comm")
        fx = tmp / "fx"
        expected = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx)], check=True, capture_output=True,
                                             text=True).stdout)
        bios = fx / "bios.bin"
        (tmp / "empty-build.txt").write_text(HEADER)   # the build driver hands the script to every pass (SEG-070-T002)
        built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"),
                                "--bios", str(bios), "--work", str(tmp / "work"), "--input-script", str(tmp / "empty-build.txt")],
                               capture_output=True, text=True, timeout=1800)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok"), "the fixture builds (materialization fixed point): %s" % built.stdout[-400:])
        if not summary.get("ok"):
            return 1
        exe = pathlib.Path(summary["program"])
        n, per = expected["iterations"], expected["reads_per_iteration"]
        released = expected["released_pattern"] * n

        def block(report):
            return list(bytes.fromhex(report["result_block"])[: n * per])

        # no script: every poll reads the released values; no `input` report field
        code, plain = run_program(exe, disc, bios)
        check(code == 0 and plain.get("result") == "idle" and "input" not in plain, "no script: idle, no input report field")
        check(block(plain) == released and bytes.fromhex(plain["result_block"])[0xFF] == 0xA5,
              "no script: every one of the %d polls reads the hand-derived released value ($33/$7F) on both ports" % (n * per))
        # a header-only script observes and changes nothing else
        empty = tmp / "empty.txt"
        empty.write_text(HEADER)
        log0 = tmp / "log0.txt"
        code, observed = run_program(exe, disc, bios, "--input-script", empty, "--input-log", log0)
        rest = dict(observed)
        info = rest.pop("input", None)
        check(code == 0 and rest == plain, "a header-only script: every other report field identical to the run without a script")
        check(info is not None and info["events"] == 0 and info["applied"] == 0 and info["reads"] == [2 * n, 2 * n] and info["logged"] == 4 * n and
              info["log_dropped"] == 0, "a header-only script: 0 events, %d polls per port observed (%s)" % (2 * n, info))
        ticks = read_log(log0)
        check(len(ticks) == 4 * n and [t[3] for t in ticks] == released and all(a[0] < b[0] for a, b in zip(ticks, ticks[1:])) and
              [t[2] for t in ticks] == [1, 1, 2, 2] * n, "the read log: %d reads in order, strictly increasing ticks, ports 1,1,2,2, released values" % len(ticks))
        # slot s of iteration i is read 4 * i + s: TH-low port 1 = slot 0, TH-high port 1 = 1, TH-low port 2 = 2, TH-high port 2 = 3
        t_press, t_release = ticks[4 * 10][0], ticks[4 * 14][0]

        def scripted(name, events):
            path = tmp / (name + ".txt")
            path.write_text(HEADER + "".join("%d %d %s %s\n" % e for e in events))
            return path

        def run_script(name, events, *extra):
            log = tmp / (name + ".log")
            code, report = run_program(exe, disc, bios, "--input-script", scripted(name, events), "--input-log", log, *extra)
            return code, report, read_log(log)

        # START on port 1 exactly on the tick of iteration 10's first read (the TH-low port-1 read), released exactly on iteration
        # 14's; DOWN on port 2 at the same two ticks (two events at one tick: both applied before the access at that tick)
        events = [(t_press, 1, "press", "START"), (t_press, 2, "press", "DOWN"), (t_release, 1, "release", "START"),
                  (t_release, 2, "release", "DOWN")]
        code, rep, log = run_script("exact", events)
        want = list(released)
        for i in range(10, 14):
            want[4 * i + 0] = 0x13   # port 1 TH low: START pressed (bit 5 low)
            want[4 * i + 2] = 0x31   # port 2 TH low: DOWN pressed (bit 1 low)
            want[4 * i + 3] = 0x7D   # port 2 TH high: DOWN (bit 1 low)
        check(code == 0 and block(rep) == want,
              "events exactly on read ticks: iterations 10-13 see START (port 1) and DOWN (port 2), 9 and 14 do not: the access at the event's own tick sees it")
        check([t[3] for t in log] == want, "the machine's own read log equals what the program stored and what was derived by hand")
        check(rep["input"]["events"] == 4 and rep["input"]["applied"] == 4 and rep["input"]["pad"] == [0, 0], "all four events applied, pad released at the end")
        # one tick later: the read at the event tick still sees the old state
        events = [(t_press + 1, 1, "press", "START"), (t_release + 1, 1, "release", "START")]
        code, rep, _ = run_script("later", events)
        want = list(released)
        for i in range(11, 15):
            want[4 * i + 0] = 0x13
        check(code == 0 and block(rep) == want,
              "events one tick after the reads: the read at tick T still sees the old state, the next iteration the new (START in iterations 11-14)")
        # one tick earlier than the read: seen
        events = [(t_press - 1, 1, "press", "START"), (t_release - 1, 1, "release", "START")]
        code, rep, _ = run_script("earlier", events)
        want = list(released)
        for i in range(10, 14):
            want[4 * i + 0] = 0x13
        check(code == 0 and block(rep) == want, "events one tick before the reads: the same result as exactly on them (iterations 10-13)")
        # button change between reads: UP then DOWN across a poll, A pressed at the same instant (TH-low shows A, TH-high shows none)
        t_a, t_b = ticks[4 * 5 + 1][0], ticks[4 * 5 + 3][0]
        events = [(t_a, 1, "press", "UP"), (t_b, 1, "release", "UP"), (t_b, 1, "press", "A")]
        code, rep, _ = run_script("between", events)
        want = list(released)
        want[4 * 5 + 1] = 0x7E            # iteration 5 port 1 TH-high read at the press tick: UP
        for i in range(6, n):
            want[4 * i + 0] = 0x23        # A pressed from iteration 5's port-2 TH-high read on: port 1 TH low shows A (bit 4)
        check(code == 0 and block(rep) == want,
              "buttons change between reads of one poll: UP visible for one read only, then A held to the end")

        # ---- determinism: repeats and >= 8 deadline partitions, reports and logs ----
        events = [(t_press, 1, "press", "START"), (t_press, 2, "press", "DOWN"), (t_release, 1, "release", "START"),
                  (t_release, 2, "release", "DOWN")]
        code, base, base_log = run_script("exact", events)
        for k in range(2):
            c, r, l = run_script("exact", events)
            check(c == 0 and r == base and l == base_log, "repeat %d: identical report and read log" % k)
        for seed in range(8):
            c, r, l = run_script("exact", events, "--slice-seed", seed)
            check(c == 0 and r == base and l == base_log, "deadline partition (seed %d): identical report and read log" % seed)
        for ticks_slice in (1_000_003, t_press, t_press + 1, 777_777_777):
            c, r, l = run_script("exact", events, "--slice-ticks", ticks_slice)
            check(c == 0 and r == base and l == base_log, "fixed deadline slicing (%d ticks): identical report and read log" % ticks_slice)
        # the run stopped by a deadline between the two events and resumed in the same process is the slicing above; a run ended
        # before the first event reports nothing applied and the released reads
        c, r, _ = run_script("early", events, "--until-tick", t_press)
        check(c == 11 and r["input"]["applied"] == 0 and r["input"]["pad"] == [0, 0], "ended before the first event tick: nothing applied")

        # ---- refusals before any guest instruction (exit 23, typed class and line) ----
        refusals = [
            ("bad.txt", "%s100 3 press START\n" % HEADER, "bad_port", 2),
            ("bad2.txt", "%s200 1 press UP\n100 1 release UP\n" % HEADER, "tick_order", 3),
            ("bad3.txt", "%s100 1 press MODE\n" % HEADER, "unsupported_button", 2),
            ("bad4.txt", "100 1 press START\n", "missing_header", 1),
            ("bad5.txt", "%s100 1 press UP\n200 1 press UP\n" % HEADER, "redundant_transition", 3),
        ]
        for name, text, cls, line in refusals:
            (tmp / name).write_text(text)
            code, rep = run_program(exe, disc, bios, "--input-script", tmp / name)
            check(code == 23 and rep == {"result": "input_refused", "error": cls, "line": line},
                  "refused before any guest instruction: %s line %d -> exit %d %s" % (cls, line, code, rep))
        code, rep = run_program(exe, disc, bios, "--input-script", tmp / "absent.txt")
        check(code == 23 and rep == {"result": "input_refused", "error": "unreadable", "line": 0}, "an unreadable script is refused")
        code, _ = run_program(exe, disc, bios, "--input-log", tmp / "orphan.log")
        check(code == 2, "--input-log without --input-script is a usage error")
        # the synthetic (non-BIOS) profile has no controller: a script is refused there
        sdisc, sexe, _, _ = builder.program("subside", "subside")
        code, rep = common.run(sexe, sdisc, 6000000, "--input-script", str(empty))
        check(code == 23 and rep == {"result": "input_refused", "error": "unsupported_profile", "line": 0},
              "the synthetic profile refuses a script (no controller there): %s" % rep)

        # ---- unsupported accesses stay typed stops, with or without a script ----
        for spec, want_stop in expected["negatives"].items():
            for extra in ((), ("--input-script", empty)):
                code, neg = run_program(exe, disc, bios, "--set", spec, *extra)
                stop = neg.get("stop", {})
                lo, hi = want_stop["range"]
                check(code == 10 and stop.get("class") == "main_device_unsupported" and lo <= stop.get("pc", -1) < hi and
                      stop.get("address") == want_stop["address"] and stop.get("width") == want_stop["width"] and
                      stop.get("write") == want_stop["write"],
                      "%s%s: typed stop main_device_unsupported at $%X (%s)" % (spec, " with a script" if extra else "", want_stop["address"], stop))
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
