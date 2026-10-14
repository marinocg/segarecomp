#!/usr/bin/env python3
"""Capture the real launcher at both review sizes. Requires Pillow, a built launcher and a synthetic launcher_ui_fixture.py library.

No guest program is executed. Optional concept comparisons are local review material only:
keep --references and --comparisons outside the product tree. Never use commercial input.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw


# (file name, SEGARECOMP_SHOT_PAGE, extra review-aid environment). The fixture's visible order (last played first): 0 Fictional Quest (Genesis,
# Compatibility, ready), 1 Neon Harbor Demo (Optimized selected after a Compatibility build), 2 Pixel Orchard (built Optimized), 3 Coastal Drift
# (Master System, ready), 4 Broken Vector (a real failed build), 5 Mystery Cart (Master System, mapper not declared), 6 Summit Chase (metadata only,
# its ROM moved away: the missing-ROM recovery).
PAGES = [
    ("01_library_home", "library", {}), ("02_library_list", "list", {}),
    ("03_game_details", "details:0", {}), ("03b_details_genesis_optimized", "details:2", {}),
    ("03c_details_mode_switch", "details:1", {}), ("03d_details_sms_mapper", "details:5", {}),
    ("03e_details_metadata_only", "details:6", {}), ("03f_details_failed", "details:4", {}),
    ("03g_details_failed_log_lower", "details:4", {"SEGARECOMP_SHOT_LOG": "1", "SEGARECOMP_SHOT_SCROLL": "bottom"}),
    ("03h_details_sms_ready", "details:3", {}),
    ("04_import_empty_results", "import", {}),
    ("05_storage_cleanup_dialog", "storage-modal", {}), ("06_storage", "storage", {"SEGARECOMP_SHOT_SELECT": "1"}),
    ("07_settings_library", "settings0", {}), ("08_settings_metadata", "settings1", {}),
    ("09_settings_build_runtime", "settings2", {}), ("10_settings_video_controls", "settings3", {}),
    ("11_input_diagnostics", "settings4", {}), ("12_settings_about", "settings5", {}),
    ("13_library_select", "library", {"SEGARECOMP_SHOT_SELECT": "1"}),
    ("13b_library_context_menu", "library", {"SEGARECOMP_SHOT_MENU": "1"}),
    ("15_settings_library_lower", "settings0", {"SEGARECOMP_SHOT_SCROLL": "bottom"}), ("16_settings_video_lower", "settings3", {"SEGARECOMP_SHOT_SCROLL": "bottom"}),
    ("17_storage_lower", "storage", {"SEGARECOMP_SHOT_SELECT": "1", "SEGARECOMP_SHOT_SCROLL": "bottom"}),
    ("18_details_lower", "details:0", {"SEGARECOMP_SHOT_SCROLL": "bottom"}),
    ("19_metadata_lower", "settings1", {"SEGARECOMP_SHOT_SCROLL": "bottom"}), ("20_about_lower", "settings5", {"SEGARECOMP_SHOT_SCROLL": "bottom"}),
]
REFERENCES = {"01_library_home": "01_library_home", "03_game_details": "03_game_details",
              "04_import_empty_results": "02_import_empty", "05_storage_cleanup_dialog": "07_bulk_cleanup",
              "06_storage": "04_storage_management", "07_settings_library": "05_library_settings",
              "09_settings_build_runtime": "06_build_runtime_settings"}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--launcher", type=Path, required=True)
    p.add_argument("--home", type=Path, required=True)
    p.add_argument("--fixture", type=Path, required=True, help="Existing synthetic fixture directory containing cache/")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--references", type=Path)
    p.add_argument("--comparisons", type=Path)
    p.add_argument("--build-rom", type=Path, help="Optional project-authored synthetic cartridge: capture a real build in a temporary cache (never runs it)")
    a = p.parse_args()
    binary = a.launcher.resolve()
    metadata = {"binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "captures": []}
    for size, directory in [("1080x740", "launcher"), ("720x520", "launcher-small")]:
        out = a.out / directory
        out.mkdir(parents=True, exist_ok=True)
        for name, page, extra in PAGES:
            bmp = out / (name + ".bmp")
            env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy",
                       SEGARECOMP_HOME=str(a.home.resolve()), SEGARECOMP_CACHE_DIR=str(a.fixture.resolve() / "cache"),
                       SEGARECOMP_LIBRARY_DIR=str(a.fixture.resolve() / "cache" / "library"),
                       SEGARECOMP_SHOT=str(bmp.resolve()), SEGARECOMP_SHOT_PAGE=page, SEGARECOMP_SHOT_SIZE=size,
                       SEGARECOMP_SHOT_FRAMES="24")
            # Clear inherited review actions: capture must not build, play or modify storage.
            for key in ["SEGARECOMP_SHOT_BUILD", "SEGARECOMP_SHOT_ROM", "SEGARECOMP_GUI_AUTOPLAY", "SEGARECOMP_GUI_SCRIPT", "SEGARECOMP_SHOT_SELECT", "SEGARECOMP_SHOT_DROP",
                        "SEGARECOMP_SHOT_SCROLL", "SEGARECOMP_SHOT_LOG", "SEGARECOMP_SHOT_MENU", "SEGARECOMP_SHOT_SCALE"]:
                env.pop(key, None)
            env.update(extra)

            result = subprocess.run([str(binary)], env=env, capture_output=True, timeout=60)
            if result.returncode:
                raise RuntimeError(result.stderr.decode(errors="replace"))
            image = Image.open(bmp).convert("RGB")
            assert image.size == tuple(map(int, size.split("x"))), image.size
            assert image.getextrema() != ((0, 0), (0, 0), (0, 0)), "Empty capture"
            image.save(out / (name + ".png"))
            bmp.unlink()
            metadata["captures"].append(f"{directory}/{name}.png")
            if a.references and a.comparisons and name in REFERENCES:
                reference = Image.open(a.references / (REFERENCES[name] + ".png")).convert("RGB")
                reference.thumbnail((1080, 740))
                sheet = Image.new("RGB", (max(reference.width, image.width), reference.height + image.height + 56), "#08142f")
                draw = ImageDraw.Draw(sheet)
                draw.text((10, 6), "KIT CONCEPT (illustrative only, not runtime art)", fill="white")
                sheet.paste(reference, (0, 24))
                draw.text((10, reference.height + 30), f"REBUILT LAUNCHER / synthetic fixture / {size}", fill="white")
                sheet.paste(image, (0, reference.height + 56))
                a.comparisons.mkdir(parents=True, exist_ok=True)
                sheet.save(a.comparisons / f"{size}-{name}.png")
        with tempfile.TemporaryDirectory(prefix="segarecomp-import-capture-") as scratch:  # import results on a throw-away copy of the fixture
            import shutil
            shutil.copytree(a.fixture.resolve() / "cache", Path(scratch) / "cache", ignore=shutil.ignore_patterns("zig-cache", "obj", "generated*"))
            fresh = Path(scratch) / "Harbor Lights (Fictional).md"
            image = bytearray(0x200)
            image[:12] = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))
            image[0x100:0x110] = b"SEGA GENESIS    "
            image[0x120:0x130] = b"HARBOR LIGHTS   "
            image[0x1E0] = 77
            fresh.write_bytes(bytes(image))
            refused = Path(scratch) / "notes.txt"
            refused.write_text("not a cartridge\n")
            duplicate = next((a.fixture.resolve() / "roms").glob("Fictional Quest*.md"))
            bmp = out / "04b_import_results.bmp"
            env.update(SEGARECOMP_CACHE_DIR=str(Path(scratch) / "cache"), SEGARECOMP_LIBRARY_DIR=str(Path(scratch) / "cache" / "library"),
                       SEGARECOMP_SHOT_PAGE="import", SEGARECOMP_SHOT=str(bmp.resolve()), SEGARECOMP_SHOT_DROP=";".join(str(x) for x in (fresh, duplicate, refused)))
            for key in extra:
                env.pop(key, None)
            subprocess.run([str(binary)], env=env, capture_output=True, timeout=60, check=True)
            env.pop("SEGARECOMP_SHOT_DROP", None)
            Image.open(bmp).convert("RGB").save(out / "04b_import_results.png")
            bmp.unlink()
            metadata["captures"].append(f"{directory}/04b_import_results.png")
        with tempfile.TemporaryDirectory(prefix="segarecomp-empty-capture-") as scratch:  # a first start: the empty library
            bmp = out / "22_empty_library.bmp"
            env.update(SEGARECOMP_CACHE_DIR=str(Path(scratch) / "cache"), SEGARECOMP_LIBRARY_DIR=str(Path(scratch) / "cache" / "library"),
                       SEGARECOMP_SHOT_PAGE="library", SEGARECOMP_SHOT=str(bmp.resolve()))
            for key in extra:
                env.pop(key, None)
            subprocess.run([str(binary)], env=env, capture_output=True, timeout=60, check=True)
            Image.open(bmp).convert("RGB").save(out / "22_empty_library.png")
            bmp.unlink()
            metadata["captures"].append(f"{directory}/22_empty_library.png")
        if a.build_rom:
            with tempfile.TemporaryDirectory(prefix="segarecomp-build-capture-") as scratch:
                bmp = out / "21_build_progress.bmp"
                env.update(SEGARECOMP_CACHE_DIR=str(Path(scratch) / "cache"), SEGARECOMP_SHOT_ROM=str(a.build_rom.resolve()),
                           SEGARECOMP_LIBRARY_DIR=str(Path(scratch) / "cache" / "library"),
                           SEGARECOMP_SHOT_BUILD="1", SEGARECOMP_SHOT_DURING_BUILD="1", SEGARECOMP_SHOT_FRAMES="2",
                           SEGARECOMP_SHOT_PAGE="details:0", SEGARECOMP_SHOT=str(bmp.resolve()))
                env.pop("SEGARECOMP_SHOT_SCROLL", None)
                env.pop("SEGARECOMP_SHOT_SELECT", None)
                subprocess.run([str(binary)], env=env, capture_output=True, timeout=180, check=True)
                Image.open(bmp).convert("RGB").save(out / "21_build_progress.png")
                bmp.unlink()
                metadata["captures"].append(f"{directory}/21_build_progress.png")
    # HiDPI simulation: the same logical 1080x740 layout rendered at a forced 2x UI scale into 2160x1480 output pixels (not an OS display test)
    out = a.out / "launcher-hidpi"
    out.mkdir(parents=True, exist_ok=True)
    for name, page, extra in [("01_library_home", "library", {}), ("03_game_details", "details:0", {}), ("07_settings_library", "settings0", {})]:
        bmp = out / (name + ".bmp")
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy",
                   SEGARECOMP_HOME=str(a.home.resolve()), SEGARECOMP_CACHE_DIR=str(a.fixture.resolve() / "cache"),
                   SEGARECOMP_LIBRARY_DIR=str(a.fixture.resolve() / "cache" / "library"), SEGARECOMP_SHOT=str(bmp.resolve()), SEGARECOMP_SHOT_PAGE=page,
                   SEGARECOMP_SHOT_SIZE="2160x1480", SEGARECOMP_SHOT_SCALE="2", SEGARECOMP_SHOT_FRAMES="24")
        for key in ["SEGARECOMP_SHOT_BUILD", "SEGARECOMP_SHOT_ROM", "SEGARECOMP_SHOT_SELECT", "SEGARECOMP_SHOT_SCROLL", "SEGARECOMP_SHOT_LOG", "SEGARECOMP_SHOT_MENU"]:
            env.pop(key, None)
        subprocess.run([str(binary)], env=env, capture_output=True, timeout=60, check=True)
        image = Image.open(bmp).convert("RGB")
        assert image.size == (2160, 1480), image.size
        image.save(out / (name + ".png"))
        bmp.unlink()
        metadata["captures"].append(f"launcher-hidpi/{name}.png")
    (a.out / "launcher-capture.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Captured {len(metadata['captures'])} screens; binary SHA-256 {metadata['binary_sha256']}")


if __name__ == "__main__":
    main()
