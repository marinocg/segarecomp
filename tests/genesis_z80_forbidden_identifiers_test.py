#!/usr/bin/env python3
"""SEG-032-T008 (contract section 14): the Genesis Z80 / audio / materialization production tree names no title, no driver, no
decompressor, no per-game list and no hash literal. The word list is tests/fixtures/genesis-z80-forbidden-identifiers.txt.

usage: genesis_z80_forbidden_identifiers_test.py <source-root>
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1]).resolve()
WORDS = root / "tests" / "fixtures" / "genesis-z80-forbidden-identifiers.txt"

# The production surface of the SEG-032 subsystem (everything a build links or runs for the Z80 sound program).
SCOPE = [
    "platforms/genesis/runtime/z80_machine.c", "platforms/genesis/runtime/z80_machine.h", "platforms/genesis/runtime/z80_registry.h",
    "platforms/genesis/runtime/genesis_audio.c", "platforms/genesis/runtime/genesis_audio.h",
    "platforms/genesis/runtime/genesis_sound.c", "platforms/genesis/runtime/genesis_sound.h",
    "platforms/genesis/runtime/genesis_sound_hook.c", "platforms/genesis/runtime/genesis_materialize_hook.c",
    "platforms/genesis/viewer/z80_epoch_probe_main_hook.c", "platforms/genesis/viewer/viewer_main_hook.c",
    "platforms/genesis/machine/include/segarecomp/machine/genesis/z80_images.hpp",
    "platforms/genesis/machine/include/segarecomp/machine/genesis/z80_materialization.hpp",
    "platforms/genesis/machine/src/z80_images.cpp", "platforms/genesis/machine/src/z80_materialization.cpp",
    "libs/device/sega/ym2612/include", "libs/device/sega/ym2612/src",
    "libs/codegen/c11/include/segarecomp/codegen/c11/z80.hpp", "libs/codegen/c11/include/segarecomp/codegen/c11/z80_lowering.hpp",
    "libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h", "libs/codegen/c11/src/z80.cpp",
    "libs/cpu/z80", "apps/segarecomp/build_command.cpp", "apps/segarecomp/build_command.hpp", "packaging/install.cmake",
]


def files():
    for entry in SCOPE:
        path = root / entry
        if path.is_dir():
            yield from sorted(p for p in path.rglob("*") if p.is_file() and "third_party" not in p.parts)
        elif path.is_file():
            yield path
        else:
            raise SystemExit("scan scope entry is missing: " + entry)
    yield from sorted((root / "libs/codegen/c11/src").glob("z80_*.cpp"))


def patterns():
    out = []
    for line in WORDS.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(re.compile(line, re.I))
    return out


def scan(text, pats):
    return [p.pattern for p in pats if p.search(text)]


def main():
    pats = patterns()
    assert len(pats) >= 15, "the word list must not be emptied"
    # negative controls: the scanner really flags each class
    for planted in ("static const char title[] = \"Sonic\";", "/* golden axe driver_list */", "0x" + "ab" * 32 + ";", "int saxman_decode(void);",
                    "const per_game_images[] = {0};"):
        assert scan(planted, pats), "scanner failed to flag: " + planted
    for clean in ("z80_run(rt, deadline);", "static const uint8_t tag[] = \"segarecomp.genesis.z80.image.v1\";", "0x7F11"):
        assert not scan(clean, pats), "scanner flagged a clean line: " + clean
    scanned, failures = 0, []
    for path in files():
        scanned += 1
        for pattern in scan(path.read_text(errors="replace"), pats):
            failures.append("%s: %s" % (path.relative_to(root), pattern))
    assert scanned >= 25, "scan scope shrank unexpectedly (%d files)" % scanned
    # no per-ROM compat hint may describe the Z80 (the existing hint files are 68K logical-table descriptors keyed by ROM hash)
    for hint in (root / "platforms/genesis/compat").glob("*"):
        text = hint.read_text(errors="replace")
        if re.search(r"z80|ym2612|psg|sound", text, re.I):
            failures.append("%s: a compat hint file describes the sound subsystem" % hint.name)
    if failures:
        print("\n".join(failures))
        return 1
    print("genesis z80 forbidden identifiers: ok (%d files, %d patterns)" % (scanned, len(pats)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
