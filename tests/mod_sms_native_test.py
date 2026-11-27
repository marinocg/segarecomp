#!/usr/bin/env python3
"""SEG-147-T001 + SEG-147-T002 (ADR 0324): a Z80 routine injected into bank 7 of a synthetic Sega-mapper cartridge runs through the UNCHANGED generated-native Master System producer
(`segarecomp build --data-package`; no interpreter, no JIT, no runtime patching) and leaves the hand-derived machine state; the unmodified cartridge does not. The variant is built from the verified
effective image under the package's declared mapper, its identity differs from Vanilla, and a conflicting declaration, platform, image or package identity builds nothing. Project-authored bytes only;
every run is bounded by --frames and a timeout.
usage: mod_sms_native_test.py <segarecomp-cli> <c-compiler> <product-root>"""
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import sms_fixture_rom as builder  # noqa: E402
from mod_test_support import Cli, sha, temp, write  # noqa: E402

exe, compiler, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
cli = Cli(exe)
tmp = temp()

# the base program: map bank 7 into slot 2, call its stub at 0x8000 (a plain `ret`), then store A at $C000
code, labels = builder.Assembler("""
.org 0x0000
        di
        im 1
        ld sp,0xDFF0
        jp main
.org 0x0038
        reti
.org 0x0066
        retn
.org 0x0100
main:   ld hl,0xC000
        ld de,0xC001
        ld bc,0x0FFF
        ld (hl),0
        ldir
        ld a,7
        ld (0xFFFF),a
site:   call 0x8000
        ld (0xC000),a
idle:   jr idle
""").assemble()
stub, _ = builder.Assembler(".org 0x8000\n        ret\n.org 0x8010\n        add a,3\n        ret\n").assemble()   # bank 7: the stub and `helper`
markers = {7 * 0x4000 + (address - 0x8000): byte for address, byte in stub.items()}
image = builder.build_rom(0x20000, code, markers)
site = labels["site"] + 1
vanilla = write(tmp / "vanilla.sms", image)

ASM = """free * 0x0300 0x40 0xff
free * 0x1c100 0x100 0xff
org * 0x0300
tally:                  ; the fixed first 1 KiB: reachable from every bank
  ld hl,0xc002
  inc (hl)
  ret
org * 0x1c100 at 0x8100 ; bank 7, executed through slot 2
entry:
  ld a,0x20
  ld b,4
loop:
  inc a
  djnz loop
  call tally
  call z,tally          ; not taken: the count tally just incremented is not zero
  call tally
  call @helper          ; existing cartridge code in the same bank
  ld (0xc001),a
  ld a,(table)
  ret
data
table:
  db 0x5a
"""
project = tmp / "proj"
write(project / "asm" / "main.asm", ASM)
write(project / "edits" / "main.edits", "# retarget the base program's call from the stub to the injected routine\nrev-a u16 0x%08x 0x8000 0x8100\n" % site)
write(project / "mod.toml", f'[mod]\nid = "sms-inject"\nversion = "1.0.0"\nname = "SMS Inject"\nlicense = "CC0-1.0"\n\n[target]\nplatform = "master-system"\n\n[[target.revision]]\nlabel = "rev-a"\nsha256 = "{sha(image)}"\nmapper = "sega"\n\n'
      '[[source]]\nkind = "rom-edit-set"\npath = "edits/main.edits"\n\n[[source]]\nkind = "guest-asm"\npath = "asm/main.asm"\ncpu = "z80"\n\n[[symbol]]\nname = "helper"\nrevision = "rev-a"\naddress = "0x0001c010"\nprovenance = "verified"\n')

package = tmp / "inject.segapatch"
rc, packed, p = cli.run("mod", "pack", project, "--out", package, "--rom", vanilla)
assert rc == 0 and packed["ok"], (rc, packed, p.stderr)
applied = []
for i in range(3):
    out = tmp / f"modded{i}.sms"
    rc, doc, p = cli.run("mod", "apply", package, "--rom", vanilla, "--out", out)
    assert rc == 0 and doc["blocks_injected"] == 2 and doc["header_repaired"] is True, (rc, doc, p.stderr)
    applied.append(out.read_bytes())
