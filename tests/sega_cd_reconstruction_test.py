#!/usr/bin/env python3
"""SEG-111 (ADR 0255): build-time executable image reconstruction in the PRODUCT `segarecomp sega-cd build`, on project-authored fixtures.

No manual extent, per-title manifest, reference emulator or Python driver is involved: the build pass observes bounded RAM write streams, the build driver resolves them at
the existing frontier and admits the result through the existing machinery (`extent:` for a proven copy, `produced:` for bytes a verified producer computed).

Relocation (Class A)
  relocated            a loaded module copies a 24-byte fragment (below the exact-copy resolver's floors) to PRG RAM and runs it: explained automatically, one extra round,
                       the fragment runs natively, repeated builds / other job counts give the identical program identity
  relocated_short      incomplete output: the produced prefix is admitted; the program stops, typed, at the first byte never produced
  relocated_duplicate  the fragment occurs twice on the disc: the causal read, not the byte pattern, selects the source
  relocated_patched    rewritten after the copy: refused (stream_overwritten); the fragment never runs
  relocated_main       the main processor copies a fragment of its Word RAM module into main Work RAM (the other supported RAM class)
  relocated_staged     copied through a data register (no causal read): equal bytes are NOT a relocation extent (they are, at most, a produced image)
  derivation matrix    the evidence of the frontier pass is mutated one fact at a time: each is a typed refusal
Produced code (Class B)
  decoded / decoded_add   a verified producer XOR-decodes / add-decodes bytes that are on no disc span: admitted as `produced:`, compiled, run, identical identity
  decoded_partial         only part of the output exists: the program stops before executing a byte nobody produced
  decoded_patched / _volatile / _large / jump_unwritten / transformed (Word RAM) / generated (many writers): typed refusals, nothing wrong executes
  decoded_corrupt         the payload is corrupt on the disc: the corrupt output is what is produced and the program does not reach the oracle
  decoded_input           the key is machine input (backup RAM): a run with other input stops (code_bytes_mismatch) BEFORE the produced image's first instruction
  emitter                 produced admissions are validated (wrong pass identity, odd, over-long, Word RAM base)
usage: sega_cd_reconstruction_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_loader_fixture.py"
EXE = ".exe" if os.name == "nt" else ""
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


class Case:
    def __init__(self, tmp, name):
        self.name, self.tmp = name, tmp
        self.dir = tmp / ("f-" + name)
        self.truth = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--case", name, "--out", str(self.dir)], check=True, capture_output=True, text=True).stdout)
        self.cue, self.bios = self.dir / "disc" / "disc.cue", self.dir / "bios.bin"
        self.track = self.dir / "disc" / "track01.bin"

    def build(self, label, *extra, jobs=4):
        out = self.tmp / ("o-%s-%s" % (self.name, label))
        done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(self.cue), "--bios", str(self.bios), "--output", str(out), "--runtime-dir", str(ROOT), "--cc", CC,
                               "--budget", "3000000", "--jobs", str(jobs), "--cache-dir", str(self.tmp / "cache"), "--keep-passes", "1", *extra], capture_output=True, text=True, timeout=1800)
        status = out / "status.json"
        return out, (json.loads(status.read_text()) if status.exists() else None), done

    def run(self, out, *extra):
        done = subprocess.run([str(out / ("program" + EXE)), "--disc-dir", str(self.dir / "disc"), "--bios", str(self.bios), "--instruction-budget", "3000000", *extra],
                              capture_output=True, text=True, timeout=300)
        report = json.loads(done.stdout) if done.stdout.strip() else {}
        marker = report["ga"]["comstat"][2] if "ga" in report else None
        return done.returncode, report, marker


def derive(cue, capture, region, pass_id, extents=(), snapshot=None):
    cmd = [SEGARECOMP, "sega-cd", "derive-relocation", "--cue", str(cue), "--capture", str(capture), "--region", str(region), "--pass", pass_id]
    for e in extents:
        cmd += ["--extent", e]
    if snapshot:
        cmd += ["--snapshot", str(snapshot)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    return done.returncode, (json.loads(done.stdout) if done.stdout.strip() else {})


def round_of(status, kind):
    return next(h for h in status["history"] if h.get("admitted") == kind)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-recon-") as tmp:
        tmp = pathlib.Path(tmp)

        # ================= Class A: a verified relocated copy =================
        rel = Case(tmp, "relocated")
        truth = rel.truth["relocation"]
        want = "sub:%d:%d:%d" % (truth["offset"], truth["base"], truth["length"])
        out, status, done = rel.build("a", jobs=8)
        check(done.returncode == 0 and status and status["status"] == "ok" and status["frontier"] is None and status["stop"] == "none",
              "relocation: the native build completes with no frontier left (no manual extent, no per-title input)")
        check(want in status["extents"] and status["reconstruction"]["relocated_extents"] == 1 and any(h.get("admitted") == "relocated_extent" for h in status["history"]),
              "relocation: the fragment below the 32/64-byte floors is explained and admitted as an exact `extent:` equal to the ground truth (used only to judge)")
        check(len(status["history"]) == status["rounds"] and status["rounds"] == 4, "relocation: no extra discovery or provenance round (epoch, module, fragment, final = 4 rounds)")
        code, report, marker = rel.run(out)
        check(report.get("result") == "idle" and marker == 0xA001, "relocation: the generated program runs the relocated fragment natively to its marker")
        out_b, status_b, _ = rel.build("b", jobs=2)
        out_c, status_c, _ = rel.build("c", jobs=8)
        check(status_b["program_identity"] == status["program_identity"] == status_c["program_identity"] and status_b["extents"] == status["extents"],
              "determinism: repeated builds and different compiler job counts give the identical program identity and extents")
        pass_dir = out / "passes" / str(round_of(status, "relocated_extent")["round"])
        capture = json.loads((pass_dir / "capture.json").read_text())
        check(capture["streams"]["version"] == 1 and capture["streams"]["items"] and capture["streams"]["evicted"] == 0 and len(capture["streams"]["items"]) <= 8,
              "observer: the frontier capture carries a versioned, bounded stream table")
        # ---- the derivation matrix: mutate the evidence of the frontier pass one fact at a time ----
        round_id = round_of(status, "relocated_extent")["program_identity"]
        modules = [e for e in status["extents"] if e != want]
        cap_text = (pass_dir / "capture.json").read_text()
        region = pass_dir / "region.bin"
        cue = rel.cue

        def mutated(name, change):
            """The frontier capture with one fact changed (the runtime's compact JSON, re-serialised in the same key order)."""
            doc = json.loads(cap_text)
            change(doc, next(i for i in doc["streams"]["items"] if i["length"] == truth["length"] and i["paired"] == 1))
            path = tmp / ("m-%s.json" % name)
            path.write_text(json.dumps(doc, separators=(",", ":")) + "\n")
            return path

        def flag(key, value=1):
            return lambda doc, stream: stream.__setitem__(key, value)

        def derive_with(name, text=None, extents=modules, pass_id=round_id, cue_path=cue, region_path=region, snapshot=None):
            path = pass_dir / "capture.json" if text is None else text
            return derive(cue_path, path, region_path, pass_id, extents, snapshot)

        rc, ans = derive_with("base")
        check(rc == 0 and ans.get("class") == "copy" and "%s:%d:%d:%d" % (ans["processor"], ans["offset"], ans["base"], ans["length"]) == want,
              "derivation: the unmutated evidence gives exactly the extent the build admitted")
        rc, again = derive_with("base")
        check(again == ans, "derivation: repeated runs give the identical answer")

        def refused(label, error, **kw):
            rc, ans = derive_with(label.replace(" ", "_"), **kw)
            check(rc == 8 and ans.get("error") == error, "refusal (%s): %s" % (error, label) + ("" if ans.get("error") == error else " -- got %s" % ans))

        refused("a wrong pass identity (evidence of another program)", "pass_mismatch", pass_id="0" * 64)
        refused("no admitted extent contains the source", "source_not_admitted", extents=[])
        refused("the extent belongs to the other processor", "source_not_admitted", extents=["main:" + modules[0].split(":", 1)[1]])
        b = modules[0].split(":")
        refused("an extent that does not reach the source", "source_not_admitted", extents=["sub:%s:%s:%d" % (b[1], b[2], 0x80)])
        refused("a stream fed from several source runs", "source_discontiguous", text=mutated("gap", flag("source_gap")))
        refused("the stream was written over after it was produced", "stream_overwritten", text=mutated("clobbered", flag("clobbered")))
        refused("a write was dropped by write protect", "stream_dropped", text=mutated("dropped", flag("dropped")))
        refused("a device register was read while it was produced", "volatile_input", text=mutated("volatile", flag("volatile")))
        refused("another processor wrote the stream", "foreign_writer", text=mutated("foreign", flag("writer_cpu", 0)))
        refused("an odd program counter", "too_short", text=mutated("odd", lambda doc, stream: doc.__setitem__("pc", doc["pc"] + 1)))
        refused("the table evicted streams and none owns the program counter", "stream_evicted",
                text=mutated("evicted", lambda doc, stream: (doc["streams"].__setitem__("evicted", 3), stream.__setitem__("destination", stream["destination"] + 0x1000))))
        refused("the disc delivery evidence overflowed", "ledger_overflow", text=mutated("overflow", lambda doc, stream: doc["region"].__setitem__("delivered_overflow", 1)))
        ledger = capture["region"]["delivered"]
        refused("the source blocks were not delivered", "source_not_delivered", text=mutated("undelivered", lambda doc, stream: doc["region"].__setitem__("delivered", [[ledger[0][0] + 150, 1]])))
        refused("an empty delivery ledger", "no_delivery_evidence", text=mutated("emptyledger", lambda doc, stream: doc["region"].__setitem__("delivered", [])))
        refused("malformed evidence", "bad_evidence", text=mutated("malformed", lambda doc, stream: doc["streams"].__setitem__("version", 2)))
        # the live window is not the stream
        bad_region = tmp / "m-region.bin"
        raw = bytearray(region.read_bytes())
        raw[truth["base"] - capture["region"]["base"] + 5] ^= 0xFF
        bad_region.write_bytes(bytes(raw))
        refused("the live window differs from the stream", "stream_not_live", region_path=bad_region)
        # the disc differs from what the stream copied (the source was modified, or this is not the same disc)
        other = Case(tmp, "direct")
        refused("another disc (the mapped source bytes differ)", "source_modified", cue_path=other.cue)
        saved = rel.track.read_bytes()
        mod = bytearray(saved)
        mod[truth["offset"] + 3] ^= 0x01
        rel.track.write_bytes(bytes(mod))
        refused("the disc source was modified after the copy", "source_modified")
        rel.track.write_bytes(saved)
        # two admitted extents mapping the source to different disc bytes
        refused("two admitted extents map the source differently", "ambiguous_source", extents=modules + ["sub:%d:%s:%s" % (int(b[1]) + 2048, b[2], b[3])])
        # a stream that is not paired is never a relocation extent
        unpaired = mutated("unpaired", flag("paired", 0))
        rc, ans = derive_with("unpaired", text=unpaired)
        check(rc == 8 and ans.get("error") == "not_a_copy", "equal bytes without a causal read are never a relocation extent (no snapshot offered: refused)")
        rc, ans = derive_with("unpaired2", text=unpaired, snapshot=tmp / "unpaired.bin")
        check(rc == 0 and ans.get("class") == "produced" and bytes.fromhex(next(i for i in json.loads(unpaired.read_text())["streams"]["items"] if i["length"] == truth["length"])["bytes"]) ==
              (tmp / "unpaired.bin").read_bytes(), "an unpaired stream can only be a produced image, whose claim is the producer's, not a disc span")

        # ---- the other relocation shapes ----
        short = Case(tmp, "relocated_short")
        out_s, st, _ = short.build("a")
        code, report, marker = short.run(out_s)
        check(st["reconstruction"]["relocated_extents"] == 1 and any(e.endswith(":16") for e in st["extents"]) and report["stop"]["class"] == "entry_not_compiled" and
              report["stop"]["pc"] == 0x30000 + 16 and marker == 0xA001,
              "incomplete output: only the produced 16 bytes are admitted; the program stops, typed, at the first byte nobody produced")
        dup = Case(tmp, "relocated_duplicate")
        out_d, st, _ = dup.build("a")
        t = dup.truth["relocation"]
        check("sub:%d:%d:%d" % (t["offset"], t["base"], t["length"]) in st["extents"] and st["frontier"] is None,
              "duplicate source: the fragment occurs twice on the disc and the causal read selects the one that was copied")
        pat = Case(tmp, "relocated_patched")
        out_p, st, _ = pat.build("a")
        code, report, marker = pat.run(out_p)
        check(st["frontier"] and st["frontier"].get("relocation") == "stream_overwritten" and marker == 0 and report["stop"]["class"] == "entry_not_compiled",
              "destination overwritten: refused (stream_overwritten); the fragment's first instruction never retires")
        mn = Case(tmp, "relocated_main")
        out_m, st, _ = mn.build("a")
        tm = mn.truth["relocation"]
        done = subprocess.run([str(out_m / ("program" + EXE)), "--disc-dir", str(mn.dir / "disc"), "--bios", str(mn.bios), "--instruction-budget", "3000000"], capture_output=True, text=True, timeout=300)
        rep = json.loads(done.stdout)
        check(st["frontier"] is None and "main:%d:%d:%d" % (tm["offset"], tm["base"], tm["length"]) in st["extents"] and st["reconstruction"]["relocated_extents"] == 1 and
              rep["result"] == "idle" and rep["result_block"][0x20 * 2:0x20 * 2 + 4] == "a001",
              "main Work RAM: a fragment the main processor copied from its Word RAM module into Work RAM is explained and runs natively (the other supported RAM class)")
        stg = Case(tmp, "relocated_staged")
        out_g, st, _ = stg.build("a")
        code, report, marker = stg.run(out_g)
        tg = stg.truth["relocation"]
        check(st["reconstruction"]["relocated_extents"] == 0 and not any(e.endswith(":%d:%d" % (tg["base"], tg["length"])) for e in st["extents"]) and
              st["reconstruction"]["produced_images"] == 1 and marker == 0xA001,
              "register-staged copy (equal bytes, no causal read): no relocation extent is admitted; the bytes are only a produced image")

        # ================= Class B: produced code =================
        dec = Case(tmp, "decoded")
        out1, st1, done = dec.build("a", jobs=8)
        plain = bytes.fromhex(dec.truth["produced"]["plain"])
        check(done.returncode == 0 and st1["status"] == "ok" and st1["frontier"] is None and st1["extents"] == [] and st1["reconstruction"]["produced_images"] == 1 and st1["stop"] == "none",
              "produced code: bytes a verified producer decoded (on no disc span) are admitted with no manual image, and the build ends with no frontier")
        check(plain not in dec.track.read_bytes() and (out1 / "produced0.bin").read_bytes() == plain,
              "produced code: the admitted bytes are exactly the decoded code and occur nowhere on the disc (no fabricated disc offset)")
        code, report, marker = dec.run(out1)
        check(report.get("result") == "idle" and marker == dec.truth["markers"][0], "produced code: the precompiled body runs and reproduces the independent oracle marker")
        out2, st2, _ = dec.build("b", jobs=2)
        check(st2["program_identity"] == st1["program_identity"] and (out2 / "produced0.bin").read_bytes() == plain, "produced code: identical inputs give the identical program identity and bytes (other job count)")
        code, report, marker = dec.run(out2)
        check(marker == 0xA001, "produced code: the second build runs the same")
        # emitter validation of produced admissions (the chain is the epoch, then the produced image)
        epoch_snapshot = out1 / "epoch0.bin"
        h = st1["history"]

        def emit(admit, image=out1 / "produced0.bin"):
            cmd = [SEGARECOMP, "sega-cd", "emit", "--cue", str(dec.cue), "--bios", str(dec.bios), "--out", str(tmp / "x.c"),
                   "--admit", "epoch:%s:%s" % (h[0]["program_identity"], epoch_snapshot), "--admit", admit.replace("@IMAGE@", str(image))]
            return subprocess.run(cmd, capture_output=True, text=True)

        base = dec.truth["produced"]["base"]
        done = emit("produced:%s:sub:%d:@IMAGE@" % (h[1]["program_identity"], base))
        check(done.returncode == 0, "emitter: the produced image under the identity of the pass that produced it validates")
        done = emit("produced:%s:sub:%d:@IMAGE@" % ("0" * 64, base))
        check(done.returncode != 0 and "provenance_mismatch" in done.stdout, "emitter: another pass identity is refused (provenance_mismatch)")
        done = emit("produced:%s:sub:%d:@IMAGE@" % (h[1]["program_identity"], base + 1))
        check(done.returncode != 0 and "produced_invalid" in done.stdout, "emitter: an odd base is refused (produced_invalid)")
        done = emit("produced:%s:sub:%d:@IMAGE@" % (h[1]["program_identity"], 0x80000))
        check(done.returncode != 0 and "produced_invalid" in done.stdout, "emitter: a Word RAM base is refused (produced_invalid)")
        odd = tmp / "odd.bin"
        odd.write_bytes(plain[:63])
        done = emit("produced:%s:sub:%d:@IMAGE@" % (h[1]["program_identity"], base), odd)
        check(done.returncode != 0 and "produced_invalid" in done.stdout, "emitter: an odd length is refused (produced_invalid)")
        big = tmp / "big.bin"
        big.write_bytes(b"\x4e\x71" * 2049)
        done = emit("produced:%s:sub:%d:@IMAGE@" % (h[1]["program_identity"], base), big)
        check(done.returncode != 0, "emitter: an image over the 4 KiB bound is refused")
        done = emit("produced:%s:sub:%d:%s" % (h[1]["program_identity"], base, tmp / "missing.bin"))
        check(done.returncode != 0 and "snapshot_unreadable" in done.stdout, "emitter: an unreadable snapshot is refused (snapshot_unreadable)")
        # tampering with the input after the build: the program refuses to start (the disc identity is part of the program)
        saved = dec.track.read_bytes()
        raw = bytearray(saved)
        raw[dec.truth["produced"]["length"] // 2 + 40 * 2048] ^= 0x01
        dec.track.write_bytes(bytes(raw))
        code, report, marker = dec.run(out1)
        check(code != 0 and marker != 0xA001, "input changed after the build: the program does not run (%s)" % (report.get("error") or report.get("result")))
        dec.track.write_bytes(saved)

        add = Case(tmp, "decoded_add")
        out_a, st, _ = add.build("a")
        code, report, marker = add.run(out_a)
        check(st["reconstruction"]["produced_images"] == 1 and st["frontier"] is None and (out_a / "produced0.bin").read_bytes() == bytes.fromhex(add.truth["produced"]["plain"]) and marker == 0xA001,
              "second transformation (add a constant): the same admission, no new production code, same oracle")

        part = Case(tmp, "decoded_partial")
        out_pa, st, _ = part.build("a")
        code, report, marker = part.run(out_pa)
        check(st["reconstruction"]["produced_images"] == 1 and (out_pa / "produced0.bin").stat().st_size == 32 and report["stop"]["class"] == "entry_not_compiled" and
              report["stop"]["pc"] == 0x30000 + 32 and marker == 0xA001,
              "partial output: only the 32 produced bytes are admitted; the program stops before executing a byte nobody produced")

        for case, refusal in (("decoded_patched", "stream_overwritten"), ("decoded_volatile", "volatile_input"), ("decoded_large", "stream_too_large")):
            c = Case(tmp, case)
            o, st, _ = c.build("a")
            code, report, marker = c.run(o)
            check(st["frontier"] and st["frontier"].get("relocation") == refusal and st["reconstruction"]["produced_images"] == 0 and marker == 0 and report["stop"]["class"] == "entry_not_compiled",
                  "%s: refused (%s), nothing is admitted, the produced code never retires" % (case, refusal))
        for case, refusal in (("transformed", "unsupported_region"), ("generated", "stream_evicted")):
            c = Case(tmp, case)
            o, st, _ = c.build("a")
            check(st["frontier"] and st["frontier"].get("relocation") == refusal and st["reconstruction"]["produced_images"] == 0 and not st["extents"],
                  "%s: refused (%s)" % (case, refusal))
        c = Case(tmp, "jump_unwritten")
        o, st, _ = c.build("a")
        last = st["history"][-1]
        rc, ans = derive(c.cue, o / "passes" / str(last["round"]) / "capture.json", o / "passes" / str(last["round"]) / "region.bin", last["program_identity"], [], tmp / "none.bin")
        check(rc == 8 and ans.get("error") == "no_stream" and st["reconstruction"]["produced_images"] == 0,
              "arbitrary RAM with no producer: no stream owns the frontier, nothing is admitted (no_stream)")
        rc, ans = derive(c.cue, o / "passes" / str(last["round"]) / "capture.json", o / "passes" / str(last["round"]) / "region.bin", last["program_identity"], [], tmp / "none.bin")
        check(not (tmp / "none.bin").exists(), "a refusal writes no snapshot")

        cor = Case(tmp, "decoded_corrupt")
        o, st, _ = cor.build("a")
        code, report, marker = cor.run(o)
        check(st["reconstruction"]["produced_images"] == 1 and report.get("result") != "idle" and report["stop"]["class"] == "entry_not_compiled",
              "corrupted payload: the corrupt output is what is produced; the program does not complete and stops, typed, at the first instruction nobody can compile")
        again, st_again, _ = cor.build("b", jobs=8)
        check(st_again["program_identity"] == st["program_identity"] and st_again["frontier"] == st["frontier"], "corrupted payload: the refusal evidence is deterministic")

        inp = Case(tmp, "decoded_input")
        ram_a = tmp / "backup-a.bin"
        ram_b = tmp / "backup-b.bin"
        ram_a.write_bytes(bytes([0x5A]) + bytes(8191))
        ram_b.write_bytes(bytes([0x5B]) + bytes(8191))
        o, st, _ = inp.build("a", "--backup-ram", str(ram_a))
        code, report, marker = inp.run(o, "--backup-ram", str(ram_a))
        check(st["reconstruction"]["produced_images"] == 1 and marker == 0xA001, "machine input: with the backup RAM the build saw, the produced body runs and gives the oracle")
        code, report, marker = inp.run(o, "--backup-ram", str(ram_b))
        check(marker == 0 and report["stop"]["class"] == "code_bytes_mismatch" and report["stop"]["pc"] == 0x30000,
              "machine input: other backup RAM makes the producer write other bytes; the program stops (code_bytes_mismatch) BEFORE the first instruction of the produced image retires")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
