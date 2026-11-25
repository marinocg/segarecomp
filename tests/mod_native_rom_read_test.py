#!/usr/bin/env python3
"""SEG-152-T018 (ADR 0321): a native provider reads manifest-declared `m68k.rom` ranges of the EFFECTIVE image in `render`. Everything here uses the FABRICATED gameplay fixture image (project-authored, no
commercial bytes). Proves: (1) the provider reads widths 1/2/4 big-endian and the values equal the fixture image's bytes; (2) the generated artifact contains exactly the declared bytes and no others;
(3) each violation (undeclared, straddling, odd word, wrong phase, per-callback budget) is a stable cause that faults only the mod while the guest still runs to its own stop; (4) a range past the image is
refused at build with E_MEMORY_RANGE and no executable; (5) the declaration rules (read-only, named revision, non-empty, 4096-byte limit) are stable package classes; (6) `mod inspect` lists the ranges.
usage: mod_native_rom_read_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import importlib.util
import json
import pathlib
import re
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mod_support import Harness  # noqa: E402

PROVIDER = r'''/* Synthetic ROM-read probe (SEG-152-T018). */
#include <segarecomp_mod_genesis.h>

static int done;
static const char digits[] = "0123456789abcdef";

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM ? 0 : 1;
}

static uint32_t put_hex(char *out, uint32_t value, uint32_t nibbles) {
  uint32_t i;
  for (i = 0U; i < nibbles; ++i) out[i] = digits[(value >> (4U * (nibbles - 1U - i))) & 15U];
  return nibbles;
}

static void provider_phase(void *state, const SegaModHost *host) {
  (void)state;
  if (host->phase != PROBE_PHASE || done) return;
  done = 1;
  {
    uint32_t a = 0U, b = 0U, c = 0U, d = 0U, e = 0U, n = 0U, i;
    char line[64];
    (void)i;
    PROBE
    line[n++] = 'R'; line[n++] = ':';
    n += put_hex(line + n, a, 2U); line[n++] = ' ';
    n += put_hex(line + n, b, 4U); line[n++] = ' ';
    n += put_hex(line + n, c, 8U); line[n++] = ' ';
    n += put_hex(line + n, d, 8U); line[n++] = ' ';
    n += put_hex(line + n, e, 2U);
    host->log(line, n);
  }
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 0U, SEGA_MOD_PLATFORM, "rom-read-probe", 1U, provider_init, provider_phase, NULL, NULL, NULL,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &provider;
}
'''
OK_PROBE = ("host->read(SEGA_GEN_SPACE_M68K_ROM, 0x100U, 1U, &a); host->read(SEGA_GEN_SPACE_M68K_ROM, 0x102U, 2U, &b); host->read(SEGA_GEN_SPACE_M68K_ROM, 0x104U, 4U, &c);"
            " host->read(SEGA_GEN_SPACE_M68K_ROM, 0x180U, 4U, &d); host->read(SEGA_GEN_SPACE_M68K_ROM, 0x187U, 1U, &e);")
RANGES = [(0x100, 16), (0x180, 8)]


def manifest(sha, ranges, extra_memory="", caps='"host.log", "memory.read.m68k.rom"'):
    memory = "".join(f'\n[[memory]]\nrevision = "fixture"\nspace = "m68k.rom"\noffset = {o}\nlength = {n}\naccess = "read"\n' for o, n in ranges)
    return f"""[mod]
id = "rom-read-probe"
author = "segarecomp tests"
version = "0.1.0"
name = "ROM read probe"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "fixture"
sha256 = "{sha}"

[[source]]
kind = "native-c11"
path = "src/provider.c"

[abi]
native = 2

