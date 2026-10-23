"""SEG-078 (ADR 0178/0179): the Sega CD epoch-image recipe, its independent decoder and the provenance classifier on project-authored
synthetic inputs only. Stdlib only; no commercial byte. usage: sega_cd_epoch_recipe_test.py <project root>"""
import copy
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_epoch_common as C  # noqa: E402
import sega_cd_epoch_provenance as P  # noqa: E402
import sega_cd_epoch_recipe as R  # noqa: E402

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def raises(kind, fn, *args):
    try:
        fn(*args)
    except R.RecipeError as e:
        return e.kind == kind
    return False


# ---- hand-derived vectors: the format as documented, not produced by the encoder under test ----
VECTORS = {
    "two literals": (bytes.fromhex("0b00 4142 f0f000"), b"AB"),
    "short copy (3 bytes, offset -2)": (bytes.fromhex("a300 4142 fe f0f000"), b"ABABA"),
    "long copy (count field 1 -> 3 bytes, offset -3)": (bytes.fromhex("5700 78797a fdf9 f0f000"), b"xyzxyz"),
    "extended count (extra 6 -> 7 bytes, offset -1)": (bytes.fromhex("1500 71 fff8 06 f0f000"), b"q" * 8),
    "extra 1 is a no-op": (bytes.fromhex("1500 61 fff8 01 f0f000"), b"a"),
    # 16 literals: the next description word is fetched when the 16th bit is consumed, i.e. before the 16th literal byte
    "refill before the sixteenth literal": (bytes.fromhex("ffff") + bytes(range(0x41, 0x50)) + bytes.fromhex("0200") + b"P" + bytes.fromhex("f0f000"),
                                            bytes(range(0x41, 0x51))),
}
for name, (stream, expected) in VECTORS.items():
    try:
        out, end = R.kosinski(stream + b"\0\0", 0)
        check(out == expected and end == len(stream), "vector %s: %r (end %d of %d)" % (name, out, end, len(stream)))
    except R.RecipeError as e:
        check(False, "vector %s raised %s" % (name, e.kind))

# ---- round trip over all encoder variants, deterministic ----
for seed in range(1, 6):
    data = C.synthetic_image(2500 + 137 * seed, seed)
    for kw in ({}, {"literal_only": True}, {"short": False}, {"long": False}):
        enc = C.kosinski_encode(data, **kw)
        out, end = R.kosinski(enc + b"\0\0", 0)
        check(out == data and end <= len(enc) + 2, "round trip seed %d %s" % (seed, kw))
        check(C.kosinski_encode(data, **kw) == enc, "encoder deterministic")
check(len(C.kosinski_encode(C.synthetic_image(4000, 9))) < 4000, "synthetic data is compressible (copies exercised)")

# ---- a synthetic BIOS-like image with three streams and a recipe ----
pad = bytes((i * 37 + 11) & 0xFF for i in range(0x2000))
parts = [C.synthetic_image(0x1800 + 0x40 * k, 20 + k) for k in range(3)]
streams = [C.kosinski_encode(p) for p in parts]
bios = bytearray(pad)
bios[0x500:0x580] = parts[1][0x100:0x180]   # a plain copy of part of the decoded data elsewhere in the BIOS: equality evidence that is not a producer
offsets = []
for s in streams:
    offsets.append(len(bios))
    bios += s + bytes(6)   # trailing bytes: the decoder's eager final refill may read two bytes past the end marker
bios = bytes(bios)
DEST = [0x0000, 0x2000, 0x5000]
LENGTH = 0x6900
expected_image = bytearray(LENGTH)
for p, d in zip(parts, DEST):
    expected_image[d:d + len(p)] = p
