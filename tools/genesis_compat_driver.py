#!/usr/bin/env python3
"""Headless Genesis compatibility driver (SEG-068-T007).

One built game, one guest execution: the driver builds the ROM through the existing product route (`segarecomp build`), runs the
resulting executable once with a deterministic frame-keyed pad script, captures the chosen frames through the existing headless frame
capture and reports the existing sound summary. It re-implements nothing: rendering, PPM export, frame digests, the mixer, the PCM
digest and `SOUND_SUMMARY` all come from the generated program's runtime (platforms/genesis/runtime, platforms/genesis/viewer). What
this tool adds is host-side only: environment plumbing, PPM -> PNG conversion, a contact sheet and an aggregate-only report.

Everything it writes stays under one output directory that must be ignored by Git (default `build/compat-driver`); the report printed
to stdout and written to `report.json` contains aggregates and classifications only (frames reached, stop class, non-silence, digests),
never a ROM name, path, address or byte. Frames and contact sheets are local diagnostic material and are never committed.

Subcommands:
  run     build (unless --exe is given), run once, capture frames, convert, report
  matrix  run a list of games from a JSON file and emit the sanitized compatibility matrix

Pad script (`--pad-script`): "<frame>:<player>:<mask>[,...]" with a virtual frame number, player 1 or 2 and a button mask
(decimal or 0x-hex; bits as GENESIS_PAD_* in platforms/genesis/runtime/runtime.h: UP 1, DOWN 2, LEFT 4, RIGHT 8, B 16, C 32, A 64,
START 128). Entries are applied only between runner slices, at the first slice boundary at or after the keyed frame; see
platforms/genesis/viewer/frame_capture.h for the exact rule. Example (hold START from frame 120, release at 140):
  --pad-script 120:1:0x80,140:1:0

Classifying where a title ends: the capture observes only until its window is complete (it never raises the runner allowance), so a
late window (for example `--capture-first 3000 --capture-count 1`) turns the run into a "how far does it get" probe: the capture
outcome then reports a guest stop (with the stop classification and the frames reached) or an exhausted budget instead of frames.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAX_FRAMES = 64
SCRIPT_ENTRY = re.compile(r"^[0-9]+:[12]:(0[xX][0-9a-fA-F]+|[0-9]+)$")  # hex needs digits; "0x" alone is malformed


class DriverError(Exception):
    pass


def parse_mask(text: str) -> int:
    """Documented grammar: unprefixed = decimal (leading zeroes allowed, never octal), 0x/0X = hexadecimal."""
    if text[:2] in ("0x", "0X"):
        return int(text[2:], 16)
    return int(text, 10)


def validate_pad_script(text: str) -> str:
    """The same grammar the generated program re-validates (genesis_frame_capture_run_from_environment)."""
    if text == "":
        return text
    previous = -1
    for entry in text.split(","):
        if not SCRIPT_ENTRY.match(entry):
            raise DriverError(f"malformed pad-script entry {entry!r} (expected <frame>:<1|2>:<mask>)")
        frame, _, mask = entry.split(":")
        if parse_mask(mask) > 255:
            raise DriverError(f"pad mask above 255 in {entry!r}")
        if int(frame) < previous:
            raise DriverError("pad-script frames must be non-decreasing")
        previous = int(frame)
    return text


def ensure_ignored(directory: pathlib.Path) -> None:
    """Outputs may only go outside the repository or under a Git-ignored directory (frames derived from a commercial image must
    never be commit-able)."""
    try:
        directory.resolve().relative_to(ROOT)
    except ValueError:
        return  # outside the repository: not committable
    result = subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-q", str(directory.resolve() / "x")], capture_output=True)
    if result.returncode != 0:
        raise DriverError(f"output directory {directory} is inside the repository but not ignored by Git; use build/ or an ignored path")


def build_game(args, out_dir: pathlib.Path) -> pathlib.Path:
    build_dir = out_dir / "build"
    build_dir.mkdir(parents=True, exist_ok=True)
    command = [str(args.segarecomp), "build", "--rom", str(args.rom), "--output", str(build_dir), "--cc", args.cc,
               "--runtime-dir", str(args.runtime_dir)]
    if args.cxx:
        command += ["--cxx", args.cxx]
    if args.jobs:
        command += ["--jobs", str(args.jobs)]
    done = subprocess.run(command, text=True, capture_output=True, timeout=args.build_timeout)
    if done.returncode != 0:
        raise DriverError("build failed: " + (re.findall(r"@result failed stage=(\S+)", done.stdout) or ["unknown"])[0])
    executable = next((p for p in build_dir.iterdir() if p.stem == "game" and p.is_file()), None)
    if executable is None:
        raise DriverError("build produced no executable")
    return executable


def read_ppm(path: pathlib.Path):
    data = path.read_bytes()
    match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if match is None:
        raise DriverError("unexpected PPM header")
    width, height = int(match.group(1)), int(match.group(2))
    pixels = data[match.end():]
    if len(pixels) != width * height * 3:
        raise DriverError("PPM size mismatch")
    return width, height, pixels


def write_png(path: pathlib.Path, width: int, height: int, rgb: bytes) -> None:
    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    rows = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def contact_sheet(frames, columns: int):
    """Tiles equally sized frames left to right, top to bottom (pure host-side presentation of already-exported frames)."""
    width, height = frames[0][0], frames[0][1]
    columns = max(1, min(columns, len(frames)))
    rows = (len(frames) + columns - 1) // columns
    sheet_w, sheet_h = columns * width, rows * height
    canvas = bytearray(sheet_w * sheet_h * 3)
    for index, (fw, fh, pixels) in enumerate(frames):
        if (fw, fh) != (width, height):
            continue  # a frame of another size (mode change) is skipped, not rescaled
        ox, oy = (index % columns) * width, (index // columns) * height
        for y in range(height):
            start = ((oy + y) * sheet_w + ox) * 3
            canvas[start:start + width * 3] = pixels[y * width * 3:(y + 1) * width * 3]
    return sheet_w, sheet_h, bytes(canvas)


def parse_summaries(stderr: str):
    capture = re.search(r"CAPTURE_SUMMARY (\{.*\})", stderr)
    sound = re.search(r"SOUND_SUMMARY (\{.*\})", stderr)
    return (json.loads(capture.group(1)) if capture else None), (json.loads(sound.group(1)) if sound else None)


def run_game(args) -> dict:
    out_dir = pathlib.Path(args.out_dir).resolve()
    ensure_ignored(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    pad_script = validate_pad_script(args.pad_script or "")
    if not 1 <= args.capture_count <= MAX_FRAMES:
        raise DriverError(f"--capture-count must be 1..{MAX_FRAMES}")
    if args.budget <= 0:
        raise DriverError("--budget must be a positive instruction budget (a generated program is never run unbounded)")
    existing = next(iter(sorted((out_dir / "build").glob("game*"))), None) if getattr(args, "reuse_build", False) else None
    if args.exe:
        executable = pathlib.Path(args.exe).resolve()
    elif existing is not None and existing.is_file():
        executable = existing  # --reuse-build: the previous build of this output directory (same ROM, same product revision)
    else:
        executable = build_game(args, out_dir)
    frames_dir = out_dir / "frames"
    if frames_dir.exists():
        for old in frames_dir.glob("frame-*.ppm"):
            old.unlink()
    frames_dir.mkdir(exist_ok=True)
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "SEGARECOMP_CAPTURE_DIR": str(frames_dir),
           "SEGARECOMP_CAPTURE_FIRST": str(args.capture_first), "SEGARECOMP_CAPTURE_COUNT": str(args.capture_count),
           "SEGARECOMP_CAPTURE_STRIDE": str(args.capture_stride), "SEGARECOMP_CAPTURE_SLICE": str(args.capture_slice),
           "SEGARECOMP_SOUND_SUMMARY": "1"}
    if pad_script:
        env["SEGARECOMP_PAD_SCRIPT"] = pad_script
    if args.pcm_out:
        env["SEGARECOMP_AUDIO_PCM_OUT"] = str(pathlib.Path(args.pcm_out).resolve())
    done = subprocess.run([str(executable), "--instruction-budget", str(args.budget)], text=True, capture_output=True,
                          timeout=args.run_timeout, env=env)
    capture, sound = parse_summaries(done.stderr)
    try:
        sanitized = json.loads(done.stdout)
    except json.JSONDecodeError:
        sanitized = {}
    ppms = sorted(frames_dir.glob("frame-*.ppm"))
    if args.png or args.contact_sheet:
        decoded = []
        for ppm in ppms:
            width, height, pixels = read_ppm(ppm)
            decoded.append((width, height, pixels))
            if args.png:
                write_png(ppm.with_suffix(".png"), width, height, pixels)
        if args.contact_sheet and decoded:
            write_png(out_dir / "contact-sheet.png", *contact_sheet(decoded, args.columns))
    audio = (sound or {}).get("audio", {})
    report = {
        "schema": 1,
        "result": sanitized.get("result"),
        "stop_class": sanitized.get("stop_class"),
        "diagnostic_category": sanitized.get("diagnostic_category"),
        "exit_status": done.returncode,
        "capture": None if capture is None else {
            "outcome": capture["outcome"], "frames_captured": capture["frames_captured"],
            "frames_published": capture["frames_published"], "dispatches": capture["dispatches"],
            "pad_entries_applied": capture.get("pad_entries_applied", 0), "frame_digests": capture["digests"]},
        "sound": None if sound is None else {
            "non_silent": audio.get("non_silent"), "frames": audio.get("frames"), "sha256": audio.get("sha256"),
            "fault": audio.get("fault"), "virtual_frames": sound.get("virtual_frames")},
    }
    (out_dir / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def add_run_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--segarecomp", type=pathlib.Path, default=ROOT / "build" / "dev" / "apps" / "segarecomp" / "segarecomp")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--runtime-dir", type=pathlib.Path, default=ROOT / "platforms" / "genesis")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--build-timeout", type=int, default=1800)
    parser.add_argument("--run-timeout", type=int, default=900, help="wall-clock limit for the single guest execution (seconds)")
    parser.add_argument("--budget", type=int, default=400_000_000, help="finite --instruction-budget for the run")
    parser.add_argument("--capture-first", type=int, default=1)
    parser.add_argument("--capture-count", type=int, default=8)
    parser.add_argument("--capture-stride", type=int, default=1)
    parser.add_argument("--capture-slice", type=int, default=65536, help="dispatches per runner slice (pad-script granularity)")
    parser.add_argument("--pad-script", default="")
    parser.add_argument("--pcm-out", default="")
    parser.add_argument("--reuse-build", action="store_true", help="reuse <out-dir>/build/game from an earlier run instead of rebuilding")
    parser.add_argument("--png", action="store_true", help="also write a PNG next to every captured PPM")
    parser.add_argument("--contact-sheet", action="store_true")
    parser.add_argument("--columns", type=int, default=4)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="build, run once and report")
    run.add_argument("--rom", type=pathlib.Path)
    run.add_argument("--exe", type=pathlib.Path, help="an already built game (skips the build)")
    run.add_argument("--out-dir", default=str(ROOT / "build" / "compat-driver"))
    add_run_arguments(run)
    matrix = sub.add_parser("matrix", help="run a JSON list of games and print the sanitized matrix")
    matrix.add_argument("--games", type=pathlib.Path, required=True,
                        help='JSON list of {"label": str, "rom": path, "pad_script": str (optional), "capture_count": int (optional)}')
    matrix.add_argument("--out-dir", default=str(ROOT / "build" / "compat-matrix"))
    add_run_arguments(matrix)
    args = parser.parse_args(argv)
    try:
        if args.command == "run":
            if args.rom is None and args.exe is None:
                raise DriverError("--rom or --exe is required")
            print(json.dumps(run_game(args), indent=2, sort_keys=True))
            return 0
        games = json.loads(args.games.read_text(encoding="utf-8"))
        rows = []
        for game in games:
            single = argparse.Namespace(**vars(args))
            single.rom = pathlib.Path(game["rom"])
            single.exe = None
            single.out_dir = str(pathlib.Path(args.out_dir) / re.sub(r"[^A-Za-z0-9_.-]", "_", game["label"]))
            single.pad_script = game.get("pad_script", args.pad_script)
            single.capture_count = game.get("capture_count", args.capture_count)
            try:
                report = run_game(single)
                capture = report["capture"] or {}
                rows.append({"label": game["label"], "build": "ok", "result": report["result"], "stop_class": report["stop_class"],
                             "diagnostic_category": report["diagnostic_category"],
                             "capture_outcome": capture.get("outcome"),
                             "frames_captured": capture.get("frames_captured"), "frames_published": capture.get("frames_published"),
                             "non_silent": (report["sound"] or {}).get("non_silent")})
            except DriverError as error:
                rows.append({"label": game["label"], "build": str(error), "result": None, "stop_class": None,
                             "diagnostic_category": None, "capture_outcome": None, "frames_captured": None, "frames_published": None, "non_silent": None})
        print(json.dumps({"schema": 1, "rows": rows}, indent=2, sort_keys=True))
        return 0
    except DriverError as error:
        print(f"genesis_compat_driver: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
