#!/usr/bin/env python3
"""SEG-112 (ADR 0256): generation-time producer recognition in the product `segarecomp sega-cd build`, on project-authored fixtures.

The fixture IP (boot code the fixture BIOS copies to main Work RAM and calls) copies or transforms a fragment of itself into Work RAM with a counted byte loop and
calls it. With static producers enabled the fragment is proven from the disc's boot area and the loop's instruction semantics BEFORE the first emission:

  static_copy / static_xor / static_add / static_sub   the fragment is in the first emission: no observed round, no relocation or produced admission; the program runs it
                                                       natively and reproduces the oracle; repeated builds and other job counts give the identical program identity
  fallback                                             `--static-producers 0` is the M15 behaviour: one more round (relocated extent / produced image), the same final machine state
  emitter                                              a static-fragment admission is recomputed from the disc: wrong coordinates, digest, processor, a disc with no producer, or a
                                                       malformed spec are refused (static_fragment_unproven / _syntax); nothing is trusted
  unrecognized producers                               producers outside the boot images (decoded*, relocated) are untouched: the observed fallback still handles them
usage: sega_cd_static_producer_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import os
import pathlib
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

    def build(self, label, *extra, jobs=4):
        out = self.tmp / ("o-%s-%s" % (self.name, label))
        subprocess.run([SEGARECOMP, "sega-cd", "build", "--cue", str(self.cue), "--bios", str(self.bios), "--output", str(out), "--runtime-dir", str(ROOT), "--cc", CC,
                        "--budget", "3000000", "--jobs", str(jobs), "--cache-dir", str(self.tmp / "cache"), *extra], capture_output=True, text=True, timeout=1800)
        status = out / "status.json"
        return out, (json.loads(status.read_text()) if status.exists() else None)

    def run(self, out):
        done = subprocess.run([str(out / ("program" + EXE)), "--disc-dir", str(self.dir / "disc"), "--bios", str(self.bios), "--instruction-budget", "3000000"], capture_output=True, text=True, timeout=300)
        return json.loads(done.stdout)

    def producers(self):
        done = subprocess.run([SEGARECOMP, "sega-cd", "discover-producers", "--cue", str(self.cue)], capture_output=True, text=True)
        return done.returncode, json.loads(done.stdout)

    def emit(self, *admits):
        cmd = [SEGARECOMP, "sega-cd", "emit", "--cue", str(self.cue), "--bios", str(self.bios), "--out", str(self.tmp / "x.c")]
        for a in admits:
            cmd += ["--admit", a]
        return subprocess.run(cmd, capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-static-") as tmp:
        tmp = pathlib.Path(tmp)
        reports = {}
        for name, label in (("static_copy", "counted copy"), ("static_xor", "xor transform"), ("static_add", "add transform"), ("static_sub", "subtract transform")):
            c = Case(tmp, name)
            t = c.truth["relocation"]
            rc, found = c.producers()
            plain = c.truth["static_fragment"]
            check(rc == 0 and found["proven"] == 1 and found["candidates"] == 1 and found["fragments"][0].startswith("main:%s:" % ("copy" if name == "static_copy" else "transform")) and
                  found["fragments"][0].split(":")[3:5] == [str(t["base"]), str(t["length"])],
                  "%s: the recognizer proves the fragment from the boot area alone (no pass, no run)" % label)
            rc, again = c.producers()
            check(again == found, "%s: the proof is deterministic" % label)
            out, st = c.build("static", jobs=8)
            rep = c.run(out)
            check(st["status"] == "ok" and st["rounds"] == 3 and st["reconstruction"]["static_fragments"] == 1 and st["reconstruction"]["relocated_extents"] == 0 and
                  st["reconstruction"]["produced_images"] == 0 and st["frontier"] is None and rep["result"] == "idle" and rep["result_block"][0x40:0x44] == "a001",
                  "%s: in the first emission - 3 rounds (epoch, IP, final), no relocation or produced admission, native run reproduces the oracle" % label)
            check(not any(h.get("admitted") in ("relocated_extent", "produced") for h in st["history"]), "%s: no execution-observed admission anywhere in the history" % label)
            out2, st2 = c.build("static2", jobs=2)
            check(st2["program_identity"] == st["program_identity"], "%s: identical program identity across builds and job counts" % label)
            out_b, st_b = c.build("fallback", "--static-producers", "0")
            rep_b = c.run(out_b)
            check(st_b["rounds"] == 4 and st_b["reconstruction"]["static_fragments"] == 0 and st_b["frontier"] is None and len(st_b["extents"]) + st_b["reconstruction"]["produced_images"] == len(st["extents"]) + 1 and rep_b["result_block"][0x40:0x44] == "a001",
                  "%s: the M15 fallback still works and needs one more round (4)" % label)
            keys = ("result", "stop", "cpus", "digests", "device_log", "frame_sha256", "instructions", "ga", "result_block")
            check(all(rep[k] == rep_b[k] for k in keys), "%s: static and fallback programs end in the identical machine state (program identities differ: other image authority)" % label)
            reports[name] = (c, st)

        c, st = reports["static_xor"]
        good = "static-fragment:main:%d:%d:" % (c.truth["relocation"]["base"], c.truth["relocation"]["length"])
        digest = json.loads(subprocess.run([SEGARECOMP, "sega-cd", "discover-producers", "--cue", str(c.cue)], capture_output=True, text=True).stdout)["admits"][0].split(":")[-1]
        done = c.emit(good + digest)
        check(done.returncode == 0, "emitter: the recomputed static fragment is accepted")
        for label, spec, want in (("wrong digest", good + "0" * 64, "static_fragment_unproven"),
                                  ("wrong base", "static-fragment:main:%d:%d:%s" % (c.truth["relocation"]["base"] + 2, c.truth["relocation"]["length"], digest), "static_fragment_unproven"),
                                  ("wrong length", "static-fragment:main:%d:%d:%s" % (c.truth["relocation"]["base"], c.truth["relocation"]["length"] - 2, digest), "static_fragment_unproven"),
                                  ("wrong processor", "static-fragment:sub:%d:%d:%s" % (c.truth["relocation"]["base"], c.truth["relocation"]["length"], digest), "static_fragment_unproven"),
                                  ("malformed", "static-fragment:main:xx:%d:%s" % (c.truth["relocation"]["length"], digest), "static_fragment_syntax")):
            done = c.emit(spec)
            check(done.returncode != 0 and want in done.stdout, "emitter: %s is refused (%s)" % (label, want))
        other = Case(tmp, "direct")
        done = other.emit(good + digest)
        check(done.returncode != 0 and "static_fragment_unproven" in done.stdout, "emitter: a disc whose boot area has no such producer refuses the same spec")
        rc, none = other.producers()
        check(rc == 0 and none["proven"] == 0 and none["fragments"] == [], "a disc without a recognisable producer proves nothing")

        # producers the recognizer does not own are untouched: the observed fallback still handles them with static producers enabled
        for name, want in (("decoded", "produced_images"), ("relocated", "relocated_extents")):
            d = Case(tmp, name)
            out, st = d.build("a")
            rep = d.run(out)
            check(st["status"] == "ok" and st["reconstruction"]["static_fragments"] == 0 and st["reconstruction"][want] == 1 and st["frontier"] is None and rep["result"] == "idle",
                  "%s: a producer outside the boot images is not claimed statically; the M15 fallback handles it (%s)" % (name, want))
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
