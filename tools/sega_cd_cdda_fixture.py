#!/usr/bin/env python3
"""Project-authored BIOS-like image and disc for the SEG-104 CD-DA end-to-end tests (ADR 0244, 0246). Stdlib only.

Every byte is written here with the fixture encoder of tools/sega_cd_fixture_disc.py; no BIOS, game or commercial byte is read or reproduced.

The main program is the live fixture's frame loop (vertical interrupt counts frames; `--set main:d6=N` frames, then it stops) that first copies a
sub program into PRG RAM and releases the sub. The sub program drives the CD drive through the real Gate Array command registers:

  1. PLAY  the start of audio track 2 (virtual PREGAP before it)     wait 40 drive periods
  2. PAUSE                                                           wait 10
  3. RESUME                                                          wait 20     (the run continues at the position it paused at)
  4. PLAY  the last 5 sectors of track 2 and run on: track 3 has an in-file INDEX 00 pregap (non-silent filler: the drive reports "no audio
     playing" there, so none of it may be heard), then its INDEX 01 start                                                   wait 190
  5. SEEK  track 3 + 20 (the drive pauses at the target: nothing is heard)                                                  wait 10
  6. PLAY  track 3 + 20                                                                                                     wait 25
  7. STOP                                                                                                                   wait 10, then done

The audio tracks hold the closed-form samples of `sample(lba, frame, channel)` below (left and right differ everywhere; the first three frames of a
sector are the extremes -32768 / 32767, the byte-order-sensitive pair 0x0001 / 0x0100 and 0 / -1). The test derives the expected canonical stream from
the run log of the product alone as the concatenation of `sector(lba)` over each logged run, so it checks the bytes, the channel order, the sample
byte order and the run boundaries against this independent oracle.

usage: sega_cd_cdda_fixture.py build --out DIR   ->   DIR/bios.bin, DIR/disc/ (disc.cue + tracks) and DIR/expected.json (stdout: the same JSON)
"""
import argparse
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402  (the shared project-authored MC68000 encoder and disc builder)

ROM_SIZE = 0x1000
MAIN = 0x0200
HANDLER = 0x0700
FAULT = 0x0C00
SUB_SRC = 0x0D00
RESULT = 0xFFC000
FRAMES = RESULT + 0x20
DATA1 = 0xA10003
VDP_DATA = 0xC00000
VDP_CTRL = 0xC00004
GA = 0xA12000
WINDOW = 0x020000
SUB_SSP = 0x2000
SUB_ENTRY = 0x0100
GA_SUB = 0xFF8000
DEFAULT_FRAMES = 0xFFFF

T1 = F.TRACK1_SECTORS           # data track sectors
T2_N = 120                      # audio sectors of track 2 (virtual PREGAP of 150 before its INDEX 01)
T2_FIRST = T1 + 150             # LBA of track 2's INDEX 01
T3_PRE = 150                    # in-file INDEX 00 pregap sectors of track 3
T3_N = 120
T3_FIRST = T2_FIRST + T2_N + T3_PRE
SECTOR_FRAMES = 588

SCRIPT = [("play", T2_FIRST, 40), ("pause", 0, 10), ("resume", 0, 20), ("play", T2_FIRST + T2_N - 5, 190), ("seek", T3_FIRST + 20, 10),
          ("play", T3_FIRST + 20, 25), ("stop", 0, 10)]


def sample(lba, frame, channel):
    """Closed-form signed 16-bit sample of audio sector `lba`, frame `frame` (0-587), channel 0 = left, 1 = right."""
    if frame == 0:
        return 32767 if channel else -32768
    if frame == 1:
        return 0x0100 if channel else 0x0001
    if frame == 2:
        return -1 if channel else 0
    x = (lba * 2654435761 + frame * 40503 + (0x9E37 if channel else 0x1357)) & 0xFFFFFFFF
    v = ((x >> 7) ^ (x << 3) ^ (0x5A5A if channel else 0)) & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


