#!/usr/bin/env python3
"""The recompiler contract layer must not import Genesis routing/device code."""
import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
FORBIDDEN = (
    "m68k_pipeline.hpp",
    "M68kFrontendProfile",
    "M68kStartupIngress",
    "GenesisFrontierClass",
    "ControllerIo",
    "controller_io",
    "m68k_route_genesis_device_access",
    "m68k_controller_io_access",
    "genesis_route_access",
)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def main() -> None:
    offenders = []
    for root in (ROOT / "libs/recompiler/include/segarecomp/recompiler", ROOT / "libs/recompiler/src"):
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            code = strip_comments(path.read_text(encoding="utf-8"))
            for token in FORBIDDEN:
                if re.search(r"\b" + re.escape(token) + r"\b", code):
                    offenders.append((str(path.relative_to(ROOT)), token))
    assert not offenders, f"recompiler must not depend on Genesis policy: {offenders}"
    # SEG-018-T003: the generic recompiler owns no MC68000 include, type or link.
    m68k = []
    for path in sorted((ROOT / "libs/recompiler").rglob("*")):
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        if path.name == "CMakeLists.txt":
            text = re.sub(r"#[^\n]*", "", text)
            if "cpu_m68k" in text:
                m68k.append((str(path.relative_to(ROOT)), "cpu_m68k"))
            continue
        code = strip_comments(text)
        if re.search(r"segarecomp/cpu/m68k|\bM68k\w*|\bm68k_\w*", code):
            m68k.append((str(path.relative_to(ROOT)), "m68k"))
    assert not m68k, f"recompiler must be CPU-neutral (no MC68000 ownership): {m68k}"
    print("recompiler no-Genesis-policy dependency test: ok")


if __name__ == "__main__":
    main()
