#!/usr/bin/env python3
"""SEG-062-T002 (Checkpoint A): Sega CD disc ingestion and IP/SP executable images against an independent reader.

For generated fixture discs (several fixtures and seeds, cooked MODE1/2048 and raw MODE1/2352, with an audio track) the C++ reader
(`segarecomp sega-cd inspect`, libs/media + platforms/sega-cd/machine) and the independent Python reader (sega_cd_disc_oracle.py)
must agree on files, tracks, sector counts, header fields, IP/SP extents, image digests, entry PCs, every data sector digest and the
disc identity. Re-ingestion is byte-identical. Every malformed input yields a stable typed rejection and no partial result.

usage: sega_cd_disc_ingestion_test.py <segarecomp> <source-root>
"""
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

SEGARECOMP, ROOT = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_disc_oracle as oracle  # noqa: E402

BUILDER = ROOT / "tools" / "sega_cd_fixture_disc.py"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def build(directory, fixture, variant, *extra):
    subprocess.run([sys.executable, str(BUILDER), "build", "--fixture", fixture, "--variant", variant, "--out", str(directory),
                    *extra], check=True, capture_output=True)
    return directory / "disc.cue"


def inspect(cue, *extra):
    run = subprocess.run([SEGARECOMP, "sega-cd", "inspect", "--cue", str(cue), *extra], capture_output=True, text=True, timeout=120)
    return run.returncode, run.stdout


def compare(cue, label):
    code, out = inspect(cue, "--sector-digests")
    if code != 0:
        check(False, label + ": inspect failed: " + out)
        return None
    got = json.loads(out)
    ref = oracle.read_disc(cue, sector_digests=True)
    check(got["identity"] == ref["identity"], label + ": disc identity")
    check(got["total_sectors"] == ref["total_sectors"], label + ": total sectors")
    check(got["files"] == ref["files"], label + ": files (names, sizes, SHA-256)")
    check(got["tracks"] == ref["tracks"], label + ": track table")
    check(got["sector_sha256"] == ref["sector_sha256"], label + ": every data-sector digest (%d)" % len(ref["sector_sha256"]))
    boot, rb = got["boot"], ref["boot"]
    for field in ("system_id", "volume_name", "system_name", "hardware_id", "product_code", "region", "ip", "sp"):
        check(boot[field] == rb[field], label + ": header field " + field)
    main, sub = boot["images"]
    check(main["processor"] == "main" and sub["processor"] == "sub" and main["cpu"] == sub["cpu"] == "mc68000",
          label + ": one mc68000 set per processor instance")
    check(main["sha256"] == rb["ip_sha256"] and sub["sha256"] == rb["sp_sha256"], label + ": IP/SP image digests")
    check(main["entry_pc"] == rb["main_entry_pc"] and sub["entry_pc"] == rb["sub_entry_pc"], label + ": entry PCs")
    check(main["execution_base"] == 0xFF0000 and sub["execution_base"] == 0x6000, label + ": profile bases")
    check(main["length"] == rb["ip"]["load_size"] and sub["length"] == rb["sp"]["load_size"], label + ": image lengths")
    check(main["authority"] == sub["authority"] == "immutable_input" and main["verification"] == "byte_identity",
          label + ": authority and verification")
    check("disc" not in json.dumps(main["provenance"]) and main["provenance"]["producers"] == {"sega_cd.boot_ip": 1},
          label + ": provenance report carries counts only")
    return got


def expect_error(cue, error, label, stage="disc"):
    code, out = inspect(cue)
    try:
        got = json.loads(out)
    except ValueError:
        got = {}
    if stage == "disc":
        ok = code == 3 and got.get("ok") is False and got.get("error") == error and "tracks" not in got
    else:
        ok = code == 4 and got.get("boot", {}).get("ok") is False and error in (got["boot"].get("error"), got["boot"].get("boot_error"))
    check(ok, "reject %s -> %s (got %d %s)" % (label, error, code, out.strip()[:160]))


