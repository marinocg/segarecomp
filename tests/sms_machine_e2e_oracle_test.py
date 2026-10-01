#!/usr/bin/env python3
"""SEG-009-T008 (ADR 0062): the end-to-end machine fixture against the two pinned Master System references.

usage: sms_machine_e2e_oracle_test.py <sms_image_emitter> <cc> <product-root> [--write-manifest]

Skips cleanly (exit 0) unless SEGARECOMP_SMS_ORACLE_CHECKOUT holds the pinned checkouts (a wrong or dirty pin fails). The
`machine_e2e` fixture (builder-declared Sega mapper) runs generated-native through the platform and black-box through the
libretro cores of Gearsystem and Genesis Plus GX (tests/sms_oracle/libretro_frames_host.c) under the same scripted input.
Compared, per reference:

  * the guest-visible result block (RAM mirror, bank markers at reset and after switches, masked bank value, the same logical
    PC under three banks, undecoded and nationalization port reads, a VRAM read-back sum/xor): exact;
  * every frame log record of the program from guest frame 1 (frame interrupt V counter, sprite flags, four line-interrupt V
    counters, slot-1/slot-2 bank markers: exact; controller ports and pause NMI count: exact after a per-reference constant
    input-phase offset, which is asserted to exist and recorded);
  * framebuffers of every frame after the display enable settled on both machines, through the colour-class bijection (each
    CRAM value maps to exactly one reference RGB and vice versa within the run): exact per pixel outside the U8 fine-scroll
    gap mask (contract U8: the platform keeps the backdrop; Gearsystem shows the wrapped background column and Genesis Plus GX
    a constant colour, three different answers, so the gap is masked and the mask is checked for staleness);
  * PCM: a mixed-signal per-frame RMS comparison under one per-reference gain (the references synthesize band-limited output
    with their own scaling; ADR 0062 section 6): every steady frame of every channel within +/-1 dB of the gain, silent steady
    frames below 5% of it, and the input-phase offset asserted constant.

`--write-manifest` records the comparison in the `reference` section of tests/fixtures/sms-e2e-validation.json; without it the
recorded values are asserted (a reference that starts agreeing, or disagreeing, fails as stale).
"""
import json
import math
import pathlib
import struct
import subprocess
import sys
import tempfile

import sms_e2e_common as c
from sms_e2e_common import builder

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "sms_oracle"))
import pins  # noqa: E402
import sms_oracle_smoke_test as smoke  # noqa: E402

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
WRITE = "--write-manifest" in sys.argv[4:]
FAILED = []
COMPARE_FROM_FRAME = 6     # after the display enable of both machines (frame 4 here, frame 5 in Genesis Plus GX) has settled
RUN_FRAMES = builder.E2E_FRAMES


def check(cond, message):
    if not cond:
        FAILED.append(message)
        print("FAIL:", message)


