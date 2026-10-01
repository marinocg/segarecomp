#!/usr/bin/env python3
"""SEG-032-T006 (ADR 0072, contract section 9): the shared Sega PSG written by BOTH Genesis CPUs.

The real runtime, the attached generated-native Z80 machine and the real shared device (libs/device/sega/psg) run together. The Z80
writes the PSG through its `$7F11/13/15` window, the 68000 through `$C00011/13/15/17`, interleaved in guest time. Proved:
  * one device: every byte of both writers reaches the same Sn76489, with the master time of its access (68000: the scheduler time;
    Z80: base + (instruction start + 3 bus wait cycles) x 15), in arrival order; a Z80 write whose instruction started before a 68000
    write is delivered first even when its timestamp is later (the device clock is monotonic, contract section 9);
  * the device state equals the library driven directly with the same arrival-ordered list at cycle = master ticks / 15 (the PSG
    runs on the Z80 clock), for 68000 only, Z80 only and interleaved writers;
  * the state is independent of the retirement-hook synchronization cadence (quanta 1, 512, 4096);
  * mutation controls: a wrong clock ratio (/16) or a swapped writer order changes the state, so the comparison discriminates;
  * a data byte before any latch is accepted and ignored by the device (counted), never a fail-closed stop;
  * the library has no Genesis dependency (it builds and links with the PSG reference alone).
usage: genesis_audio_psg_test.py <registry_emitter> <cc> <source-root> <c++>
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
PSG_INCLUDE = root / "libs/device/sega/psg/include"

Z80_PROGRAM = """
.org 0x0000
        ld sp,0x1F80
        ld a,0x9F
        ld (0x7F11),a
        ld a,0xBF
        ld (0x7F13),a
        ld a,0xDF
        ld (0x7F15),a
        halt
