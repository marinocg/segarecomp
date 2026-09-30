#!/usr/bin/env python3
"""SEG-009-T001 (ADR 0062): whole-machine adversarial smoke of the pinned Master System references.

The expectations below are authored from public documents (MacDonald, SMS Power!), not from any reference's own
test suite, and every one names its source. The same project-authored `oracle_smoke` fixture ROM
(tools/sms_fixture_rom.py) runs black-box through each finalist's libretro core, built locally from the pinned
checkout in a temporary copy and driven by tests/sms_oracle/libretro_host.c (injection: ROM program + scripted pad
and pause; observation: the RAM result block and two framebuffer summaries).

Also checked: two runs are byte-identical (determinism), and a negative control (the reference configured as a
Japanese console) must fail the nationalization expectations, proving the smoke discriminates.

Skips cleanly (exit 0) unless SEGARECOMP_SMS_ORACLE_CHECKOUT holds the pinned checkouts; a wrong or dirty pin fails.
usage: sms_oracle_smoke_test.py [cc]
"""
import glob
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE / "sms_oracle"))
sys.path.insert(0, str(REPO / "tools"))
import pins  # noqa: E402
import sms_fixture_rom  # noqa: E402

NAMES = ["drhelius_Gearsystem", "ekeeke_Genesis-Plus-GX"]
MAX_FRAMES = 600

FINALISTS = {
    "gearsystem": {
        "tree": "drhelius_Gearsystem", "make": ["-C", "platforms/libretro"], "glob": "platforms/libretro/gearsystem_libretro.*",
        "options": ["gearsystem_system=Master System / Mark III", "gearsystem_region=Master System Export",
                    "gearsystem_mapper=SEGA", "gearsystem_timing=NTSC (60 Hz)", "gearsystem_overscan=Disabled",
                    "gearsystem_hide_left_bar=No", "gearsystem_bios_sms=Disabled"],
        "japan": "gearsystem_region=Master System Japan",
    },
    "genesis_plus_gx": {
        "tree": "ekeeke_Genesis-Plus-GX", "make": ["-f", "Makefile.libretro"], "glob": "genesis_plus_gx_libretro.*",
        "options": ["genesis_plus_gx_system_hw=master system II", "genesis_plus_gx_region_detect=ntsc-u",
                    "genesis_plus_gx_bios=disabled", "genesis_plus_gx_overscan=disabled",
                    "genesis_plus_gx_left_border=disabled", "genesis_plus_gx_vdp_mode=60hz"],
        "japan": "genesis_plus_gx_region_detect=ntsc-j",
    },
}

# (offset in 0xC100.., mask, expected, source). Offsets are the result-block layout of the oracle_smoke fixture.
EXPECT = [
    (0x00, 0xFF, 0x5A, "fixture: result block reached"),
    (0x01, 0xFF, 0xFF, "MD-HW §3 SMS 2 map: reads of $00-$3F return $FF"),
    (0x02, 0xFF, 0xFF, "MD-HW §3 SMS 2 map: reads of $00-$3F return $FF ($3E)"),
    (0x03, 0xC0, 0xC0, "MD-HW §4, SP-REGION: export console reads back TH output high"),
    (0x04, 0xC0, 0x00, "MD-HW §4, SP-REGION: export console reads back TH output low"),
    (0x05, 0xFF, 0xFF, "MD-HW §4: port $DC idle, active low"),
    (0x06, 0xFF, 0xFF, "MD-HW §3: $C0 mirrors $DC"),
    (0x07, 0x3F, 0x3F, "MD-HW §4: port $DD idle; bit 4 reset = 1 (no button), bit 5 = 1"),
    (0x08, 0xFF, 0xA5, "MD-VDP §3: control read clears the first/second byte flag"),
    (0x09, 0xFF, 0x3C, "MD-VDP §3: the first control byte updates the address low byte"),
    (0x0A, 0xFF, 0x00, "MD-VDP §3: the write landed at the updated address only"),
    (0x0B, 0xFF, 0x44, "MD-VDP §3: a data write loads the read buffer"),
    (0x0C, 0xFF, 0x22, "MD-VDP §3: a data read refills the buffer from VRAM and increments"),
    (0x0D, 0xFF, 0x88, "MD-VDP §3: the address register wraps past $3FFF"),
    (0x0E, 0xFF, 0x77, "MD-VDP §3: the write at $3FFF"),
    (0x0F, 0xFF, 0x9C, "MD-VDP §2: $BD/$80 mirror the control/data ports"),
    (0x10, 0xFF, 0xC1, "MD-VDP §12: frame interrupt flag set on line $C1 (192 lines)"),
    (0x11, 0xE0, 0x00, "MD-VDP §4: the status read clears the flags"),
    (0x12, 0x80, 0x80, "MD-VDP §12: IM1 frame interrupt with the flag set"),
    (0x13, 0xFF, 0xC1, "MD-VDP §12: frame interrupt taken on line $C1"),
    (0x15, 0xFF, 12, "MD-VDP §12: R10 = 15 underflows 12 times over lines 0-192"),
    (0x16, 0xFF, 0x0F, "MD-VDP §12: first line interrupt on line 15"),
    (0x17, 0xFF, 0x1F, "MD-VDP §12: second line interrupt 16 lines later"),
    (0x18, 0xFF, 0x2A, "MD-HW §5: IM2 vector read at (I << 8) | $FF on the SMS 2"),
    (0x19, 0xFF, 0xDA, "MD-VDP §11, SP-VCNT: NTSC 192-line V counter jumps after $DA"),
    (0x1A, 0xFF, 0xD5, "MD-VDP §11, SP-VCNT: ... to $D5"),
    (0x1B, 0x60, 0x40, "MD-VDP §10: nine sprites on a line set overflow regardless of pattern"),
    (0x1C, 0x60, 0x20, "MD-VDP §10: overlapping opaque sprite pixels set collision"),
    (0x1D, 0xFF, 0x01, "SP-MAP, SP-BIOS: slot 1 = bank 1 at power-on (315-5235)"),
    (0x1E, 0xFF, 0x02, "SP-MAP: slot 2 = bank 2 at power-on (315-5235)"),
    (0x1F, 0xFF, 0x02, "SP-MAP: bank 10 of an 8-bank ROM is masked to bank 2"),
    (0x20, 0xFF, 0x0A, "MD-HW §2: mapper writes go to RAM too ($DFFF)"),
    (0x21, 0xFF, 0xB0, "SP-MAP: $0000-$03FF stays bank 0 when slot 0 is remapped"),
    (0x22, 0xFF, 0xC3, "SP-MAP: slot 0 exposes bank offset $0400+"),
    (0x23, 0xFF, 0x03, "SP-MAP: slot 0 bank select"),
    (0x24, 0xFF, 0x05, "SP-MAP: slot 1 bank select"),
    (0x25, 0xFF, 0x05, "MD-HW §2, SP-MAP: $FFFE reads back the RAM copy"),
    (0x26, 0xFF, 0x02, "MD-HW §5, SP-PAUSE: one NMI per press; holding does not repeat"),
    (0x27, 0xFF, 0xFE, "MD-HW §4: UP held reads 0 in port $DC bit 0"),
    (0xFF, 0xFF, 0xA5, "fixture: program completed"),
]


