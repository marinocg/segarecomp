#!/usr/bin/env python3
"""Offline reference check of the native Krisalis replacement (ADR 0318, docs/testing/genesis-native-krisalis-validation.md).

The reference is the ORIGINAL Z80 driver, executed offline by an independent Z80 emulator (the third-party `z80` Python package, NOT vendored, never linked into the
product). The driver image is not read from this repository: it is reassembled from the 68000's own stores into the sound RAM that the native program logged
(`SEGARECOMP_AUDIO_WRITE_LOG` with `SEGARECOMP_AUDIO_WRITE_LOG_RAM=all`), i.e. exactly the bytes the game uploaded. Nothing here is committed with commercial data: the
logs, the ROM and the reconstructed RAM stay local.

What is compared (each a failure, never a tolerance):
  1. Independent command observation: the commands the 68000 committed (a non-zero store to the mailbox command byte, with the cells it had written before it) against the
     commands the native driver reports consuming ('C' lines): none missed, none invented, same bytes, same order.
  2. The register-level YM2612 write sequence (everything except the DAC data register $2A) of the native driver against the original's, up to one shared horizon. Neither side
     may have an extra or a missing write inside the window.
  3. The DAC data byte sequence against the original's (identical bytes in identical order; the count may differ only at the horizon).
  4. The bank-register bits of the PCM bank selection.
Time is reported (the bounded skew of each compared write), never folded into identity. Not claimed: cycle accuracy, an identical interleaving of DAC bytes with FM writes
(the original polls the DAC from inside its row code; the native model paces it by an event model).

Usage:  krisalis_reference_check.py --native-log LOG --rom ROM --revision N [--json OUT]
"""
import argparse
import json
import sys
from collections import namedtuple

Z80_TICK = 15  # master ticks per Z80 cycle
BOUNDARY_SPILL = 6  # bytes a sample boundary may shift between the two sides (the old sample is still polled while the next one starts)

MAILBOX = {1: 0x0E58, 2: 0x0F58, 3: 0x0FAC}
CELLS_AT, CELLS_LEN = 2, 12

Line = namedtuple("Line", "kind port value ticks")


class CheckError(Exception):
    pass


def parse_log(text):
    lines = []
    for raw in text.splitlines():
        parts = raw.split()
        if len(parts) != 4:
            continue
        kind, port, value, ticks = parts
        lines.append(Line(kind, int(port), int(value, 16), int(ticks)))
    return lines


# ---- the YM2612 register view of a stream of port writes ----------------------------------------------------------------------------------------------------
def split_port_stream(port_writes, revision):
    """[(port, value, ticks)] -> (fm_writes, dac_writes), each [(part, register, value, ticks)] in order.

    The DAC player's own groups are recognised at port level: revisions 2 and 3 write (A0 <- $2A, D0 <- byte); revision 1 writes (A0 <- $2A, D0 <- byte, A1 <- $B6, D1 <- $C0) on every
    poll. Everything else is the register view of the chip (address latches are per part), including the DAC enable ($2B) and the end-of-sample pan restore."""
    latch = [0, 0]
    fm, dac = [], []
    i, n = 0, len(port_writes)
    while i < n:
        port, value, ticks = port_writes[i]
        if port == 0 and value == 0x2A and i + 1 < n and port_writes[i + 1][0] == 1:
            dac.append((0, 0x2A, port_writes[i + 1][1], ticks))
            latch[0] = 0x2A
            i += 2
            if revision == 1 and i + 1 < n and port_writes[i][0] == 2 and port_writes[i][1] == 0xB6 and port_writes[i + 1][0] == 3 and port_writes[i + 1][1] == 0xC0:
                latch[1] = 0xB6
                i += 2
            continue
        if (port == 2 and value == 0xB6 and i + 3 < n and port_writes[i + 1][0] == 3 and port_writes[i + 2][:2] == (0, 0x2B) and port_writes[i + 3][:2] == (1, 0x00)):
            latch[0] = 0x2B   # the end of a sample (FM6's pan comes back from its voice, the DAC goes off): the player's own group, not row work
            latch[1] = 0xB6
            i += 4
            continue
        if revision == 2 and port == 2 and value == 0xB6 and i + 1 < n and port_writes[i + 1][0] == 3:
            latch[1] = 0xB6   # revision 2: every poll of the player rewrites the FM6 pan (an FM6 voice load's own pan write is indistinguishable and treated the same on both sides)
            i += 2
            continue
        if port == 0 and value == 0x2B and i + 1 < n and port_writes[i + 1][0] == 1:
            dac.append((0, 0x2B, port_writes[i + 1][1], ticks))   # the DAC enable: the player's start of a sample (its order against a tick the original and the model may serve in either order, see below)
            latch[0] = 0x2B
            i += 2
            continue
        part = port >> 1
        if port & 1 == 0:
            latch[part] = value
        else:
            fm.append((part, latch[part], value, ticks))
        i += 1
    return fm, dac