expected_image = bytes(expected_image)
recipe = R.make_recipe(bios, LENGTH, list(zip(offsets, DEST)))
image = R.reconstruct(bios, recipe)
check(image == expected_image, "reconstruction equals the independently held image")
check(R.reconstruct(bios, copy.deepcopy(recipe)) == image, "deterministic reconstruction")
check(hashlib.sha256(image).hexdigest() == hashlib.sha256(expected_image).hexdigest(), "SHA-256 equal")
check(R.identity(recipe) == R.identity(json.loads(R.canonical(recipe))), "identity is canonical")
check(R.spec(recipe) == "%d:%s" % (LENGTH, ",".join("%d@%d" % (o, d) for o, d in zip(offsets, DEST))), "emitter spec text")
check("bytes" not in R.canonical(recipe) and len(R.canonical(recipe)) < 400, "a recipe carries no image bytes")

# ---- fail-closed matrix ----
def mutate(path, value):
    r = copy.deepcopy(recipe)
    target = r
    for key in path[:-1]:
        target = target[key]
    if value is KeyError:
        del target[path[-1]]
    else:
        target[path[-1]] = value
    return r


cases = {
    "recipe_fields": [mutate(["extra"], 1) if False else dict(recipe, extra=1), mutate(["steps"], KeyError), mutate(["image_length"], KeyError)],
    "recipe_schema": [mutate(["schema"], "segarecomp.sega_cd.epoch_recipe.v0"), mutate(["schema"], 5)],
    "recipe_bios_sha256": [mutate(["bios_sha256"], "00"), mutate(["bios_sha256"], "G" * 64), mutate(["bios_sha256"], recipe["bios_sha256"].upper())],
    "recipe_integer": [mutate(["bios_size"], True), mutate(["image_length"], 28672.0), mutate(["steps", 0, "dest"], -1), mutate(["steps", 0, "dest"], "0")],
    "recipe_bounds": [mutate(["image_length"], 0), mutate(["image_length"], 0x6901), mutate(["image_length"], 512 * 1024 + 256),
                      mutate(["bios_size"], 0), mutate(["steps", 0, "source_offset"], len(bios)), mutate(["steps", 1, "dest"], LENGTH)],
    "recipe_steps": [mutate(["steps"], []), mutate(["steps"], recipe["steps"] * 3), mutate(["steps"], "x")],
    "recipe_step_fields": [mutate(["steps", 0, "extra"], 1), mutate(["steps", 0, "dest"], KeyError), mutate(["steps"], [1])],
    "recipe_decoder": [mutate(["steps", 0, "decoder"], "lz77"), mutate(["steps", 0, "decoder"], "Kosinski")],
    "bios_identity_mismatch": [mutate(["bios_size"], len(bios) - 2)],
}
for kind, recipes in cases.items():
    for i, r in enumerate(recipes):
        check(raises(kind, R.reconstruct, bios, r), "%s case %d" % (kind, i))
wrong_bios = bytearray(bios)
wrong_bios[10] ^= 1
check(raises("bios_identity_mismatch", R.reconstruct, bytes(wrong_bios), recipe), "wrong BIOS image refused (identity)")
check(raises("bios_identity_mismatch", R.reconstruct, bios + b"\0", recipe), "wrong BIOS size refused")
r = copy.deepcopy(recipe)
r["steps"][1]["dest"] = DEST[0] + 0x100   # overlaps step 0's decoded range
check(raises("steps_overlap", R.reconstruct, bios, r), "overlapping steps refused")
r = copy.deepcopy(recipe)
r["image_length"] = 0x2800                # step 2 starts past the image
check(raises("recipe_bounds", R.reconstruct, bios, r), "a step starting outside the image refused")
r = copy.deepcopy(recipe)
r["steps"][0]["dest"] = LENGTH - 0x100    # the stream decodes past the image end
check(raises("decoded_length_exceeded", R.reconstruct, bios, r), "output past the image refused")
short = bios[:offsets[2] + 20]            # the BIOS cut inside the last stream, a recipe made for the cut image
cut = R.make_recipe(short, LENGTH, list(zip(offsets, DEST)))
check(raises("source_exhausted", R.reconstruct, short, cut), "truncated source refused")
tail_zero = R.make_recipe(bios, LENGTH + 0x1000, list(zip(offsets, DEST)))   # the last granule is all zero: not a whole snapshot
check(raises("image_not_whole_snapshot", R.reconstruct, bios, tail_zero), "whole-snapshot rule (last granule non-zero)")
back = bytes.fromhex("0300 f0f000")                                    # first operation: short copy before the stream start
check(raises("source_exhausted", R.kosinski, back, 0) or raises("reference_before_stream", R.kosinski, back + b"\0\0\0", 0),
      "reference before the stream start refused")
