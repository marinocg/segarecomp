#!/usr/bin/env python3
"""Resolve the one canonical, worktree-independent, ignored location for a
ROM-hash-bound private external-analysis-hints file (ADR-0023).

SEG-007-T164/T165 established the ADR-0023 opt-in external-hints contract
(``--external-hints <path>``) and, for the authorized pinned Sonic ROM,
adopted a corrected annotation as this task's own authorized *assisted*
production input. That concrete, commercial-derived-scalar-bearing record
must never be committed (``docs/testing/commercial-games.md``), and must
never be read implicitly by ordinary raw-ROM compilation.

Development may run each task in its own linked ``git worktree``. A path relative to one task's worktree
(for example a bare ``.cache/sonic-external-hints.json``) is not a durable,
worktree-independent input contract for a later task's own separate
worktree. This tool resolves the exact same canonical path from *any*
linked worktree of this repository, mirroring ``tools/ghidra.py``'s own
``_main_worktree_root()`` technique: every linked worktree shares one
``git rev-parse --git-common-dir`` common repo, so deriving the canonical
root from that shared location (not ``__file__``, which resolves inside
whichever worktree happens to be running this tool) gives every task the
identical answer for the identical ROM.

This tool only *resolves* the path; it never reads, writes, or trusts the
file's content, and it never makes any caller depend on the file's
existence. A caller still passes the resolved path to
``--external-hints`` explicitly -- this remains an authorized *assisted*
route, never an implicit default for ordinary raw-ROM recompilation.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]


def main_worktree_root() -> pathlib.Path:
    """Resolve the top-level of the main working tree (the Git common repo root).

    Identical technique to ``tools/ghidra.py``'s own ``_main_worktree_root()``:
    every linked ``git worktree`` shares one ``.git`` common directory, so
    deriving the root from ``git rev-parse --git-common-dir`` (not
    ``__file__``, which is worktree-local) gives every linked worktree the
    same canonical answer. Falls back to this file's own repo root when git
    is unavailable or the resolved path cannot be confirmed.
    """

    try:
        result = subprocess.run(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            check=True, text=True, capture_output=True, cwd=str(PROJECT_ROOT),
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return PROJECT_ROOT
    common_dir = pathlib.Path(result.stdout.strip())
    if not common_dir.is_absolute() or not common_dir.exists():
        return PROJECT_ROOT
    # <main-worktree>/.git  ->  <main-worktree>
    return common_dir.parent


def analysis_hints_path(rom_sha256: str) -> pathlib.Path:
    """The one canonical, ignored, per-ROM private analysis-hints path.

    ``<main-worktree-root>/.tools/analysis-hints/<rom-sha256>.json``. ``.tools/``
    is this repository's existing ignored convention for local tool/runtime
    state (see ``.gitignore`` and ``tools/ghidra.py``'s own
    ``DEFAULT_RUNTIME_ROOT``); this is a sibling use of that same convention,
    never a new tracked location. The caller is solely responsible for the
    file's own content and for supplying it explicitly via
    ``--external-hints``.
    """

    return main_worktree_root() / ".tools" / "analysis-hints" / f"{rom_sha256}.json"


def rom_sha256(rom_path: pathlib.Path) -> str:
    return hashlib.sha256(rom_path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, help="path to the local ROM image")
    args = parser.parse_args()
    rom_path = pathlib.Path(args.rom).resolve()
    if not rom_path.is_file():
        sys.stderr.write(f"no such ROM file: {rom_path}\n")
        return 1
    digest = rom_sha256(rom_path)
    print(str(analysis_hints_path(digest)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