# ---- command observation -----------------------------------------------------------------------------------------------------------------------------------
def committed_commands(lines, revision):
    """The commands the 68000 stored: [(ticks, [cmd, 12 cell bytes])], from its own stores into the mailbox ('W' lines, offset = Z80 RAM offset)."""
    base = MAILBOX[revision]
    cells = bytearray(CELLS_LEN)
    commands = []
    activation = next((l.ticks for l in lines if l.kind == "A"), None)
    if activation is None:
        raise CheckError("the log has no activation line")
    for l in lines:
        if l.kind != "W" or l.ticks < activation:
            continue
        offset = l.port - base
        if CELLS_AT <= offset < CELLS_AT + CELLS_LEN:
            cells[offset - CELLS_AT] = l.value
        elif offset == 0 and l.value != 0:
            commands.append((l.ticks, bytes([l.value]) + bytes(cells)))
    return commands


def consumed_commands(lines):
    """[(ticks, bytes)] from the native driver's 'C' lines (13 bytes per command, index in `port`)."""
    out, current, stamp = [], bytearray(), 0
    for l in lines:
        if l.kind != "C":
            continue
        if l.port == 0 and current:
            out.append((stamp, bytes(current)))
            current = bytearray()
        if l.port == 0:
            stamp = l.ticks
        current.append(l.value)
    if current:
        out.append((stamp, bytes(current)))
    return out


def compare_commands(committed, consumed, horizon, pending_ticks):
    """Returns a list of failure strings (empty = identical)."""
    failures = []
    pending_from = horizon - pending_ticks
    i = 0
    for index, (t, bytes_) in enumerate(committed):
        if i >= len(consumed):
            if t < pending_from:
                failures.append("command %d written at %d was never consumed" % (index, t))
            continue
        ct, cb = consumed[i]
        if cb != bytes_:
            failures.append("command %d: written %s, consumed %s" % (index, bytes_.hex(), cb.hex()))
            i += 1
            continue
        if ct < t:
            failures.append("command %d consumed at %d before it was written at %d" % (index, ct, t))
        i += 1
    if i < len(consumed):
        failures.append("%d consumed command(s) the game never wrote" % (len(consumed) - i))
    return failures


# ---- stream comparison -----------------------------------------------------------------------------------------------------------------------------------------
def compare_streams(native, reference, horizon, tail_ticks, label):
    """Both are [(part, reg, value, ticks)]. Returns (failures, compared, max_skew)."""
    failures = []
    boundary = horizon - tail_ticks
    n = [w for w in native if w[3] <= horizon]
    r = [w for w in reference if w[3] <= horizon]
    compared = 0
    max_skew = 0
    skews = []
    for i in range(min(len(n), len(r))):
        if n[i][:3] != r[i][:3]:
            failures.append("%s: write %d differs: native (part %d reg %02X value %02X) reference (part %d reg %02X value %02X)" % ((label, i) + n[i][:3] + r[i][:3]))
            return failures, compared, max_skew
        compared += 1
        skews.append(n[i][3] - r[i][3])
        max_skew = max(max_skew, abs(skews[-1]))
    compare_streams.skews = skews
    if len(n) != len(r):
        longer, name = (n, "native") if len(n) > len(r) else (r, "reference")
        extra = longer[min(len(n), len(r)):]
        # only writes that sit at the very end of the window may be missing from the other side (its skew pushed them over the horizon)
        if any(w[3] < boundary for w in extra):
            failures.append("%s: %s has %d extra write(s) inside the window (first at %d)" % (label, name, len(extra), extra[0][3]))
    return failures, compared, max_skew


