#!/usr/bin/env python3
"""Deterministic reconstruction of a Sega CD sub-CPU runnable-epoch image from a BIOS image and a structural recipe (SEG-078, ADR 0179).

A recipe never contains executable bytes. It names the BIOS image it applies to (SHA-256 and size) and the image's length; the
image is zero-initialised (the BIOS clears PRG RAM before it builds anything; the clear is a recorded producer, not a copy) and
then each ordered step decodes one bounded immutable source span into it:

    {"decoder": "kosinski", "source_offset": <BIOS offset>, "dest": <PRG RAM offset>}

The only transformation is the public Kosinski LZ decoder (the same algorithm the BIOS routine implements; this implementation is
independent of the BIOS code and is checked against it by differential evidence recorded in the ADR). No other operation exists:
there is no scripting, no conditional, no loop in the recipe. Everything fails closed (typed `RecipeError`): unknown fields or
versions, wrong BIOS identity, steps out of bounds or overlapping, a back-reference before the stream start, source exhaustion, a
decoded length that does not end inside the image.

usage: sega_cd_epoch_recipe.py reconstruct --bios FILE --recipe FILE --out FILE
       sega_cd_epoch_recipe.py verify --bios FILE --recipe FILE [--expect-sha256 HEX] [--epoch FILE]
       sega_cd_epoch_recipe.py spec --bios FILE --recipe FILE      (the emitter's --admit epoch-recipe:<spec> text)
       sega_cd_epoch_recipe.py scan --bios FILE --out RECIPE            (candidate from a structural scan of the BIOS; not an authority)
       sega_cd_epoch_recipe.py derive --bios FILE --epoch FILE --write-log FILE --out RECIPE   (local, private input; see below)
"""
import argparse
import hashlib
import json
import pathlib
import sys

SCHEMA = "segarecomp.sega_cd.epoch_recipe.v1"
GRANULE = 256                    # the epoch granule (ADR 0142 decision 3)
MAX_IMAGE = 512 * 1024           # PRG RAM
MAX_BIOS = 128 * 1024
MAX_STEPS = 8
MAX_DECODE_STEPS = 1 << 22       # token bound per step
DECODERS = ("kosinski",)
TOP = {"schema", "bios_sha256", "bios_size", "image_length", "steps"}
STEP = {"decoder", "source_offset", "dest"}


class RecipeError(Exception):
    def __init__(self, kind):
        super().__init__(kind)
        self.kind = kind


def canonical(recipe):
    return json.dumps(recipe, sort_keys=True, separators=(",", ":"))


def identity(recipe):
    return hashlib.sha256(canonical(recipe).encode()).hexdigest()


def validate(recipe, bios=None):
    """Structural validation; with `bios`, also the identity binding. Raises RecipeError."""
    if not isinstance(recipe, dict) or set(recipe) != TOP:
        raise RecipeError("recipe_fields")
    if recipe["schema"] != SCHEMA:
        raise RecipeError("recipe_schema")
    sha, size, length, steps = recipe["bios_sha256"], recipe["bios_size"], recipe["image_length"], recipe["steps"]
    if not (isinstance(sha, str) and len(sha) == 64 and all(c in "0123456789abcdef" for c in sha)):
        raise RecipeError("recipe_bios_sha256")
    for v in (size, length):
        if type(v) is not int:
            raise RecipeError("recipe_integer")
    if not (0 < size <= MAX_BIOS) or not (0 < length <= MAX_IMAGE) or length % GRANULE != 0:
        raise RecipeError("recipe_bounds")
    if not isinstance(steps, list) or not (1 <= len(steps) <= MAX_STEPS):
        raise RecipeError("recipe_steps")
    for step in steps:
        if not isinstance(step, dict) or set(step) != STEP:
            raise RecipeError("recipe_step_fields")
        if step["decoder"] not in DECODERS:
            raise RecipeError("recipe_decoder")
        for key in ("source_offset", "dest"):
            if type(step[key]) is not int or step[key] < 0:
                raise RecipeError("recipe_integer")
        if step["source_offset"] >= size or step["dest"] >= length:
            raise RecipeError("recipe_bounds")
    if bios is not None and (len(bios) != size or hashlib.sha256(bios).hexdigest() != sha):
        raise RecipeError("bios_identity_mismatch")


