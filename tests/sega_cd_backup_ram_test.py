#!/usr/bin/env python3
"""SEG-065-T002 (ADR 0147): the Sega CD internal backup RAM machine input.

  * tests/tools/sega_cd_backup_ram_table.c: every access shape through the runtime's own bus ABI against hand-derived expectations
    (odd/even byte, word, long, the 16 KiB and 1 MiB mirrors, counters), the fail-closed default (typed stop with processor, pc,
    address, width and direction, state untouched) and the exact-length install;
  * tools/sega_cd_backup_ram.py: the authored formatted, empty image (size, zero body, the 64-byte trailer fields) and its check;
  * a generated-native fixture program: `--backup-ram` with a formatted image reports the input's digest and leaves the rest of the
    report identical to the run without it; a wrong-length or unreadable input is refused before any guest instruction (exit 22).

usage: sega_cd_backup_ram_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

TOOL = ROOT / "tools" / "sega_cd_backup_ram.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def tool(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True, timeout=60).returncode


def main():
    with tempfile.TemporaryDirectory(prefix="scd-bram-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / ("backup_ram_table" + common.EXE)
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        str(ROOT / "tests" / "tools" / "sega_cd_backup_ram_table.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        out = subprocess.run([str(table)], capture_output=True, text=True, timeout=120)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        check(out.returncode == 0 and len(rows) >= 50 and not any(r.startswith("FAIL") for r in rows),
              "backup RAM bus table: %d rows" % len(rows))
        for line in rows:
            if line.startswith("FAIL"):
                print("      " + line)

        # the authored image
        image = tmp / "formatted.brm"
        check(tool("format", image) == 0, "format writes an image")
        data = image.read_bytes()
        trailer = data[-64:]
        check(len(data) == 8192 and data[:-64] == bytes(8192 - 64), "8 KiB, zero except the final 64 bytes")
        check(trailer[:16] == b"_" * 11 + b"\x00\x00\x00\x00\x40", "trailer: volume field")
        check(trailer[16:24] == (125).to_bytes(2, "big") * 4 and trailer[24:32] == bytes(8),
              "trailer: 125 free blocks (8192 / 64 - 3) four times, then eight zero bytes")
        check(trailer[32:48] == b"SEGA_CD_ROM\x00\x01\x00\x00\x00" and trailer[48:] == b"RAM_CARTRIDGE___", "trailer: format signatures")
        check(tool("check", image) == 0, "check accepts the formatted image")
        bad = tmp / "bad.brm"
        bad.write_bytes(data[:-1] + b"\x00")
        check(tool("check", bad) == 1, "check rejects a changed byte")
        bad.write_bytes(data + b"\x00")
        check(tool("check", bad) == 1, "check rejects 8193 bytes")
        check(tool("check", tmp / "missing.brm") == 1, "check rejects a missing file")
        check(tool("frobnicate", image) == 2, "usage error")

        # a generated-native program with and without the machine input
        disc, exe, _, _ = builder.program("subside", "subside")
        code_a, plain = common.run(exe, disc, 6000000)
        code_b, with_ram = common.run(exe, disc, 6000000, "--backup-ram", str(image))
        check(code_a == 0 and "backup_ram" not in plain, "without --backup-ram: no backup_ram report field (reports unchanged)")
        check(code_b == 0 and with_ram.get("backup_ram") == {"reads": 0, "writes": 0, "sha256": hashlib.sha256(data).hexdigest()},
              "with --backup-ram: reads 0, writes 0, digest of the input (%s)" % json.dumps(with_ram.get("backup_ram")))
        rest = dict(with_ram)
        rest.pop("backup_ram", None)
        check(rest == plain, "with --backup-ram: every other report field identical to the run without it")
        for label, path, error in (("8191 bytes", "short.brm", "length"), ("8193 bytes", "long.brm", "length"),
                                   ("a missing file", "absent.brm", "unreadable")):
            if path != "absent.brm":
                (tmp / path).write_bytes(data[:8191] if path == "short.brm" else data + b"\x00")
            code, report = common.run(exe, disc, 6000000, "--backup-ram", str(tmp / path))
            check(code == 22 and report == {"result": "backup_ram_refused", "error": error},
                  "refused before any guest instruction: %s -> exit %d %s" % (label, code, report))
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