def _percentiles(values):
    if not values:
        return [0, 0, 0]
    ordered = sorted(abs(v) for v in values)
    return [ordered[int(len(ordered) * q) - 1 if q < 1 else -1] for q in (0.5, 0.95, 0.99)]


def dac_segments(dac, fm=None):
    """DAC data bytes grouped by sample: a segment starts at each DAC-enable write ($2B <- $80) of the same side's player stream (it is part of `dac`)."""
    segments = {}
    index = 0
    for _part, reg, value, _ticks in dac:
        if reg == 0x2B:
            if value == 0x80:
                index += 1
            continue
        segments.setdefault(index, []).append(value)
    return segments


def compare_dac(native_dac, native_fm, reference_dac, reference_fm, tolerance_bytes):
    """Per sample: the bytes both sides wrote must be the same stream; the original polls the DAC between pieces of its row work, so where a sample is cut (a new sample, a
    silence, a note on FM6) the two may differ in LENGTH by the polls the timing skew moves across the cut: at most `tolerance_bytes`. Returns (failures, bytes_compared, max_length_difference)."""
    failures = []
    n, r = dac_segments(native_dac), dac_segments(reference_dac)
    starts_n = sum(1 for w in native_dac if w[1] == 0x2B and w[2] == 0x80)
    starts_r = sum(1 for w in reference_dac if w[1] == 0x2B and w[2] == 0x80)
    if abs(starts_n - starts_r) > 1:   # at most the sample the horizon cuts
        failures.append("dac: %d samples started natively, %d by the original" % (starts_n, starts_r))
    compared, worst = 0, 0
    for key in sorted(set(n) | set(r)):
        a, b = n.get(key, []), r.get(key, [])
        # the bytes of the previous sample that are still being polled while a new one is started may fall on either side of the start marker: the first bytes may be shifted by a few
        spill = BOUNDARY_SPILL if min(len(a), len(b)) > 2 * BOUNDARY_SPILL else 0   # a short sample is compared exactly
        shift = next((d for d in sorted(range(-spill, spill + 1), key=abs)
                      if a[max(0, d):][:min(len(a) - max(0, d), len(b) - max(0, -d))] == b[max(0, -d):][:min(len(a) - max(0, d), len(b) - max(0, -d))]), None)
        if shift is None:
            m = min(len(a), len(b))
            first = next((i for i in range(m) if a[i] != b[i]), 0)
            failures.append("dac: sample %d byte %d differs: native %02X reference %02X" % (key, first, a[first] if a else 0, b[first] if b else 0))
            continue
        compared += min(len(a) - max(0, shift), len(b) - max(0, -shift))
        worst = max(worst, abs((len(a) - max(0, shift)) - (len(b) - max(0, -shift))))
        if abs((len(a) - max(0, shift)) - (len(b) - max(0, -shift))) > tolerance_bytes:
            failures.append("dac: sample %d length differs by %d bytes (native %d, reference %d)" % (key, abs(len(a) - len(b)), len(a), len(b)))
    return failures, compared, worst


