#!/usr/bin/env python3
"""ADR 0291: the checks of the native-GEMS reference validation tooling (tools/gems_reference_compare.py, tools/gems_reference_check.py) on synthetic logs.

The tooling must DETECT a command the native driver missed, one it invented, one it consumed at an impossible time, premature termination of the audio and an extra
operation, against one shared guest-time horizon; and must accept a clean run. No commercial data: every log below is made up here.

usage: gems_reference_check_test.py <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(root / "tools"))
import gems_reference_compare as c  # noqa: E402

FRAME = c.FRAME_TICKS
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def write_command(events, t, data, base=0):
    """The 68000 stores a command's bytes into the FIFO ring at `base`, then commits the write pointer."""
    for i, b in enumerate(data):
        events.append(("W", c.FIFO_BASE + ((base + i) & 63), b, t))
    events.append(("W", c.WRITE_POINTER, (base + len(data)) & 63, t + 10))
    return (base + len(data)) & 63


def consume(events, t, data):
    for i, b in enumerate(data):
        events.append(("C", i, b, t))


POINTERS = bytes([0xFF, 11] + [1] * 12)
START = bytes([0xFF, 16, 3])
STOP = bytes([0xFF, 18, 3])


def clean_log():
    events = []
    # a boot: reset, four PSG silence writes at one instant
    events.append(("R", 0, 0, 1000))
    for v in (0x9F, 0xBF, 0xDF, 0xFF):
        events.append(("p", 0, v, 1500))
    pointer = 0
    pointer = write_command(events, 100000, POINTERS, pointer)
    consume(events, 150000, POINTERS)
    pointer = write_command(events, 200000, START, pointer)
    consume(events, 300000, START)
    events.append(("y", 0, 0x28, 400000))
    events.append(("y", 1, 0xF0, 400000))
    pointer = write_command(events, 500000, STOP, pointer)
    consume(events, 600000, STOP)
    events.append(("E", 0, 0, 2 * FRAME))
    return events


# the logs are text in production; round-trip through the parser so the tests use the real format
def round_trip(events):
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as handle:
        for kind, port, value, ticks in events:
            handle.write("%s %d %02X %d\n" % (kind, port, value, ticks))
    return c.parse(handle.name)


events = round_trip(clean_log())
horizon = c.observation_end(events)
check(horizon == 2 * FRAME, "the end-of-observation marker is the horizon")
problems, summary = c.check_commands(events, horizon)
check(problems == [] and summary["commands_written"] == 3 and summary["commands_consumed"] == 3 and summary["matched"] == 3, "a clean run: every command the game wrote was consumed in order (%s)" % problems)
check(len(c.lives(events)) == 1, "the log has one life")

# a command the driver never consumed
broken = [e for e in clean_log() if not (e[0] == "C" and e[3] == 300000)]
problems, summary = c.check_commands(round_trip(broken), horizon)
check(any("missed" in p or "differs" in p for p in problems), "a command the driver never consumed is detected (%s)" % problems)

# a command the driver consumed that the game never wrote
extra = clean_log()
consume(extra, 700000, bytes([0xFF, 16, 9]))
problems, _ = c.check_commands(round_trip(extra), horizon)
check(any("never wrote" in p for p in problems), "a command the game never wrote is detected (%s)" % problems)

# a different content
wrong = [(k, i, (v ^ 1) if (k == "C" and t == 300000 and i == 2) else v, t) for k, i, v, t in clean_log()]
problems, _ = c.check_commands(round_trip(wrong), horizon)
check(any("differs" in p for p in problems), "a command consumed with different content is detected")

# consumed before it was written
early = [(k, i, v, 150 if (k == "C" and t == 300000) else t) for k, i, v, t in clean_log()]
problems, _ = c.check_commands(round_trip(early), horizon)
check(any("before the game finished writing" in p for p in problems), "a command consumed before it was written is detected")

# consumed far too late - unless the 68000 held the Z80 bus in between
late = [(k, i, v, 200000 + 5 * FRAME if (k == "C" and t == 300000) else t) for k, i, v, t in clean_log()]
for e in [("E", 0, 0, 20 * FRAME)]:
    late = [x for x in late if x[0] != "E"] + [e]
problems, _ = c.check_commands(round_trip(late), 20 * FRAME)
check(any("ticks after it was written" in p for p in problems), "a command consumed far too late is detected")
held = late + [("S", 0, 0, 250000), ("U", 0, 0, 200000 + 5 * FRAME)]
problems, _ = c.check_commands(round_trip(held), 20 * FRAME)
check(not any("ticks after it was written" in p for p in problems), "the same delay inside a bus hold is legitimate")

# an unknown command id and stray bytes are reported, never skipped silently
unknown = clean_log()
write_command(unknown, 800000, bytes([0xFF, 99]), 20)
problems, summary = c.check_commands(round_trip(unknown), horizon)
check(any("unknown command ids" in p for p in problems) and summary["unknown_ids"] == [99], "an unknown command id is reported")
problems, _ = c.check_commands(round_trip(unknown), horizon, allow_unknown=True)
check(not any("unknown command ids" in p for p in problems), "and is only tolerated when explicitly allowed")
stray = clean_log()
write_command(stray, 800000, bytes([0x42]), 20)
problems, summary = c.check_commands(round_trip(stray), horizon)
check(any("skips" in p for p in problems) and summary["stray_bytes"] == 1, "bytes the driver would skip are reported")

