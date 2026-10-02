#!/usr/bin/env python3
"""SEG-032-T005 (ADR 0072, contract sections 4, 5, 8, 10): BUSREQ/RESET and deterministic M68K/Z80 scheduling.

Synthetic Z80 programs X, Y (two marker values) and Z (no EI) are materialized as RAM-backed images (the T003 registry) and run
by the attached Genesis Z80 machine on the REAL runtime, driven by a 68K bus script through genesis_route_access and the real
retirement hook. Proved:
  * power-on: the Z80 is held in reset; BUSREQ with /RESET low is never acknowledged and the 68K cannot reach the Z80 area (typed);
  * the Z80 runs only while /RESET is released and BUSREQ is clear; BUSREQ stops it (the counter freezes while the 68K clock runs),
    the grant is visible at once, release resumes it from its exact state;
  * /RESET assert stops it, release restarts it from PC 0 (a restart epoch with an empty hold window re-binds the previous image);
  * the runnable transition after an upload activates the image by its activation signature: the same Z80 PC runs different code
    under two image identities (markers $11 / $22), with dirty carry-over data in between, and an unknown signature is the typed
    z80_unknown_image;
  * RAM data mutation with valid code continues; an executable-byte mutation (z80_code_mismatch) permanently isolates the sound CPU
    (contract section 18): nothing re-decodes the bytes, no further Z80 instruction executes or writes a device, the M68K, the BUSREQ/RESET
    registers, M68K-originated YM2612/PSG writes and Z80 RAM accesses keep their documented behaviour, a reset or re-upload (even of an
    unregistered image) does not clear the fault, and the isolation is sync-cadence invariant;
  * the VBlank INT line reaches the Z80 only while IFF1 is set (program Z never takes it);
  * determinism and sync-cadence invariance: the final Z80 state digest, counters and cycles are identical at retirement-hook
    quanta 1, 512 and 4096 and across repeated runs;
  * nothing decodes opcodes at run time: the linked executable contains no Z80 decoder symbol.
usage: genesis_z80_machine_test.py <registry_emitter> <cc> <source-root> <c++>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

registry_emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
cxx = sys.argv[4] if len(sys.argv) > 4 else "c++"
sys.path.insert(0, str(root / "tools"))
import sms_fixture_rom as sms  # noqa: E402
import genesis_ym2612_build as ymbuild  # noqa: E402
import z80_conformance as z  # noqa: E402

BUSREQ, RESET, ZRAM = 0xA11100, 0xA11200, 0xA00000
STOP_CLASS_Z80, STOP_CLASS_DEVICE = 12, 2
DIAG_UNKNOWN, DIAG_MISMATCH, DIAG_NO_BUS = 56, 57, 55

TEMPLATE = """
.org 0x0000
        im 1
        ld sp,0x1F80
%s
        ld a,%d
        ld (0x1002),a
        ld hl,0x1000
loop:   inc (hl)
        jr loop
.org 0x0038
        push af
        ld a,(0x1001)
        inc a
        ld (0x1001),a
        pop af
        ei
        reti
"""


def assemble(marker, ei=True):
    image, labels = sms.Assembler(TEMPLATE % ("        ei" if ei else "        nop", marker)).assemble()
    length = max(image) + 1
    return bytes(image.get(i, 0) for i in range(length)), labels["loop"]


FAULT_TEMPLATE = """
.org 0x0000
        im 1
        ld sp,0x1F80
        ld hl,0x1000
loop:   inc (hl)
        ld a,0x9F
        ld (0x7F11),a
        jr loop
