#!/usr/bin/env python3
"""SEG-008-T009 regression: the static-budget SMS-shaped scale model must be the ADR 0058 logical map.

  0x0000-0x03FF invariant first 1 KiB; 0x0400-0x3FFF banked slot 0 remainder; 0x4000-0x7FFF slot 1; 0x8000-0xBFFF slot 2;
  0xC000-0xFFFF RAM / non-code (no image window there).

CodeWindow semantics: image offset `o` of a window is exposed at logical `base + o`. The test parses the spec text of the
`banked512-*` shapes (and the local-ROM shape builder) and checks the exact windows, then emits a small instance and checks
the owner set. usage: z80_static_budget_shape_test.py <z80_image_emitter> <cc> <product-root>
"""
import pathlib
import random
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import z80_conformance as zc  # noqa: E402
import z80_static_budget as budget  # noqa: E402

FAILED = False


def check(cond, message):
    global FAILED
    if not cond:
        FAILED = True
        print("FAIL:", message)


def parse(spec):
    images = {}
    for line in spec.splitlines():
        parts = line.split()
        if parts[0] == "image":
            images[int(parts[1])] = {"kind": parts[2], "windows": [], "bytes": 0}
        elif parts[0] == "window":
            images[int(parts[1])]["windows"].append(tuple(int(x, 16) for x in parts[2:5]))
        elif parts[0] == "bytes":
            images[int(parts[1])]["bytes"] = len(parts[2]) // 2
    return images


def check_sms_map(spec, banks):
    images = parse(spec)
    check(sorted(images) == [1] + [2 + n for n in range(banks)], "image identities are not 1 + banks 2..%d" % (banks + 1))
    inv = images[1]
    check(inv["kind"] == "invariant" and inv["windows"] == [(0x0000, 0x0000, 0x0400)] and inv["bytes"] == 0x400,
          "the invariant image is not the first 1 KiB at logical 0x0000")
    covered = set()
    for ident, image in images.items():
        if ident == 1:
            continue
        check(image["kind"] == "banked" and image["bytes"] == 0x4000, "bank %d is not a 16 KiB banked image" % ident)
        check(image["windows"] == [(0x0000, 0x0400, 0x3C00), (0x4000, 0x0000, 0x4000), (0x8000, 0x0000, 0x4000)],
              "bank %d windows are not slot 0 (0x0400-0x3FFF), slot 1, slot 2" % ident)
        for base, first, length in image["windows"]:
            covered.update(range(base + first, base + first + length))
    covered.update(range(0x0000, 0x0400))
    check(covered == set(range(0x0000, 0xC000)), "code windows do not tile exactly 0x0000-0xBFFF")
    check(not any(a >= 0xC000 for a in covered), "an image window exposes the 0xC000-0xFFFF RAM region")


def main():
    emitter, cc = sys.argv[1], sys.argv[2]
    pool = budget.legal_encoding_pool()
    _, forms = zc.load_dataset()
    check(len(pool) == sum(hi - lo + 1 for f in forms.values() for lo, hi in f["byte_ranges"]),
          "the dense pool is not the dataset's concrete encodings")
    for shape in ("banked512-dense", "banked512-random"):
        spec, mode, limits = budget.shape_spec(shape, pool)
        check(mode == 1 and limits is budget.BUDGETS_512K, "%s uses the wrong measurement mode/budgets" % shape)
        check_sms_map(spec, 32)
    rng = random.Random(7)
    with tempfile.TemporaryDirectory() as tmp:
        rom = pathlib.Path(tmp) / "synthetic.bin"
        rom.write_bytes(bytes(rng.randrange(256) for _ in range(0x4000 * 3 - 5)))  # padded to whole banks like a real image
        rom_text, _, _ = budget.shape_spec("rom:" + str(rom), pool)
        check_sms_map(rom_text, 3)
        # Emit a small instance: one owner per (bank, offset) over the union of its slots, the invariant owners absolute,
        # and no owner for the RAM region.
        small = budget.sms_spec(bytes(rng.randrange(256) for _ in range(0x400)),
                                [bytes(rng.randrange(256) for _ in range(0x4000)) for _ in range(2)])
        check_sms_map(small, 2)
        stats, owners, error = zc.emit_image(zc.Toolchain(cc, emitter), small, pathlib.Path(tmp) / "small", "sms", list_owners=True)
        check(error is None, "emission of the small SMS-shaped image failed: %s" % error)
        if error is None:
            for ident in (2, 3):
                keys = sorted(o["key"] for o in owners if o["identity"] == ident)
                check(keys == list(range(0x4000)), "bank %d: owners are not one per image offset 0..0x3FFF" % ident)
                check(all(o["relative"] for o in owners if o["identity"] == ident), "bank %d owners must be window-relative" % ident)
            inv = sorted(o["key"] for o in owners if o["identity"] == 1)
            check(inv == list(range(0x400)), "invariant owners must be the absolute PCs 0x0000-0x03FF")
            check(all(o["key"] < 0xC000 for o in owners if o["identity"] == 1), "an owner exists in the RAM region")
    if FAILED:
        return 1
    print("z80 static-budget SMS shape: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
