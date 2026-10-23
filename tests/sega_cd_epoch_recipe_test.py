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
import sega_cd_template_provenance as T  # noqa: E402

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
DEST = [0x0000, 0x2000, 0x5000]


def call_site(src, dst, target_disp=0x40):
    """LEA abs.l,A0 ; LEA abs.l,A1 ; BSR.W - a project-authored call shape (the scan's only structural input)."""
    return b"\x41\xf9" + struct.pack(">I", src) + b"\x43\xf9" + struct.pack(">I", 0x20000 + dst) + b"\x61\x00" + struct.pack(">h", target_disp)


for k, (o, d) in enumerate(zip(offsets, DEST)):
    bios[0x600 + 0x20 * k:0x600 + 0x20 * k + 16] = call_site(o, d, 0x1000 - (0x600 + 0x20 * k + 14))
bios = bytes(bios)
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

# ---- the committed synthetic example in the schema document validates against its project-authored BIOS pattern ----
doc = (ROOT / "docs" / "architecture" / "sega-cd-epoch-recipe.md").read_text()
example = json.loads(doc.split("```json\n")[1].split("\n```")[0])
try:
    R.validate(example, pad)
    check(R.canonical(example) == doc.split("```json\n")[1].split("\n```")[0], "the documented example is in canonical form")
except R.RecipeError as e:
    check(False, "the documented example does not validate: %s" % e.kind)

# ---- the structural scan: candidates from the BIOS image alone, no execution ----
scanned, scanned_length = R.scan(bios)
check(scanned == list(zip(offsets, DEST)) and scanned_length == LENGTH, "scan finds the call sites and the image length (%s %s)" % (scanned, scanned_length))
check(R.reconstruct(bios, R.make_recipe(bios, scanned_length, scanned)) == image, "the scanned recipe reconstructs the same image")
variant = bytearray(bios)
variant[0x600 + 0x20 + 14:0x600 + 0x20 + 16] = struct.pack(">h", 0x2000)           # one site calls another routine
try:
    R.scan(bytes(variant))
    check(False, "two call targets accepted")
except R.ScanError as e:
    check(str(e) == "scan_ambiguous", "two call targets refused: %s" % e)
variant = bytearray(bios)
variant[0x600 + 0x40:0x600 + 0x50] = call_site(offsets[2], DEST[0] + 0x100, 0x1000 - (0x640 + 14))         # destination inside step 0's range
try:
    R.scan(bytes(variant))
    check(False, "overlapping destinations accepted")
except R.ScanError as e:
    check(str(e) == "scan_overlap", "overlapping destinations refused: %s" % e)
variant = bytearray(bios)
for k in range(3):
    variant[0x600 + 0x20 * k:0x600 + 0x20 * k + 16] = bytes(16)
try:
    R.scan(bytes(variant))
    check(False, "a BIOS without call sites accepted")
except R.ScanError:
    check(True, "a BIOS without call sites refused")
variant = bytearray(bios)
variant[0x600 + 8:0x600 + 12] = struct.pack(">I", 0x100)                             # destination outside the PRG-RAM window: not a site
check(R.scan(bytes(variant))[0] == list(zip(offsets, DEST))[1:], "a destination outside the window is not a site")

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


# ---- mutable JMP template writer attribution ----
def store(kind, cpu, offset, value, pc, width=2):
    return C.log_record(kind, width, cpu, offset, value, pc)


tlog = b""
for i in range(5):   # main: a 5-slot table built by ROM code in work RAM, then slot 1 re-installed by other ROM code
    tlog += store(C.KIND_WORK, 0, 0xFD00 + 6 * i, 0x4EF9, 0x3C0) + store(C.KIND_WORK, 0, 0xFD02 + 6 * i, 0x0000, 0x3C2, 4)
tlog += store(C.KIND_WORK, 0, 0xFD06, 0x4EF9, 0x2000)
for i in range(4):   # sub: a 4-slot table built by code inside a reconstructed range
    tlog += store(C.KIND_PRG, 1, 0x5EE0 + 6 * i, 0x4EF9, 0x0340)
