#!/usr/bin/env python3
"""SEG-009-T008: end-to-end generated-native Master System machine fixture (hermetic).

usage: sms_machine_e2e_test.py <sms_image_emitter> <cc> <product-root> [--write-manifest]

The project-authored `machine_e2e` fixture (tools/sms_fixture_rom.py, builder-declared Sega mapper) is emitted through the SMS
generation route, compiled with the full SMS runtime (memory map, mapper, scheduler, VDP, renderer, controllers, pause and PSG)
and the real headless driver as strict C11, and run for a finite frame bound under a scripted input. Checked here:

  * every artifact against the independent references of tests/sms_e2e_common.py (contract-derived predictions, the VDP,
    renderer and PSG models) and against the committed validation manifest tests/fixtures/sms-e2e-validation.json;
  * the fixture covers each baseline capability area (asserted from artifacts, not from the source);
  * generated-native-only execution: the link inputs are generated owners plus runtime device code, the executable defines no
    interpreter/decoder symbol, the runtime sources contain no opcode decoding, and PC 0x4100 dispatches under three banks;
  * determinism: two runs, fixed and pseudo-random slice splits, and byte-identical regeneration of the generated C;
  * fault injection: each of six one-line perturbations of the runtime (mapper mask, VDP latch, line counter, sprite limit,
    PSG noise taps, pause edge) must change the artifacts it owns.

`--write-manifest` rewrites the `native` section of the manifest (the `reference` section is owned by
sms_machine_e2e_oracle_test.py and preserved).
"""
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

import sms_e2e_common as c
from sms_e2e_common import builder

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
WRITE = "--write-manifest" in sys.argv[4:]
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)
        print("FAIL:", message)


def first_difference(a, b):
    n = min(len(a), len(b))
    return next((i for i in range(n) if a[i] != b[i]), n if len(a) != len(b) else None)


# ---- expectations against the native run ---------------------------------------------------------------------------

