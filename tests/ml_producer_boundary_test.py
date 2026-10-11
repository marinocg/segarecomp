#!/usr/bin/env python3
"""SEG-049-T004 (ADR 0103): no ML producer type reaches the validator, the emitter, the runtime or generated code.

The ML producers (`ml_region_producer`, `ml_region_gen2_producer`) and the shared admission owner (`ml_admission`) may be included only by the Genesis machine
library itself, the `segarecomp` CLI and tests. The unchanged prune/validator owner (`hybrid_admission`) must not know any producer, and no runtime / emitter / CPU /
device / media library may include or mention them. Usage: <test.py> <source-root>.
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent
PRODUCER_HEADERS = re.compile(r'#\s*include\s+"[^"]*(ml_region_producer|ml_region_gen2_producer|ml_admission)\.hpp"')
ALLOWED = ("platforms/genesis/machine/", "apps/segarecomp/", "tests/")
failures = []
for path in list(root.glob("libs/**/*.[ch]pp")) + list(root.glob("libs/**/*.h")) + list(root.glob("platforms/**/*.[ch]pp")) + list(root.glob("platforms/**/*.[ch]")) + \
        list(root.glob("apps/**/*.[ch]pp")):
    rel = path.relative_to(root).as_posix()
    text = path.read_text(errors="replace")
    if PRODUCER_HEADERS.search(text) and not rel.startswith(ALLOWED):
        failures.append(f"{rel}: includes an ML producer header outside the allowed owners")
    if rel.startswith("platforms/genesis/machine/") and rel.rsplit("/", 1)[-1].startswith("hybrid_admission") and re.search(r"ml_region|ml_admission|GenesisMl", text):
        failures.append(f"{rel}: the validator owner must not know an ML producer")
    if rel.startswith(("libs/", "platforms/genesis/runtime", "platforms/genesis/viewer", "platforms/genesis/compat")) and re.search(r"ml_region_gen2|GenesisMl2|GenesisMlProducer", text):
        failures.append(f"{rel}: ML producer symbol in a runtime/emitter/CPU library")
launcher = (root / "apps/segarecomp-launcher")
for path in launcher.glob("*.[ch]pp"):
    code = "\n".join(re.sub(r"//.*", "", line) for line in path.read_text(errors="replace").splitlines())
    if re.search(r"GenesisMl|machine/genesis|threshold|\.onnx|\.pt\"|model_json", code):
        failures.append(f"{path.relative_to(root).as_posix()}: the launcher must not know models, thresholds or producers")
if failures:
    print("\n".join(failures), file=sys.stderr)
    sys.exit(1)
print("ml_producer_boundary_test: ok")
