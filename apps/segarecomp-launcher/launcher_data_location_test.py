#!/usr/bin/env python3
"""SEG-150-T008 (ADR 0317): startup honours the data-location bootstrap through the real headless launcher.

Checks: without a bootstrap the default root is used (an existing profile upgrades with nothing moved); a bootstrap names the active data root
(<root>/library/library.json); a bootstrap whose root is unavailable falls back to the default and creates nothing; SEGARECOMP_CACHE_DIR beats the
bootstrap; a damaged bootstrap falls back to the default.

usage: launcher_data_location_test.py <Segarecomp> <fake_segarecomp>
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def make_package(root, fake_cli):
    for rel in ("toolchain/zig" + EXE_SUFFIX, "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c",
                "sdl3/include/SDL3/SDL.h"):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")
    (root / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy(fake_cli, root / "bin" / ("segarecomp" + EXE_SUFFIX))


def genesis_rom(path, tag):
    image = bytearray(0x200)
    image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
    image[0x100:0x110] = b"SEGA GENESIS    "
    image[0x180] = tag
    path.write_bytes(bytes(image))


def catalog_titles(root):
    file = pathlib.Path(root) / "library" / "library.json"
    if not file.exists():
        return None
    return sorted(e["detected_title"] for e in json.loads(file.read_text())["games"])


def main():
    exe, fake_cli = sys.argv[1], sys.argv[2]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="segarecomp_dataloc_")).resolve()
    try:
        package = tmp / "package"
        make_package(package, fake_cli)
        data_home = tmp / "home"
        data_home.mkdir()
        env = {k: v for k, v in os.environ.items() if k not in ("SEGARECOMP_CACHE_DIR", "SEGARECOMP_LIBRARY_DIR")}
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SEGARECOMP_HOME=str(package), SEGARECOMP_NO_COVER_FETCH="1",
                   HOME=str(data_home), XDG_DATA_HOME=str(data_home / "xdg"), LOCALAPPDATA=str(data_home / "local"),
                   SEGARECOMP_DATA_BOOTSTRAP=str(tmp / "pointer" / "location.json"))
        default_root = (data_home / ("local" if os.name == "nt" else "xdg")) / "Segarecomp"
        if sys.platform == "darwin":
            default_root = data_home / "Library" / "Application Support" / "Segarecomp"
        bootstrap = pathlib.Path(env["SEGARECOMP_DATA_BOOTSTRAP"])

        def add(name, tag, extra=None):
            rom = tmp / (name + ".md")
            genesis_rom(rom, tag)
            done = subprocess.run([exe, "--import", str(rom)], env=dict(env, **(extra or {})), timeout=60, capture_output=True, text=True)
            assert "import=added" in done.stdout, done.stdout + done.stderr

        # 1. no bootstrap: today's defaults
        add("alpha", 1)
        assert catalog_titles(default_root) == ["alpha"], catalog_titles(default_root)
        assert not bootstrap.exists() and not bootstrap.parent.exists()

        # 2. a bootstrap names the active root
        new_root = tmp / "moved"
        new_root.mkdir()
        bootstrap.parent.mkdir()
        bootstrap.write_text(json.dumps({"schema": 1, "root": str(new_root)}))
        add("beta", 2)
        assert catalog_titles(new_root) == ["beta"], catalog_titles(new_root)
        assert catalog_titles(default_root) == ["alpha"], "the old root must not change"

        # 3. an unavailable root falls back to the default and creates nothing at the missing location
        gone = tmp / "unplugged" / "drive"
        bootstrap.write_text(json.dumps({"schema": 1, "root": str(gone)}))
        add("gamma", 3)
        assert catalog_titles(default_root) == ["alpha", "gamma"] and not gone.exists() and not gone.parent.exists()

        # 4. the environment variable beats the bootstrap
        bootstrap.write_text(json.dumps({"schema": 1, "root": str(new_root)}))
        env_root = tmp / "envroot"
        add("delta", 4, {"SEGARECOMP_CACHE_DIR": str(env_root)})
        assert catalog_titles(env_root) == ["delta"] and catalog_titles(new_root) == ["beta"]

        # 5. a damaged bootstrap falls back to the default
        bootstrap.write_text("{ not json")
        add("epsilon", 5)
        assert catalog_titles(default_root) == ["alpha", "epsilon", "gamma"], catalog_titles(default_root)
        assert bootstrap.read_text() == "{ not json", "a damaged bootstrap is left for inspection, never rewritten"
        print("launcher_data_location_test: ok")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


main()
