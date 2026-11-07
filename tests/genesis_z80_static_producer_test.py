#!/usr/bin/env python3
"""ADR 0277: the typed outcomes of the static-first Genesis Z80 image producer on project-authored fixture ROMs.

No compiler and no guest execution: `segarecomp genesis-z80-static-report --rom <image>` walks the image and prints sanitized aggregates
(outcome, epoch/image counts, mechanism counts, refusal classes). Each row names the shape the fixture exercises:

  proven            single contiguous ROM upload; multi-segment/multi-epoch sequences; a stub followed by a driver; a Kosinski-compressed driver
                    expanded by the recognised reference routine; the same driver activated more than once; a plain restart with an empty hold
                    window; carry-over bytes outside an uploaded extent
  refused           computed (XOR) upload, unknown data-dependent length, unknown source pointer, unknown destination pointer, a destination
                    beyond the sound RAM (raw and decompressed), an UNRECOGNISED decompressor routine, and a valid stream that is not executable
                    authority (expanded into work RAM)

usage: genesis_z80_static_producer_test.py <segarecomp> <source-root>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

cli, root = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
sys.path.insert(0, str(root / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def report(rom_bytes, directory, name):
    path = directory / (name + ".md")
    path.write_bytes(rom_bytes)
    done = subprocess.run([cli, "genesis-z80-static-report", "--rom", str(path)], text=True, capture_output=True, timeout=120)
    assert done.returncode == 0, done.stderr
    fields = dict(item.split("=", 1) for item in done.stdout.split())
    return fields, done.stdout


# name -> expected (outcome, epochs, images, extra fields that must be present with these values)
EXPECT = {
    "sound_raw": ("complete", 1, 1, {"copy_loops": "1", "decoder_calls": "0"}),
    "sound_tone": ("complete", 1, 1, {"copy_loops": "1"}),
    "sound_multi_epoch": ("complete", 4, 2, {"copy_loops": "3"}),                  # A, B, restart, A again: 4 epochs, 2 images, stable order
    "static_two_phase": ("complete", 4, 2, {"copy_loops": "3"}),                   # A, B, A, restart
    "multi_epoch_dirty": ("complete", 6, 3, {}),                                   # carry-over bytes outside every extent; plain restart; repeats
    "static_kos": ("complete", 1, 1, {"decoder_calls": "1", "copy_loops": "0"}),   # the recognised reference decompressor
    "static_stub_driver": ("complete", 3, 2, {"decoder_calls": "2", "copy_loops": "1"}),  # stub, driver, driver again
    "static_kos_work_ram": ("complete", 2, 2, {"decoder_calls": "0"}),             # a valid stream expanded into WORK RAM contributes no image bytes
    "sound_domestic_gated": ("complete", 1, 1, {}),                                # an unknown region branch outside the window does not hide the upload
    # refused shapes: nothing is claimed
    "sound_decoded": ("partial", 1, 1, {"refusal.z80_store_value_unknown": "1"}),  # the raw epoch is proven, the computed one is not
    "static_ram_count": ("partial", 1, 1, {"refusal.length_unknown": "1"}),   # the first (proven) upload stands; the data-dependent re-upload is refused
    "static_ram_source": ("none", 0, 0, {"refusal.source_not_immutable": "1"}),
    "static_ram_destination": ("none", 0, 0, {"refusal.z80_store_unknown_address": "1"}),
    "static_raw_beyond_ram": ("none", 0, 0, {"refusal.destination_out_of_ram": "1"}),
    "static_kos_beyond_ram": ("none", 0, 0, {"refusal.destination_out_of_ram": "1"}),
    "static_kos_unrecognised": ("none", 0, 0, {"refusal.decompression_unproven": "1", "decoder_calls": "0"}),
}


def main():
    with tempfile.TemporaryDirectory(prefix="segarecomp-z80-static-producer-") as directory:
        tmp = pathlib.Path(directory)
        for name, (outcome, epochs, images, extra) in sorted(EXPECT.items()):
            fields, line = report(fx.build(name), tmp, name)
            check(fields["outcome"] == outcome and int(fields["epochs"]) == epochs and int(fields["images"]) == images,
                  "%s: outcome=%s epochs=%d images=%d (got %s)" % (name, outcome, epochs, images, line.strip()))
            for key, value in extra.items():
                check(fields.get(key) == value, "%s: %s=%s" % (name, key, value))
            if outcome == "complete" or outcome == "partial":
                check(int(fields["steps"]) < 20000, "%s: bounded walk (%s steps)" % (name, fields["steps"]))
        # determinism: the same image, repeatedly, is the same report
        a = report(fx.build("static_stub_driver"), tmp, "det_a")[1]
        b = report(fx.build("static_stub_driver"), tmp, "det_b")[1]
        check(a == b, "the report is deterministic")
        # the report is sanitized: counts and class names only (no hex digest, no address-like field)
        check(not re.search(r"[0-9a-f]{16,}", a) and "0x" not in a, "the report carries no digest/address")
        # a corrupted stream (its first descriptor word makes the first token a copy from before the stream) is detected, never silently decoded
        rom = bytearray(fx.build("static_kos"))
        stream = fx.scd_common.kosinski_encode(fx.sound_driver(2))
        at = bytes(rom).find(stream)
        check(at > 0, "the encoded stream is locatable in the fixture")
        rom[at : at + 2] = b"\x00\x00"
        fields, line = report(bytes(rom), tmp, "corrupt_stream")
        check(fields["outcome"] in ("none", "partial", "unsupported") and int(fields["epochs"]) == 0 and fields.get("refusal.decode_failed") == "1",
              "a corrupted stream is refused (decode_failed): " + line.strip())
        # a truncated image (no code at the reset vector) is unsupported, not a crash
        fields, line = report(bytes(4) + b"\x00\x10\x00\x01" + bytes(8), tmp, "truncated")
        check(fields["outcome"] == "unsupported" and fields.get("refusal.entry_unreadable") == "1", "an unreadable entry is unsupported: " + line.strip())
    if failures:
        print("FAILED: %d check(s)" % len(failures))
        return 1
    print("all static producer checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
