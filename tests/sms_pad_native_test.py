#!/usr/bin/env python3
"""SEG-009-T006: generated-native Master System controllers, I/O control, TH-latched H counter and pause NMI.

usage: sms_pad_native_test.py <sms_image_emitter> <cc> <product-root>

Test-local fixtures (assembled with the project's fixture assembler, never committed ROMs) are emitted through the SMS
generation route, compiled with the SMS runtime and the real headless driver (which attaches the pad device) as strict
C11 and run with a finite --frames bound under scripted input (the T001 script format). Expectations are computed here
from the contract (section 11: pinout, active-low, I/O-control readback; section 7: instruction-start ordering) and the
published Z80 instruction T-states, never from the C runtime:
  * pad_ports: every frame's interrupt handler reads $DC/$DD idle-ctrl and after a varying $3F write; all values equal the
    pin-by-pin reference (exhaustive coverage is in sms_pad_tests);
  * pad_boundary (U11, controller part): a tight $DC read loop crosses scripted input boundaries; the first changed sample
    is the first whose instruction-start T-state is >= the frame start (events apply before any access at or after it);
  * pad_hlatch (U3): TH rising edges at known T offsets latch the H counter; every latched value equals the counter at the
    write's T offset for one consistent handler phase, and the phase is the expected frame-interrupt entry;
  * pad_pause: a pause held over several frames raises one NMI per press edge; a level-held model would raise one per held
    frame (mutation control); the interrupt trace shows each asserted/accepted pair at the contract T-states;
  * determinism: two runs and pseudo-random slicing give one digest.
"""
import json
import pathlib
import re
import sys
import tempfile
import hashlib

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import sms_fixture_rom as builder  # noqa: E402
from sms_vdp_native import Native  # noqa: E402

LINE, FRAME = 228, 59736
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


# ---- independent reference ------------------------------------------------------------------------------------------

def ref_ports(p1, p2, ctrl):
    """(DC, DD) from named pins. p1/p2: 6-bit pressed masks over U D L R 1 2."""
    def released(mask, bit):
        return not (mask >> bit) & 1
    a = {"up": released(p1, 0), "down": released(p1, 1), "left": released(p1, 2), "right": released(p1, 3),
         "tl": released(p1, 4), "tr": released(p1, 5), "th": True}
    b = {"up": released(p2, 0), "down": released(p2, 1), "left": released(p2, 2), "right": released(p2, 3),
         "tl": released(p2, 4), "tr": released(p2, 5), "th": True}
    for pin_dir_bit, level_bit, port, pin in ((0, 4, a, "tr"), (1, 5, a, "th"), (2, 6, b, "tr"), (3, 7, b, "th")):
        if not (ctrl >> pin_dir_bit) & 1:
            port[pin] = bool((ctrl >> level_bit) & 1)
    dc = [a["up"], a["down"], a["left"], a["right"], a["tl"], a["tr"], b["up"], b["down"]]
    dd = [b["left"], b["right"], b["tl"], b["tr"], True, True, a["th"], b["th"]]
    return (sum(1 << i for i, x in enumerate(dc) if x), sum(1 << i for i, x in enumerate(dd) if x))


