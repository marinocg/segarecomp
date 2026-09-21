#!/usr/bin/env python3
"""SEG-020-T006: CPU vs device first-divergence classification on project-authored synthetic streams,
plus an end-to-end proof against the real runtime's detail output (test-only fault injection into a
temporary copy only)."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("genesis_device_divergence", ROOT / "tools/genesis_device_divergence.py")
dd = importlib.util.module_from_spec(spec)
sys.modules["genesis_device_divergence"] = dd
spec.loader.exec_module(dd)

COMPONENTS = ("vdp_registers", "vdp_dma", "vdp_vram", "vdp_cram", "vdp_vsram", "interrupt", "psg", "z80_bus",
              "z80_ram", "controller_io")


def cpu(boundary, pc, **over):
    r = {"boundary": boundary, "pc": pc, "sr": 0x2700, "usp": 0, "d": [0] * 8, "a": [0] * 7 + [0xFF8000],
         "unsupported": 0, "effects": []}
    r.update(over)
    return r


def dev(boundary, events=(), unsupported=0, **components):
    c = {n: "%016x" % 1 for n in COMPONENTS}
    c.update(components)
    return {"boundary": boundary, "unsupported": unsupported, "components": c, "events": list(events)}


def write(addr, value, region=4, width=2):
    return dict(k=1, r=region, w=width, a=addr, v=value)


DRIVER = r"""
#include "runtime.h"
static void w(GenesisRuntime *r, uint32_t a, uint32_t v) {
  GenesisRuntimeStop s; (void)genesis_route_access(r, a, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &v, &s); }