def kosinski(src, pos, limit=MAX_IMAGE):
    """Decode one Kosinski stream from src[pos:]. Returns (output bytes, source position after the end marker).

    Bit field: 16-bit little-endian words consumed from the least significant bit; the next word is fetched the moment the
    sixteenth bit has been taken. 1 = literal byte. 01 = long copy: bytes lo, hi;     the sign-extended 16-bit value 0xE000 | ((hi & 0xF8) << 5) | lo (a negative offset from the next output byte); count =
    (hi & 7) + 2, or when (hi & 7) = 0 an extra byte n: n = 0 ends the stream, n = 1 is a no-op, otherwise n + 1 bytes. 00 + two count bits (MSB first) + one byte:
    short copy of count + 2 bytes at displacement byte - 256. A copy may only reference bytes already produced by this stream."""
    out = bytearray()
    state = {"pos": pos, "bits": 0, "left": 0}

    def byte():
        p = state["pos"]
        if p >= len(src):
            raise RecipeError("source_exhausted")
        state["pos"] = p + 1
        return src[p]

    def refill():
        lo = byte()
        state["bits"] = lo | (byte() << 8)
        state["left"] = 16

    def bit():
        b = state["bits"] & 1
        state["bits"] >>= 1
        state["left"] -= 1
        if state["left"] == 0:
            refill()
        return b

    def copy(displacement, count):
        if -displacement > len(out) or len(out) + count > limit:
            raise RecipeError("reference_before_stream" if -displacement > len(out) else "decoded_length_exceeded")
        for _ in range(count):
            out.append(out[-(-displacement)])

    refill()
    for _ in range(MAX_DECODE_STEPS):
        if bit():
            if len(out) >= limit:
                raise RecipeError("decoded_length_exceeded")
            out.append(byte())
        elif bit():
            lo, hi = byte(), byte()
            displacement = (0xE000 | ((hi & 0xF8) << 5) | lo) - 0x10000
            count = hi & 7
            if count:
                copy(displacement, count + 2)
            else:
                extra = byte()
                if extra == 0:
                    return bytes(out), state["pos"]
                if extra != 1:
                    copy(displacement, extra + 1)
        else:
            count = (bit() << 1) | bit()
            copy(byte() - 256, count + 2)
    raise RecipeError("token_bound_exceeded")


def reconstruct(bios, recipe):
    """-> the epoch image bytes. Deterministic; reads only `bios` and `recipe`."""
    validate(recipe, bios)
    image = bytearray(recipe["image_length"])
    covered = []
    for step in recipe["steps"]:
        data, _ = kosinski(bios, step["source_offset"], recipe["image_length"] - step["dest"])
        lo, hi = step["dest"], step["dest"] + len(data)
        if not data or hi > recipe["image_length"]:
            raise RecipeError("step_out_of_bounds")
        if any(lo < b and a < hi for a, b in covered):
            raise RecipeError("steps_overlap")
        covered.append((lo, hi))
        image[lo:hi] = data
    if all(b == 0 for b in image[-GRANULE:]):
        raise RecipeError("image_not_whole_snapshot")  # the epoch snapshot rule: the last granule holds a non-zero byte
    return bytes(image)


WINDOW = 0x20000   # main sees PRG RAM bank 0 at $020000-$03FFFF through the Gate Array window (contract section 3)


class ScanError(Exception):
    pass


def scan(bios):
    """Candidate steps found by a structural scan of an immutable BIOS image; returns (steps, image_length). NOT an authority.

    The scan looks for the call shape `LEA abs.l,A0 ; LEA abs.l,A1 ; BSR.W target` (A0 = source, A1 = a destination in the PRG-RAM window
    bank 0), keeps the sites whose source decodes with the independent decoder to a properly terminated stream inside the image, and
    requires every kept site to call one target. It does not execute or lift anything and does not prove that the routine at the target
    is a Kosinski decoder or that the sites run: the run-time whole-image epoch verification is the authority (ADR 0142 decision 4), so a
    wrong candidate stops `materialized_image_unverified` before the sub retires an instruction. It is local tooling, not compiler core."""
    sites = []
    for i in range(0, len(bios) - 16, 2):
        if bios[i:i + 2] == b"\x41\xf9" and bios[i + 6:i + 8] == b"\x43\xf9" and bios[i + 12:i + 14] == b"\x61\x00":
            src = int.from_bytes(bios[i + 2:i + 6], "big")
            dst = int.from_bytes(bios[i + 8:i + 12], "big")
            disp = int.from_bytes(bios[i + 14:i + 16], "big", signed=True)
            if src >= len(bios) or not (WINDOW <= dst < WINDOW + MAX_IMAGE):
                continue
            try:
                data, _ = kosinski(bios, src, MAX_IMAGE - (dst - WINDOW))
            except RecipeError:
                continue
            if data:
                sites.append((i, i + 14 + disp, src, dst - WINDOW, len(data)))
    targets = {t for _, t, _, _, _ in sites}
    if not sites or len(targets) != 1 or len(sites) > MAX_STEPS:
        raise ScanError("scan_ambiguous")
    spans = sorted((d, d + n) for _, _, _, d, n in sites)
    if any(spans[k][1] > spans[k + 1][0] for k in range(len(spans) - 1)):
        raise ScanError("scan_overlap")
    end = max(hi for _, hi in spans)
    image = bytearray(end)
    for _, _, src, dst, n in sites:
        image[dst:dst + n] = kosinski(bios, src)[0]
    length = len(image.rstrip(b"\0"))
    length = (length + GRANULE - 1) // GRANULE * GRANULE
    return [(src, dst) for _, _, src, dst, _ in sites], length


