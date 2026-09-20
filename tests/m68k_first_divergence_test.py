#!/usr/bin/env python3
"""SEG-020-T005: first-divergence diagnosis (generated M68k boundaries vs pinned Musashi).

Always runs: pure comparison semantics on project-authored synthetic streams, and the fixture
proof against real emitted C (the synthetic vector-5 divide-by-zero -> handler -> RTE program) with
test-only fault injection applied to *temporary copies* only. When the pinned Musashi checkout
(SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT) is available the real oracle stream is produced and
the unperturbed fixture must report no divergence while each injected fault must be reported at
its first differing boundary. Without the checkout the oracle-dependent claims are skipped.
"""
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("m68k_first_divergence", ROOT / "tools/m68k_first_divergence.py")
fd = importlib.util.module_from_spec(spec)
sys.modules["m68k_first_divergence"] = fd
spec.loader.exec_module(fd)


def rec(boundary, pc, effects=(), **over):
    r = {"boundary": boundary, "pc": pc, "sr": 0x2700, "usp": 0, "d": [0] * 8, "a": [0] * 7 + [0xFF8000],
         "unsupported": 0, "effects": [e if isinstance(e, dict) else dict(k=1, w=2, a=e[0], v=e[1]) for e in effects]}
    r.update(over)
    return r


def synthetic_semantics():
    base = [rec(1, 0x102, [(0xFF0100, 1)]), rec(2, 0x104), rec(3, 0x106)]
    same = fd.compare_streams(base, [dict(r) for r in base], 8, 0x100)
    assert same["result"] == "no_divergence" and same["domain"] == "none" and same["compared_boundaries"] == 3, same
    # Wrong memory write, registers identical.
    wrong = [rec(1, 0x102, [(0xFF0100, 2)])] + base[1:]
    r = fd.compare_streams(wrong, base, 8, 0x100)
    assert (r["last_matching_boundary"], r["first_differing_boundary"], r["pc"], r["domain"]) == (0, 1, 0x100, "cpu"), r
    assert [f["field"] for f in r["fields"]] == ["effect:write@00FF0100/w2"], r
    # Missing write and register-only differences.
    r = fd.compare_streams([rec(1, 0x102)] + base[1:], base, 8, 0x100)
    assert r["fields"][0]["generated"] is None, r
    late = [base[0], base[1], rec(3, 0x106, d=[0, 0, 0, 0, 0, 0, 0, 9] )]
    r = fd.compare_streams(late, base, 8, 0x100)
    assert (r["last_matching_boundary"], r["first_differing_boundary"], r["pc"]) == (2, 3, 0x104), r
    assert [f["field"] for f in r["fields"]] == ["d7"], r
    # Trap-only difference (vector) and write-order insensitivity within one boundary.
    trap = lambda vec: rec(1, 0x120, effects=[
        dict(k=1, w=2, a=0xFF7FFA, v=0x2700), dict(k=2, w=0, a=0x120, v=vec)])
    r = fd.compare_streams([trap(5)], [trap(30)], 4, 0x100)
    assert r["fields"][0]["field"] == "effect:trap", r
    a = rec(1, 0x120, effects=[dict(k=1, w=2, a=8, v=1), dict(k=1, w=4, a=10, v=2)])
    b = rec(1, 0x120, effects=[dict(k=1, w=4, a=10, v=2), dict(k=1, w=2, a=8, v=1)])
    assert fd.compare_streams([a], [b], 4, 0x100)["result"] == "no_divergence"
    # Unsupported is never equal; stream length mismatch; mandatory positive limit; determinism.
    r = fd.compare_streams([rec(1, 2, unsupported=1)], [rec(1, 2)], 4, 0x100)
    assert r["result"] == "unsupported_for_comparison" and r["first_differing_boundary"] == 1, r
    r = fd.compare_streams(base[:2], base, 8, 0x100)
    assert r["fields"][0]["field"] == "boundary_presence" and r["first_differing_boundary"] == 3, r
    assert fd.compare_streams(wrong, base, 1, 0x100) == fd.compare_streams(wrong, base, 1, 0x100)
    assert fd.render(r) == fd.render(fd.compare_streams(base[:2], base, 8, 0x100))
    try:
        fd.compare_streams(base, base, 0, 0x100)
        raise AssertionError("limit 0 accepted")
    except ValueError:
        pass
    # Limit bounds the compare: a divergence beyond the limit is not looked at.
    assert fd.compare_streams(late, base, 2, 0x100)["result"] == "no_divergence"
    # Generated detail-line wrapper parses.
    line = json.dumps({"m68k_checkpoint": base[0]})
    assert fd.parse_stream(line + "\n" + '{"m68k_checkpoint":null}\n') == [base[0]]


def checked(args, **kw):
    r = subprocess.run(args, text=True, capture_output=True, **kw)
    assert r.returncode == 0, (args, r.stderr)
    return r