def main():
    with tempfile.TemporaryDirectory(prefix="scd-ingest-") as tmp:
        tmp = pathlib.Path(tmp)
        # --- agreement with the independent reader -------------------------------------------------------------------------
        cases = [("comm", "cooked", []), ("comm", "raw", []), ("wordram2m", "cooked", []), ("timer", "raw", [])]
        cases += [("cdframe", variant, ["--seed", str(seed)]) for variant in ("cooked", "raw") for seed in (1, 62, 977)]
        identities = {}
        for fixture, variant, extra in cases:
            label = "%s/%s%s" % (fixture, variant, "".join(" " + e for e in extra))
            cue = build(tmp / label.replace("/", "_").replace(" ", "_"), fixture, variant, *extra)
            got = compare(cue, label)
            if got:
                identities[label] = got["identity"]
        check(len(set(identities.values())) == len(identities), "every distinct disc has a distinct identity")

        # --- determinism and identity --------------------------------------------------------------------------------------
        cue = build(tmp / "det", "cdframe", "raw")
        first, second = inspect(cue, "--sector-digests"), inspect(cue, "--sector-digests")
        check(first == second and first[0] == 0, "re-ingesting the same inputs gives identical output")
        identity = json.loads(first[1])["identity"]
        check(inspect(cue, "--expect-identity", identity)[0] == 0, "the matching expected identity is accepted")
        expect_error_cue = cue
        code, out = inspect(expect_error_cue, "--expect-identity", "0" * 64)
        check(code == 3 and json.loads(out)["error"] == "identity_mismatch", "a different expected identity is rejected")

        flipped = build(tmp / "flip", "cdframe", "raw", "--data-flip", "100")
        got_flip = json.loads(inspect(flipped, "--sector-digests")[1])
        base = json.loads(first[1])
        check(got_flip["identity"] != base["identity"], "one changed data byte changes the disc identity")
        changed = [lba for lba in base["sector_sha256"] if base["sector_sha256"][lba] != got_flip["sector_sha256"][lba]]
        check(changed == ["24"], "exactly the changed data sector's digest changes (%s)" % changed)
        check(got_flip["boot"]["images"][0]["sha256"] == base["boot"]["images"][0]["sha256"],
              "a data-sector change leaves the IP image digest unchanged")
        ip_patch = tmp / "ip_patch"
        shutil.copytree(tmp / "det", ip_patch)
        data = bytearray((ip_patch / "track01.bin").read_bytes())
        data[16 + 0x200 + 1] ^= 0x01  # raw sector 0 user byte $201 (inside the IP)
        (ip_patch / "track01.bin").write_bytes(data)
        got_ip = json.loads(inspect(ip_patch / "disc.cue")[1])
        check(got_ip["boot"]["images"][0]["sha256"] != base["boot"]["images"][0]["sha256"] and
              got_ip["boot"]["images"][0]["sha256"] == oracle.read_disc(ip_patch / "disc.cue")["boot"]["ip_sha256"],
              "an IP byte change changes the main image digest exactly as the oracle computes it")

        # --- malformed discs ------------------------------------------------------------------------------------------------
        good = tmp / "good"
        build(good, "cdframe", "cooked")
        raw = tmp / "goodraw"
        build(raw, "cdframe", "raw")

        def variant_dir(name, source=good):
            directory = tmp / ("neg_" + name)
            shutil.copytree(source, directory)
            return directory

        def with_cue(name, text, source=good):
            directory = variant_dir(name, source)
            (directory / "disc.cue").write_text(text)
            return directory / "disc.cue"

        one = 'FILE "track01.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'
        d = variant_dir("truncated")
        blob = (d / "track01.bin").read_bytes()
        (d / "track01.bin").write_bytes(blob[:-100])
        expect_error(d / "disc.cue", "file_size_not_multiple", "truncated BIN (size not a sector multiple)")
        d = variant_dir("short")
        (d / "track01.bin").write_bytes(blob[:2048 * 8])
        (d / "disc.cue").write_text(one)
        expect_error(d / "disc.cue", "system_area_short", "BIN shorter than the system area", stage="boot")
        expect_error(with_cue("index_beyond", one + "  TRACK 02 MODE1/2048\n    INDEX 01 00:00:50\n"), "file_truncated",
                     "INDEX beyond the end of its file")
        expect_error(with_cue("no_file", '  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'), "file_missing", "TRACK before FILE")
        expect_error(with_cue("file_no_track", 'FILE "track02.bin" BINARY\n' + one), "file_missing", "FILE without TRACK")
        expect_error(with_cue("missing", 'FILE "nothere.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'),
                     "file_unreadable", "missing referenced file")
        expect_error(with_cue("dup", one + 'FILE "track01.bin" BINARY\n  TRACK 02 MODE1/2048\n    INDEX 01 00:00:00\n'),
                     "file_ambiguous", "the same FILE twice")
        for name, path in (("dotdot", "../good/track01.bin"), ("abs", "/etc/hostname"), ("subdir", "sub/track01.bin"),
                           ("backslash", "..\\\\track01.bin"), ("drive", "C:track01.bin")):
            expect_error(with_cue("path_" + name, one.replace("track01.bin", path)), "path_unsafe", "path traversal: " + path)
        d = variant_dir("symlink")
        os.symlink(tmp / "good" / "track01.bin", d / "link.bin")
        (d / "disc.cue").write_text(one.replace("track01.bin", "link.bin"))
        expect_error(d / "disc.cue", "path_unsafe", "symlinked BIN")
        expect_error(with_cue("gap", one.replace("00:00:00", "00:00:02")), "track_layout", "first INDEX not at 0 (gap)")
        expect_error(with_cue("overlap", one + "  TRACK 02 MODE1/2048\n    INDEX 01 00:00:00\n"), "track_layout",
                     "overlapping tracks in one file")
        expect_error(with_cue("numbering", one.replace("TRACK 01", "TRACK 02")), "track_numbering", "tracks not numbered from 1")
        expect_error(with_cue("mode2", one.replace("MODE1/2048", "MODE2/2352")), "cue_unsupported", "MODE2 track")
        expect_error(with_cue("index0", one.replace("INDEX 01", "INDEX 00")), "cue_unsupported", "INDEX 00")
        expect_error(with_cue("wave", one.replace("BINARY", "WAVE")), "cue_unsupported", "WAVE file type")
        expect_error(with_cue("syntax", one.replace('"track01.bin"', '"track01.bin')), "cue_syntax", "unterminated quote")
        expect_error(with_cue("time", one.replace("00:00:00", "00:61:00")), "cue_syntax", "seconds out of range")
        d = variant_dir("rawsync", raw)
        blob = bytearray((d / "track01.bin").read_bytes())
        blob[2352 * 20 + 5] = 0x12
        (d / "track01.bin").write_bytes(blob)
        expect_error(d / "disc.cue", "raw_sync", "bad raw sync pattern")
        d = variant_dir("rawheader", raw)
        blob = bytearray((d / "track01.bin").read_bytes())
        blob[2352 * 21 + 14] ^= 0x01
        (d / "track01.bin").write_bytes(blob)
        expect_error(d / "disc.cue", "raw_header", "raw header MSF disagreeing with its LBA")
        d = variant_dir("rawmode", raw)
        blob = bytearray((d / "track01.bin").read_bytes())
        blob[2352 * 22 + 15] = 2
        (d / "track01.bin").write_bytes(blob)
        expect_error(d / "disc.cue", "raw_header", "raw header mode 2 in a MODE1 track")
        d = variant_dir("conflict")
        blob = bytearray((d / "track01.bin").read_bytes())
        blob[:12] = oracle.SYNC
        (d / "track01.bin").write_bytes(blob)
        expect_error(d / "disc.cue", "header_conflict", "cooked track whose data starts with a raw sync (conflicting header)")

        def boot_patch(name, patch):
            directory = variant_dir("boot_" + name)
            blob = bytearray((directory / "track01.bin").read_bytes())
            patch(blob)
            (directory / "track01.bin").write_bytes(blob)
            return directory / "disc.cue"

        expect_error(boot_patch("id", lambda b: b.__setitem__(slice(0, 4), b"XEGA")), "system_id", "wrong system ID", "boot")
        expect_error(boot_patch("ip_zero", lambda b: struct.pack_into(">I", b, 0x34, 0)), "descriptor_zero_size",
                     "IP load size zero", "boot")
        expect_error(boot_patch("sp_beyond", lambda b: struct.pack_into(">I", b, 0x44, 0x8000)), "descriptor_outside",
                     "SP extent beyond the system area", "boot")
        expect_error(boot_patch("ip_header", lambda b: struct.pack_into(">I", b, 0x30, 0x100)), "descriptor_outside",
                     "IP extent overlapping the header", "boot")
        expect_error(boot_patch("overlap", lambda b: struct.pack_into(">I", b, 0x40, 0x280)), "descriptor_overlap",
                     "IP and SP extents overlapping", "boot")
        expect_error(boot_patch("odd", lambda b: struct.pack_into(">I", b, 0x40, 0x801)), "descriptor_odd",
                     "odd SP disc address", "boot")
        expect_error(boot_patch("entry", lambda b: struct.pack_into(">I", b, 0x38, 0x4000)), "entry_outside",
                     "IP entry offset beyond the loaded bytes", "boot")
        d = tmp / "audio_first"
        d.mkdir()
        (d / "a.bin").write_bytes(bytes(2352 * 20))
        (d / "disc.cue").write_text('FILE "a.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n')
        expect_error(d / "disc.cue", "first_track_not_data", "audio first track", "boot")

    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_disc_ingestion_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
