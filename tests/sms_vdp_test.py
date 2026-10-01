#!/usr/bin/env python3
"""SEG-009-T004: generated-native SMS VDP port/state/interrupt/mode test with mutation controls.

usage: sms_vdp_test.py <sms_image_emitter> <cc> <product-root>

Project-authored fixtures (tools/sms_fixture_rom.py) are emitted through the SMS generation route, compiled with the SMS
runtime and the real headless driver with the VDP device wiring as strict C11, and run with a finite --frames bound.
Expectations come from the machine contract (docs/architecture/master-system-machine-contract.md section 9) and the
independent Python model (tests/sms_vdp_model.py), never from the C runtime under test:
  * the generated port-operation sequences (vdp_seq_a/b): every read result, the final VRAM/CRAM/register state, the VRAM
    read-back checksum and the VDP trace (register writes, status reads, VRAM/CRAM write counts) equal the model; status
    flags (bits 7-5) are timing/sprite dependent and masked here (they are checked by the interrupt fixtures below);
  * the frame/line interrupt fixture (vdp_irq): handler-observed V counter and status values per phase against the
    contract rules (frame flag line, line counter period and reload, enable gating, pending-but-disabled, acknowledge),
    and the interrupt trace consistent with the VDP flag events;
  * the straddle fixtures (vdp_straddle, vdp_straddle_write): status/V counter reads and an R10 write ordered at the
    instruction-start T-state against the line-193 flag/counter step and the next line's counter reload (U11, VDP part);
  * the reset probe (U9), the typed mode/H counter stops and the blank-display initialisation that must not stop;
  * determinism and slice equivalence (fixed and pseudo-random slices: same digest, traces and artifacts);
  * mutation controls: each deliberately wrong model rule (sms_vdp_model.MUTATIONS) must disagree with the native results.
"""
import pathlib
import sys
import tempfile

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import sms_fixture_rom as builder  # noqa: E402
import sms_vdp_model as model  # noqa: E402
from sms_vdp_native import Native, write_straddle_flip  # noqa: E402

# finite frame bounds per fixture (the reference runs complete in 12-134 frames; the programs idle in HALT afterwards)
FRAMES = {"vdp_seq_a": "60", "vdp_seq_b": "60", "vdp_irq": "60", "vdp_straddle": "200", "vdp_straddle_write": "60", "vdp_reset_probe": "120",
          "vdp_mode_blank_ok": "40", "vdp_mode_display_stop": "40", "vdp_mode_status_stop": "40", "vdp_hcounter_stop": "40"}
LINE, FRAME = 228, 59736
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def seq_outputs(res, seed):
    """The comparable outputs of a vdp_seq run: native (from artifacts) and model."""
    reads, kinds, (hl, bc), vdp = model.run_sequence(seed)
    ram = res["ram"]
    count = len(reads)
    end = ram[0x1F4] | (ram[0x1F5] << 8)
    masked = [(v & 0x1F) if k == "status" else v for v, k in zip(ram[0x200:0x200 + count], kinds)]
    native = {"end": end, "reads": masked, "hash": (ram[0x1F0] | ram[0x1F1] << 8, ram[0x1F2] | ram[0x1F3] << 8),
              "vram": res["vram"], "cram": res["cram"], "regs": res["vdp"]["regs"]}
    expected = {"end": 0xC200 + count, "reads": reads, "hash": (hl, bc), "vram": bytes(vdp.vram), "cram": bytes(vdp.cram),
                "regs": list(vdp.reg)}
    return native, expected, vdp, kinds


def check_seq(res, seed, label):
    check(res["code"] == 0 and res["ram"][0x1FF] == 0xA5, "%s: program did not complete (exit %s)" % (label, res["code"]))
    native, expected, vdp, kinds = seq_outputs(res, seed)
    for key in ("end", "reads", "hash", "vram", "cram", "regs"):
        if native[key] != expected[key]:
            detail = ""
            if key == "reads":
                first = next((i for i, (a, b) in enumerate(zip(native[key], expected[key])) if a != b), None)
                detail = " (first difference at read %s)" % first
            check(False, "%s: %s differs from the model%s" % (label, key, detail))
    v = res["vdp"]
    check((v["address"], v["code"], v["buffer"], v["latch"]) ==
          (vdp.address, vdp.code, vdp.buffer, 0 if vdp.first is None else 1), "%s: final address/code/buffer/latch differ" % label)
    trace = res["vdp_trace"]
    check([(int(a[2]), int(a[3], 16)) for a in trace if a[1] == "reg"] == vdp.reg_writes,
          "%s: register-write trace differs from the model" % label)
    check(sum(1 for a in trace if a[1] == "status") == kinds.count("status"), "%s: status-read trace count" % label)
    check(sum(int(a[2]) for a in trace if a[1] == "vram") == vdp.vram_writes, "%s: VRAM write summaries do not add up" % label)
    check(sum(int(a[2]) for a in trace if a[1] == "cram") == vdp.cram_writes, "%s: CRAM write summaries do not add up" % label)
    check(res["vdp"]["trace_dropped"] == 0, "%s: VDP trace overflowed" % label)
    return native, expected


