#!/usr/bin/env python3
"""SEG-078-T001 (ADR 0204): the Sega CD live (interactive) driver, hermetic.

A project-authored BIOS-like image (tools/sega_cd_bios_live_fixture.py) draws a backdrop whose colour is a function of the pad its vertical-
interrupt handler reads. Its viewer variant is linked with a scripted test host (tests/tools/sega_cd_live_test_host.c) instead of the SDL3
viewer: the same driver loop, the same presenter hook, no window. Checked end to end, against values derived here and not from the runtime:

  * tests/tools/sega_cd_live_table.c: the recorder (grammar, limits, all-or-nothing), the pacer period, presentation counts;
  * every presented frame is the frame the pad of the poll before it produced (canonical digest computed here);
  * the recorded script replayed headlessly (and under other deadline slicing) reproduces the live report, the input counters and digest,
    and the captured frames exactly; the live run itself is partition independent;
  * pause, resume and single step change neither machine time nor the recording; the pad changed while paused is stamped when the machine moves;
  * quit, recorder capacity (a valid prefix, typed exit), host failure and usage refusals; the headless program and its report are unchanged.

usage: sega_cd_live_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_live_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
HOST = ROOT / "tests" / "tools" / "sega_cd_live_test_host.c"
HEADER = "segarecomp-input-script 1\n"
FRAME = 262 * 3420 * 500_000
FIRST_ONSET = 111_858 * 500_000
TPS = 26_846_587_500_000
BUDGET = 400_000_000
failures = []

UP, DOWN, LEFT, RIGHT, B, C, A, START = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def raw_bytes(mask):
    """The TH-high and TH-low bytes of a three-button pad (0 = pressed): derived from the pad protocol, not from the runtime."""
    high = 0x7F & ~(mask & (UP | DOWN | LEFT | RIGHT | B | C))
    low = 0x33
    for button, bit in ((START, 0x20), (A, 0x10), (UP, 0x01), (DOWN, 0x02)):
        if mask & button:
            low &= ~bit
    return high & 0xFF, low & 0xFF


def backdrop_word(mask):
    """CRAM word 0 the handler writes for a pad (the fixture's documented formula) -> the canonical frame digest of a flat backdrop."""
    high, low = raw_bytes(mask)
    p = (~high) & 0x3F
    q = (~low) & 0x30
    return ((p & 7) << 1) | ((p & 0x38) << 2) | (q << 5)


def flat_digest(word):
    colour = (((word >> 9) & 7) << 6) | (((word >> 5) & 7) << 3) | ((word >> 1) & 7)
    pixel = struct.pack("<H", (1 << 9) | colour)
    return hashlib.sha256(struct.pack("<HH", 256, 224) + pixel * (256 * 224)).hexdigest()


def tick_of_onset(n):
    return FIRST_ONSET + (n - 1) * FRAME


def run(exe, args, env=None, timeout=300):
    out = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=timeout, env={**os.environ, **(env or {})})
    try:
        report = json.loads(out.stdout) if out.stdout.strip() else {}
    except ValueError:
        report = {"unparsable": out.stdout[-200:]}
    return out.returncode, report


def read_log(path):
    frames, polls, paces, end = {}, [], [], None
    for line in pathlib.Path(path).read_text().splitlines():
        f = line.split()
        if f[0] == "F":
            frames[int(f[1])] = f[2]
        elif f[0] == "P":
            polls.append((int(f[1]), int(f[2])))
        elif f[0] == "T":
            paces.append((int(f[1]), int(f[2])))
        elif f[0] == "E":
            end = f[1]
    return frames, polls, paces, end