def check_guest_results(res):
    ram = res["ram"]
    for address, expected in c.INIT_BLOCK.items():
        check(ram[address] == expected, "init block $C%03X = %02X, expected %02X" % (address, ram[address], expected))
    check((ram[0x112], ram[0x113]) == c.expected_readback(), "VRAM read-back sum/xor $C112/$C113 = %02X %02X, expected %02X %02X" %
          ((ram[0x112], ram[0x113]) + c.expected_readback()))
    check(ram[0x1FF] == 0xA5, "the program did not finish ($C1FF = %02X)" % ram[0x1FF])
    accepted = c.accepted_frame_times(res["irq"])
    records = c.guest_frames(ram)
    check(len(records) == len(accepted) and len(records) >= builder.E2E_LOGGED_FRAMES + 1,
          "%d frame records for %d accepted frame interrupts" % (len(records), len(accepted)))
    for n, rec in enumerate(records):
        expected = c.expected_record(n, accepted[n] // c.FRAME)
        check(list(rec[:13]) == expected, "frame log %d: %s, expected %s" % (n, list(rec[:13]), expected))
    check(all(rec[13:] == bytes(3) for rec in records), "frame log padding not zero")
    return len(records)


def check_interrupts(res, frames):
    irq = res["irq"]
    model = c.expected_irq_edges(res["vdp_trace"], builder.E2E_FRAMES)
    model += c.pause_edges_trace()
    order = {"pause": 0, "frame": 1, "line": 2}
    model.sort(key=lambda e: (e[0], order[e[1]]))
    native = [e for e in irq if e[2] in ("asserted", "deasserted")]
    check(native == model, "interrupt asserted/deasserted trace differs from the contract model (first difference at %s)" %
          first_difference(native, model))
    # every acceptance is of an asserted level, at most 7 T after its assertion when the CPU sits in HALT
    last_assert, late = {}, 0
    for t, src, ev in irq:
        if ev == "asserted":
            last_assert[src] = t
        elif ev == "deasserted":
            last_assert.pop(src, None)
        else:
            check(src in last_assert and t >= last_assert[src], "%s accepted without an asserted level at %d" % (src, t))
            late += t - last_assert.get(src, t) >= 8
    check(late == 0, "%d interrupts accepted 8+ T after assertion while the program idles in HALT" % late)
    lines = len(c.vdp_model.line_interrupt_lines(builder.E2E_R10))
    accepted_line = sum(1 for e in irq if e[1] == "line" and e[2] == "accepted")
    check(accepted_line == lines * frames, "line interrupts accepted: %d, expected %d" % (accepted_line, lines * frames))
    pauses = [e for e in irq if e[1] == "pause" and e[2] == "accepted"]
    check(len(pauses) == len(c.pause_edges()), "pause NMIs accepted: %d, edges in the script: %d" % (len(pauses), len(c.pause_edges())))


def check_mapper_and_vdp(res, frames):
    seq = [(r, v) for _, r, v in res["mapper"]]
    check(seq == c.expected_mapper(frames), "mapper register write sequence differs (first difference at %s)" %
          first_difference(seq, c.expected_mapper(frames)))
    check(all(a[0] < b[0] for a, b in zip(res["mapper"], res["mapper"][1:])), "mapper trace times are not increasing")
    regs = [(arg, byte) for _, kind, arg, byte, _ in res["vdp_trace"] if kind == "reg"]
    expected = c.expected_reg_sequence(frames)
    check(regs[:len(expected)] == expected and len(regs) in (len(expected), len(expected) + 4),
          "VDP register write sequence differs (first difference at %s)" % first_difference(regs, expected))
    final = list(c.RESET_REGS)
    for reg, value in regs:
        final[reg] = value
    check(res["vdp"]["regs"] == final[:11], "final VDP registers %s, expected %s" % (res["vdp"]["regs"], final[:11]))
    check(res["vram"] == c.expected_vram(), "VRAM differs from the uploaded tables")
    check(res["cram"] == c.expected_cram(), "CRAM differs from the uploaded palette")
    import hashlib
    last_v = [d for _, k, _, _, d in res["vdp_trace"] if k == "vram"][-1]
    last_c = [d for _, k, _, _, d in res["vdp_trace"] if k == "cram"][-1]
    check(last_v == int.from_bytes(hashlib.sha256(c.expected_vram()).digest()[:8], "big"), "final VRAM write summary hash")
    check(last_c == int.from_bytes(hashlib.sha256(c.expected_cram()).digest()[:8], "big"), "final CRAM write summary hash")
    check(res["vdp"]["trace_dropped"] == 0 and res["status"]["irq_trace_dropped"] == 0 and res["status"]["mapper_trace_dropped"] == 0,
          "a trace overflowed")


def check_frames(res):
    writes = c.reg_writes(res["vdp_trace"])
    enable = c.display_enable_time(writes)
    first = enable // c.FRAME
    check(first >= c.FIRST_DISPLAY_FRAME_MIN, "display enabled too early (frame %d)" % first)
    frames = res["frames"]
    check([f[0] for f in frames] == list(range(len(frames))) and len(frames) == builder.E2E_FRAMES, "%d frame records" % len(frames))
    check(all(a[2] <= b[2] for a, b in zip(frames, frames[1:])), "frame records link to a non-monotonic trace position")
    vram, cram = res["vram"], res["cram"]
    hashes, flagged = set(), 0
    for frame, height, _, digest in frames:
        if frame < first:
            continue
        h, data, overflow, collision = c.model_frame(vram, cram, writes, frame)
        check(h == height == 192, "frame %d height" % frame)
        check(c.sha256(data) == digest, "frame %d hash differs from the render model" % frame)
        hashes.add(digest)
        flagged += overflow and collision
    check(len(hashes) == len(frames) - first, "frames %d..%d must all differ (scroll/raster changes): %d distinct" % (first, len(frames) - 1, len(hashes)))
    check(flagged == len(frames) - first - 1, "the sprite scene lost its overflow/collision content (%d frames)" % flagged)
    last_frame = frames[-1][0]
    check(res["fb_last"] == c.model_frame(vram, cram, writes, last_frame)[1], "last framebuffer differs from the render model")
    meta = json.loads((res["dir"] / "frame.json").read_text())
    check(meta["sha256"] == frames[-1][3] and meta["frames_completed"] == len(frames) and meta["height"] == 192, "frame.json")
    return len(frames) - first


def check_audio(res):
    end = res["status"]["cycles"]
    writes = c.psg_writes(res["irq"])
    check(len(writes) == sum(1 for pair in builder.E2E_PSG.values() for b in pair if b), "PSG writes: %d" % len(writes))
    expected = c.expected_pcm(res["irq"], end)
    data = c.pcm_bytes(expected)
    check(res["pcm"] == data, "PCM differs from the PSG model fed the acknowledge-relative write timeline (%s)" %
          first_difference(res["pcm"], data))
    check(len(set(expected)) >= 8, "the PSG timeline must exercise several distinct sample values")
    check(res["audio_lines"][-1].split() == ["run", str(len(expected)), c.sha256(data)], "audio.sha256 run line")


# ---- capability coverage ------------------------------------------------------------------------------------------------

def check_coverage(res, emit_report):
    covered = {}
    ram = res["ram"]
    covered["reset_entry"] = res["status"]["z80_outcome"] == "halted" and res["status"]["sms_error"] == "SMS_OK"
    covered["ram_and_mirror"] = ram[0x101] == 0x3C and ram[0x102] == 0xA7
    covered["mapper_bank_switch"] = len({v for _, r, v in res["mapper"]}) >= 6
    covered["mapper_mask"] = ram[0x107] == 0xB2 and ram[0x108] == 0xB5
    owners = [l.split() for l in emit_report.splitlines() if l.startswith("owner ")]
    # identity = 2 + bank for slot-1 images; the routine at logical 0x4100 is offset 0x100 of the bank image
    identities = {o[1] for o in owners if int(o[2], 16) == 0x100 and o[3] == "full" and o[1] in ("3", "5", "8")}
    covered["same_pc_three_identities"] = identities == {"3", "5", "8"} and (ram[0x109], ram[0x10A], ram[0x10B], ram[0x10C]) == (0x11, 0x33, 0x66, 0x11)
    covered["banked_table_reads"] = ram[0x105] == 0xB3 and c.expected_vram()[:16] == res["vram"][:16] != bytes(16)
    covered["vdp_upload_vram_cram"] = res["vram"] == c.expected_vram() and res["cram"] == c.expected_cram()
    covered["background_scrolling"] = len({f[3] for f in res["frames"][6:]}) > 30 and any(v for _, r, v in c.reg_writes(res["vdp_trace"]) if r == 8)
    covered["sprites_overflow_collision"] = (ram[0x200 + 2] & 0x60) == 0x60
    covered["frame_interrupt_im1"] = sum(1 for e in res["irq"] if e[1:] == ("frame", "accepted")) >= builder.E2E_LOGGED_FRAMES
    covered["line_interrupt"] = sum(1 for e in res["irq"] if e[1:] == ("line", "accepted")) >= 4 * builder.E2E_LOGGED_FRAMES
    covered["controllers_scripted"] = len({(ram[0x200 + 16 * n + 3], ram[0x200 + 16 * n + 4]) for n in range(builder.E2E_LOGGED_FRAMES)}) >= 8
    covered["io_control_readback"] = (ram[0x110], ram[0x111]) == (0xC0, 0x00)
    covered["pause_nmi"] = sum(1 for e in res["irq"] if e[1:] == ("pause", "accepted")) == len(c.pause_edges()) >= 3
    covered["psg_tone_noise"] = len(set(res["pcm"][i:i + 2] for i in range(0, len(res["pcm"]), 2))) >= 8 and {b for p in builder.E2E_PSG.values() for b in p} >= {0x8E, 0xA3, 0xC7, 0xE6, 0xE1}
    for area, ok in covered.items():
        check(ok, "capability area %r is not covered by the fixture artifacts" % area)
    return sorted(covered)


# ---- generated-native-only execution ----------------------------------------------------------------------------------

FORBIDDEN_SYMBOL = re.compile(r"interp|decode|opcode|emulat|execute|fetch_and|z80_step|z80_decode", re.I)
ALLOWED_SYMBOLS = {"sms_port_decode", "mh_execute_header"}  # the I/O port decoder; the Mach-O image header symbol
FORBIDDEN_SOURCE = re.compile(r"\b\w*(interpret|opcode|emulat)\w*|\bz80_step\b|\bz80_decode\b", re.I)


def check_generated_only(native, exe, gen_dir):
    platform = ROOT / "platforms" / "master-system"
    psg_src = ROOT / "libs" / "device" / "sega" / "psg" / "src"
    for src in native.sources:
        ok = platform in src.parents or psg_src in src.parents
        check(ok, "link input %s is neither SMS runtime/headless nor the PSG device" % src)
        check(not (ROOT / "libs" / "cpu") in src.parents, "link input %s belongs to a CPU library" % src)
    units = (gen_dir / "sms.units").read_text().split()
    check(units and all(u.startswith("sms_") and u.endswith(".c") for u in units), "generated unit names")
    for name in units:
        text = (gen_dir / name).read_text(errors="replace")
        check(not FORBIDDEN_SOURCE.search(text), "generated unit %s contains interpreter/decoder vocabulary" % name)
    for src in native.sources:
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", src.read_text(), flags=re.S)   # vocabulary in code, not in prose comments
        check(not FORBIDDEN_SOURCE.search(text), "%s contains interpreter/opcode-decoder vocabulary" % src.name)
    check("z80_run" in (gen_dir / "sms_main.c").read_text(), "z80_run is not defined by a generated unit")
    nm = shutil.which("nm")
    if nm is None:
        print("note: nm unavailable, executable symbol inspection skipped")
        return
    out = subprocess.run([nm, str(exe)], capture_output=True, text=True, timeout=120).stdout
    names = {line.split()[-1].lstrip("_") for line in out.splitlines() if re.match(r"^[0-9a-fA-F]*\s+[TtDdSsBb]\s+\S+$", line.strip())}
    if sys.platform == "win32" and not names:
        print("note: no symbol table in the PE executable, symbol inspection skipped (source/link-input checks above still apply)")
        return
    bad = sorted(n for n in names if FORBIDDEN_SYMBOL.search(n) and n not in ALLOWED_SYMBOLS)
    check(not bad, "the executable defines interpreter/decoder symbols: %s" % bad[:8])
    check(any(n.startswith("z80_entry_") for n in names) and any(n.startswith("sms_machine_") for n in names),
          "the executable lacks the generated dispatcher or the machine runtime symbols")


def tree_digest(path):
    return {p.name: c.sha256(p.read_bytes()) for p in sorted(pathlib.Path(path).iterdir()) if p.is_file()}


# ---- fault injection ---------------------------------------------------------------------------------------------------

FAULTS = [
    # (name, relative file, old text, new text, artifacts that must change)
    ("mapper_mask", "platforms/master-system/runtime/sms_mapper_contract.h",
     "return (uint32_t)value & (sms_bank_count(rom_size) - 1u);", "return (uint32_t)value & (sms_bank_count(rom_size) - 1u) & 3u;",
     {"ram.bin", "state.sha256"}),
    ("vdp_latch", "platforms/master-system/runtime/sms_vdp.c",
     "      v->latch_set = 0u;\n      trace_add(v, cycles, SMS_VDP_TRACE_STATUS_READ", "      trace_add(v, cycles, SMS_VDP_TRACE_STATUS_READ",
     {"vram.bin", "vdp.trace", "state.sha256"}),
    ("line_counter", "platforms/master-system/runtime/sms_vdp.c",
     "      v->line_counter = v->reg[10];\n      v->line_pending = 1u;", "      v->line_counter = (uint8_t)(v->reg[10] + 1u);\n      v->line_pending = 1u;",
     {"irq.trace", "ram.bin"}),
    ("sprite_limit", "platforms/master-system/runtime/sms_render.h", "#define SMS_SPRITES_PER_LINE 8u", "#define SMS_SPRITES_PER_LINE 7u",
     {"frames.txt", "framebuffer.bin"}),
    ("psg_taps", "libs/device/sega/psg/include/segarecomp/device/sega/psg/sn76489.h", "#define SN76489_DEFAULT_TAPS 0x0009u",
     "#define SN76489_DEFAULT_TAPS 0x0003u", {"audio.pcm", "audio.sha256"}),
    ("pause_edge", "platforms/master-system/runtime/sms_machine.c", "if (was_pressed == 0u && ev->pause != 0u) {",
     "if (was_pressed < 2u && ev->pause != 0u) {", {"irq.trace", "ram.bin"}),
]


def build_mutant(native, gen_dir, name, rel, old, new, tmp):
    base = pathlib.Path(tmp) / ("mut_" + name)
    tree = base / "tree"
    for sub in ("platforms/master-system/runtime", "platforms/master-system/headless", "libs/device/sega/psg", "libs/codegen/c11/include"):
        shutil.copytree(ROOT / sub, tree / sub)
    target = tree / rel
    text = target.read_text()
    check(text.count(old) == 1, "fault %s: patch site not found exactly once in %s" % (name, rel))
    target.write_text(text.replace(old, new))
    mutated = c.Native(EMITTER, CC, tree, base)
    shared = base / "shared"
    shared.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, os.cpu_count() or 1))) as pool:
        objs = list(pool.map(lambda s: mutated.compile(s, shared), mutated.sources))
    if any(o is None for o in objs):
        check(False, "fault %s: mutated runtime did not compile: %s" % (name, mutated.failures[:1]))
        return None
    generated = sorted(gen_dir.glob("sms_*.o"))
    exe = base / "mutant.exe"
    link = native.run([CC, *generated, *objs, "-o", exe])
    check(link.returncode == 0, "fault %s: link failed: %s" % (name, link.stderr[:400]))
    return exe if link.returncode == 0 else None


