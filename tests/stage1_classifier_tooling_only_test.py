#!/usr/bin/env python3
"""SEG-007-T007: the stage-1 classifier must never be imported by production code.

Asserts no file under ``src/`` or ``include/`` references ``tools.inventory`` or
``stage1_classifier`` in any form (import statement, string reference, comment). Fully
synthetic; requires no commercial ROM and no local Musashi checkout.
"""
import pathlib
import sys

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
NEEDLES = ("tools.inventory", "stage1_classifier", "tools/inventory")


def main() -> None:
    offenders = []
    for directory in ("src", "include"):
        root = PROJECT_ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for needle in NEEDLES:
                if needle in text:
                    offenders.append((str(path.relative_to(PROJECT_ROOT)), needle))

    assert not offenders, f"stage-1 classifier must be tooling-only, found: {offenders}"
    print("stage1 classifier tooling-only test: ok")


if __name__ == "__main__":
    main()
    sys.exit(0)
