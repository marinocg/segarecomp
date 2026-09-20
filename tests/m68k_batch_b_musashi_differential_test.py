#!/usr/bin/env python3
"""Optional pinned-Musashi differential for synthetic B1/B2 vectors."""
import json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VECTORS = ROOT / "tests/fixtures/m68k-batch-b-musashi-vectors.json"
RUNNER_SOURCE = ROOT / "tests/tools/m68k_batch_b_musashi_runner.c"
PIN = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
CHECKOUT_ENV = "SEGARECOMP_M68K_BATCH_B_MUSASHI_CHECKOUT"

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
    assert checkout.is_dir(), f"B1 Musashi checkout is not a directory: {checkout}"
    assert checked(["git", "-C", str(checkout), "rev-parse", "HEAD"]).stdout.strip() == PIN, "B1 Musashi checkout is not pinned"
    assert subprocess.run(["git", "-C", str(checkout), "diff", "--quiet", "HEAD", "--"]).returncode == 0, "B1 Musashi checkout has tracked changes"
    for source in ("m68kmake.c", "m68k_in.c", "m68kcpu.c", "softfloat/softfloat.c"): assert (checkout / source).is_file(), f"B1 Musashi checkout lacks {source}"
    generator = temporary / "m68kmake"
    checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(checkout / "m68kmake.c"), "-o", str(generator)], env=environment())
    shutil.copyfile(checkout / "m68k_in.c", temporary / "m68k_in.c")
    checked([str(generator)], cwd=temporary, env=environment())
    assert (temporary / "m68kops.c").is_file() and (temporary / "m68kops.h").is_file()
    runner = temporary / "m68k-batch-b-musashi-runner"
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
            print("B1 pinned Musashi oracle unavailable; generated-C synthetic checks completed without differential claim"); return
        for entry in manifest["accepted"]:
            image = temporary / f"{entry['id']}.bin"; image.write_bytes(bytes.fromhex(entry["code_hex"])); args = [str(image), *arguments(entry)]
            source = temporary / f"{entry['id']}.c"; executable = temporary / entry["id"]; source.write_text(checked([harness, *args]).stdout)
            checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(executable)], env=environment())
            observed, oracle = json.loads(checked([str(executable)]).stdout), json.loads(checked([str(runner), str(image), "00000100", entry["seed_sr"], *entry["seed_d"], *entry["seed_a"], *(value for pair in entry["ram_seed"] for value in pair)]).stdout)
            assert oracle["musashi_revision"] == PIN
            for field in ("d", "a", "pc", "sr", "ram"): assert observed[field] == oracle[field], (entry["id"], field, observed, oracle)
            if entry.get("writes_memory", False): assert oracle["ram_writes"] > 0, (entry["id"], oracle)
            else: assert oracle["ram_writes"] == 0, (entry["id"], oracle)
            if entry["ram_seed"]: assert oracle["ram_reads"] > 0, (entry["id"], oracle)
    print(f"compared {len(manifest['accepted'])} synthetic B1/B2/B3/B4 vectors against pinned Musashi")
if __name__ == "__main__": main()