def vdp_irq_expectations():
    """Per-phase (V counter, status) lists of the vdp_irq fixture from the contract (sources in tools/sms_fixture_rom.py)."""
    line15 = [line for line in model.line_interrupt_lines(15)]
    return [
        ("frame IRQ, 192-line", [0xC1, 0xC1], [0x9F, 0x9F]),
        ("frame IRQ, 224-line", [0xE1, 0xE1], [0x9F, 0x9F]),
        ("line IRQ R10=15", line15, [0x1F] * len(line15)),
        ("line IRQ R10=0 from the start of a frame", list(range(100)), [0x1F] * 100),
        ("frame flag pending while disabled, enabled at line 208", [0xD0], [0x9F]),
        ("line flag pending while disabled, enabled at line 64", [0x40], [0x1F]),
        ("status read clears a pending line flag before the enable", [0x2F], [0x1F]),
        ("line IRQ R10=15 across the frame boundary", line15 + [15, 31], [0x1F] * len(line15) + [0x9F, 0x1F]),
        ("R10 written at line 191/192: the new value applies from the next reload", list(range(30)), [0x9F] + [0x1F] * 29),
    ]


def check_irq(res):
    ram = res["ram"]
    check(res["code"] == 0 and ram[0x1FF] == 0xA5, "vdp_irq: program did not complete (exit %s)" % res["code"])
    bounds = [ram[0xE0 + i] for i in range(9)]
    starts = [0] + bounds[:-1]
    check(bounds[0] == 2 and bounds[1] == 4 and bounds[2] == 16 and bounds[3] == 116 and bounds[4] == 117 and bounds[5] == 118
          and bounds[6] == 119 and bounds[7] == 133 and bounds[8] == 163, "vdp_irq: interrupt counts per phase %r" % bounds)
    for (name, vs, ss), a, b in zip(vdp_irq_expectations(), starts, bounds):
        got_v = [ram[0x100 + i] for i in range(a, b)]
        got_s = [ram[0x200 + i] for i in range(a, b)]
        check(got_v == vs, "vdp_irq %s: V counters %r, expected %r" % (name, got_v, vs))
        check(got_s == ss, "vdp_irq %s: status bytes %r, expected %r" % (name, got_s, ss))
    # interrupt trace vs VDP flag/enable events: every assertion is a flag event or an enabling register write, every
    # deassertion a status read or a disabling register write (contract section 7)
    trace = res["vdp_trace"]
    stamps = {kind: {int(a[0]) for a in trace if a[1] == kind} for kind in ("flag_frame", "flag_line", "status", "reg")}
    bad = []
    for cycles, source, event in res["irq"]:
        t = int(cycles)
        if source == "pause":
            continue
        if event == "asserted" and t not in stamps["flag_" + source] | stamps["reg"]:
            bad.append((t, source, event))
        if event == "deasserted" and t not in stamps["status"] | stamps["reg"]:
            bad.append((t, source, event))
    check(not bad, "vdp_irq: interrupt trace entries without a VDP cause: %r" % bad[:4])
    frame_flags = sorted(int(a[0]) for a in trace if a[1] == "flag_frame")
    check(all((t // LINE) % 262 in (193, 225) and t % LINE == 0 for t in frame_flags), "vdp_irq: frame flags off the contract line/offset")
    check(all(t % LINE == 0 and (t // LINE) % 262 == int(a[2]) for a in trace if a[1] == "flag_line" for t in [int(a[0])]),
          "vdp_irq: line flag stamps do not match their line argument")
    return [ram[0x100 + i] for i in range(bounds[7])]  # phases 0-7 (the mutation control compares phase 2)


def check_straddle(res):
    ram = res["ram"]
    check(res["code"] == 0 and ram[0x1FF] == 0xA5, "vdp_straddle: program did not complete (exit %s)" % res["code"])
    steps = builder.VDP_STRADDLE_STEPS
    s = [ram[0x100 + n] >> 7 for n in range(steps)]
    v = [ram[0x140 + n] for n in range(steps)]
    n_s = next((n for n, x in enumerate(s) if x), None)
    n_v = next((n for n, x in enumerate(v) if x == 0xC1), None)
    # Timing from the published Z80 T-states (LD HL,(nn) 16, JP (HL) 4, IN A,(n) 11, NOP 4, IM1 response 13; the halted CPU
    # accepts on a 4-T grid, so the handler entry is T0 + d + 13 with d in 0..3): the series-S read is at T0 + d + 44 + 4 n
    # and the series-V read at T0 + d + 33 + 4 n; both must flip when the read's instruction-start T reaches T0 + 228 (the
    # frame flag and the V counter step at line 193, offset 0).
    check(n_s == 46, "vdp_straddle: status flag flips at n = %s, expected 46" % n_s)
    check(n_v in (48, 49), "vdp_straddle: V counter flips at n = %s, expected 48 or 49" % n_v)
    check(all(x == (1 if n >= n_s else 0) for n, x in enumerate(s)), "vdp_straddle: status series is not a clean step")
    check(all(x == (0xC1 if n >= n_v else 0xC0) for n, x in enumerate(v)), "vdp_straddle: V series is not a clean step %r" % v)
    return {"n_status": n_s, "n_vcounter": n_v}


def check_straddle_write(res):
    ram = res["ram"]
    check(res["code"] == 0 and ram[0x1FF] == 0xA5, "vdp_straddle_write: program did not complete (exit %s)" % res["code"])
    flip, clean = write_straddle_flip(ram, 0x100)
    # R10 = 3 is written by the OUT whose instruction-start T is T0 + d + 69 + 4 n (handler entry T0 + d + 33, status read 11,
    # LD A,n 7, OUT 11, LD A,n 7; d in 0..3): it reaches the reload of the next line's underflow (T0 + 228) from
    # n = 40 (d = 0..3: (159 - d) / 4 rounded up is 40, 39 for d = 3)
    check(clean and flip in (39, 40), "vdp_straddle_write: R10 write/reload flip at n = %s clean=%s, expected 39 or 40" % (flip, clean))
    return flip


def check_reset(res):
    ram = res["ram"]
    check(res["code"] == 0 and ram[0x1FF] == 0xA5, "vdp_reset_probe: program did not complete (exit %s)" % res["code"])
    check(ram[0x100] == 0 and ram[0x101] == 0x1F and list(ram[0x102:0x10A]) == [0] * 8,
          "vdp_reset_probe: first buffer/status/VRAM reads %r" % list(ram[0x100:0x10A]))
    check(ram[0x10A] == 0, "vdp_reset_probe: an interrupt was taken with only the reset registers (R10 = $FF)")
    check(ram[0x10B] != 0, "vdp_reset_probe: no line interrupt after only R10 was written (R0 bit 4 is set at reset)")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        roms = tmp / "roms"
        names = ["vdp_seq_a", "vdp_seq_b", "vdp_irq", "vdp_straddle", "vdp_straddle_write", "vdp_reset_probe", "vdp_mode_blank_ok",
                 "vdp_mode_display_stop", "vdp_mode_status_stop", "vdp_hcounter_stop"]
        args = []
        for n in names:
            args += ["--fixture", n]
        check(builder.main(["--out", str(roms), *args]) == 0, "fixture builder failed")
        native = Native(EMITTER, CC, ROOT, tmp)
        exes = {n: native.build(roms, n) for n in names}
        if native.failures or any(e is None for e in exes.values()):
            print("\n".join(native.failures))
            return 1
        runs = {n: native.execute(exes[n], n, "--frames", FRAMES[n]) for n in names}
        for n, r in runs.items():
            check("status" in r and r["status"]["irq_trace_dropped"] == 0, "%s: no status artifact or a dropped IRQ trace" % n)

        # ---- generated port sequences against the independent model ----
        outputs = {}
        for name, seed in builder.VDP_SEQ_SEEDS.items():
            outputs[name] = check_seq(runs[name], seed, name)
        # ---- interrupts, straddle, reset ----
        check_irq(runs["vdp_irq"])
        check_straddle(runs["vdp_straddle"])
        check_straddle_write(runs["vdp_straddle_write"])
        check_reset(runs["vdp_reset_probe"])

        # ---- typed stops and the blank-display initialisation ----
        ok = runs["vdp_mode_blank_ok"]
        check(ok["code"] == 0 and ok["status"]["sms_error"] == "SMS_OK" and ok["ram"][0x101] == 0xA5,
              "vdp_mode_blank_ok: an unsupported mode with the display blanked must not stop")
        for name, error, exact in (("vdp_mode_display_stop", "SMS_ERROR_VDP_MODE_UNSUPPORTED", LINE),
                                   ("vdp_mode_status_stop", "SMS_ERROR_VDP_MODE_UNSUPPORTED", None),
                                   ("vdp_hcounter_stop", "SMS_ERROR_HCOUNTER_UNRESOLVED", None)):
            r = runs[name]
            check(r["code"] == 4 and r["status"]["sms_error"] == error and not r["status"]["resumable"],
                  "%s: expected the typed stop %s (exit %s, %s)" % (name, error, r["code"], r["status"].get("sms_error")))
            check(r["ram"][0x100] == 0x5A and r["ram"][0x101] == 0, "%s: the guest ran past the stop" % name)
            if exact is not None:
                check(r["status"]["error_cycles"] == exact, "%s: stop stamped at T %s, expected %d (line 1 event)" %
                      (name, r["status"]["error_cycles"], exact))
            else:
                check(r["status"]["error_cycles"] < LINE, "%s: stop stamped at T %s" % (name, r["status"]["error_cycles"]))
        check(runs["vdp_hcounter_stop"]["status"]["error_address"] == 0x7F, "vdp_hcounter_stop: error port")
        again = native.execute(exes["vdp_mode_display_stop"], "again_stop", "--frames", FRAMES["vdp_mode_display_stop"])
        check(again["digest"] == runs["vdp_mode_display_stop"]["digest"] and again["status"] == runs["vdp_mode_display_stop"]["status"],
              "a typed stop is not deterministic")

        # ---- determinism and slice equivalence ----
        for name in ("vdp_seq_a", "vdp_irq"):
            base = runs[name]
            variants = [("rerun", []), ("fixed slices", ["--slice-cycles", "4099"]), ("random slices", ["--slice-seed", "11"])]
            for label, extra in variants:
                r = native.execute(exes[name], name + "_" + label.replace(" ", "_"), "--frames", FRAMES[name], *extra)
                same = (r["digest"] == base["digest"] and r["irq"] == base["irq"] and r["vdp_trace"] == base["vdp_trace"]
                        and r["vram"] == base["vram"] and r["cram"] == base["cram"] and r["vdp"] == base["vdp"])
                check(same, "%s: %s differ from the baseline run" % (name, label))

        # ---- mutation controls: a wrong rule must be detected ----
        detected = {}
        for mutation in model.MUTATIONS:
            if mutation == "line_counter_off_by_one":
                good = model.line_interrupt_lines(15)
                bad = model.line_interrupt_lines(15, mutation=mutation)
                seen = [v for v in check_irq(runs["vdp_irq"]) if v not in (0xC1, 0xE1)][:len(good)]
                detected[mutation] = [line for line in bad] != seen and [line for line in good] == seen
                continue
            found = False
            for name, seed in builder.VDP_SEQ_SEEDS.items():
                native_out, _, _, kinds = seq_outputs(runs[name], seed)
                reads, _, digest, vdp = model.run_sequence(seed, mutation)
                mutant = [(v & 0x1F) if k == "status" else v for v, k in zip(reads, kinds)]
                found = found or mutant != native_out["reads"] or digest != native_out["hash"] or bytes(vdp.cram) != native_out["cram"]
            detected[mutation] = found
        for mutation, hit in detected.items():
            check(hit, "mutation control: %r is not detected by the differential" % mutation)

        failures = native.failures + FAILED
        for f in failures:
            print("FAIL:", f)
        print("sms vdp native: %d failure(s); mutations detected: %s" % (len(failures), ", ".join(sorted(m for m, h in detected.items() if h))))
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
