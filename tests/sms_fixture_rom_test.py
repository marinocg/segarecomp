#!/usr/bin/env python3
"""SEG-009-T001: hermetic checks of the Master System fixture ROM builder (ADR 0064).

1. `--check`: every fixture rebuilds byte-for-byte to the committed manifest (size, mapper, SHA-256).
2. Header fields follow SMS Power "ROM header": TMR SEGA at $7FF0, region $4, size code, checksum rule.
3. Independent encoding cross-check: every instruction the embedded assembler emits for a fixture is
   looked up in the SEG-008 legal-form dataset (tests/fixtures/z80-legal-forms.json, derived from UM0080
   independently of this tool) by its opcode byte; mnemonic and length must agree.
4. Mapper declaration: fixtures declare a baseline identity; nothing is inferred from the header.
5. Assembler rejects unknown forms and out-of-range relative jumps (fail closed, never a guess).
6. Optional: the product header classifier (`segarecomp inspect`, argv[1]) recognizes every fixture as
   Master System export (reuse-unchanged evidence for libs/media).
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import sms_fixture_rom as builder  # noqa: E402

SIZE_CODES = {0x8000: 0xC, 0x10000: 0xE, 0x20000: 0xF, 0x40000: 0x0, 0x80000: 0x1}


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def form_index():
    data = json.loads((REPO / "tests" / "fixtures" / "z80-legal-forms.json").read_text(encoding="utf-8"))
    cols = data["form_columns"]
    space_i, mnem_i, len_i, ranges_i = cols.index("space"), cols.index("mnemonic"), cols.index("length"), cols.index("byte_ranges")
    index = {}
    for form in data["forms"]:
        if form[space_i] not in ("base", "ed"):
            continue
        for lo, hi in form[ranges_i]:
            for byte in range(lo, hi + 1):
                index.setdefault((form[space_i], byte), set()).add((form[mnem_i], form[len_i]))
    return index


def main():
    check(builder.main(["--check"]) == 0, "fixture manifest is not reproducible")
    manifest = json.loads(builder.MANIFEST.read_text(encoding="utf-8"))
    index = form_index()
    roms = {}
    for row in manifest["fixtures"]:
        name = row["fixture"]
        rom, meta = builder.build(name)
        again, _ = builder.build(name)
        check(rom == again, "%s: two builds differ" % name)
        check(meta == row, "%s: manifest row differs" % name)
        check(hashlib.sha256(rom).hexdigest() == row["sha256"], "%s: sha256" % name)
        check(row["mapper"] in builder.MAPPER_IDENTITIES, "%s: mapper identity not a baseline family" % name)
        check(row["declaration_source"] == "fixture_builder", "%s: declaration source" % name)
        # header (SMS Power ROM header)
        check(rom[0x7FF0:0x7FF8] == b"TMR SEGA", "%s: signature" % name)
        check(rom[0x7FFF] >> 4 == 0x4, "%s: region must be SMS export" % name)
        check(rom[0x7FFF] & 0xF == SIZE_CODES[len(rom)], "%s: size code" % name)
        total = (sum(rom[:0x7FF0]) + sum(rom[0x8000:])) & 0xFFFF
        check(rom[0x7FFA] | rom[0x7FFB] << 8 == total, "%s: checksum" % name)
        if row["mapper"] == "rom_only":
            check(len(rom) == 0x8000, "%s: rom_only must be 32 KiB" % name)
        roms[name] = rom
    # independent encoding cross-check
    checked = 0
    for source in (builder.TRIVIAL_SOURCE, builder.ORACLE_SMOKE_SOURCE, *builder.T003_SOURCES):
        asm = builder.Assembler(source)
        asm.assemble()
        for address, mnem, encoded in asm.listing:
            space, op = ("ed", encoded[1]) if encoded[0] == 0xED else ("base", encoded[0])
            forms = index.get((space, op))
            check(forms is not None, "0x%04X %s: opcode %s/%02X is not a legal form" % (address, mnem, space, op))
            check(any(m == mnem.upper() and length == len(encoded) for m, length in forms),
                  "0x%04X %s %s: disagrees with the legal-form dataset %s" % (address, mnem, bytes(encoded).hex(), forms))
            checked += 1
    check(checked > 300, "too few instructions cross-checked")
    # full-byte golden encodings (Zilog UM0080 opcode tables): register fields, operands and prefixes
    golden = {"ld b,a": "47", "ld a,b": "78", "ld c,a": "4f", "ld a,c": "79", "ld (hl),0x12": "3612",
              "ld a,(hl)": "7e", "ld (hl),a": "77", "ld a,(0x1234)": "3a3412", "ld (0x1234),a": "323412",
              "ld hl,0x1234": "213412", "ld sp,0xdff0": "31f0df", "out (0xbf),a": "d3bf", "in a,(0x7e)": "db7e",
              "im 1": "ed56", "im 2": "ed5e", "ld i,a": "ed47", "otir": "edb3", "ldir": "edb0", "reti": "ed4d",
              "retn": "ed45", "push af": "f5", "pop bc": "c1", "add a,16": "c610", "cp 0xff": "feff", "or c": "b1",
              "and 0xc0": "e6c0", "xor a": "af", "dec bc": "0b", "inc hl": "23", "inc (hl)": "34", "call 0x1234": "cd3412",
              "jp 0x1234": "c33412", "out (c),a": "ed79", "in b,(c)": "ed40", "rst 0x38": "ff"}
    for text, expected in golden.items():
        image, _ = builder.Assembler(text).assemble()
        got = bytes(image[a] for a in sorted(image)).hex()
        check(got == expected, "golden encoding %r: %s != %s" % (text, got, expected))
    rel, _ = builder.Assembler("x: jr x\ndjnz x\njr nz,x").assemble()
    check(bytes(rel[a] for a in sorted(rel)).hex() == "18fe10fc20fa", "relative branch encodings")
    # fail closed on unknown forms and ranges
    for bad in ("ld (ix+1),a", "frobnicate", ".org 0\nlabel: jr far\n.org 0x200\nfar: nop"):
        try:
            builder.Assembler(bad).assemble()
        except builder.AsmError:
            continue
        raise AssertionError("assembler accepted %r" % bad)
    # optional product classifier
    if len(sys.argv) > 1 and sys.argv[1]:
        with tempfile.TemporaryDirectory() as tmp:
            for name, rom in roms.items():
                path = pathlib.Path(tmp) / (name + ".sms")
                path.write_bytes(rom)
                out = subprocess.run([sys.argv[1], "inspect", str(path)], text=True, capture_output=True, timeout=60)
                check(out.returncode == 0, "%s: segarecomp inspect failed: %s" % (name, out.stderr))
                check("diagnostic: HDR_RECOGNIZED_SMS" in out.stdout and "region-system: sms_export" in out.stdout,
                      "%s: not classified as SMS export:\n%s" % (name, out.stdout))
                check("mapper" not in out.stdout.lower(), "%s: header classification must not name a mapper" % name)
    print("ok: %d fixtures reproduce; %d instructions agree with the legal-form dataset" % (len(roms), checked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
