#!/usr/bin/env python3
"""SEG-131 Checkpoint B (ADR 0278): reversible Genesis machine snapshots.

The REAL Genesis runtime, the generated-native Z80 machine (a registered synthetic image that drives the YM2612 and the PSG), the
deterministic mixer and the SRAM / X24C01 owners are driven by a 68K bus script through genesis_route_access and the real retirement hook.
Everything is project-authored; no commercial data.

Proved (each against an independent whole-state hash that does not use the snapshot code):
  * capture S, run forward to T, restore S, run the same future to T: CPU/memory/VDP/Z80/device state, the mixer's SHA-256 over every
    produced sample, and the YM2612 state digest are identical (with the Z80 running, the YM2612 playing, the PSG sounding, an armed VRAM
    fill DMA, a latched H-interrupt, an EEPROM transaction in the middle of its address phase, BUSREQ asserted, and /RESET asserted);
  * restore is idempotent and capture does not modify the machine;
  * a sound-disabled build round-trips, and states do not cross the sound / no-sound boundary or the identity / region boundary;
  * every single-byte corruption sampled over the header, section table, payloads and digest, every truncation and a trailing byte are
    refused before any mutation (the whole-state hash is unchanged); values outside what a device can hold are refused even when the
    digest is valid (INVALID_STATE); a Z80 image absent from the program is refused (IMAGE_MISSING).
usage: genesis_snapshot_test.py <registry_emitter> <cc> <source-root> <c++>
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
VDP_DATA, VDP_CTRL = 0xC00000, 0xC00004
SRAM_ODD = 0x200001
OK, WRONG_CARTRIDGE, INCOMPATIBLE, INVALID_STATE, IMAGE_MISSING = 0, 10, 11, 12, 13

SECTION = {"CPU": 1, "RAM": 2, "VDP": 3, "IO": 4, "CART": 5, "Z80": 6, "PSG": 7, "YM": 8, "MIXER": 9, "AUDIO": 10}


def program(marker):
    """A synthetic Z80 sound driver: FM channel 1 keyed on, timer A running, DAC on, and a loop that streams the counter to the DAC and PSG."""
    writes = [(0x2B, 0x80), (0x30, 0x01), (0x40, 0x18), (0x50, 0x1F), (0x60, 0x08), (0x70, 0x04), (0x80, 0x27), (0xB0, 0x07), (0xB4, 0xC0),
              (0xA4, 0x22), (0xA0, 0x69), (0x24, 0x40), (0x25, 0x01), (0x27, 0x15), (0x28, 0xF0)]
    body = "".join("        ld a,0x%02X\n        ld (0x4000),a\n        ld a,0x%02X\n        ld (0x4001),a\n" % w for w in writes)
    source = """
.org 0x0000
        jp start
.org 0x0038
        push af
        ld a,(0x1001)
        inc a
        ld (0x1001),a
        pop af
        ei
        reti
.org 0x0050
start:  im 1
        ld sp,0x1F80
%s        ld a,0x2A
        ld (0x4000),a
        ld a,0x9F
        ld (0x7F11),a
        ld a,0x%02X
        ld (0x1002),a
        ld hl,0x1000
        ei
loop:   inc (hl)
        ld a,(hl)
        ld (0x4001),a
        and 0x0F
        or 0x90
        ld (0x7F11),a
        ld a,(hl)
        and 0x1F
        or 0x80
        ld (0x7F11),a
        jr loop
