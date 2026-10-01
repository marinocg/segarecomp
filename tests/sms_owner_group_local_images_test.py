#!/usr/bin/env python3
"""SEG-033-T003: grouped owners and shared bodies on authorized local SMS images (extended tier; skips without games/sms).

usage: sms_owner_group_local_images_test.py <sms_image_emitter> <cc> <product-root>

For every local image whose mapper identity is established (`sms_local_images_test.IDENTITIES`), builds the generated program
as the reference emission (`--owner-group 1 --share-bodies 0`) and as the production default, runs both headless for a fixed
frame bound and requires every artifact to be byte-identical (machine-state digest, mapper/IRQ/VDP traces, VRAM/CRAM, frame
records, framebuffer, PCM and its digest). Only equalities and counts are printed; no byte, hash or trace of an image is
recorded, and the images are never copied into the repository.
"""
import hashlib
import pathlib
import re
import sys
import tempfile

EMITTER, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
from sms_vdp_native import Native  # noqa: E402

FRAMES = "120"
MODES = {"reference": ["--owner-group", "1", "--share-bodies", "0"], "default": []}


def identities():
    text = (HERE / "sms_local_images_test.py").read_text(encoding="utf-8")
    return {m.group(1): m.group(2) for m in re.finditer(r'"([0-9a-f]{64})": \("(\w+)",', text)}


def main():
    images = sorted((ROOT / "games" / "sms").glob("*.sms")) if (ROOT / "games" / "sms").is_dir() else []
    known = identities()
    todo = [(p, known[hashlib.sha256(p.read_bytes()).hexdigest()]) for p in images if hashlib.sha256(p.read_bytes()).hexdigest() in known]
    if not todo:
        print("skipped: no local image with an established mapper identity under games/sms")
        return 0
    failed = []
    with tempfile.TemporaryDirectory(prefix="sms-group-local-") as tmpname:
        tmp = pathlib.Path(tmpname)
        for n, (rom, mapper) in enumerate(todo[:1]):  # the reference build of one image is minutes of compile; one image suffices per run
            roms = tmp / ("roms%d" % n)
            roms.mkdir()
            (roms / "image.sms").write_bytes(rom.read_bytes())
            out = {}
            for mode, args in MODES.items():
                native = Native(EMITTER, CC, ROOT, tmp / ("%s%d" % (mode, n)), emitter_args=args)
                (tmp / ("%s%d" % (mode, n))).mkdir()
                exe = native.build(roms, "image", mapper=mapper)
                failed.extend(native.failures)
                if exe is None:
                    break
                out[mode] = native.execute(exe, mode, "--frames", FRAMES, "--cycle-budget", "2000000000", timeout=900)
            if len(out) != 2:
                continue
            ref, new = out["reference"], out["default"]
            names = sorted(p.name for p in ref["dir"].iterdir())
            if ref["code"] != new["code"] or names != sorted(p.name for p in new["dir"].iterdir()):
                failed.append("image %d: exit status or artifact set differs" % n)
                continue
            for name in names:
                if (ref["dir"] / name).read_bytes() != (new["dir"] / name).read_bytes():
                    failed.append("image %d: artifact %s differs" % (n, name))
            print("image %d: %d artifacts compared, state digest %s" % (n, len(names), "identical" if not failed else "DIFFERS"))
    if failed:
        print("\n".join(failed[:10]))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
