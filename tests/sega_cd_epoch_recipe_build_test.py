"""SEG-078 (ADR 0179): the static epoch producer end to end on the project-authored synthetic BIOS image.

The fixture BIOS (tools/sega_cd_bios_fixture.py) builds the sub program by XOR-transforming a ROM span into PRG RAM. The test appends a
Kosinski stream of the fixture's own pre-transform sub image to a copy of that BIOS (so the recipe's source is project-authored) and checks:
  - a recipe-assisted build reproduces the conventional fixed point's observable program behaviour with one fewer round (no epoch
    discovery pass), the epoch admitted with authority static_proof and verified whole at the epoch at run time;
  - every unsound recipe fails closed: emission refusals, an unverified epoch (the sub retires nothing), a broken provenance chain;
  - deterministic emission.
usage: sega_cd_epoch_recipe_build_test.py <segarecomp> <cc> <project root>"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_common as common  # noqa: E402
import sega_cd_bios_fixture as fixture  # noqa: E402
import sega_cd_epoch_common as C  # noqa: E402
import sega_cd_epoch_recipe as R  # noqa: E402

DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def emit(cue, bios, out, *extra):
    r = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(cue), "--bios", str(bios), "--out", str(out), *map(str, extra)],
                       capture_output=True, text=True, timeout=300)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else {})


def build(tmp, name, disc, bios, copy_spec, *extra):
    r = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(disc / "disc.cue"), "--bios", str(bios),
                        "--work", str(tmp / name), "--copy-image", copy_spec, *extra], capture_output=True, text=True, timeout=1800)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else {})


def run(exe, disc, bios, *extra):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", "5000000", *map(str, extra)],
                         capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


LAYOUT_DEPENDENT = {"main:d6=2", "main:d4=2"}
rom, sub, t2 = fixture.build_rom()
expected = fixture.expected(rom, sub, t2)
LENGTH = expected["sub_image_length"]
image = sub.ljust(LENGTH, b"\0")
STREAM_AT, ALT_AT = 0x1000, 0x2000
altered = bytearray(image)
altered[0x120] ^= 0xFF
bios_bytes = bytearray(rom.ljust(STREAM_AT, b"\0"))
bios_bytes += C.kosinski_encode(image) + bytes(6)
bios_bytes = bytes(bios_bytes).ljust(ALT_AT, b"\0") + C.kosinski_encode(bytes(altered)) + bytes(6)
bios_bytes += bytes(len(bios_bytes) & 1)   # the BIOS image size is even
copy = expected["copy_image"]
copy_spec = "%s:0x%x:0x%x:0x%x" % (copy["processor"], copy["base"], copy["rom_offset"], copy["length"])
good = "%d:%d@0" % (LENGTH, STREAM_AT)

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    disc = tmp / "disc"
    common.build_disc(disc, "comm")
    bios = tmp / "bios.bin"
    bios.write_bytes(bios_bytes)
    recipe = R.make_recipe(bios_bytes, LENGTH, [(STREAM_AT, 0)])
    check(R.spec(recipe) == good and R.reconstruct(bios_bytes, recipe) == image, "the Python tool reconstructs the fixture's own image")

    # ---- conventional vs recipe-assisted fixed point ----
    code, conv = build(tmp, "conv", disc, bios, copy_spec)
    check(code == 0 and conv.get("ok"), "conventional fixed point: %s" % str(conv)[-400:])
    code, rec = build(tmp, "rec", disc, bios, copy_spec, "--epoch-recipe", good)
    check(code == 0 and rec.get("ok"), "recipe-assisted fixed point: %s" % str(rec)[-400:])
    if not (conv.get("ok") and rec.get("ok")):
        sys.exit(1)
    check(conv["rounds"] == 5 and rec["rounds"] == 4, "rounds %s -> %s (one epoch discovery round fewer)" % (conv["rounds"], rec["rounds"]))
    kinds = [h.get("admitted", {}).get("kind") for h in rec["history"]]
    check(kinds == ["epoch_recipe", "operand_template", "operand_template", "operand_template", None], "recipe history %s" % kinds)
    templates = lambda s: {(h["admitted"]["cpu"], h["admitted"]["pc"]) for h in s["history"] if h.get("admitted", {}).get("kind") == "operand_template"}
    check(templates(rec) == templates(conv), "the same three operand templates are still discovered")
    authorities = {i["name"]: i["authority"] for i in rec["emit"]["images"]}
    check(authorities == {"bios": "immutable_input", "copy0": "static_proof", "epoch0": "static_proof"}, "authorities %s" % authorities)
    check(rec["program_identity"] != conv["program_identity"], "the provenance differs, so does the program identity")
    code1, a = run(conv["program"], disc, bios)
    code2, b = run(rec["program"], disc, bios)
    check(code1 == code2 == 0 and a.get("result") == b.get("result") == "idle", "both programs run to idle")
    check(a["digests"] == b["digests"] and a["result_block"] == b["result_block"] and a["ga"]["comstat"] == b["ga"]["comstat"] and
          a["cpus"] == b["cpus"], "identical observable behaviour (digests, result block, COMSTAT, both processors)")
    check(b["bios_profile"]["epochs"] == 1 and b["bios_profile"]["epoch_image"] == 0 and b["result_block"] == a["result_block"],
          "the epoch is verified whole at run time")
    check(b["ga"]["comstat"] == expected["sub_comstat"], "COMSTAT as the fixture expects")

    # ---- the run-time authority is unchanged: every fixture negative stops identically on the recipe-assisted program ----
    for spec, (cls, cpu, pc) in expected["negatives"].items():
        code, neg = run(rec["program"], disc, bios, "--set", spec)
        stop = neg.get("stop", {})
        conv_code, conv_neg = run(conv["program"], disc, bios, "--set", spec)
        same = conv_neg.get("stop", {})
        check(code == conv_code and stop == same, "negative %s: the recipe-assisted program stops exactly as the conventional one (%s vs %s)" % (spec, stop, same))
        if spec not in LAYOUT_DEPENDENT:   # these two depend on addresses past the 3.5 KiB image: this test's BIOS is longer (it carries the streams)
            check(code == 10 and stop.get("class") == cls and stop.get("cpu") == cpu and (pc is None or stop.get("pc") == pc), "negative %s -> %s" % (spec, stop))
        if cls == "materialized_image_unverified":
            check(neg["cpus"][1]["retired"] == 0, "%s: the sub retired nothing from the unverified image" % spec)
    for spec in expected["positive_variants"]:   # both epoch paths and the repeated reset
        _, ours = run(rec["program"], disc, bios, "--set", spec)
        _, theirs = run(conv["program"], disc, bios, "--set", spec)
        check(ours["digests"] == theirs["digests"] and ours["ga"]["comstat"] == theirs["ga"]["comstat"], "positive variant %s identical" % spec)

    # ---- deterministic emission ----
    c1, e1 = emit(disc / "disc.cue", bios, tmp / "a.c", "--admit", "epoch-recipe:" + good, "--copy-image", copy_spec)
    c2, e2 = emit(disc / "disc.cue", bios, tmp / "b.c", "--admit", "epoch-recipe:" + good, "--copy-image", copy_spec)
    check(c1 == c2 == 0 and (tmp / "a.c").read_bytes() == (tmp / "b.c").read_bytes() and e1["program_identity"] == e2["program_identity"],
          "deterministic emission")

    # ---- fail-closed emission ----
    def refused(spec, error, stage="emit", kind="--admit"):
        code, out = emit(disc / "disc.cue", bios, tmp / "x.c", "--admit", spec, "--copy-image", copy_spec)
        check(code != 0 and out.get("ok") is False and out.get("error") == error, "%s -> %s (got %s)" % (spec, error, out))
    refused("epoch-recipe:%d:%d@%d" % (LENGTH, STREAM_AT + 2, 0), "epoch_recipe_invalid")           # decodes garbage: a typed decode failure
    refused("epoch-recipe:%d:%d@0" % (LENGTH + 1, STREAM_AT), "epoch_recipe_invalid")               # not a whole snapshot length
    refused("epoch-recipe:%d:%d@0" % (LENGTH - 0x100, STREAM_AT), "epoch_recipe_invalid")          # the stream decodes past the image
    refused("epoch-recipe:%d:%d@0,%d@16" % (LENGTH, STREAM_AT, STREAM_AT), "epoch_recipe_invalid")  # overlapping steps
    refused("epoch-recipe:%d:%d@0" % (LENGTH, len(bios_bytes) + 4), "epoch_recipe_invalid")         # source outside the BIOS image
    refused("epoch-recipe:0%d:%d@0" % (LENGTH, STREAM_AT), "epoch_recipe_syntax")                   # non-canonical spelling
    refused("epoch-recipe:%d:" % LENGTH, "epoch_recipe_syntax")                                      # no steps
    refused("epoch-recipe:%d:%d@0," % (LENGTH, STREAM_AT), "epoch_recipe_syntax")
    refused("epoch-recipe:%d:%d@0:x" % (LENGTH, STREAM_AT), "epoch_recipe_syntax")
    refused("epoch-recipe:" + ",".join(["%d:%d@0" % (LENGTH, STREAM_AT)] * 1) + "@1", "epoch_recipe_syntax")
    refused_fabricated = emit(disc / "disc.cue", bios, tmp / "x.c", "--admit", "epoch:%s:%s" % ("0" * 64, tmp / "bios.bin"), "--copy-image", copy_spec)[1]
    check(refused_fabricated.get("error") == "provenance_mismatch", "a fabricated pass identity on an epoch is refused: %s" % refused_fabricated)
    # the BIOS the recipe was not made for: the plain fixture image has no stream there
    plain = tmp / "plain.bin"
    plain.write_bytes(rom)
    c, out = emit(disc / "disc.cue", plain, tmp / "x.c", "--admit", "epoch-recipe:" + good, "--copy-image", copy_spec)
    check(c != 0 and out.get("error") == "epoch_recipe_invalid", "a recipe applied to another BIOS image is refused: %s" % out)
    # a chain break after a recipe is still a chain break (no fabricated pass identity)
    c, out = emit(disc / "disc.cue", bios, tmp / "x.c", "--admit", "epoch-recipe:" + good, "--copy-image", copy_spec,
                  "--admit", "template:%s:main:0x%x:4ef900000000" % ("0" * 64, expected["thunks"]["main"]))
    check(c != 0 and out.get("error") == "provenance_mismatch" and out.get("index") == 1, "a fabricated pass identity is refused: %s" % out)

    # ---- a recipe that decodes but describes another image: wrong destination, patched stream, stale BIOS bytes ----
    for label, bios_file, spec_text in (("wrong destination", bios, "%d:%d@256" % (LENGTH + 256, STREAM_AT)),
                                        ("patched stream", bios, "%d:%d@0" % (LENGTH, ALT_AT)),):
        code, out = emit(disc / "disc.cue", bios_file, tmp / "p.c", "--admit", "epoch-recipe:" + spec_text, "--copy-image", copy_spec)
        check(code == 0, "%s emits (decodable): %s" % (label, out))
    stale = bytearray(bios_bytes)
    stale[STREAM_AT + 0x40] ^= 0x01     # the BIOS stream the recipe was made for changed by one bit
    stale_path = tmp / "stale.bin"
    stale_path.write_bytes(bytes(stale))
    code, out = emit(disc / "disc.cue", stale_path, tmp / "s.c", "--admit", "epoch-recipe:" + good, "--copy-image", copy_spec)
    if code != 0:
        check(out.get("error") == "epoch_recipe_invalid", "a stale BIOS image is refused at emission: %s" % out)
    else:
        code, failed = build(tmp, "stale", disc, stale_path, copy_spec, "--epoch-recipe", good)
        check(code == 1 and failed.get("failure") == "epoch_recipe_unverified", "stale BIOS bytes fail closed at the epoch: %s" % failed)
    for label, spec_text in (("wrong destination", "%d:%d@256" % (LENGTH + 256, STREAM_AT)),):
        code, failed = build(tmp, "wd", disc, bios, copy_spec, "--epoch-recipe", spec_text)
        check(code == 1 and failed.get("failure") == "epoch_recipe_unverified", "%s fails closed at the epoch: %s" % (label, failed))

    # ---- a decodable but wrong image: the epoch verification stops before the sub retires anything ----
    wrong = "%d:%d@0" % (LENGTH, ALT_AT)
    c, out = emit(disc / "disc.cue", bios, tmp / "w.c", "--admit", "epoch-recipe:" + wrong, "--copy-image", copy_spec)
    check(c == 0, "a decodable recipe emits (the run time is the authority): %s" % out)
    code, bad = build(tmp, "bad", disc, bios, copy_spec, "--epoch-recipe", wrong)
    check(code == 1 and bad.get("failure") == "epoch_recipe_unverified", "the driver fails closed on an unverified recipe epoch: %s" % bad)
    # run that program directly: typed stop, sub retired nothing
    exe = tmp / "bad" / "program"
    if exe.exists():
        _, report = run(exe, disc, bios)
        check(report["stop"]["class"] == "materialized_image_unverified" and report["cpus"][1]["retired"] == 0,
              "unverified epoch: typed stop with the sub having retired nothing (%s)" % report["stop"])
    # the recipe-assisted program refuses a different BIOS image before any instruction (the identity is bound by the program)
    out = subprocess.run([str(rec["program"]), "--disc-dir", str(disc), "--bios", str(plain), "--instruction-budget", "100"],
                         capture_output=True, text=True, timeout=60)
    result = json.loads(out.stdout) if out.stdout.strip() else {}
    check(out.returncode == 21 and result.get("result") == "bios_refused", "a different BIOS image is refused: %s" % result)

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("sega_cd_epoch_recipe_build_test: ok")