""" % (body, marker)
    image, _labels = sms.Assembler(source).assemble()
    return bytes(image.get(i, 0) for i in range(max(image) + 1))


PROGRAMS = {"X": program(0x11), "Y": program(0x22)}


def registry_spec(names):
    out = ""
    for name in names:
        prog = PROGRAMS[name]
        ram = prog + bytes(8192 - len(prog))
        written = bytearray(1024)
        for offset in range(len(prog)):
            written[offset >> 3] |= 1 << (offset & 7)
        out += "epoch %s %s\n" % (ram.hex(), bytes(written).hex())
    return out


class Script:
    def __init__(self):
        self.lines = []

    def raw(self, text):
        self.lines.append(text)

    def w16(self, address, value):
        self.raw("w16 %06X %04X" % (address, value))

    def w8(self, address, value):
        self.raw("w8 %06X %02X" % (address, value))

    def advance(self, m68k_cycles, step=10):
        for _ in range(m68k_cycles // step):
            self.raw("retire %d" % step)

    def vdp_setup(self):
        for word in (0x8F02, 0x8014, 0x8174, 0x8A00, 0x8C81, 0x8230, 0x8407):
            self.w16(VDP_CTRL, word)
        self.w16(VDP_CTRL, 0xC000)  # CRAM write at 0
        self.w16(VDP_CTRL, 0x0000)
        for i in range(8):
            self.w16(VDP_DATA, 0x0EEE >> (i & 3))
        self.w16(VDP_CTRL, 0x4000)  # VRAM write at 0
        self.w16(VDP_CTRL, 0x0000)
        for i in range(16):
            self.w16(VDP_DATA, 0x1111 * (i & 7))
        self.w16(VDP_CTRL, 0x4000)  # VSRAM write at 0
        self.w16(VDP_CTRL, 0x0010)
        for i in range(4):
            self.w16(VDP_DATA, 0x0010 * (i + 1))

    def epoch(self, name):
        self.w16(BUSREQ, 0x100)
        self.w16(RESET, 0x100)
        for i, byte in enumerate(PROGRAMS[name]):
            self.w8(ZRAM + i, byte)
        self.w16(RESET, 0)
        self.w16(BUSREQ, 0)
        self.w16(RESET, 0x100)

    def eeprom_bit(self, bit):
        self.w8(SRAM_ODD, bit)           # SCL low, SDA = bit
        self.w8(SRAM_ODD, bit | 2)       # SCL high
        self.w8(SRAM_ODD, bit)

    def eeprom_start(self):
        self.w8(SRAM_ODD, 3)             # SDA high, SCL high
        self.w8(SRAM_ODD, 2)             # SDA falls while SCL is high: START
        self.w8(SRAM_ODD, 0)

    def eeprom_byte(self, value):
        for shift in range(7, -1, -1):
            self.eeprom_bit((value >> shift) & 1)
        self.eeprom_bit(1)               # acknowledge clock (master releases SDA)

    def eeprom_stop(self):
        self.w8(SRAM_ODD, 0)
        self.w8(SRAM_ODD, 2)
        self.w8(SRAM_ODD, 3)             # SDA rises while SCL is high: STOP

    def text(self):
        return "\n".join(self.lines) + "\n"


failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def run(exe, script, variant="-"):
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sgsnap"))
    path = tmp / "script.txt"
    path.write_text(script.text() if isinstance(script, Script) else script)
    done = subprocess.run([str(exe), str(path), variant], text=True, capture_output=True)
    assert done.returncode == 0, done.stderr + done.stdout
    return done.stdout


def full(out):
    return re.findall(r"^FULL ([0-9a-f]{64})$", out, re.M)


def mixers(out):
    """The guest-time mixer aggregate and the SHA-256 over every produced sample. The host-only playback ring (`ring=`) is intentionally
    emptied by a restore, so it is not part of the comparison."""
    return [re.sub(r" ring=\d+", "", m) for m in re.findall(r"^MIXER (.*)$", out, re.M)]


def statuses(out, kind):
    return [int(m.group(2)) for m in re.finditer(r"^%s (\d+) (-?\d+)" % kind, out, re.M)]


def states(out):
    return [dict(kv.split("=") for kv in m.group(1).split()) for m in re.finditer(r"^STATE (.*)$", out, re.M)]


def sections(out, slot_layout_index=0):
    chunks = out.split("INSPECT")
    text = chunks[slot_layout_index + 1]
    secs = {}
    for m in re.finditer(r"^SEC (\d+) (\d+) (\d+)$", text, re.M):
        secs[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    hdr = re.search(r"^HDR (\d+) (\d+) (\d+)", text, re.M)
    return secs, int(hdr.group(1))


def scenario_prefix(s, eeprom_variant):
    s.vdp_setup()
    s.w8(0xA10009, 0x40)  # controller I/O latches
    s.w8(0xA10003, 0x40)
    s.w8(0xC00011, 0x95)  # the 68K writes the PSG port too
    s.epoch("X")
    s.advance(3000)
    if eeprom_variant:
        s.eeprom_start()
        for bit in (0, 1, 0, 1):   # four address bits clocked: the transaction is mid-address
            s.eeprom_bit(bit)
    else:
        for i in range(0, 64, 2):
            s.w8(SRAM_ODD + i, 0x40 + i)
    s.w16(VDP_CTRL, 0x9380)  # fill length (low)
    s.w16(VDP_CTRL, 0x9400)
    s.w16(VDP_CTRL, 0x9780)  # DMA mode: VRAM fill (armed, completed by the data-port write)
    s.w16(VDP_CTRL, 0x4000)
    s.w16(VDP_CTRL, 0x0080)
    s.advance(1500)


def scenario_suffix(s, eeprom_variant):
    s.advance(4000)
    s.w8(0xC00011, 0xB3)
    s.w16(VDP_DATA, 0xA5A5)           # completes the armed fill
    s.advance(2000)
    if eeprom_variant:
        for bit in (1, 1, 0):         # finish the address (7 bits) and the R/W bit = write
            s.eeprom_bit(bit)
        s.eeprom_bit(0)
        s.eeprom_bit(1)               # acknowledge
        s.eeprom_byte(0x5A)
        s.eeprom_stop()
    else:
        for i in range(64, 128, 2):
            s.w8(SRAM_ODD + i, 0x80 + i)
    s.w16(RESET, 0)                   # /RESET asserted, then released: a restart epoch
    s.advance(500)
    s.w16(RESET, 0x100)
    s.advance(6000)
    s.w16(VDP_CTRL, 0x8164)           # display on, vblank interrupt enable
    s.advance(125000)                 # crosses a vblank onset (and several scanlines of H-interrupt traffic)
    s.raw("mixer")
    s.raw("fullhash")
    s.raw("state")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        harness = root / "tests" / "tools" / "genesis_snapshot_harness.c"
        rt = root / "platforms/genesis/runtime"
        includes = ["-I", str(rt), "-I", str(root / "libs/device/sega/psg/include"), "-I", str(root / "libs/device/sega/ym2612/include")]
        ym_objects = ymbuild.build_objects(cc, cxx, root, tmp / "ymobj")
        exes = {}
        for tag, names in (("full", ["X", "Y"]), ("only_y", ["Y"]), ("none", [])):
            spec = tmp / (tag + ".spec")
            spec.write_text(registry_spec(names))
            done = subprocess.run([registry_emitter, str(spec), str(tmp / tag), "genesis_z80"], text=True, capture_output=True)
            assert done.returncode == 0, done.stdout + done.stderr
            exe, message = z.compile_units(
                tc, tmp / tag, "genesis_z80",
                extra_sources=[harness, rt / "runtime.c", rt / "z80_machine.c", rt / "genesis_audio.c", rt / "genesis_mixer.c", rt / "genesis_snapshot.c",
                               root / "libs/device/sega/psg/src/sn76489.c"],
                extra_flags=includes + ["-DSEGARECOMP_GENESIS_SOUND"], extra_objects=ym_objects)
            assert exe is not None, message
            exes[tag] = exe
        quiet = tmp / "nosound.exe"
        built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(rt), str(harness), str(rt / "runtime.c"),
                                str(rt / "genesis_snapshot.c"), "-o", str(quiet)], text=True, capture_output=True)
        assert built.returncode == 0, built.stderr
        exes["quiet"] = quiet

        # ================= restore reproduces the future (sound build; SRAM variant) =================
        for variant in ("sram", "eeprom"):
            eeprom = variant == "eeprom"
            s = Script()
            scenario_prefix(s, eeprom)
            s.raw("state")
            s.raw("fullhash")
            s.raw("snap 1")
            s.raw("fullhash")          # capture must not touch the machine
            s.raw("snap 2")
            s.raw("eq 1 2")            # and is deterministic
            scenario_suffix(s, eeprom)  # -> mixer, fullhash, state at T
            s.raw("restore 1")
            s.raw("fullhash")          # equals the hash at S
            scenario_suffix(s, eeprom)  # the same future again
            s.raw("restore 1")
            scenario_suffix(s, eeprom)  # and a third time (restore is repeatable)
            if eeprom:
                s.raw("storage 5A")
            out = run(exes["full"], s, variant)
            st = states(out)
            fh = full(out)
            # fh: [S, S-after-snap, T1, S-after-restore, T2, T3]
            check(len(fh) == 6 and fh[0] == fh[1], variant + ": capture does not modify the machine")
            check(statuses(out, "SNAP") == [0, 0] and "\nEQ\n" in out, variant + ": capture succeeds and is deterministic (identical bytes)")
            check(statuses(out, "RESTORE") == [0, 0], variant + ": restore reports OK")
            check(len(fh) == 6 and fh[3] == fh[0], variant + ": the restored machine is bit-identical (independent hash) to the machine at S")
            check(len(fh) == 6 and fh[2] == fh[4] == fh[5] and fh[2] != fh[0], variant + ": the future after restore equals the straight future (CPU, memory, VDP, Z80, devices)")
            mx = mixers(out)
            check(len(mx) == 3 and mx[0] == mx[1] == mx[2] and "nonsilent=1" in mx[0], variant + ": the mixed audio stream (SHA-256 over every sample) is identical and non-silent")
            # the scenario really exercised what it claims
            check(st[0]["dma"].startswith("1/") or st[0]["dma"].startswith("1") and st[0]["hint_pending"] == "1" and int(st[0]["z80_cycles"]) > 0 and st[0]["reset_released"] == "1",
                  variant + ": at S: armed fill DMA in progress, H-interrupt latched, Z80 running (%s)" % st[0])
            if eeprom:
                check(int(st[0]["eeprom_phase"]) == 2 and int(st[0]["eeprom_cycles"]) >= 4, variant + ": at S the EEPROM is mid-transaction (address phase, bits clocked)")
                check("STORAGE 5a" in out or "STORAGE 5A" in out.upper(), "eeprom: the committed cell is readable after the restored future")

        # ================= Z80 held in reset / BUSREQ asserted at the capture =================
        for label, setup in (("Z80 in /RESET (power-on)", []),
                             ("Z80 stopped by BUSREQ", ["w16 %06X 100" % BUSREQ])):
            s = Script()
            if setup:
                s.epoch("X")
                s.advance(2000)
                s.raw(setup[0])
            s.raw("snap 1")
            s.raw("fullhash")
            s.epoch("X")
            s.advance(3000)
            s.raw("mixer")
            s.raw("fullhash")
            s.raw("restore 1")
            s.raw("fullhash")
            s.epoch("X")
            s.advance(3000)
            s.raw("mixer")
            s.raw("fullhash")
            out = run(exes["full"], s, "sram")
            fh = full(out)
            mx = mixers(out)
            check(len(fh) == 4 and fh[0] == fh[2] and fh[1] == fh[3], label + ": restore then the same upload and run reproduces the straight run")
            check(len(mx) == 2 and mx[0] == mx[1], label + ": identical audio stream")

        # ================= a program whose Z80 registry is EMPTY (no hash table exists in it) =================
        s = Script()
        s.vdp_setup()
        s.advance(3000)
        s.raw("snap 1")
        s.raw("fullhash")
        s.advance(6000)
        s.w16(VDP_DATA, 0x4321)
        s.advance(3000)
        s.raw("fullhash")
        s.raw("restore 1")
        s.raw("fullhash")
        s.advance(6000)
        s.w16(VDP_DATA, 0x4321)
        s.advance(3000)
        s.raw("fullhash")
        out = run(exes["none"], s, "sram")
        fh = full(out)
        check(statuses(out, "SNAP") == [0] and statuses(out, "RESTORE") == [0] and len(fh) == 4 and fh[0] == fh[2] and fh[1] == fh[3], "a program with an empty Z80 registry links and round-trips (no image hash table needed)")

        # ================= sound-disabled build =================
        s = Script()
        scenario_prefix(s, False)
        s.raw("snap 1")
        s.raw("fullhash")
        s.advance(5000)
        s.w16(VDP_DATA, 0x1234)
        s.advance(130000)
        s.raw("fullhash")
        s.raw("restore 1")
        s.raw("fullhash")
        s.advance(5000)
        s.w16(VDP_DATA, 0x1234)
        s.advance(130000)
        s.raw("fullhash")
        out = run(exes["quiet"], s, "sram")
        fh = full(out)
        check(len(fh) == 4 and fh[0] == fh[2] and fh[1] == fh[3] and fh[0] != fh[1], "sound-disabled build: restore reproduces the future")

        # ================= identity, region, build profile =================
        s = Script()
        scenario_prefix(s, False)
        s.raw("snap 1")
        s.raw("save 1 %s" % (tmp / "sound.sgstate"))
        s.raw("fullhash")
        s.raw("ident 3")
        s.raw("restore 1")
        s.raw("ident A5")
        s.raw("region 2")
        s.raw("restore 1")
        s.raw("region 0")
        s.raw("fullhash")
        out = run(exes["full"], s, "sram")
        fh = full(out)
        check(statuses(out, "RESTORE") == [WRONG_CARTRIDGE, INCOMPATIBLE], "a different cartridge identity is refused; a different region is refused")
        check(len(fh) == 2 and fh[0] == fh[1], "refused restores leave the machine untouched")
        # cross build: a sound state in a sound-less program, and the reverse
        s = Script()
        s.raw("fullhash")
        s.raw("load 1 %s" % (tmp / "sound.sgstate"))
        s.raw("restore 1")
        s.raw("fullhash")
        s.raw("snap 2")
        s.raw("save 2 %s" % (tmp / "quiet.sgstate"))
        out = run(exes["quiet"], s, "sram")
        fh = full(out)
        check(statuses(out, "RESTORE") == [INCOMPATIBLE] and fh[0] == fh[1], "a sound state is refused by a sound-less program, unchanged")
        s = Script()
        s.raw("fullhash")
        s.raw("load 1 %s" % (tmp / "quiet.sgstate"))
        s.raw("restore 1")
        s.raw("fullhash")
        out = run(exes["full"], s, "sram")
        fh = full(out)
        check(statuses(out, "RESTORE") == [INCOMPATIBLE] and fh[0] == fh[1], "a sound-less state is refused by a sound program, unchanged")
        # a program that lacks the Z80 image the state executes
        s = Script()
        s.raw("fullhash")
        s.raw("load 1 %s" % (tmp / "sound.sgstate"))
        s.raw("restore 1")
        s.raw("fullhash")
        out = run(exes["only_y"], s, "sram")
        fh = full(out)
        check(statuses(out, "RESTORE") == [IMAGE_MISSING] and fh[0] == fh[1], "a state needing a Z80 image this program does not contain is refused, unchanged")

        # ================= corruption =================
        s = Script()
        scenario_prefix(s, False)
        s.raw("snap 1")
        s.raw("layout 1")
        out = run(exes["full"], s, "sram")
        secs, size = sections(out)
        check(set(secs) == set(range(1, 11)) and size < 4 * 1024 * 1024, "the container carries all ten sections within the file bound (%d bytes)" % size)
        offsets = {0, 3, 7, 8, 9, 10, 12, 14, 16, 20, 24, 28, 32, 63, 64, 96, 127, 128, 136, 144, 148, 152, 156, 160, 167, 168, 170, 175, 300, size - 33, size - 32, size - 17, size - 1}
        for name, (off, length) in secs.items():
            offsets.update({off, off + length // 2, off + length - 1})
        s = Script()
        scenario_prefix(s, False)
        s.raw("snap 1")
        s.raw("fullhash")
        for offset in sorted(offsets):
            s.raw("snap 2")
            s.raw("tamper 2 %x 5A" % offset)
            s.raw("restore 2")
        for length in (0, 1, 7, 8, 9, 100, 155, 156, 160, 200, 1000, size - 33, size - 32, size - 1):
            s.raw("snap 2")
            s.raw("trunc 2 %x" % length)
            s.raw("restore 2")
        s.raw("snap 2")
        s.raw("grow 2")
        s.raw("restore 2")
        s.raw("fullhash")
        out = run(exes["full"], s, "sram")
        codes = statuses(out, "RESTORE")
        fh = full(out)
        check(len(codes) == len(offsets) + 15 and all(c != OK for c in codes), "every sampled single-byte corruption, every truncation and a trailing byte is refused (%d cases)" % len(codes))
        check(len(fh) == 2 and fh[0] == fh[1], "refused corrupt states never mutate the machine")

        # valid digest, impossible values: the device validators, not the checksum, must refuse
        impossible = {
            "VDP DMA phase": ("VDP", 61, 0x07), "VDP DMA kind": ("VDP", 62, 0x07), "CPU interrupt level": ("CPU", 74, 0x09),
            "IO bus_requested": ("IO", 0, 0x05), "CART EEPROM phase": ("CART", 11, 0x0F), "Z80 interrupt mode": ("Z80", 28, 0x07),
            "PSG attenuation": ("PSG", 15, 0x60), "YM sample/clock invariant": ("YM", 40, 0x01), "MIXER window invariant": ("MIXER", 8, 0x01),
            "YM operator envelope state (ymfm blob)": ("YM", 623, 0xF0), "YM register address (ymfm blob)": ("YM", 49, 0xFF), "MIXER sum overflow": ("MIXER", 39, 0x80),
        }
        s = Script()
        scenario_prefix(s, True)
        s.raw("snap 1")
        s.raw("fullhash")
        s.raw("layout 1")
        out = run(exes["full"], s, "eeprom")
        secs, _ = sections(out)
        s = Script()
        scenario_prefix(s, True)
        s.raw("snap 1")
        s.raw("fullhash")
        for name, (sec, rel, mask) in impossible.items():
            s.raw("snap 2")
            s.raw("tamper 2 %x %x" % (secs[SECTION[sec]][0] + rel, mask))
            s.raw("redigest 2")
            s.raw("restore 2")
        s.raw("fullhash")
        out = run(exes["full"], s, "eeprom")
        codes = statuses(out, "RESTORE")
        fh = full(out)
        for (name, _), code in zip(impossible.items(), codes):
            check(code == INVALID_STATE, "a valid-digest state with an impossible %s is refused as INVALID_STATE (got %d)" % (name, code))
        check(len(fh) == 2 and fh[0] == fh[1], "validation refusals never mutate the machine")

        # extreme clock / scheduler values with a RECOMPUTED valid digest: refused by the same staged validation (restore AND the slot list's validate), machine untouched
        hdr_ticks = 136
        adversarial = {
            "guest clock beyond 2^62 ticks": [("H", hdr_ticks + 7, 0x40), ("IO", 8225 + 7, 0x40)],
            "Z80 cycle counter at an extreme": [("Z80", 39 + 7, 0xFF)],
            "Z80 cycle base at an extreme": [("Z80", 47 + 7, 0xFF)],
            "YM sample count at an extreme": [("AUDIO", 82 + 7, 0xFF)],
            "PSG tick count at an extreme": [("PSG", 26 + 7, 0xFF)],
            "raster next_line_tick off its invariant": [("IO", 8242, 0x01)],
            "raster last_line at an extreme": [("IO", 8234 + 7, 0x7F)],
        }
        s = Script()
        scenario_prefix(s, True)
        s.raw("snap 1")
        s.raw("fullhash")
        s.raw("layout 1")
        out = run(exes["full"], s, "eeprom")
        secs, _ = sections(out)
        s = Script()
        scenario_prefix(s, True)
        s.raw("snap 1")
        s.raw("fullhash")
        s.raw("validate 1")
        for name, edits in adversarial.items():
            s.raw("snap 2")
            for sec, rel, mask in edits:
                base = 0 if sec == "H" else secs[SECTION[sec]][0]
                s.raw("tamper 2 %x %x" % (base + rel, mask))
            s.raw("redigest 2")
            s.raw("validate 2")
            s.raw("restore 2")
        s.raw("fullhash")
        out = run(exes["full"], s, "eeprom")
        codes = statuses(out, "RESTORE")
        vcodes = [int(m.group(2)) for m in re.finditer(r"^VALIDATE (\d+) (-?\d+)", out, re.M)]
        fh = full(out)
        check(vcodes[0] == OK, "an untouched state validates (the slot list shows it as usable)")
        for (name, _), code, vcode in zip(adversarial.items(), codes, vcodes[1:]):
            check(code != OK and vcode == code, "extreme %s (valid digest): restore and slot validation both refuse it (%d / %d)" % (name, code, vcode))
        check(len(fh) == 2 and fh[0] == fh[1], "extreme-value refusals never mutate the machine")

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