tlog += store(C.KIND_PRG, 1, 0x7000, 0x4EF9, 0x9000)          # a lone slot written by code no image explains
tlog += store(C.KIND_PRG, 1, 0x7100, 0x4EB9, 0x0340)          # a JSR opcode: never a template store
tlog += store(C.KIND_PRG, 1, 0x7200, 0x4EF90000, 0x0340, 4)         # long store: the high half is the opcode
with tempfile.TemporaryDirectory() as tmp2:
    tmp2 = pathlib.Path(tmp2)
    (tmp2 / "t.bin").write_bytes(tlog)
    trep, tdetail = T.classify([(0, 0xFFFD06), (0, 0xFFFD18), (1, 0x5EE6), (1, 0x7000), (1, 0x9999), (1, 0x7100), (1, 0x7200)],
                               P.read_log(tmp2 / "t.bin"), 0x20000, [(0, 0x5800)])
check(trep["classes"] == {"static_writer": 2, "reconstructed_writer": 2, "runtime_writer": 1, "unattributed": 2},
      "template writer attribution %s" % trep["classes"])
check(trep["slot_runs"]["largest"][:2] == [5, 4] and trep["templates_in_multi_slot_runs"] == 3 and trep["statically_derived_before_execution"] == 0,
      "slot runs %s" % trep["slot_runs"])
check(not any(k in json.dumps(trep) for k in ('"address"', '"pc"')), "template report holds counts only")


# ---- mutation-driven negatives (adversarial review) ----
with tempfile.TemporaryDirectory() as tmp3:
    tmp3 = pathlib.Path(tmp3)
    (tmp3 / "w.bin").write_bytes(log_bytes)
    recs3 = P.read_log(tmp3 / "w.bin")
    bad_image = bytearray(image)
    bad_image[DEST[0] + 7] ^= 0x10                      # a decoded stream whose bytes no longer match what the decoder produces
    repb, _ = P.classify(bytes(bad_image), {"bios": bios}, recs3)
    rb = {row["class"]: row for row in repb["classes"]}
    check(rb[4]["regions"] == 2 and rb[6]["bytes"] == len(parts[0]) and not repb["fully_explained"],
          "a stream the independent decoder does not reproduce is class 6, never class 4")
    # a stream of non-zero values over bytes that are zero in the image is not a zero fill
    nz = b"".join(C.log_record(C.KIND_PRG, 2, 0, a, 0x0101, 0x100) for a in range(0, 0x100, 2))
    (tmp3 / "nz.bin").write_bytes(nz)
    repn, _ = P.classify(bytes(0x100), {"bios": bios}, P.read_log(tmp3 / "nz.bin"))
    rn = {row["class"]: row for row in repn["classes"]}
    check(rn[6]["bytes"] == 0x100 and not rn[5]["independently_supported"] and rn[5]["bytes"] == 0, "non-zero writes are not a zero fill")
    # unwritten zero bytes are counted as class 5 but never reported as supported
    (tmp3 / "none.bin").write_bytes(C.log_record(C.KIND_WORK, 2, 0, 0, 1, 0x100))
    repu, _ = P.classify(bytes(0x100), {"bios": bios}, P.read_log(tmp3 / "none.bin"))
    ru = {row["class"]: row for row in repu["classes"]}
    check(ru[5]["bytes"] == 0x100 and not ru[5]["independently_supported"], "unwritten zeros are not writer-supported")
    # a main template whose writer runs from RAM (not the BIOS image) is a runtime writer
    mlog = store(C.KIND_WORK, 0, 0xFD00, 0x4EF9, 0x21000)
    (tmp3 / "m.bin").write_bytes(mlog)
    repm, _ = T.classify([(0, 0xFFFD00)], P.read_log(tmp3 / "m.bin"), 0x20000, [])
    check(repm["classes"]["runtime_writer"] == 1 and repm["classes"]["static_writer"] == 0, "a main writer outside the BIOS image is a runtime writer")

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("sega_cd_epoch_recipe_test: ok")