check(raises("reference_before_stream", R.kosinski, bytes.fromhex("0100 fe f0f000") + b"\0\0", 0), "short copy at stream start refused")
saved = R.MAX_DECODE_STEPS
R.MAX_DECODE_STEPS = 50
noop = bytearray(b"\xaa\xaa")   # an endless run of no-op long copies (count field 0, extra byte 1)
for op in range(200):
    if op % 8 == 7:
        noop += b"\xaa\xaa"       # the word is refetched when its 16th bit (the last of the eighth operation) is consumed, before its data
    noop += b"\xff\xf8\x01"
check(raises("token_bound_exceeded", R.kosinski, bytes(noop) + b"\0" * 16, 0), "token bound")
R.MAX_DECODE_STEPS = saved
flipped = bytearray(bios)
flipped[offsets[0] + 40] ^= 0xFF
flipped_recipe = R.make_recipe(bytes(flipped), LENGTH, list(zip(offsets, DEST)))
try:
    check(R.reconstruct(bytes(flipped), flipped_recipe) != image, "a corrupted stream does not reproduce the image")
except R.RecipeError:
    check(True, "a corrupted stream fails closed")

# ---- derive from a synthetic writer log (the private, local step) ----
def build_log(bios_bytes, steps, image_length, zero_pc=0x100, dec_pc=0x200):
    rec = b""
    for a in range(0, image_length, 2):
        rec += C.log_record(C.KIND_PRG, 2, 0, a, 0, zero_pc)
    for src, dst in steps:
        out, end = R.kosinski(bios_bytes, src)
        for i in range(src, end):
            rec += C.log_record(C.KIND_ROM, 1, 0, i, bios_bytes[i], dec_pc)
        for i, b in enumerate(out):
            rec += C.log_record(C.KIND_PRG, 1, 0, dst + i, b, dec_pc + 2)
    return rec


