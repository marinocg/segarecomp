#!/usr/bin/env python3
"""SEG-028-T002 (ADR 0077): the CPU-neutral executable-image header names no CPU, console, device, mapper or Z80-emitter concept.

The scan strips // and /* */ comments (string literals are scanned as code) and matches each forbidden identifier case-insensitively
at word boundaries ('_' and '.' separate words, so `genesis.cartridge` and `z80_image` are caught). The header's only non-standard
include is segarecomp/core/address.hpp. A self-check plants a forbidden token in a temporary copy and requires the scan to find it.

usage: executable_image_forbidden_identifiers_test.py <source-root>
"""
import pathlib
import re
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
HEADER = root / "libs" / "recompiler" / "include" / "segarecomp" / "recompiler" / "executable_image.hpp"

FORBIDDEN = [
    "genesis", "mega", "sms", "master_system", "z80", "m68k", "mc68000", "68000", "68k", "epoch", "signature", "activation",
    "hold_window", "mapper", "slot", "bank", "mirror", "cartridge", "rom", "ram", "vdp", "ImageKind", "live_bytes", "live_guard",
    "PassRunner", "CodeImage", "CodeWindow", "uint16_t", "8192", "0x2000", "0x4000", "0xFFFF",
]
ALLOWED_INCLUDES = {"segarecomp/core/address.hpp"}

failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def strip_comments(text):
    """Remove // and /* */ comments, keeping string and character literals intact."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            quote = c
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            out.append(" ")
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def pattern(word):
    # A "word" boundary: not preceded/followed by a letter or digit ('_' and '.' are separators). Hex literals such as 0xFFFF must
    # also not be a prefix of a longer hex literal (0xFFFFFFFF is a different, CPU-neutral constant).
    return re.compile(r"(?<![A-Za-z0-9])" + re.escape(word) + r"(?![A-Za-z0-9])", re.I)


PATTERNS = [(word, pattern(word)) for word in FORBIDDEN]


def scan(code):
    return sorted({word for word, pat in PATTERNS if pat.search(code)})


def includes(code):
    return re.findall(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', code, re.M)


def main():
    if not HEADER.is_file():
        raise SystemExit("missing header: " + str(HEADER))
    text = HEADER.read_text()
    code = strip_comments(text)
    hits = scan(code)
    check(not hits, "executable_image.hpp code names no forbidden identifier" + (": " + ", ".join(hits) if hits else ""))

    non_std = [path for kind, path in includes(code) if kind == '"' or "/" in path]
    check(set(non_std) == ALLOWED_INCLUDES,
          "the only non-standard include is segarecomp/core/address.hpp (found: " + ", ".join(non_std) + ")")

    # Self-checks: the comment stripper keeps strings, the scanner finds planted tokens, comments are ignored.
    check(scan(strip_comments('const char *p = "genesis.cartridge";')) == ["cartridge", "genesis"],
          "self-check: a forbidden token inside a string literal is detected")
    check(scan(strip_comments("int x; // z80 bank mirror\n/* epoch */ int y;")) == [],
          "self-check: forbidden tokens inside comments are ignored")
    check(scan("std::uint32_t v = 0xFFFFFFFFU;") == [] and scan("v = 0xFFFF;") == ["0xFFFF"],
          "self-check: 0xFFFF is matched as a whole literal only")
    check(scan("enum Kind { RomImage, z80_window };") == ["z80"] and scan("ImageKind k;") == ["ImageKind"],
          "self-check: '_' separates words; identifiers are case-insensitive whole words")
    with tempfile.TemporaryDirectory() as tmp:
        planted = pathlib.Path(tmp) / "executable_image.hpp"
        planted.write_text(text.replace("namespace segarecomp {", "namespace segarecomp {\ninline constexpr int kZ80Bank = 0;\n"
                                        "struct CodeWindow;\n", 1))
        found = scan(strip_comments(planted.read_text()))
        check("CodeWindow" in found, "self-check: a planted forbidden identifier in a temporary copy is detected (" +
              ", ".join(found) + ")")
        planted.write_text(text.replace('#include "segarecomp/core/address.hpp"',
                                        '#include "segarecomp/core/address.hpp"\n#include "segarecomp/cpu/z80/decode.hpp"', 1))
        extra = [path for kind, path in includes(strip_comments(planted.read_text())) if kind == '"' or "/" in path]
        check(set(extra) != ALLOWED_INCLUDES, "self-check: a planted extra project include is detected")

    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("executable_image_forbidden_identifiers_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
