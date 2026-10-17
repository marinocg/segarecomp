"""SEG-064-T002 (ADR 0142): the Sega CD BIOS-image profile mechanism proof on a project-authored synthetic image.

Builds the synthetic BIOS-like image (tools/sega_cd_bios_fixture.py), runs the bounded build-time materialization fixed point
(tools/sega_cd_bios_build.py: emit -> strict-C11 compile -> build pass -> admit -> ... -> confirming run) and checks:
  - the provenance chain (epoch image, then two operand templates) and that the materialized image equals the fixture's own
    pre-transform sub image (computed independently by the fixture, not by the runtime);
  - the positive run against hand-derived results, and - when the pinned Musashi checkout is configured - against the independent
    two-processor reference in ROM mode (which knows nothing about images or selection);
  - every negative class (stop before the invalid image retires anything), BIOS refusal, emission refusals, instruction budget;
  - determinism over repeated runs and >= 8 deadline partitions.
usage: sega_cd_bios_profile_test.py <segarecomp> <cc> <project root>"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
BUDGET = 5_000_000
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def run_program(exe, disc, bios, *extra, budget=BUDGET):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", str(budget), *map(str, extra)],
                         capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


def emit(cue, bios, out, *extra):
    r = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(cue), "--bios", str(bios), "--out", str(out), *map(str, extra)],
                       capture_output=True, text=True, timeout=300)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else {})


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    disc = tmp / "disc"
    common.build_disc(disc, "comm")
    fx = tmp / "fx"
    expected = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx)], check=True, capture_output=True,
                                         text=True).stdout)
    bios = fx / "bios.bin"
    copy = expected["copy_image"]
    copy_spec = "%s:0x%x:0x%x:0x%x" % (copy["processor"], copy["base"], copy["rom_offset"], copy["length"])

    # ---- the bounded fixed point ----
    built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"),
                            "--bios", str(bios), "--work", str(tmp / "work"), "--copy-image", copy_spec], capture_output=True, text=True,
                           timeout=1800)
    summary = json.loads(built.stdout) if built.stdout.strip() else {}
    check(built.returncode == 0 and summary.get("ok"), "materialization fixed point: %s" % built.stdout[-800:])
    if not summary.get("ok"):
        sys.exit(1)
    kinds = [h.get("admitted", {}).get("kind") for h in summary["history"]]
    check(kinds == ["epoch", "operand_template", "operand_template", "operand_template", None], "admission order %s" % kinds)
    epoch = summary["history"][0]["admitted"]
    check(epoch["length"] == expected["sub_image_length"] and epoch["sha256"] == expected["sub_image_sha256"],
          "materialized image equals the fixture's own pre-transform image")
    templates = {(h["admitted"]["cpu"], h["admitted"]["pc"]) for h in summary["history"] if h.get("admitted", {}).get("kind") == "operand_template"}
    check(templates == {("main", expected["thunks"]["main"]), ("sub", expected["thunks"]["sub"]), ("sub", expected["thunks"]["sub_in_image"])},
          "templates at the three thunks (one already inside the materialized image)")
    emitted = summary["emit"]
    check(emitted["templates"] == 3 and emitted["deduplicated"] >= 1 and emitted["processors"][1]["variant_pcs"] >= 1,
          "overlapping images: deduplicated identical variants and live-byte variant PCs (%s)" % emitted)
    authorities = {i["name"]: i["authority"] for i in emitted["images"]}
    check(authorities == {"bios": "immutable_input", "copy0": "static_proof", "epoch0": "bounded_build_time_materialization"},
          "image authorities %s" % authorities)
    rom = next(i for i in emitted["images"] if i["name"] == "bios")
    check(rom["compiled"] + rom["rejected"] == expected["rom_size"] // 2 and rom["rejected"] >= 1, "broad admission over every aligned start")
    print("cost: c_bytes=%d functions=%d compile_seconds=%s" % (summary["c_bytes"], emitted["functions"], summary["compile_seconds"]))
    exe = pathlib.Path(summary["program"])

    # ---- the positive run ----
    code, report = run_program(exe, disc, bios)
    check(code == 0 and report.get("result") == "idle", "positive run idle (%s)" % report.get("stop"))
    block = bytes.fromhex(report["result_block"])
    for offset, value in expected["result"].items():
        got = int.from_bytes(block[int(offset):int(offset) + 2], "big")
        check(got == value, "result +%s = %04x, expected %04x" % (offset, got, value))
    check(block[0xFF] == 0xA5, "main finished")
    check(report["ga"]["comstat"] == expected["sub_comstat"], "sub COMSTAT %s" % report["ga"]["comstat"])
    check(report["bios_profile"]["epochs"] == 1 and report["bios_profile"]["epoch_image"] == 0, "one verified epoch")

    # ---- independent reference (pinned Musashi, ROM mode) ----
    checkout = common.musashi_checkout()
    if checkout is None:
        print("note: pinned Musashi checkout not configured; the independent reference comparison is skipped")
    else:
        ref_exe = common.build_reference(checkout, CC, tmp / "musashi")
        ref_dir = tmp / "ref"
        ref_dir.mkdir()
        r = subprocess.run([str(ref_exe), "--rom", str(bios), str(ref_dir)], capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
        check(r.returncode == 0, "reference ran: %s" % r.stderr[-300:])
        for spec in expected["positive_variants"]:  # the SBRQ-release epoch and the repeated reset, independently
            vdir = tmp / ("ref-" + spec.replace(":", "-").replace("=", "-"))
            vdir.mkdir()
            rv = subprocess.run([str(ref_exe), "--rom", str(bios), str(vdir), spec], capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
            _, ours = run_program(exe, disc, bios, "--set", spec)
            check(rv.returncode == 0 and json.loads((vdir / "state.json").read_text())["comstat"] == ours["ga"]["comstat"] and
                  hashlib.sha256((vdir / "prg_ram.bin").read_bytes()).hexdigest() == ours["digests"]["prg_ram"] and
                  hashlib.sha256((vdir / "work_ram.bin").read_bytes()).hexdigest() == ours["digests"]["work_ram"],
                  "reference agrees on variant %s" % spec)
        if r.returncode == 0:
            state = json.loads((ref_dir / "state.json").read_text())
            work = (ref_dir / "work_ram.bin").read_bytes()
            prg = (ref_dir / "prg_ram.bin").read_bytes()
            check(hashlib.sha256(work).hexdigest() == report["digests"]["work_ram"], "reference work RAM")
            check(hashlib.sha256(prg).hexdigest() == report["digests"]["prg_ram"], "reference PRG RAM")
            check(state["comstat"] == report["ga"]["comstat"], "reference COMSTAT")
            for k in range(2):
                for field in ("d", "a", "pc"):
                    check(state["cpus"][k][field] == report["cpus"][k][field], "reference cpu %d %s" % (k, field))

    # ---- determinism: repeats and >= 8 deadline partitions ----
    for _ in range(2):
        check(run_program(exe, disc, bios)[1] == report, "repeat run identical")
    for seed in range(8):
        check(run_program(exe, disc, bios, "--slice-seed", seed)[1] == report, "deadline partition seed %d identical" % seed)
    check(run_program(exe, disc, bios, "--slice-ticks", 1000003)[1] == report, "fixed deadline slicing identical")

    # ---- the SBRQ-release epoch path and repeated resets (two epochs) ----
    for spec, epochs in expected["positive_variants"].items():
        code, alt = run_program(exe, disc, bios, "--set", spec)
        check(code == 0 and alt.get("result_block")[:0x20 * 2] == report["result_block"][:0x20 * 2] and
              alt["ga"]["comstat"][:6] == report["ga"]["comstat"][:6], "variant %s reaches the same results (%s)" % (spec, alt.get("stop")))
        check(alt.get("bios_profile", {}).get("epochs") == epochs and alt["ga"]["comstat"][6] == epochs,
              "variant %s: %s verified epoch(s), each one a reset entry of the sub (COMSTAT6)" % (spec, epochs))

    # ---- negatives: typed stops before an instruction from the invalid image retires ----
    for spec, (cls, cpu, pc) in expected["negatives"].items():
        code, neg = run_program(exe, disc, bios, "--set", spec)
        stop = neg.get("stop", {})
        check(code == 10 and stop.get("class") == cls and stop.get("cpu") == cpu and (pc is None or stop.get("pc") == pc),
              "negative %s -> %s" % (spec, stop))
        if code == 10:
            check(neg["cpus"][cpu]["pc"] == stop["pc"] or cls == "materialized_image_unverified", "%s: the stopped instruction did not retire" % spec)
            if cls == "materialized_image_unverified":
                check(neg["cpus"][1]["retired"] == 0, "%s: the sub retired nothing from the unverified image" % spec)

    # ---- a build pass never re-proposes an admitted template: an uncompiled destination is an ordinary stop ----
    for spec in ("main:d4=1", "main:d4=2"):
        pass_dir = tmp / ("pass-" + spec.replace(":", "-").replace("=", "-"))
        pass_dir.mkdir()
        code, neg = run_program(exe, disc, bios, "--set", spec, "--materialize-dir", pass_dir)
        check(code == 10 and neg["stop"]["class"] == "entry_not_compiled" and not (pass_dir / "capture.json").exists(),
              "%s in a build pass: typed stop, no frontier capture" % spec)

    # ---- instruction budget ----
    code, short = run_program(exe, disc, bios, budget=500)
    check(code == 11 and short.get("result") == "budget_exhausted", "budget exhaustion is typed")

    # ---- BIOS refusal before any guest instruction ----
    def refused(path):
        out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(path), "--instruction-budget", "100"], capture_output=True,
                             text=True, timeout=60)
        return out.returncode, json.loads(out.stdout) if out.stdout.strip() else {}
    wrong = tmp / "wrong.bin"
    data = bytearray(bios.read_bytes())
    data[0x300] ^= 0xFF
    wrong.write_bytes(bytes(data))
    short_image = tmp / "short.bin"
    short_image.write_bytes(bios.read_bytes()[:-2])
    for path, error in ((tmp / "missing.bin", "bios_missing"), (wrong, "bios_identity_mismatch"), (short_image, "bios_size_invalid")):
        code, result = refused(path)
        check(code == 21 and result.get("result") == "bios_refused" and result.get("error") == error, "refusal %s -> %s" % (error, result))

    # ---- emission refusals ----
    def variant(name, mutate):
        image = bytearray(bios.read_bytes())
        mutate(image)
        path = tmp / name
        path.write_bytes(bytes(image))
        return path
    no_header = variant("noheader.bin", lambda b: b.__setitem__(slice(0x100, 0x104), b"XXXX"))
    odd_reset = variant("oddreset.bin", lambda b: b.__setitem__(7, b[7] | 1))
    for path, error in ((no_header, "bios_header_invalid"), (odd_reset, "bios_reset_vector_invalid")):
        code, result = emit(disc / "disc.cue", path, tmp / "x.c")
        check(code == 7 and result.get("error") == error, "emission refusal %s -> %s" % (error, result))
    admits = summary["admissions"]
    snapshot = admits[0].split(":", 2)[2]  # the path is the remainder (Windows drive letters contain ":")
    bad = [
        (["--copy-image", "main:0x480:0xc00:0x48"], "copy_invalid"),  # main has no RAM there: the wrong memory view
        (["--copy-image", copy_spec, "--admit", "epoch:%s:%s" % ("0" * 64, snapshot)], "provenance_mismatch"),
        (["--copy-image", copy_spec, "--admit", admits[0], "--admit",
          "template:%s:main:0x%x:4eb9000004600000" % (summary["history"][1]["program_identity"], expected["thunks"]["main"])], "template_invalid"),
        (["--copy-image", copy_spec, "--admit", admits[0], "--admit",
          "template:%s:main:0x%x:4ef9000004600000" % (summary["history"][1]["program_identity"], 0x0480)], "template_invalid"),
    ]
    trailing = tmp / "trailing.bin"
    trailing.write_bytes(pathlib.Path(snapshot).read_bytes() + bytes(256))
    first_identity = summary["history"][0]["program_identity"]
    bad.append((["--copy-image", copy_spec, "--admit", "epoch:%s:%s" % (first_identity, trailing)], "epoch_invalid"))
    for args, error in bad:
        code, result = emit(disc / "disc.cue", bios, tmp / "x.c", *args)
        check(code == 6 and result.get("error") == error, "emission refusal %s -> %s" % (error, result))

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("sega_cd_bios_profile_test: OK")
