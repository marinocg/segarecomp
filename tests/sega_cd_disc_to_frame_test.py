#!/usr/bin/env python3
"""SEG-062-T005 (Checkpoint C): real disc sectors -> CDD/CDC -> DMA into Word RAM -> main CPU -> Genesis VDP -> frame.

Pipeline under test (all from checked-in generators): synthetic CUE/BIN -> build-time IP/SP extraction -> static recompilation of
both MC68000 programs -> strict-C11 executable -> the sub CPU drives the CDD (read command at LBA 24) and the CDC (decode into the
buffer, DMA destination 7) -> the external disc is read through the runtime sector reader -> Word RAM is handed to the main CPU ->
the main CPU programs CRAM, tiles and plane A -> the VDP frame is exported by the runtime. Checked:
  * the PPM frame equals, pixel for pixel, the frame computed independently from the oracle-read sectors (tests/
    sega_cd_frame_oracle.py), and its digest equals the committed project-authored expectation;
  * the Word RAM payload, VRAM, CRAM and CDC block headers equal independently derived values (sector payload = oracle digest);
  * two runs and several deadline partitions give identical reports (frame, device log, digests), cooked and raw discs alike;
  * liveness: a disc with one changed data byte, rebuilt, renders a different frame; the old program refuses that disc at start-up;
  * typed failures: audio track reached (pregap, audio LBA, reading past the data track), unsupported DMA destination, missing /
    truncated / altered disc files; an aborted DMA keeps its partial progress (no rollback, no EDT);
  * defined (GPGX) results: playing past the disc ends in drive status END, the CDD checksum is never inspected, CTRL1 MODRQ is
    accepted;
  * no data-sector byte sequence appears in the generated C.

usage: sega_cd_disc_to_frame_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import pathlib
import re
import shutil
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402
import sega_cd_disc_oracle as oracle  # noqa: E402
import sega_cd_frame_oracle as frames  # noqa: E402

EXPECTED = json.loads((ROOT / "tests" / "fixtures" / "sega-cd-frame-expected.json").read_text())
BUDGET = 3000000
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def payload(disc_dir):
    disc = oracle.read_disc(pathlib.Path(disc_dir) / "disc.cue")
    return disc["_user_block"](24) + disc["_user_block"](25)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-frame-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        reports = {}
        for variant in ("cooked", "raw"):
            disc, exe, expected, emitted = builder.program("cdframe_" + variant, "cdframe", variant)
            frame = tmp / ("frame_" + variant + ".ppm")
            code, report = common.run(exe, disc, BUDGET, frame=frame)
            reports[variant] = report
            data = payload(disc)
            check(code == 0 and report["result"] == "idle", "%s: run ends idle (exit %d, %s, stop %s)" % (
                variant, code, report.get("result"), report.get("stop", {}).get("class")))
            check(hashlib.sha256(data).hexdigest() == expected["data_sha256"], variant + ": oracle payload equals the builder's data")
            check(report["cdd"]["delivered"] == 2 and report["cdc"]["edt"] == 1 and report["cdc"]["dma_total"] == 2048,
                  variant + ": two sectors delivered by the drive; the last DMA completed (EDT)")
            check(common.result_words(report, 4) == expected["result_words"],
                  variant + ": CDC block headers seen by the sub are BCD MSF 00:02:24 / 00:02:25 mode 1")
            word_ram = data + bytes(0x40000 - len(data))
            check(report["digests"]["word_ram"] == hashlib.sha256(word_ram).hexdigest(),
                  variant + ": Word RAM holds exactly the oracle's sector payload (sub DMA, independent digest)")
            check(report["cram_words"] == frames.expected_cram_hex(data) and report["digests"]["vram"] == frames.expected_vram_sha256(data),
                  variant + ": CRAM and VRAM equal the values derived from the payload")
            width, height, pixels = frames.read_ppm(frame)
            check((width, height) == (320, 224) and pixels == frames.expected_ppm_pixels(data),
                  variant + ": the runtime frame equals the independently rendered frame pixel for pixel")
            check(report["frame_sha256"] == EXPECTED["cdframe"]["frame_sha256"],
                  variant + ": frame digest equals the committed expectation %s" % EXPECTED["cdframe"]["frame_sha256"][:16])
            check(hashlib.sha256(frame.read_bytes()).hexdigest() == EXPECTED["cdframe"]["ppm_sha256"],
                  variant + ": PPM file digest equals the committed expectation")
            source = (tmp / ("cdframe_" + variant) / "program.c").read_text()
            arrays = b"".join(bytes(int(v, 16) for v in re.findall(r"0x([0-9A-F]{2})\b", line))
                              for line in source.splitlines() if "_bytes[] = {" in line)
            leaked = any(data[i:i + 16] in arrays for i in range(0, len(data) - 16, 16))
            check(not leaked and len(arrays) > 0, variant + ": no data-sector byte sequence is embedded in the generated C")
        check({k: v for k, v in reports["cooked"].items() if k not in ("program_identity", "disc_identity")} ==
              {k: v for k, v in reports["raw"].items() if k not in ("program_identity", "disc_identity")},
              "cooked and raw discs give identical machine state, device log and frame")

        disc, exe = tmp / "cdframe_cooked" / "disc", tmp / "cdframe_cooked" / ("program" + common.EXE)
        base = reports["cooked"]
        check(common.run(exe, disc, BUDGET)[1] == base, "two process runs give identical reports")
        partitions = [["--slice-ticks", "999983"], ["--slice-ticks", "357954500000"], ["--slice-ticks", "15034089"],
                      ["--slice-seed", "21"], ["--slice-seed", "22"], ["--slice-seed", "23"]]
        same = [common.run(exe, disc, BUDGET, *p)[1] == base for p in partitions]
        check(all(same), "identical frame, device log and state for %d deadline partitions (%s)" % (len(partitions), same))

        # Liveness and integrity.
        flipped_disc, flipped_exe, _, _ = builder.program("cdframe_flip", "cdframe", "cooked", "--data-flip", "40")
        code, flipped = common.run(flipped_exe, flipped_disc, BUDGET, frame=tmp / "flip.ppm")
        check(code == 0 and flipped["frame_sha256"] != base["frame_sha256"] and flipped["disc_identity"] != base["disc_identity"],
              "a disc with one changed data byte, rebuilt with its own identity, renders a different frame")
        _, _, flip_pixels = frames.read_ppm(tmp / "flip.ppm")
        check(flip_pixels == frames.expected_ppm_pixels(payload(flipped_disc)), "the changed frame equals the oracle's for the new data")
        code, refused = common.run(exe, flipped_disc, BUDGET)
        check(code == 20 and refused.get("error") == "disc_identity_mismatch",
              "the unchanged program refuses the altered disc at start-up (disc_identity_mismatch)")
        broken = tmp / "truncated"
        shutil.copytree(disc, broken)
        blob = (broken / "track01.bin").read_bytes()
        (broken / "track01.bin").write_bytes(blob[:-2048])
        code, refused = common.run(exe, broken, BUDGET)
        check(code == 20 and refused.get("error") == "disc_size_mismatch", "a truncated BIN is refused at start-up")
        (broken / "track01.bin").unlink()
        code, refused = common.run(exe, broken, BUDGET)
        check(code == 20 and refused.get("error") == "disc_file_missing", "a missing BIN is refused at start-up")

        negatives = [
            ("lba_pregap", ["--cd-lba", "45"], "cd_read_audio_track"),
            ("lba_audio", ["--cd-lba", "200"], "cd_read_audio_track"),
            ("dma_prg_ram", ["--dma-dest", "5"], "cdc_destination_unsupported"),
            ("dma_host", ["--dma-dest", "3"], "cdc_destination_unsupported"),
            ("read_past_track", ["--cd-lba", "39"], "cd_read_audio_track"),
        ]
        for name, options, stop in negatives:
            ndisc, nexe, _, _ = builder.program("neg_" + name, "cdframe", "cooked", *options)
            code, report = common.run(nexe, ndisc, BUDGET)
            check(code == 10 and report["stop"]["class"] == stop and report["stop"]["cpu"] == 1,
                  "%s: typed stop %s (got %s)" % (name, stop, report.get("stop", {}).get("class")))

        # Defined GPGX results instead of stops.
        ndisc, nexe, _, _ = builder.program("past_disc", "cdframe", "cooked", "--cd-lba", "300")
        code, report = common.run(nexe, ndisc, 400000)
        check(code == 11 and report["cdd"]["status"] == 0x0C and report["cdd"]["delivered"] == 0 and report["cdc"]["edt"] == 0,
              "play past the last track: drive status END, no block delivered, the polling program ends on its budget (exit %d)" % code)
        for name, options in (("checksum", ["--bad-checksum"]), ("modrq", ["--bad-cdc-register"])):
            ndisc, nexe, _, _ = builder.program("variant_" + name, "cdframe", "cooked", *options)
            code, report = common.run(nexe, ndisc, BUDGET)
            check(code == 0 and report["frame_sha256"] == base["frame_sha256"] and report["digests"]["word_ram"] == base["digests"]["word_ram"],
                  "%s variant: the same frame (exit %d)" % (name, code))
            if name == "checksum":
                check(report["cdd"]["tx"][8:] == [0, 0], "a wrong TX9 checksum is never inspected: the command word write clears TX8/TX9")
            else:
                check(report["cdc"]["ctrl1"] == 0x08 and report["cdc"]["stat2"] == 0x08,
                      "CTRL1 MODRQ is accepted (STAT2 follows it); Mode 1 data blocks are unaffected")

        adisc, aexe, _, _ = builder.program("abort_dma", "cdframe", "cooked", "--abort-dma")
        code, report = common.run(aexe, adisc, BUDGET)
        done = report["cdc"]["dma_done"]
        data = payload(adisc)
        check(code == 0 and report["cdc"]["edt"] == 0 and report["cdc"]["dma_active"] == 0 and 0 < done < 2048 and done % 8 == 0,
              "an aborted DMA stops with partial progress (%d of 2048 bytes moved, 8-byte groups, no EDT)" % done)
        check(report["digests"]["word_ram"] == hashlib.sha256(data[:done] + bytes(0x40000 - done)).hexdigest(),
              "the bytes moved before the abort stay in Word RAM (no rollback), nothing after them")
        check((report["ga"]["comstat"][7] >> 8) & 0x80 == 0, "the sub observed EDT = 0 after the abort")
    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_disc_to_frame_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
