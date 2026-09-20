#!/usr/bin/env python3
"""SEG-007-T162 Scope §5: end-to-end MCP smoke test for the containerized Ghidra
agent path.

This exercises the *real* bridge->Ghidra path, not a mock:

1. start / confirm the pinned stack through ``tools/ghidra.py``;
2. launch the exact stdio MCP command an MCP client uses
   (``python3 tools/ghidra.py stdio``);
3. complete MCP ``initialize``;
4. discover/connect the headless Ghidra instance (``list_instances``);
5. load a tiny repository-generated MC68000 big-endian fixture with an explicit
   language id (``68000:BE:32:default``) and image base 0;
6. obtain and use a stable program selector;
7. run real program-scoped *read-only* operations (``get_current_program_info``
   + ``get_entry_points`` + ``disassemble_bytes`` with ``dry_run``) and assert
   the disassembly matches how the fixture bytes were constructed
   (``moveq #0x2a,D0`` / ``add.l A0,D0`` / ``rts``). No mutation tool
   (``create_function``, analysis, renames) is invoked; ``dry_run`` disassembly
   leaves ``function_count`` at 0 and ``analyzed`` false;
8. repeat the connect + program-scoped read from an ordinary linked
   ``git worktree`` (the continuous-agent task-worktree shape).

The fixture is 3 instructions of hand-assembled MC68000 machine code, fully
repo-owned and generated in-test. It skips with a clear reason when
Docker/Compose or the stack is unavailable.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tests"))

from ghidra_mcp_client import GhidraMcpClient  # noqa: E402

_SPEC = importlib.util.spec_from_file_location(
    "ghidra_tool_smoke", PROJECT_ROOT / "tools" / "ghidra.py"
)
assert _SPEC is not None and _SPEC.loader is not None
GHIDRA_TOOL = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(GHIDRA_TOOL)

LANGUAGE_ID = "68000:BE:32:default"

# 0x0000: 702A  moveq  #0x2a,D0
# 0x0002: D088  add.l  A0,D0
# 0x0004: 4E75  rts
FIXTURE_CODE = bytes.fromhex("702a") + bytes.fromhex("d088") + bytes.fromhex("4e75")
FIXTURE_IMAGE = FIXTURE_CODE + b"\x00" * (0x20 - len(FIXTURE_CODE))


def _docker_available() -> bool:
    if shutil.which("docker") is None:
        return False
    try:
        subprocess.run(
            ["docker", "compose", "version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
        return True
    except (subprocess.SubprocessError, OSError):
        return False


def _stack_ready(checkout: Path) -> bool:
    try:
        result = subprocess.run(
            [sys.executable, str(checkout / "tools" / "ghidra.py"), "health"],
            capture_output=True,
            text=True,
            timeout=60,
            cwd=str(checkout),
        )
        return result.returncode == 0
    except (subprocess.SubprocessError, OSError):
        return False


class GhidraMcpSmokeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not _docker_available():
            raise unittest.SkipTest("Docker/Compose not available")
        if not _stack_ready(PROJECT_ROOT):
            # Try one build/start; images are cached in normal use.
            try:
                built = subprocess.run(
                    [sys.executable, str(PROJECT_ROOT / "tools" / "ghidra.py"), "up"],
                    capture_output=True,
                    text=True,
                    timeout=1800,
                    cwd=str(PROJECT_ROOT),
                )
            except (subprocess.SubprocessError, OSError) as error:
                raise unittest.SkipTest(f"cannot start Ghidra stack: {error}")
            if built.returncode != 0 or not _stack_ready(PROJECT_ROOT):
                raise unittest.SkipTest(
                    "Ghidra stack did not become healthy (build/emulation environment)"
                )
        cls._input_dir = GHIDRA_TOOL.DEFAULT_RUNTIME_ROOT / "input"
        cls._input_dir.mkdir(parents=True, exist_ok=True)
        # Bind the path before writing so tearDownClass removes it even if the
        # write (or a later setUpClass step) fails partway through.
        cls._fixture = cls._input_dir / "seg_m68k_smoke_fixture.bin"
        cls._fixture.write_bytes(FIXTURE_IMAGE)

    @classmethod
    def tearDownClass(cls) -> None:
        fixture = getattr(cls, "_fixture", None)
        if fixture is not None:
            try:
                fixture.unlink()
            except FileNotFoundError:
                pass

    def _run_program_scoped_checks(self, client: GhidraMcpClient) -> None:
        info = client.initialize()
        self.assertEqual(info.get("serverInfo", {}).get("name"), "ghidra-mcp")

        instances = client.call_tool("list_instances")
        self.assertIn('"connected": true', instances)
        self.assertIn("127.0.0.1:8089", instances)

        loaded = client.call_tool(
            "load_program",
            {"file": "/inputs/seg_m68k_smoke_fixture.bin", "language": LANGUAGE_ID},
        )
        self.assertIn('"success":true', loaded.replace(" ", ""))
        selector = "seg_m68k_smoke_fixture.bin"

        program_info = client.call_tool("get_current_program_info", {"program": selector})
        self.assertIn(LANGUAGE_ID, program_info)
        self.assertIn('"image_base":"00000000"', program_info.replace(" ", ""))

        # Read-only: raw-binary loader marks 0x0 as the program entry point.
        entry_points = client.call_tool("get_entry_points", {"program": selector})
        self.assertRegex(entry_points, r"(?i)@\s*0*0\b")

        # Read-only known-result disassembly. ``dry_run`` guarantees no persistent
        # disassembly/function is written to the synthetic project.
        disasm = client.call_tool(
            "disassemble_bytes",
            {
                "start_address": "0x0",
                "length": len(FIXTURE_CODE),
                "program": selector,
                "dry_run": True,
                "include_instructions": True,
            },
        )
        normalized = disasm.lower().replace(" ", "")
        self.assertIn('"dry_run":true', normalized)
        self.assertIn('"mnemonic":"moveq","operands":"0x2a,d0"', normalized)
        self.assertIn('"mnemonic":"add.l","operands":"a0,d0"', normalized)
        self.assertIn('"mnemonic":"rts"', normalized)

        # The read path must not have mutated the synthetic program.
        status = client.call_tool("analysis_status", {"program": selector})
        self.assertIn('"function_count":0', status.replace(" ", ""))

    def test_end_to_end_from_base_checkout(self) -> None:
        with GhidraMcpClient(timeout=240) as client:
            self._run_program_scoped_checks(client)

    def test_end_to_end_from_linked_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            linked = Path(temporary) / "linked"
            try:
                subprocess.run(
                    ["git", "worktree", "add", "--detach", str(linked)],
                    check=True,
                    capture_output=True,
                    text=True,
                    cwd=str(PROJECT_ROOT),
                )
            except subprocess.CalledProcessError as error:
                self.skipTest(f"git worktree unavailable: {error.stderr}")
            try:
                # Mirror the working-tree tool (git checks out committed HEAD).
                (linked / "tools").mkdir(parents=True, exist_ok=True)
                (linked / "tools" / "ghidra.py").write_text(
                    (PROJECT_ROOT / "tools" / "ghidra.py").read_text(encoding="utf-8"),
                    encoding="utf-8",
                )
                self.assertTrue(_stack_ready(linked), "linked worktree cannot see the stack")
                with GhidraMcpClient(timeout=240, checkout_root=linked) as client:
                    self._run_program_scoped_checks(client)
            finally:
                subprocess.run(
                    ["git", "worktree", "remove", "--force", str(linked)],
                    check=False,
                    capture_output=True,
                    text=True,
                    cwd=str(PROJECT_ROOT),
                )


if __name__ == "__main__":
    unittest.main()
