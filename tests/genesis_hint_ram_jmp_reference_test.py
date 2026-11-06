#!/usr/bin/env python3
"""SEG-122-T011 (ADR 0272): independent whole-machine differential of H-interrupt delivery through a work-RAM `JMP (xxx).L` stub.

The project-authored fixture (tests/genesis_hint_ram_jmp_fixture.py) runs through
  * the generated-native program (the static thunk, live operand and level-4 delivery of this PR), and
  * the pinned Genesis Plus GX libretro core (tests/genesis_oracle/pins.py), driven by the unchanged tests/genesis_oracle/libretro_host.c.
Compared: handler counts and order, SR stacked on entry and seen inside the handler, SR after the final mask, phase timeouts, completion.
Never compared: the mainline spin count (timing) and audio. Controls (disabled, masked) must agree on "no interrupt"; `bad_opcode` and `bad_target`
are not comparable behaviours (the reference executes whatever follows) and only require that NO handler runs in either machine, with the generated
program refusing with its typed stop. Discrimination: a generated program with level-4 delivery removed (the old behaviour) must FAIL the same comparison.

The generated-native half always runs; the reference half skips cleanly unless SEGARECOMP_GENESIS_ORACLE_CHECKOUT holds the pinned checkout.
usage: genesis_hint_ram_jmp_reference_test.py <segarecomp cli> <cc> <repo root>
"""
import glob
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "genesis_oracle"))
sys.path.insert(0, str(HERE))
import pins  # noqa: E402
import genesis_hint_ram_jmp_fixture as fixture  # noqa: E402