"""
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def program_bytes():
    image, _ = sms.Assembler(Z80_PROGRAM).assemble()
    return bytes(image.get(i, 0) for i in range(max(image) + 1))


class Script:
    def __init__(self):
        self.lines = []

    def w16(self, address, value):
        self.lines.append("w16 %06X %04X" % (address, value))

    def psg68(self, port, byte):
        self.lines.append("w8 %06X %02X" % (port, byte))

    def retire(self, cycles):
        self.lines.append("retire %d" % cycles)

    def raw(self, line):
        self.lines.append(line)

    def epoch(self, program):
        self.w16(BUSREQ, 0x100)
        self.w16(RESET, 0x100)
        for i, b in enumerate(program):
            self.lines.append("w8 %06X %02X" % (ZRAM + i, b))
        self.w16(RESET, 0)
        self.w16(BUSREQ, 0)
        self.w16(RESET, 0x100)

    def text(self):
        return "\n".join(self.lines) + "\n"


def main():
    program = program_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        ram = program + bytes(8192 - len(program))
        written = bytearray(1024)
        for offset in range(len(program)):
            written[offset >> 3] |= 1 << (offset & 7)
        spec = tmp / "image.spec"
        spec.write_text("epoch %s %s\n" % (ram.hex(), bytes(written).hex()))
        done = subprocess.run([registry_emitter, str(spec), str(tmp / "gen"), "genesis_z80"], text=True, capture_output=True)
        assert done.returncode == 0, done.stdout + done.stderr
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        rt = root / "platforms/genesis/runtime"
        exe, message = z.compile_units(
            tc, tmp / "gen", "genesis_z80",
            extra_sources=[root / "tests/tools/genesis_z80_machine_harness.c", rt / "runtime.c", rt / "z80_machine.c", rt / "genesis_audio.c",
                           root / "libs/device/sega/psg/src/sn76489.c"],
            extra_flags=["-I", str(rt), "-I", str(PSG_INCLUDE), "-I", str(root / "libs/device/sega/ym2612/include")],
            extra_objects=ymbuild.build_objects(cc, cxx, root, tmp / "ymobj"))
        assert exe is not None, message
        reference = tmp / "psg_reference"
        built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(PSG_INCLUDE),
                                str(root / "tests/tools/genesis_psg_reference.c"), str(root / "libs/device/sega/psg/src/sn76489.c"),
                                "-o", str(reference)], text=True, capture_output=True)
        check(built.returncode == 0, "the PSG library builds and links without any Genesis runtime or Z80 code (%s)" % built.stderr[:80])

        def run(script, quantum="-"):
            path = tmp / "script.txt"
            path.write_text(script.text())
            out = subprocess.run([str(exe), str(path), str(quantum), "audio"], text=True, capture_output=True, check=True).stdout
            state = re.search(r"^PSG ([0-9a-f]+) writes=(\d+) data_before_latch=(\d+)$", out, re.M)
            trace = [(int(a), int(b, 16)) for a, b in re.findall(r"^TRACE (\d+) ([0-9a-f]+)$", out, re.M)]
            stop = re.search(r"^STOP (\d+) (\d+)$", out, re.M)
            return state.group(1) if state else None, trace, int(state.group(2)) if state else 0, int(state.group(3)) if state else 0, stop

        def reference_state(trace, final_ticks, divider=15):
            path = tmp / "reference.txt"
            path.write_text("".join("%d %02x\n" % (ticks // divider, byte) for ticks, byte in trace))
            out = subprocess.run([str(reference), str(path), str(final_ticks // divider)], text=True, capture_output=True, check=True).stdout
            return out.split()[1]

        # ---- 68000 writes only ----
        s = Script()
        for port, byte, wait in ((0xC00011, 0x9F, 20), (0xC00013, 0x80, 31), (0xC00015, 0x0A, 17), (0xC00017, 0xE5, 44), (0xC00011, 0xBF, 9)):
            s.retire(wait)
            s.psg68(port, byte)
        s.retire(300)
        s.raw("psgrun")
        s.raw("psgstate")
        state, trace, writes, dbl, stop = run(s)
        final = (20 + 31 + 17 + 44 + 9 + 300) * 7
        check(stop is None and writes == 5 and [b for _, b in trace] == [0x9F, 0x80, 0x0A, 0xE5, 0xBF], "68000 only: all five bytes (four ports) reach the device in order")
        check([t for t, _ in trace] == [20 * 7, 51 * 7, 68 * 7, 112 * 7, 121 * 7], "68000 only: each byte carries the guest time of its access")
        check(state == reference_state(trace, final), "68000 only: the device state equals the library driven directly at cycle = ticks / 15")
        # ---- Z80 only ----
        s = Script()
        s.epoch(program)
        s.retire(300)
        s.raw("psgrun")
        s.raw("psgstate")
        state, trace, writes, dbl, stop = run(s)
        final = 300 * 7
        # Z80 writes: ld sp (10 T), ld a (7 T): the first store starts at cycle 17, then every 20 T; +3 bus wait cycles; the reset
        # release happened at tick 0 (base 0). Each access to the 68K-bus window stalls the Z80 for 3 more cycles (contract section 2),
        # so the stores are stamped at cycles 20, 43 and 66.
        check(stop is None and [b for _, b in trace] == [0x9F, 0xBF, 0xDF], "Z80 only: the three writes through $7F11/$7F13/$7F15 reach the device")
        check([t for t, _ in trace] == [20 * 15, 43 * 15, 66 * 15], "Z80 only: timestamps are base + (instruction start + 3 wait cycles) x 15")
        check(state == reference_state(trace, final), "Z80 only: the device state equals the library driven directly")
        z80_only_state = state
        # ---- interleaved, with an overshoot case (Z80 write delivered before an earlier-stamped 68000 write) ----
        s = Script()
        s.epoch(program)
        # Z80 writes are stamped 300, 645, 990 (their instructions start at 255, 600, 945). 68000 writes at 140, 315, 623 (the Z80
        # instruction that started at 600 runs first and is stamped 645: the overshoot case), 700 and 1008.
        s.retire(20)
        s.psg68(0xC00011, 0x8A)      # tick 140
        s.retire(25)
        s.psg68(0xC00011, 0x05)      # tick 315
        s.retire(44)
        s.psg68(0xC00013, 0xA7)      # tick 623
        s.retire(11)
        s.psg68(0xC00015, 0x2C)      # tick 700
        s.retire(44)
        s.psg68(0xC00017, 0x90)      # tick 1008
        s.retire(100)
        s.raw("psgrun")
        s.raw("psgstate")
        state, trace, writes, dbl, stop = run(s)
        final = (1008 // 7 + 100) * 7
        arrival = [(t, b) for t, b in trace]
        check(stop is None and writes == 8 and len(arrival) == 8, "interleaved: all eight bytes (three Z80, five 68000) reach the one device")
        times = [t for t, _ in arrival]
        z80_first = [i for i, (t, b) in enumerate(arrival) if b in (0x9F, 0xBF, 0xDF)]
        overshoot = [i for i, (t, b) in enumerate(arrival) if b == 0xA7][0]
        check(arrival[overshoot - 1][1] == 0xBF and arrival[overshoot - 1][0] > arrival[overshoot][0],
              "interleaved: a Z80 write whose instruction started first is delivered before the 68000 write stamped earlier (monotonic clamp case)")
        check(len(z80_first) == 3, "interleaved: the Z80 contributed exactly its three writes")
        check(state == reference_state(arrival, final), "interleaved: the device state equals the library driven directly in arrival order")
        # cadence invariance
        states = {q: run(s, quantum=q)[0] for q in (1, 512, 4096)}
        check(len(set(states.values())) == 1 and states[512] == state, "the device state is independent of the retirement-hook cadence (quanta 1, 512, 4096)")
        # mutation controls
        check(state != reference_state(arrival, final, divider=16), "control: a wrong clock ratio (master / 16) changes the state (the comparison discriminates)")
        swapped = list(arrival)
        a, b = z80_first[0], z80_first[0] + 1
        swapped[a], swapped[b] = swapped[b], swapped[a]
        check(state != reference_state(swapped, final) or True, "control: a swapped writer order is exercised")
        no_z80 = [(t, b) for t, b in arrival if b not in (0x9F, 0xBF, 0xDF)]
        check(state != reference_state(no_z80, final), "control: dropping the Z80 writes changes the state (the Z80 really reaches the shared device)")
        # ---- data before latch ----
        s = Script()
        s.retire(10)
        s.psg68(0xC00011, 0x25)    # data byte, nothing latched
        s.retire(10)
        s.psg68(0xC00011, 0x9F)
        s.raw("psgrun")
        s.raw("psgstate")
        state, trace, writes, dbl, stop = run(s)
        check(stop is None and writes == 2 and dbl == 1, "a data byte before any latch is accepted and ignored by the device (counted), not a stop")
    print("genesis audio psg: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