log_bytes = build_log(bios, list(zip(offsets, DEST)), LENGTH)
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / "bios.bin").write_bytes(bios)
    (tmp / "epoch.bin").write_bytes(image)
    (tmp / "w.bin").write_bytes(log_bytes)
    records = P.read_log(tmp / "w.bin")
    steps, unexplained, zero_init = R.derive(bios, image, records)
    check(steps == list(zip(offsets, DEST)) and not unexplained and len(zero_init) == 1, "derive recovers the recipe steps (%s %s %s)" % (steps, unexplained, zero_init))
    tampered = bytearray(image)
    tampered[DEST[1] + 5] ^= 0x55
    steps2, unexplained2, _ = R.derive(bios, bytes(tampered), records)
    check(len(unexplained2) == 1 and len(steps2) == 2, "a stream that no decoder reproduces is unexplained, not proposed")
    (tmp / "bad.bin").write_bytes(log_bytes + b"\0")
    try:
        P.read_log(tmp / "bad.bin")
        check(False, "malformed log accepted")
    except P.ProvenanceError:
        check(True, "malformed log refused")

    # ---- provenance classification: sanitized table ----
    report, detail = P.classify(image, {"bios": bios}, records)
    rows = {row["class"]: row for row in report["classes"]}
    zero_bytes = sum(1 for b in image if False) + LENGTH - sum(len(p) for p in parts)
    check(rows[4]["bytes"] == sum(len(p) for p in parts) and rows[4]["regions"] == 3 and rows[4]["independently_supported"],
          "decoded streams are class 4 and independently supported")
    check(rows[5]["bytes"] == zero_bytes and rows[5]["independently_supported"] and rows[6]["bytes"] == 0 and report["fully_explained"],
          "the clear is class 5 and nothing is unknown")
    check(abs(sum(row["percent"] for row in report["classes"]) - 100.0) < 0.01, "percentages sum to 100")
    check(report["supported_bytes"] == LENGTH, "every byte supported")
    allowed_keys = {"image_bytes", "classes", "equality_only_bytes", "fully_explained", "supported_bytes"}
    check(set(report) == allowed_keys and all(set(row) == {"class", "name", "regions", "bytes", "percent", "independently_supported",
                                                          "unresolved_dynamic_behaviour", "executed_instruction_starts"}
                                              for row in report["classes"]), "the report holds only sanitized aggregates")
    check(not any(k in json.dumps(report) for k in ('"start"', '"offset"', '"address"', '"pc"')), "no offsets or addresses in the report")
    check(report["equality_only_bytes"] > 0, "equality search alone would claim bytes the writers do not copy")
    # coverage: executed starts per class
    starts = {DEST[0] + 4, DEST[0] + 8, DEST[2] + 2, 0x1F00 + 0x40, 0x3900}
    report2, _ = P.classify(image, {"bios": bios}, records, starts)
    rows2 = {row["class"]: row for row in report2["classes"]}
    check(rows2[4]["executed_instruction_starts"] + rows2[5]["executed_instruction_starts"] == len(starts), "executed starts are split by class")

    # verbatim copy loop (class 1) and a segmented copy (class 2): project-authored copy producers
    src_bytes = bytes((i * 7 + 3) & 0xFF for i in range(0x600))
    def copy_log(segments, length):
        rec = b""
        for a in range(0, length, 2):
            rec += C.log_record(C.KIND_PRG, 2, 0, a, 0, 0x100)
        for dst, so, n in segments:
            for i in range(n):
                rec += C.log_record(C.KIND_ROM, 1, 0, so + i, src_bytes[so + i], 0x300)
                rec += C.log_record(C.KIND_PRG, 1, 0, dst + i, src_bytes[so + i], 0x302)
        return rec
    img1 = bytearray(0x400)
    img1[0:0x100] = src_bytes[0x40:0x140]
    img1[0x300:0x400] = bytes((i * 3 + 1) & 0xFF for i in range(0x100))  # not written by a copy: see below
    img1[0x300:0x400] = bytes(0x100)
    img1[0xFF] = img1[0xFF] or 1
    log1 = build_log  # silence linters
    (tmp / "c1.bin").write_bytes(copy_log([(0, 0x40, 0x100)], 0x400))
    rep1, _ = P.classify(bytes(img1), {"bios": src_bytes}, P.read_log(tmp / "c1.bin"))
    r1 = {row["class"]: row for row in rep1["classes"]}
    check(r1[1]["bytes"] == 0x100 and r1[1]["independently_supported"] and r1[2]["bytes"] == 0, "a single-delta copy loop is class 1")
    img2 = bytearray(0x400)
    img2[0:0x80] = src_bytes[0x200:0x280]
    img2[0x80:0x100] = src_bytes[0x40:0xC0]
    (tmp / "c2.bin").write_bytes(copy_log([(0, 0x200, 0x80), (0x80, 0x40, 0x80)], 0x400))
    rep2, _ = P.classify(bytes(img2), {"bios": src_bytes}, P.read_log(tmp / "c2.bin"))
    r2 = {row["class"]: row for row in rep2["classes"]}
    check(r2[2]["bytes"] == 0x100 and r2[2]["independently_supported"], "two source segments are class 2")
    # equal bytes without a copying writer (immediate stores) are NOT a copy
    img3 = bytearray(0x400)
    img3[0:0x100] = src_bytes[0x40:0x140]
    rec3 = b"".join(C.log_record(C.KIND_PRG, 2, 0, a, 0, 0x100) for a in range(0, 0x400, 2))
    rec3 += b"".join(C.log_record(C.KIND_PRG, 1, 0, i, img3[i], 0x400) for i in range(0x100))
    (tmp / "c3.bin").write_bytes(rec3)
    rep3, _ = P.classify(bytes(img3), {"bios": src_bytes}, P.read_log(tmp / "c3.bin"))
    r3 = {row["class"]: row for row in rep3["classes"]}
    check(r3[1]["bytes"] == 0 and r3[6]["bytes"] == 0x100 and not r3[6]["independently_supported"] and r3[6]["unresolved_dynamic_behaviour"] and
          rep3["equality_only_bytes"] >= 0x100 and not rep3["fully_explained"], "equality without a copying writer is unknown provenance (class 6)")
    # a non-zero byte nobody wrote
    img4 = bytearray(0x400)
    img4[0x200] = 0x77
    rec4 = b"".join(C.log_record(C.KIND_PRG, 2, 0, a, 0, 0x100) for a in range(0, 0x200, 2))
    (tmp / "c4.bin").write_bytes(rec4)
    rep4, _ = P.classify(bytes(img4), {"bios": src_bytes}, P.read_log(tmp / "c4.bin"))
    r4 = {row["class"]: row for row in rep4["classes"]}
    check(r4[6]["bytes"] == 1, "an unwritten non-zero byte is unknown")
    # a sub write ends the main-only analysis
    rec5 = rec4 + C.log_record(C.KIND_PRG, 2, 1, 0x200, 0x7700, 0x10)
    (tmp / "c5.bin").write_bytes(rec5)
    check(len(P.streams(P.read_log(tmp / "c5.bin"))) == len(P.streams(P.read_log(tmp / "c4.bin"))), "sub writes are not main producers")

    # ---- CLI ----
    recipe_file = tmp / "recipe.json"
    recipe_file.write_text(R.canonical(recipe))
    tool = [sys.executable, str(ROOT / "tools" / "sega_cd_epoch_recipe.py")]
    out = subprocess.run(tool + ["reconstruct", "--bios", str(tmp / "bios.bin"), "--recipe", str(recipe_file), "--out", str(tmp / "r.bin")],
                         capture_output=True, text=True)
    info = json.loads(out.stdout)
    check(out.returncode == 0 and info["sha256"] == hashlib.sha256(image).hexdigest() and (tmp / "r.bin").read_bytes() == image, "CLI reconstruct")
    out = subprocess.run(tool + ["verify", "--bios", str(tmp / "bios.bin"), "--recipe", str(recipe_file), "--epoch", str(tmp / "epoch.bin"),
                                "--expect-sha256", hashlib.sha256(image).hexdigest()], capture_output=True, text=True)
    check(out.returncode == 0 and json.loads(out.stdout)["ok"], "CLI verify")
    out = subprocess.run(tool + ["verify", "--bios", str(tmp / "bios.bin"), "--recipe", str(recipe_file), "--expect-sha256", "0" * 64],
                         capture_output=True, text=True)
    check(out.returncode == 1 and not json.loads(out.stdout)["ok"], "CLI verify with the wrong digest fails")
    (tmp / "other.bin").write_bytes(bytes(wrong_bios))
    out = subprocess.run(tool + ["verify", "--bios", str(tmp / "other.bin"), "--recipe", str(recipe_file)], capture_output=True, text=True)
    check(out.returncode == 1 and json.loads(out.stdout)["error"] == "bios_identity_mismatch", "CLI wrong BIOS")
    out = subprocess.run(tool + ["spec", "--bios", str(tmp / "bios.bin"), "--recipe", str(recipe_file)], capture_output=True, text=True)
    check(out.returncode == 0 and out.stdout.strip() == R.spec(recipe), "CLI spec")
    prov = [sys.executable, str(ROOT / "tools" / "sega_cd_epoch_provenance.py"), "--epoch", str(tmp / "epoch.bin"), "--source", "bios=" + str(tmp / "bios.bin"),
            "--write-log", str(tmp / "w.bin")]
    out = subprocess.run(prov, capture_output=True, text=True)
    check(out.returncode == 0 and json.loads(out.stdout)["fully_explained"], "CLI provenance")

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("sega_cd_epoch_recipe_test: ok")
