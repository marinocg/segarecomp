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
    "platforms/genesis/runtime/genesis_mixer.c", "platforms/genesis/runtime/genesis_mixer.h", "platforms/genesis/runtime/genesis_audio_format.h",
    "platforms/genesis/runtime/genesis_audio_present.c", "platforms/genesis/runtime/genesis_audio_present.h",
    "platforms/genesis/runtime/genesis_audio_sink.h",
    "platforms/genesis/runtime/genesis_sound_hook.c", "platforms/genesis/runtime/genesis_materialize_hook.c",
    "platforms/genesis/viewer/z80_epoch_probe_main_hook.c", "platforms/genesis/viewer/viewer_main_hook.c",
    "platforms/genesis/machine/include/segarecomp/machine/genesis/z80_images.hpp",
    "platforms/genesis/machine/include/segarecomp/machine/genesis/z80_materialization.hpp",
    "platforms/genesis/machine/src/z80_images.cpp", "platforms/genesis/machine/src/z80_materialization.cpp",
    "platforms/genesis/machine/include/segarecomp/machine/genesis/m68k_copy_alias.hpp", "platforms/genesis/machine/src/m68k_copy_alias.cpp",
    "libs/device/sega/ym2612/include", "libs/device/sega/ym2612/src",
    "libs/codegen/c11/include/segarecomp/codegen/c11/z80.hpp", "libs/codegen/c11/include/segarecomp/codegen/c11/z80_lowering.hpp",
    "libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h", "libs/codegen/c11/src/z80.cpp",
    "libs/cpu/z80", "apps/segarecomp/build_command.cpp", "apps/segarecomp/build_command.hpp", "packaging/install.cmake",
]


# ADR 0277 (amends contract section 14 for exactly these files): the static-first producer is the one place that may name the decompressor
# FORMAT it recognises (the evidence-based loader contract) and carry the digests of the recognised ROUTINE FORMS (an algorithm's code,
# position independent, shared by several titles - not a title, not a ROM hash). Everything else stays forbidden here: titles, drivers lists,
# manifests, per-game data, other formats, and any hash literal other than the declared routine-form table entries.
PRODUCER_SCOPE = [
    "platforms/genesis/machine/include/segarecomp/machine/genesis/z80_static_producer.hpp",
    "platforms/genesis/machine/src/z80_static_producer.cpp",
    "libs/core/include/segarecomp/core/kosinski.hpp",
]
PRODUCER_ALLOWED_WORDS = ("kosinski",)
PRODUCER_MAX_DIGESTS = 2  # the recognised routine forms; each must sit on its table line


def producer_failures(pats):
    failures = []
    hash_pattern = re.compile(r"(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])", re.I)
    for entry in PRODUCER_SCOPE:
        path = root / entry
        if not path.is_file():
            raise SystemExit("producer scope entry is missing: " + entry)
        text = path.read_text(errors="replace")
        for pattern in pats:
            if any(word in pattern.pattern for word in PRODUCER_ALLOWED_WORDS) or pattern.pattern == hash_pattern.pattern:
                continue
            if pattern.search(text):
                failures.append("%s: %s" % (entry, pattern.pattern))
        digests = 0
        for line in text.splitlines():
            if hash_pattern.search(line):
                digests += 1
                if not re.search(r'\{"(reference|library)",', line):
                    failures.append("%s: a hash literal outside the routine-form table: %s" % (entry, line.strip()[:60]))
        if digests > PRODUCER_MAX_DIGESTS:
            failures.append("%s: more than %d routine-form digests" % (entry, PRODUCER_MAX_DIGESTS))
    return failures


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
    scanned, failures = 0, producer_failures(pats)
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
