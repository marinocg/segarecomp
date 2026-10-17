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


def frames(sectors):
    return "%02d:%02d:%02d" % (sectors // 4500, sectors // 75 % 60, sectors % 75)


def raw_mode1(lba, fill):
    msf = lba + 150
    bcd = lambda v: ((v // 10) << 4) | (v % 10)  # noqa: E731
    return oracle.SYNC + bytes([bcd(msf // 4500), bcd(msf // 75 % 60), bcd(msf % 75), 1]) + bytes([fill]) * 2048 + bytes(288)


def audio(sectors, seed):
    return bytes((i * 7 + seed) & 0xFF for i in range(sectors * 2352))


def index00_cases(tmp, raw):
    """SEG-063-T003: INDEX 00 (in-file pregap) next to INDEX 01 and PREGAP. Every expected layout below is a hand-derived literal
    (CUE sheet semantics: a stored pregap is addressable disc time before INDEX 01, owned by no track's program area, and the
    previous track ends where it begins), checked against the C++ reader AND the independent oracle."""
    data = (raw / "track01.bin").read_bytes()
    D = len(data) // 2352
    check(D == 40, "INDEX 00 base: the raw fixture data track has 40 sectors")
    head = 'FILE "%s" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'

    def disc(name, files, cue):
        directory = tmp / ("idx_" + name)
        directory.mkdir()
        for file_name, blob in files.items():
            (directory / file_name).write_bytes(blob)
        (directory / "disc.cue").write_text(cue)
        return directory / "disc.cue"

    def layout(cue, label, tracks, total):
        got = compare(cue, label)
        if not got:
            return None
        want = [{"number": n, "type": t, "file": f, "file_offset": o, "sector_size": 2352, "pregap": p, "index0": g, "first_lba": l,
                 "sector_count": c} for n, t, f, o, p, g, l, c in tracks]
        check(got["tracks"] == want, label + ": track table equals the hand-derived layout")
        check(got["total_sectors"] == total, label + ": total %d sectors including every pregap" % total)
        readable = sorted(int(k) for k in got["sector_sha256"])
        check(readable == [l + s for n, t, f, o, p, g, l, c in tracks if t != "audio" for s in range(c)],
              label + ": exactly the data tracks' program sectors are readable (no pregap sector)")
        return got

    # 1. Two tracks in one BIN, INDEX 01 only (unchanged behaviour).
    a = layout(disc("one_bin", {"disc.bin": data + audio(75, 1)}, head % "disc.bin" + "  TRACK 02 AUDIO\n    INDEX 01 %s\n" % frames(D)),
               "one BIN, INDEX 01 only", [(1, "mode1_2352", 0, 0, 0, 0, 0, 40), (2, "audio", 0, 40 * 2352, 0, 0, 40, 75)], 115)
    # 2. One BIN, audio tracks with stored INDEX 00 pregaps (the redump layout): data 40 | gap 30 | audio 50 | gap 10 | audio 20.
    one_bin = data + audio(30, 2) + audio(50, 3) + audio(10, 4) + audio(20, 5)
    b_cue = head % "disc.bin" + ("  TRACK 02 AUDIO\n    INDEX 00 00:00:40\n    INDEX 01 00:00:70\n"
                                 "  TRACK 03 AUDIO\n    INDEX 00 00:01:45\n    INDEX 01 00:01:55\n")
    b = layout(disc("one_bin_index0", {"disc.bin": one_bin}, b_cue), "one BIN, INDEX 00 pregaps",
               [(1, "mode1_2352", 0, 0, 0, 0, 0, 40), (2, "audio", 0, 70 * 2352, 0, 30, 70, 50),
                (3, "audio", 0, 130 * 2352, 0, 10, 130, 20)], 150)
    # 3. Separate BINs, each audio track with a stored pregap at the start of its own file.
    c = layout(disc("multi_index0", {"t1.bin": data, "t2.bin": audio(150 + 40, 6), "t3.bin": audio(75 + 30, 7)},
                    head % "t1.bin" + 'FILE "t2.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'
                    'FILE "t3.bin" BINARY\n  TRACK 03 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:01:00\n'),
               "separate BINs, INDEX 00 pregaps",
               [(1, "mode1_2352", 0, 0, 0, 0, 0, 40), (2, "audio", 1, 150 * 2352, 0, 150, 190, 40),
                (3, "audio", 2, 75 * 2352, 0, 75, 305, 30)], 335)
    # 4. A synthetic PREGAP (not stored) in a separate BIN.
    layout(disc("multi_pregap", {"t1.bin": data, "t2.bin": audio(75, 8)},
                head % "t1.bin" + 'FILE "t2.bin" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n'),
           "separate BINs, PREGAP", [(1, "mode1_2352", 0, 0, 0, 0, 0, 40), (2, "audio", 1, 0, 150, 0, 190, 75)], 265)
    # 5. A raw MODE1 track after a stored pregap: its sector headers carry the LBAs after the gap (data 40 | gap 20 | data 10).
    gap_data = data + audio(20, 9) + b"".join(raw_mode1(60 + s, s) for s in range(10))
    e_cue = head % "disc.bin" + "  TRACK 02 MODE1/2352\n    INDEX 00 00:00:40\n    INDEX 01 00:00:60\n"
    layout(disc("raw_after_gap", {"disc.bin": gap_data}, e_cue), "raw MODE1 track after a stored pregap",
           [(1, "mode1_2352", 0, 0, 0, 0, 0, 40), (2, "mode1_2352", 0, 60 * 2352, 0, 20, 60, 10)], 70)
    wrong = data + audio(20, 9) + b"".join(raw_mode1(40 + s, s) for s in range(10))
    expect_error(disc("raw_headers_ignore_gap", {"disc.bin": wrong}, e_cue), "raw_header",
                 "raw headers numbered as if the stored pregap took no disc time")
    # 6. Identity: a zero-length INDEX 00 is no pregap; a stored pregap is part of the layout identity.
    zero = disc("zero_gap", {"disc.bin": data + audio(75, 1)},
                head % "disc.bin" + "  TRACK 02 AUDIO\n    INDEX 00 %s\n    INDEX 01 %s\n" % (frames(D), frames(D)))
    code, out = inspect(zero)
    check(a is not None and code == 0 and json.loads(out)["identity"] == a["identity"],
          "a zero-length INDEX 00 keeps the INDEX 01-only identity")
    check(None not in (a, b, c) and len({a["identity"], b["identity"], c["identity"]}) == 3, "stored pregaps change the identity")

    # 7. Malformed index combinations and boundaries (all on the one-BIN layout of case 2).
    def bad(name, cue, error, label):
        expect_error(disc("neg_" + name, {"disc.bin": one_bin}, cue), error, label)

    t2 = "  TRACK 02 AUDIO\n"
    rest = "  TRACK 03 AUDIO\n    INDEX 01 00:01:55\n"
    bad("dup00", head % "disc.bin" + t2 + "    INDEX 00 00:00:40\n    INDEX 00 00:00:41\n    INDEX 01 00:00:70\n" + rest,
        "index_order", "duplicate INDEX 00")
    bad("dup01", head % "disc.bin" + t2 + "    INDEX 01 00:00:40\n    INDEX 01 00:00:70\n" + rest, "index_order", "duplicate INDEX 01")
    bad("late00", head % "disc.bin" + t2 + "    INDEX 01 00:00:70\n    INDEX 00 00:00:40\n" + rest, "index_order",
        "INDEX 00 after INDEX 01")
    bad("after01", head % "disc.bin" + t2 + "    INDEX 00 00:00:71\n    INDEX 01 00:00:70\n" + rest, "index_order",
        "INDEX 00 later than INDEX 01")
    bad("pregap_then00", head % "disc.bin" + t2 + "    PREGAP 00:02:00\n    INDEX 00 00:00:40\n    INDEX 01 00:00:70\n" + rest,
        "pregap_conflict", "PREGAP and INDEX 00 on one track")
    bad("00_then_pregap", head % "disc.bin" + t2 + "    INDEX 00 00:00:40\n    PREGAP 00:02:00\n    INDEX 01 00:00:70\n" + rest,
        "pregap_conflict", "INDEX 00 and then PREGAP on one track")
    bad("beyond", head % "disc.bin" + t2 + "    INDEX 00 00:02:10\n    INDEX 01 00:02:20\n", "file_truncated",
        "INDEX 00 / INDEX 01 beyond the end of the BIN")
    bad("into_prev", head % "disc.bin" + t2 + "    INDEX 00 00:00:00\n    INDEX 01 00:00:70\n" + rest, "track_layout",
        "INDEX 00 overlapping the previous track's program area")
    bad("no01", head % "disc.bin" + t2 + "    INDEX 00 00:00:40\n" + rest, "track_numbering", "INDEX 00 without INDEX 01")
    expect_error(disc("file_gap", {"t1.bin": data, "t2.bin": audio(190, 6)},
                      head % "t1.bin" + 'FILE "t2.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:10\n    INDEX 01 00:02:00\n'),
                 "track_layout", "a separate BIN whose first INDEX 00 is not at its start")
    expect_error(disc("mixed", {"disc.bin": data[:2048 * 40] + audio(10, 1)},
                      'FILE "disc.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'
                      "  TRACK 02 AUDIO\n    INDEX 00 00:00:30\n    INDEX 01 00:00:35\n"),
                 "mixed_sector_sizes", "cooked data and audio in one BIN")


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
        expect_error(with_cue("index0", one.replace("INDEX 01", "INDEX 00")), "cue_unsupported", "INDEX 00 on track 1")
        expect_error(with_cue("index2", one + "    INDEX 02 00:00:10\n"), "cue_unsupported", "INDEX 02")
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

        index00_cases(tmp, raw)

    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_disc_ingestion_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
