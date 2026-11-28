#!/usr/bin/env python3
"""SEG-148-T002 (ADR 0287 / 0328): verified, layout-preserving Sega CD disc edits on synthetic discs (tools/sega_cd_fixture_disc.py).

`mod apply --cue` asserts the disc identity and every original-range digest before one byte is written, writes a new layout-identical image set (originals only read), and the existing Sega CD
build consumes it under its new identity: the patched `cdframe` disc (cooked and raw) emits, compiles as strict C11 and boots to the frame the independent oracle computes from the PATCHED
sectors, and the Vanilla program refuses it. `mod verify-disc` carries the layout-rejection vectors of sega-cd-mod-sources.md section 4. The independent oracles (tests/sega_cd_disc_oracle.py,
tests/sega_cd_iso9660_oracle.py) share no code with the C++ reader.
usage: mod_disc_edit_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile
import zlib

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402
import sega_cd_disc_oracle as oracle  # noqa: E402
import sega_cd_frame_oracle as frames  # noqa: E402
import sega_cd_iso9660_oracle as iso  # noqa: E402
import sega_cd_fixture_disc as F  # noqa: E402

BUDGET = 3000000
BIOS_SHA = hashlib.sha256(b"synthetic-bios-requirement").hexdigest()
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def cli(*args):
    done = subprocess.run([SEGARECOMP, "mod", *map(str, args)], capture_output=True, text=True, timeout=600)
    try:
        return done.returncode, json.loads(done.stdout)
    except json.JSONDecodeError:
        return done.returncode, {"error": done.stdout + done.stderr}


def refused(result, klass, label):
    code, doc = result
    check(code == 1 and doc.get("ok") is False and doc.get("class") == klass, "%s -> %s (got %s %s %s)" % (label, klass, code, doc.get("class"), doc.get("error", "")[:200]))


def digests(directory):
    return {p.name: sha(p.read_bytes()) for p in pathlib.Path(directory).iterdir()}


class Projects:
    def __init__(self, tmp):
        self.tmp, self.n = tmp, 0

    def make(self, identity, edits, bios='requirement = "digest"\nsha256s = ["%s"]' % BIOS_SHA, extra=""):
        """edits: (payload bytes, {source fields}); `original_sha256` may be given as `original` bytes."""
        self.n += 1
        d = self.tmp / ("mod-%d" % self.n)
        (d / "disc").mkdir(parents=True)
        text = '[mod]\nid = "disc-data"\nversion = "1.0.0"\nname = "disc data"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "sega-cd"\ndisc_identity = "%s"\n\n[bios]\n%s\n' % (identity, bios)
        for i, (data, fields) in enumerate(edits):
            (d / "disc" / ("e%d.bin" % i)).write_bytes(data)
            fields = dict(fields)
            if "original" in fields:
                fields["original_sha256"] = sha(fields.pop("original"))
            text += '\n[[source]]\nkind = "disc-extent-edit"\npath = "disc/e%d.bin"\n' % i
            text += "".join("%s = %s\n" % (k, json.dumps(v)) for k, v in fields.items())
        (d / "mod.toml").write_text(text + extra, newline="\n")
        return d


def edit(data, start, original, target=0, **fields):
    return data, dict(target=target, start=start, original=original, **fields)


def bps(source, target):
    """A minimal BPS1 delta written here (TargetRead only), independent of the product writer."""
    def number(v):
        out = bytearray()
        while True:
            x = v & 0x7F
            v >>= 7
            if v == 0:
                out.append(0x80 | x)
                return bytes(out)
            out.append(x)
            v -= 1

    body = b"BPS1" + number(len(source)) + number(len(target)) + number(0) + number(((len(target) - 1) << 2) | 1) + target
    body += zlib.crc32(source).to_bytes(4, "little") + zlib.crc32(target).to_bytes(4, "little")
    return body + zlib.crc32(body).to_bytes(4, "little")


def user_offset(variant, lba, within=0):
    """(raw file offset) of byte `within` of the user data of track-1 sector `lba`."""
    return lba * (2352 if variant == "raw" else 2048) + (16 if variant == "raw" else 0) + within


def cdframe(tmp, builder, projects):
    for variant in ("cooked", "raw"):
        disc, exe, expected, _ = builder.program("cdframe_" + variant, "cdframe", variant)
        cue = disc / "disc.cue"
        original = oracle.read_disc(cue)
        before = digests(disc)
        track = (disc / "track01.bin").read_bytes()
        at = user_offset(variant, expected["data_lba"], expected["palette_bytes"] + 32)   # the first non-transparent tile
        old = track[at:at + 32]
        new = bytes(b ^ 0x5A for b in old)
        mod = projects.make(original["identity"], [edit(new, at, old)])
        code, doc = cli("check", mod)
        check(code == 0 and doc.get("trust_class") == "data" and doc.get("source_identity_kind") == "disc-image", variant + ": a disc-extent-edit project is a data-only sega-cd package: %s" % doc.get("error", ""))
        package = tmp / (variant + ".segapatch")
        check(cli("pack", mod, "--out", package)[0] == 0 and cli("inspect", package)[0] == 0, variant + ": it packs and the package re-reads")
        out = tmp / ("patched-" + variant)
        code, doc = cli("apply", package, "--cue", cue, "--out", out)
        check(code == 0 and doc.get("edits_applied") == 1 and doc.get("code_checked") is False, "%s: the extent edit applies: %s" % (variant, doc))
        patched = oracle.read_disc(out / "disc.cue")
        check(patched["identity"] == doc.get("effective_disc_identity") != original["identity"] == doc.get("original_disc_identity"), variant + ": the effective disc identity changes (independent oracle)")
        data = (out / "track01.bin").read_bytes()
        check(data == track[:at] + new + track[at + 32:] and (out / "track02.bin").read_bytes() == (disc / "track02.bin").read_bytes() and (out / "disc.cue").read_bytes() == cue.read_bytes(),
              variant + ": only the declared bytes differ; the other FILE and the CUE are identical")
        check(digests(disc) == before, variant + ": the original disc is only read")
        again = tmp / ("again-" + variant)
        code, doc2 = cli("apply", mod, "--cue", cue, "--out", again)
        check(code == 0 and digests(again) == digests(out) and doc2["effective_disc_identity"] == doc["effective_disc_identity"], variant + ": two runs (project and package) are bit-for-bit identical")
        code, doc3 = cli("verify-disc", package, "--cue", cue, "--patched", out / "disc.cue")
        check(code == 0 and doc3.get("effective_disc_identity") == patched["identity"], variant + ": verify-disc accepts the patched disc")

        # The patched disc builds with the existing Sega CD build and boots to the fixture's expected state.
        c = tmp / ("patched-%s.c" % variant)
        code, report = builder.emit(out / "disc.cue", c)
        check(code == 0 and report.get("ok") is True, "%s: the existing build consumes the patched disc: %s" % (variant, report.get("error", "")))
        program = tmp / ("patched-%s-program%s" % (variant, common.EXE))
        builder.compile(c, program)
        frame = tmp / (variant + "-patched.ppm")
        code, run = common.run(program, out, BUDGET, frame=frame)
        user = patched["_user_block"](expected["data_lba"]) + patched["_user_block"](expected["data_lba"] + 1)
        _, base = common.run(exe, disc, BUDGET)
        check(code == 0 and run["result"] == "idle" and common.result_words(run, 4) == expected["result_words"], variant + ": it boots to the fixture's expected end state")
        _, _, pixels = frames.read_ppm(frame)
        check(pixels == frames.expected_ppm_pixels(user) and run["frame_sha256"] != base["frame_sha256"], variant + ": its frame is the oracle's frame of the PATCHED sectors, not the original one")
        check(run["disc_identity"] == patched["identity"], variant + ": the program embeds the effective disc identity")
        code, refusal = common.run(exe, out, BUDGET)
        check(code == 20 and refusal.get("error") == "disc_identity_mismatch", variant + ": the Vanilla program refuses the patched disc")
        code, refusal = common.run(program, disc, BUDGET)
        check(code == 20 and refusal.get("error") == "disc_identity_mismatch", variant + ": the Patched program refuses the original disc")

        # A BPS delta over the digest-verified original range gives the same disc.
        delta = projects.make(original["identity"], [edit(bps(old, new), at, old, replacement="bps", length=32)])
        code, doc4 = cli("apply", delta, "--cue", cue, "--out", tmp / ("bps-" + variant))
        check(code == 0 and doc4.get("effective_disc_identity") == patched["identity"], "%s: a BPS replacement yields the same effective disc: %s" % (variant, doc4))


def rejections(tmp, projects):
    disc = tmp / "base"
    expected = common.build_disc(disc, "cdframe", "raw")
    cue = disc / "disc.cue"
    identity = oracle.read_disc(cue)["identity"]
    track, audio = (disc / "track01.bin").read_bytes(), (disc / "track02.bin").read_bytes()
    at = user_offset("raw", expected["data_lba"])
    old = track[at:at + 4]
    good = edit(b"\x00\x01\x02\x03", at, old)
    n = [0]

    def apply(mod, label, klass, source=cue):
        n[0] += 1
        out = tmp / ("r%d" % n[0])
        refused(cli("apply", mod, "--cue", source, "--out", out), klass, label)
        check(not out.exists() and not list(tmp.glob("r%d.partial*" % n[0])), label + ": no output or partial folder is left")

    apply(projects.make(identity, [edit(good[0], at, b"zzzz")]), "an expected-value (original digest) mismatch fails closed", "E_DISC_ORIGINAL_MISMATCH")
    apply(projects.make(identity, [edit(b"\x11" * 4, at + 100, track[at + 100:at + 104]), edit(good[0], at, b"zzzz")]), "one mismatching edit refuses the whole package", "E_DISC_ORIGINAL_MISMATCH")
    apply(projects.make("0" * 64, [good]), "another disc", "E_DISC_IDENTITY")
    apply(projects.make(identity, [edit(b"abcd", len(track) - 2, track[-2:])]), "a range past the end of the file (out of range)", "E_DISC_RANGE")
    apply(projects.make(identity, [edit(b"abcd", 0, track[:4], target=7)]), "a FILE index the disc does not have", "E_DISC_RANGE")
    apply(projects.make(identity, [edit(b"abcd", at, old, track=2)]), "a track outside the target FILE", "E_DISC_RANGE")
    refused(cli("check", projects.make(identity, [good, edit(b"ab", at + 2, track[at + 2:at + 4])])), "E_DISC_OVERLAP", "two overlapping edits")
    apply(projects.make(identity, [edit(b"abcd", 0, audio[:4], target=1)]), "an audio track target", "E_DISC_AUDIO_TARGET")
    apply(projects.make(identity, [edit(b"\x55" * 4, 12, track[12:16])]), "a raw sector header change", "E_DISC_SECTOR_HEADER")
    refused(cli("check", projects.make(identity, [edit(b"abcde", at, old, length=4)])), "E_DISC_PAYLOAD_SIZE", "a payload that is not exactly `length` bytes (layout-changing edit)")
    apply(projects.make(identity, [edit(bps(old, b"12345678"), at, old, replacement="bps", length=4)]), "a delta whose target changes the size (layout-changing edit)", "E_DISC_PAYLOAD_SIZE")
    apply(projects.make(identity, [edit(b"not a bps delta, never", at, old, replacement="bps", length=4)]), "a malformed delta", "E_DISC_DELTA")
    refused(cli("apply", projects.make(identity, [good]), "--cue", cue, "--out", disc / "inside"), "E_DISC_OUTPUT", "an output inside the original disc's folder")
    refused(cli("apply", projects.make(identity, [good]), "--cue", cue, "--out", disc), "E_DISC_OUTPUT", "the original disc's folder as the output")
    busy = tmp / "busy"
    busy.mkdir()
    (busy / "keep.txt").write_text("keep")
    refused(cli("apply", projects.make(identity, [good]), "--cue", cue, "--out", busy), "E_DISC_OUTPUT", "a non-empty output folder")
    check((busy / "keep.txt").read_text() == "keep", "the non-empty folder is untouched")
    bios = tmp / "bios.bin"
    rom = bytearray(0x400)
    rom[0:8] = (0x00FFFE00).to_bytes(4, "big") + (0x200).to_bytes(4, "big")
    rom[0x100:0x104] = b"SEGA"
    bios.write_bytes(bytes(rom))
    refused(cli("apply", projects.make(identity, [good]), "--cue", cue, "--bios", bios, "--out", tmp / "o-bios"), "E_DISC_BIOS", "a BIOS outside the declared digests")
    refused(cli("apply", projects.make(identity, [good], bios='requirement = "class"\nclass = "sega_cd_model1_v110_us"'), "--cue", cue, "--bios", bios, "--out", tmp / "o-class"),
            "E_DISC_BIOS", "a BIOS that is not the declared profile class")
    code, doc = cli("apply", projects.make(identity, [good], bios='requirement = "digest"\nsha256s = ["%s"]' % sha(bytes(rom))), "--cue", cue, "--bios", bios, "--out", tmp / "o-biosok")
    check(code == 0, "a BIOS matching a declared digest is accepted: %s" % doc)
    refused(cli("check", projects.make(identity, [good], bios='requirement = "digest"\nsha256s = []')), "E_SOURCE_DIGEST", "an empty BIOS digest list")
    refused(cli("check", projects.make(identity, [good], bios='requirement = "file"')), "E_FIELD_VALUE", "an unknown BIOS requirement")
    refused(cli("check", projects.make(identity, [good], extra='\n[[source]]\nkind = "luau-source"\npath = "x.luau"\n')), "E_ARCHIVE_ENTRY", "a missing source file")

    # ---- layout vectors (sega-cd-mod-sources.md section 4), on a three-track disc: the audio FILE holds two audio tracks of different lengths ----
    base3 = tmp / "base3"
    shutil.copytree(disc, base3)
    cue3 = ('FILE "track01.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
            'FILE "track02.bin" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n  TRACK 03 AUDIO\n    INDEX 00 00:00:20\n    INDEX 01 00:00:30\n')
    (base3 / "disc.cue").write_text(cue3, newline="\n")
    identity3 = oracle.read_disc(base3 / "disc.cue")["identity"]
    mod = projects.make(identity3, [good])
    ok_dir = tmp / "ok3"
    code, doc = cli("apply", mod, "--cue", base3 / "disc.cue", "--out", ok_dir)
    check(code == 0, "the three-track disc (with an INDEX 00 pregap) is patched: %s" % doc)
    apply(projects.make(identity3, [edit(b"abcd", 22 * 2352, audio[22 * 2352:22 * 2352 + 4], target=1)]), "an edit of the INDEX 00 pregap", "E_DISC_PREGAP", base3 / "disc.cue")

    def vector(klass, label, cue_text=None, files=None):
        n[0] += 1
        d = tmp / ("v%d" % n[0])
        shutil.copytree(ok_dir, d)
        if cue_text is not None:
            (d / "disc.cue").write_text(cue_text, newline="\n")
        for name, blob in (files or {}).items():
            (d / name).write_bytes(blob)
        refused(cli("verify-disc", mod, "--cue", base3 / "disc.cue", "--patched", d / "disc.cue"), klass, label)

    vector("E_DISC_FILE_SIZE", "a changed file size", files={"track02.bin": audio + bytes(2352)})
    vector("E_DISC_TRACK_COUNT", "an added track", cue_text=cue3 + "  TRACK 04 AUDIO\n    INDEX 01 00:00:40\n")
    vector("E_DISC_TRACK_COUNT", "a removed track", cue_text=cue3.split("  TRACK 03")[0])
    sectors = len(audio) // 2352
    swapped = cue3.replace("INDEX 00 00:00:20\n    INDEX 01 00:00:30", "INDEX 00 00:00:%02d\n    INDEX 01 00:00:%02d" % (sectors - 30, sectors - 20))
    vector("E_DISC_TRACK_ORDER", "reordered tracks (their lengths exchanged)", cue_text=swapped)
    vector("E_DISC_SECTOR_SIZE", "a changed track mode", cue_text=cue3.replace("TRACK 01 MODE1/2352", "TRACK 01 AUDIO"))
    vector("E_DISC_TRACK_LENGTH", "a changed track length", cue_text=cue3.replace("INDEX 00 00:00:20\n    INDEX 01 00:00:30", "INDEX 00 00:00:21\n    INDEX 01 00:00:31"))
    vector("E_DISC_PREGAP", "a changed PREGAP", cue_text=cue3.replace("PREGAP 00:02:00", "PREGAP 00:03:00"))
    vector("E_DISC_PREGAP", "a changed INDEX 00", cue_text=cue3.replace("INDEX 00 00:00:20", "INDEX 00 00:00:19"))
    vector("E_DISC_INDEX_UNSUPPORTED", "INDEX 02", cue_text=cue3 + "    INDEX 02 00:00:50\n")
    blob = bytearray((ok_dir / "track02.bin").read_bytes())
    blob[5] ^= 1
    vector("E_DISC_UNDECLARED_CHANGE", "a change outside the declared ranges", files={"track02.bin": bytes(blob)})
    blob = bytearray((ok_dir / "track01.bin").read_bytes())
    blob[at] ^= 0x80
    vector("E_DISC_UNDECLARED_CHANGE", "other bytes inside a declared range", files={"track01.bin": bytes(blob)})


def iso_files(tmp, projects):
    """An ISO9660 file of several blocks: addressed by name, and on a raw disc its logical range crosses sector headers and EDC bytes."""
    content = random.Random(148).randbytes(5000)
    files = F.module_files()
    files = {name: (block, data) for name, (block, data) in files.items()}
    files["README.TXT;1"] = (30, content)
    sectors = dict(F.iso_volume(files))
    for name, (block, data) in files.items():
        for k in range((len(data) + 2047) // 2048):
            sectors[block + k] = data[2048 * k:2048 * (k + 1)].ljust(2048, b"\0")
    area, _ = F.system_area(bytes(64), bytes(64), "ISOEDIT")
    for variant in ("cooked", "raw"):
        users = [area[2048 * i:2048 * (i + 1)] if i < 16 else sectors.get(i, bytes([i & 0xFF]) * 2048) for i in range(F.TRACK1_SECTORS)]
        d = tmp / ("iso-" + variant)
        d.mkdir()
        (d / "track01.bin").write_bytes(b"".join(F.raw_sector(i, u) if variant == "raw" else u for i, u in enumerate(users)))
        (d / "disc.cue").write_text('FILE "track01.bin" BINARY\n  TRACK 01 %s\n    INDEX 01 00:00:00\n' % ("MODE1/2352" if variant == "raw" else "MODE1/2048"), newline="\n")
        disc = oracle.read_disc(d / "disc.cue")
        new = bytes(range(256)) * 2
        start = 1900   # crosses from the file's first block into its second
        mod = projects.make(disc["identity"], [edit(new, start, content[start:start + len(new)], iso_file="/README.TXT;1")])
        out = tmp / ("iso-out-" + variant)
        code, doc = cli("apply", mod, "--cue", d / "disc.cue", "--out", out)
        check(code == 0, "%s: an edit named by ISO9660 file and offset applies: %s" % (variant, doc))
        if code != 0:
            continue
        patched = oracle.read_disc(out / "disc.cue")
        entry = {e["path"]: e for e in iso.tree(lambda b: patched["_user_block"](b))["entries"]}["/README.TXT;1"]
        got = b"".join(patched["_user_block"](entry["block"] + k) for k in range(3))[:entry["size"]]
        check(entry["size"] == len(content) and got == content[:start] + new + content[start + len(new):], variant + ": the file holds exactly the edit (independent ISO9660 reader); its record is unchanged")
        before, after = (d / "track01.bin").read_bytes(), (out / "track01.bin").read_bytes()
        changed = [i for i in range(len(before)) if before[i] != after[i]]
        size = 2352 if variant == "raw" else 2048
        header = 16 if variant == "raw" else 0
        check(len(changed) > 0 and all(header <= i % size < header + 2048 and i // size in (30, 31) for i in changed), variant + ": only user bytes of the file's sectors differ (no header or EDC/ECC byte)")
        for name, klass, fields in (("past the file size (growing a file is layout-changing)", "E_DISC_RANGE", dict(start=len(content) - 4, iso_file="/README.TXT;1")),
                                    ("a file the volume does not have", "E_DISC_ISO_FILE", dict(start=0, iso_file="/MISSING.BIN;1"))):
            bad = projects.make(disc["identity"], [(b"x" * 8, dict(target=0, original=bytes(8), **fields))])
            refused(cli("apply", bad, "--cue", d / "disc.cue", "--out", tmp / ("iso-bad-" + variant + klass)), klass, variant + ": " + name)


def packages(tmp, projects):
    """The disc size model: an archive over 32 MiB is legal only when every payload is a disc kind; the cartridge commands refuse a disc package by name."""
    identity = "1" * 64
    big = bytes(33 << 20)
    only = projects.make(identity, [(big, dict(target=0, start=0, original_sha256=sha(big)))])
    package = tmp / "big.segapatch"
    code, doc = cli("pack", only, "--out", package)
    check(code == 0 and package.stat().st_size > (32 << 20) and cli("inspect", package)[0] == 0, "a disc-only package over 32 MiB packs and re-reads: %s" % doc.get("error", ""))
    mixed = projects.make(identity, [(big, dict(target=0, start=0, original_sha256=sha(big)))], extra='\n[[source]]\nkind = "guest-asm"\npath = "main.asm"\ncpu = "m68k"\n')
    (mixed / "main.asm").write_text("free * 0x4000000 0x10 0x00\norg * 0x4000000 at 0xff0000\n  rts\n", newline="\n")
    code, doc = cli("check", mixed)
    check(code == 1 and doc.get("class") == "E_ARCHIVE_SIZE", "with any other payload kind the same size is refused: %s" % doc)
    small = projects.make(identity, [(b"ab", dict(target=0, start=0, original_sha256=sha(b"cd")))])
    cart = tmp / "cart.bin"
    cart.write_bytes(bytes(512))
    for sub, extra in (("bps", ["--out", tmp / "x.bps"]), ("apply", ["--out", tmp / "x.bin"]), ("session", [])):
        refused(cli(sub, small, "--rom", cart, *extra), "E_SOURCE_PLATFORM", "`mod %s --rom` on a disc package" % sub)
    check(cli("apply", small, "--cue", tmp / "missing.cue", "--out", tmp / "o-missing")[1].get("class") == "E_DISC_LOAD", "an unreadable disc is E_DISC_LOAD")


def main():
    with tempfile.TemporaryDirectory(prefix="scd-moddisc-") as tmp:
        tmp = pathlib.Path(tmp)
        projects = Projects(tmp)
        cdframe(tmp, common.Builder(SEGARECOMP, CC, tmp), projects)
        rejections(tmp, projects)
        iso_files(tmp, projects)
        packages(tmp, projects)
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