"""


def assemble_fault():
    image, labels = sms.Assembler(FAULT_TEMPLATE).assemble()
    length = max(image) + 1
    return bytes(image.get(i, 0) for i in range(length)), labels["loop"]


PROGRAMS = {"X": assemble(0x11)[0], "Y": assemble(0x22)[0], "Z": assemble(0x33, ei=False)[0], "F": assemble_fault()[0]}
FAULT_LOOP_OFFSET = assemble_fault()[1]  # the structural-mutation target (`inc (hl)`)
LOOP_OFFSET = assemble(0x11)[1]  # the `inc (hl)` of the counting loop


def registry_spec(names):
    out = ""
    for name in names:
        program = PROGRAMS[name]
        ram = program + bytes(8192 - len(program))
        written = bytearray(1024)
        for offset in range(len(program)):
            written[offset >> 3] |= 1 << (offset & 7)
        out += "epoch %s %s\n" % (ram.hex(), bytes(written).hex())
    return out


class Script:
    def __init__(self):
        self.lines = []

    def raw(self, text):
        self.lines.append(text)

    def busreq(self, on):
        self.raw("w16 %06X %04X" % (BUSREQ, 0x100 if on else 0))

    def reset(self, released):
        self.raw("w16 %06X %04X" % (RESET, 0x100 if released else 0))

    def read(self, address):
        self.raw("r8 %06X" % address)

    def read16(self, address):
        self.raw("r16 %06X" % address)

    def write(self, address, value):
        self.raw("w8 %06X %02X" % (address, value))

    def advance(self, m68k_cycles, step=10):
        for _ in range(m68k_cycles // step):
            self.raw("retire %d" % step)

    def state(self):
        self.raw("state")

    def epoch(self, name):
        """The documented upload sequence: BUSREQ, /RESET released, copy, /RESET pulse, BUSREQ cancel, /RESET release."""
        self.busreq(True)
        self.reset(True)
        for i, byte in enumerate(PROGRAMS[name]):
            self.write(ZRAM + i, byte)
        self.reset(False)
        self.busreq(False)
        self.reset(True)

    def text(self):
        return "\n".join(self.lines) + "\n"


failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def parse(out):
    reads = [(int(m.group(1), 16), int(m.group(2), 16)) for m in re.finditer(r"^R ([0-9a-f]+) ([0-9a-f]+)$", out, re.M)]
    states = [dict(kv.split("=") for kv in m.group(1).split()) for m in re.finditer(r"^STATE (.*)$", out, re.M)]
    stop = re.search(r"^STOP (\d+) (\d+)$", out, re.M)
    return reads, states, (int(stop.group(1)), int(stop.group(2))) if stop else None


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        harness = root / "tests" / "tools" / "genesis_z80_machine_harness.c"
        exes = {}
        for tag, names in (("full", ["X", "Y", "Z", "F"]), ("only_y", ["Y"]), ("only_f", ["F"])):
            spec = tmp / (tag + ".spec")
            spec.write_text(registry_spec(names))
            done = subprocess.run([registry_emitter, str(spec), str(tmp / tag), "genesis_z80"], text=True, capture_output=True)
            assert done.returncode == 0, done.stdout + done.stderr
            exe, message = tc and z.compile_units(
                tc, tmp / tag, "genesis_z80",
                extra_sources=[harness, root / "platforms/genesis/runtime/runtime.c", root / "platforms/genesis/runtime/z80_machine.c",
                               root / "platforms/genesis/runtime/genesis_audio.c", root / "platforms/genesis/runtime/genesis_mixer.c", root / "libs/device/sega/psg/src/sn76489.c"],
                extra_flags=["-I", str(root / "platforms/genesis/runtime"), "-I", str(root / "libs/device/sega/psg/include"), "-I", str(root / "libs/device/sega/ym2612/include")],
                extra_objects=ymbuild.build_objects(cc, cxx, root, tmp / "ymobj"))
            assert exe is not None, message
            exes[tag] = exe

        def run(script, tag="full", quantum=None, audio=False):
            path = tmp / "script.txt"
            path.write_text(script.text())
            cmd = [str(exes[tag]), str(path), str(quantum) if quantum else "-"] + (["audio"] if audio else [])
            out = subprocess.run(cmd, text=True, capture_output=True, check=True).stdout
            return parse(out) + (out,)

        # ---- power-on: held in reset, never acknowledged ----
        s = Script()
        s.read16(BUSREQ)  # D8 set: not granted
        s.busreq(True)
        s.read16(BUSREQ)  # still not granted: /RESET is low
        s.read(ZRAM)    # typed stop: no 68K access without the grant
        reads, states, stop, _ = run(s)
        check(reads[0] == (BUSREQ, 0x100) and reads[1] == (BUSREQ, 0x100), "power-on: /RESET is asserted, BUSREQ is never acknowledged while it is low")
        check(stop == (STOP_CLASS_DEVICE, DIAG_NO_BUS), "power-on: the 68K cannot reach the Z80 area without the grant (typed)")

        # ---- run, hold, resume ----
        s = Script()
        s.epoch("X")
        s.advance(300)           # 2100 master ticks: the Z80 runs
        s.busreq(True)
        s.read16(BUSREQ)         # granted
        s.read(ZRAM + 0x1000)    # counter C1
        s.advance(400)           # the 68K clock runs, the Z80 is held
        s.read(ZRAM + 0x1000)    # counter C2 == C1
        s.state()
        s.busreq(False)
        s.advance(600)
        s.busreq(True)
        s.read(ZRAM + 0x1000)    # counter C3 > C2
        s.state()
        reads, states, stop, _ = run(s)
        check(stop is None, "scenario A runs without a stop")
        c1, c2, c3 = reads[1][1], reads[2][1], reads[3][1]
        check(reads[0] == (BUSREQ, 0) and c1 != 0, "BUSREQ with /RESET released is acknowledged at once and the Z80 had run")
        check(c1 == c2, "while the bus is held the counter does not advance although the 68K clock runs")
        check(c3 != c2, "BUSREQ release resumes the Z80 from its exact state")
        # ---- reset while executing, restart from PC 0 ----
        s = Script()
        s.epoch("X")
        s.advance(300)
        s.reset(False)           # assert: the Z80 stops
        s.state()
        s.advance(200)
        s.state()                # identical: stopped
        s.busreq(True)
        s.read16(BUSREQ)         # not acknowledged while /RESET is low
        s.reset(True)            # released with BUSREQ held: reset, not running, grant returns
        s.read16(BUSREQ)
        s.state()
        s.busreq(False)          # empty hold window: a restart epoch re-binds image X and runs from PC 0
        s.advance(100)
        s.state()
        reads, states, stop, _ = run(s)
        check(stop is None, "scenario B runs without a stop")
        check(states[0]["z80"] == states[1]["z80"] and states[0]["cycles"] == states[1]["cycles"], "/RESET assert stops the Z80 (its state no longer changes)")
        check(reads[0] == (BUSREQ, 0x100) and reads[1] == (BUSREQ, 0), "BUSREQ is not acknowledged while /RESET is low; releasing /RESET with BUSREQ held grants")
        check(states[2]["cycles"] == "0" and states[2]["pc"] == "0000", "/RESET release resets the Z80 (PC 0, cycle counter 0)")
        check(int(states[3]["cycles"]) > 0 and states[3]["bound"] == "1", "a restart epoch (empty hold window) re-binds the previous image and runs from PC 0")
        # ---- two images under one PC, dirty carry-over, markers ----
        s = Script()
        s.epoch("X")
        s.advance(300)
        s.busreq(True)
        s.read(ZRAM + 0x1002)    # $11
        s.write(ZRAM + 0x1F00, 0xAA)   # dirty carry-over data, a separate hold
        s.busreq(False)
        s.advance(100)
        s.epoch("Y")
        s.advance(300)
        s.busreq(True)
        s.read(ZRAM + 0x1002)    # $22 under the same PC 0
        s.read(ZRAM + 0x1F00)    # the carry-over byte survived the new epoch
        s.state()
        reads, states, stop, _ = run(s)
        check(stop is None, "scenario C runs without a stop")
        check(reads[0][1] == 0x11 and reads[1][1] == 0x22, "the same Z80 PC runs different code under two image identities (markers $11 and $22)")
        check(reads[2][1] == 0xAA and states[0]["bound"] == "2", "dirty carry-over data does not change the selection; image 2 is bound")
        # ---- unknown image ----
        s = Script()
        s.epoch("X")
        s.advance(100)
        reads, states, stop, _ = run(s, tag="only_y")
        check(stop == (STOP_CLASS_Z80, DIAG_UNKNOWN), "an epoch whose signature is not in the registry is the typed z80_unknown_image")
        # ---- data mutation continues, executable mutation is a typed mismatch ----
        s = Script()
        s.epoch("X")
        s.advance(300)
        s.busreq(True)
        s.write(ZRAM + 0x1000, 0x40)   # data
        s.busreq(False)
        s.advance(300)
        s.busreq(True)
        s.read(ZRAM + 0x1000)
        reads, states, stop, _ = run(s)
        check(stop is None and reads[0][1] >= 0x40, "RAM data mutation while the code stays valid: the Z80 continues with the new data")
        # ---- structural mutation: the sound CPU is isolated, the machine continues (contract section 18) ----
        def fault_script():
            s = Script()
            s.epoch("F")
            s.advance(300)
            s.busreq(True)
            s.read(ZRAM + 0x1000)                      # R0: counter before the mutation (the Z80 ran)
            s.raw("psgstate")
            s.write(ZRAM + FAULT_LOOP_OFFSET, 0x35)    # inc (hl) -> dec (hl): a structural mutation
            s.busreq(False)
            s.advance(300)
            s.state()                                  # S0: faulted
            s.busreq(True)
            s.read16(BUSREQ)                           # R1: granted at once
            s.read(ZRAM + 0x1000)                      # R2: counter
            s.raw("psgstate")
            s.state()                                  # S1
            s.busreq(False)
            s.advance(2000)
            s.busreq(True)
            s.read(ZRAM + 0x1000)                      # R3: unchanged: nothing executes
            s.read(ZRAM + 0x1F00)                      # R4: M68K-visible Z80 RAM keeps working
            s.write(ZRAM + 0x1F00, 0x5A)
            s.read(ZRAM + 0x1F00)                      # R5: 0x5A
            s.raw("psgstate")
            s.state()                                  # S2: Z80 frozen
            s.write(0xA04000, 0x22)                    # M68K-originated YM2612 writes keep the ordinary mapping
            s.write(0xA04001, 0x00)
            s.raw("ymstate")
            s.write(0xC00011, 0x9F)                    # M68K-originated PSG write
            s.raw("psgstate")                          # PSG writes +1
            s.busreq(False)
            s.advance(500)
            s.reset(False)                             # reset and re-upload of the original image do not clear the fault
            s.read16(BUSREQ)                           # R6: not granted (BUSREQ released)
            s.epoch("F")
            s.advance(1000)
            s.busreq(True)
            s.read16(BUSREQ)                           # R7: granted
            s.read(ZRAM + 0x1000)                      # R8: still unchanged (the reset did not restart the Z80)
            s.raw("psgstate")
            s.state()                                  # S3
            s.busreq(False)
            s.epoch("Y")                               # an image absent from this registry: no unknown-image stop after the fault
            s.advance(1000)
            s.busreq(True)
            s.read(ZRAM + 0x1002)
            s.state()                                  # S4
            return s

        reads, states, stop, out = run(fault_script(), tag="only_f", audio=True)
        psg = [int(m.group(1)) for m in re.finditer(r"^PSG [0-9a-f]+ writes=(\d+)", out, re.M)]
        check(stop is None, "a structural executable-byte mutation does not stop the machine (the sound CPU is isolated)")
        check(reads[0][1] > 0 and psg[0] > 0, "before the mutation the Z80 ran and wrote the PSG")
        check(states[0]["fault"] != "0" and states[0]["view_stop"] == "0", "after the mutation the sound CPU is latched as faulted (typed diagnostic)")
        check(reads[1] == (BUSREQ, 0) and reads[7] == (BUSREQ, 0), "BUSREQ is answered by the documented ownership model: granted at once, no fabricated Z80 behaviour")
        check(reads[6] == (BUSREQ, 0x100), "with BUSREQ released the grant bit reads not-granted")
        check(reads[2][1] == reads[3][1] == reads[8][1], "no further Z80 instruction executes after the fault (counter frozen across time, reset and re-upload)")
        check(psg[1] == psg[2] and psg[4] == psg[3], "Z80-originated PSG writes cease after the fault")
        check(reads[4][1] == 0 and reads[5][1] == 0x5A, "M68K-visible Z80 RAM accesses keep their documented behaviour")
        check(psg[3] == psg[2] + 1, "an M68K-originated PSG write follows the ordinary mapped behaviour")
        check(re.search(r"^YM digest=\w+ samples=\d+ fnv=\w+ writes=2$", out, re.M) is not None, "M68K-originated YM2612 writes keep the ordinary mapped behaviour (2 writes, none from the faulted Z80)")
        check(states[1]["z80"] == states[2]["z80"] and states[1]["cycles"] == states[2]["cycles"] and states[2]["pc"] == states[1]["pc"], "the faulted Z80 state is frozen")
        check(states[3]["fault"] == states[0]["fault"] and states[3]["z80"] == states[2]["z80"], "a reset and re-upload do not clear the fault")
        check(states[4]["fault"] == states[0]["fault"] and states[4]["bound"] == "0", "an epoch with an unregistered image after the fault is not a stop and binds nothing")
        again = run(fault_script(), tag="only_f", audio=True)
        check(again[3] == out, "the isolation is deterministic across repeated runs")
        cadence = {q: run(fault_script(), tag="only_f", quantum=q, audio=True) for q in (1, 512, 4096)}
        keys = [(r[0], [st["z80"] for st in r[1][1:]]) for r in cadence.values()]  # S0 is taken before the lazy follower synchronized
        check(keys[0] == keys[1] == keys[2], "the isolation is sync-cadence invariant (reads and Z80 digests at quanta 1, 512, 4096)")
        # a healthy image is never reported as faulted; M68K YM2612 writes are unaffected by the Z80 state
        s = Script()
        s.epoch("X")
        s.advance(1000)
        s.state()
        reads, states, stop, _ = run(s)
        check(stop is None and states[0]["fault"] == "0", "a supported image is not marked faulted")
        # ---- interrupts ----
        results = {}
        for name in ("X", "Z"):
            s = Script()
            s.epoch(name)
            s.advance(109000, step=1000)   # up to just before the VBlank onset (line 224)
            s.advance(3000, step=100)      # across the onset scanline
            s.busreq(True)
            s.read(ZRAM + 0x1001)
            reads, states, stop, _ = run(s)
            results[name] = (reads[0][1] if reads else None, stop)
        check(results["X"][1] is None and results["X"][0] > 0, "the VBlank INT reaches the Z80 while IFF1 is set (the handler ran)")
        check(results["Z"][1] is None and results["Z"][0] == 0, "with interrupts never enabled the handler never runs")
        # ---- determinism and sync-cadence invariance ----
        s = Script()
        s.epoch("X")
        s.advance(1500, step=7)
        s.busreq(True)
        s.read(ZRAM + 0x1000)
        s.state()
        s.busreq(False)
        s.advance(110000, step=997)   # across a VBlank
        s.busreq(True)
        s.read(ZRAM + 0x1000)
        s.read(ZRAM + 0x1001)
        s.state()
        runs = {q: run(s, quantum=q) for q in (1, 512, 4096)}
        again = run(s, quantum=512)
        keys = [(r[0], [st["z80"] for st in r[1]], [st["cycles"] for st in r[1]]) for r in (runs[1], runs[512], runs[4096])]
        check(keys[0] == keys[1] == keys[2] and runs[512][2] is None, "sync-cadence invariance: identical reads, Z80 digests and cycle counts at quanta 1, 512 and 4096")
        check(again[3] == runs[512][3], "a repeated run is byte-identical")
        # ---- no runtime decoding ----
        symbols = subprocess.run(["nm", str(exes["full"])], text=True, capture_output=True).stdout
        check(not re.search(r"decode_at|classify_all|z80_decode|replay_bytes", symbols), "the linked executable contains no Z80 decoder symbol")
    print("genesis z80 machine: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
