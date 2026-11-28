#!/usr/bin/env python3
"""SEG-148-T003 (ADR 0328): main-M68K guest patching of disc-resident code, and the precise rejection of every other code class.

Project-authored BIOS image and disc (tools/sega_cd_bios_loader_fixture.py, cases `main_patch` and `main_decoded`). `mod disc-build` builds the ORIGINAL disc itself, judges every write against
what that build proved, materializes the patched disc, builds it and judges again. The positive case patches one proven main-module instruction run (a declared extent edit) and injects a routine
into the module's free area; `mod disc-diff-run` must report the expected divergence. Each rejected class has a negative vector with its exact diagnostic.
usage: mod_disc_code_test.py <segarecomp> <c-compiler> <source-root>
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_disc_oracle as oracle  # noqa: E402
import sega_cd_fixture_disc as F  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_loader_fixture.py"
BUDGET = "3000000"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fixture(tmp, case):
    out = tmp / ("fx-" + case)
    truth = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--case", case, "--out", str(out)], check=True, capture_output=True, text=True).stdout)
    return out, truth


def cli(*args, timeout=1800):
    done = subprocess.run([SEGARECOMP, "mod", *map(str, args)], capture_output=True, text=True, timeout=timeout)
    try:
        return done.returncode, json.loads(done.stdout), done.stderr
    except json.JSONDecodeError:
        return done.returncode, {}, done.stdout + done.stderr


class Mod:
    """A mod project written from keyword parts; the disc identity and every original digest come from the independent oracle and the fixture bytes."""

    def __init__(self, tmp, fx):
        self.tmp, self.fx, self.n = tmp, fx, 0
        self.disc = oracle.read_disc(fx / "disc" / "disc.cue")
        self.track = (fx / "disc" / "track01.bin").read_bytes()
        self.bios_sha = sha((fx / "bios.bin").read_bytes())

    def project(self, edits=(), guest=None, symbols=(), cpu="m68k"):
        """edits: (offset, replacement bytes, {extra source fields})"""
        self.n += 1
        d = self.tmp / ("%s-mod-%d" % (self.fx.name, self.n))
        (d / "disc").mkdir(parents=True)
        text = '[mod]\nid = "disc-code"\nversion = "1.0.0"\nname = "disc code"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "sega-cd"\ndisc_identity = "%s"\n\n' % self.disc["identity"]
        text += '[bios]\nrequirement = "digest"\nsha256s = ["%s"]\n' % self.bios_sha
        for i, (offset, value, fields) in enumerate(edits):
            (d / "disc" / ("e%d.bin" % i)).write_bytes(value)
            fields = dict(fields)
            original = fields.pop("original", self.track[offset:offset + len(value)])
            text += '\n[[source]]\nkind = "disc-extent-edit"\npath = "disc/e%d.bin"\ntarget = 0\nstart = %d\noriginal_sha256 = "%s"\n' % (i, offset, sha(original))
            for key, v in fields.items():
                text += "%s = %s\n" % (key, json.dumps(v))
        if guest is not None:
            (d / "code").mkdir()
            (d / "code" / "main.asm").write_text(guest, newline="\n")
            text += '\n[[source]]\nkind = "guest-asm"\npath = "code/main.asm"\ncpu = "%s"\n' % cpu
        for y in symbols:
            text += "\n[[symbol]]\n" + "".join("%s = %s\n" % (k, json.dumps(v)) for k, v in y.items())
        (d / "mod.toml").write_text(text, newline="\n")
        return d


def refused(result, klass, label, contains=None):
    code, doc, err = result
    ok = code == 1 and doc.get("ok") is False and doc.get("class") == klass and (contains is None or contains in doc.get("error", ""))
    check(ok, "%s -> %s (got %s %s %s)" % (label, klass, code, doc.get("class"), doc.get("error", err[-300:])))


def jsr(address):
    a = F.Asm(0)
    a.jsr(F.absl(address))
    return a.assemble()


def main():
    with tempfile.TemporaryDirectory(prefix="scd-modcode-") as tmp:
        tmp = pathlib.Path(tmp)
        cache = tmp / "objects"
        fx, truth = fixture(tmp, "main_patch")
        p = truth["patch"]
        module, base, mark = p["module_offset"], p["main_base"], p["main_mark"]
        sled, free = p["sled_at"], p["free_at"]
        mods = Mod(tmp, fx)
        original_files = {f.name: f.read_bytes() for f in (fx / "disc").iterdir()}
        disc_args = ["--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--runtime-dir", ROOT, "--cc", CC, "--budget", BUDGET, "--jobs", "4", "--cache-dir", cache]

        def build(project, name):
            return cli("disc-build", project, "--out", tmp / name, *disc_args)

        main_edit = {"code_cpu": "main", "load_address": "0x%x" % (base + sled)}
        guest = ("free * 0x%x 0x%x 0x00\norg * 0x%x at 0x%x\n  move.w #0xc0de,0x%x\n  rts\n"
                 % (module + free, p["free_len"], module + free, base + free, mark + 2))
        call = jsr(base + free)
        edits = [(module + sled, call, main_edit), (module + 2, (0xB007).to_bytes(2, "big"), {"code_cpu": "main", "load_address": "0x%x" % (base + 2)})]

        # ---- declaration and source-level rejections (no build) ----
        z80 = mods.project(edits=[(module + sled, call, {"code_cpu": "z80", "load_address": "0xa00000"})])
        refused(cli("check", z80), "unsupported_code_class:z80", "an extent edit declared as Z80 code")
        refused(cli("check", mods.project(guest="free * 0x%x 0x100 0x00\norg * 0x%x at 0x100\n  ret\n" % (module + free, module + free), cpu="z80")), "unsupported_code_class:z80", "Z80 guest code")
        refused(cli("check", mods.project(edits=[(module + sled, call, {"code_cpu": "main", "load_address": "0xa00010"})])), "unsupported_code_class:z80", "a main patch placed in the Z80 area")
        refused(cli("check", mods.project(edits=[(module + sled, call, {"code_cpu": "sub", "load_address": "0x%x" % (base + sled)})])), "unsupported_code_class:sub_m68k", "an extent edit declared as sub-CPU code")
        sub_symbol = {"name": "sub_entry", "address": "0x%x" % p["sub_base"], "provenance": "verified", "cpu": "sub", "module": "boot"}
        refused(cli("check", mods.project(guest="free * 0x%x 0x100 0x00\norg * 0x%x at 0x%x\n  jsr @sub_entry\n  rts\n" % (module + free, module + free, base + free), symbols=[sub_symbol])),
                "unsupported_code_class:sub_m68k", "injected main code naming a sub-CPU symbol")
        refused(cli("check", mods.project(edits=[(module + sled, call, dict(main_edit, length=len(call) + 2))])), "unsupported_code_class:layout_changing", "a code patch whose replacement changes the size")
        refused(cli("check", mods.project(guest="free * 0x%x 0x100 0x00\norg * 0x%x\n  rts\n" % (module + free, module + free))), "E_GUEST_ADDRESS", "Sega CD code without an execution address")

        # ---- image-level rejections (before any build) ----
        refused(build(mods.project(edits=[(len(mods.track) - 2, b"\x4e\x71\x4e\x71", dict(main_edit, original=b""))]), "o-grow"), "unsupported_code_class:layout_changing", "a code patch that would grow the file")
        refused(build(mods.project(guest="free * 0x%x 0x100 0x00\norg * 0x%x at 0x%x\n  rts\n" % (len(mods.track) - 16, len(mods.track) - 16, base)), "o-growfree"),
                "unsupported_code_class:layout_changing", "injection past the end of track 1")
        refused(build(mods.project(guest=guest.replace(" 0x00\n", " 0xff\n", 1)), "o-notfree"), "E_GUEST_RANGE_NOT_FREE", "a free range whose original bytes are not the fill")
        refused(build(mods.project(edits=[(module + free, b"\x00\x01", {})], guest=guest), "o-collide"), "E_GUEST_COLLISION", "a data edit inside the injection's free range")
        refused(build(mods.project(edits=[(module + sled, call, dict(main_edit, original=b"\0" * len(call)))]), "o-mismatch"), "E_DISC_ORIGINAL_MISMATCH", "an expected-value mismatch")
        check(not any((tmp / n).exists() for n in ("o-grow", "o-growfree", "o-notfree", "o-collide", "o-mismatch")), "a refused plan creates no build folder at all")
        refused(cli("apply", mods.project(edits=edits), "--cue", fx / "disc" / "disc.cue", "--out", tmp / "o-apply"), "E_DISC_CODE_EVIDENCE", "`mod apply` refuses a code patch")

        # ---- the supported patch: the Vanilla build proves the bytes, the Patched build changes behavior ----
        good = mods.project(edits=edits, guest=guest)
        code, doc, err = build(good, "good")
        check(code == 0 and doc.get("ok") is True and doc.get("code_patched") is True and doc.get("blocks_injected") == 1 and doc.get("edits_applied") == 2,
              "a proven main-code patch with bounded injection builds: %s %s" % (doc, err[-500:]))
        if not doc.get("ok"):
            return finish()
        check(doc["original_disc_identity"] == mods.disc["identity"] and doc["effective_disc_identity"] == oracle.read_disc(tmp / "good" / "disc" / "disc.cue")["identity"] != mods.disc["identity"],
              "the patched disc has its own identity (independent oracle)")
        vanilla_status = json.loads((tmp / "good" / "vanilla" / "status.json").read_text())
        modded_status = json.loads((tmp / "good" / "modded" / "status.json").read_text())
        check(vanilla_status["program_identity"] != modded_status["program_identity"], "the Patched program has its own identity")
        proven = [e for e in vanilla_status["disc_sources"]["extents"] if e["cpu"] == "main"]
        check([(e["offset"], e["base"], e["length"]) for e in proven] == [(module, base, 4096)], "the evidence is exactly the main module's disc extent: %s" % proven)
        described = json.loads(subprocess.run([str(tmp / "good" / "modded" / ("program" + (".exe" if os.name == "nt" else ""))), "--describe"], capture_output=True, text=True, timeout=60).stdout)
        check(described["disc_identity"] == doc["effective_disc_identity"], "the Patched program embeds the effective disc identity")

        code, diff, err = cli("disc-diff-run", tmp / "good", "--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--budget", BUDGET, "--timeout", "120")
        check(code == 0 and diff.get("ok") is True and diff.get("identical") is False, "the differential run reports a divergence: %s %s" % (diff, err[-300:]))
        fields = {d["field"]: d for d in diff.get("differences", [])}
        report = json.loads((tmp / "good" / "modded-report.json").read_text())
        before = json.loads((tmp / "good" / "vanilla-report.json").read_text())

        def word(r, address):
            at = 2 * (address - 0xFFC000)
            return int(r["result_block"][at:at + 4], 16)

        check(report["result"] == before["result"] == "idle", "both programs run to the end")
        check((word(before, mark), word(before, mark + 2), word(before, mark + 4)) == (0xA001, 0, p["data_word"]), "Vanilla: the original marker, no injected word, the original data word")
        check((word(report, mark), word(report, mark + 2), word(report, mark + 4)) == (0xB007, 0xC0DE, p["data_word"]),
              "Patched: the edited immediate and the injected routine's word (the generated-native program runs both patches)")
        check("result_block" in fields and "disc_identity" not in fields and "program_identity" not in fields, "the divergence names the result block, never the identities: %s" % sorted(fields))
        check({f.name: f.read_bytes() for f in (fx / "disc").iterdir()} == original_files, "the original disc is never written")

        # A data edit beside the patch: visible in the run, allowed because the build proved no code there.
        data = mods.project(edits=[(p["data_offset"], (0x5678).to_bytes(2, "big"), {})])
        code, doc, err = build(data, "data")
        check(code == 0 and doc.get("code_patched") is False, "a data edit outside every proven code extent builds: %s %s" % (doc, err[-300:]))
        if doc.get("ok"):
            code, diff, _ = cli("disc-diff-run", tmp / "data", "--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--budget", BUDGET, "--timeout", "120")
            report = json.loads((tmp / "data" / "modded-report.json").read_text())
            check(code == 0 and diff.get("identical") is False and word(report, mark + 4) == 0x5678, "its run reads the new data word")
        refused(build(data, "data"), "E_DISC_OUTPUT", "an existing build folder is not reused")

        # ---- evidence rejections ----
        def evidence_refused(project, name, klass, label):
            refused(build(project, name), klass, label)
            check((tmp / name / "vanilla" / "status.json").exists() and not (tmp / name / "disc").exists() and not (tmp / name / "modded").exists() and not (tmp / name / "disc-mod.json").exists(),
                  label + ": refused from the Vanilla evidence; no patched disc or program exists")

        sub_at = p["sub_offset"] + 8
        evidence_refused(mods.project(edits=[(sub_at, b"\x4e\x71", {"code_cpu": "main", "load_address": "0x%x" % (p["sub_base"] + 8)})]), "o-sub", "unsupported_code_class:sub_m68k",
                         "sub-CPU module bytes declared as main code")
        evidence_refused(mods.project(edits=[(sub_at, b"\x4e\x71", {})]), "o-subdata", "unsupported_code_class:sub_m68k", "sub-CPU module bytes written as `data`")
        evidence_refused(mods.project(edits=[(module + sled, b"\x4e\x71", {})]), "o-undeclared", "unsupported_code_class:undeclared_code", "proven main code written as `data`")
        evidence_refused(mods.project(edits=[(p["data_offset"] + 2, b"\x4e\x71", {"code_cpu": "main", "load_address": "0x%x" % (base + 0x4002)})]), "o-unproven",
                         "unsupported_code_class:not_disc_proven", "disc bytes no build proved to be code")
        evidence_refused(mods.project(edits=[(module + sled, b"\x4e\x71", {"code_cpu": "main", "load_address": "0x%x" % (base + sled + 2)})]), "o-placement",
                         "unsupported_code_class:not_disc_proven", "proven bytes at an address the build did not prove")
        evidence_refused(mods.project(edits=[(module + sled, b"\x4e\x71", {"code_cpu": "main", "load_address": "0xff8000"})]), "o-ramonly", "unsupported_code_class:not_disc_proven",
                         "a RAM address no disc extent reaches")
        # An injected @symbol whose module context the evidence does not support.
        wrong_module = {"name": "entry", "address": "0x%x" % base, "provenance": "verified", "cpu": "main", "module": "boot"}
        evidence_refused(mods.project(guest="free * 0x%x 0x100 0x00\norg * 0x%x at 0x%x\n  jsr @entry\n  rts\n" % (module + free, module + free, base + free), symbols=[wrong_module]), "o-module",
                         "unsupported_code_class:unrecognized_module", "a symbol whose module context the build does not prove")
        evidence_refused(mods.project(guest="free * 0x%x 0x100 0x00\norg * 0x%x at 0x%x\n  jsr @entry\n  rts\n" % (module + free, module + free, base + free),
                                      symbols=[dict(wrong_module, module="/NOPE.BIN;1")]), "o-module2", "unsupported_code_class:unrecognized_module", "a symbol naming a module the disc does not have")

        # The module rule reads what the ASSEMBLER resolved: whitespace the assembler drops (`@ entry`) cannot hide a symbol from it.
        for i, operand in enumerate(("@ entry", "@\tentry", "@en try")):
            evidence_refused(mods.project(guest="free * 0x%x 0x100 0x00\norg * 0x%x at 0x%x\n  jsr %s\n  rts\n" % (module + free, module + free, base + free, operand),
                                          symbols=[dict(wrong_module, module="/NOPE.BIN;1"), dict(wrong_module, name="en")]), "o-space%d" % i,
                             "unsupported_code_class:unrecognized_module", "the symbol written as %r" % operand)

        # Nothing is ever created in the original disc's folder, by the output folder or the object cache.
        listing = sorted(p.name for p in (fx / "disc").iterdir())
        refused(cli("disc-build", good, "--out", fx / "disc" / "sub" / "deeper", *disc_args), "E_DISC_OUTPUT", "a nested output inside the original disc's folder")
        refused(cli("disc-build", good, "--out", tmp / "o-cache", "--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--runtime-dir", ROOT, "--cc", CC, "--budget", BUDGET,
                    "--cache-dir", fx / "disc" / "cachehere"), "E_DISC_OUTPUT", "an object cache inside the original disc's folder")
        check(sorted(p.name for p in (fx / "disc").iterdir()) == listing and not (tmp / "o-cache").exists(), "neither refusal creates anything")
        # A spelling that passes THROUGH the originals' folder (`<originals>/new/../../x`) resolves outside it: the build uses the resolved form, so `new` is never created.
        through = str(fx / "disc") + "/new/../../through-out"
        code, doc, err = cli("disc-build", good, "--out", through, "--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--runtime-dir", ROOT, "--cc", CC, "--budget", BUDGET, "--jobs", "4",
                             "--cache-dir", str(fx / "disc") + "/new2/../../../objects")
        check(code == 0 and (fx / "through-out" / "disc-mod.json").exists() and sorted(p.name for p in (fx / "disc").iterdir()) == listing,
              "a path through the originals' folder builds at its resolved place and creates nothing in that folder: %s %s" % (doc, err[-300:]))

        # No option that turns a proof source off, and no test-only control, is forwarded to the build.
        for option in ("--static-boot", "--static-discovery", "--boot-fixture-profiles", "--pass-set", "--viewer-host-source"):
            code, _, _ = cli("disc-build", good, "--out", tmp / "o-option", *disc_args, option, "0")
            check(code == 2 and not (tmp / "o-option").exists(), "`%s` is not accepted" % option)

        # A required other mod is never silently absent.
        needy = mods.project(edits=[(p["data_offset"], (0x5678).to_bytes(2, "big"), {})])
        with open(needy / "mod.toml", "a") as f:
            f.write('\n[[dependency]]\nid = "other-mod"\n')
        refused(cli("disc-build", needy, "--out", tmp / "o-needy", *disc_args), "E_SET_MISSING_DEPENDENCY", "a disc mod with an unmet dependency")

        # The differential run executes only the two programs a build writes, whatever a folder's record says.
        record = (tmp / "good" / "disc-mod.json").read_text()
        for old, new in (('"vanilla/program"', '"/bin/true"'), ('"modded/program"', '"modded/../../program"'), ('"disc/disc.cue"', '"disc/../../x.cue"')):
            if old not in record:
                continue   # the Windows program names carry .exe
            forged = tmp / "forged"
            forged.mkdir(exist_ok=True)
            (forged / "disc-mod.json").write_text(record.replace(old, new))
            refused(cli("disc-diff-run", forged, "--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--budget", BUDGET, "--timeout", "60"), "E_DISC_BUILD", "a forged record naming %s" % new)

        # ---- dynamic code: the bytes that run are made in RAM ----
        dfx, dtruth = fixture(tmp, "main_decoded")
        dp = dtruth["patch"]
        dmods = Mod(tmp, dfx)
        dynamic = dmods.project(edits=[(dp["module_offset"] + dp["decode_at"], b"\x4e\x71", {"code_cpu": "main", "load_address": "0x%x" % dp["produced_base"]})])
        refused(cli("disc-build", dynamic, "--out", tmp / "o-dynamic", "--cue", dfx / "disc" / "disc.cue", "--bios", dfx / "bios.bin", "--runtime-dir", ROOT, "--cc", CC, "--budget", BUDGET,
                    "--jobs", "4", "--cache-dir", cache), "unsupported_code_class:dynamic", "a patch of code that is decoded into RAM at run time")
        status = json.loads((tmp / "o-dynamic" / "vanilla" / "status.json").read_text())
        check(any(d["cpu"] == "main" and d["kind"] == "produced" and d["base"] == dp["produced_base"] for d in status["disc_sources"]["dynamic"]) and not (tmp / "o-dynamic" / "disc").exists(),
              "the Vanilla build reports the main-side produced image as non-disc code")

        # ---- the BIOS requirement and manual evidence ----
        other = tmp / "other-bios.bin"
        blob = bytearray((fx / "bios.bin").read_bytes())
        blob[-1] ^= 1
        other.write_bytes(bytes(blob))
        refused(cli("disc-build", good, "--out", tmp / "o-bios", "--cue", fx / "disc" / "disc.cue", "--bios", other, "--runtime-dir", ROOT, "--cc", CC, "--budget", BUDGET), "E_DISC_BIOS", "another BIOS")
        code, _, err = cli("disc-build", good, "--out", tmp / "o-emit", *disc_args, "--emit-arg", "--disc-image")
        check(code == 2 and not (tmp / "o-emit").exists(), "`--emit-arg` (hand-admitted extents are never evidence) is not accepted")
        code, _, _ = cli("disc-build", good, "--out", tmp / "o-nobudget", "--cue", fx / "disc" / "disc.cue", "--bios", fx / "bios.bin", "--runtime-dir", ROOT, "--cc", CC)
        check(code == 2, "an explicit instruction budget is required")
        refused(cli("session", good, "--rom", fx / "bios.bin"), "E_SOURCE_PLATFORM", "a cartridge command on a disc package")
    return finish()


def finish():
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
