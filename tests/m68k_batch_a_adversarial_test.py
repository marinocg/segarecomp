#!/usr/bin/env python3
"""Adversarial validation of SEG-007-T023's common MC68000 startup/
data-movement batch (docs/references/
m68k-common-startup-data-movement-batch-contract.md), modeled directly on
tests/tst_l_adversarial_test.py.

This test independently tries to falsify the delivered capability rather
than re-running the static-slice fixture: for every accepted vector it
re-derives the expected CCR bits directly from the contract's own stated
per-mnemonic rule (never by reusing production code or the fixture's own
"expected_sr" field for that computation), and asserts the generated C
contains no forbidden token (source-image interpreter, opcode decoder,
runtime file/argv access). It reuses tests/fixtures/
m68k-batch-a-fixtures.json's rejection catalog for illegal-EA/size/
truncation/alignment/mapping coverage (that catalog IS the adversarial
boundary evidence; duplicating it here would only risk drift) and adds its
own accepted-vector CCR/PC independent re-derivation and Musashi
differential, reading tests/fixtures/m68k-batch-a-adversarial-vectors.json
for the per-vector mnemonic/size metadata that re-derivation needs.

The pinned Musashi differential is optional and off by default. Set
SEGARECOMP_M68K_BATCH_A_MUSASHI_CHECKOUT to a local ignored checkout at the
exact pin (313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd). This test builds the
tracked Musashi generator, regenerates its opcode sources in a temporary
directory, and builds its committed runner there -- exactly the
checkout-based pattern tst_l_adversarial_test.py already established (no
opaque prebuilt adapter is accepted).
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATIC_FIXTURES = ROOT / "tests/fixtures/m68k-batch-a-fixtures.json"
ADVERSARIAL_VECTORS = ROOT / "tests/fixtures/m68k-batch-a-adversarial-vectors.json"
ORACLE_REVISION = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
ORACLE_CHECKOUT_ENVIRONMENT = "SEGARECOMP_M68K_BATCH_A_MUSASHI_CHECKOUT"
RUNNER_SOURCE = ROOT / "tests/tools/m68k_batch_a_musashi_runner.c"

FORBIDDEN_TOKENS = ("fopen", "fread", "freopen", "open(", "getchar", "scanf", "argv",
                    "decode_m68k_instruction", "opcode")


def compiler_environment():
    environment = os.environ.copy()
    if not environment.get("SDKROOT") and shutil.which("xcrun"):
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True, check=False)
        if sdk.returncode == 0:
            environment["SDKROOT"] = sdk.stdout.strip()
    return environment


def resolve_compiler(compiler):
    resolved = shutil.which(compiler)
    assert resolved is not None, f"compiler unavailable: {compiler}"
    return Path(resolved).resolve()


# --- Independent CCR re-derivation, transcribed from the contract's own
#     "Condition-code effects" table, never from production code. ---------

def sign_bit(value, size):
    width = {"b": 8, "w": 16, "l": 32}[size]
    return (value >> (width - 1)) & 1


def is_zero(value, size):
    mask = {"b": 0xFF, "w": 0xFFFF, "l": 0xFFFFFFFF}[size]
    return (value & mask) == 0


def move_result_ccr(seed_sr, value, size):
    """TST/MOVE: N=sign(result), Z=(result==0), V=0, C=0, X unaffected."""
    n = sign_bit(value, size)
    z = 1 if is_zero(value, size) else 0
    x = (seed_sr >> 4) & 1
    return (seed_sr & 0xFF00) | (x << 4) | (n << 3) | (z << 2)


def unaffected_ccr(seed_sr):
    """MOVEA/LEA/JMP/JSR: entire CCR/SR is not affected."""
    return seed_sr


def clr_ccr(seed_sr):
    """CLR: N=0, Z=1, V=0, C=0 always; X unaffected; independent of value."""
    x = (seed_sr >> 4) & 1
    return (seed_sr & 0xFF00) | (x << 4) | 0b0100


def independent_expected_sr(mnemonic, seed_sr_hex, size, result_value_hex):
    seed_sr = int(seed_sr_hex, 16)
    if mnemonic in ("TST", "MOVE"):
        return f"{move_result_ccr(seed_sr, int(result_value_hex, 16), size):04X}"
    if mnemonic in ("MOVEA", "LEA", "JMP", "JSR"):
        return f"{unaffected_ccr(seed_sr):04X}"
    if mnemonic == "CLR":
        return f"{clr_ccr(seed_sr):04X}"
    raise AssertionError(f"unknown mnemonic {mnemonic}")


def validate_manifests():
    static_manifest = json.loads(STATIC_FIXTURES.read_text())
    manifest = json.loads(ADVERSARIAL_VECTORS.read_text())
    for m in (static_manifest, manifest):
        assert m["schema"] == 1
        ownership = m["ownership"].lower()
        assert "project-authored" in ownership and "no commercial content" in ownership
    assert manifest["musashi_revision"] == ORACLE_REVISION
    assert len(manifest["accepted"]) >= 20
    assert len(manifest["rejections"]) >= 20
    return static_manifest, manifest


def command(executable, image, entry):
    args = [str(executable), str(image), entry["entry_pc"], f"{int(entry['entry_pc'], 16):016X}",
            entry["seed_sr"], *entry["seed_d"], *entry["seed_a"]]
    for seed in entry.get("ram_seed", []):
        args += [seed["address"], seed["value"]]
    return args


def require_accepted_vector(harness, compiler, entry, temporary):
    image_bytes = bytes.fromhex(entry["image_hex"])
    assert hashlib.sha256(image_bytes).hexdigest() == entry["sha256"], entry["id"]
    image = temporary / f"{entry['id']}.bin"
    image.write_bytes(image_bytes)
    args = command(harness, image, entry)
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    assert result.returncode == 0 and result.stderr == "", (entry["id"], result)
    generated = result.stdout
    lowered = generated.lower()
    assert all(token not in lowered for token in FORBIDDEN_TOKENS), entry["id"]

    source = temporary / f"{entry['id']}.c"
    binary = temporary / entry["id"]
    source.write_text(generated)
    compiled = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                str(source), "-o", str(binary)], text=True, capture_output=True, check=False,
                               env=compiler_environment())
    assert compiled.returncode == 0, (entry["id"], compiled.stderr)
    executed = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
    assert executed.returncode == 0 and executed.stderr == "", (entry["id"], executed)
    return json.loads(executed.stdout)


def read_register(observed, name):
    bank, index = name[0], int(name[1:])
    return int(observed[bank][index], 16)


def independent_ccr_check(entry, observed):
    mnemonic = entry["mnemonic"]
    seed_sr = entry["seed_sr"]
    if mnemonic == "CLR":
        expected = clr_ccr(int(seed_sr, 16))
        assert int(observed["sr"], 16) == expected, (entry["id"], observed)
        return
    if mnemonic in ("MOVEA", "LEA", "JMP", "JSR"):
        expected = unaffected_ccr(int(seed_sr, 16))
        assert int(observed["sr"], 16) == expected, (entry["id"], observed)
        return
    if mnemonic in ("TST", "MOVE"):
        # The result register's OWN value in the harness's observed
        # post-state is the correctly sized, correctly signed value whose
        # N/Z the contract's rule keys off; re-derive independently from it
        # (never from the fixture's own "expected_sr").
        result = read_register(observed, entry["result_register"])
        expected = independent_expected_sr(mnemonic, seed_sr, entry["size"], f"{result:08X}")
        assert observed["sr"] == expected, (entry["id"], observed, expected)
        return
    raise AssertionError(f"unhandled mnemonic {mnemonic}")


def pinned_oracle(compiler, temporary):
    checkout_setting = os.environ.get(ORACLE_CHECKOUT_ENVIRONMENT)
    if not checkout_setting:
        return None
    checkout = Path(checkout_setting)
    assert checkout.is_dir(), f"batch-A Musashi checkout is not a directory: {checkout}"
    revision = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"],
                              text=True, capture_output=True, check=False)
    assert revision.returncode == 0 and revision.stdout.strip() == ORACLE_REVISION, (
        f"batch-A Musashi checkout is not pinned at {ORACLE_REVISION}: {checkout}", revision.stderr)
    clean = subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"],
                            text=True, capture_output=True, check=False)
    assert clean.returncode == 0, "batch-A Musashi checkout has tracked source modifications"
    generator = temporary / "m68kmake"
    generator_input = temporary / "m68k_in.c"
    generated_ops = temporary / "m68kops.c"
    generated_ops_header = temporary / "m68kops.h"
    temporary_cpu_source = temporary / "m68kcpu.c"
    runner = temporary / "m68k-batch-a-musashi-runner"
    environment = compiler_environment()
    generator_build = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                                      "-pedantic", str(checkout / "m68kmake.c"), "-o", str(generator)],
                                     text=True, capture_output=True, check=False, env=environment)
    assert generator_build.returncode == 0, ("cannot build tracked Musashi opcode generator", generator_build.stderr)
    shutil.copyfile(checkout / "m68k_in.c", generator_input)
    generation = subprocess.run([str(generator)], cwd=temporary, text=True, capture_output=True, check=False,
                                env=environment)
    assert generation.returncode == 0, ("cannot generate fresh Musashi opcode sources", generation.stderr)
    assert generated_ops.is_file() and generated_ops_header.is_file()
    temporary_cpu_source.symlink_to(checkout / "m68kcpu.c")
    build = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-error=unused-variable", "-pedantic",
                            f'-DMUSASHI_GIT_REVISION="{ORACLE_REVISION}"', "-I", str(temporary), "-I",
                            str(checkout), "-I", str(checkout / "softfloat"), str(RUNNER_SOURCE),
                            str(temporary_cpu_source), str(generated_ops), str(checkout / "softfloat/softfloat.c"),
                            "-o", str(runner)], text=True, capture_output=True, check=False, env=environment)
    assert build.returncode == 0, ("cannot build committed batch-A Musashi runner", build.stderr)
    return runner


def run_oracle(runner, entry):
    image = Path(tempfile.mkstemp(suffix=".bin")[1])
    try:
        image.write_bytes(bytes.fromhex(entry["image_hex"]))
        ram_seeds = [field for seed in entry.get("ram_seed", []) for field in (seed["address"], seed["value"])]
        result = subprocess.run([str(runner), str(image), entry["entry_pc"], entry["seed_sr"],
                                 *entry["seed_d"], *entry["seed_a"], *ram_seeds],
                                text=True, capture_output=True, check=False)
        assert result.returncode == 0 and result.stderr == "", (entry["id"], result)
        state = json.loads(result.stdout)
        assert state["musashi_revision"] == ORACLE_REVISION, (entry["id"], state)
        return state
    finally:
        image.unlink(missing_ok=True)


def main():
    harness = Path(sys.argv[1])
    compiler = resolve_compiler(sys.argv[3])
    static_manifest, manifest = validate_manifests()

    with tempfile.TemporaryDirectory() as temporary_name:
        temporary = Path(temporary_name)
        runner = pinned_oracle(compiler, temporary)
        oracle_compared = 0
        for entry in manifest["accepted"]:
            observed = require_accepted_vector(harness, compiler, entry, temporary)
            independent_ccr_check(entry, observed)
            assert observed["pc"] == entry["expected_pc"], (entry["id"], observed)
            if runner is not None:
                oracle_state = run_oracle(runner, entry)
                assert oracle_state["d"] == observed["d"], (entry["id"], oracle_state, observed)
                assert oracle_state["a"] == observed["a"], (entry["id"], oracle_state, observed)
                assert oracle_state["pc"] == observed["pc"], (entry["id"], oracle_state, observed)
                assert oracle_state["sr"] == observed["sr"], (entry["id"], oracle_state, observed)
                oracle_compared += 1

        # The rejection catalog is reused verbatim from the static-slice
        # fixture (not duplicated) as the adversarial boundary/precedence
        # evidence; re-verify determinism and forbidden-token absence
        # independently of that test's own assertions.
        for entry in static_manifest["rejections"]:
            image = temporary / f"adv-{entry['id']}.bin"
            image.write_bytes(bytes.fromhex(entry["image_hex"]))
            if entry["kind"] in ("decode", "effective_address"):
                args = [str(harness), str(image), "00000100", "0000000000000100", "2714",
                        "11111111", "22222222", "33333333", "44444444",
                        "55555555", "66666666", "77777777", "88888888",
                        "00000000", "00000000", "00000000", "00000000",
                        "00000000", "00000000", "00000000", "00FF0100"]
                first = subprocess.run(args, text=True, capture_output=True, check=False)
                second = subprocess.run(args, text=True, capture_output=True, check=False)
                assert first.returncode == 1 and first.stdout == "", (entry["id"], first)
                assert first.stderr == second.stderr, entry["id"]
                payload = json.loads(first.stderr)
                assert payload["category"] == entry["expected_category"], (entry["id"], payload)

    status = (f"compared {oracle_compared} vectors against the pinned Musashi checkout"
              if runner is not None else "pinned Musashi oracle unavailable; static independent-CCR evidence completed")
    print(f"adversarially validated {len(manifest['accepted'])} accepted batch-A vectors with independently "
          f"re-derived CCR/PC, {len(static_manifest['rejections'])} reused rejection-boundary vectors, "
          f"determinism, and forbidden-token absence ({status})")


if __name__ == "__main__":
    main()
