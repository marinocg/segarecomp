#!/usr/bin/env python3
"""SEG-074-T002 / ADR 0158: the opt-in generated-C compaction switches are observably identical to the reference.

The project-authored fixture (tests/m68k_pipeline_test.cpp `compact_fixture`) is emitted with every combination of
`--aot-operand-sharing` (operand-parameterized AOT helpers) and `--shared-return-targets` (one generated return-target
membership function) in the sharded (direct-entry) and single-file forms. Each build runs the SEG-022-T011 differential
C harness: every compiled entry is entered under deterministic architectural scenarios (mapped, unmapped and odd address
registers, supervisor and user mode, handlers present or absent) and the complete observable result is printed (control
transfer, typed stop class/diagnostic, full provenance, registers, SR/USP, a work-RAM digest, the checkpoint digest and a
whole-runtime digest); then the same after a bounded run. Three extra scenarios put a VALID return target (first and
last member of the return-target set), and an INVALID one, on top of the stack, so every RTS is entered with a matching
and a mismatching popped value. All outputs must equal the reference. Structural checks pin the intended shape (operand
helpers exist, are called with exactly the constant they replaced, and never switch on guest code; the return-target
table is the reference set, strictly ascending, and no inline chain remains). Mutants of the generated text (a wrong
operand argument, a dropped table member, an accept-all membership function) must be detected. Strict C11, -Werror,
and an -O2 rebuild of the combined form."""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import genesis_immutable_rom_aot_body_factoring_differential_test as base  # noqa: E402

run = base.run

EXTRA_SEED = r'''
  if (scenario >= 5U) {
    static const uint32_t returns[3] = { %(ret_first)s, %(ret_first)s + 2U, %(ret_last)s };
    const uint32_t value = returns[scenario - 5U];
    const size_t at = (size_t)(r->a[7] & UINT32_C(0x00FFFFFF)) - UINT32_C(0x00FF0000);
    r->work_ram[at] = (uint8_t)(value >> 24); r->work_ram[at + 1U] = (uint8_t)(value >> 16);
    r->work_ram[at + 2U] = (uint8_t)(value >> 8); r->work_ram[at + 3U] = (uint8_t)value;
  }
'''
HARNESS = base.HARNESS.replace("scenario < 5U", "scenario < 8U").replace(
    "  if (scenario >= 3U) {\n    r->privilege_violation_handler_present", EXTRA_SEED + "  if (scenario >= 3U) {\n    r->privilege_violation_handler_present", 1)
assert "scenario < 8U" in HARNESS and "returns[3]" in HARNESS


def build_and_run(compiler, runtime, sources, include_dir, header, entries, tmp, tag, rets, opt="-O0"):
    (tmp / f"{tag}_harness.c").write_text(HARNESS % {
        "header": header, "entries": ", ".join("UINT32_C(0x%08X)" % a for a in entries),
        "ret_first": "UINT32_C(0x%08X)" % rets[0], "ret_last": "UINT32_C(0x%08X)" % rets[-1]})
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", opt, "-I", str(runtime), "-I", str(include_dir)]
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


def read_shard(shard):
    units = (shard / "bridge_generated.units").read_text().split()
    text = "".join((shard / n).read_text() for n in units) + (shard / "bridge_generated.h").read_text()
    return units, text


def chain_targets(text):
    return sorted({int(v, 16) for v in re.findall(r"m68k_observed_return != UINT32_C\(0x([0-9A-Fa-f]{8})\)", text)})


def table_targets(text):
    body = re.search(r"genesis_return_targets\[\] = \{\n(.*?)\n\};", text, re.S)
    assert body, "return-target table"
    return [int(v, 16) for v in re.findall(r"0x([0-9A-F]{8})u", body.group(1))]


