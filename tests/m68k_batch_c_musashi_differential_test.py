#!/usr/bin/env python3
"""Optional pinned-Musashi differential for synthetic Batch-C (SEG-007-T025) C1+C2+C3 vectors."""
import json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VECTORS = ROOT / "tests/fixtures/m68k-batch-c-musashi-vectors.json"
RUNNER_SOURCE = ROOT / "tests/tools/m68k_batch_c_musashi_runner.c"
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_BATCH_C_MUSASHI_CHECKOUT"

def environment():
    result = os.environ.copy()
    if not result.get("SDKROOT") and shutil.which("xcrun"):
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True)
        if sdk.returncode == 0: result["SDKROOT"] = sdk.stdout.strip()
    return result
def checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (args, result.stderr)
    return result
def build_runner(compiler, temporary):
    setting = os.environ.get(CHECKOUT_ENV)
    if not setting: return None
    checkout = Path(setting)
    assert checkout.is_dir(), f"Batch-C Musashi checkout is not a directory: {checkout}"
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN, "Batch-C Musashi checkout is not pinned"
    assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0, "Batch-C Musashi checkout has tracked changes"
    for source in ("m68kmake.c", "m68k_in.c", "m68kcpu.c", "softfloat/softfloat.c"): assert (checkout / source).is_file(), f"Batch-C Musashi checkout lacks {source}"
    generator = temporary / "m68kmake"
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(checkout / "m68kmake.c"), "-o", str(generator)], env=environment())
    shutil.copyfile(checkout / "m68k_in.c", temporary / "m68k_in.c")
    checked([str(generator)], cwd=temporary, env=environment())
    assert (temporary / "m68kops.c").is_file() and (temporary / "m68kops.h").is_file()
    runner = temporary / "m68k-batch-c-musashi-runner"
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-variable", "-pedantic", f'-DMUSASHI_GIT_REVISION="{PIN}"', "-I", str(temporary), "-I", str(checkout), "-I", str(checkout / "softfloat"), str(RUNNER_SOURCE), str(checkout / "m68kcpu.c"), str(temporary / "m68kops.c"), str(checkout / "softfloat/softfloat.c"), "-o", str(runner)], env=environment())
    return runner
