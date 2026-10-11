#!/usr/bin/env python3
"""SEG-049-T004 (ADR 0103): model / schema / policy mismatch handling of the embedded gen2 producer.

Compiles the gen2 evaluator translation unit against MUTATED copies of the generated table (one deliberate change each: a weight, the model digest, the token-schema
digest, the representation digest, the rank-plan digest, the policy percent, the weight-table digest, a BatchNorm statistic) and requires the fail-closed identity
reason `model_identity`; the unmutated copy must verify. Usage: <test.py> <c++ compiler> <source-root>.
"""
import pathlib
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

cxx, root = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
machine = root / "platforms" / "genesis" / "machine"
includes = [machine / "include", root / "libs/recompiler/include", root / "libs/core/include", root / "libs/cpu/m68k/include",
            root / "libs/device/sega/genesis/include", root / "libs/media/include"]
DRIVER = r'''
#include <cstdio>
#include <span>
#include <vector>
#include "segarecomp/machine/genesis/ml_region_gen2_producer.hpp"
namespace segarecomp { std::vector<std::uint8_t> genesis_decode_tokens(std::span<const std::uint8_t>) { return {}; } }
int main() { const auto f = segarecomp::genesis_ml2_model_identity_failure(); std::printf("[%s]\n", f.c_str()); return 0; }
'''
inc = (machine / "src" / "ml_region_gen2_model_data.inc").read_text()


def replace_first(text, pattern, repl):
    new, count = re.subn(pattern, repl, text, count=1)
    assert count == 1 and new != text, pattern
    return new


MUTATIONS = {
    "unmutated": (lambda t: t, ""),
    "weight": (lambda t: replace_first(t, r"(g2_emb_hi_weight\[2048\] = \{\n    )0\.33540815f", r"\g<1>0.4f"), "model_identity"),
    "batchnorm_statistic": (lambda t: replace_first(t, r"(g2_bn4_running_var\[64\] = \{\n    )([0-9.e+-]+)f", r"\g<1>9.0f"), "model_identity"),
    "model_digest": (lambda t: replace_first(t, r'(g2_source_model_sha256 = ")1', r"\g<1>2"), "model_identity"),
    "token_schema": (lambda t: replace_first(t, r'(g2_token_schema_sha256 = ")9', r"\g<1>8"), "model_identity"),
    "representation": (lambda t: replace_first(t, r'(g2_representation_sha256 = ")c', r"\g<1>d"), "model_identity"),
    "rank_plan": (lambda t: replace_first(t, r'(g2_rank_plan_sha256 = ")8', r"\g<1>9"), "model_identity"),
    "policy_percent": (lambda t: replace_first(t, r"g2_policy_percent = 35", "g2_policy_percent = 40"), "model_identity"),
    "weight_table_digest": (lambda t: replace_first(t, r'(g2_weight_table_sha256 = ")7', r"\g<1>6"), "model_identity"),
}


def run(name, tmp):
    mutate, want = MUTATIONS[name]
    work = pathlib.Path(tmp) / name
    work.mkdir()
    (work / "ml_region_gen2_producer.cpp").write_text((machine / "src" / "ml_region_gen2_producer.cpp").read_text())
    (work / "ml_region_gen2_model_data.inc").write_text(mutate(inc))
    (work / "driver.cpp").write_text(DRIVER)
    exe = work / ("driver.exe" if sys.platform == "win32" else "driver")
    cmd = [cxx, "-std=c++20", "-O0", "-ffp-contract=off", *[f"-I{p}" for p in includes], str(work / "driver.cpp"), str(work / "ml_region_gen2_producer.cpp"),
           str(root / "libs/media/src/sha256.cpp"), "-o", str(exe)]
    built = subprocess.run(cmd, capture_output=True, text=True)
    if built.returncode != 0:
        return name, f"compile failed: {built.stderr[-500:]}"
    out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.strip()
    return name, None if out == f"[{want}]" else f"expected [{want}] got {out}"


with tempfile.TemporaryDirectory(prefix="gen2-identity-") as tmp, ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(lambda n: run(n, tmp), MUTATIONS))
bad = [f"{name}: {message}" for name, message in results if message]
if bad:
    print("\n".join(bad), file=sys.stderr)
    sys.exit(1)
print("genesis_ml_gen2_identity_mutation_test: ok")