def check(native_lines, reference_fm, reference_dac, reference_bank, revision, tail_ticks, dac_tolerance=64):
    horizon = next((l.ticks for l in native_lines if l.kind == "E"), None)
    if horizon is None:
        raise CheckError("the log has no end marker (E)")
    native_port_writes = [(l.port, l.value, l.ticks) for l in native_lines if l.kind == "y"]
    native_fm, native_dac = split_port_stream(native_port_writes, revision)
    failures = []
    f, fm_compared, fm_skew = compare_streams(native_fm, reference_fm, horizon, tail_ticks, "fm")
    failures += f
    f, dac_compared, dac_worst = compare_dac([w for w in native_dac if w[3] <= horizon], native_fm, [w for w in reference_dac if w[3] <= horizon], reference_fm, dac_tolerance)
    failures += f
    committed, consumed = committed_commands(native_lines, revision), consumed_commands(native_lines)
    failures += compare_commands(committed, consumed, horizon, tail_ticks)
    return {
        "horizon": horizon,
        "fm_writes_compared": fm_compared,
        "dac_bytes_compared": dac_compared,
        "dac_max_length_difference": dac_worst,
        "max_skew_ticks": fm_skew,
        "skew_ticks_p50_p95_p99": _percentiles(getattr(compare_streams, "skews", [])),
        "commands_written": len(committed),
        "commands_consumed": len(consumed),
        "failures": failures,
        "identical": not failures,
    }


# ---- the stimulus: the 68000's own bus transactions, replayed as a closed loop ---------------------------------------------------------------------------------
POLL_HOLD, POLL_GAP = 1200, 900   # master ticks the 68000 holds the bus per poll of the command byte, and waits between polls (as its wait loop does)


def make_script(lines, revision):
    """The 68000's side of a logged run as a script both implementations are driven by: the sound-RAM image at activation, then one transaction per bus hold (its time, its length and the stores
    made inside it). A transaction that stores a command is issued only when the implementation under test has cleared the previous one (the 68000 waits for that), so the two runs
    are closed loops over the SAME command sequence and neither is fed an impossible stimulus."""
    base = MAILBOX[revision]
    activation = next((l for l in lines if l.kind == "A"), None)
    bank = next((l.port for l in lines if l.kind == "B"), 0)
    horizon = next((l.ticks for l in lines if l.kind == "E"), None)
    if activation is None or horizon is None:
        raise CheckError("the log needs an activation (A) and an end marker (E)")
    out = ["A %d %d %d" % (revision, bank, activation.ticks)]
    image = {}
    transactions = []
    current = None
    for l in lines:
        if l.kind == "W" and l.ticks <= activation.ticks:
            image[l.port & 0x1FFF] = l.value
        elif l.kind == "S" and l.ticks >= activation.ticks and current is None:
            current = {"t": l.ticks, "writes": [], "hold": 0}
        elif l.kind == "W" and current is not None:
            current["writes"].append((l.port & 0x1FFF, l.value))
        elif l.kind == "U" and current is not None:
            current["hold"] = max(0, l.ticks - current["t"])
            transactions.append(current)
            current = None
    for off in sorted(image):
        out.append("I %d %d" % (off, image[off]))
    for t in transactions:
        if not t["writes"]:
            continue   # a bare poll of the 68000's wait loop: the closed loop generates its own
        has_cmd = any(off == base and value != 0 for off, value in t["writes"])
        out.append("T %d %d %d %d" % (t["t"], t["hold"], 1 if has_cmd else 0, len(t["writes"])))
        out.extend("%d %d" % w for w in t["writes"])
    out.append("E %d" % horizon)
    return "\n".join(out) + "\n"