NAMES = ["ekeeke_Genesis-Plus-GX"]
VARIANTS = ("normal", "disabled", "masked", "bad_opcode", "bad_target")
COMPARED = {"A": (0x00, 2), "B": (0x02, 2), "log": (0x0C, 4), "sr_stacked": (0x40, 2), "sr_handler": (0x42, 2), "sr_final": (0x44, 2),
            "timeout_a1": (0x52, 2), "timeout_b": (0x54, 2), "timeout_a2": (0x56, 2)}

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
GenesisControlTransfer harness_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t allowance);
#define genesis_runtime_run harness_run
#define main generated_bridge_main
#include "generated.c"
#undef main
#undef genesis_runtime_run
GenesisControlTransfer harness_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t allowance) {
  GenesisControlTransfer t = {0};
  uint64_t total = 0;
  (void)allowance;
  while (total < 20000000ULL) {
    t = genesis_runtime_run(runtime, dispatch, 20000U);
    total += 20000U;
    if (runtime->work_ram[0xFF] == 0xA5 || t.kind == GENESIS_STOP) break;
  }
  printf("{\"complete\":%s,\"stop\":%d,\"stop_class\":%d,\"results\":\"", runtime->work_ram[0xFF] == 0xA5 ? "true" : "false", t.kind == GENESIS_STOP, t.kind == GENESIS_STOP ? (int)t.stop.stop_class : -1);
  for (unsigned i = 0; i < 256; ++i) printf("%02X", runtime->work_ram[i]);
  printf("\"}\n");
  exit(0);
}
int main(void) { char *argv[] = {"generated", "--instruction-budget", "1000000", NULL}; return generated_bridge_main(3, argv); }
'''


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


def native(cli, cc, root, work, name, variant, mutate=None):
    rom = work / f"{name}.md"
    data = fixture.build(variant)
    rom.write_bytes(data)
    sha = hashlib.sha256(data).hexdigest()
    emitted = run([cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"], timeout=300)
    assert emitted.returncode == 0, emitted.stderr[:2000]
    # The static thunk is in the first emission, except when the construction stores no JMP opcode (then no static proof exists).
    assert ("genesis_aot_00FFFFF6(GenesisRuntime *runtime)" in emitted.stdout) == (variant != "bad_opcode"), variant
    text = emitted.stdout
    if mutate:
        text = mutate(text)
    directory = work / name
    directory.mkdir()
    (directory / "generated.c").write_text(text)
    (directory / "harness.c").write_text(HARNESS)
    runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"
    built = run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O1", "-I", str(runtime_dir), "-I", str(directory),
                 str(directory / "harness.c"), str(runtime_dir / "runtime.c"), "-o", str(directory / "run")])
    assert built.returncode == 0, built.stderr[:3000]
    done = run([str(directory / "run")], timeout=300)
    assert done.returncode == 0, done.stderr[:2000]
    return json.loads(done.stdout)


def fields(result):
    block = bytes.fromhex(result["results"])
    return {key: int.from_bytes(block[offset:offset + size], "big") for key, (offset, size) in COMPARED.items()}


def reference(host, core, work, variant):
    rom = work / f"ref_{variant}.md"
    rom.write_bytes(fixture.build(variant))
    done = run([host, core, str(rom), "1500", "genesis_plus_gx_system_hw=mega drive / genesis", "genesis_plus_gx_region_detect=ntsc-u", "genesis_plus_gx_bios=disabled"], timeout=300)
    assert done.returncode in (0, 4) and done.stdout.strip(), done.stderr
    return json.loads(done.stdout)


def build_core(root, tmp):
    tree = pins.private_copy(root, "ekeeke_Genesis-Plus-GX", tmp)
    subprocess.run(["make", "-j" + str(max(1, min(8, os.cpu_count() or 1))), "-f", "Makefile.libretro"], cwd=tree, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    found = [p for p in glob.glob(str(tree / "genesis_plus_gx_libretro.*")) if p.endswith((".so", ".dylib"))]
    assert len(found) == 1, "libretro core not produced"
    return found[0]


EXPECTED_NORMAL = {"A": 8, "B": 4, "log": 0x55AA55, "sr_stacked": 0x2000, "sr_handler": 0x2400, "sr_final": 0x2700, "timeout_a1": 0, "timeout_b": 0, "timeout_a2": 0}


def main():
    cli, cc, root = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary)
        got = {v: native(cli, cc, root, work, f"n_{v}", v) for v in VARIANTS}
        again = native(cli, cc, root, work, "n_normal2", "normal")
        assert fields(again) == fields(got["normal"]), "generated-native result is deterministic"
        # Expectations authored from the architecture (not from either machine): AAAA BBBB AAAA, SR entry/handler/final, no spurious timeouts.
        assert got["normal"]["complete"] and fields(got["normal"]) == EXPECTED_NORMAL, fields(got["normal"])
        for v in ("disabled", "masked"):
            f = fields(got[v])
            assert got[v]["complete"] and f["A"] == f["B"] == f["log"] == 0 and f["timeout_a1"] == f["timeout_b"] == f["timeout_a2"] == 1, (v, f)
        for v in ("bad_opcode", "bad_target"):
            # No handler may run. A stub that was never proven (no JMP constructed) has no handler entry, so the level-4 request is dropped by the existing
            # default policy and every wait times out; an unsupported live target takes the typed stop. Either is fail closed.
            f = fields(got[v])
            assert f["A"] == f["B"] == 0 and (got[v]["stop"] or (f["timeout_a1"], f["timeout_b"], f["timeout_a2"]) == (1, 1, 1)), (v, got[v])
        # Discrimination: removing level-4 delivery (the old behaviour) must be visible to the same comparison.
        mutated = native(cli, cc, root, work, "n_no_hint", "normal",
                         mutate=lambda t: t.replace("runtime.software_exception_handler_present[28] = 1;", "runtime.software_exception_handler_present[28] = 0;", 1))
        assert fields(mutated) != EXPECTED_NORMAL and fields(mutated)["A"] == 0, fields(mutated)
        print("generated-native half: ok (normal %s)" % json.dumps(fields(got["normal"])))
        checkout = pins.checkout(NAMES)
        if checkout is None:
            print("SKIP reference half: " + pins.skip_reason(NAMES))
            return 0
        host = work / "host"
        subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-o", str(host), str(HERE / "genesis_oracle" / "libretro_host.c"), "-ldl"], check=True)
        core = build_core(checkout, work)
        ref = {v: reference(str(host), core, work, v) for v in VARIANTS}
        assert reference(str(host), core, work, "normal") == ref["normal"], "reference is deterministic"
        failed = 0
        for v in ("normal", "disabled", "masked"):
            same = ref[v]["complete"] and fields(ref[v]) == fields(got[v])
            print("%-4s %-9s reference=%s native=%s" % ("ok" if same else "FAIL", v, json.dumps(fields(ref[v])), json.dumps(fields(got[v]))))
            failed += 0 if same else 1
        for v in ("bad_opcode", "bad_target"):
            f = fields(ref[v])
            ok = f["A"] == 0 and f["B"] == 0 and not ref[v]["complete"]
            print("%-4s %-9s reference ran no handler and did not complete; native stop=%s class=%s" % ("ok" if ok else "FAIL", v, got[v]["stop"], got[v]["stop_class"]))
            failed += 0 if ok else 1
        assert fields(mutated) != fields(ref["normal"]), "the comparison detects dropped level-4 delivery"
        print("independent reference differential: %s" % ("FAILED (%d)" % failed if failed else "ok"))
        return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