assert applied[0] == applied[1] == applied[2] and applied[0] != image and vanilla.read_bytes() == image
modded = applied[0]
changed = {i for i in range(len(image)) if image[i] != modded[i]}
assert all(0x300 <= i < 0x340 or 0x1C100 <= i < 0x1C200 or i in (site, site + 1, 0x7FFA, 0x7FFB) for i in changed), sorted(changed)[:20]
expected_sum = (sum(modded[:0x7FF0]) + sum(modded[0x8000:])) & 0xFFFF
assert modded[0x7FFA] | modded[0x7FFB] << 8 == expected_sum


def build(name, rom, *extra):
    out = tmp / name
    r = subprocess.run([exe, "build", "--rom", str(rom), "--output", str(out), "--cc", compiler, "--optimize", "0", "--runtime-dir", str(root / "platforms" / "master-system"),
                        "--cc-arg", "-Wno-misleading-indentation", *map(str, extra)], text=True, capture_output=True, timeout=900)
    return r, out, json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}


def run(out):
    artifacts = out / "artifacts"
    artifacts.mkdir()
    game = out / ("game.exe" if sys.platform == "win32" else "game")
    r = subprocess.run([str(game), "--frames", "20", "--artifacts", str(artifacts)], text=True, capture_output=True, timeout=120)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    return (artifacts / "ram.bin").read_bytes()


data = ("--data-package", package, "--data-identity", packed["package_identity"])
r, out, status = build("modded", vanilla, *data)
assert r.returncode == 0 and status["status"] == "ok", (r.returncode, r.stdout[-2000:], r.stderr[-2000:], status)
assert status["platform"] == "master-system" and status["mapper"] == "sega" and status["data_mod"] == {"package_identity": packed["package_identity"], "original_sha256": sha(image), "effective_sha256": sha(modded)}, status
assert json.loads((out / "generated" / "sms_cartridge.json").read_text())["sha256"] == sha(modded)   # the program's cartridge identity (its save states bind to it) is the variant's, not Vanilla's
assert not (out / "input").exists(), "the derived image is not kept"
ram = run(out)
# $C000 = the value the routine returned (the data table byte), $C001 = 0x20 + 4 + 3 (the loop, then the same-bank helper), $C002 = two calls into the fixed area
assert (ram[0], ram[1], ram[2]) == (0x5A, 0x27, 2), (ram[0], ram[1], ram[2])

r, vout, vstatus = build("vanilla", vanilla, "--mapper", "sega")
assert r.returncode == 0, (r.stdout[-2000:], r.stderr[-2000:])
vram = run(vout)
assert (vram[0], vram[1], vram[2]) == (7, 0, 0) and "data_mod" not in vstatus, (vram[0], vram[1], vram[2], vstatus)
assert json.loads((vout / "generated" / "sms_cartridge.json").read_text())["sha256"] == sha(image) != sha(modded)
assert vanilla.read_bytes() == image

# fail closed before any generated C exists
other = write(tmp / "other.sms", builder.build_rom(0x20000, code, {}))
for name, rom, extra, diagnostic in [("mapper-conflict", vanilla, data + ("--mapper", "rom_only"), "DATA_MAPPER_MISMATCH"),
                                     ("platform-conflict", vanilla, data + ("--platform", "genesis"), "DATA_PLATFORM_MISMATCH"),
                                     ("wrong-image", other, data, "DATA_IMAGE_MISMATCH"),
                                     ("wrong-identity", vanilla, ("--data-package", package, "--data-identity", "0" * 64), "DATA_IDENTITY_CHANGED")]:
    r, out, status = build(name, rom, *extra)
    assert r.returncode == 1 and status.get("diagnostic") == diagnostic and not (out / "generated").exists(), (name, r.returncode, status)
print("mod_sms_native_test: ok")
