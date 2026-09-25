#!/usr/bin/env python3
"""SEG-022-T008: sharded immutable-ROM AOT entries are grouped into bounded owners without changing
which guest PCs are enterable. Project-authored synthetic ROM; strict C11, -Werror."""
import pathlib
import re
import subprocess
import sys
import tempfile

BASE = 0x1008


def run(*args, **kw):
    done = subprocess.run(list(map(str, args)), text=True, capture_output=True, **kw)
    assert done.returncode == 0, (args, done.stderr)
    return done


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "bridge_generated.h"
GenesisControlTransfer genesis_bridge_dispatch(GenesisRuntime *runtime);
extern int generated_main(int argc, char **argv);
static const uint32_t represented[] = { %(represented)s };
static const uint32_t unrepresented[] = { %(unrepresented)s };
static const uint32_t probes[] = { %(probes)s };
static int inconsistent(GenesisControlTransfer t) {
  return t.kind == GENESIS_STOP && t.stop.stop_class == GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY;
}
static GenesisRuntime fresh(uint32_t pc) {
  GenesisRuntime r = {0};
  r.pc = pc; r.a[5] = UINT32_C(0x00FF0070); r.a[7] = UINT32_C(0x00FF0100); r.sr = UINT16_C(0x2700);
  return r;
}
int main(void) {
  size_t i;
  /* Every represented PC is enterable, alone; every unrepresented one (in or out of an owner span) is not. */
  for (i = 0; i < sizeof represented / sizeof represented[0]; ++i) {
    GenesisRuntime r = fresh(represented[i]);
    assert(genesis_compiled_entry_lookup(represented[i]) != NULL);
    assert(!inconsistent(genesis_bridge_dispatch(&r)));
  }
  for (i = 0; i < sizeof unrepresented / sizeof unrepresented[0]; ++i) {
    GenesisRuntime r = fresh(unrepresented[i]);
    assert(genesis_compiled_entry_lookup(unrepresented[i]) == NULL);
    assert(inconsistent(genesis_bridge_dispatch(&r)));
    assert(r.pc == unrepresented[i]);
  }
  /* First, middle, boundary and last represented PCs of each owner retire exactly one instruction. */
  for (i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
    GenesisRuntime r = fresh(probes[i]);
    GenesisControlTransfer t = genesis_bridge_dispatch(&r);
    assert(!inconsistent(t));
    assert(t.kind == GENESIS_STOP || r.pc != probes[i]);
  }
  /* Routed memory failure before commit: NEG.B (0,A5) with A5 unmapped changes nothing. */
  {
    GenesisRuntime r = fresh(UINT32_C(0x00001008));
    GenesisControlTransfer t;
    r.a[5] = UINT32_C(0x00500000);
    t = genesis_bridge_dispatch(&r);
    assert(t.kind == GENESIS_STOP && !inconsistent(t));
    assert(r.pc == UINT32_C(0x00001008) && r.a[5] == UINT32_C(0x00500000) && r.sr == UINT16_C(0x2700));
  }
  /* Dynamic-target membership (the one authority every indirect JMP/JSR site consults): represented
     targets inside owners are members, unrepresented aligned/odd PCs inside an owner span are not. */
  assert(genesis_compiled_entry_lookup(UINT32_C(0x00001108)) != NULL);
  assert(genesis_compiled_entry_lookup(UINT32_C(0x000010D0)) == NULL);
  assert(genesis_compiled_entry_lookup(UINT32_C(0x000010D1)) == NULL);
  /* Budgeted execution crossing an owner boundary stops resumably and resumes to the same result. */
  {
    GenesisRuntime whole = fresh(UINT32_C(0x000010FC)), split = fresh(UINT32_C(0x000010FC));
    GenesisControlTransfer a, b;
    a = genesis_runtime_run(&whole, genesis_bridge_dispatch, 8U);
    b = genesis_runtime_run(&split, genesis_bridge_dispatch, 3U);
    b = genesis_runtime_run(&split, genesis_bridge_dispatch, 5U);
    assert(a.kind == b.kind && whole.pc == split.pc && whole.sr == split.sr && whole.d[0] == split.d[0] && whole.a[5] == split.a[5]);
    assert(whole.pc > UINT32_C(0x00001108));
  }
  puts("aot owner harness OK");
  return 0;
}
'''


def main():
    pipeline, compiler, root = sys.argv[1:4]
    root = pathlib.Path(root)
    runtime = root / "platforms/genesis/runtime"
    single = run(pipeline, "--emit-aot-owner-single").stdout
    baseline = sorted(int(a, 16) for a in re.findall(r"static GenesisControlTransfer genesis_aot_([0-9A-F]{8})\(GenesisRuntime", single))
    assert len(baseline) > 256, len(baseline)
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="aot-owner-") as tmp:
        tmp = pathlib.Path(tmp)
        shards = []
        for name in ("a", "b"):
            run(pipeline, "--emit-aot-owner-shards", tmp / name)
            shards.append({p.name: p.read_bytes() for p in (tmp / name).iterdir()})
        assert shards[0] == shards[1], "owner grouping must be deterministic"
        out = tmp / "a"
        units = (out / "bridge_generated.units").read_text().split()
        aot_text = "".join((out / n).read_text() for n in units if "_aot_" in n)
        owners = re.findall(r"^static GenesisControlTransfer (genesis_aot_owner_\d+)\(", aot_text, re.M) or \
            re.findall(r"^GenesisControlTransfer (genesis_aot_owner_\d+)\(", aot_text, re.M)
        assert len(owners) >= 2 and len(owners) < len(baseline) // 32, owners
        assert "static GenesisControlTransfer genesis_aot_0" not in aot_text
        # An owner lives in exactly one TU, and every owner is a bounded switch with a fail-closed default.
        for owner in owners:
            assert sum(owner + "(GenesisRuntime *runtime) {" in (out / n).read_text() for n in units) == 1, owner
        cases = sorted(int(a, 16) for a in re.findall(r"case UINT32_C\(0x([0-9A-F]{8})\): goto genesis_aot_entry_", aot_text))
        assert cases == baseline, "admitted AOT PC set must equal the pre-owner baseline exactly"
        assert aot_text.count("default: return genesis_internal_dispatch_inconsistency_stop(runtime);") == len(owners)
        table = re.findall(r"\{ UINT32_C\(0x([0-9A-F]{8})\), (\w+) \}", "".join((out / n).read_text() for n in units if "_entries_" in n))
        compiled = sorted(int(a, 16) for a, _ in table)
        assert set(baseline) <= set(compiled), "final compiled-address set keeps every AOT PC"
        assert {int(a, 16) for a, b in table if b.startswith("genesis_aot_owner_")} == set(baseline)

        aot_pcs = set(baseline)
        lo, hi = min(baseline), max(baseline)
        gaps = [pc for pc in range(lo, hi, 2) if pc not in aot_pcs and pc not in compiled]
        assert 0x10D0 in gaps
        per_owner = 128
        probes = sorted({baseline[0], baseline[len(baseline) // 2], baseline[per_owner - 1], baseline[per_owner],
                         baseline[2 * per_owner - 1], baseline[2 * per_owner], baseline[-1]})
        fmt = lambda values: ", ".join("UINT32_C(0x%08X)" % v for v in values) or "UINT32_C(0)"
        sample = baseline[:: max(1, len(baseline) // 64)] + [baseline[-1]]
        (tmp / "harness.c").write_text(HARNESS % {"represented": fmt(sample), "unrepresented": fmt([0x10D0, 0x0FF0, hi + 2, 0x1009]),
                                                  "probes": fmt(probes)})
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O0", "-I", str(runtime), "-I", str(out)]
        objects = []
        for index, source in enumerate([out / n for n in units] + [tmp / "harness.c", runtime / "runtime.c"]):
            obj = tmp / f"o{index}.o"
            extra = ["-Dmain=generated_main"] if source.name == "bridge_generated_main.c" else []
            run(compiler, *flags, *extra, "-c", "-o", obj, source)
            objects.append(obj)
        run(compiler, "-o", tmp / "harness", *objects)
        ran = run(tmp / "harness")
        assert "aot owner harness OK" in ran.stdout
    print("genesis_immutable_rom_aot_owner_generated_test: OK")


if __name__ == "__main__":
    main()
