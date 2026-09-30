#!/usr/bin/env python3
"""SEG-009-T005: generated-native SMS rendering fixtures against the independent renderer model, with mutation controls.

usage: sms_render_native_test.py <sms_image_emitter> <cc> <product-root>

Project-authored fixtures (tools/sms_fixture_rom.py render_scene_a/b/c, render_raster) are emitted through the SMS generation
route, compiled with the SMS runtime and the real headless driver (VDP + renderer device wiring) as strict C11 and run with a
finite --frames bound. For every completed frame after the display enable the expectation is computed by
tests/sms_render_model.py (written from the machine contract) from the final VRAM/CRAM artifacts and the register writes of the
VDP trace: a write executed at instruction-start T reaches the line whose scanline event is at T' > T (U2/U11). Compared:
  * the per-frame SHA-256 list (frames.txt) and the last framebuffer (framebuffer.bin), byte for byte;
  * the sprite overflow/collision bits the guest reads from the status register;
  * the VDP trace linkage of each frame record (non-decreasing, within the trace);
  * determinism and slice equivalence (fixed and pseudo-random slices: identical frames, hashes, digest);
  * mutation controls: each deliberately wrong model rule must disagree with the native frames.
"""
import hashlib
import pathlib
import sys
import tempfile

EMITTER, CC = sys.argv[1], sys.argv[2]
ROOT = pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import sms_fixture_rom as builder  # noqa: E402
import sms_render_model as model  # noqa: E402
from sms_vdp_native import Native  # noqa: E402

LINE, FRAME = 228, 59736
FRAMES = {"render_scene_a": "10", "render_scene_b": "10", "render_scene_c": "10", "render_raster": "24"}
RESET_REGS = [0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0, 0, 0, 0xFF]
FAILED = []


def check(cond, message):
    if not cond:
        FAILED.append(message)


def reg_writes(trace):
    return [(int(a[0]), int(a[2]), int(a[3], 16)) for a in trace if a[1] == "reg" and int(a[2]) <= 10]


def regs_fn(writes, frame):
    def regs_at(line):
        t = frame * FRAME + line * LINE
        regs = list(RESET_REGS)
        for cycles, reg, value in writes:
            if cycles < t:
                regs[reg] = value
        return regs
    return regs_at


def frame_records(res):
    return [(int(a[0]), int(a[1]), int(a[2]), a[3]) for a in (l.split() for l in (res["dir"] / "frames.txt").read_text().splitlines())]


def model_hash(res, frame, writes, mutation=None):
    height, rows, overflow, collision = model.render(res["vram"], res["cram"], regs_fn(writes, frame), mutation)
    data = bytes(v for row in rows for v in row)
    return height, data, overflow, collision


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="sms_render_native_"))
    roms = tmp / "roms"
    builder.main(["--out", str(roms), *[a for n in FRAMES for a in ("--fixture", n)]])
    native = Native(EMITTER, CC, ROOT, tmp)
    runs = {}
    for name, frames in FRAMES.items():
        exe = native.build(roms, name)
        if exe is None:
            continue
        runs[name] = (exe, native.execute(exe, name, "--frames", frames))
    check(not native.failures, "native build failed: %s" % native.failures)
    compared = {}
    for name, (exe, res) in runs.items():
        check(res["code"] == 0, "%s: exit %s" % (name, res["code"]))
        if res["code"] != 0:
            continue
        writes = reg_writes(res["vdp_trace"])
        enable = next((c for c, r, v in writes if r == 1 and v & 0x40), None)
        check(enable is not None, "%s: the display was never enabled" % name)
        if enable is None:
            continue
        records = frame_records(res)
        check(res["vdp"]["trace_dropped"] == 0, "%s: VDP trace overflowed" % name)
        frames_seen = [r[0] for r in records]
        check(frames_seen == sorted(set(frames_seen)) and frames_seen[0] == 0, "%s: frame records not consecutive from 0" % name)
        check(len(records) == int(FRAMES[name]), "%s: %d frame records for %s frames" % (name, len(records), FRAMES[name]))
        check(all(a[2] <= b[2] for a, b in zip(records, records[1:])) and records[-1][2] <= res["vdp"]["trace_entries"],
              "%s: trace linkage is not monotonic" % name)
        full = [r for r in records if r[0] * FRAME >= enable]
        check(all(r[1] == (224 if name == "render_scene_c" else 192) for r in full), "%s: frame height" % name)
        check(len(full) >= 3, "%s: fewer than 3 complete frames after the display enable" % name)
        flags = (False, False)
        hashes = set()
        for frame, height, _, digest in full:
            mh, data, overflow, collision = model_hash(res, frame, writes)
            check(hashlib.sha256(data).hexdigest() == digest, "%s: frame %d hash differs from the model" % (name, frame))
            flags = (flags[0] or overflow, flags[1] or collision)
            hashes.add(digest)
        compared[name] = (full, writes)
        if name == "render_raster":
            check(len(hashes) >= 10, "%s: per-line register changes produced only %d distinct frames" % (name, len(hashes)))
        else:
            check(len(hashes) == 1, "%s: a static scene rendered %d distinct frames" % (name, len(hashes)))
            status = res["ram"][0x100]
            check(((status >> 6) & 1, (status >> 5) & 1) == (int(flags[0]), int(flags[1])),
                  "%s: status overflow/collision bits %s differ from the model %s" % (name, ((status >> 6) & 1, (status >> 5) & 1), flags))
            check(flags == (True, True) or name != "render_scene_a", "%s: scene lost its overflow/collision content" % name)
        last = (res["dir"] / "framebuffer.bin").read_bytes()
        _, data, _, _ = model_hash(res, records[-1][0], writes) if records[-1][0] * FRAME >= enable else (0, None, 0, 0)
        check(last == data, "%s: last framebuffer differs from the model" % name)
        check(res["vdp"]["trace_dropped"] == 0, "%s: trace" % name)
        meta = __import__("json").loads((res["dir"] / "frame.json").read_text())
        check(meta["sha256"] == records[-1][3] and meta["frames_completed"] == len(records), "%s: frame.json disagrees with frames.txt" % name)
        # determinism and slice equivalence
        for tag, extra in (("rep", []), ("fixed", ["--slice-cycles", "9973"]), ("seeded", ["--slice-seed", "11"])):
            other = native.execute(exe, name + "_" + tag, "--frames", FRAMES[name], *extra)
            check(other["digest"] == res["digest"], "%s: %s run digest differs" % (name, tag))
            for art in ("frames.txt", "framebuffer.bin", "vdp.trace"):
                check((other["dir"] / art).read_bytes() == (res["dir"] / art).read_bytes(), "%s: %s run %s differs" % (name, tag, art))
    # mutation controls: every wrong rule must be caught by some fixture frame
    for mutation in model.MUTATIONS:
        detected = False
        for name, (full, writes) in compared.items():
            res = runs[name][1]
            for frame, _, _, digest in full[:3]:
                _, data, _, _ = model_hash(res, frame, writes, mutation)
                if hashlib.sha256(data).hexdigest() != digest:
                    detected = True
                    break
            if detected:
                break
        check(detected, "mutation %s was not detected by any fixture frame" % mutation)
    if FAILED:
        print("\n".join(FAILED[:30]))
        return 1
    print("ok: %d fixtures, %d compared frames match the independent model; %d mutants detected" %
          (len(compared), sum(len(v[0]) for v in compared.values()), len(model.MUTATIONS)))
    return 0


sys.exit(main())