def frame_checks(frames):
    """Doc-derived framebuffer expectations (MD-VDP §5, §9; SP-PAL CRAM mirror)."""
    out = []
    a, b = frames.get("frame_a"), frames.get("frame_b")
    out.append(("frame dimensions 256x192 active area", a is not None and (a["width"], a["height"]) == (256, 192)))
    out.append(("two white tiles at row 0, columns 0-1 (CRAM $21 aliases $01)",
                a is not None and a["white_pixels"] == 128 and a["white_box"] == [0, 0, 15, 7]))
    out.append(("R0 bit 5 blanks column 0 only", b is not None and b["white_pixels"] == 64 and b["white_box"] == [8, 0, 15, 7]))
    out.append(("column 0 shows the backdrop, distinct from background colour 0",
                b is not None and b["left_of_white"] not in (-1, 0xFFFFFF, b["below_white"])))
    return out


def evaluate(result):
    results = bytes.fromhex(result["results"])
    rows = []
    for offset, mask, expected, source in EXPECT:
        value = results[offset]
        rows.append(("C1%02X %s" % (offset, source), (value & mask) == expected, "%02X" % value))
    for name, ok in frame_checks(result):
        rows.append(("frame: " + name, ok, ""))
    rows.append(("handshake completed", bool(result["complete"]), ""))
    return rows


def build_core(root, spec, tmp):
    tree = pins.private_copy(root, spec["tree"], tmp)
    jobs = str(max(1, min(8, os.cpu_count() or 1)))
    subprocess.run(["make", "-j" + jobs, *spec["make"]], cwd=tree, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=900)
    found = [p for p in glob.glob(str(tree / spec["glob"])) if p.endswith((".so", ".dylib"))]
    if len(found) != 1:
        raise AssertionError("%s: libretro core not produced" % spec["tree"])
    return found[0]


def run(host, core, rom, options):
    proc = subprocess.run([host, core, rom, str(MAX_FRAMES), *options], text=True, capture_output=True, timeout=120)
    if proc.returncode not in (0, 4) or not proc.stdout.strip():
        raise AssertionError("host failed (%d): %s" % (proc.returncode, proc.stderr.strip()))
    return json.loads(proc.stdout)


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    if os.name != "posix":
        print("skipped: the libretro smoke host needs POSIX dlopen")
        return 0
    root = pins.checkout(NAMES)
    if root is None:
        print("skipped: " + pins.skip_reason(NAMES))
        return 0
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        host = str(pathlib.Path(tmp) / "libretro_host")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O1",
                        str(HERE / "sms_oracle" / "libretro_host.c"), "-o", host, "-ldl"], check=True)
        rom_bytes, meta = sms_fixture_rom.build("oracle_smoke")
        rom = str(pathlib.Path(tmp) / "oracle_smoke.sms")
        pathlib.Path(rom).write_bytes(rom_bytes)
        table = {}
        for name, spec in FINALISTS.items():
            core = build_core(root, spec, tmp)
            first = run(host, core, rom, spec["options"])
            second = run(host, core, rom, spec["options"])
            rows = evaluate(first)
            rows.append(("determinism: two runs identical", first == second, ""))
            japan = evaluate(run(host, core, rom, [o for o in spec["options"] if "region" not in o] + [spec["japan"]]))
            discriminates = not all(ok for label, ok, _ in japan if "SP-REGION" in label)
            rows.append(("negative control: Japanese console fails nationalization", discriminates, ""))
            table[name] = rows
            failures += sum(1 for _, ok, _ in rows if not ok)
        labels = [label for label, _, _ in next(iter(table.values()))]
        for index, label in enumerate(labels):
            cells = []
            for name in FINALISTS:
                _, ok, value = table[name][index]
                cells.append("%s%s" % ("pass" if ok else "FAIL", "(" + value + ")" if value else ""))
            print("%-100s %s" % (label[:100], "  ".join(cells)))
        checks = len(labels) * len(FINALISTS)
        print("sms oracle smoke: %d checks, %d failures (fixture sha256 %s)" % (checks, failures, meta["sha256"][:16]))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