def spec(recipe):
    """The emitter's canonical text form (segarecomp sega-cd emit --admit epoch-recipe:<spec>)."""
    return "%d:%s" % (recipe["image_length"], ",".join("%d@%d" % (s["source_offset"], s["dest"]) for s in recipe["steps"]))


def make_recipe(bios, image_length, steps):
    recipe = {"schema": SCHEMA, "bios_sha256": hashlib.sha256(bios).hexdigest(), "bios_size": len(bios), "image_length": image_length,
              "steps": [{"decoder": "kosinski", "source_offset": s, "dest": d} for s, d in steps]}
    validate(recipe, bios)
    return recipe


def derive(bios, epoch, log_records):
    """LOCAL/PRIVATE: infer a recipe from the writer log of a generated main-BIOS run (it never runs in a build).

    A step is proposed for every sequential output stream of main PRG RAM writes whose source span (the contiguous run of BIOS
    reads ending at the stream's first write) decodes, with the independent decoder, to exactly the bytes the stream wrote. A
    stream of only zero values is an initialisation (the BIOS's PRG RAM clear). Anything else is returned as unexplained."""
    streams = []
    expected = None
    for n, (kind, width, cpu, _, offset, value, pc) in enumerate(log_records):
        if cpu != 0 or kind != 1:
            continue
        if streams and expected == offset:
            streams[-1][1] = offset + width
            streams[-1][3] = n
        else:
            streams.append([offset, offset + width, n, n])
        expected = offset + width
    steps, unexplained, zero_init = [], [], []
    for first, last, a, b in streams:
        if all(r[5] == 0 for r in log_records[a:b + 1] if r[0] == 1 and r[2] == 0):  # a constant-zero fill: the BIOS's PRG RAM clear
            zero_init.append((first, last - first))
            continue
        # the source starts where the contiguous ascending run of BIOS reads that ends at the stream's first write begins
        start, expect = None, None
        for r in reversed(log_records[:a + 1]):
            if r[0] != 3:
                continue
            if expect is not None and r[4] != expect:
                break
            start, expect = r[4], r[4] - 1
        if start is None or start >= len(bios):
            unexplained.append((first, last - first))
            continue
        try:
            data, _ = kosinski(bios, start)
        except RecipeError:
            unexplained.append((first, last - first))
            continue
        if len(data) == last - first and epoch[first:last] == data:
            steps.append((start, first))
        else:
            unexplained.append((first, last - first))
    return steps, unexplained, zero_init


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("reconstruct", "verify", "derive", "spec", "scan"):
        p = sub.add_parser(name)
        p.add_argument("--bios", required=True)
        if name not in ("derive", "scan"):
            p.add_argument("--recipe", required=True)
        if name == "reconstruct":
            p.add_argument("--out", required=True)
        if name == "verify":
            p.add_argument("--expect-sha256")
            p.add_argument("--epoch")
        if name == "scan":
            p.add_argument("--out", required=True)
        if name == "derive":
            p.add_argument("--epoch", required=True)
            p.add_argument("--write-log", required=True)
            p.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    bios = pathlib.Path(args.bios).read_bytes()
    try:
        if args.cmd == "scan":
            steps, length = scan(bios)
            recipe = make_recipe(bios, length, steps)
            pathlib.Path(args.out).write_text(canonical(recipe) + "\n")
            print(json.dumps({"steps": len(steps), "image_length": length, "recipe_identity": identity(recipe)}))
            return 0
        if args.cmd == "derive":
            import sega_cd_epoch_provenance as prov
            epoch = pathlib.Path(args.epoch).read_bytes()
            steps, unexplained, zero_init = derive(bios, epoch, prov.read_log(args.write_log))
            recipe = make_recipe(bios, len(epoch), steps)
            pathlib.Path(args.out).write_text(canonical(recipe) + "\n")
            print(json.dumps({"steps": len(steps), "unexplained_streams": len(unexplained), "zero_init_streams": len(zero_init), "recipe_identity": identity(recipe)}))
            return 0
        recipe = json.loads(pathlib.Path(args.recipe).read_text())
        if args.cmd == "spec":
            validate(recipe, bios)
            print(spec(recipe))
            return 0
        image = reconstruct(bios, recipe)
        digest = hashlib.sha256(image).hexdigest()
        if args.cmd == "reconstruct":
            pathlib.Path(args.out).write_bytes(image)
        ok = (args.cmd == "reconstruct" or ((not args.expect_sha256 or digest == args.expect_sha256) and
                                             (not args.epoch or pathlib.Path(args.epoch).read_bytes() == image)))
        print(json.dumps({"ok": ok, "length": len(image), "sha256": digest, "recipe_identity": identity(recipe)}))
        return 0 if ok else 1
    except RecipeError as e:
        print(json.dumps({"ok": False, "error": e.kind}))
        return 1
    except ScanError as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        return 1


if __name__ == "__main__":
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    sys.exit(main())
