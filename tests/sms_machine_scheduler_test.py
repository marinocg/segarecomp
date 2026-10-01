#!/usr/bin/env python3
"""SEG-009-T003: generated-native Master System scheduler, interrupt wiring and headless driver.

usage: sms_machine_scheduler_test.py <sms_image_emitter> <cc> <product-root>

Builds project-authored fixtures (tools/sms_fixture_rom.py), emits them through the SMS generation route, compiles each
with the SMS runtime and the real headless driver (platforms/master-system/headless) as strict C11, and runs them with a
finite --frames/--cycle-budget. Expectations are computed here from the published Z80 instruction T-states and the
scheduling/machine contracts (z80-scheduling-contract.md, master-system-machine-contract.md section 2 and 7), never from
the platform code:
  * the frame interrupt (INT level, acknowledged by a status read) and the pause NMI edge are accepted at the expected
    T-states of a halted CPU (4-T halted M1 grid, IM1 13 T, NMI 11 T) with the exact interrupt trace;
  * split-run equivalence: fixed and pseudo-random slicing give the same state digest, traces and device log;
  * budgets: a cycle budget is a resumable report (exit 2), the frame bound wins when both bounds coincide (exit 0);
  * U11 scheduler ordering: every device access (single OUT series, DD-prefix chains, V counter reads) is applied after
    every scanline event with T_event <= T_access (the instruction-start T-state) and before the next one, and the
    fixtures contain accesses whose bus cycle falls after a line start (the adversarial straddles);
  * unimplemented device ports, bad memory control, BIOS image and malformed input fail closed with typed errors;
  * the mapper trace of the bank-crossing fixture.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sms_cc_cache  # noqa: E402
import sms_fixture_rom as builder  # noqa: E402
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sms_u11 as u11  # noqa: E402

PLATFORM = ROOT / "platforms" / "master-system"
RUNTIME = PLATFORM / "runtime"
Z80_INCLUDE = ROOT / "libs" / "codegen" / "c11" / "include"
STRICT = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-D_CRT_SECURE_NO_WARNINGS"]
PSG_LIB = ROOT / "libs" / "device" / "sega" / "psg"
RUNTIME_SOURCES = [RUNTIME / n for n in ("sms_memory.c", "sms_sha256.c", "sms_input.c", "sms_machine.c", "sms_psg.c", "sms_pad.c", "sms_vdp.c")] + [
    PSG_LIB / "src" / "sn76489.c", PLATFORM / "headless" / "sms_audio.c"]
DRIVER = PLATFORM / "headless" / "sms_headless.c"
NO_DEVICES = PLATFORM / "headless" / "sms_devices_none.c"
TEST_DEVICES = ROOT / "tests" / "tools" / "sms_test_devices.c"
LINE, FRAME = 228, 59736
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def run(cmd, timeout=600, env=None):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, env=env)


def build(tmp, roms, name):
    """Emit + compile `name`; returns {'test': exe with stub devices, 'none': exe with no devices} or None."""
    out = tmp / ("gen_" + name)
    r = run([EMITTER, roms / (name + ".sms"), out, "sms", "--manifest", roms / (name + ".mapper.json")])
    check(r.returncode == 0, "%s: emission failed: %s" % (name, (r.stdout + r.stderr)[:300]))
    if r.returncode != 0:
        return None
    units = [out / u for u in (out / "sms.units").read_text().split()]
    objs = []
    for src in units + RUNTIME_SOURCES + [DRIVER, NO_DEVICES, TEST_DEVICES]:
        obj = out / (src.stem + ".o")
        if not obj.exists():
            generated = sms_cc_cache.GENERATED_UNIT_FLAGS if src.parent == out else []  # authored sources keep the full strict set
            c = sms_cc_cache.compile_object([CC, *STRICT, *generated, "-O0", "-I", Z80_INCLUDE, "-I", RUNTIME, "-I", PLATFORM / "headless", "-I", PSG_LIB / "include", "-I", out, "-c", src, "-o", obj])
            check(c.returncode == 0, "%s: %s did not compile as strict C11: %s" % (name, src.name, c.stderr[:1500]))
            if c.returncode != 0:
                return None
        objs.append(obj)
    shared = [o for o in objs if o.stem not in ("sms_devices_none", "sms_test_devices")]
    exes = {}
    for label, dev in (("none", "sms_devices_none"), ("test", "sms_test_devices")):
        exe = out / ("sms_" + label + ".exe")
        link = run([CC, *shared, out / (dev + ".o"), "-o", exe])
        check(link.returncode == 0, "%s: link failed: %s" % (name, link.stderr[:800]))
        exes[label] = exe
    return exes


def execute(exe, tmp, tag, *args, env_log=False, timeout=180):
    artifacts = tmp / ("art_" + tag)
    artifacts.mkdir(exist_ok=True)
    env = None
    log = artifacts / "device.log"
    if env_log:
        import os
        env = dict(os.environ, SMS_TEST_DEVICE_LOG=str(log))
    r = run([exe, *args, "--artifacts", artifacts], timeout=timeout, env=env)
    res = {"code": r.returncode, "stdout": r.stdout, "dir": artifacts}
    if (artifacts / "status.json").exists():
        res["status"] = json.loads((artifacts / "status.json").read_text())
        res["digest"] = (artifacts / "state.sha256").read_text().strip()
        res["irq"] = [tuple(l.split()) for l in (artifacts / "irq.trace").read_text().splitlines()]
        res["mapper"] = [tuple(l.split()) for l in (artifacts / "mapper.trace").read_text().splitlines()]
        res["ram"] = (artifacts / "ram.bin").read_bytes()
    if env_log and log.exists():
        res["log"] = [l.split() for l in log.read_text().splitlines()]
    return res


def ceil_div(a, b):
    return -(-a // b)


def expected_irq_trace(frames, pause_press_frames):
    """Contract model of fixture `sched_irq`: `im 1; ld sp,nn; ei; halt` then frame INT (line 192) and pause NMI events.

    T-states (Zilog UM0080): IM 1 = 8, LD SP,nn = 10, EI = 4, HALT = 4 per halted M1 cycle (the halted CPU is on a 4-T
    grid from the HALT), IM1 response 13, NMI response 11, IN A,(n) 11, LD HL,nn 10, INC (HL) 11, EI 4, RETI/RETN 14,
    JR 12. An interrupt that wakes the halted CPU pushes the address after HALT (`jr main`), so both handlers return
    through `jr main; halt`."""
    halted_at = 8 + 10 + 4 + 4
    events = [(f * FRAME + 192 * LINE, "frame") for f in range(frames)]
    events += [(f * FRAME, "pause") for f in pause_press_frames]
    events.sort()
    trace = []
    for when, kind in events:
        a = halted_at + 4 * ceil_div(max(0, when - halted_at), 4)
        trace.append((when, kind, "asserted"))
        trace.append((a, kind, "accepted"))
        if kind == "frame":
            trace.append((a + 13, kind, "deasserted"))  # IN A,(0xBF) starts after the 13-T response
            halted_at = a + 13 + 11 + 10 + 11 + 4 + 14 + 12 + 4
        else:
            halted_at = a + 11 + 10 + 11 + 14 + 12 + 4
    return trace


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        roms = tmp / "roms"
        names = ["sched_irq", "sched_straddle", "port_open", "port_vdp", "memctl_bad", "bank_crossing", "u11_probe"]
        args = []
        for n in names:
            args += ["--fixture", n]
        check(builder.main(["--out", str(roms), *args]) == 0, "fixture builder failed")
        exes = {n: build(tmp, roms, n) for n in names}
        if FAILED:
            return finish()

        # ---- interrupt wiring: INT level + pause NMI edge against the contract model --------------------------------
        irq = exes["sched_irq"]
        (tmp / "pause.txt").write_text("# hold 6 frames = one press, then a separate press\n"
                                       "0 ------ ------ -\n1 U---1- ------ P\n7 ------ ------ -\n9 ------ ------ P\n10 ------ ------ -\n")
        for label, script, presses in (("nopause", None, []), ("pause", tmp / "pause.txt", [1, 9])):
            frames = 12
            extra = ["--input", script] if script else []
            res = execute(irq["test"], tmp, "irq_" + label, "--frames", str(frames), *extra, env_log=True)
            check(res["code"] == 0 and res["status"]["stop"] == "frame", "irq %s: %r" % (label, res.get("status")))
            want = [(str(t), k, e) for (t, k, e) in expected_irq_trace(frames, presses) if t < res["status"]["cycles"]]
            got = [(t, s, e) for (t, s, e) in res["irq"]]
            # events whose acceptance lies beyond the stop are not in the trace yet; compare the shared prefix
            check(got == want[:len(got)] and len(got) >= len(want) - 1,
                  "irq %s: trace differs from the contract model:\n got %r\nwant %r" % (label, got[:8], want[:8]))
            counts = res["ram"][0x200], res["ram"][0x201]
            check(counts[1] == len(presses), "irq %s: NMI handler ran %d times, want %d" % (label, counts[1], len(presses)))
            check(counts[0] in (frames - 1, frames), "irq %s: INT handler ran %d times" % (label, counts[0]))
            # pause presses are sampled at the frame boundary: the level 'P' held 6 frames yields one NMI
        # the same run twice is byte-identical; slicing does not change the observable result
        base = execute(irq["test"], tmp, "irq_base", "--frames", "10", "--input", tmp / "pause.txt", env_log=True)
        again = execute(irq["test"], tmp, "irq_again", "--frames", "10", "--input", tmp / "pause.txt", env_log=True)
        check(base["digest"] == again["digest"] and base["irq"] == again["irq"] and base["log"] == again["log"],
              "two identical runs differ")
        variants = [("--slice-cycles", "1000"), ("--slice-cycles", "228"), ("--slice-cycles", "59736"), ("--slice-cycles", "7"),
                    ("--slice-seed", "1"), ("--slice-seed", "2"), ("--slice-seed", "3")]
        for flag, value in variants:
            r = execute(irq["test"], tmp, "irq_split_" + flag[2:] + value, "--frames", "10", "--input", tmp / "pause.txt", flag, value, env_log=True)
            check(r["digest"] == base["digest"] and r["irq"] == base["irq"] and r["log"] == base["log"] and r["status"]["cycles"] == base["status"]["cycles"],
                  "split run %s %s differs from the straight run (cycles %s vs %s)" % (flag, value, r["status"]["cycles"], base["status"]["cycles"]))
        one = execute(irq["test"], tmp, "irq_split_1", "--frames", "2", "--slice-cycles", "1", env_log=True)
        two = execute(irq["test"], tmp, "irq_straight_2", "--frames", "2", env_log=True)
        check(one["digest"] == two["digest"], "a 1-T slice sequence differs from the straight run")
        # the input script changes the digest (the control is sensitive)
        check(base["digest"] != execute(irq["test"], tmp, "irq_nopause10", "--frames", "10")["digest"], "digest ignores the input script")

        # ---- budgets: resumable reports ------------------------------------------------------------------------------
        b = execute(irq["test"], tmp, "budget", "--cycle-budget", "100000")
        check(b["code"] == 2 and b["status"]["stop"] == "cycle_budget" and b["status"]["resumable"] is True and
              100000 <= b["status"]["cycles"] < 100000 + 4, "cycle budget is a resumable report: %r" % (b.get("status"),))
        f = execute(irq["test"], tmp, "frame_first", "--frames", "1", "--cycle-budget", "59736")
        check(f["code"] == 0 and f["status"]["stop"] == "frame", "frame wins a coincident bound: %r" % (f.get("status"),))
        c = execute(irq["test"], tmp, "budget_first", "--frames", "1", "--cycle-budget", "59000")
        check(c["code"] == 2 and c["status"]["stop"] == "cycle_budget", "the earlier cycle budget wins: %r" % (c.get("status"),))
        check(run([irq["none"], "--frames", "zero"], timeout=30).returncode == 64, "malformed --frames must be a usage error")
        # F3: out-of-range numerals (ERANGE) are usage errors for every numeric option; the maximum itself is in range
        too_big = ("18446744073709551616", "99999999999999999999999999")
        for option in ("--frames", "--cycle-budget", "--slice-cycles", "--slice-seed"):
            for numeral in too_big:
                check(run([irq["none"], option, numeral], timeout=30).returncode == 64,
                      "%s %s must be a usage error (exit 64)" % (option, numeral))
        check(run([irq["none"], "--frames", "1", "--cycle-budget", "18446744073709551615"], timeout=60).returncode == 0,
              "the maximum uint64 numeral is in range")

        # ---- U11 ordering: accesses at instruction-start T vs scanline events -----------------------------------------
        st = execute(exes["sched_straddle"]["test"], tmp, "straddle", "--frames", "3", env_log=True)
        check(st["code"] == 0, "straddle run: %r" % (st.get("status"),))
        events = [(int(x[1]), int(x[2])) for x in st["log"] if x[0] == "E"]
        check(all(t == n * LINE for n, t in events) and [n for n, _ in events] == list(range(len(events))),
              "scanline events must be exactly every 228 T, in order, starting at T 0")
        check(len(events) >= 3 * 262, "scanline events missing: %d" % len(events))
        accesses = [(x[0], int(x[1], 16), int(x[2]), int(x[3]), int(x[4], 16)) for x in st["log"] if x[0] in "RW"]
        check(bool(accesses), "no device accesses were logged")
        bad = [a for a in accesses if a[3] != (a[2] // LINE) * LINE]
        check(not bad, "device state was not advanced exactly through the events <= T_access: %r" % bad[:3])
        psg = [a for a in accesses if a[0] == "W" and a[1] == 0x7F]
        gaps = [psg[i + 1][2] - psg[i][2] for i in range(len(psg) - 1)]
        plain = [a for i, a in enumerate(psg[:-1]) if gaps[i] == 11]
        chains = [a for i, a in enumerate(psg[:-1]) if gaps[i] == 35]
        # 60 single OUTs (11 T, bus write at +8) and 20 chains of 6 prefixes + OUT (35 T, bus write at +32) per frame
        straddle_plain = [a for a in plain if (a[2] + 8) // LINE != a[2] // LINE]
        straddle_chain = [a for a in chains if (a[2] + 32) // LINE != a[2] // LINE]
        check(len(plain) >= 3 * 59 and len(chains) >= 3 * 19, "PSG write series incomplete: %d plain, %d chain" % (len(plain), len(chains)))
        check(len(straddle_plain) >= 2 and len(straddle_chain) >= 2,
              "fixture has no straddling accesses: %d plain, %d chain" % (len(straddle_plain), len(straddle_chain)))
        counters = [a for a in accesses if a[0] == "R" and a[1] == 0x7E]
        check(len(counters) >= 3 * 29, "V counter read series incomplete: %d" % len(counters))
        check(all(a[4] == (a[2] // LINE) % 262 for a in counters), "V counter reads do not follow the instruction-start line")
        straddle_reads = [a for a in counters if (a[2] + 8) // LINE != a[2] // LINE]
        (tmp / "u11.json").write_text(json.dumps({"plain": len(plain), "plain_straddle": len(straddle_plain),
                                                   "chain": len(chains), "chain_straddle": len(straddle_chain),
                                                   "reads": len(counters), "read_straddle": len(straddle_reads)}))
        sp = execute(exes["sched_straddle"]["test"], tmp, "straddle_split", "--frames", "3", "--slice-seed", "9", env_log=True)
        check(sp["digest"] == st["digest"] and sp["log"] == st["log"], "split straddle run differs from the straight run")

        # ---- U11 probe on this platform: instruction-start ordering makes the flip position independent of the prefixes
        probe = execute(exes["u11_probe"]["test"], tmp, "u11_probe", "--frames", "6")
        check(probe["code"] == 0 and probe["ram"][0xF5] == 4, "u11 probe did not complete: %r" % (probe.get("status"),))
        analysis = u11.analyze(probe["ram"][0x100:])
        check(analysis["monotone"], "u11 probe results are not a monotone flip: %r" % analysis)
        stars = set(analysis["n_star"].values())
        check(len(stars) == 1 and None not in stars, "instruction-start ordering: n* must not depend on the prefix count: %r" % analysis["n_star"])
        print("u11 platform n*(k): %r" % analysis["n_star"])

        # ---- fail-closed ports and control ----------------------------------------------------------------------------
        po = execute(exes["port_open"]["none"], tmp, "port_open", "--frames", "1")
        check(po["code"] == 0 and po["ram"][0x210:0x214] == b"\xff\xff\xff\xff",
              "open/disabled port reads must return $FF without a device: %r" % (po.get("ram", b"")[0x210:0x214],))
        idle = run([exes["port_open"]["none"]], timeout=30)  # no bound: guest halts with interrupts disabled -> halt idle
        check(idle.returncode == 0 and idle.stdout.startswith("stop halt_idle"), "halt-idle stop: %r" % idle.stdout[:80])
        pv = execute(exes["port_vdp"]["none"], tmp, "port_vdp", "--frames", "1")
        check(pv["code"] == 4 and pv["status"]["stop"] == "platform_error" and pv["status"]["resumable"] is False and
              pv["status"]["sms_error"] == "SMS_ERROR_PORT_UNIMPLEMENTED" and pv["status"]["error_address"] & 0xFF == 0xBF and
              pv["status"]["error_value"] == 0x55, "unimplemented VDP port: %r" % (pv.get("status"),))
        check(pv["ram"][0x220] == 0x55 and pv["ram"][0x221] == 0, "the machine must stop at the boundary after the offending OUT")
        mb = execute(exes["memctl_bad"]["none"], tmp, "memctl_bad", "--frames", "1")
        check(mb["code"] == 4 and mb["status"]["sms_error"] == "SMS_ERROR_CONTROL_BIT_UNSUPPORTED" and mb["status"]["error_address"] & 0xFF == 0x3E,
              "incompatible port $3E write: %r" % (mb.get("status"),))
        bios = run([exes["port_open"]["none"], "--bios", "bios.sms", "--frames", "1"], timeout=30)
        check(bios.returncode == 4 and "SMS_ERROR_BIOS_UNSUPPORTED" in bios.stdout, "a supplied BIOS image must be refused")
        check(run([exes["port_open"]["none"], "--frames", "1", "--input", tmp / "missing.txt"], timeout=30).returncode == 64,
              "a missing input script is a usage error")
        (tmp / "bad.txt").write_text("1 ------ ------ P\n0 ------ ------ -\n")
        check(run([exes["port_open"]["none"], "--frames", "1", "--input", tmp / "bad.txt"], timeout=30).returncode == 64,
              "a decreasing frame in the input script is a usage error")

        # ---- mapper trace ---------------------------------------------------------------------------------------------
        bc = execute(exes["bank_crossing"]["none"], tmp, "bank_crossing", "--cycle-budget", "200000")
        check([(r, v) for _, r, v in bc["mapper"]] == [("FFFE", "03"), ("FFFD", "02")], "mapper trace: %r" % bc["mapper"])
        check(all(int(t) > 0 for t, _, _ in bc["mapper"]) and int(bc["mapper"][0][0]) < int(bc["mapper"][1][0]), "mapper trace time order")
    return finish()


def finish():
    if FAILED:
        print("\n".join(FAILED[:30]))
        return 1
    print("sms machine scheduler: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
