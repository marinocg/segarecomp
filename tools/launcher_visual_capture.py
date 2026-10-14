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


PAGES = [
    ("01_library_home", "library"), ("02_library_list", "list"),
    ("03_game_details", "details:0"), ("04_import_empty_results", "import"),
    ("05_storage_cleanup_dialog", "storage-modal"), ("06_storage", "storage"),
    ("07_settings_library", "settings0"), ("08_settings_metadata", "settings1"),
    ("09_settings_build_runtime", "settings2"), ("10_settings_video_controls", "settings3"),
    ("11_input_diagnostics", "settings4"), ("12_settings_about", "settings5"),
    ("13_library_select", "library"),
    ("14_no_cover_details", "details:2"),
    ("15_settings_library_lower", "settings0"), ("16_settings_video_lower", "settings3"),
    ("17_storage_lower", "storage"), ("18_details_lower", "details:0"),
    ("19_metadata_lower", "settings1"), ("20_about_lower", "settings5"),
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
        for name, page in PAGES:
            bmp = out / (name + ".bmp")
            env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy",
                       SEGARECOMP_HOME=str(a.home.resolve()), SEGARECOMP_CACHE_DIR=str(a.fixture.resolve() / "cache"),
                       SEGARECOMP_LIBRARY_DIR=str(a.fixture.resolve() / "cache" / "library"),
                       SEGARECOMP_SHOT=str(bmp.resolve()), SEGARECOMP_SHOT_PAGE=page, SEGARECOMP_SHOT_SIZE=size,
                       SEGARECOMP_SHOT_FRAMES="24")
            # Clear inherited review actions: capture must not build, play or modify storage.
            for key in ["SEGARECOMP_SHOT_BUILD", "SEGARECOMP_SHOT_ROM", "SEGARECOMP_GUI_AUTOPLAY", "SEGARECOMP_GUI_SCRIPT", "SEGARECOMP_SHOT_SELECT", "SEGARECOMP_SHOT_DROP", "SEGARECOMP_SHOT_SCROLL"]:
                env.pop(key, None)
            if name.endswith("_lower"):
                env["SEGARECOMP_SHOT_SCROLL"] = "bottom"
            if name == "13_library_select" or page == "storage":
                env["SEGARECOMP_SHOT_SELECT"] = "1"
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
    (a.out / "launcher-capture.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Captured {len(metadata['captures'])} screens; binary SHA-256 {metadata['binary_sha256']}")


if __name__ == "__main__":
    main()