def main():
    with tempfile.TemporaryDirectory(prefix="scd-live-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / ("live_table" + common.EXE)
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        "-I", str(ROOT / "platforms" / "genesis" / "viewer"), str(ROOT / "tests" / "tools" / "sega_cd_live_table.c"),
                        str(ROOT / "platforms" / "genesis" / "viewer" / "viewer.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        out = subprocess.run([str(table)], capture_output=True, text=True, timeout=300)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        check(out.returncode == 0 and len(rows) >= 40 and not any(r.startswith("FAIL") for r in rows), "live table: %d rows" % len(rows))
        for line in rows:
            if line.startswith("FAIL"):
                print("      " + line)

        disc = tmp / "disc"
        common.build_disc(disc, "comm")
        fx = tmp / "fx"
        subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx)], check=True, capture_output=True, text=True)
        bios = fx / "bios.bin"
        built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"),
                                "--bios", str(bios), "--work", str(tmp / "work"), "--pass-set", "main:d6=3",
                                "--viewer-host-source", str(HOST)], capture_output=True, text=True, timeout=1800)
        summary = json.loads(built.stdout) if built.stdout.strip() else {}
        check(built.returncode == 0 and summary.get("ok") and summary.get("viewer", {}).get("host") == "test",
              "the fixture builds with its viewer variant" + ("" if summary.get("ok") else ": %s" % built.stdout[-300:]))
        if not summary.get("ok"):
            return 1
        exe, vexe = pathlib.Path(summary["program"]), pathlib.Path(summary["viewer"]["program"])
        base = ["--disc-dir", disc, "--bios", bios, "--instruction-budget", BUDGET, "--set", "main:d6=40"]

        # ---- headless program unchanged; both describe the same program ----
        code, plain = run(exe, base)
        check(code == 0 and plain.get("result") == "idle" and "live" not in plain and "input" not in plain, "headless: idle, no live and no input report field")
        high, low = raw_bytes(0)
        block = bytes.fromhex(plain["result_block"])
        check(block[0x10] == high and block[0x11] == low and block[0xFF] == 0xA5 and int.from_bytes(block[0x20:0x22], "big") == 40,
              "headless: the handler ran 40 frames and read the released pad ($7F/$33)")
        code, d_head = run(exe, ["--describe"])
        code2, d_view = run(vexe, ["--describe"])
        check(code == 0 and code2 == 0 and d_head["describe"] == 1 and d_head["runtime_abi"] == 1 and d_head["viewer"] is False and d_view["viewer"] is True,
              "--describe: runtime ABI 1; the headless program is not a viewer, the viewer variant is")
        strip = lambda d: {k: v for k, v in d.items() if k != "viewer"}   # noqa: E731
        check(strip(d_head) == strip(d_view) and d_head["program_identity"] == summary["program_identity"] and d_head["bios"]["size"] == 4096 and
              d_head["bios"]["sha256"] == hashlib.sha256(bios.read_bytes()).hexdigest() and d_head["profile"] == "bios_image",
              "--describe: the viewer variant is the same program (identity, BIOS and disc facts equal)")
        check(run(exe, ["--describe", "--disc-dir", disc])[0] == 2, "--describe accepts no other argument")
        check(run(exe, base + ["--record-input", tmp / "x.txt"])[0] == 2, "--record-input is a usage error on the headless program")

        # ---- the pad sequence of the "keyboard" ----
        pads = [0] * 4 + [RIGHT] * 6 + [RIGHT | A] * 3 + [START] * 2 + [0] * 6 + [UP | B] * 3 + [LEFT | C] * 4 + [DOWN] * 5 + [0] * 12
        assert len(pads) >= 45
        script = tmp / "host.txt"
        script.write_text("".join("%02x\n" % p for p in pads))
        captures = [2, 3, 10, 17, 25, 38]

        def live(name, pad_script, extra=(), env=None):
            rec, rep, log = tmp / (name + ".rec"), tmp / (name + ".json"), tmp / (name + ".log")
            e = {"SCD_TEST_HOST_SCRIPT": str(pad_script), "SCD_TEST_HOST_LOG": str(log), **(env or {})}
            capdir = tmp / (name + "-cap")
            capdir.mkdir(exist_ok=True)
            cap = []
            for n in captures:
                cap += ["--capture-onset", n]
            code, report = run(vexe, base + ["--record-input", rec, "--report", rep, "--input-log", tmp / (name + ".reads"),
                                              "--capture-dir", capdir, *cap, *extra], e)
            report = json.loads(rep.read_text()) if rep.exists() else report
            return code, report, rec.read_text() if rec.exists() else None, read_log(log) if log.exists() else None

        code, lrep, recorded, hostlog = live("l1", script)
        frames, polls, paces, end = hostlog
        check(code == 0 and lrep["result"] == "idle" and lrep["live"]["outcome"] == "machine_stop", "live: the program ran to its end (idle), exit 0")
        check(lrep["live"]["skipped"] == 1 and lrep["live"]["rejected"] == 0 and lrep["live"]["presented"] == lrep["live"]["rendered"] ==
              len(frames) and sorted(frames) == list(range(2, 2 + len(frames))), "live: onsets 2.. presented once each, the power-on frame skipped, none rejected (%d frames)" % len(frames))
        check(len(frames) >= 38 and lrep["bios_profile"]["vblank_onsets"] == 40, "live: 40 vertical-blank onsets, %d frames presented" % len(frames))
        # every presented frame is the frame the pad of the poll before it made
        wrong = [k for k in frames if k - 2 < len(pads) and frames[k] != flat_digest(backdrop_word(pads[k - 2]))]
        check(not wrong, "live: every presented frame equals the independently derived backdrop of its pad (poll k-2); mismatches %s" % wrong[:5])
        check(len({frames[k] for k in frames}) >= 6, "live: the pad is visible - at least six distinct frames")
        # the pacer is told the machine's frame length
        check(paces and all(p == (FRAME, TPS) for p in paces) and FRAME / TPS != 1 / 60, "live: paced by the machine's own frame (%d ticks at %d Hz tick rate), not 60 Hz" % (FRAME, TPS))
        check(len(paces) == len(polls) - 1 or len(paces) == len(polls), "live: one pace call per frame advanced")

        # ---- the recording ----
        events = []
        prev = 0
        for line in recorded.splitlines()[1:]:
            tick, port, action, names = line.split()
            events.append((int(tick), int(port), action, names))
        want = []
        for j, pad in enumerate(pads[:40]):
            if pad != prev:
                rel, prs = prev & ~pad, pad & ~prev
                order = ("UP", UP), ("DOWN", DOWN), ("LEFT", LEFT), ("RIGHT", RIGHT), ("A", A), ("B", B), ("C", C), ("START", START)
                for action, mask in (("release", rel), ("press", prs)):
                    if mask:
                        want.append((tick_of_onset(j + 1), 1, action, ",".join(n for n, bit in order if mask & bit)))
            prev = pad
        check(recorded.startswith(HEADER) and events == want[:len(events)] and len(events) >= len(want) - 1,
              "record: the script holds exactly the pad's transitions, each stamped at the onset tick of its poll (%d events)" % len(events))
        check(all(t in {tick_of_onset(n) for n in range(1, 60)} for t, _, _, _ in events), "record: no event carries a tick that is not a frame boundary")

        # ---- replay equality (headless program, recorded script) ----
        def replay(name, extra=()):
            rep = tmp / (name + ".json")
            capdir = tmp / (name + "-cap")
            capdir.mkdir(exist_ok=True)
            cap = []
            for n in captures:
                cap += ["--capture-onset", n]
            rec = tmp / "l1.rec"
            code, report = run(exe, base + ["--input-script", rec, "--input-log", tmp / (name + ".reads"), "--report", rep,
                                            "--capture-dir", capdir, *cap, *extra])
            return code, json.loads(rep.read_text())

        def without_live(report):
            return {k: v for k, v in report.items() if k != "live"}

        code, rrep = replay("r1")
        check(code == 0 and without_live(lrep) == rrep, "replay: the headless replay report equals the live report exactly (every register, digest, input counter and capture)")
        check(rrep["input"]["events"] == len(events) and rrep["input"]["applied"] == len(events) and rrep["input"]["log_dropped"] == 0 and
              rrep["input"]["reads"][0] > 0, "replay: input counters - %s" % {k: rrep["input"][k] for k in ("events", "applied", "reads", "logged")})
        caps = {c["onset"]: c["sha256"] for c in rrep["frame_captures"]}
        check(len(caps) == len(captures) and all(caps[n] == frames[n] for n in captures), "replay: the captured frames are the presented frames (canonical digests)")
        check((tmp / "l1.reads").read_bytes() == (tmp / "r1.reads").read_bytes(), "replay: the data-port read logs are byte-identical")
        for label, extra in (("slice-ticks 12345678901", ["--slice-ticks", 12345678901]), ("slice-seed 7", ["--slice-seed", 7]),
                             ("slice-ticks 3", ["--slice-ticks", 3_000_000_000_000])):
            code, again = replay("r-" + label.replace(" ", "-"), extra)
            check(code == 0 and again == rrep, "replay under different deadline slicing (%s): identical report" % label)
        for label, extra in (("slice-seed 5", ["--slice-seed", 5]), ("slice-ticks 99999999999", ["--slice-ticks", 99999999999])):
            code, lrep2, rec2, log2 = live("l-" + label.replace(" ", "-"), script, extra)
            check(code == 0 and without_live(lrep2) == rrep and rec2 == recorded and log2[0] == frames,
                  "live under different deadline slicing (%s): same report, same recording, same frames" % label)

        # ---- pause, resume and single step ----
        paused = []
        for j, pad in enumerate(pads):
            paused.append("%02x\n" % pad)
            if j in (3, 9, 20):
                paused += ["%02x P\n" % (pad ^ START)] * 3 + ["%02x PS\n" % (pad ^ START), "%02x P\n" % pad, "%02x PS\n" % pad]   # held, one step, held, one step
        paused_script = tmp / "paused.txt"
        paused_script.write_text("".join(paused))
        # the stepped polls advance a frame with the pad they carry: build the advancing sequence they imply
        advancing = []
        for line in paused:
            f = line.split()
            if len(f) == 1 or "S" in f[1]:
                advancing.append(int(f[0], 16))
        sequence_script = tmp / "sequence.txt"
        sequence_script.write_text("".join("%02x\n" % p for p in advancing))
        code, prep, prec, plog = live("p1", paused_script)
        code0, srep, srec, slog = live("p0", sequence_script)
        check(code == 0 and code0 == 0 and without_live(prep) == without_live(srep) and prec == srec and plog[0] == slog[0],
              "pause/step: a run with held and single-stepped polls equals the run of the polls that advanced (report, recording, frames)")
        check(prep["live"]["paused_polls"] == 3 * 3 + 3 * 1 and prep["live"]["steps"] == 2 * 3 and srep["live"]["paused_polls"] == 0,
              "pause/step: held polls and steps counted (%s)" % {k: prep["live"][k] for k in ("paused_polls", "steps")})
        held_boundaries = [b for b, _ in plog[1]]
        check(all(held_boundaries[i] <= held_boundaries[i + 1] for i in range(len(held_boundaries) - 1)), "pause/step: the frame index never goes back")

        # ---- quit: the run ends at a boundary; the recording replays to the same instant ----
        quit_script = tmp / "quit.txt"
        quit_script.write_text("".join("%02x\n" % p for p in pads[:17]) + "00 Q\n")
        code, qrep, qrec, qlog = live("q1", quit_script)
        check(code == 0 and qrep["live"]["outcome"] == "quit" and qrep["live"]["frames"] == 17 and qlog[3] == "quit" and qrec.startswith(HEADER),
              "quit: exit 0, outcome quit after 17 frames, the recording is written")
        until = tick_of_onset(1) + 17 * FRAME
        rep = tmp / "q-replay.json"
        code, _ = run(exe, base + ["--input-script", tmp / "q1.rec", "--until-tick", until, "--report", rep, "--input-log", tmp / "q-replay.reads"])
        qr = json.loads(rep.read_text())
        ql = json.loads((tmp / "q1.json").read_text())
        keep = lambda r: {k: v for k, v in r.items() if k not in ("live", "frame_captures")}   # noqa: E731
        check(keep(ql) == keep(qr) and qr["result"] == "deadline" and qr["ticks"] == ql["ticks"],
              "quit: the replay to the quit instant (--until-tick) reproduces the live report")

        # ---- recorder capacity: a typed stop that keeps the valid prefix ----
        flip = tmp / "flip.txt"
        flip.write_text("".join("%02x\n" % (0x01 if j & 1 else 0x02) for j in range(3500)))
        long_base = ["--disc-dir", disc, "--bios", bios, "--instruction-budget", BUDGET, "--set", "main:d6=4000"]
        rec, rep, log = tmp / "c2.rec", tmp / "c2.json", tmp / "c2.log"
        code, _ = run(vexe, long_base + ["--record-input", rec, "--report", rep], {"SCD_TEST_HOST_SCRIPT": str(flip), "SCD_TEST_HOST_LOG": str(log)}, timeout=600)
        crep = json.loads(rep.read_text())
        text = rec.read_text()
        check(code == 25 and crep["live"]["outcome"] == "recording_capacity" and read_log(log)[3] == "recording_capacity",
              "capacity: the run stops with the typed exit 25 (outcome recording_capacity)")
        check(HEADER in text and len(text.encode()) <= 65536 and len(text.splitlines()) - 1 < 4096 and len(text.splitlines()) - 1 == crep["input"]["events"],
              "capacity: the recording is the valid prefix - %d events, %d bytes" % (len(text.splitlines()) - 1, len(text.encode())))
        until = tick_of_onset(1) + crep["live"]["frames"] * FRAME
        rep2 = tmp / "c2-replay.json"
        run(exe, long_base + ["--input-script", rec, "--until-tick", until, "--report", rep2])
        check(keep(json.loads(rep2.read_text())) == keep(crep), "capacity: the replay of the kept prefix reproduces the live report to the stop")

        # a session that records nothing has no capacity: the same 3,500 transitions run to the end of the program
        rec2, rep3, log3 = tmp / "nr.rec", tmp / "nr.json", tmp / "nr.log"
        code, _ = run(vexe, long_base + ["--report", rep3], {"SCD_TEST_HOST_SCRIPT": str(flip), "SCD_TEST_HOST_LOG": str(log3)}, timeout=600)
        nrep = json.loads(rep3.read_text())
        check(code == 0 and nrep["live"]["outcome"] in ("quit", "machine_stop") and nrep["input"]["events"] >= 3400 and nrep["input"]["applied"] >= nrep["input"]["events"] - 2,
              "no recording: the same session is not bounded by the recorder's limits (%s events counted)" % nrep["input"]["events"])

        # ---- host failures and usage refusals ----
        for what in ("poll", "present", "pace"):
            code, frep = run(vexe, base, {"SCD_TEST_HOST_SCRIPT": str(script), "SCD_TEST_HOST_FAIL": what})
            check(code == 24, "a failing host (%s) is the typed exit 24" % what)
        check(run(vexe, base + ["--record-input", tmp / "y.txt", "--input-script", tmp / "l1.rec"])[0] == 2, "--record-input with --input-script is a usage error")
        # replay in the viewer variant: the script drives the machine, the host pad is not sampled
        rep = tmp / "vreplay.json"
        capdir = tmp / "vr-cap"
        capdir.mkdir()
        cap = []
        for n in captures:
            cap += ["--capture-onset", n]
        code, _ = run(vexe, base + ["--input-script", tmp / "l1.rec", "--input-log", tmp / "vr.reads", "--report", rep, "--capture-dir", capdir, *cap],
                      {"SCD_TEST_HOST_SCRIPT": str(script), "SCD_TEST_HOST_LOG": str(tmp / "vr.log")})
        vrep = json.loads(rep.read_text())
        check(code == 0 and without_live(vrep) == rrep and read_log(tmp / "vr.log")[0] == frames,
              "viewer replay (--input-script): the recorded script drives the viewer variant to the same report and the same presented frames")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
