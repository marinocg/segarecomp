"""SEG-079-T002 (ADR 0180): batched `JMP (xxx).L` slot-run operand-template admission on a project-authored synthetic slot-table image.

Builds tools/sega_cd_slot_table_fixture.py twice with the bounded materialization fixed point (tools/sega_cd_bios_build.py): batched (a
frontier capture reports the contiguous slot run and ONE admission covers it) and conventional (`--no-template-runs`, one template per round).
Checks: the round counts, an identical observable outcome, hand-derived results, the typed run-time negatives of the slots, and the
fail-closed emitter refusals of a malformed run (bound, hole, mixed opcode, odd operand, outside RAM, overlap, length).
usage: sega_cd_template_run_test.py <segarecomp> <cc> <project root>"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_slot_table_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def build(tmp, name, disc, bios, *extra):
    r = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios", str(bios),
                        "--work", str(tmp / name), *extra], capture_output=True, text=True, timeout=1800)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else {})


def run(exe, disc, bios, *extra):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", "5000000", *map(str, extra)],
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
    expected = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx)], check=True, capture_output=True, text=True).stdout)
    bios = fx / "bios.bin"
    slots = expected["slots"]

    code, batched = build(tmp, "batched", disc, bios)
    check(code == 0 and batched.get("ok"), "batched fixed point: %s" % batched)
    code, plain = build(tmp, "plain", disc, bios, "--no-template-runs")
    check(code == 0 and plain.get("ok"), "conventional fixed point: %s" % plain)
    code, again = build(tmp, "again", disc, bios)
    if not (batched.get("ok") and plain.get("ok") and again.get("ok")):
        sys.exit(1)

    # ---- round economics and the admission shape ----
    check(plain["rounds"] == slots["a_count"] + 4 + 1, "conventional: one round per slot + confirm (%d)" % plain["rounds"])
    check(batched["rounds"] == 5, "batched: table A run, table B run, B mixed-opcode slot, B last slot, confirm (%d)" % batched["rounds"])
    shapes = [(h["admitted"]["kind"], h["admitted"].get("slots", 1)) for h in batched["history"] if h.get("admitted")]
    check(shapes == [("operand_template_run", slots["a_count"]), ("operand_template_run", 2), ("operand_template", 1), ("operand_template", 1)],
          "run admissions %s" % shapes)
    check(batched["history"][0]["admitted"]["base"] == slots["a_base"], "table A run starts at its first slot")
    check(batched["emit"]["templates"] == plain["emit"]["templates"] == slots["a_count"] + 4, "same slot count admitted either way")
    check(batched["history"][0]["program_identity"] == plain["history"][0]["program_identity"], "identical round-0 program")
    check(again["program_identity"] == batched["program_identity"] and again["admissions"] == batched["admissions"], "deterministic batched build")

    # ---- identical observable behaviour ----
    exe_b, exe_p = pathlib.Path(batched["program"]), pathlib.Path(plain["program"])
    cb, rb = run(exe_b, disc, bios)
    cp, rp = run(exe_p, disc, bios)
    check(cb == 0 and rb.get("result") == "idle", "batched positive run idle (%s)" % rb.get("stop"))
    observable = lambda r: {k: r.get(k) for k in ("result", "stop", "cpus", "digests", "result_block", "ticks")}  # noqa: E731
    check(observable(rb) == observable(rp), "batched and conventional programs have identical observable run reports")
    check(batched["outcome"] == plain["outcome"], "identical build-pass outcomes")
    block = bytes.fromhex(rb["result_block"])
    for offset, value in expected["result"].items():
        got = int.from_bytes(block[int(offset):int(offset) + 2], "big")
        check(got == value, "result +%s = %04x, expected %04x" % (offset, got, value))
    check(block[0xFF] == 0xA5, "main finished")

    # ---- run-time verification of every slot is unchanged: typed negatives and the operand-changed positive ----
    for spec, (cls, cpu, pc) in expected["negatives"].items():
        for exe in (exe_b, exe_p):
            code, neg = run(exe, disc, bios, "--set", spec)
            stop = neg.get("stop", {})
            check(code == 10 and stop.get("class") == cls and stop.get("cpu") == cpu and stop.get("pc") == pc, "negative %s -> %s" % (spec, stop))
    for spec, words in expected["variant"].items():
        code, alt = run(exe_b, disc, bios, "--set", spec)
        block = bytes.fromhex(alt.get("result_block", ""))
        check(code == 0 and all(int.from_bytes(block[int(o):int(o) + 2], "big") == v for o, v in words.items()), "variant %s reaches other compiled routine" % spec)

    # ---- fail-closed emitter refusals of a malformed run ----
    first = batched["admissions"][0].split(":")
    check(first[0] == "template-run" and int(first[4]) == slots["a_count"], "run admission syntax %s" % first[:5])
    identity0, cpu, base, count, data = first[1], first[2], int(first[3], 0), int(first[4]), first[5]
    check(batched["history"][0]["program_identity"] == identity0, "the run carries its pass identity")

    def spec_of(identity, address, n, hexdata):
        return "template-run:%s:%s:0x%x:%d:%s" % (identity, cpu, address, n, hexdata)

    cue = disc / "disc.cue"
    code, good = emit(cue, bios, tmp / "x.c", "--admit", batched["admissions"][0])
    check(code == 0 and good.get("ok") and good["templates"] == slots["a_count"], "the captured run emits (%s)" % good)
    slot = lambda k: data[12 * k:12 * k + 12]  # noqa: E731
    mutate = lambda k, text: data[:12 * k] + text + data[12 * k + 12:]  # noqa: E731
    bad = {
        "hole (a slot of zero bytes)": spec_of(identity0, base, count, mutate(4, "000000000000")),
        "mixed opcode (JMP .W in the run)": spec_of(identity0, base, count, mutate(4, "4ef8" + slot(4)[4:8] + "4e71")),
        "JSR in the run": spec_of(identity0, base, count, mutate(4, "4eb9" + slot(4)[4:])),
        "odd operand": spec_of(identity0, base, count, mutate(4, slot(4)[:-2] + "%02x" % (int(slot(4)[-2:], 16) | 1))),
        "outside RAM": spec_of(identity0, 0x400, count, data),
        "odd base": spec_of(identity0, base + 1, count, data),
        "run past the end of RAM": spec_of(identity0, 0xFFFFF8, count, data),
    }
    for name, admit in bad.items():
        code, result = emit(cue, bios, tmp / "x.c", "--admit", admit)
        check(code == 6 and result.get("error") == "template_invalid", "refused: %s -> %s" % (name, result))
    over = 129
    code, result = emit(cue, bios, tmp / "x.c", "--admit", spec_of(identity0, base, over, (slot(0) * over)))
    check(code != 0 and not result.get("ok"), "a run over the 128-slot bound is refused (%s)" % result)
    code, result = emit(cue, bios, tmp / "x.c", "--admit", spec_of(identity0, base, 128, slot(0) * 127))
    check(code != 0 and not result.get("ok"), "a length mismatch is refused (%s)" % result)
    code, result = emit(cue, bios, tmp / "x.c", "--admit", "template-run:%s:%s:0x%x:1:%s" % (identity0, cpu, base, slot(0)))
    check(code != 0 and not result.get("ok"), "a one-slot run record is not a valid run (%s)" % result)
    code, result = emit(cue, bios, tmp / "x.c", "--admit", spec_of("0" * 64, base, count, data))
    check(code == 6 and result.get("error") == "provenance_mismatch", "a broken provenance chain is refused (%s)" % result)
    # overlap with an already admitted slot, and the exact bound of 128 slots
    second = spec_of(batched["history"][1]["program_identity"], base + 6 * (count - 2), 2, data[12 * (count - 2):])
    code, result = emit(cue, bios, tmp / "x.c", "--admit", batched["admissions"][0], "--admit", second)
    check(code == 6 and result.get("error") == "template_invalid" and result.get("index") == 1, "overlapping run refused (%s)" % result)
    full = slot(0)
    code, result = emit(cue, bios, tmp / "x.c", "--admit", spec_of(identity0, base, 128, "".join(slot(k % count) for k in range(128))))
    check(code == 0 and result.get("ok") and result["templates"] == 128, "a 128-slot run is admitted at the bound (%s)" % result)

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("sega_cd_template_run_test: OK")
