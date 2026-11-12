#!/usr/bin/env python3
"""SEG-136-T004 (ADR 0292): a synthetic routine injected into a Genesis image by `segarecomp mod apply` is recompiled by the generated-native path (no interpreter, no JIT) and executed; the
resulting machine state is the hand-derived one. The routine uses a counted loop, PC-relative data, a subroutine call and a work-RAM store. The unmodified image does not complete,
so the state comes from the injected bytes. Synthetic image only; strict C11, finite instruction budget.
usage: mod_guest_exec_test.py <segarecomp-cli> <c-compiler> <repo-root>"""
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mod_test_support import Cli, sha, write  # noqa: E402

exe, compiler, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
cli = Cli(exe)
work = pathlib.Path(tempfile.mkdtemp(prefix="segarecomp-guest-exec-"))

size = 0x2000
image = bytearray(0x00 for _ in range(size))
struct.pack_into(">II", image, 0, 0x00FF0000, 0x1000)
image[0x100:0x110] = b"SEGA MEGA DRIVE "
image[0x1000:0x100A] = bytes.fromhex("4eb90000100c60fe4e75")   # the entry carrier the startup analysis accepts; the mod code is elsewhere in the same image
image = bytes(image)

ASM = """free * 0x0800 0x100 0x00
org * 0x0800
entry:
  moveq #0,d2
  moveq #4,d0
loop:
  addq.l #2,d2
  dbra d0,loop
  lea table(pc),a0
  move.b (a0)+,d4
  move.w d2,0xff0010
  bsr.s sub
  rts
sub:
  addq.l #1,d2
  rts
data
table:
  dc.b 7,8
"""
project = work / "proj"
write(project / "asm" / "main.asm", ASM)
write(project / "mod.toml", f'[mod]\nid = "exec-mod"\nversion = "1.0.0"\nname = "Exec"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "genesis"\n\n[[target.revision]]\nlabel = "rev-a"\nsha256 = "{sha(image)}"\n\n'
      '[[source]]\nkind = "guest-asm"\npath = "asm/main.asm"\ncpu = "m68k"\n')
rc, doc, p = cli.run("mod", "apply", project, "--rom", write(work / "vanilla.bin", image), "--out", work / "modded.bin")
assert rc == 0 and doc["ok"] and doc["blocks_injected"] == 1, (rc, doc, p.stderr)
modded = (work / "modded.bin").read_bytes()
assert modded != image and modded[:0x800] == image[:0x800] and modded[0x900:] == image[0x900:]

env = os.environ.copy()
if not env.get("SDKROOT") and shutil.which("xcrun"):
    env["SDKROOT"] = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True, check=True).stdout.strip()
OBSERVE = (' fprintf(stderr, "d0=%u d2=%u d4=%u a0=%u ram10=%u,%u pc=%u\\n", (unsigned)runtime.d[0], (unsigned)runtime.d[2], (unsigned)runtime.d[4], (unsigned)runtime.a[0],'
           ' (unsigned)runtime.work_ram[0x10], (unsigned)runtime.work_ram[0x11], (unsigned)runtime.pc);')


# The AOT enumeration compiles every aligned start of the image. The run starts at the injected routine (pc 0x800) The routine has no caller, so its final RTS stops fail-closed (return_target_mismatch) at 0x818, after every effect below.
SEED = ""


def generate(path, data):
    r = subprocess.run([exe, "emit-general-startup-bridge-c", "--rom", str(path), "--entry", "00001000", "--mapping-base", "00000000", "--rom-sha256", sha(data), "--immutable-rom-aot"],
                       text=True, capture_output=True)
    return r.stdout


def execute(name, path, data):
    generated = generate(path, data)
    generated = generated.replace("runtime.pc = UINT32_C(0x00001000);", "runtime.pc = UINT32_C(0x00000800);" + SEED, 1)
    assert "translation rejected" not in generated[:100] and "genesis_runtime_run" in generated, generated[:200]
    assert generated == generate(path, data).replace("runtime.pc = UINT32_C(0x00001000);", "runtime.pc = UINT32_C(0x00000800);" + SEED, 1), "the generated C is not deterministic"
    anchor = re.search(r"\} while \([^;]*\);", generated)
    assert anchor is not None
    source = work / (name + ".c")
    source.write_text(generated[:anchor.end()] + OBSERVE + generated[anchor.end():])
    binary = work / name
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(root / "platforms/genesis/runtime"), str(source),
                            str(root / "platforms/genesis/runtime/runtime.c"), "-o", str(binary)], text=True, capture_output=True, env=env)
    assert build.returncode == 0, build.stderr
    return subprocess.run([str(binary), "--instruction-budget", "10000"], text=True, capture_output=True, timeout=300)


run = execute("modded", work / "modded.bin", modded)
# d0 = 0x0000FFFF (DBRA ran the word to -1), d2 = 5 * 2 + 1 (the loop plus the subroutine), d4 = the PC-relative table's first byte, a0 = table + 1 (post-increment),
# work RAM 0xFF0010 holds d2 as stored BEFORE the subroutine (10), and the run stops at the routine's final RTS (0x818), which has no caller frame.
want = "d0=65535 d2=11 d4=7 a0=%d ram10=0,10 pc=%d" % (0x81F, 0x818)
assert want in run.stderr and '"diagnostic_category":"return_target_mismatch"' in run.stdout, (want, run.stderr, run.stdout)

# the unmodified image has zero-padded bytes there: it must not produce that state
vanilla = execute("vanilla", work / "vanilla.bin", image)
assert "d2=11" not in vanilla.stderr and "ram10=0,10" not in vanilla.stderr, vanilla.stderr
print("mod_guest_exec_test: ok")
