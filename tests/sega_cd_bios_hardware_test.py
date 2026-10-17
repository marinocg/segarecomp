"""SEG-064-T003: the main-side hardware of the Sega CD BIOS-image profile on a project-authored image.

Builds tools/sega_cd_bios_hw_fixture.py, runs the bounded materialization fixed point (tools/sega_cd_bios_build.py) and checks:
  - the admission chain: the level-6 vector names a work-RAM JMP (xxx).L thunk built by main; the interrupt enters it uncompiled, the
    build pass reports it as a frontier and it is admitted as an operand template; then the sub epoch image, equal to the fixture's
    own image (SHA-256 computed by the fixture);
  - every result word against the fixture's expectations (version register with the Mega-CD attached, controller port, Z80 BUSACK with
    prefetch data, the write-only Z80 upload, VRAM fill and the DMA status bit, 68K->VRAM DMA from work RAM and from the BIOS image,
    the VBlank flag, three level-6 interrupts through the thunk, the H-INT vector register and BIOS $72, empty-slot open bus);
  - the sub ran from the verified image (COMSTAT0) and the run ends idle;
  - typed stops for every unsupported or unverified case, before the sub retires an instruction from an unverified image;
  - a different BIOS image is refused before any instruction;
  - repeats and >= 8 deadline partitions give identical reports (the epoch tick included).
usage: sega_cd_bios_hardware_test.py <segarecomp> <cc> <project root>"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_hw_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
BUDGET = 3_000_000
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def run_program(exe, disc, bios, *extra, budget=BUDGET):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", str(budget), *map(str, extra)],
                         capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    disc = tmp / "disc"
    common.build_disc(disc, "comm")
    fx = tmp / "fx"
    expected = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx)], check=True, capture_output=True,
                                         text=True).stdout)
    bios = fx / "bios.bin"
    built = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"),
                            "--bios", str(bios), "--work", str(tmp / "work")], capture_output=True, text=True, timeout=1800)
    summary = json.loads(built.stdout) if built.stdout.strip() else {}
    check(built.returncode == 0 and summary.get("ok"), "materialization fixed point: %s" % built.stdout[-800:])
    if not summary.get("ok"):
        sys.exit(1)
    admitted = [h.get("admitted") for h in summary["history"]]
    check(admitted[0] == {"kind": "operand_template", "cpu": "main", "pc": expected["thunk"]},
          "the level-6 RAM thunk is admitted as an operand template (%s)" % admitted[0])
    check(admitted[1] is not None and admitted[1]["kind"] == "epoch" and admitted[1]["length"] == expected["sub_image_length"] and
          admitted[1]["sha256"] == expected["sub_image_sha256"], "the epoch image equals the fixture's sub image")
    check(admitted[2:] == [None] and summary["rounds"] == 3, "two admissions, then the candidate (%s)" % admitted)
    exe = pathlib.Path(summary["program"])

    code, report = run_program(exe, disc, bios)
    check(code == 0 and report.get("result") == "idle", "positive run idle (%s)" % report.get("stop"))
    block = bytes.fromhex(report["result_block"])
    for offset, value in expected["result_words"].items():
        got = int.from_bytes(block[int(offset):int(offset) + 2], "big")
        check(got == value, "result +%s = %04x, expected %04x" % (offset, got, value))
    check(block[0xFF] == 0xA5 and block[0xF0:0xF2] == b"\0\0", "main reached the epoch without a fault")
    check(report["ga"]["comstat"][0] == expected["sub_comstat0"], "the sub ran from the verified image")
    check(report["bios_profile"]["epochs"] == 1 and report["bios_profile"]["epoch_image"] == 0, "one verified epoch")

    # ---- determinism: repeats and deadline partitions (the epoch tick is part of the report) ----
    for _ in range(2):
        check(run_program(exe, disc, bios)[1] == report, "repeat run identical")
    for seed in range(8):
        check(run_program(exe, disc, bios, "--slice-seed", seed)[1] == report, "deadline partition seed %d identical" % seed)
    check(run_program(exe, disc, bios, "--slice-ticks", 777777)[1] == report, "fixed deadline slicing identical")

    # ---- negatives ----
    for spec, (cls, cpu) in expected["negatives"].items():
        code, neg = run_program(exe, disc, bios, "--set", spec)
        stop = neg.get("stop", {})
        check(code == 10 and stop.get("class") == cls and stop.get("cpu") == cpu, "negative %s -> %s" % (spec, stop))
        if cls == "materialized_image_unverified":
            check(neg.get("cpus", [{}, {}])[1].get("retired") == 0, "%s: the sub retired nothing" % spec)
        else:
            check(neg.get("bios_profile", {}).get("epochs") == 0, "%s: stopped before the epoch" % spec)

    # ---- a different image is refused before any instruction ----
    other = tmp / "other.bin"
    data = bytearray(bios.read_bytes())
    data[0x0FF0] ^= 0x01
    other.write_bytes(bytes(data))
    code, refused = run_program(exe, disc, other)
    check(code == 21 and refused.get("error") == "bios_identity_mismatch", "a different BIOS image is refused (%s)" % refused)

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("sega_cd_bios_hardware_test: OK")
