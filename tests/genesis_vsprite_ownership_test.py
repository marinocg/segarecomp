"""SEG-140-T002 (ADR 0303): there is exactly one pattern/palette decoder. virtual_sprite.c reaches pixels only through the VDP's own routines and carries no decode table or nibble/CRAM arithmetic of its own."""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1]
runtime = root / "platforms" / "genesis" / "runtime"
src = (runtime / "virtual_sprite.c").read_text()
hdr = (runtime / "virtual_sprite.h").read_text()
vdp = (runtime / "vdp_render.c").read_text()
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
code = re.sub(r"//.*", "", code)
check("genesis_vdp_resolve_sprite_pixel(" in code and "genesis_vdp_decode_cram_entry(" in code, "pixels are resolved through the VDP's own routines")
check(not re.search(r">>\s*4|&\s*0x0F|\*\s*255U?\s*/\s*7|0x0E|<<\s*9", code, flags=re.I), "virtual_sprite.c has no nibble extraction or CRAM channel arithmetic of its own")
check(not re.search(r"\b(static\s+)?(const\s+)?uint8_t\s+\w+\s*\[\s*\w*\s*\]\s*=\s*\{", code), "virtual_sprite.c defines no lookup table")
check("vdp_render.h" in hdr, "the header builds on the VDP header")
check(len(re.findall(r"genesis_vdp_sprite_pattern_pixel\(", vdp)) >= 3, "the VDP keeps ONE static pattern reader shared by the row composer and the resolver")
check("sprites" in (runtime / "CMakeLists.txt").read_text() and "segarecomp_runtime_genesis_vsprite" in (runtime / "CMakeLists.txt").read_text(), "the module is its own library, not part of the runtime every program links")
runtime_cmake = (runtime / "CMakeLists.txt").read_text()
check(re.search(r"add_library\(segarecomp_runtime_genesis STATIC[^)]*virtual_sprite", runtime_cmake) is None, "virtual_sprite.c is not compiled into segarecomp_runtime_genesis")
if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print("genesis_vsprite_ownership_test: ok")