def hcounter(t_in_line):
    """H counter port value: 342 pixels per line, 2 per count, sequence 0x00-0x93 then 0xE9-0xFF, origin pixel 266 at T=0."""
    sequence = list(range(0x94)) + list(range(0xE9, 0x100))
    return sequence[((3 * t_in_line) // 2 + 266) % 342 // 2]


T_TABLE = [(r"^ld [abcdehl],0x", 7), (r"^ld \(hl\),a", 7), (r"^ld a,\(0x", 13), (r"^ld \(0x[0-9A-Fa-f]+\),a", 13),
           (r"^ld hl,0x", 10), (r"^ld hl,\(", 16), (r"^ld \(0x[0-9A-Fa-f]+\),hl", 16), (r"^ld [abcdehl],[abcdehl]$", 4),
           (r"^out \(0x", 11), (r"^in a,\(0x", 11), (r"^inc hl", 6), (r"^inc a", 4), (r"^nop", 4), (r"^push ", 11),
           (r"^pop ", 10), (r"^add a,", 7), (r"^xor a", 4)]


def tstates(line):
    for pattern, t in T_TABLE:
        if re.match(pattern, line):
            return t
    raise ValueError("no T-state for " + line)


# ---- fixtures -------------------------------------------------------------------------------------------------------

def setregs_frame_irq():
    return ["ld a,0x06", "out (0xBF),a", "ld a,0x80", "out (0xBF),a", "ld a,0xA0", "out (0xBF),a", "ld a,0x81", "out (0xBF),a"]


def program(handler_body, main_init):
    lines = [".org 0x0000", "di", "im 1", "ld sp,0xDFF0", "jp main", ".org 0x0038", "jp irq", ".org 0x0066", "retn",
             ".org 0x0100", "irq:", "push af", "push hl", "in a,(0xBF)"] + handler_body + ["pop hl", "pop af", "ei", "reti",
                                                                                           ".org 0x0200", "main:"]
    lines += main_init + setregs_frame_irq() + ["in a,(0xBF)", "ei", "idle: halt", "jr idle"]
    return "\n".join("        " + l if not l.endswith(":") and not l.startswith(".") else l for l in lines) + "\n"


def ports_source():
    body = ["ld hl,(0xC2F0)", "in a,(0xDC)", "ld (hl),a", "inc hl", "in a,(0xDD)", "ld (hl),a", "inc hl",
            "ld a,(0xC2F2)", "add a,37", "ld (0xC2F2),a", "out (0x3F),a",
            "in a,(0xDC)", "ld (hl),a", "inc hl", "in a,(0xDD)", "ld (hl),a", "inc hl",
            "ld a,0xFF", "out (0x3F),a", "ld (0xC2F0),hl"]
    return program(body, ["xor a", "ld (0xC2F2),a", "ld hl,0xC300", "ld (0xC2F0),hl"])


HLATCH_SPACING = [0, 1, 5, 13, 40]


def hlatch_body():
    body = ["ld hl,0xC500"]
    for n in HLATCH_SPACING:
        body += ["ld a,0xD5", "out (0x3F),a"] + ["nop"] * n + ["ld a,0xF5", "out (0x3F),a", "in a,(0x7F)", "ld (hl),a", "inc hl"]
    return body


def hlatch_source():
    return program(hlatch_body(), [])


def boundary_source():
    return "\n".join([".org 0x0000", "        ld hl,0xC400", "loop:   in a,(0xDC)", "        ld (hl),a", "        inc hl",
                      "        ld a,h", "        cp 0xDC", "        jr nz,loop", "done:   halt", "        jr done", ""])


def pause_source():
    return "\n".join([".org 0x0000", "        im 1", "        ld sp,0xDFF0", "        ei", "main:   halt", "        jr main",
                      ".org 0x0066", "        ld hl,0xC201", "        inc (hl)", "        retn", ""])


def make_rom(source):
    image, _ = builder.Assembler(source).assemble()
    rom = builder.build_rom(0x8000, image, {})
    return rom, {"size": len(rom), "mapper": "rom_only", "sha256": hashlib.sha256(rom).hexdigest(),
                 "declaration_source": "fixture_builder"}


def mask_text(mask):
    return "".join(c if (mask >> i) & 1 else "-" for i, c in enumerate("UDLR12"))


def script(events):
    return "".join("%d %s %s %s\n" % (f, mask_text(a), mask_text(b), "P" if p else "-") for f, a, b, p in events)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        roms = tmp / "roms"
        roms.mkdir()
        sources = {"pad_ports": ports_source(), "pad_boundary": boundary_source(), "pad_hlatch": hlatch_source(),
                   "pad_pause": pause_source()}
        for name, src in sources.items():
            rom, meta = make_rom(src)
            (roms / (name + ".sms")).write_bytes(rom)
            meta["fixture"] = name
            (roms / (name + ".mapper.json")).write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n")
        native = Native(EMITTER, CC, ROOT, tmp)
        exes = {name: native.build(roms, name) for name in sources}

        def run(name, frames, events=None, *extra):
            if exes[name] is None:
                return None
            args = ["--frames", str(frames)]
            if events is not None:
                path = tmp / (name + ".input")
                path.write_text(script(events))
                args += ["--input", str(path)]
            res = native.execute(exes[name], name + "_" + "_".join(extra or ["a"]), *args, *extra)
            check(res["code"] == 0, "%s: exit %s %s" % (name, res["code"], res["stdout"][:200]))
            return res

        # --- pad_ports: a pseudo-random pad script, 40 frames
        state = 0x1234567
        events = []
        for frame in range(40):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            events.append((frame, (state >> 4) & 0x3F, (state >> 12) & 0x3F, False))
        res = run("pad_ports", 40, events)
        if res is not None:
            ram = res["ram"]
            for k in range(40):
                _, p1, p2, _ = events[k]
                ctrl = (37 * (k + 1)) & 0xFF
                expected = (*ref_ports(p1, p2, 0xFF), *ref_ports(p1, p2, ctrl))
                got = tuple(ram[0x300 + 4 * k: 0x304 + 4 * k])
                check(got == expected, "pad_ports frame %d: got %s expected %s (ctrl %02X)" % (k, got, expected, ctrl))
            again = run("pad_ports", 40, events, "--slice-seed", "7")
            check(again is not None and again["digest"] == res["digest"], "pad_ports: slicing changes the digest")
            again2 = run("pad_ports", 40, events, "--slice-cycles", "1000")
            check(again2 is not None and again2["digest"] == res["digest"], "pad_ports: fixed slicing changes the digest")

        # --- pad_boundary (U11)
        bev = [(0, 0, 0, False), (1, 0x01, 0, False), (2, 0, 0, False), (3, 0, 0x02, False)]
        res = run("pad_boundary", 5, bev)
        if res is not None:
            ram = res["ram"]
            samples = ram[0x400:0x1C00]
            first_pc_t = 10  # ld hl,nn
            per = 11 + 7 + 6 + 4 + 7 + 12  # in, ld (hl),a, inc hl, ld a,h, cp n, jr nz taken
            for frame, expect in ((1, 0xFE), (2, 0xFF), (3, 0x7F)):
                i = next((n for n in range(len(samples)) if first_pc_t + per * n >= frame * FRAME), None)
                check(i is not None and samples[i] == expect and samples[i - 1] != expect,
                      "pad_boundary frame %d: flip not at the first sample with T >= frame start (index %s)" % (frame, i))

        # --- pad_hlatch
        res = run("pad_hlatch", 2)
        if res is not None:
            body = hlatch_body()
            t = 0
            offsets = []
            for line in ["push af", "push hl", "in a,(0xBF)"] + body:
                if line.startswith("out (0x3F)") and offsets is not None:
                    pass
                t0 = t
                t += tstates(line)
                if line.startswith("out (0x3F)"):
                    offsets.append(t0)
            rising = offsets[1::2]  # every second write is the $F5 rising edge
            got = list(res["ram"][0x500:0x500 + len(rising)])
            fits = [x for x in range(LINE) if [hcounter((x + o) % LINE) for o in rising] == got]
            check(fits != [], "pad_hlatch: no handler phase reproduces the latched values %s" % got)
            # frame interrupt asserted at line 192; halted CPU on the 4-T grid (0..3 T late); IM1 response 13 T; `jp irq` 10 T; `push af` next
            check(any(23 <= x <= 26 for x in fits), "pad_hlatch: phase %s is not the frame-interrupt entry (23..26)" % fits)
            check(len(set(got)) == len(got), "pad_hlatch: spacing must give distinct values %s" % got)

        # --- pad_pause
        pev = [(0, 0, 0, False), (1, 0, 0, True), (2, 1, 0, True), (3, 0, 0, True), (4, 0, 0, False), (5, 0, 0, True),
               (6, 0, 0, False), (7, 0, 0, True), (8, 0, 0, True)]
        edges = [f for (f, _, _, p), (_, _, _, q) in zip(pev[1:], pev[:-1]) if p and not q]
        held = sum(1 for e in pev if e[3])
        check(edges == [1, 5, 7] and held == 6 and len(edges) != held, "pad_pause: reference counts")
        res = run("pad_pause", 10, pev)
        if res is not None:
            check(res["ram"][0x201] == len(edges), "pad_pause: NMI handler ran %d times, expected %d edges (level-held would be %d)"
                  % (res["ram"][0x201], len(edges), held))
            pause = [(int(c), e) for c, s, e in res["irq"] if s == "pause"]
            asserted = [c for c, e in pause if e == "asserted"]
            accepted = [c for c, e in pause if e == "accepted"]
            check(asserted == [f * FRAME for f in edges], "pad_pause: asserted at %s" % asserted)
            check(len(accepted) == len(edges) and all(a >= s and a - s < 8 for a, s in zip(accepted, asserted)),
                  "pad_pause: accepted %s" % accepted)
            for args in (("--slice-seed", "3"), ("--slice-cycles", "999")):
                other = run("pad_pause", 10, pev, *args)
                check(other is not None and other["digest"] == res["digest"], "pad_pause: slicing %s changes the digest" % (args,))
            bad = tmp / "bad.input"
            bad.write_text("3 ------ ------ -\n1 ------ ------ -\n")
            bad_run = native.run([exes["pad_pause"], "--frames", "2", "--input", bad])
            check(bad_run.returncode == 64, "pad_pause: a decreasing script frame must be rejected")

        for f in native.failures:
            check(False, f)
    for f in FAILED:
        print("FAIL:", f)
    if not FAILED:
        print("sms_pad_native_test: ok")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