def main():
    executable, compiler, root = sys.argv[1:]
    root = pathlib.Path(root)
    synthetic_semantics()

    generated = checked([executable, "--emit-general-startup-bridge-divide-by-zero-rte"]).stdout
    assert not generated.startswith("/* translation rejected:"), generated
    rom_match = re.search(r"genesis_owned_region_data_0\[\] = \{([^}]*)\}", generated)
    image = bytes(int(x, 16) for x in re.findall(r"UINT8_C\(0x([0-9A-Fa-f]{2})\)", rom_match.group(1)))
    runtime_c = (root / "platforms/genesis/runtime/runtime.c").read_text()

    # Production sources carry no injection hook: the perturbation anchors exist untouched and the
    # comparison tool exposes no injection surface.
    assert runtime_c.count("routed_value = saved_sr;") == 1
    assert "--inject" not in (root / "tools/m68k_first_divergence.py").read_text()
    assert "SEGARECOMP_FAULT" not in runtime_c and "fault_inject" not in generated

    wrapper = (
        '#include "runtime.h"\n'
        "static GenesisControlTransfer harness_retire(GenesisRuntime *r, uint32_t c, uint32_t n) {\n"
        "  GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r, c, n);\n"
        "  genesis_m68k_checkpoint_write_detail(stderr, r); return t; }\n"
        "static int harness_raise(GenesisRuntime *r, uint32_t f, uint32_t *h, GenesisRuntimeStop *s) {\n"
        "  int rc = genesis_raise_divide_by_zero(r, f, h, s);\n"
        "  if (rc == 1) genesis_m68k_checkpoint_write_detail(stderr, r); return rc; }\n"
        "#define genesis_runtime_retire_m68k_instruction harness_retire\n"
        "#define genesis_raise_divide_by_zero harness_raise\n")

    def instrument(text):
        assert text.count('#include "runtime.h"\n') == 1
        text = text.replace('#include "runtime.h"\n', wrapper, 1)
        anchor = "runtime.divide_by_zero_handler_present = 1; "
        assert text.count(anchor) == 1
        return text.replace(anchor, anchor + "runtime.sr = 0x2700; runtime.m68k_checkpoint.enabled = 1; ", 1)

    env = fd._toolchain_env()
    with tempfile.TemporaryDirectory() as directory:
        temp = pathlib.Path(directory)

        def build_and_run(name, source, runtime_source):
            (temp / (name + ".c")).write_text(source)
            (temp / (name + "_rt.c")).write_text(runtime_source)
            exe = temp / name
            checked([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                     "-I", str(root / "platforms/genesis/runtime"), str(temp / (name + ".c")),
                     str(temp / (name + "_rt.c")), "-o", str(exe)], env=env)
            return checked([exe]).stderr

        clean = build_and_run("clean", instrument(generated), runtime_c)
        assert len(fd.parse_stream(clean)) == 4, clean  # DIV(trap), BRA, MOVEQ, RTE

        # Test-only fault injection, temporary copies only.
        reg_src = instrument(generated)
        assert reg_src.count("runtime->d[7] = UINT32_C(0x00000055);") == 1
        reg_fault = build_and_run("regfault", reg_src.replace("runtime->d[7] = UINT32_C(0x00000055);",
                                                              "runtime->d[7] = UINT32_C(0x00000056);"), runtime_c)
        mem_fault = build_and_run("memfault", instrument(generated),
                                  runtime_c.replace("routed_value = saved_sr;", "routed_value = saved_sr ^ 1U;"))
        # Injection is visible only in the temporary copies.
        assert runtime_c == (root / "platforms/genesis/runtime/runtime.c").read_text()
        assert "saved_sr ^ 1U" not in (root / "platforms/genesis/runtime/runtime.c").read_text()

        checkout = os.environ.get(fd.CHECKOUT_ENV)
        if not checkout:
            print("synthetic comparison + generated fixtures OK; pinned Musashi oracle unavailable")
            return 0
        oracle_out = fd.run_oracle(pathlib.Path(checkout), compiler, image, 0x100, 0xFF8000, 0x2700, 4)
        oracle = fd.parse_stream(oracle_out)
        assert len(oracle) == 4, oracle_out
        again = fd.run_oracle(pathlib.Path(checkout), compiler, image, 0x100, 0xFF8000, 0x2700, 4)
        assert again == oracle_out, "oracle stream is deterministic"

        base = fd.compare_streams(fd.parse_stream(clean), oracle, 16, 0x100, "synthetic")
        assert base["result"] == "no_divergence" and base["domain"] == "none" and base["compared_boundaries"] == 4, base

        r = fd.compare_streams(fd.parse_stream(mem_fault), oracle, 16, 0x100, "synthetic")
        assert (r["result"], r["domain"], r["last_matching_boundary"], r["first_differing_boundary"],
                r["pc"]) == ("diverged", "cpu", 0, 1, 0x100), r
        # Registers, SR and PC match; only the stacked-SR memory write differs.
        assert [f["field"] for f in r["fields"]] == ["effect:write@00FF7FFA/w2"], r
        assert r["fields"][0]["generated"]["value"] == 0x2701 and r["fields"][0]["oracle"]["value"] == 0x2700, r
        assert fd.render(r) == fd.render(fd.compare_streams(fd.parse_stream(mem_fault), oracle, 16, 0x100, "synthetic"))

        r = fd.compare_streams(fd.parse_stream(reg_fault), oracle, 16, 0x100, "synthetic")
        assert (r["result"], r["domain"], r["last_matching_boundary"], r["first_differing_boundary"],
                r["pc"]) == ("diverged", "cpu", 2, 3, 0x124), r
        assert [f["field"] for f in r["fields"]] == ["d7"], r

        # CLI round trip (deterministic bytes, non-zero exit on divergence).
        (temp / "g.jsonl").write_text(mem_fault)
        (temp / "o.jsonl").write_text(oracle_out)
        cli = subprocess.run([sys.executable, str(ROOT / "tools/m68k_first_divergence.py"), "compare",
                              "--generated", str(temp / "g.jsonl"), "--oracle", str(temp / "o.jsonl"),
                              "--limit", "16", "--initial-pc", "0x100", "--image", "synthetic"],
                             text=True, capture_output=True)
        assert cli.returncode == 1 and cli.stdout == fd.render(fd.compare_streams(
            fd.parse_stream(mem_fault), oracle, 16, 0x100, "synthetic")), cli.stdout
    print("first divergence: unperturbed fixture equal to pinned Musashi; memory-write and register "
          "faults reported at first differing boundary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