def arguments(entry): return ["00000100", entry["seed_sr"], *entry["seed_d"], *entry["seed_a"], "0", *(value for pair in entry["ram_seed"] for value in pair)]
def main():
    harness, compiler = sys.argv[1:]; manifest = json.loads(VECTORS.read_text())
    assert manifest["schema"] == 1 and manifest["musashi_revision"] == PIN
    assert "project-authored" in manifest["ownership"] and "commercial" in manifest["ownership"]
    with tempfile.TemporaryDirectory() as name:
        temporary = Path(name); runner = build_runner(compiler, temporary)
        if runner is None:
            print("Batch-C pinned Musashi oracle unavailable; generated-C synthetic checks completed without differential claim"); return
        for entry in manifest["accepted"]:
            image = temporary / f"{entry['id']}.bin"; image.write_bytes(bytes.fromhex(entry["code_hex"])); args = [str(image), *arguments(entry)]
            source = temporary / f"{entry['id']}.c"; executable = temporary / entry["id"]; source.write_text(checked([harness, *args]).stdout)
            checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(executable)], env=environment())
            observed, oracle = json.loads(checked([str(executable)]).stdout), json.loads(checked([str(runner), str(image), "00000100", entry["seed_sr"], *entry["seed_d"], *entry["seed_a"], *(value for pair in entry["ram_seed"] for value in pair)]).stdout)
            assert oracle["musashi_revision"] == PIN
            for field in ("d", "a", "pc", "sr", "ram"): assert observed[field] == oracle[field], (entry["id"], field, observed, oracle)
            # C1 (SWAP/EXT.W/EXT.L) kinds never touch memory at all. C2
            # (PEA/LINK push a real long; UNLK reads one) are the first
            # Batch-C kinds with a genuine memory access, so the write/read
            # assertion is keyed off each vector's own declared shape rather
            # than a blanket "no memory access" claim.
            if entry.get("writes_memory", False):
                assert oracle["ram_writes"] > 0, (entry["id"], oracle)
            else:
                assert oracle["ram_writes"] == 0, (entry["id"], oracle)
            # PEA/LINK push to their seeded stack slot without ever reading
            # it; only a genuine memory-source instruction (UNLK's pop, or a
            # future checkpoint's memory operand) is expected to register a
            # real read during the instruction's own execution (the oracle's
            # ram_reads counter is captured before its own end-of-run
            # diagnostic readback, so it reflects only the instruction itself).
            if entry["ram_seed"] and entry.get("reads_memory", True):
                assert oracle["ram_reads"] > 0, (entry["id"], oracle)
            # SEG-007-T025 (Batch C, C5c2 audit): MOVEM's data-transfer count
            # is register-mask-dependent (unlike every fixed-width C1-C4
            # form above), so the blanket >0/==0 checks above are too weak
            # to catch an extra phantom transfer or a missing one. Decode
            # each MOVEM vector's own primary/mask words independently here
            # (never trusting the generated-C/host implementation under
            # test) and assert the runner's own byte-granular
            # ram_writes/ram_reads counters exactly equal
            # popcount(mask) * width -- never merely nonzero -- and that the
            # OTHER direction's counter is exactly zero (register->memory
            # never reads memory; memory->register never writes it).
            # Instruction fetch is never counted here (the runner's
            # ram_reads/ram_writes counters are RAM-region data-access
            # counters only, never incremented for code fetch, since
            # read8/write8 route program-image bytes through a completely
            # separate, uncounted branch).
            if entry["id"].startswith("movem"):
                primary = int(entry["code_hex"][0:4], 16)
                mask = int(entry["code_hex"][4:8], 16)
                direction_memory_to_registers = ((primary >> 10) & 1) != 0
                width = 4 if ((primary >> 6) & 1) != 0 else 2
                expected_bytes = bin(mask).count("1") * width
                if direction_memory_to_registers:
                    assert oracle["ram_reads"] == expected_bytes and oracle["ram_writes"] == 0, (
                        entry["id"], expected_bytes, oracle,
                    )
                else:
                    assert oracle["ram_writes"] == expected_bytes and oracle["ram_reads"] == 0, (
                        entry["id"], expected_bytes, oracle,
                    )
            # SEG-007-T025 (Batch C, C7a): every memory-form shift/rotate is a
            # genuine one-address WORD read-modify-write, always exactly 2
            # read bytes and 2 write bytes -- never more (a phantom extra
            # access) and never fewer (a missing one), and never a
            # read-without-write or write-without-read (contract: "for every
            # RMW assert exactly one source WORD consumed... exactly one
            # destination WORD written").
            if entry["id"].startswith("shift-mem"):
                assert oracle["ram_reads"] == 2 and oracle["ram_writes"] == 2, (entry["id"], oracle)
    print(f"compared {len(manifest['accepted'])} synthetic Batch-C C1 (SWAP/EXT.W/EXT.L) + "
          f"C2 (PEA/LINK/UNLK) + C3 (BTST/BCHG/BCLR/BSET) + C4a (BRA/Bcc) + "
          f"C4b (BSR) + C4c (DBcc) + C5a (MOVEM ordinary forms + base-alias correction) + "
          f"C5b (MOVEM predecrement forms) + C5c1 (MOVEM postincrement forms) + "
          f"C6a (LSL/LSR register forms) + C6b (ASL/ASR register forms) + "
          f"C6c (ROL/ROR register forms) + C6d (ROXL/ROXR register forms) + "
          f"C7a (ASL/ASR/LSL/LSR memory RMW forms) + "
          f"C7b (ROL/ROR/ROXL/ROXR memory RMW forms) "
          f"vectors against pinned Musashi")
if __name__ == "__main__": main()