def sector(lba):
    """The canonical S16LE interleaved bytes of one sector (left sample first, each sample least-significant byte first)."""
    return b"".join(struct.pack("<hh", sample(lba, f, 0), sample(lba, f, 1)) for f in range(SECTOR_FRAMES))


def cdd_tx(command, lba):
    tx = [command, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    if command in (3, 4):
        msf = lba + 150
        mm, ss, ff = msf // 75 // 60, msf // 75 % 60, msf % 75
        tx[2:8] = [mm // 10, mm % 10, ss // 10, ss % 10, ff // 10, ff % 10]
    tx[9] = (~sum(tx[:9])) & 0xF
    return [(tx[2 * i] << 8) | tx[2 * i + 1] for i in range(5)]


def vdp_reg(m, reg, value):
    m.move("w", F.imm(0x8000 | (reg << 8) | value), F.absl(VDP_CTRL))


# SEG-107 (ADR 0251): the PCM variant. The same CD-DA script, preceded by RF5C164 setup (a positive-only wave with $FF loop markers, two channels)
# and with the channels switched off and on again while the CD-DA runs. Positive-only samples keep the independent reference exact.
PCM_BASE = 0xFF0000
PCM_WAVE = bytes([0x80 | ((i * 3) & 0x7E) for i in range(63)] + [0xFF] + [0x80 | ((i * 5 + 9) & 0x7E) for i in range(31)] + [0xFF])
SCRIPT_PCM = [("play", T2_FIRST, 40), ("pcm_off", 0, 6), ("pause", 0, 10), ("pcm_on", 0, 8), ("resume", 0, 20), ("play", T2_FIRST + T2_N - 5, 190),
              ("seek", T3_FIRST + 20, 10), ("play", T3_FIRST + 20, 25), ("stop", 0, 10)]
PCM_CHANNELS = [  # channel, ENV, PAN, FD, LS, ST
    (0, 0xFF, 0xFF, 0x0800, 0x0000, 0x00),
    (1, 0x90, 0x3C, 0x0555, 0x0040, 0x00),
]


def pcm_reg(s, reg, value):
    s.move("b", F.imm(value), F.absl(PCM_BASE + 2 * reg + 1))


def pcm_setup(s):
    pcm_reg(s, 7, 0x80)  # chip enabled, bank 0
    for i, b in enumerate(PCM_WAVE):
        pcm_reg(s, 0x1000 + i, b)
    for ch, env, pan, fd, ls, st in PCM_CHANNELS:
        pcm_reg(s, 7, 0xC0 | ch)
        for reg, v in ((0, env), (1, pan), (2, fd & 0xFF), (3, fd >> 8), (4, ls & 0xFF), (5, ls >> 8), (6, st)):
            pcm_reg(s, reg, v)
    pcm_reg(s, 8, 0xFC)  # channels 0 and 1 ON


def sub_program(pcm=False):
    s = F.Asm(0)
    s.lw(SUB_SSP)
    s.lw(SUB_ENTRY)
    while s.pc < SUB_ENTRY:
        s.w(0)
    if pcm:
        pcm_setup(s)
    for n, (name, lba, wait) in enumerate(SCRIPT_PCM if pcm else SCRIPT):
        if name in ("pcm_off", "pcm_on"):
            pcm_reg(s, 8, 0xFF if name == "pcm_off" else 0xFC)
        else:
            command = {"play": 3, "seek": 4, "pause": 6, "resume": 7, "stop": 1}[name]
            for i, word in enumerate(cdd_tx(command, lba)):
                s.move("w", F.imm(word), F.absl(GA_SUB + 0x42 + 2 * i))  # the word write to $FF804A sends the command
        s.move("w", F.imm(wait - 1), F.d(6))                          # `wait` outer rounds of ~160,000 cycles (about one drive period)
        s.label("o%d" % n)
        s.move("w", F.imm(15999), F.d(7))
        s.label("i%d" % n)
        s.dbra(7, "i%d" % n)
        s.dbra(6, "o%d" % n)
    s.move("w", F.imm(0xC0DE), F.absl(GA_SUB + 0x20))
    s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x0F))
    s.stop(0x2700)
    return s.assemble()