int main(void) {
  static GenesisRuntime r;
  unsigned i;
  r.m68k_checkpoint.enabled = 1; r.device_checkpoint.enabled = 1; r.sr = 0x2700; r.a[7] = 0xFFFF00;
  for (i = 0; i < 3; ++i) {
    w(&r, 0xC00004, 0x8F02); w(&r, 0xC00004, 0x4000 + (i << 4)); w(&r, 0xC00004, 0); w(&r, 0xC00000, 0x1230 + i);
#ifdef FAULT
    if (i == 1) r.devices.vdp.vram[2] ^= 1;  /* device evolves differently; no CPU-visible effect */
#endif
    genesis_runtime_retire_m68k_instruction(&r, 4, 0x200 + 2 * i);
    genesis_m68k_checkpoint_write_detail(stderr, &r);
    genesis_device_checkpoint_write_detail(stderr, &r);
  }
  return 0;
}
"""


def real_runtime(compiler, root):
    import os, shutil
    env = os.environ.copy()
    if not env.get("SDKROOT") and shutil.which("xcrun"):
        env["SDKROOT"] = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True).stdout.strip()
    with tempfile.TemporaryDirectory() as directory:
        d = pathlib.Path(directory)
        (d / "driver.c").write_text(DRIVER)
        out = {}
        for name, flags in (("clean", []), ("fault", ["-DFAULT"])):
            exe = d / name
            r = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", *flags,
                                "-I", str(root / "platforms/genesis/runtime"), str(d / "driver.c"),
                                str(root / "platforms/genesis/runtime/runtime.c"), "-o", str(exe)],
                               text=True, capture_output=True, env=env)
            assert r.returncode == 0, r.stderr
            run = subprocess.run([str(exe)], text=True, capture_output=True)
            assert run.returncode == 0, run.stderr
            out[name] = run.stderr
    clean, fault = out["clean"], out["fault"]
    assert len(dd.parse_device_stream(clean)) == 3 and len(dd.cpu_tool.parse_stream(clean)) == 3
    assert fault != clean
    ok = dd.compare(dd.cpu_tool.parse_stream(clean), dd.cpu_tool.parse_stream(clean), dd.parse_device_stream(clean),
                    dd.parse_device_stream(clean), 8, 0x1FE)
    assert ok["result"] == "no_divergence", ok
    r = dd.compare(dd.cpu_tool.parse_stream(fault), dd.cpu_tool.parse_stream(clean), dd.parse_device_stream(fault),
                   dd.parse_device_stream(clean), 8, 0x1FE)
    assert (r["domain"], r["classification"], r["last_matching_boundary"], r["first_differing_boundary"]) == (
        "device", "device_state", 1, 2), r
    assert [f["field"] for f in r["fields"]] == ["state:vdp_vram"], r
    # Bounded storage in the real runtime: the runtime struct keeps a fixed event capacity.
    assert "GENESIS_DEVICE_CHECKPOINT_EVENT_CAPACITY 8" in (root / "platforms/genesis/runtime/runtime.h").read_text()


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "cc"
    real_runtime(compiler, ROOT)
    cpus = [cpu(1, 0x102), cpu(2, 0x104), cpu(3, 0x106)]
    devs = [dev(1, [write(0xC00004, 0x8F02)]), dev(2), dev(3)]
    same = dd.compare(cpus, cpus, devs, devs, 8, 0x100, "synthetic")
    assert same["result"] == "no_divergence" and same["domain"] == "none" and same["compared_boundaries"] == 3, same

    # CPU-first: a register differs (device streams also differ later; the CPU domain wins).
    cpu_bad = [cpus[0], cpu(2, 0x104, d=[0, 0, 0, 0, 0, 0, 0, 7]), cpus[2]]
    dev_bad = [devs[0], dev(2, vdp_vram="%016x" % 9), devs[2]]
    r = dd.compare(cpu_bad, cpus, dev_bad, devs, 8, 0x100)
    assert (r["domain"], r["last_matching_boundary"], r["first_differing_boundary"], r["pc"]) == ("cpu", 1, 2, 0x102), r
    assert [f["field"] for f in r["fields"]] == ["d7"], r
    # Same boundary with both CPU and device differences: still the cpu domain.
    r = dd.compare(cpu_bad, cpus, [devs[0], dev(2, vdp_vram="%016x" % 9), devs[2]], devs, 8, 0x100)
    assert r["domain"] == "cpu", r

    # Device state evolution: CPU digests match, a VRAM component digest differs.
    state_bad = [devs[0], devs[1], dev(3, vdp_vram="%016x" % 9)]
    r = dd.compare(cpus, cpus, state_bad, devs, 8, 0x100, "synthetic")
    assert (r["domain"], r["classification"], r["last_matching_boundary"], r["first_differing_boundary"],
            r["pc"]) == ("device", "device_state", 2, 3, 0x104), r
    assert [f["field"] for f in r["fields"]] == ["state:vdp_vram"], r
    assert r["image"] == "synthetic"

    # Device command/interrupt event differences with CPU equal.
    for name, bad_events, field in (
            ("wrong value", [write(0xC00004, 0x8F04)], "event:write@vdp/00C00004/w2"),
            ("missing event", [], "event:write@vdp/00C00004/w2"),
            ("extra irq admit", [write(0xC00004, 0x8F02), dict(k=3, r=0, w=0, a=0, v=6)], "event:irq_admit")):
        r = dd.compare(cpus, cpus, [dev(1, bad_events), devs[1], devs[2]], devs, 8, 0x100)
        assert (r["domain"], r["classification"], r["first_differing_boundary"]) == ("device", "device_command", 1), (name, r)
        assert [f["field"] for f in r["fields"]] == [field], (name, r)
    # Order of repeated events to one destination matters; distinct destinations keep order (event:order).
    a = dev(1, [write(0xC00004, 1), write(0xC00004, 2)])
    b = dev(1, [write(0xC00004, 2), write(0xC00004, 1)])
    r = dd.compare(cpus[:1], cpus[:1], [a], [b], 4, 0x100)
    assert r["classification"] == "device_command" and len(r["fields"]) == 2, r
    x = dev(1, [write(0xC00004, 1), write(0xC00000, 2)])
    y = dev(1, [write(0xC00000, 2), write(0xC00004, 1)])
    r = dd.compare(cpus[:1], cpus[:1], [x], [y], 4, 0x100)
    assert [f["field"] for f in r["fields"]] == ["event:order"], r

    # Unsupported is never equal; presence; limit; determinism.
    # Unsupported CPU boundary fails closed even when visible registers also differ (either side).
    bad_regs = cpu(1, 0x102, d=[0, 0, 0, 0, 0, 0, 0, 7], unsupported=1)
    for g, e in (([bad_regs] + cpus[1:], cpus), (cpus, [bad_regs] + cpus[1:])):
        r = dd.compare(g, e, devs, devs, 8, 0x100)
        assert (r["result"], r["domain"], r["first_differing_boundary"]) == ("unsupported_for_comparison", "none", 1), r
    r = dd.compare(cpus, cpus, [dev(1, unsupported=1)] + devs[1:], devs, 8, 0x100)
    assert r["result"] == "unsupported_for_comparison" and r["domain"] == "none", r
    r = dd.compare(cpus, cpus, devs[:2], devs, 8, 0x100)
    assert r["domain"] == "device" and r["first_differing_boundary"] == 3, r
    assert dd.compare(cpus, cpus, state_bad, devs, 2, 0x100)["result"] == "no_divergence"
    try:
        dd.compare(cpus, cpus, devs, devs, 0, 0x100)
        raise AssertionError("limit 0 accepted")
    except ValueError:
        pass
    assert dd.render(dd.compare(cpus, cpus, state_bad, devs, 8, 0x100)) == dd.render(
        dd.compare(cpus, cpus, state_bad, devs, 8, 0x100))
    # The M68k tool ignores device lines in a shared stream.
    import json
    text = "\n".join(json.dumps(x) for x in ({"m68k_checkpoint": cpus[0]}, {"device_checkpoint": devs[0]}))
    assert dd.cpu_tool.parse_stream(text) == [cpus[0]] and dd.parse_device_stream(text) == [devs[0]]

    # CLI round trip: deterministic bytes and non-zero exit on divergence.
    with tempfile.TemporaryDirectory() as directory:
        d = pathlib.Path(directory)
        def stream(cs, ds):
            return "\n".join(json.dumps(x) for pair in zip(cs, ds) for x in ({"m68k_checkpoint": pair[0]},
                                                                            {"device_checkpoint": pair[1]})) + "\n"
        (d / "g.jsonl").write_text(stream(cpus, state_bad))
        (d / "e.jsonl").write_text(stream(cpus, devs))
        cli = subprocess.run([sys.executable, str(ROOT / "tools/genesis_device_divergence.py"), "--generated",
                              str(d / "g.jsonl"), "--expected", str(d / "e.jsonl"), "--limit", "8",
                              "--initial-pc", "0x100", "--image", "synthetic"], text=True, capture_output=True)
        assert cli.returncode == 1 and cli.stdout == dd.render(dd.compare(
            cpus, cpus, state_bad, devs, 8, 0x100, "synthetic")), cli.stdout
    print("device divergence classification OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
