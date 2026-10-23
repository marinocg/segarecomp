#!/usr/bin/env python3
"""SEG-071-T002 (ADR 0177): the long-instruction refresh-skip record is build-time, per verified variant.

Four project-authored disc extents are placed at the same main work-RAM address, so the pc has four verified variants (selected at run
time by their exact bytes). The generated program must give each variant its own record, derived from the CPU owner's decoded
instruction: a register-count shift reads its count register (before the instruction executes), MOVEM.L carries 8 x its register count,
and MOVEM.W, EXT.L (whose opcode shares MOVEM's mask) and an immediate-count shift carry none. The record is written after the variant's
byte guard and after the verify-only return; the runtime source holds no opcode classification of live guest bytes.

usage: sega_cd_refresh_skip_variant_test.py <segarecomp> <c-compiler> <source-root>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []


def check(ok, message):
    print(("ok    " if ok else "FAIL  ") + message)
    if not ok:
        failures.append(message)


# (label, opcode bytes, expected refresh record or None)
VARIANTS = [
    ("LSR.W D7,D1", bytes.fromhex("EE69"), "m->refresh_skip_cycles = 2U * (cpu->d[7] & UINT32_C(63));"),
    ("LSL.L D3,D3 (count register is the destination)", bytes.fromhex("E7AB"), "m->refresh_skip_cycles = 2U * (cpu->d[3] & UINT32_C(63));"),
    ("MOVEM.L (A7)+,D0-D7/A0-A6", bytes.fromhex("4CDF7FFF"), "m->refresh_skip_cycles = 120U;"),
    ("MOVEM.L D0-D1,-(A7)", bytes.fromhex("48E7C000"), "m->refresh_skip_cycles = 16U;"),
    ("MOVEM.W (A7)+,D0-D7/A0-A6", bytes.fromhex("4C9F7FFF"), None),
    ("EXT.L D0 (opcode $48C0 shares MOVEM.L's mask)", bytes.fromhex("48C0"), None),
    ("ASL.W #1,D0", bytes.fromhex("E340"), None),
    ("NOP", bytes.fromhex("4E71"), None),
]


def main():
    with tempfile.TemporaryDirectory(prefix="scd-skip-") as tmp:
        tmp = pathlib.Path(tmp)
        disc = tmp / "disc"
        common.build_disc(disc, "comm")
        fx = tmp / "fx"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_fixture.py"), "build", "--out", str(fx)], check=True, capture_output=True)
        track = disc / "track01.bin"
        data = bytearray(track.read_bytes())
        specs = []
        for i, (_label, raw, _want) in enumerate(VARIANTS):
            offset = 0x7000 + 0x100 * i
            data[offset:offset + len(raw)] = raw
            specs += ["--disc-image", "main:%d:0xFF0100:%d" % (offset, len(raw))]
        track.write_bytes(bytes(data))
        out = tmp / "program.c"
        r = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(disc / "disc.cue"), "--bios", str(fx / "bios.bin"), "--out", str(out), *specs],
                           capture_output=True, text=True, timeout=300)
        check(r.returncode == 0, "emission with %d overlapping verified variants at one main pc (%s)" % (len(VARIANTS), r.stdout[-300:] + r.stderr[-300:]))
        source = out.read_text()
        funcs = {}
        for m in re.finditer(r"static const uint8_t (scd_p0_i\d+_FF0100)_bytes\[\] = \{([^}]*)\};\nstatic int \1\(ScdMachine \*m\) \{\n(.*?)(?=\nstatic |\Z)", source, re.S):
            raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
            funcs[raw] = m.group(3)
        for label, raw, want in VARIANTS:
            body = funcs.get(raw)
            if body is None:
                check(False, "%s: a variant function with exactly its bytes exists" % label)
                continue
            records = re.findall(r"m->refresh_skip_cycles = [^;]*;", body)
            check(records == ([want] if want else []), "%s: record %s (found %s)" % (label, want or "none", records))
            if want:
                guard, verify, record, first = (body.find("scd_code_guard"), body.find("m->verify_only"), body.find("m->refresh_skip_cycles"),
                                                body.rfind("cpu->pc = pc;"))
                check(0 <= guard < record and (verify < 0 or verify < record) and record < first,
                      "%s: the record follows the byte guard and the verify-only return, before the body (%s)" % (label, (guard, verify, record, first)))
        # processor 1 (the sub) never carries the main-only record
        check("scd_p1_" in source and not re.search(r"scd_p1_[^\n]*refresh_skip", source) and
              all("refresh_skip" not in f for f in re.findall(r"static int scd_p1_i\d+_\w+\(ScdMachine \*m\) \{.*?\n\}\n", source, re.S)),
              "no sub-processor variant carries a refresh-skip record")
        # the runtime holds no opcode classification of live guest bytes
        text = (ROOT / "platforms" / "sega-cd" / "runtime" / "scd_machine.c").read_text()
        check("scd_main_refresh_skip_cycles" not in text and not re.search(r"0xF000U\) == 0xE000U|0xFB80U", text),
              "the runtime scheduler carries no opcode-mask classification")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