def transplant(host, donor):
    """A stimulus for a driver image whose own game stalls early (or does not exercise a feature): the host's own start-up transactions, then the donor's command and cell traffic with its mailbox
    stores moved to the host's mailbox address. The donor's sample parameters, sample table and banks are not copied (they address the donor's ROM); the host's own voice bank and sample table stay.
    Both implementations see the same stimulus, so the comparison is as strict as for a real run; it only is not a recording of the host game."""
    hb, db = MAILBOX[host["revision"]], MAILBOX[donor["revision"]]
    skip = set(range(0x10, 0x17)) | set(range(0x18, 0x38)) | {0x42, 0x43, 0x44, 0x45}
    out = dict(host)
    out["transactions"] = list(host["transactions"])
    t0 = (host["transactions"][-1]["t"] + host["transactions"][-1]["hold"] if host["transactions"] else host["activation"]) + 1_000_000
    shift = t0 - donor["transactions"][0]["t"]
    for tx in donor["transactions"]:
        writes = []
        for off, value in tx["writes"]:
            rel = off - db
            if 0 <= rel < 0x48 and rel not in skip:
                writes.append((hb + rel, value))
        if writes:
            out["transactions"].append({"t": tx["t"] + shift, "hold": tx["hold"], "cmd": 1 if any(o == hb and v != 0 for o, v in writes) else 0, "writes": writes})
    out["horizon"] = donor["horizon"] + shift
    return out


def format_script(script):
    out = ["A %d %d %d" % (script["revision"], script["bank"], script["activation"])]
    out += ["I %d %d" % (off, script["image"][off]) for off in sorted(script["image"])]
    for t in script["transactions"]:
        out.append("T %d %d %d %d" % (t["t"], t["hold"], t["cmd"], len(t["writes"])))
        out.extend("%d %d" % w for w in t["writes"])
    out.append("E %d" % script["horizon"])
    return "\n".join(out) + "\n"


def parse_script(text):
    script = {"image": {}, "transactions": [], "revision": 0, "bank": 0, "activation": 0, "horizon": 0}
    rows = text.splitlines()
    i = 0
    while i < len(rows):
        f = rows[i].split()
        i += 1
        if not f:
            continue
        if f[0] == "A":
            script["revision"], script["bank"], script["activation"] = int(f[1]), int(f[2]), int(f[3])
        elif f[0] == "I":
            script["image"][int(f[1])] = int(f[2])
        elif f[0] == "E":
            script["horizon"] = int(f[1])
        elif f[0] == "T":
            n = int(f[4])
            writes = [tuple(int(x) for x in rows[i + j].split()) for j in range(n)]
            i += n
            script["transactions"].append({"t": int(f[1]), "hold": int(f[2]), "cmd": int(f[3]), "writes": writes})
    return script


