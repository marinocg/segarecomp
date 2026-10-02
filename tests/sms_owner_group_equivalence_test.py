#!/usr/bin/env python3
"""SEG-033-T003: the Master System machine behaves identically with grouped owners and shared bodies (hermetic fixtures).

usage: sms_owner_group_equivalence_test.py <sms_image_emitter> <cc> <product-root> [fixture ...]

Builds the project-authored SMS fixtures twice through the one SMS generation route, once as the reference emission (one C
function per start, every effect inline: `--owner-group 1 --share-bodies 0`, byte-identical to the emission before SEG-033) and
once with the production default (bounded multi-entry owners, shared effect bodies), runs both with the real headless driver and
the real VDP/PSG/pad runtime, and requires every artifact to be byte-identical: machine-state digest, status (cycles, frames),
IRQ and mapper traces, RAM, VDP register trace, VRAM/CRAM, every frame record, the final framebuffer and the PSG PCM stream
with its digest. The fixtures cover interrupts and pause NMI with scripted controllers, mapper bank switching, the VDP and the
renderer (including raster effects) and audio. Authorized local images get the same comparison in the extended tier
(`sms_owner_group_local_images_test`) with only digests compared; nothing from an image is stored.

CI wall-clock: the optional fixture names restrict the run to those fixtures (CTest registers disjoint slices whose union is every
fixture; an unknown name fails). Without them every fixture runs.
"""
import pathlib
import sys
import tempfile

EMITTER, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
from sms_e2e_common import builder  # noqa: E402
from sms_vdp_native import Native  # noqa: E402

# fixture -> (headless --frames bound, scripted input?)
FIXTURES = {"machine_e2e": (str(builder.E2E_FRAMES), True), "vdp_seq_a": ("60", False), "vdp_irq": ("60", False),
            "vdp_straddle_write": ("60", False), "render_scene_a": ("10", False), "render_raster": ("24", False),
            "vdp_reset_probe": ("120", False)}
if len(sys.argv) > 4:
    unknown = [n for n in sys.argv[4:] if n not in FIXTURES]
    assert not unknown, "unknown fixtures: %s" % unknown
    FIXTURES = {n: FIXTURES[n] for n in sys.argv[4:]}
MODES = {"reference": ["--owner-group", "1", "--share-bodies", "0"], "default": []}
FAILED = []


def main():
    with tempfile.TemporaryDirectory(prefix="sms-group-equiv-") as tmpname:
        tmp = pathlib.Path(tmpname)
        roms = tmp / "roms"
        builder.main(["--out", str(roms), *[a for name in FIXTURES for a in ("--fixture", name)]])
        (tmp / "input.txt").write_text(builder.E2E_SCRIPT, encoding="utf-8")
        results = {}
        for mode, args in MODES.items():
            native = Native(EMITTER, CC, ROOT, tmp / mode, emitter_args=args)
            (tmp / mode).mkdir()
            for name, (frames, scripted) in FIXTURES.items():
                exe = native.build(roms, name)
                if exe is None:
                    break
                extra = ["--input", str(tmp / "input.txt")] if scripted else []
                results[(mode, name)] = native.execute(exe, "%s_%s" % (mode, name), "--frames", frames, *extra)
            FAILED.extend(native.failures)
        compared = 0
        for name in FIXTURES:
            ref, new = results.get(("reference", name)), results.get(("default", name))
            if ref is None or new is None:
                FAILED.append("%s: missing result" % name)
                continue
            if ref["code"] != new["code"]:
                FAILED.append("%s: exit status %s vs %s" % (name, ref["code"], new["code"]))
            names = sorted(p.name for p in ref["dir"].iterdir())
            if names != sorted(p.name for p in new["dir"].iterdir()):
                FAILED.append("%s: artifact sets differ" % name)
                continue
            for artifact in names:
                compared += 1
                if (ref["dir"] / artifact).read_bytes() != (new["dir"] / artifact).read_bytes():
                    FAILED.append("%s: artifact %s differs between the reference and the grouped emission" % (name, artifact))
            if not ref.get("digest"):
                FAILED.append("%s: the run produced no state digest" % name)
        e2e = results.get(("default", "machine_e2e"))
        if e2e and e2e["code"] == 0:
            pcm = (e2e["dir"] / "audio.pcm").read_bytes()
            frames = (e2e["dir"] / "frames.txt").read_text().splitlines()
            if len(pcm) < 1000 or len(frames) < 10:
                FAILED.append("machine_e2e produced too little audio/video to be evidence (%d bytes PCM, %d frames)" % (len(pcm), len(frames)))
    if FAILED:
        print("\n".join(FAILED[:15]))
        return 1
    print("sms owner-group equivalence: %d artifacts across %d fixtures identical (reference vs grouped+shared)" % (compared, len(FIXTURES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
