#!/usr/bin/env python3
"""Bounded synthetic viewer gallery captures; no ROM or generated guest is executed.

Requires Pillow and a newly built sv_gallery. Comparisons contain illustrative kit
art: --comparisons MUST be in an ignored review directory, never published.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from PIL import Image, ImageDraw


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--gallery", type=Path, required=True)
    p.add_argument("--assets", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--references", type=Path)
    p.add_argument("--comparisons", type=Path)
    p.add_argument("--publish", type=Path, help="Publish only synthetic 1080x740/720x520 pages and input cases")
    a = p.parse_args()
    binary = a.gallery.resolve()
    if a.comparisons:
        check = subprocess.run(["git", "check-ignore", str(a.comparisons.resolve())], capture_output=True)
        if check.returncode:
            p.error("comparisons must be ignored by git")
    manifest = {"schema": 1, "command": [sys.executable, *sys.argv],
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "fixture": "project-authored fictional RGB scene; virtual controllers, never physical hardware",
                "hidpi": "output-pixel simulations at 2x logical sizes; not an OS/display HiDPI test",
                "runs": []}
    published = []
    for console in ("genesis", "sms", "sms224"):
        for size in ("720x520", "1080x740", "1440x1040", "2160x1480", "2560x1440"):
            for pad in ("none", "raw", "locked", "live") if size == "1080x740" else ("none",):
                out = a.out.resolve() / f"{console}-{size}-{pad}"
                out.mkdir(parents=True, exist_ok=True)
                env = dict(os.environ, SEGARECOMP_ASSETS=str(a.assets.resolve()),
                           SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1", SV_GALLERY_PAD=pad)
                command = [str(binary), str(out), console, size]
                run = subprocess.run(command, env=env, capture_output=True, timeout=30)
                if run.returncode:
                    raise RuntimeError(run.stderr.decode(errors="replace"))
                captures = []
                for source in sorted(out.glob("*.png")) + sorted(out.glob("*.bmp")):
                    image = Image.open(source).convert("RGB")
                    if image.size != tuple(map(int, size.split("x"))):
                        raise RuntimeError(f"wrong output size: {source}: {image.size}")
                    final = source.with_suffix(".png")
                    image.save(final)
                    if source.suffix == ".bmp":
                        source.unlink()
                    captures.append({"path": str(final.relative_to(a.out.resolve())),
                                     "sha256": hashlib.sha256(final.read_bytes()).hexdigest()})
                    if a.publish:
                        target = None
                        if pad == "none" and size in ("1080x740", "720x520"):
                            folder = "viewer-" + ("sms-224" if console == "sms224" else console)
                            target = a.publish / (folder if size == "1080x740" else folder + "-small") / final.name
                        if target:
                            target.parent.mkdir(parents=True, exist_ok=True)
                            image.save(target)
                            published.append({"path": str(target.relative_to(a.publish)), "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
                        if size == "1080x740" and final.stem == "05b_controller_status":
                            prefix = "viewer" if console == "genesis" else "viewer-" + console
                            target = a.publish / "input-diagnostics" / f"{prefix}_controller_status_{pad}.png"
                            target.parent.mkdir(parents=True, exist_ok=True)
                            image.save(target)
                            published.append({"path": str(target.relative_to(a.publish)), "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
                    reference = a.references / final.name if a.references else None
                    if reference and reference.exists() and a.comparisons and pad == "none":
                        concept = Image.open(reference).convert("RGB")
                        concept.thumbnail((1080, 740))
                        display = image.copy()
                        display.thumbnail((1080, 740))
                        sheet = Image.new("RGB", (max(concept.width, display.width), concept.height + display.height + 56), "#08142f")
                        draw = ImageDraw.Draw(sheet)
                        draw.text((8, 6), "KIT CONCEPT: illustrative only, never runtime art", fill="white")
                        sheet.paste(concept, (0, 24))
                        draw.text((8, concept.height + 30), f"NEW BINARY: {console} / {size} / fictional fixture", fill="white")
                        sheet.paste(display, (0, concept.height + 56))
                        a.comparisons.mkdir(parents=True, exist_ok=True)
                        sheet.save(a.comparisons / f"{console}-{size}-{final.name}")
                manifest["runs"].append({"command": command, "timeout_seconds": 30,
                                         "environment": {k: env[k] for k in ("SEGARECOMP_ASSETS", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "SV_GALLERY_PAD")},
                                         "captures": captures})
    (a.out / "viewer-capture.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if a.publish:
        public = {"schema": 1, "binary_sha256": manifest["binary_sha256"], "fixture": manifest["fixture"], "hidpi": manifest["hidpi"],
                  "reproduce": "python3 tools/viewer_visual_capture.py --gallery build/dev/libs/viewer/sv_gallery --assets apps/segarecomp-launcher/assets --out build/visual-review/viewer --publish docs/product/screenshots",
                  "runs": len(manifest["runs"]), "total_review_screenshots": sum(len(r["captures"]) for r in manifest["runs"]),
                  "captures": published}
        (a.publish / "viewer-capture.json").write_text(json.dumps(public, indent=2) + "\n")
    print(f"{len(manifest['runs'])} bounded runs; {sum(len(r['captures']) for r in manifest['runs'])} screenshots")


if __name__ == "__main__":
    main()