[capabilities]
required = [{caps}]
{memory}{extra_memory}"""


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), sys.argv[4]
    if sys.platform != "linux":
        print("mod_native_rom_read_test: skipped (the generated-native build is exercised on Linux)")
        return 0
    spec = importlib.util.spec_from_file_location("fixture_rom", root / "platforms/genesis/sdk/examples/gameplay/fixture_rom.py")
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="segarecomp-rom-read-") as directory:
        tmp = pathlib.Path(directory)
        h = Harness(cli, compiler, root, sdl, tmp)
        check = h.check
        image = fixture.make_rom()
        rom = tmp / "fixture.rom"
        rom.write_bytes(image)
        import hashlib
        image_sha = hashlib.sha256(image).hexdigest()
        check(len(image) >= 0x200, "the fixture image covers the probed offsets")

        def project(name, probe=OK_PROBE, phase="SEGA_MOD_PHASE_RENDER", ranges=RANGES, text=None):
            d = tmp / name
            (d / "src").mkdir(parents=True)
            (d / "src/provider.c").write_text(PROVIDER.replace("PROBE_PHASE", phase).replace("PROBE", probe))
            (d / "mod.toml").write_text(text if text is not None else manifest(image_sha, ranges))
            return d

        def pack(d):
            out = tmp / (d.name + ".segapatch")
            r = h.run_cli("mod", "pack", d, "--out", out)
            doc = json.loads(r.stdout)
            return r, doc, out

        def packed(name, **kw):
            r, doc, out = pack(project(name, **kw))
            check(r.returncode == 0 and doc.get("ok"), f"pack {name}: {r.stdout[:300]}")
            return out, doc.get("package_identity")

        # ---- (1)/(2) the reads and the generated bytes ----
        pkg, ident = packed("ok")
        r = h.build(rom, tmp / "ok", pkg, ident, viewer=False, native=True)
        check(r.returncode == 0 and (tmp / "ok/game").is_file(), f"the ROM-reading provider builds: {r.stdout[-300:]}{r.stderr[-300:]}")
        if h.failures:
            return 1
        _, lines = h.headless(tmp / "ok", "ok", "10:1:0x00", frames="1", first="3")
        want = "R:%02x %04x %08x %08x %02x" % (image[0x100], int.from_bytes(image[0x102:0x104], "big"), int.from_bytes(image[0x104:0x108], "big"), int.from_bytes(image[0x180:0x184], "big"), image[0x187])
        logs = [e.get("line") for e in lines.get("MOD_LOG", [])]
        check(want in logs, f"the provider read widths 1/2/4 big-endian from the declared ranges: {logs} vs {want}")
        check(lines["MOD_SUMMARY"][0]["faulted"] is False, "the ROM reads do not fault")
        art = (tmp / "ok/native-mod/native_artifact.c").read_text()
        size = int(re.search(r"segarecomp_mod_rom_data_size = (\d+)UL", art).group(1))
        data = [int(x) for x in re.search(r"segarecomp_mod_rom_data\[\] = \{([^}]*)\}", art).group(1).split(",") if x.strip()][:-1]
        declared = b"".join(image[o:o + n] for o, n in RANGES)
        check(size == 24 and bytes(data) == declared, "the generated program holds exactly the declared bytes in range order (a byte outside every range is absent)")
        check("{SEGARECOMP_GEN_SPACE_ROM, 256UL, 16UL, 1UL}" in art and "{SEGARECOMP_GEN_SPACE_ROM, 384UL, 8UL, 1UL}" in art, "the ranges array gains the ROM entries")

        # ---- (3) violations fault ONLY the mod ----
        cases = (
            ("undeclared", "host->read(SEGA_GEN_SPACE_M68K_ROM, 0x110U, 1U, &a);", "SEGA_MOD_PHASE_RENDER", "range"),
            ("straddle", "host->read(SEGA_GEN_SPACE_M68K_ROM, 0x10EU, 4U, &a);", "SEGA_MOD_PHASE_RENDER", "range"),
            ("odd-word", "host->read(SEGA_GEN_SPACE_M68K_ROM, 0x101U, 2U, &a);", "SEGA_MOD_PHASE_RENDER", "alignment"),
            ("width", "host->read(SEGA_GEN_SPACE_M68K_ROM, 0x100U, 3U, &a);", "SEGA_MOD_PHASE_RENDER", "range"),
            ("phase", "host->read(SEGA_GEN_SPACE_M68K_ROM, 0x100U, 1U, &a);", "SEGA_MOD_PHASE_POST_INPUT", "phase"),
            ("budget", "for (i = 0U; i < 4097U; ++i) host->read(SEGA_GEN_SPACE_M68K_ROM, 0x100U, 1U, &a);", "SEGA_MOD_PHASE_RENDER", "budget"),
        )
        vanilla_ram = None
        for name, probe, phase, cause in cases:
            p, pid = packed("fault-" + name, probe=probe, phase=phase)
            r = h.build(rom, tmp / ("fault-" + name), p, pid, viewer=False, native=True)
            if not check(r.returncode == 0, f"fault-{name}: builds: {r.stdout[-300:]}{r.stderr[-300:]}"):
                continue
            _, lines = h.headless(tmp / ("fault-" + name), "fault-" + name, "10:1:0x00", frames="1", first="200")
            ms = lines["MOD_SUMMARY"][0]
            check(ms["faulted"] is True and f"cause={cause}" in ms["fault"], f"fault-{name}: stable cause {cause}: {ms['fault']}")
            check(lines["CAPTURE_SUMMARY"][0]["outcome"] == "incomplete_guest_stop", f"fault-{name}: the guest still runs to its own stop")
            ram = lines["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"]
            vanilla_ram = vanilla_ram or ram
            check(ram == vanilla_ram, f"fault-{name}: the guest state is independent of the fault")
        _, lines = h.headless(tmp / "ok", "ok-end", "10:1:0x00", frames="1", first="200")
        check(lines["CAPTURE_SUMMARY"][0]["final_work_ram_sha256"] == vanilla_ram, "ROM reads never change guest state")

        # ---- (4) a range past the effective image is refused at build ----
        p, pid = packed("past-end", ranges=[(len(image) - 4, 8)])
        out = tmp / "past-end"
        r = h.build(rom, out, p, pid, viewer=False, native=True)
        diag = h.status(out).get("diagnostic") if (out / "status.json").exists() else None
        check(r.returncode != 0 and not (out / "game").exists() and diag == "E_MEMORY_RANGE", f"a range past the image is E_MEMORY_RANGE at build: {diag}")

        # ---- (5) declaration rules ----
        def declared(what, code, text):
            d = tmp / ("decl-" + what)
            d.mkdir(exist_ok=True)
            (d / "src").mkdir(exist_ok=True)
            (d / "src/provider.c").write_text(PROVIDER.replace("PROBE_PHASE", "SEGA_MOD_PHASE_RENDER").replace("PROBE", OK_PROBE))
            (d / "mod.toml").write_text(text)
            r, doc, _ = pack(d)
            check(r.returncode != 0 and doc.get("class") == code, f"{what}: expected {code}, got {doc.get('class')} ({doc.get('error')})")

        good = manifest(image_sha, RANGES)
        declared("write-access", "E_MEMORY_ACCESS", good.replace('access = "read"', 'access = "write"', 1))
        declared("read-write-access", "E_MEMORY_ACCESS", good.replace('access = "read"', 'access = "read-write"', 1))
        declared("no-revision", "E_MEMORY_REVISION", good.replace('revision = "fixture"\nspace = "m68k.rom"', 'space = "m68k.rom"', 1))
        declared("empty", "E_MEMORY_RANGE", manifest(image_sha, [(0x100, 0)]))
        declared("budget", "E_MEMORY_ROM_BUDGET", manifest(image_sha, [(0x0, 2048), (0x1000, 2049)]))
        declared("no-capability", "E_CAPABILITY_INCONSISTENT", manifest(image_sha, RANGES, caps='"host.log"'))
        r, doc, _ = pack(project("limit", ranges=[(0x0, 2048), (0x1000, 2048)]))
        check(r.returncode == 0 and doc.get("ok"), f"exactly 4096 ROM bytes are accepted: {r.stdout[:200]}")

        # ---- (6) mod inspect lists the ranges and the total ----
        r = h.run_cli("mod", "inspect", "--json", pkg)
        doc = json.loads(r.stdout)
        rr = doc.get("rom_read", {})
        check(r.returncode == 0 and rr.get("limit_bytes") == 4096 and rr.get("ranges") == [{"revision": "fixture", "offset": 256, "length": 16}, {"revision": "fixture", "offset": 384, "length": 8}] and
              rr.get("totals") == [{"revision": "fixture", "bytes": 24}], f"mod inspect lists the ROM ranges and the total: {rr}")
        if h.failures:
            print(len(h.failures), "failure(s)")
            return 1
        print("mod_native_rom_read_test: ok")
        return 0


if __name__ == "__main__":
    sys.exit(main())