# ---- the oracle: the original driver, executed by an independent emulator ---------------------------------------------------------------------------------------
def run_oracle(script, rom):
    """Returns (fm_writes, dac_writes, bank_bits, committed) of the original driver driven by the script. `committed` are the commands the 68000 stored: [(ticks, 13 bytes)]. Needs the `z80` package."""
    try:
        import z80
    except ImportError as error:  # pragma: no cover - environment
        raise CheckError("the independent oracle needs the third-party `z80` package (pip install z80); it is not vendored: %s" % error)
    revision, base = script["revision"], MAILBOX[script["revision"]]
    ram = bytearray(0x2000)
    for off, value in script["image"].items():
        ram[off] = value
    machine = z80.Z80Machine()
    machine.set_memory_block(0, bytes(ram))
    machine.pc = 0
    machine.mark_addrs(0x4000, 4, machine.READ_MARK | machine.WRITE_MARK)
    machine.mark_addrs(0x6000, 1, machine.WRITE_MARK)
    machine.mark_addrs(0x8000, 0x8000, machine.READ_MARK)
    FRAME = 100000  # the emulator ends a "frame" every 100000 cycles: run() returns there and frame_tick wraps
    start = script["activation"]
    state = {"bank": script["bank"], "ym": [], "bankbits": [], "frames": 0, "prev": 0, "hold_total": 0, "pending": False}

    def cycles_now():
        ft = machine.frame_tick
        if ft < state["prev"]:
            state["frames"] += 1
        state["prev"] = ft
        return state["frames"] * FRAME + ft

    def master_now():
        return start + cycles_now() * Z80_TICK + state["hold_total"]

    def on_read(address):
        if address >= 0x8000:
            index = (state["bank"] << 15) | (address & 0x7FFF)
            return rom[index] if index < len(rom) else 0xFF
        return 0x00  # the YM status: never busy

    def on_write(address, value):
        if 0x4000 <= address < 0x4004:
            state["ym"].append((address - 0x4000, value, master_now()))
        elif address == 0x6000:
            state["bank"] = ((value & 1) << 8) | (state["bank"] >> 1)
            state["bankbits"].append(value & 1)

    machine.set_read_callback(on_read)
    machine.set_write_callback(on_write)

    def advance(to_master):
        target = (to_master - start - state["hold_total"]) // Z80_TICK
        while True:
            remaining = target - cycles_now()
            if remaining <= 0:
                return
            if state["pending"]:  # the emulator keeps the last event raised until it steps past it (one instruction)
                machine.step_over_breakpoint()
                state["pending"] = False
                cycles_now()
                continue
            machine.ticks_to_stop = remaining
            state["pending"] = machine.run() != 0
            cycles_now()

    committed = []
    cells = bytearray(CELLS_LEN)
    now = start
    for tx in script["transactions"]:
        t = max(tx["t"], now)
        advance(t)
        if tx["cmd"]:
            while machine.memory[base] != 0:   # the 68000 polls with short holds until the Z80 has taken the previous command
                state["hold_total"] += POLL_HOLD
                t += POLL_HOLD + POLL_GAP
                advance(t)
        for off, value in tx["writes"]:
            machine.memory[off] = value
            if CELLS_AT <= off - base < CELLS_AT + CELLS_LEN:
                cells[off - base - CELLS_AT] = value
            elif off == base and value != 0:
                committed.append((t, bytes([value]) + bytes(cells)))
        state["hold_total"] += tx["hold"]
        now = t + tx["hold"]
    if script["horizon"] > now:
        advance(script["horizon"])
    fm, dac = split_port_stream(state["ym"], revision)
    return fm, dac, state["bankbits"], committed


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--revision", type=int, required=True, choices=sorted(MAILBOX))
    ap.add_argument("--game-log", help="a logged run: writes --script from it and stops")
    ap.add_argument("--transplant-from", help="a donor script: --script (the host's) is rewritten as the host's start-up followed by the donor's command traffic, and the tool stops")
    ap.add_argument("--script", required=True, help="the 68000 stimulus (see make_script)")
    ap.add_argument("--native-log", help="the log the replacement produced when driven by the script (tools/krisalis_replay_harness)")
    ap.add_argument("--tail-frames", type=float, default=2.0, help="writes this close to the horizon may be missing from the side that lags (default 2 frames)")
    ap.add_argument("--dac-tolerance", type=int, default=64)
    ap.add_argument("--json")
    args = ap.parse_args(argv)
    if args.game_log:
        with open(args.game_log, "r") as handle:
            text = make_script(parse_log(handle.read()), args.revision)
        with open(args.script, "w") as handle:
            handle.write(text)
        return 0
    if args.transplant_from:
        with open(args.script, "r") as handle:
            host = parse_script(handle.read())
        with open(args.transplant_from, "r") as handle:
            donor = parse_script(handle.read())
        with open(args.script, "w") as handle:
            handle.write(format_script(transplant(host, donor)))
        return 0
    if not args.native_log:
        ap.error("--native-log is required unless --game-log is given")
    with open(args.script, "r") as handle:
        script = parse_script(handle.read())
    with open(args.native_log, "r") as handle:
        lines = parse_log(handle.read())
    with open(args.rom, "rb") as handle:
        rom = handle.read()
    fm, dac, bank, _committed = run_oracle(script, rom)
    result = check(lines, fm, dac, bank, args.revision, int(args.tail_frames * 896040), args.dac_tolerance)
    print(json.dumps(result, indent=1))
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(result, handle, indent=1)
    return 0 if result["identical"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