def check_faults(native, gen_dir, baseline, tmp):
    for name, rel, old, new, owned in FAULTS:
        exe = build_mutant(native, gen_dir, name, rel, old, new, tmp)
        if exe is None:
            continue
        res = native.execute(exe, "fault_" + name, "--frames", str(builder.E2E_FRAMES), "--input", str(native.tmp / "input.txt"))
        d = res["dir"]
        changed = set()
        for art in c.ARTIFACTS:
            path = d / art
            if not path.exists() or c.sha256(path.read_bytes()) != baseline["artifact_sha256"][art]:
                changed.add(art)
        check(bool(changed & owned), "fault %s: none of %s changed (changed: %s, exit %s)" % (name, sorted(owned), sorted(changed), res["code"]))
        print("  fault %-13s exit %s, detected in %s" % (name, res["code"], ", ".join(sorted(changed))[:90]))


# ---- main ------------------------------------------------------------------------------------------------------------

def main():
    check(builder.main(["--check"]) == 0, "fixture manifest is not reproducible")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms_e2e_"))
    try:
        return run(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(tmp):
    native, exe = c.build_native(EMITTER, CC, ROOT, tmp)
    check(exe is not None, "native build failed: %s" % native.failures)
    if exe is None:
        return finish()
    roms = tmp / "roms"
    gen_dir = tmp / ("gen_" + c.FIXTURE)
    res = c.run_native(native, exe, "a")
    check(res["code"] == 0, "fixture run exited %s: %s" % (res["code"], res["stdout"][:200]))
    if res["code"] != 0:
        return finish()
    check(res["status"]["stop"] == "frame" and res["status"]["frames_completed"] == builder.E2E_FRAMES, "stop: %s" % res["status"])
    frames_serviced = check_guest_results(res)
    check_interrupts(res, frames_serviced)
    check_mapper_and_vdp(res, frames_serviced)
    compared = check_frames(res)
    check_audio(res)
    report = subprocess.run([EMITTER, str(roms / (c.FIXTURE + ".sms")), str(tmp / "list"), "sms", "--manifest",
                             str(roms / (c.FIXTURE + ".mapper.json")), "--list"], capture_output=True, text=True, timeout=300)
    check(report.returncode == 0, "emission report failed")
    areas = check_coverage(res, report.stdout)
    check_generated_only(native, exe, gen_dir)

    # manifest of expected digests (hermetic CI); the reference section is written by the oracle test
    digests = c.artifact_digests(res)
    digests.update({"fixture": builder.build(c.FIXTURE)[1], "input_script_sha256": c.sha256(builder.E2E_SCRIPT.encode()),
                    "frames_bound": builder.E2E_FRAMES, "capability_areas": areas})
    if WRITE:
        manifest = json.loads(c.MANIFEST.read_text()) if c.MANIFEST.exists() else {}
        manifest.update({"schema": 1, "native": digests})
        c.MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        print("manifest native section written")
    else:
        manifest = json.loads(c.MANIFEST.read_text()) if c.MANIFEST.exists() else {}
        check(manifest.get("native") == digests, "artifacts differ from the committed validation manifest %s" % [
            k for k in digests if manifest.get("native", {}).get(k) != digests[k]])
        ref = manifest.get("reference", {})
        check(set(ref) >= {"gearsystem", "genesis_plus_gx"} and all(v.get("fixture_sha256") == digests["fixture"]["sha256"] for v in ref.values()),
              "the manifest lacks a current reference comparison record for both pinned references")
        check(all(v.get("native_state_digest") == digests["state_digest"] for v in ref.values()), "reference record is for another native run")

    # determinism: a second run and arbitrary slice splits give byte-identical artifacts
    for tag, extra in (("b", []), ("fixed", ["--slice-cycles", "9973"]), ("seeded", ["--slice-seed", "17"]), ("tiny", ["--slice-cycles", "61"])):
        other = c.run_native(native, exe, tag, *extra)
        check(other["code"] == 0 and other["digest"] == res["digest"], "run %s: state digest differs" % tag)
        for art in c.ARTIFACTS:
            check((other["dir"] / art).read_bytes() == (res["dir"] / art).read_bytes(), "run %s: %s differs" % (tag, art))
    # regeneration of the generated C is byte-identical
    again = tmp / "regen"
    r = subprocess.run([EMITTER, str(roms / (c.FIXTURE + ".sms")), str(again), "sms", "--manifest", str(roms / (c.FIXTURE + ".mapper.json"))],
                       capture_output=True, text=True, timeout=300)
    check(r.returncode == 0, "regeneration failed")
    first = {k: v for k, v in tree_digest(gen_dir).items() if k.endswith((".c", ".h", ".json", ".units"))}
    check(first and first == {k: v for k, v in tree_digest(again).items() if k.endswith((".c", ".h", ".json", ".units"))},
          "regenerated C is not byte-identical")
    check_faults(native, gen_dir, digests, tmp)
    print("e2e: %d frames run, %d compared against the render model, %d IRQ trace entries, %d PCM samples, digest %s" %
          (res["status"]["frames_completed"], compared, len(res["irq"]), len(res["pcm"]) // 2, res["digest"][:16]))
    return finish()


def finish():
    if FAILED:
        print("sms machine e2e: %d failures" % len(FAILED))
        return 1
    print("sms machine e2e: ok")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    sys.exit(main())
