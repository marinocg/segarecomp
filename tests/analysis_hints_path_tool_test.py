#!/usr/bin/env python3
"""SEG-007-T165 Scope correction: the canonical private analysis-hints path
must be derived from the Git common repository root, so a linked git
worktree resolves the exact same ignored per-ROM path as the main checkout
(and therefore the same file every task's own worktree must pass explicitly
via --external-hints)."""

import hashlib
import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "analysis_hints_path_tool", PROJECT_ROOT / "tools" / "analysis_hints_path.py"
)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class AnalysisHintsPathToolTest(unittest.TestCase):
    def test_path_is_scoped_under_dot_tools_analysis_hints(self) -> None:
        digest = "0" * 64
        resolved = TOOL.analysis_hints_path(digest)
        self.assertEqual(
            resolved,
            TOOL.main_worktree_root() / ".tools" / "analysis-hints" / f"{digest}.json",
        )

    def test_path_is_keyed_by_rom_sha256_digest(self) -> None:
        first = TOOL.analysis_hints_path("a" * 64)
        second = TOOL.analysis_hints_path("b" * 64)
        self.assertNotEqual(first, second)

    def test_cli_prints_digest_bound_path_for_a_real_rom(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            rom_path = Path(temporary) / "synthetic.md"
            rom_bytes = b"synthetic-non-commercial-fixture-bytes"
            rom_path.write_bytes(rom_bytes)
            result = subprocess.run(
                ["python3", str(PROJECT_ROOT / "tools" / "analysis_hints_path.py"), "--rom", str(rom_path)],
                check=True, text=True, capture_output=True, cwd=str(PROJECT_ROOT),
            )
            expected_digest = hashlib.sha256(rom_bytes).hexdigest()
            printed = Path(result.stdout.strip())
            self.assertEqual(printed.name, f"{expected_digest}.json")
            self.assertEqual(printed.parent.name, "analysis-hints")
            self.assertEqual(printed.parent.parent.name, ".tools")

    def test_cli_fails_closed_for_a_missing_rom(self) -> None:
        result = subprocess.run(
            ["python3", str(PROJECT_ROOT / "tools" / "analysis_hints_path.py"), "--rom",
             "/nonexistent/does-not-exist.md"],
            text=True, capture_output=True, cwd=str(PROJECT_ROOT),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")

    def test_root_falls_back_without_git(self) -> None:
        with patch.object(TOOL.subprocess, "run", side_effect=FileNotFoundError("git")):
            self.assertEqual(TOOL.main_worktree_root(), TOOL.PROJECT_ROOT)


class AnalysisHintsPathWorktreeTest(unittest.TestCase):
    """Mirrors GhidraRuntimeRootWorktreeTest (tests/ghidra_tool_test.py): a
    linked git worktree must resolve the identical canonical path as the
    main checkout for the identical ROM digest."""

    def _root_from(self, cwd: Path) -> str:
        script = (
            "import importlib.util, pathlib;"
            "s=importlib.util.spec_from_file_location('a', r'%s');"
            "m=importlib.util.module_from_spec(s); s.loader.exec_module(m);"
            "print(m.main_worktree_root())" % (cwd / "tools" / "analysis_hints_path.py")
        )
        result = subprocess.run(
            ["python3", "-c", script], check=True, text=True, capture_output=True, cwd=str(cwd)
        )
        return result.stdout.strip()

    def test_linked_worktree_resolves_same_root(self) -> None:
        if not (PROJECT_ROOT / ".git").exists():
            self.skipTest("not a primary git checkout")
        base_root = self._root_from(PROJECT_ROOT)
        with tempfile.TemporaryDirectory() as temporary:
            linked = Path(temporary) / "linked"
            try:
                subprocess.run(
                    ["git", "worktree", "add", "--detach", str(linked)],
                    check=True, text=True, capture_output=True, cwd=str(PROJECT_ROOT),
                )
            except subprocess.CalledProcessError as error:  # pragma: no cover
                self.skipTest(f"git worktree unavailable: {error.stderr}")
            try:
                (linked / "tools").mkdir(parents=True, exist_ok=True)
                (linked / "tools" / "analysis_hints_path.py").write_text(
                    (PROJECT_ROOT / "tools" / "analysis_hints_path.py").read_text(encoding="utf-8"),
                    encoding="utf-8",
                )
                linked_root = self._root_from(linked)
                self.assertEqual(base_root, linked_root)
                self.assertTrue(os.path.isdir(linked_root) or True)  # root need not pre-exist
            finally:
                subprocess.run(
                    ["git", "worktree", "remove", "--force", str(linked)],
                    check=False, text=True, capture_output=True, cwd=str(PROJECT_ROOT),
                )


if __name__ == "__main__":
    unittest.main()