def main_program(sub_len):
    m = F.Asm(MAIN)
    m.move("b", F.imm(0x40), F.absl(0xA10009))
    vdp_reg(m, 0, 0x04)
    vdp_reg(m, 1, 0x44)
    vdp_reg(m, 12, 0x00)
    vdp_reg(m, 15, 0x02)
    m.clr("w", F.absl(FRAMES))
    m.tst("w", F.d(6))
    m.bcc("ne", "have_count")
    m.move("w", F.imm(DEFAULT_FRAMES), F.d(6))
    m.label("have_count")
    m.move("l", F.imm(SUB_SRC), F.a(0))                   # the sub program into PRG RAM (bank 0) through the window, then release the sub
    m.move("l", F.imm(WINDOW), F.a(1))
    m.move("w", F.imm(sub_len // 2 - 1), F.d(0))
    m.label("copy")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy")
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))           # SRES 1, SBRQ 0
    vdp_reg(m, 1, 0x64)                                   # IE0: the vertical interrupt
    m.move_sr(0x2000)
    m.label("frames")
    m.move("w", F.absl(FRAMES), F.d(1))
    m.alu("cmp", "w", F.d(6), 1)
    m.bcc("cs", "frames")
    m.move_sr(0x2700)
    vdp_reg(m, 1, 0x44)
    m.move("b", F.imm(0xA5), F.absl(RESULT + 0xFF))
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= HANDLER, hex(MAIN + len(code))
    return code


def handler_program():
    h = F.Asm(HANDLER)
    h.quick("add", "w", 1, F.absl(FRAMES))
    h.rte()
    return h.assemble()


def build_bios(pcm=False):
    sub = sub_program(pcm)
    sub_len = (len(sub) + 1) & ~1
    rom_size = 0x2000 if pcm else ROM_SIZE
    rom = bytearray(rom_size)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, FAULT)
    struct.pack_into(">I", rom, 30 * 4, HANDLER)          # level 6
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE CDDA FIXTURE 104 "[:32]
    main = main_program(sub_len)
    rom[MAIN:MAIN + len(main)] = main
    handler = handler_program()
    rom[HANDLER:HANDLER + len(handler)] = handler
    fault = F.Asm(FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    rom[FAULT:FAULT + len(fault.assemble())] = fault.assemble()
    assert SUB_SRC + len(sub) <= rom_size
    rom[SUB_SRC:SUB_SRC + len(sub)] = sub
    return bytes(rom)


def build_disc(out):
    F.build("comm", out, "cooked", False)
    (out / "track02.bin").write_bytes(b"".join(sector(T2_FIRST + i) for i in range(T2_N)))
    pre = b"\x77" * 2352 * T3_PRE                         # non-silent INDEX 00 content: it must never be heard
    (out / "track03.bin").write_bytes(pre + b"".join(sector(T3_FIRST + i) for i in range(T3_N)))
    cue = (out / "disc.cue").read_text()
    cue += 'FILE "track02.bin" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n'
    cue += 'FILE "track03.bin" BINARY\n  TRACK 03 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'
    (out / "disc.cue").write_text(cue, newline="\n")


def expected():
    return {"track1_sectors": T1, "t2_first": T2_FIRST, "t2_sectors": T2_N, "t3_first": T3_FIRST, "t3_sectors": T3_N, "t3_pregap": T3_PRE,
            "script": SCRIPT, "sector_frames": SECTOR_FRAMES}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--pcm", action="store_true", help="SEG-107: the variant that also drives the RF5C164 PCM chip")
    args = parser.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "bios.bin").write_bytes(build_bios(args.pcm))
    build_disc(args.out / "disc")
    exp = expected()
    (args.out / "expected.json").write_text(json.dumps(exp, indent=1, sort_keys=True) + "\n")
    print(json.dumps(exp, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
