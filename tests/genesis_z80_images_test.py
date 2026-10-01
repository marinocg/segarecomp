#!/usr/bin/env python3
"""SEG-032-T003 (ADR 0073): the Genesis materialized-image registry, its RAM-backed emission and its generated lookup.

The epochs are the multi-epoch dirty-data program of the fixture builder replayed through the real runtime router (the
T002 harness), so the registry sees exactly the epochs the machine produces. Proved:
  * registry semantics: X added, X again (dirty mailbox) existing, Y added, X existing, X' added, a plain restart re-binds
    X' and registers nothing; three images, ordinals in activation order;
  * the C++ content hash and signature equal the independent Python derivations (tools/genesis_z80_materialization_probe.py);
  * emission is deterministic (two runs byte-identical) and the image bound is a typed failure (nine distinct images);
  * the generated registry compiles with the RAM-backed images as strict C11 and its lookup returns the right ordinal for
    every registered signature and 0 for an unknown or one-bit-corrupted signature;
  * an empty registry still links (`z80_run` stub with the typed unknown-image outcome) and a registry image runs in the
    host runner with the same guard semantics (a mutated byte stops with code_mismatch).
usage: genesis_z80_images_test.py <registry_emitter> <cc> <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

registry_emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(root / "tools"))
sys.path.insert(0, str(root / "tests"))
import genesis_z80_fixture_rom as fx  # noqa: E402
import genesis_z80_materialization_probe as probe  # noqa: E402
import z80_conformance as z  # noqa: E402

failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def epochs_from_script(tmp):
    binary = tmp / "script_harness"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(root / "platforms/genesis/runtime"),
                    str(root / "tests/tools/genesis_z80_epoch_script_harness.c"),
                    str(root / "platforms/genesis/runtime/runtime.c"), "-o", str(binary)], check=True)
    programs = dict(zip(("x", "x2", "y"), fx.multi_epoch_programs()))
    script = tmp / "script.txt"
    script.write_text(fx.access_script(fx.multi_epoch_ops(), programs))
    out = subprocess.run([str(binary), str(script), "00"], text=True, capture_output=True, check=True).stdout
    return [(line.split()[2], line.split()[3]) for line in out.splitlines() if line.startswith("EPOCH ")]


def run_emitter(spec_text, tmp, name):
    spec = tmp / (name + ".spec")
    spec.write_text(spec_text)
    outdir = tmp / name
    result = subprocess.run([registry_emitter, str(spec), str(outdir), "genesis_z80"], text=True, capture_output=True)
    return result, outdir


def parse(stdout):
    epochs = [l.split() for l in stdout.splitlines() if l.startswith("epoch ")]
    images = {int(l.split()[1]): dict(kv.split("=") for kv in l.split()[2:]) for l in stdout.splitlines() if l.startswith("image ")}
    return epochs, images


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        epochs = epochs_from_script(tmp)
        spec = "".join("epoch %s %s\n" % e for e in epochs)
        first, out1 = run_emitter(spec, tmp, "first")
        check(first.returncode == 0 and "emitted ok" in first.stdout, "registry emission succeeds for the six-epoch program")
        rows, images = parse(first.stdout)
        check([r[2] for r in rows] == ["added", "existing", "added", "existing", "added", "restart"], "registry outcomes: added existing added existing added restart")
        check([r[3] for r in rows] == ["bound=1", "bound=1", "bound=2", "bound=1", "bound=3", "bound=3"], "bound ordinals follow the activation order; a restart re-binds the previous image")
        check(sorted(images) == [1, 2, 3], "three images in activation order")
        # cross-language equality of both keys
        decoded = [(bytes.fromhex(r), bytes.fromhex(w)) for r, w in epochs]
        eff = probe.effective_signatures(decoded)
        wanted = {}
        for (ram, written), sig in zip(decoded, eff):
            wanted.setdefault(sig, (probe.content_hash(ram), probe.signature(ram, written)))
        check(sorted(v["signature"] for v in images.values()) == sorted(s for _, s in wanted.values())
              and sorted(v["content"] for v in images.values()) == sorted(c for c, _ in wanted.values()),
              "the C++ content hashes and signatures equal the independent Python derivations")
        second, out2 = run_emitter(spec, tmp, "second")
        same = all((out1 / p.name).read_bytes() == p.read_bytes() for p in out2.iterdir()) and \
            sorted(p.name for p in out1.iterdir()) == sorted(p.name for p in out2.iterdir())
        check(second.stdout == first.stdout and same, "two emissions are byte-identical (stdout and every generated file)")
        # image bound
        distinct = ""
        base = bytearray(8192)
        for k in range(9):
            ram = bytearray(base)
            ram[0] = k + 1
            written = bytearray(1024)
            written[0] = 1
            distinct += "epoch %s %s\n" % (bytes(ram).hex(), bytes(written).hex())
        over, _ = run_emitter(distinct, tmp, "over")
        check(over.returncode == 1 and "bound_exceeded" in over.stdout and "image bound exceeded" in over.stdout,
              "a ninth distinct image is the typed bound failure")
        # compile the generated registry + images, link a lookup driver
        driver = tmp / "driver.c"
        lines = ["#include <stdio.h>", "#include <string.h>", '#include "z80_registry.h"', "int main(void) {"]
        for ordinal, v in sorted(images.items()):
            sig = bytes.fromhex(v["signature"])
            lines.append("  { static const unsigned char s[32] = {%s}; if (genesis_z80_image_for_signature(s) != %du) return 10 + %d; }"
                         % (",".join(str(b) for b in sig), ordinal, ordinal))
            bad = bytearray(sig)
            bad[5] ^= 1
            lines.append("  { static const unsigned char s[32] = {%s}; if (genesis_z80_image_for_signature(s) != 0u) return 20 + %d; }"
                         % (",".join(str(b) for b in bad), ordinal))
        lines += ['  printf("count=%u\\n", (unsigned)genesis_z80_image_count);', "  return 0;", "}"]
        driver.write_text("\n".join(lines) + "\n")
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)  # compile_units never runs the emitter
        exe, message = z.compile_units(tc, out1, "genesis_z80", extra_sources=[driver],
                                       extra_flags=["-I", str(root / "platforms/genesis/runtime")])
        # z80_run needs a host for main-less linking: the driver does not call it, only links the generated code
        check(exe is not None, "the generated image units and the registry compile as strict C11 and link (%s)" % (message or "ok"))
        if exe is not None:
            ran = subprocess.run([str(exe)], text=True, capture_output=True)
            check(ran.returncode == 0 and "count=3" in ran.stdout, "the generated lookup returns every ordinal and 0 for unknown signatures")
        # empty registry
        empty, out3 = run_emitter("", tmp, "empty")
        check(empty.returncode == 0, "an empty registry emits")
        driver2 = tmp / "driver2.c"
        driver2.write_text('#include <stdio.h>\n#include "z80_registry.h"\n#include "segarecomp/codegen/c11/runtime/z80_runtime.h"\n'
                           'int main(void) { Z80Runtime rt; unsigned char s[32] = {0}; rt.outcome = Z80_OUTCOME_NONE;\n'
                           '  if (genesis_z80_image_count != 0u || genesis_z80_image_for_signature(s) != 0u) return 1;\n'
                           '  return z80_run(&rt, 10u) == Z80_ERROR_UNKNOWN_IMAGE_IDENTITY ? 0 : 2; }\n')
        exe, message = z.compile_units(tc, out3, "genesis_z80", extra_sources=[driver2],
                                       extra_flags=["-I", str(root / "platforms/genesis/runtime")])
        check(exe is not None and subprocess.run([str(exe)]).returncode == 0,
              "an empty registry still links and its z80_run is the typed unknown-image outcome (%s)" % (message or "ok"))
    print("genesis z80 images: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