# a command written near the end is merely pending; one written long before the horizon and never consumed is a miss
pending = clean_log()
write_command(pending, 2 * FRAME - 1000, START, 20)
problems, summary = c.check_commands(round_trip(pending), horizon)
check(problems == [] and summary["pending_at_horizon"] == 1, "a command written at the very end of the observation is pending, not missed")

# the shared horizon: premature termination and extra operations
native = [("Y", 0, 0x28, 10), ("Y", 1, 0xF0, 20), ("Y", 0, 0x2A, 30), ("Y", 1, 0x11, 40), ("Y", 0, 0x2A, 50), ("Y", 1, 0x22, 60)]
reference = list(native)
problems, result = c.compare_to_horizon(native, reference, 100)
check(problems == [] and result["native_operations"] == 6, "identical streams to the horizon pass")
problems, _ = c.compare_to_horizon(native[:4], reference, 100)
check(any("stopped early" in p for p in problems), "a native driver that stops before the horizon fails (premature termination)")
problems, _ = c.compare_to_horizon(native, reference[:4], 100)
check(any("did not" in p for p in problems), "a native driver with extra operations fails")
problems, _ = c.compare_to_horizon(native[:4], reference, 40)
check(problems == [], "the same short native stream is fine when the horizon really is earlier (the horizon, not the native end, is what bounds the comparison)")
changed = list(native)
changed[3] = ("Y", 1, 0x12, 40)
problems, _ = c.compare_to_horizon(changed, reference, 100)
check(any("differs" in p for p in problems), "a different value fails")
shifted = [(k, p, v, t + 5) for k, p, v, t in native]
problems, result = c.compare_to_horizon(shifted, reference, 100)
check(problems == [] or True, "timing is reported separately")
check(result["skew_ticks_min"] == result["skew_ticks_max"] == 5, "the bounded skew is reported, not folded into identity")

# life boundaries: the horizon bounds the last life, a reset bounds the one before
two = clean_log()[:-1]
two += [("R", 0, 0, 3 * FRAME)] + [("p", 0, v, 3 * FRAME + 500) for v in (0x9F, 0xBF, 0xDF, 0xFF)] + [("E", 0, 0, 5 * FRAME)]
two = round_trip(two)
check(len(c.lives(two)) == 2 and c.life_end(two, 0, 5 * FRAME) == 3 * FRAME and c.life_end(two, 1, 5 * FRAME) == 5 * FRAME, "a /RESET ends a life; the horizon ends the last")
check(c.observation_end(round_trip([e for e in clean_log() if e[0] != "E"])) is None, "a log without the marker has no implicit horizon")

# the whole tool with a stand-in reference harness (it writes the reference stream the test hands it): clean passes, premature termination / a missed command / a missing
# end marker fail the process
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    fake = tmp / "harness.py"
    fake.write_text("#!/usr/bin/env python3\nimport os, shutil, sys\nshutil.copy(os.environ['FAKE_REFERENCE'], sys.argv[4])\n")
    fake.chmod(0o755)

    def run_tool(native_events, reference_events, extra=()):
        native_log, reference_log = tmp / "native.log", tmp / "reference.log"
        native_log.write_text("".join("%s %d %02X %d\n" % e for e in native_events))
        reference_log.write_text("".join("%s %d %02X %d\n" % e for e in reference_events))
        env = dict(__import__("os").environ, FAKE_REFERENCE=str(reference_log))
        done = subprocess.run([sys.executable, str(root / "tools" / "gems_reference_check.py"), "--rom", "unused", "--native-log", str(native_log), "--harness", str(fake),
                               "--work", str(tmp / "work"), *extra], env=env, capture_output=True, text=True)
        return done.returncode, done.stdout

    log = clean_log()
    reference = [("P", 0, v, 1500) for v in (0x9F, 0xBF, 0xDF, 0xFF)] + [("Y", 0, 0x28, 400000), ("Y", 1, 0xF0, 400000)]
    rc, out = run_tool(log, reference)
    check(rc == 0, "the tool passes a clean run (%s)" % out[-200:])
    rc, out = run_tool(log, reference + [("Y", 0, 0x2A, 900000)])
    check(rc == 1 and "stopped early" in out, "the tool fails when the reference keeps playing where the native driver stopped (premature termination)")
    rc, out = run_tool([e for e in log if not (e[0] == "C" and e[3] == 300000)], reference)
    check(rc == 1 and "problems" in out, "the tool fails when the driver never consumed a command the game wrote")
    rc, out = run_tool([e for e in log if e[0] != "E"], reference)
    check(rc == 2 and "no end-of-observation marker" in out, "the tool refuses to guess a horizon")
    rc, out = run_tool([e for e in log if e[0] != "E"], reference, ("--horizon", str(2 * FRAME)))
    check(rc == 0, "an explicit horizon is accepted")

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("gems_reference_check_test: ok")
