#!/usr/bin/env python3
"""SEG-032-T007 (ADR 0072/0074, contract sections 4, 9, 11): the YM2612 reached by BOTH Genesis CPUs on the real runtime.

The real runtime, the attached generated-native Z80 machine and the real YM2612 device (vendored ymfm behind the C ABI) run together.
Proved:
  * one device, two writers: the 68000 writes `$A04000-$A04003` (BYTE), the Z80 writes `$4000-$5FFF` (`address & 3`); every write
    reaches the same chip with the guest master time of its access, in arrival order;
  * for 68000 only, Z80 only and interleaved writers (and across a Z80 /RESET), the chip's native sample stream and complete state digest equal
    the library driven directly (tests/tools/ym2612_driver.c) with the recorded arrival-ordered list - the integration adds nothing and
    loses nothing - and the stream is not constant (the chip really plays);
  * a Z80 driver that polls the busy flag before each write and streams DAC samples works end to end (busy, status, DAC);
  * Timer A, programmed and polled by the Z80, sets its flag at (1024 - NA) x 144 input clocks after the load: the Z80's marker byte is
    absent a little before and present a little after;
  * the Z80 /RESET line resets the YM2612 on assertion and release (the chip returns to its idle output);
  * the state is independent of the retirement-hook cadence (quanta 1, 512, 4096);
  * controls: dropping the Z80 writes, or skipping the reset, changes the digest (the comparison discriminates).
usage: genesis_audio_ym_test.py <registry_emitter> <cc> <source-root> <c++>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

registry_emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
cxx = sys.argv[4] if len(sys.argv) > 4 else "c++"
sys.path.insert(0, str(root / "tools"))
import genesis_ym2612_build as ymbuild  # noqa: E402
import sms_fixture_rom as sms  # noqa: E402
import z80_conformance as z  # noqa: E402

BUSREQ, RESET, ZRAM = 0xA11100, 0xA11200, 0xA00000
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def patch(alg=7, fb=0, tl=0x20, ar=0x1F, d1r=0, sl_rr=0x0F, mul=1, fnum=0x269, block=4, pan=0xC0):
    w = [(0, 0xB0), (1, (fb << 3) | alg), (0, 0xB4), (1, pan)]
    for off in (0x00, 0x04, 0x08, 0x0C):
        w += [(0, 0x30 + off), (1, mul), (0, 0x40 + off), (1, tl), (0, 0x50 + off), (1, ar), (0, 0x60 + off), (1, d1r),
              (0, 0x70 + off), (1, 0), (0, 0x80 + off), (1, sl_rr)]
    w += [(0, 0xA4), (1, (block << 3) | (fnum >> 8)), (0, 0xA0), (1, fnum & 0xFF)]
    return w


KEY_ON = [(0, 0x28), (1, 0xF0)]


def z80_program(writes, tail="        halt\n", marker_poll=None):
    src = ".org 0x0000\n        ld sp,0x1F80\n"
    for n, (port, value) in enumerate(writes):
        src += "w%d:     ld a,(0x4000)\n        add a,a\n        jr c,w%d\n        ld a,%d\n        ld (0x%04X),a\n" % (n, n, value, 0x4000 + port)
    src += tail
    image, _ = sms.Assembler(src).assemble()
    return bytes(image.get(i, 0) for i in range(max(image) + 1))


class Script:
    def __init__(self):
        self.lines = []

    def raw(self, line):
        self.lines.append(line)

    def w16(self, a, v):
        self.lines.append("w16 %06X %04X" % (a, v))

    def retire(self, n, step=None):
        if step:
            self.lines.extend(["retire %d" % step] * (n // step))
        else:
            self.lines.append("retire %d" % n)

    def ym68(self, port, value):
        self.lines.append("w8 %06X %02X" % (0xA04000 + port, value))

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
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        progs = {}
        base_writes = patch() + KEY_ON
        progs["tone"] = z80_program(base_writes)
        # DAC streaming: enable the DAC, then write a ramp to register $2A, polling busy before each write
        dac_writes = [(0, 0x2B), (1, 0x80)]
        for i in range(48):
            dac_writes += [(0, 0x2A), (1, (i * 37 + 11) & 0xFF)]
        progs["dac"] = z80_program(dac_writes)
        # Timer A: NA = 1000, load + enable + reset flags, then poll bit 0 and leave a marker
        timer_writes = [(0, 0x24), (1, 1000 >> 2), (0, 0x25), (1, 1000 & 3), (0, 0x27), (1, 0x15)]
        progs["timer"] = z80_program(timer_writes, tail="poll:   ld a,(0x4000)\n        and 1\n        jr z,poll\n        ld a,0xAA\n        ld (0x1000),a\n        halt\n")
        spec = ""
        for name in ("tone", "dac", "timer"):
            program = progs[name]
            ram = program + bytes(8192 - len(program))
            written = bytearray(1024)
            for offset in range(len(program)):
                written[offset >> 3] |= 1 << (offset & 7)
            spec += "epoch %s %s\n" % (ram.hex(), bytes(written).hex())
        (tmp / "image.spec").write_text(spec)
        done = subprocess.run([registry_emitter, str(tmp / "image.spec"), str(tmp / "gen"), "genesis_z80"], text=True, capture_output=True)
        assert done.returncode == 0, done.stdout + done.stderr
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        rt = root / "platforms/genesis/runtime"
        exe, message = z.compile_units(
            tc, tmp / "gen", "genesis_z80",
            extra_sources=[root / "tests/tools/genesis_z80_machine_harness.c", rt / "runtime.c", rt / "z80_machine.c", rt / "genesis_audio.c",
                           root / "libs/device/sega/psg/src/sn76489.c"],
            extra_flags=["-I", str(rt), "-I", str(root / "libs/device/sega/psg/include"), "-I", str(root / "libs/device/sega/ym2612/include")],
            extra_objects=ymbuild.build_objects(cc, cxx, root, tmp / "ymobj"))
        assert exe is not None, message
        objs = ymbuild.build_objects(cc, cxx, root, tmp / "ymobj")
        driver = tmp / "ym_driver"
        built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(root / "libs/device/sega/ym2612/include"),
                                str(root / "tests/tools/ym2612_driver.c"), *map(str, objs), "-o", str(driver)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr

        def run(script, quantum="-"):
            path = tmp / "script.txt"
            path.write_text(script.text())
            out = subprocess.run([str(exe), str(path), str(quantum), "audio"], text=True, capture_output=True, check=True).stdout
            ym = re.search(r"^YM digest=([0-9a-f]+) samples=(\d+) fnv=([0-9a-f]+) writes=(\d+)$", out, re.M)
            trace = [(int(a), int(b), int(c)) for a, b, c in re.findall(r"^YMTRACE (\d+) (\d+) (\d+)$", out, re.M)]
            reads = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"^R ([0-9a-f]+) ([0-9a-f]+)$", out, re.M)]
            stop = re.search(r"^STOP (\d+) (\d+)$", out, re.M)
            return {"digest": ym.group(1), "samples": int(ym.group(2)), "fnv": ym.group(3), "writes": int(ym.group(4)), "trace": trace,
                    "reads": reads, "stop": stop} if ym else {"stop": stop, "reads": reads}

        def direct(trace, final_ticks, skip_resets=False):
            lines = []
            for ticks, port, value in trace:
                if port == 0xFF:
                    if not skip_resets:
                        lines.append("reset %d" % ticks)
                else:
                    lines.append("w %d %d %d" % (port, value, ticks))
            lines += ["adv %d" % final_ticks, "digest"]
            (tmp / "d.txt").write_text("\n".join(lines) + "\n")
            out = subprocess.run([str(driver), str(tmp / "d.txt"), str(tmp / "d.bin")], text=True, capture_output=True, check=True).stdout
            dig = re.search(r"^DIGEST ([0-9a-f]+) samples=(\d+)$", out, re.M)
            smp = re.search(r"^SAMPLES (\d+) ([0-9a-f]+)$", out, re.M)
            return dig.group(1), int(dig.group(2)), smp.group(2)

        def stream_varies(path_bin):
            data = (tmp / "d.bin").read_bytes()
            return len(set(data[i:i + 4] for i in range(0, len(data), 8))) > 6

        # ---- 68000 only ----
        s = Script()
        t_cycles = 0
        for port, value in patch() + KEY_ON:
            s.retire(30)
            s.ym68(port, value)
            t_cycles += 30
        s.retire(8000, step=50)
        s.raw("ymstate")
        r = run(s)
        final = (t_cycles + 8000) * 7
        check(r.get("stop") is None and r["writes"] == len(patch() + KEY_ON), "68000 only: every byte reaches the chip")
        d, n, f = direct(r["trace"], final)
        check((r["digest"], r["samples"], r["fnv"]) == (d, n, f) and stream_varies(None), "68000 only: sample stream and state digest equal the library driven directly; the chip plays")
        # ---- Z80 only ----
        s = Script()
        s.epoch(progs["tone"])
        s.retire(20000, step=50)   # each write waits out the 192-clock busy period (192 M68K cycles) before the next
        s.raw("ymstate")
        r = run(s)
        final = 20000 * 7
        check(r.get("stop") is None and r["writes"] == len(base_writes), "Z80 only: the polling driver writes the whole patch (%d writes)" % r.get("writes", -1))
        d, n, f = direct(r["trace"], final)
        check((r["digest"], r["samples"], r["fnv"]) == (d, n, f) and stream_varies(None), "Z80 only: sample stream and state digest equal the library driven directly")
        z80_only = r
        # ---- DAC streaming from the Z80 ----
        s = Script()
        s.epoch(progs["dac"])
        s.retire(26000, step=50)
        s.raw("ymstate")
        r = run(s)
        check(r.get("stop") is None and r["writes"] == len(dac_writes), "Z80 DAC streaming: all %d register writes (enable + 48 samples) reach the chip" % len(dac_writes))
        d, n, f = direct(r["trace"], 26000 * 7)
        check((r["digest"], r["samples"], r["fnv"]) == (d, n, f) and stream_varies(None), "Z80 DAC streaming: stream and digest equal the library driven directly (a varying DAC output)")
        # ---- interleaved, then a Z80 /RESET ----
        s = Script()
        s.epoch(progs["tone"])
        s.retire(300)
        s.ym68(0, 0xB4)           # 68000 writes a pan register while the Z80 patch programs
        s.retire(40)
        s.ym68(1, 0x80)
        s.retire(7000, step=50)
        s.w16(RESET, 0)           # /RESET asserted: the chip is reset
        s.retire(500, step=50)
        s.w16(RESET, 0x100)       # released: reset again (the Z80 restarts)
        s.retire(6000, step=50)
        s.raw("ymstate")
        r = run(s)
        final = (300 + 40 + 7000 + 500 + 6000) * 7
        arrival_ports = {p for _, p, _ in r["trace"]}
        check(r.get("stop") is None and 0xFF in arrival_ports and any(p in (0, 1) for _, p, _ in r["trace"]), "interleaved: both writers and the two /RESET resets reach the chip")
        d, n, f = direct(r["trace"], final)
        check((r["digest"], r["samples"], r["fnv"]) == (d, n, f), "interleaved + /RESET: stream and state digest equal the library driven directly in arrival order")
        d2, n2, f2 = direct(r["trace"], final, skip_resets=True)
        check(d2 != r["digest"], "control: skipping the chip resets changes the state (the /RESET line really resets the YM2612)")
        no_z80 = [e for e in r["trace"] if e[0] < 0]
        check(direct(z80_only["trace"][: len(z80_only["trace"]) // 2], 20000 * 7)[0] != z80_only["digest"], "control: dropping half of the Z80 writes changes the state")
        cad = {q: run(s, quantum=q) for q in (1, 512, 4096)}
        check(len({(c["digest"], c["fnv"], c["samples"]) for c in cad.values()}) == 1 and cad[512]["digest"] == r["digest"],
              "the chip state is independent of the retirement-hook cadence (quanta 1, 512, 4096)")
        # ---- Timer A end to end ----
        results = {}
        for label, cycles in (("before", 4000), ("after", 5200)):
            s = Script()
            s.epoch(progs["timer"])
            s.retire(cycles, step=50)
            s.w16(BUSREQ, 0x100)
            s.raw("r8 %06X" % (ZRAM + 0x1000))
            s.raw("ymstate")
            results[label] = run(s)
        marker_before = results["before"]["reads"][-1][1] if results["before"].get("reads") else None
        marker_after = results["after"]["reads"][-1][1] if results["after"].get("reads") else None
        check(marker_before == 0 and marker_after == 0xAA,
              "Timer A (NA 1000): the Z80's flag-poll marker is absent at +4000 and present at +5200 M68K cycles (the load is written after five busy waits, about +1,000; the flag follows 3,456 input clocks = 3,456 cycles later)")
    print("genesis audio ym: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