def run_reference(host, core, rom, script, out, options):
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(host), str(core), str(rom), str(RUN_FRAMES), str(script), str(out), *options], text=True,
                       capture_output=True, timeout=300)
    if r.returncode != 0:
        raise AssertionError("reference host failed (%d): %s" % (r.returncode, r.stderr.strip()))
    meta = [list(map(int, l.split())) for l in (out / "meta.txt").read_text().splitlines()]
    raw = (out / "frames.rgb").read_bytes()
    w, h = meta[0][1], meta[0][2]
    check(all(m[1:3] == [256, 192] for m in meta), "reference frame size %dx%d" % (w, h))
    frames = [raw[i * 256 * 192 * 3:(i + 1) * 256 * 192 * 3] for i in range(len(meta))]
    ram = (out / "ram.bin").read_bytes()
    audio = (out / "audio.raw").read_bytes()
    samples = struct.unpack("<%dh" % (len(audio) // 2), audio)
    mono = [(samples[i] + samples[i + 1]) / 2 for i in range(0, len(samples) - 1, 2)]
    runs, pos = [], 0
    for m in meta:
        runs.append(mono[pos:pos + m[3]])
        pos += m[3]
    return {"frames": frames, "ram": ram, "audio_runs": runs}


def bijection_equal(ours, ref, mask):
    forward, backward = {}, {}
    for i, v in enumerate(ours):
        if i in mask:
            continue
        rgb = ref[3 * i:3 * i + 3]
        if forward.setdefault(v, rgb) != rgb or backward.setdefault(rgb, v) != v:
            return False
    return True


def ac_rms(samples):
    if not samples:
        return 0.0
    mean = sum(samples) / len(samples)
    return math.sqrt(sum((x - mean) ** 2 for x in samples) / len(samples))


def compare(name, native_res, ref, models):
    record = {}
    ram = ref["ram"]
    # 1. result block
    block = {a: ram[a] for a in c.INIT_BLOCK}
    check(block == c.INIT_BLOCK, "%s: init result block differs: %s" % (name, {hex(a): v for a, v in block.items() if v != c.INIT_BLOCK[a]}))
    check((ram[0x112], ram[0x113]) == c.expected_readback() == (native_res["ram"][0x112], native_res["ram"][0x113]), "%s: VRAM read-back" % name)
    record["init_block"] = "exact"
    # 2. frame log records from guest frame 1
    records = c.guest_frames(ram)
    ours = c.guest_frames(native_res["ram"])
    count = min(len(records), builder.E2E_LOGGED_FRAMES)
    check(count >= 30, "%s: only %d frame log records" % (name, len(records)))
    for n in range(1, count):
        for field in (1, 2, 5, 6, 7, 8, 9, 11, 12):
            check(records[n][field] == ours[n][field], "%s: log %d field +%d is %02X, platform %02X" % (name, n, field, records[n][field], ours[n][field]))
    # input phase: the guest frame n observes the scripted state at absolute frame n + k (k per machine, constant)
    def find_offset(field_of, expect_of):
        found = [k for k in range(0, 12) if all(field_of(records[n]) == expect_of(n + k) for n in range(1, count))]
        return found
    pad = find_offset(lambda r: (r[3], r[4]), lambda f: c.ref_ports(*c.input_state(f)[:2], 0xFF))
    pause = find_offset(lambda r: r[10], lambda f: sum(1 for e in c.pause_edges() if e <= f))
    check(len(pad) == 1, "%s: controller log has no unique constant input offset %s" % (name, pad))
    check(len(pause) >= 1, "%s: pause NMI log has no constant input offset" % name)
    accepted = c.accepted_frame_times(native_res["irq"])
    platform_k = accepted[0] // c.FRAME
    check(all((ours[n][3], ours[n][4]) == c.ref_ports(*c.input_state(n + platform_k)[:2], 0xFF) for n in range(1, count)), "platform pad log phase")
    record["log_records_compared"] = count - 1
    record["pad_phase_offset_vs_platform"] = (pad[0] - platform_k) if pad else None
    record["pause_phase_offset_vs_platform"] = (pause[0] - platform_k) if pause else None
    # 3. framebuffers
    writes = c.reg_writes(native_res["vdp_trace"])
    vram, cram = native_res["vram"], native_res["cram"]
    best = None
    for delta in (-2, -1, 0, 1, 2):
        ok = 0
        total = 0
        for f in range(COMPARE_FROM_FRAME, RUN_FRAMES - 2):
            if f + delta >= len(ref["frames"]):
                continue
            total += 1
            ok += bijection_equal(models[f][1], ref["frames"][f + delta], models[f][2])
        if total and ok == total:
            best = delta if best is None else "ambiguous"
    check(best is not None and best != "ambiguous", "%s: no unique exact frame alignment (masked bijection)" % name)
    if best in (None, "ambiguous"):
        return record
    last = RUN_FRAMES - 3
    record["image_frame_offset"] = best
    record["frames_compared"] = [COMPARE_FROM_FRAME, last]
    record["gap_mask"] = "U8 fine-scroll gap (x < R8 & 7 on every line outside the lock)"
    # staleness of the mask: the reference must still differ from the platform inside the gap somewhere
    def gap_differences(f):
        forward = {}
        for i, v in enumerate(models[f][1]):
            if i not in models[f][2]:
                forward.setdefault(v, ref["frames"][f + best][3 * i:3 * i + 3])
        return sum(1 for i in models[f][2] if forward.get(models[f][1][i]) != ref["frames"][f + best][3 * i:3 * i + 3])
    differs = any(gap_differences(f) for f in range(COMPARE_FROM_FRAME, last + 1))
    check(differs, "%s: the reference agrees with the platform inside the U8 gap: the mask is stale" % name)
    wrapped = all(bijection_equal(models[f][3], ref["frames"][f + best], set()) for f in range(COMPARE_FROM_FRAME, last + 1))
    record["gap_content"] = "wrapped background column (exact)" if wrapped else "other (masked)"
    # the bijection itself must be non-trivial: every used CRAM value maps to its own RGB
    used = set()
    for f in range(COMPARE_FROM_FRAME, last + 1):
        used.update(models[f][1])
    check(len(used) >= 20, "%s: only %d colour classes exercised" % (name, len(used)))
    record["colour_classes"] = len(used)
    # 4. audio: per-frame AC RMS under one gain
    end = native_res["status"]["cycles"]
    per_frame = {}
    for k, s in enumerate(c.struct.unpack("<%dh" % (len(native_res["pcm"]) // 2), native_res["pcm"])):
        per_frame.setdefault(c.psg_model.sample_start(k) // c.FRAME, []).append(s)
    ours_rms = [ac_rms(per_frame.get(f, [])) for f in range(RUN_FRAMES)]
    ref_rms_runs = [ac_rms(r) for r in ref["audio_runs"]]
    writes_t = [t for t, _ in c.psg_writes(native_res["irq"])]
    write_frames = {t // c.FRAME for t in writes_t}
    steady = [f for f in range(RUN_FRAMES - 2) if not ({f - 2, f - 1, f} & write_frames)]
    loud = [f for f in steady if ours_rms[f] > 1000]
    silent = [f for f in steady if ours_rms[f] == 0.0 and f >= 8]
    shifts = []
    for shift in (0, 1, 2, 3):
        ratios = [ref_rms_runs[f + shift] / ours_rms[f] for f in loud if f + shift < len(ref_rms_runs)]
        if not ratios:
            continue
        gain = sorted(ratios)[len(ratios) // 2]
        deviations = [abs(20 * math.log10(r / gain)) for r in ratios if r > 0]
        shifts.append((max(deviations) if len(deviations) == len(ratios) else 99.0, shift, gain))
    dev, shift, gain = min(shifts)
    check(dev <= 1.0, "%s: steady per-frame RMS deviates %.2f dB from one gain (shift %d): %s" % (
        name, dev, shift, [(f, round(20 * math.log10(ref_rms_runs[f + shift] / ours_rms[f] / gain), 2)) for f in loud]))
    loud_level = gain * sum(ours_rms[f] for f in loud) / len(loud)
    check(len(silent) >= 3, "%s: fewer than 3 steady silent frames" % name)
    quiet = max([ref_rms_runs[f + shift] for f in silent if f + shift < len(ref_rms_runs)] or [0.0])
    check(quiet < 0.05 * loud_level, "%s: silent steady frames reach %.0f vs loud level %.0f" % (name, quiet, loud_level))
    record["audio"] = {"frame_shift": shift, "steady_loud_frames": len(loud), "steady_silent_frames": len(silent),
                       "gain_db_vs_platform": round(20 * math.log10(gain), 1), "max_deviation_db": round(dev, 2),
                       "tolerance_db": 1.0, "silent_frame_rms_fraction": round(quiet / loud_level, 4)}
    return record


def main():
    if sys.platform == "win32":
        print("skipped: the libretro host needs POSIX dlopen")
        return 0
    root = pins.checkout(smoke.NAMES)
    if root is None:
        print("skipped: " + pins.skip_reason(smoke.NAMES))
        return 0
    with tempfile.TemporaryDirectory(prefix="sms_e2e_oracle_") as tmpname:
        tmp = pathlib.Path(tmpname)
        native, exe = c.build_native(EMITTER, CC, ROOT, tmp)
        check(exe is not None, "native build failed: %s" % native.failures)
        res = c.run_native(native, exe, "a")
        check(res["code"] == 0, "native run failed")
        if FAILED:
            return finish()
        host = tmp / "libretro_frames_host"
        subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O1", str(HERE / "sms_oracle" / "libretro_frames_host.c"),
                        "-o", str(host), "-ldl"], check=True)
        writes = c.reg_writes(res["vdp_trace"])
        models = {}
        for f in range(COMPARE_FROM_FRAME, RUN_FRAMES):
            h, data, _, _ = c.model_frame(res["vram"], res["cram"], writes, f)
            check(c.sha256(data) == res["frames"][f][3], "platform frame %d differs from the render model" % f)
            wrapped = c.model_frame(res["vram"], res["cram"], writes, f, "no_fine_gap")[1]
            models[f] = (h, data, c.gap_mask(writes, f), wrapped)
        manifest = json.loads(c.MANIFEST.read_text()) if c.MANIFEST.exists() else {}
        records = {}
        for name, spec in smoke.FINALISTS.items():
            core = smoke.build_core(root, spec, tmp / ("core_" + name))
            rom = tmp / "roms" / (c.FIXTURE + ".sms")
            ref = run_reference(host, core, rom, tmp / "input.txt", tmp / ("ref_" + name), spec["options"])
            again = run_reference(host, core, rom, tmp / "input.txt", tmp / ("ref2_" + name), spec["options"])
            check(ref["ram"] == again["ram"] and ref["frames"] == again["frames"], "%s: two reference runs differ" % name)
            record = compare(name, res, ref, models)
            record.update({"pin": pins.PINS[spec["tree"]], "fixture_sha256": builder.build(c.FIXTURE)[1]["sha256"],
                           "native_state_digest": res["digest"]})
            records[name] = record
            print("%s: %s" % (name, json.dumps(record, sort_keys=True)))
        if WRITE:
            manifest["schema"] = 1
            manifest["reference"] = records
            c.MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
            print("manifest reference section written")
        else:
            check(manifest.get("reference") == records, "reference comparison differs from the recorded one: %s" % [
                (n, k) for n in records for k in records[n] if manifest.get("reference", {}).get(n, {}).get(k) != records[n][k]])
    return finish()


def finish():
    if FAILED:
        print("sms machine e2e oracle: %d failures" % len(FAILED))
        return 1
    print("sms machine e2e oracle: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