def main():
    pipeline, compiler, root = sys.argv[1:4]
    root = pathlib.Path(root)
    runtime = root / "platforms/genesis/runtime"
    build_dir = root / "build"
    build_dir.mkdir(exist_ok=True)
    forms = ("compact-ref", "compact-operand", "compact-returns", "compact-both")
    with tempfile.TemporaryDirectory(dir=build_dir, prefix="compaction-") as tmp:
        tmp = pathlib.Path(tmp)
        texts, singles, entries_of, outputs = {}, {}, {}, {}
        # Reference first: the return-target set is read from its inline chains.
        for form in forms:
            for copy in ("a", "b"):
                run(pipeline, "--emit-compact-fixture", form, tmp / f"{form}_{copy}")
            a = {p.name: p.read_bytes() for p in (tmp / f"{form}_a").iterdir()}
            b = {p.name: p.read_bytes() for p in (tmp / f"{form}_b").iterdir()}
            assert a == b, f"{form}: sharded emission must be deterministic"
            units, texts[form] = read_shard(tmp / f"{form}_a")
            entries_of[form] = base.entry_addresses("".join((tmp / f"{form}_a" / n).read_text() for n in units if "_entries_" in n))
            singles[form] = run(pipeline, "--emit-compact-fixture", form).stdout
            assert singles[form] == run(pipeline, "--emit-compact-fixture", form).stdout, f"{form}: single-file emission deterministic"
        ref_targets = chain_targets(texts["compact-ref"])
        assert len(ref_targets) >= 3, ref_targets
        rets = ref_targets
        for form in forms:
            assert entries_of[form] == entries_of["compact-ref"], f"{form}: the compiled-address set is unchanged"
        assert len(entries_of["compact-ref"]) > 150, len(entries_of["compact-ref"])
        for form in forms:
            shard = tmp / f"{form}_a"
            units = (shard / "bridge_generated.units").read_text().split()
            outputs[(form, "sharded")] = build_and_run(compiler, runtime, [shard / n for n in units], shard,
                                                       "bridge_generated.h", entries_of[form], tmp, f"{form}_sh", rets)
            single_path = tmp / f"{form}_single.c"
            single_path.write_text(singles[form])
            outputs[(form, "single")] = build_and_run(compiler, runtime, [single_path], tmp, "runtime.h",
                                                      entries_of[form], tmp, f"{form}_single", rets)
        reference = outputs[("compact-ref", "sharded")]
        for key, out in outputs.items():
            if out != reference:
                for left, right in zip(reference.splitlines(), out.splitlines()):
                    assert left == right, f"{key} diverges from the reference:\n  {left}\n  {right}"
                raise AssertionError(f"{key} output length differs from the reference")
        # The comparison is not vacuous.
        assert re.search(r"^step .* kind=0 ", reference, re.M), "successful continuations are compared"
        assert re.search(r"^step pc=\w+ s=5 kind=0 ", reference, re.M), "an RTS with a VALID popped return continues"
        assert re.search(r"^step pc=\w+ s=6 kind=1 ", reference, re.M), "an RTS with an INVALID popped return stops"
        # Structural: reference is the unchanged historical form.
        ref = texts["compact-ref"]
        assert "genesis_aot_operand" not in ref and "genesis_return_target_member" not in ref
        assert re.search(r"m68k_observed_return != UINT32_C", ref)

        # Optimization A.
        for form in ("compact-operand", "compact-both"):
            text, single = texts[form], singles[form]
            helpers = re.findall(r"GenesisControlTransfer (genesis_aot_shared_\d{5})\(GenesisRuntime \*runtime[^)]*"
                                 r"uint(8|16|32)_t genesis_aot_operand\) \{\n(.*?)\n\}\n", single, re.S)
            assert len(helpers) >= 25, f"{form}: operand-parameterized helpers are exercised ({len(helpers)})"
            seen_widths = {w for _, w, _ in helpers}
            # Immediates and absolute/displacement operands are emitted as 32-bit constants; 8/16-bit constants are mostly
            # structural masks, so narrower holes are rare in a small fixture and are exercised by the gate-image builds.
            assert "32" in seen_widths and seen_widths <= {"8", "16", "32"}, f"{form}: operand widths {seen_widths}"
            for name, width, body in helpers:
                assert "switch" not in body and "opcode" not in body and "case " not in body, name
                assert body.count("genesis_aot_operand") >= 1
                calls = re.findall(name + r"\(runtime[^\n]*?, (UINT%s_C\((?:0x[0-9A-Fa-f]+|\d+)\))\); \}$" % width, single, re.M)
                assert len(calls) >= 2, f"{name}: shared by at least two entries with an explicit constant argument"
            # a direct entry has no operand; an operand helper is reached only through its owner case.
            for name, _, _ in helpers:
                assert not re.search(name + r"\b[^;\n]*genesis_aot_direct", text), name
            assert len(text) < len(texts["compact-ref"]) and len(single) < len(singles["compact-ref"]), \
                f"{form}: generated C shrinks"
        for form in ("compact-ref", "compact-returns"):
            assert "genesis_aot_operand" not in texts[form] and "genesis_aot_operand" not in singles[form]

        # Optimization B.
        for form in ("compact-returns", "compact-both"):
            for text in (texts[form], singles[form]):
                assert "m68k_observed_return != UINT32_C" not in text, f"{form}: no inline return-target chain remains"
                assert "!genesis_return_target_member(m68k_observed_return)" in text
                table = table_targets(text)
                assert table == sorted(set(table)), f"{form}: the table is strictly ascending"
                assert table == rets, f"{form}: the shared table is exactly the reference return-target set"
            assert re.search(r"int genesis_return_target_member\(uint32_t value\);", texts[form]), "declared in the shared header"
        for form in ("compact-ref", "compact-operand"):
            assert "genesis_return_target_member" not in texts[form] and "genesis_return_target_member" not in singles[form]

        # -O2 rebuild of the combined sharded form (warnings are errors; behaviour must still match).
        shard = tmp / "compact-both_a"
        units = (shard / "bridge_generated.units").read_text().split()
        o2 = build_and_run(compiler, runtime, [shard / n for n in units], shard, "bridge_generated.h",
                           entries_of["compact-both"], tmp, "both_o2", rets, opt="-O2")
        assert o2 == reference, "the -O2 combined build is observably identical"

        # Mutants must be detected by the differential.
        def mutant_run(tag, form, transform, single=False):
            if single:
                target = tmp / f"mutant_{tag}_single.c"
                original = singles[form]
                text = transform(original)
                assert text != original, f"mutation {tag} changed nothing"
                target.write_text(text)
                out = build_and_run(compiler, runtime, [target], tmp, "runtime.h", entries_of[form], tmp, f"mutant_{tag}", rets)
            else:
                shard = tmp / f"{form}_a"
                target = tmp / f"mutant_{tag}"
                shutil.copytree(shard, target)
                changed = False
                for p in sorted(target.glob("*.c")):
                    original = p.read_text()
                    text = transform(original)
                    if text != original:
                        p.write_text(text)
                        changed = True
                        break
                assert changed, f"mutation {tag} changed nothing"
                names = (target / "bridge_generated.units").read_text().split()
                out = build_and_run(compiler, runtime, [target / n for n in names], target, "bridge_generated.h",
                                    entries_of[form], tmp, f"mutant_{tag}", rets)
            assert out != reference, f"mutant {tag} survived the differential"

        def wrong_operand(text):
            # Every operand-helper call site passes a constant whose last hex digit is flipped.
            return re.sub(r"(genesis_aot_shared_\d{5}\(runtime[^\n]*?, UINT(?:8|16|32)_C\(0x[0-9A-F]*)([0-9A-F])\)\); \}",
                          lambda m: m.group(1) + "0123456789ABCDEF"[(int(m.group(2), 16) ^ 1)] + ")); }", text)

        def drop_first_target(text):
            return re.sub(r"(genesis_return_targets\[\] = \{\n)0x[0-9A-F]{8}u,\s*", r"\1", text, count=1)

        def accept_all(text):
            return re.sub(r"(static int genesis_return_target_member\(uint32_t value\) \{\n)", r"\1  return 1;\n", text, count=1)

        for tag, transform in (("operand_arg", wrong_operand),):
            mutant_run(tag, "compact-operand", transform, single=True)
        mutant_run("operand_arg_sharded", "compact-operand", wrong_operand)
        mutant_run("drop_target", "compact-returns", drop_first_target, single=True)
        mutant_run("accept_all", "compact-returns", accept_all, single=True)
    print("genesis_generated_c_compaction_differential_test: OK")


if __name__ == "__main__":
    main()
