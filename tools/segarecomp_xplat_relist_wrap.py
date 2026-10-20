#!/usr/bin/env python3
# SEG-075 T002 experimental GNU as wrapper (runs inside the ephemeral offline container; argv[1] = absolute path of the real assembler).
# 1) run the real assembler exactly as invoked, plus a listing (-almn=OUT.lst); 2) keep its input as OUT.s; 3) re-assemble a copy with only
# the `#APP` / `#NO_APP` marker lines removed, with listing OUT.relist.lst and object OUT.relist.o. The extractor uses the relist listing
# only when OUT.relist.o is byte-identical to OUT.
import os
import subprocess
import sys

real = sys.argv[1]
args = sys.argv[2:]
out = None
for i, a in enumerate(args):
    if a == "-o" and i + 1 < len(args):
        out = args[i + 1]
if out is None:
    os.execv(real, [real] + args)
inputs = [a for i, a in enumerate(args) if not a.startswith("-") and (i == 0 or args[i - 1] != "-o") and a.endswith((".s", ".S", ".sx", ".asm"))]
if inputs:
    src = open(inputs[-1], "rb").read()
    base_args = [a for a in args if a != inputs[-1]]
else:
    src = sys.stdin.buffer.read()
    base_args = [a for a in args if a != "-"]
with open(out + ".s", "wb") as f:
    f.write(src)
r = subprocess.run([real, f"-almn={out}.lst"] + base_args + [out + ".s"])
if r.returncode:
    sys.exit(r.returncode)
scrub = b"".join(ln for ln in src.splitlines(keepends=True) if ln.strip() not in (b"#APP", b"#NO_APP"))
with open(out + ".relist.s", "wb") as f:
    f.write(scrub)
rel = []
skip = False
for a in base_args:
    if skip:
        skip = False
        continue
    if a == "-o":
        skip = True
        continue
    rel.append(a)
subprocess.run([real, f"-almn={out}.relist.lst", "-o", out + ".relist.o"] + rel + [out + ".relist.s"])
sys.exit(0)
