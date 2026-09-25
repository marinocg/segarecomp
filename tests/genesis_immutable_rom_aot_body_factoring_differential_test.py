#!/usr/bin/env python3
"""SEG-022-T011 / ADR-0045: factored immutable-ROM AOT bodies are observably identical to the unfactored ones.

The project-authored differential fixture (tests/m68k_pipeline_test.cpp `aot_factoring_fixture`) is emitted
twice from the same analysis -- the factored default and the unfactored reference -- in both the sharded
and the single-file form. Each build runs one C harness that enters EVERY compiled AOT entry under several
deterministic architectural scenarios (mapped, unmapped and odd address registers so routed accesses
succeed and fail, supervisor and user mode, exception handlers present or absent) and prints the complete
observable result: control transfer, stop class/diagnostic, complete stop provenance (instruction,
access, mapping claims, bus accesses), every register, a work-RAM digest, the checkpoint digest and a digest
of the whole runtime; then the same after a bounded multi-step run. The outputs must be identical, the
factored form must actually share bodies and use the routed-failure helper, and the unfactored form must
contain neither. Strict C11, -Werror."""
import pathlib
import re
import subprocess
import sys
import tempfile


def run(*args, **kw):
    done = subprocess.run(list(map(str, args)), text=True, capture_output=True, **kw)
    assert done.returncode == 0, (args, done.returncode, done.stderr[-4000:])
    return done


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "%(header)s"
GenesisControlTransfer genesis_bridge_dispatch(GenesisRuntime *runtime);
static const uint32_t entries[] = { %(entries)s };
static GenesisRuntime runtime_storage;
static uint64_t fnv(const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  uint64_t hash = UINT64_C(1469598103934665603);
  size_t i;
  for (i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
  return hash;
}
static void seed(GenesisRuntime *r, uint32_t pc, unsigned scenario) {
  unsigned i;
  memset(r, 0, sizeof *r);
  r->pc = pc;
  r->sr = scenario == 3U ? UINT16_C(0x0000) : (scenario == 4U ? UINT16_C(0x2715) : UINT16_C(0x2700));
  for (i = 0; i < 8U; ++i) r->d[i] = (UINT32_C(0x01020304) * (i + 1U)) ^ (UINT32_C(0x11111111) * scenario);
  if (scenario == 4U) { r->d[0] = 0U; r->d[1] = 0U; }
  for (i = 0; i < 7U; ++i)
    r->a[i] = scenario == 1U ? UINT32_C(0x00500000) + 0x10U * i
            : scenario == 2U ? UINT32_C(0x00FF0101) + 0x100U * i
                             : UINT32_C(0x00FF0200) + 0x100U * i;
  r->a[7] = scenario == 3U ? UINT32_C(0x00FF6000) : UINT32_C(0x00FF8000);
  r->usp = scenario == 3U ? UINT32_C(0x00FF8000) : UINT32_C(0x00FF6000);
  for (i = 0; i < sizeof r->work_ram; ++i) r->work_ram[i] = (uint8_t)(i * 37U + scenario);
  if (scenario >= 3U) {
    r->privilege_violation_handler_present = 1U; r->privilege_violation_handler_entry = entries[0];
    r->divide_by_zero_handler_present = 1U; r->divide_by_zero_handler_entry = entries[1];
  }
}
static void dump(const char *tag, uint32_t pc, unsigned scenario, GenesisControlTransfer t, const GenesisRuntime *r) {
  const GenesisProvenance *p = &t.stop.provenance;
  unsigned i, j;
  printf("%%s pc=%%08X s=%%u kind=%%d next=%%08X runner=%%u", tag, pc, scenario, (int)t.kind, t.next_pc, t.runner_dispatch_count);
  if (t.kind == GENESIS_STOP) {
    printf(" stop=%%d diag=%%d c4=%%d hip=%%u cpu=%%d src=%%08X off=%%llu b=%%02X%%02X len=%%u acc=%%u",
           (int)t.stop.stop_class, (int)t.stop.diagnostic_category, (int)t.stop.c4_lowering_dimensions,
           p->has_instruction_provenance, (int)p->instruction.cpu_variant, p->instruction.source_address,
           (unsigned long long)p->instruction.image_offset, p->instruction.primary_bytes[0],
           p->instruction.primary_bytes[1], p->instruction.length, p->has_access);
    if (p->has_access) printf(" @%%08X w=%%d dir=%%d", p->access_address, (int)p->access_width, (int)p->access_direction);
    printf(" claims=%%u", p->mapping_claim_count);
    for (i = 0; i < p->mapping_claim_count && i < GENESIS_MAX_MAPPING_CLAIMS; ++i) {
      const GenesisMappingClaim *c = &p->mapping_claims[i];
      printf(" [%%.*s %%08X-%%08X %%llu-%%llu]", (int)c->name_length, c->name, c->target_begin, c->target_end,
             (unsigned long long)c->image_begin, (unsigned long long)c->image_end);
    }
    printf(" bus=%%u", p->bus_access_count);
    for (i = 0; i < p->bus_access_count && i < GENESIS_MAX_BUS_ACCESSES; ++i) {
      const GenesisBusAccess *b = &p->bus_accesses[i];
      printf(" [%%llu %%d %%08X %%d ", (unsigned long long)b->ordinal, (int)b->kind, b->address, (int)b->region);
      for (j = 0; j < b->raw_byte_count && j < GENESIS_MAX_RAW_BYTES; ++j) printf("%%02X", b->raw_bytes[j]);
      printf("]");
    }
  }
  printf(" | pc=%%08X sr=%%04X usp=%%08X", r->pc, r->sr, r->usp);
  for (i = 0; i < 8U; ++i) printf(" d%%u=%%08X", i, r->d[i]);
  for (i = 0; i < 8U; ++i) printf(" a%%u=%%08X", i, r->a[i]);
  printf(" ram=%%016llX ckpt=%%016llX all=%%016llX\n", (unsigned long long)fnv(r->work_ram, sizeof r->work_ram),
         (unsigned long long)genesis_m68k_checkpoint_digest(r), (unsigned long long)fnv(r, sizeof *r));
}
int main(void) {
  size_t e;
  unsigned scenario;
  for (e = 0; e < sizeof entries / sizeof entries[0]; ++e)
    for (scenario = 0; scenario < 5U; ++scenario) {
      GenesisRuntime *r = &runtime_storage;
      GenesisControlTransfer t;
      seed(r, entries[e], scenario);
      t = genesis_bridge_dispatch(r);
      dump("step", entries[e], scenario, t, r);
      seed(r, entries[e], scenario);
      t = genesis_runtime_run(r, genesis_bridge_dispatch, 12U);
      dump("run", entries[e], scenario, t, r);
    }
  puts("aot factoring harness done");
  return 0;
}
'''


def entry_addresses(text):
    body = re.search(r"\bgenesis_compiled_entry_addresses\[\] = \{\n(.*?)\n\};", text, re.S)
    assert body, "compiled-entry address table"
    return [int(a, 16) for a in re.findall(r"UINT32_C\(0x([0-9A-Fa-f]{8})\)", body.group(1))]


def build_and_run(compiler, runtime, sources, include_dir, header, entries, tmp, tag):
    (tmp / f"{tag}_harness.c").write_text(HARNESS % {
        "header": header, "entries": ", ".join("UINT32_C(0x%08X)" % a for a in entries)})
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O0", "-I", str(runtime), "-I", str(include_dir)]
    objects = []
    for index, source in enumerate(list(sources) + [tmp / f"{tag}_harness.c", runtime / "runtime.c"]):
        obj = tmp / f"{tag}_{index}.o"
        extra = ["-Dmain=generated_main"] if source.name == "bridge_generated_main.c" or source.name.endswith("_single.c") else []
        run(compiler, *flags, *extra, "-c", "-o", obj, source)
        objects.append(obj)
    run(compiler, "-o", tmp / f"{tag}_harness", *objects)
    out = run(tmp / f"{tag}_harness").stdout
    assert out.endswith("aot factoring harness done\n"), out[-2000:]
    return out


def main():
    pipeline, compiler, root = sys.argv[1:4]
    root = pathlib.Path(root)
    runtime = root / "platforms/genesis/runtime"
    build_dir = root / "build"
    build_dir.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=build_dir, prefix="aot-factoring-") as tmp:
        tmp = pathlib.Path(tmp)
        results = {}
        texts = {}
        for form in ("factored", "unfactored"):
            # Sharded form, emitted twice to prove determinism.
            for copy in ("a", "b"):
                run(pipeline, "--emit-aot-factoring", form, tmp / f"{form}_{copy}")
            a = {p.name: p.read_bytes() for p in (tmp / f"{form}_a").iterdir()}
            b = {p.name: p.read_bytes() for p in (tmp / f"{form}_b").iterdir()}
            assert a == b, f"{form} sharded emission must be deterministic"
            shard = tmp / f"{form}_a"
            units = (shard / "bridge_generated.units").read_text().split()
            sharded_text = "".join((shard / n).read_text() for n in units) + (shard / "bridge_generated.h").read_text()
            entries = entry_addresses("".join((shard / n).read_text() for n in units if "_entries_" in n))
            # Single-file form.
            single = run(pipeline, "--emit-aot-factoring", form).stdout
            single_path = tmp / f"{form}_single.c"
            single_path.write_text(single)
            assert entry_addresses(single) == entries, "single-file and sharded forms compile the same entries"
            texts[form] = (sharded_text, single)
            results[(form, "sharded")] = build_and_run(compiler, runtime, [shard / n for n in units], shard,
                                                       "bridge_generated.h", entries, tmp, f"{form}_sh")
            results[(form, "single")] = build_and_run(compiler, runtime, [single_path], tmp, "runtime.h",
                                                      entries, tmp, f"{form}_single")
            results[(form, "entries")] = entries

        assert results[("factored", "entries")] == results[("unfactored", "entries")], \
            "the final compiled-address set is unchanged by factoring"
        entries = results[("factored", "entries")]
        assert len(entries) > 100, len(entries)
        reference = results[("unfactored", "sharded")]
        for key in (("unfactored", "single"), ("factored", "sharded"), ("factored", "single")):
            if results[key] != reference:
                for left, right in zip(reference.splitlines(), results[key].splitlines()):
                    assert left == right, f"{key} diverges from the unfactored reference:\n  {left}\n  {right}"
                raise AssertionError(f"{key} output length differs from the unfactored reference")

        # The comparison is not vacuous: stops with instruction provenance, routed-access failures, successful
        # continuations and exception entries all occur, across many distinct source addresses.
        stops = re.findall(r"^step pc=(\w+) s=\d kind=1 .* src=(\w+) .* acc=1 @", reference, re.M)
        assert len({src for _, src in stops}) > 20, "routed-failure stops with access provenance across many sources"
        assert all(pc == src for pc, src in stops), "a stop names the entered instruction as its own source"
        assert re.search(r"^step .* kind=0 ", reference, re.M), "successful continuations are compared"
        assert re.search(r"^run .* runner=12 ", reference, re.M) or re.search(r"^run .* kind=3 ", reference, re.M), \
            "bounded multi-step runs are compared"

        factored_sharded, factored_single = texts["factored"]
        unfactored_sharded, unfactored_single = texts["unfactored"]
        for text in (factored_sharded, factored_single):
            assert re.search(r"GenesisControlTransfer genesis_aot_shared_\d{5}\(GenesisRuntime \*runtime", text), \
                "identical bodies share a statically selected helper"
            assert "return genesis_routed_failure_stop(&" in text, "routed failures use the shared failure tail"
        assert len(factored_sharded) < len(unfactored_sharded) and len(factored_single) < len(unfactored_single)
        for text in (unfactored_sharded, unfactored_single):
            assert "genesis_aot_shared_" not in text and "return genesis_routed_failure_stop(&" not in text
            assert "genesis_aot_source" not in text
        # No runtime opcode decode or generic instruction engine: a shared helper is a straight-line body with
        # the same statements an entry would have inlined; it never switches on guest code.
        helpers = re.findall(r"GenesisControlTransfer (genesis_aot_shared_\d{5})\(GenesisRuntime \*runtime[^)]*\) \{\n(.*?)\n\}\n",
                             factored_single, re.S)
        assert helpers, "helper definitions"
        for name, body in helpers:
            assert "switch" not in body and "opcode" not in body and "genesis_aot_shared_" not in body, name
            assert factored_single.count(name + "(runtime") >= 2, f"{name} is shared by at least two entries"
    print("genesis_immutable_rom_aot_body_factoring_differential_test: OK")


if __name__ == "__main__":
    main()
