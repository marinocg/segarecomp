#!/usr/bin/env python3
"""SEG-113 (ADR 0257): static BIOS boot-image admission in the product `segarecomp sega-cd build`, on project-authored fixtures.

The fixture BIOS (tools/sega_cd_bios_boot_contract_fixture.py) implements the boot-placement contract with the machine's own CDD/CDC path and carries a
profile tag; the product offers the main boot area and the sub boot program from a verified profile and the disc alone, BEFORE the first emission:

  contract / ip_short / sp_long / sp_moved / form_ok   both images are in the first emission: 2 rounds instead of 4; the images are exactly what the execution-observed
                                                       build admits (differential), the programs end in the identical machine state, identity is deterministic
  no_profile / form_other                              no verified profile / header form: nothing is offered, the build is the execution-observed one (4 rounds)
  sp_source                                            the descriptor names other bytes than the BIOS reads: the static SP image does not explain the live bytes - the
                                                       guard stops, the contradiction is reported, the observed fallback admits the real bytes, the SP still runs
  ip_patched / ip_trailing                             a byte rewritten before the call: no wrong instruction retires; a partial placement is harmless
  emitter                                              a `static-boot:` admission is recomputed from the BIOS and the disc: wrong offset/base/length/cpu/digest, malformed,
                                                       duplicate, unverified BIOS, other header form, a changed disc are refused
usage: sega_cd_static_boot_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
FIXTURE = ROOT / "tools" / "sega_cd_bios_boot_contract_fixture.py"
EXE = ".exe" if os.name == "nt" else ""
failures = []
KEYS = ("result", "stop", "cpus", "digests", "device_log", "frame_sha256", "instructions", "ga", "result_block")


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

    def build(self, label, *extra, jobs=8, fixture=True):
        if fixture:
            extra = ("--boot-fixture-profiles", "1", *extra)   # the test-only switch: without it a tagged BIOS is never selected
        out = self.tmp / ("o-%s-%s" % (self.name, label))
        subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(self.cue), "--bios", str(self.bios), "--output", str(out), "--runtime-dir", str(ROOT), "--cc", CC,
                        "--budget", "3000000", "--jobs", str(jobs), "--cache-dir", str(self.tmp / "cache"), *extra], capture_output=True, text=True, timeout=1800)
        status = out / "status.json"
        return out, (json.loads(status.read_text()) if status.exists() else None)

    def run(self, out):
        done = subprocess.run([str(out / ("program" + EXE)), "--disc-dir", str(self.dir / "disc"), "--bios", str(self.bios), "--instruction-budget", "3000000"], capture_output=True, text=True, timeout=300)
        return json.loads(done.stdout)

    def contract(self, fixture=True):
        done = subprocess.run([SEGARECOMP, "sega-cd", "boot-contract", "--cue", str(self.cue), "--bios", str(self.bios), *(("--boot-fixture-profiles", "1") if fixture else ())], capture_output=True, text=True)
        return done.returncode, json.loads(done.stdout)

    def emit(self, *admits, cue=None, fixture=True):
        cmd = [SEGARECOMP, "sega-cd", "emit", "--cue", str(cue or self.cue), "--bios", str(self.bios), "--out", str(self.tmp / "x.c"), *(("--boot-fixture-profiles", "1") if fixture else ())]
        for a in admits:
            cmd += ["--admit", a]
        return subprocess.run(cmd, capture_output=True, text=True)


def markers(rep):
    block = rep["result_block"]
    return block[0x80:0x84], block[0x20:0x24]     # the IP marker (RESULT + $40), the SP marker (COMSTAT2 copied to RESULT + $10)


def expected_images(c):
    sp = c.truth["sp"]
    return [("main", 0x200, 0xFF0000, c.truth["ip_span"]), ("sub", sp["address"], 0x6000, sp["length"])]


def main():
    with tempfile.TemporaryDirectory(prefix="scd-static-boot-") as tmp:
        tmp = pathlib.Path(tmp)
        cases = {}
        for name in ("contract", "ip_short", "sp_long", "sp_moved", "form_ok"):
            c = cases[name] = Case(tmp, name)
            rc, found = c.contract()
            images = [(i["cpu"], i["offset"], i["base"], i["length"]) for i in found["images"]]
            check(rc == 0 and found["refusal"] == "none" and found["profile"].startswith("project_fixture_boot_contract_") and images == expected_images(c) and
                  all(i["class"] == "conditional" for i in found["images"]) and len(found["admits"]) == 2,
                  "%s: the contract derives both images from the BIOS profile and the disc alone, classed conditional (never proven)" % name)
            check(c.contract()[1] == found, "%s: the contract is deterministic" % name)
            out, st = c.build("static")
            rep = c.run(out)
            sb = st["static_boot"]
            check(st["status"] == "ok" and st["rounds"] == 2 and st["extents"] == [] and sb["state"] == "admitted" and sb["contradictions"] == 0 and st["frontier"] is None and
                  rep["result"] == "idle" and markers(rep) == ("a001", "b002"),
                  "%s: both boot images are in the first emission - 2 rounds (epoch, final), no observed extent, the IP and the SP ran natively" % name)
            check([h.get("admitted") for h in st["history"]] == ["epoch", None], "%s: the history has no image-discovery round" % name)
            out_b, st_b = c.build("control", "--static-boot", "0")
            rep_b = c.run(out_b)
            observed = sorted((e.split(":")[0], int(e.split(":")[1]), int(e.split(":")[2]), int(e.split(":")[3])) for e in st_b["extents"])
            check(st_b["status"] == "ok" and st_b["rounds"] == 4 and st_b["static_boot"]["state"] == "disabled" and observed == sorted(expected_images(c)) and markers(rep_b) == ("a001", "b002"),
                  "%s: the control (--static-boot 0) needs 4 rounds and its execution-observed extents are exactly the statically derived images (differential)" % name)
            check(all(rep[k] == rep_b[k] for k in KEYS), "%s: static and observed builds end in the identical machine state (program identities differ: no pass identity)" % name)
            out2, st2 = c.build("static2", jobs=2)
            check(st2["program_identity"] == st["program_identity"], "%s: identical program identity across builds and job counts" % name)

        # ---- the emitter recomputes the proof ------------------------------------------------------------------------------------------------------------------------
        c = cases["contract"]
        _, found = c.contract()
        ip_spec, sp_spec = found["admits"]
        done = c.emit(ip_spec, sp_spec)
        check(done.returncode == 0, "emitter: the recomputed static boot admissions are accepted")
        ip = ip_spec.split(":")
        def spec(cpu, off, base, length, digest):
            return "static-boot:%s:%s:%s:%s:%s" % (cpu, off, base, length, digest)
        for label, text, want in (("wrong digest", spec("main", ip[2], ip[3], ip[4], "0" * 64), "static_boot_unproven"),
                                  ("wrong source offset", spec("main", int(ip[2]) + 2, ip[3], ip[4], ip[5]), "static_boot_unproven"),
                                  ("wrong destination", spec("main", ip[2], int(ip[3]) + 2, ip[4], ip[5]), "static_boot_unproven"),
                                  ("wrong length", spec("main", ip[2], ip[3], int(ip[4]) - 2, ip[5]), "static_boot_unproven"),
                                  ("wrong processor", spec("sub", ip[2], ip[3], ip[4], ip[5]), "static_boot_unproven"),
                                  ("malformed coordinates", spec("main", "xx", ip[3], ip[4], ip[5]), "static_boot_syntax"),
                                  ("zero length", spec("main", ip[2], ip[3], 0, ip[5]), "static_boot_syntax")):
            done = c.emit(text)
            check(done.returncode != 0 and want in done.stdout, "emitter: %s is refused (%s)" % (label, want))
        done = c.emit(ip_spec, ip_spec)
        check(done.returncode != 0 and "static_boot_duplicate" in done.stdout, "emitter: a duplicate static boot admission is refused (static_boot_duplicate)")
        for other in ("no_profile", "form_other"):
            o = cases[other] = Case(tmp, other)
            rc, none = o.contract()
            want = "bios_not_verified" if other == "no_profile" else "header_not_verified"
            check(rc == 0 and none["refusal"] == want and none["images"] == [] and none["admits"] == [] and (none["profile"] == "") == (other == "no_profile"),
                  "%s: the eligibility check offers nothing (%s)" % (other, want))
            done = o.emit(ip_spec)
            check(done.returncode != 0 and "static_boot_unverified" in done.stdout, "%s: the emitter refuses a static boot admission on an unverified BIOS / header form" % other)
            out, st = o.build("static")
            rep = o.run(out)
            check(st["status"] == "ok" and st["rounds"] == 4 and st["static_boot"]["state"] == "not_offered" and st["static_boot"]["refusal"] == want and len(st["extents"]) == 2 and
                  markers(rep) == ("a001", "b002"), "%s: the build is the execution-observed one (4 rounds), the dynamic fallback is intact" % other)
        # a disc whose boot area cannot be used: an audio first track, a descriptor leaving the system area (invalid source span)
        for label, patch in (("audio first track", "audio"), ("SP descriptor outside the system area", "descriptor")):
            bad = tmp / ("bad-" + patch)
            shutil.copytree(cases["contract"].dir, bad)
            if patch == "audio":
                track = bad / "disc" / "track01.bin"
                track.write_bytes(track.read_bytes().ljust(2352 * 140, b"\0"))
                (bad / "disc" / "disc.cue").write_text('FILE "track01.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n', newline="\n")
            else:
                track = bad / "disc" / "track01.bin"
                data = bytearray(track.read_bytes())
                data[0x44:0x48] = (0x8000).to_bytes(4, "big")
                track.write_bytes(bytes(data))
            done = subprocess.run([SEGARECOMP, "sega-cd", "boot-contract", "--cue", str(bad / "disc" / "disc.cue"), "--bios", str(bad / "bios.bin"), "--boot-fixture-profiles", "1"], capture_output=True, text=True)
            found = json.loads(done.stdout)
            check(done.returncode == 0 and found["refusal"] == "disc_boot_area" and found["images"] == [], "%s: no boot image is offered (disc_boot_area)" % label)
            done = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(bad / "disc" / "disc.cue"), "--bios", str(bad / "bios.bin"), "--out", str(tmp / "z.c"), "--boot-fixture-profiles", "1", "--admit", ip_spec],
                                  capture_output=True, text=True)
            check(done.returncode != 0 and "static_boot_unverified" in done.stdout, "%s: the emitter refuses the old admission" % label)
        # a changed disc: the old admission's digest no longer matches what the disc and BIOS prove
        changed = tmp / "changed"
        shutil.copytree(c.dir, changed)
        track = changed / "disc" / "track01.bin"
        data = bytearray(track.read_bytes())
        data[0x400] ^= 0xFF
        track.write_bytes(bytes(data))
        done = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(changed / "disc" / "disc.cue"), "--bios", str(changed / "bios.bin"), "--out", str(tmp / "y.c"), "--boot-fixture-profiles", "1", "--admit", ip_spec],
                              capture_output=True, text=True)
        check(done.returncode != 0 and "static_boot_unproven" in done.stdout, "emitter: a changed disc refuses the old admission (the input identity is recomputed)")
        changed_cue = changed / "disc" / "disc.cue"
        done = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(changed_cue), "--bios", str(changed / "bios.bin"), "--output", str(tmp / "o-changed"), "--runtime-dir", str(ROOT), "--cc", CC, "--boot-fixture-profiles", "1",
                               "--budget", "3000000", "--jobs", "8", "--cache-dir", str(tmp / "cache")], capture_output=True, text=True, timeout=1800)
        st_changed = json.loads((tmp / "o-changed" / "status.json").read_text())
        st_first = json.loads((tmp / "o-contract-static" / "status.json").read_text())
        check(st_changed["status"] == "ok" and st_changed["rounds"] == 2 and st_changed["program_identity"] != st_first["program_identity"],
              "cache: an input change produces another program (another identity) and still needs no discovery round")

        # ---- descriptor / source disagreement: the static SP image is wrong ------------------------------------------------------------------------------------------
        c = cases["sp_source"] = Case(tmp, "sp_source")
        out, st = c.build("static")
        rep = c.run(out)
        sb = st["static_boot"]
        check(st["status"] == "ok" and sb["state"] == "admitted" and sb["contradictions"] == 1 and sb["unresolved_mismatches"] == 0 and "static_boot_contradicted" in st["inventory"]["unresolved"] and st["rounds"] == 3 and
              markers(rep) == ("a001", "b002") and rep["result"] == "idle",
              "sp_source: the static SP image does not explain the live bytes - the guard stops it, the contradiction is reported, the observed fallback admits the real bytes (3 rounds), the SP runs")
        out_b, st_b = c.build("control", "--static-boot", "0")
        rep_b = c.run(out_b)
        check(all(rep[k] == rep_b[k] for k in KEYS) and markers(rep_b) == ("a001", "b002"), "sp_source: the final machine state equals the observed build's (the static image was never preferred over the live bytes)")

        # ---- observed extents against the static mapping: partial overlap, unrelated later overlay -------------------------------------------------------------------------
        c = cases["sp_shifted"] = Case(tmp, "sp_shifted")
        out, st = c.build("static")
        rep = c.run(out)
        sb = st["static_boot"]
        check(st["status"] == "ok" and sb["contradictions"] == 1 and sb["later_overlays"] == 0 and len(st["extents"]) == 1 and markers(rep) == ("a001", "b002") and rep["result"] == "idle",
              "sp_shifted: an observed extent overlapping the static SP image only in part, from boot-area bytes mapped differently, is a contradiction (the base is not covered)")
        c = cases["ip_selfcopy"] = Case(tmp, "ip_selfcopy")
        out, st = c.build("static")
        rep = c.run(out)
        sb = st["static_boot"]
        check(st["status"] == "ok" and sb["contradictions"] == 0 and sb["internal_sources"] == 1 and sb["unresolved_mismatches"] == 0 and markers(rep) == ("a001", "b002"),
              "ip_selfcopy: a copy of the image's own bytes elsewhere inside its span is an internal source (a relocation), not a contradiction")
        c = cases["stage_overlay"] = Case(tmp, "stage_overlay")
        out, st = c.build("static")
        rep = c.run(out)
        sb = st["static_boot"]
        check(st["status"] == "ok" and sb["contradictions"] == 0 and sb["later_overlays"] == 1 and sb["unresolved_mismatches"] == 0 and st["rounds"] == 3 and "static_boot_contradicted" not in st["inventory"]["unresolved"],
              "stage_overlay: a later stage from unrelated disc content over the boot area is a later overlay - not a contradiction")

        # ---- the production path never selects a tagged BIOS ------------------------------------------------------------------------------------------------------------
        c = cases["contract"]
        rc, none = c.contract(fixture=False)
        check(rc == 0 and none["authority"] == "" and none["refusal"] == "bios_not_verified" and none["images"] == [] and none["admits"] == [],
              "production path: a BIOS carrying a fixture tag gets no profile (no static boot images)")
        done = c.emit(ip_spec, fixture=False)
        check(done.returncode != 0 and "static_boot_unverified" in done.stdout, "production path: the emitter refuses a static boot admission for a tagged BIOS")
        out, st = c.build("production", fixture=False)
        check(st["status"] == "ok" and st["rounds"] == 4 and st["static_boot"]["state"] == "not_offered" and st["static_boot"]["authority"] == "" and len(st["extents"]) == 2,
              "production path: the build of a tagged BIOS is the execution-observed one (4 rounds)")
        _, with_flag = c.contract()
        check(with_flag["authority"] == "test_assumed_fixture", "test-only path: the fixture contract is labelled test_assumed_fixture, never verified_authentic")
        # a BIOS that is tagged but behaviourally unrelated (another fixture's BIOS with the tag written into its header)
        unrelated = tmp / "f-unrelated"
        subprocess.run([sys.executable, str(ROOT / "tools" / "sega_cd_bios_loader_fixture.py"), "build", "--case", "direct", "--out", str(unrelated)], check=True, capture_output=True, text=True)
        plain_build = subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(unrelated / "disc" / "disc.cue"), "--bios", str(unrelated / "bios.bin"), "--output", str(tmp / "o-plain"),
                                      "--runtime-dir", str(ROOT), "--cc", CC, "--budget", "3000000", "--jobs", "8", "--cache-dir", str(tmp / "cache")], capture_output=True, text=True, timeout=1800)
        bios = bytearray((unrelated / "bios.bin").read_bytes())
        bios[0x100:0x120] = b"SEGA BOOT-CONTRACT FIXTURE 1    "
        (unrelated / "bios.bin").write_bytes(bytes(bios))
        subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(unrelated / "disc" / "disc.cue"), "--bios", str(unrelated / "bios.bin"), "--output", str(tmp / "o-tagged"),
                        "--runtime-dir", str(ROOT), "--cc", CC, "--budget", "3000000", "--jobs", "8", "--cache-dir", str(tmp / "cache")], capture_output=True, text=True, timeout=1800)
        st_plain, st_tag = (json.loads((tmp / n / "status.json").read_text()) for n in ("o-plain", "o-tagged"))
        check(st_tag["status"] == "ok" and st_tag["static_boot"]["state"] == "not_offered" and st_tag["rounds"] == st_plain["rounds"] and st_tag["extents"] == st_plain["extents"],
              "production path: a tagged but behaviourally unrelated BIOS admits no speculative boot image; the build equals the untagged one")

        # ---- a byte rewritten before the call; a partial placement -----------------------------------------------------------------------------------------------------
        c = cases["ip_patched"] = Case(tmp, "ip_patched")
        out, st = c.build("static")
        rep = c.run(out)
        out_b, st_b = c.build("control", "--static-boot", "0")
        rep_b = c.run(out_b)
        same = ("cpus", "instructions", "result_block", "device_log", "digests", "ga")
        check(rep["stop"]["pc"] == rep_b["stop"]["pc"] == 0xFF0000 + 0x40 and rep["stop"]["tick"] == rep_b["stop"]["tick"] and all(rep[k] == rep_b[k] for k in same) and
              rep["stop"]["class"] in ("code_bytes_mismatch", "entry_not_compiled") and rep_b["stop"]["class"] in ("code_bytes_mismatch", "entry_not_compiled") and rep["result"] != "idle",
              "ip_patched: both builds stop at the patched instruction before it retires, at the same tick and in the same machine state (the static image is never executed where the live bytes differ)")
        check(st["static_boot"]["contradictions"] == 0 and st["frontier"] is not None and st["frontier"]["refusal"] in ("no_source", "too_short"),
              "ip_patched: the bytes that run are on no disc span, so the resolver refuses them (typed) - no wrong admission; an unresolved mismatch is counted as such, never as a BIOS contradiction")
        c = cases["ip_trailing"] = Case(tmp, "ip_trailing")
        out, st = c.build("static")
        rep = c.run(out)
        check(st["status"] == "ok" and st["rounds"] == 2 and rep["result"] == "idle" and markers(rep) == ("a001", "b002") and st["static_boot"]["contradictions"] == 0,
              "ip_trailing: a partial placement (the rest of the span stale) is harmless - the image covers more than was placed, the executed bytes still match")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
